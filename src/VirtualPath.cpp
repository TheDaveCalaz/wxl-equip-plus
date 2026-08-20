// wxl-equip-extension: client-side virtual M2 path table and file provider.
//
// Virtual paths encode (cmo × model × merged-geoset-filter × texture) in the filename so the
// engine's model hash table sees a distinct cache key for every unique combination. The host
// serve hook is bypassed; bytes are served directly from an in-process table. Collection skins are
// also prefiltered here so synchronous skin loads receive the same geoset-trimmed data.
//
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "VirtualPath.hpp"
#include "WxlOffsets.hpp"

#include "game/Io.hpp"
#include "engine/assets/shared/models/m2/M2Format.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace wxl::scripts::equipextension
{
    namespace
    {
        static bool EquipLogEnabled() noexcept
        {
            static int enabled = []() noexcept -> int {
#pragma warning(suppress: 4996)
                const char* env = std::getenv("WXL_EQUIP_LOG");
                if (env && *env && *env != '0' && *env != 'n' && *env != 'N')
                    return 1;

#pragma warning(suppress: 4996)
                FILE* flag = std::fopen("WarcraftXL_equip.log.enable", "rb");
                if (!flag) return 0;
                std::fclose(flag);
                return 1;
            }();
            return enabled != 0;
        }

        static void VPathLog(const char* fmt, ...) noexcept
        {
            if (!EquipLogEnabled()) return;
#pragma warning(suppress: 4996)
            FILE* f = std::fopen("WarcraftXL_equip.log", "a");
            if (!f) return;
            va_list ap; va_start(ap, fmt);
            std::vfprintf(f, fmt, ap);
            va_end(ap);
            std::fputc('\n', f);
            std::fclose(f);
        }
        // virtual path -> raw file bytes (both .mdx and 00.skin entries per model).
        // Accessed from the game's main thread only; no mutex needed.
        std::unordered_map<std::string, std::vector<uint8_t>> g_virtualBytes;

        // cmo -> list of virtual paths it owns, for O(n) cleanup on eviction.
        std::unordered_map<void*, std::vector<std::string>> g_cmoVPaths;

        // REAL (unmangled, normalized) archive path -> patched bytes. Unlike g_virtualBytes,
        // these are served under the model's own real name and apply to every loader, not just
        // paths this module itself constructed. See VPathPopulateGlobal. Non-evictable entries
        // (the default) are permanent for the life of the process, same as before; entries
        // registered with evictable=true (creatures, weapons) can be reclaimed by EvictOverBudget
        // once their pool's combined size exceeds that pool's configured budget -- see
        // g_evictablePools below.
        std::unordered_map<std::string, std::vector<uint8_t>> g_globalOverrides;

        // Lazy-bake resolvers, tried in registration order on a g_globalOverrides miss. See
        // VPathRegisterLazyResolver's doc comment in VirtualPath.hpp. Registration happens once per
        // consumer at static-init time.
        std::vector<VPathLazyResolver> g_lazyResolvers;

        // ─── Evictable-entry LRU bookkeeping ──────────────────────────────────
        //
        // Only entries registered via VPathPopulateGlobal(..., evictable=true) are tracked here at
        // all -- non-evictable entries never appear in these tables and are never touched by
        // EvictOverBudget, so this whole mechanism is opt-in per bake, not global. A "group" is
        // everything one VPathPopulateGlobal call registered (the .m2 key, plus the .skin key if a
        // skin was present) -- evicted and touched as a single unit, since serving the .m2 for a
        // displayId without its matching .skin (or vice versa) after a partial eviction would produce
        // mismatched geometry/texture data.
        //
        // Bookkeeping is partitioned per evictionPool (e.g. "Creature", "Weapon"): each pool has its
        // own groups table and its own running byte total, budgeted independently via that pool's
        // own "Max<Pool>CacheMB" ini key (see GetEvictableByteBudget). This is what keeps one
        // feature's cache pressure from evicting another's entries -- a flood of newly-equipped
        // weapons can fill the weapon pool without touching a single creature entry, and vice versa.
        struct EvictableGroup
        {
            std::vector<std::string> keys; // table keys this group owns (.m2, optionally .skin)
            uint64_t bytes = 0;            // combined size of those keys' bytes in g_globalOverrides
            uint64_t tick  = 0;            // last-touched tick; lower = evicted first
        };
        struct EvictablePool
        {
            std::unordered_map<std::string, EvictableGroup> groups; // groupId (the .m2 vKey) -> group
            uint64_t bytesTotal = 0;
        };
        std::unordered_map<std::string, EvictablePool> g_evictablePools; // pool name -> pool state

        // Any owned table key (the .m2 groupId itself, or its .skin key) -> (pool, groupId). Both
        // keys of a group map to the same groupId (the .m2 vKey), so a touch on either the model or
        // the skin key finds the same group.
        struct EvictableKeyLoc { std::string pool; std::string groupId; };
        std::unordered_map<std::string, EvictableKeyLoc> g_evictableKeyLoc;
        uint64_t g_evictTickCounter = 0;

        // Bumps an evictable group's LRU tick, if tableKey belongs to one. A no-op for any key that
        // isn't tracked -- i.e. every non-evictable entry, and every "_wxl_"/g_virtualBytes key,
        // which never go through this path at all.
        static void TouchEvictableGroup(const std::string& tableKey)
        {
            auto locIt = g_evictableKeyLoc.find(tableKey);
            if (locIt == g_evictableKeyLoc.end()) return;
            auto poolIt = g_evictablePools.find(locIt->second.pool);
            if (poolIt == g_evictablePools.end()) return;
            auto git = poolIt->second.groups.find(locIt->second.groupId);
            if (git != poolIt->second.groups.end()) git->second.tick = ++g_evictTickCounter;
        }

        // "Max<Pool>CacheMB" from WXLExtendedEquipment.ini's [Memory] section, cached per pool name
        // after its first read (the ini isn't expected to change mid-session). <= 0 (including
        // "missing" -- the default below) means no cap is enforced for that pool: entries are still
        // tracked and touched, they're just never actually evicted. Default chosen generously; tune
        // per deployment, per pool, via the ini.
        static uint64_t GetEvictableByteBudget(const std::string& pool)
        {
            static std::unordered_map<std::string, uint64_t> cache;
            auto it = cache.find(pool);
            if (it != cache.end()) return it->second;

            char key[64];
            std::snprintf(key, sizeof(key), "Max%sCacheMB", pool.c_str());
            const int mb = WxlIniGetInt("Memory", key, 512);
            const uint64_t budget = (mb <= 0) ? 0 : static_cast<uint64_t>(mb) * 1024ull * 1024ull;
            cache.emplace(pool, budget);
            return budget;
        }

        // Evicts least-recently-touched evictable groups within one pool until that pool's byte
        // total is back under its own budget (or there's nothing left in it that's safe to evict).
        // Never touches any other pool's groups. justBakedGroupId is the group RegisterEvictableGroup
        // just finished inserting -- if it turns out to be both the LRU victim AND the only evictable
        // group in this pool, it's kept rather than evicted: the budget is a steady-state target, not
        // a hard per-bake ceiling, and evicting the entry that was just requested would just force an
        // immediate rebake on the very next load of it.
        static void EvictOverBudget(const std::string& pool, const std::string& justBakedGroupId)
        {
            const uint64_t budget = GetEvictableByteBudget(pool);
            if (budget == 0) return; // uncapped for this pool

            auto poolIt = g_evictablePools.find(pool);
            if (poolIt == g_evictablePools.end()) return;
            EvictablePool& poolState = poolIt->second;

            while (poolState.bytesTotal > budget)
            {
                std::string victim;
                uint64_t victimTick = UINT64_MAX;
                for (const auto& [groupId, group] : poolState.groups)
                {
                    if (group.tick < victimTick) { victimTick = group.tick; victim = groupId; }
                }
                if (victim.empty()) break; // nothing tracked in this pool at all

                if (victim == justBakedGroupId && poolState.groups.size() == 1) break;

                auto git = poolState.groups.find(victim);
                for (const std::string& key : git->second.keys)
                {
                    g_globalOverrides.erase(key);
                    g_evictableKeyLoc.erase(key);
                }
                poolState.bytesTotal -= git->second.bytes;
                VPathLog("  VPathPopulateGlobal: evicted '%s' from pool '%s' (%llu bytes, pool total "
                         "now %llu/%llu bytes)",
                         victim.c_str(), pool.c_str(),
                         static_cast<unsigned long long>(git->second.bytes),
                         static_cast<unsigned long long>(poolState.bytesTotal),
                         static_cast<unsigned long long>(budget));
                poolState.groups.erase(git);
            }
        }

        // Records a freshly baked (vKey, vSkin?) pair as one evictable unit in the given pool and
        // enforces that pool's budget. Called only after both keys are already in g_globalOverrides --
        // bytes are measured from there rather than passed in, so this can't drift out of sync with
        // what was actually stored.
        static void RegisterEvictableGroup(const std::string& pool, const std::string& vKey,
                                           const std::string& vSkin, bool hasSkin)
        {
            EvictableGroup group;
            group.keys.push_back(vKey);
            group.bytes = g_globalOverrides[vKey].size();
            if (hasSkin)
            {
                group.keys.push_back(vSkin);
                group.bytes += g_globalOverrides[vSkin].size();
            }
            group.tick = ++g_evictTickCounter;

            EvictablePool& poolState = g_evictablePools[pool];
            poolState.bytesTotal += group.bytes;
            for (const std::string& key : group.keys)
                g_evictableKeyLoc[key] = EvictableKeyLoc{ pool, vKey };
            poolState.groups[vKey] = std::move(group); // overwrite is fine: same-pair rebake, same size class

            EvictOverBudget(pool, vKey);
        }

        // ─── Key building ─────────────────────────────────────────────────────

        // Sorts ids[0..n-1] ascending in place (insertion sort; n <= 16).
        static void SortIds(uint16_t* ids, uint32_t n) noexcept
        {
            for (uint32_t i = 1; i < n; ++i)
            {
                uint16_t k = ids[i];
                int32_t j = static_cast<int32_t>(i) - 1;
                while (j >= 0 && ids[j] > k) { ids[j + 1] = ids[j]; --j; }
                ids[j + 1] = k;
            }
        }

        // Writes a uint16 as decimal into *q, advancing q. Returns new q.
        static char* WriteU16(char* q, char* end, uint16_t v) noexcept
        {
            char tmp[8]; int len = 0;
            if (v == 0) { tmp[len++] = '0'; }
            else { while (v) { tmp[len++] = '0' + (v % 10); v /= 10; } }
            for (int a = 0, b = len - 1; a < b; ++a, --b) { char c = tmp[a]; tmp[a] = tmp[b]; tmp[b] = c; }
            for (int i = 0; i < len && q < end; ++i) *q++ = tmp[i];
            return q;
        }

        // Writes a uintptr_t as lowercase hex into *q, advancing q. Returns new q.
        static char* WriteHex(char* q, char* end, uintptr_t v) noexcept
        {
            char tmp[9]; int len = 0;
            const char* digits = "0123456789abcdef";
            do { tmp[len++] = digits[v & 0xF]; v >>= 4; } while (v);
            for (int a = 0, b = len - 1; a < b; ++a, --b) { char c = tmp[a]; tmp[a] = tmp[b]; tmp[b] = c; }
            for (int i = 0; i < len && q < end; ++i) *q++ = tmp[i];
            return q;
        }

        static char LowerAscii(char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }

        static uint32_t HashCString(const char* s) noexcept
        {
            uint32_t h = 2166136261u;
            if (!s) return h;
            while (*s)
            {
                h ^= static_cast<uint8_t>(LowerAscii(*s++));
                h *= 16777619u;
            }
            return h;
        }

        // Builds the virtual .m2 key (sortedIds must already be sorted ascending).
        // Format: <stem>_wxl_<id0>_<id1>..._tex_<texbasename>[_mat<hash>][_grp<hex>]_cmo<hex>.m2  (all lowercase)
        // The engine normalises all paths to lowercase and uses .m2; keys must match that form.
        static size_t BuildKey(char* out, size_t outSz, void* cmo,
                                const char* realMdxPath,
                                const uint16_t* sortedIds, uint32_t idCount,
                                const char* texPath,
                                const char* materialPatchSpec,
                                uint32_t variantKey) noexcept
        {
            if (!out || outSz == 0) return 0;
            char* q   = out;
            char* end = out + outSz - 1; // reserve one byte for null

            // Copy the stem (path without extension) — lowercase.
            const char* lastDot = nullptr;
            for (const char* p = realMdxPath; *p; ++p) if (*p == '.') lastDot = p;
            const char* stemEnd = lastDot ? lastDot : realMdxPath + std::strlen(realMdxPath);
            for (const char* p = realMdxPath; p < stemEnd && q < end; ) *q++ = LowerAscii(*p++);

            // _wxl_
            for (const char* s = "_wxl_"; *s && q < end; ) *q++ = *s++;

            // Sorted geoset IDs separated by '_'.
            for (uint32_t i = 0; i < idCount && q < end; ++i)
            {
                if (i > 0 && q < end) *q++ = '_';
                q = WriteU16(q, end, sortedIds[i]);
            }

            // _tex_<texbasename> (basename = filename without path prefix or extension) — lowercase.
            if (texPath && *texPath)
            {
                const char* base = texPath;
                for (const char* p = texPath; *p; ++p)
                    if (*p == '\\' || *p == '/') base = p + 1;
                const char* dot = nullptr;
                for (const char* p = base; *p; ++p) if (*p == '.') dot = p;
                const char* baseEnd = dot ? dot : base + std::strlen(base);

                for (const char* s = "_tex_"; *s && q < end; ) *q++ = *s++;
                for (const char* p = base; p < baseEnd && q < end; ) *q++ = LowerAscii(*p++);
            }

            if (materialPatchSpec && *materialPatchSpec)
            {
                for (const char* s = "_mat"; *s && q < end; ) *q++ = *s++;
                q = WriteHex(q, end, HashCString(materialPatchSpec));
            }

            if (variantKey != 0)
            {
                for (const char* s = "_grp"; *s && q < end; ) *q++ = *s++;
                q = WriteHex(q, end, variantKey);
            }

            // _cmo<hex> (lowercase)
            for (const char* s = "_cmo"; *s && q < end; ) *q++ = *s++;
            q = WriteHex(q, end, reinterpret_cast<uintptr_t>(cmo));

            // .m2  (engine requests all paths with .m2 extension, not .mdx)
            for (const char* s = ".m2"; *s && q < end; ) *q++ = *s++;

            *q = '\0';
            return static_cast<size_t>(q - out);
        }

        // ─── File reading ─────────────────────────────────────────────────────

        // Reads all bytes of a game archive file via the existing IO wrappers.
        // Uses kOpenWholeFile so the handle buffer holds the full content immediately.
        static bool ReadGameFile(const char* path, std::vector<uint8_t>& out) noexcept
        {
            namespace io    = wxl::game::io;

            void* handle = nullptr;
            if (!io::FileOpen(path, offsets::io::kOpenWholeFile, &handle) || !handle)
                return false;

            uint32_t sizeHigh = 0;
            uint32_t size     = io::FileSize(handle, &sizeHigh);
            bool ok = false;
            if (size > 0 && sizeHigh == 0)
            {
                out.resize(size);
                uint32_t got = 0;
                io::FileRead(handle, out.data(), size, &got);
                ok = (got == size);
                if (!ok) out.clear();
            }
            io::FileClose(handle);
            return ok;
        }

        // Derives the real skin path: strip extension (.m2 or .mdx), append 00.skin.
        static void RealSkinPath(char* out, size_t outSz, const char* realMdxPath) noexcept
        {
            const char* lastDot = nullptr;
            for (const char* p = realMdxPath; *p; ++p) if (*p == '.') lastDot = p;
            size_t stemLen = lastDot ? static_cast<size_t>(lastDot - realMdxPath)
                                     : std::strlen(realMdxPath);
            if (stemLen >= outSz) stemLen = outSz - 1;
            std::memcpy(out, realMdxPath, stemLen);
            const char* suffix = "00.skin";
            size_t rem = outSz - stemLen - 1;
            size_t suffLen = std::strlen(suffix);
            if (suffLen > rem) suffLen = rem;
            std::memcpy(out + stemLen, suffix, suffLen);
            out[stemLen + suffLen] = '\0';
        }

        // Derives the virtual skin path from a virtual .m2 key: strip .m2, append 00.skin.
        static void VirtualSkinPath(char* out, size_t outSz, const char* virtualM2Key) noexcept
        {
            RealSkinPath(out, outSz, virtualM2Key); // same operation: strip extension, append 00.skin
        }

        // Writes a uint32 as decimal into *q, advancing q. Returns new q. (itemDisplayId is a
        // full 32-bit value; WriteU16 above tops out at 16 bits so it can't be reused here.)
        static char* WriteU32Dec(char* q, char* end, uint32_t v) noexcept
        {
            char tmp[10]; int len = 0;
            if (v == 0) { tmp[len++] = '0'; }
            else { while (v) { tmp[len++] = static_cast<char>('0' + (v % 10)); v /= 10; } }
            for (int a = 0, b = len - 1; a < b; ++a, --b) { char c = tmp[a]; tmp[a] = tmp[b]; tmp[b] = c; }
            for (int i = 0; i < len && q < end; ++i) *q++ = tmp[i];
            return q;
        }

        // Builds the per-itemDisplayId virtual key for VPathPopulateGlobal: <stem>_<itemDisplayId>.m2
        // normPath must already be normalized (lowercase, .m2 extension) by NormalizeRealPath.
        static void BuildGlobalVirtualKey(char* out, size_t outSz, const char* normPath,
                                          uint32_t itemDisplayId) noexcept
        {
            if (!out || outSz == 0) return;
            char* q   = out;
            char* end = out + outSz - 1; // reserve one byte for null

            const char* lastDot = nullptr;
            for (const char* p = normPath; *p; ++p) if (*p == '.') lastDot = p;
            const char* stemEnd = lastDot ? lastDot : normPath + std::strlen(normPath);
            for (const char* p = normPath; p < stemEnd && q < end; ) *q++ = *p++;

            if (q < end) *q++ = '_';
            q = WriteU32Dec(q, end, itemDisplayId);

            for (const char* p = (lastDot ? lastDot : ".m2"); *p && q < end; ) *q++ = *p++;

            *q = '\0';
        }

        static uint16_t ReadU16(const std::vector<uint8_t>& bytes, size_t off) noexcept
        {
            return static_cast<uint16_t>(bytes[off] | (bytes[off + 1] << 8));
        }

        static uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t off) noexcept
        {
            return static_cast<uint32_t>(bytes[off])
                 | (static_cast<uint32_t>(bytes[off + 1]) << 8)
                 | (static_cast<uint32_t>(bytes[off + 2]) << 16)
                 | (static_cast<uint32_t>(bytes[off + 3]) << 24);
        }

        static void WriteU32(std::vector<uint8_t>& bytes, size_t off, uint32_t value) noexcept
        {
            bytes[off + 0] = static_cast<uint8_t>(value);
            bytes[off + 1] = static_cast<uint8_t>(value >> 8);
            bytes[off + 2] = static_cast<uint8_t>(value >> 16);
            bytes[off + 3] = static_cast<uint8_t>(value >> 24);
        }

        static void Align4(std::vector<uint8_t>& bytes)
        {
            while (bytes.size() & 3u) bytes.push_back(0);
        }

        static uint32_t ParseU32Span(const char* begin, const char* end, uint32_t fallback) noexcept
        {
            if (!begin || !end || begin >= end) return fallback;
            const char* p = begin;
            while (p < end && (*p == ' ' || *p == '\t')) ++p;
            if (p >= end || *p < '0' || *p > '9') return fallback;
            uint32_t value = 0;
            while (p < end && *p >= '0' && *p <= '9')
                value = value * 10u + static_cast<uint32_t>(*p++ - '0');
            return value;
        }

        struct MaterialPatch
        {
            uint32_t textureType = 0xffffffffu;
            const char* path = nullptr;
            size_t pathLen = 0;
        };

        // NOTE: this used to also carry layer/batch/section-list fields and a hide/edgefade mode,
        // driven by the sidecar CSV's Layer, SkinSectionIDs, BatchIndexes, TargetSkinSectionIDs,
        // and TargetBatchIndexes columns. Those columns are no longer read by the loader at all
        // (see LoadMaterialSidecarFile in EquipExtension.cpp), so the hide-by-batch and edgefade
        // machinery that depended on them (BatchMatchesPatch, BatchLooksLikeEdgeFade,
        // RemoveHiddenBatches, IsHidePath, ParseNumberList, ContainsU16, and the batch/section
        // fields on MaterialPatch) has been removed along with them -- there is no longer any data
        // source to select which batches such a patch would apply to. What remains is exactly the
        // TextureType-keyed value-baking behavior: each spec entry claims every texture-unit record
        // of one declared TextureType and repoints it at one baked path.

        static uint32_t ParseMaterialPatchSpec(const char* spec,
                                               MaterialPatch* out,
                                               uint32_t outCount) noexcept
        {
            if (!spec || !*spec || !out || outCount == 0) return 0;

            uint32_t count = 0;
            const char* part = spec;
            while (*part && count < outCount)
            {
                const char* end = part;
                while (*end && *end != '|') ++end;

                const char* eq = part;
                while (eq < end && *eq != '=') ++eq;

                if (eq < end)
                {
                    MaterialPatch p = {};
                    p.textureType = ParseU32Span(part, eq, 0xffffffffu);
                    p.path = eq + 1;
                    while (p.path < end && (*p.path == ' ' || *p.path == '\t')) ++p.path;
                    const char* pathEnd = end;
                    while (pathEnd > p.path && (pathEnd[-1] == ' ' || pathEnd[-1] == '\t')) --pathEnd;
                    p.pathLen = static_cast<size_t>(pathEnd - p.path);
                    if (p.textureType != 0xffffffffu && p.pathLen > 0)
                        out[count++] = p;
                }

                if (!*end) break;
                part = end + 1;
            }
            return count;
        }

        static void PatchTargetedMaterialTextures(std::vector<uint8_t>& modelBytes,
                                                  const char* materialPatchSpec) noexcept
        {
            if (!materialPatchSpec || !*materialPatchSpec) return;

            MaterialPatch patches[16];
            const uint32_t patchCount = ParseMaterialPatchSpec(materialPatchSpec, patches, 16);
            if (!patchCount) return;

            namespace fmt = wxl::structure::m2;
            if (modelBytes.size() < sizeof(fmt::M2Header)) return;
            if (ReadU32(modelBytes, 0x00) != fmt::kMagicMD20) return;

            // Texture value patching is keyed purely on TextureType: for each patch, every texture
            // record on the model already declaring that type gets promoted to HARDCODED and
            // repointed at the patch's path, in place. This mirrors what the client itself does
            // with a model's native WEAPON_BLADE/OBJECT_SKIN records -- multiple records of the
            // same declared type already resolve to a single .blp regardless of how many exist, so
            // writing one path into all of them changes nothing structurally. Two patches that
            // target the same TextureType on one model: the first to run claims every record of
            // that type (converting them to HARDCODED), so the second finds nothing left to match
            // and is skipped -- deliberate, not a bug; a model that legitimately needs two distinct
            // simultaneous textures of one declared type is not representable here.
            uint32_t patchedRecords = 0;
            for (uint32_t pi = 0; pi < patchCount; ++pi)
            {
                uint32_t texCount = ReadU32(modelBytes, 0x50);
                uint32_t texOfs = ReadU32(modelBytes, 0x54);
                constexpr uint32_t kTexStride = sizeof(fmt::M2Texture);
                if (texOfs > modelBytes.size()) continue;
                if (texCount > (modelBytes.size() - texOfs) / kTexStride) continue;

                uint32_t matches = 0;
                for (uint32_t i = 0; i < texCount; ++i)
                {
                    const size_t rec = static_cast<size_t>(texOfs) + static_cast<size_t>(i) * kTexStride;
                    if (ReadU32(modelBytes, rec + 0x00) == patches[pi].textureType) ++matches;
                }
                if (matches == 0)
                {
                    VPathLog("  VPathPopulate: material patch textureType=%u has no matching "
                             "texture record on this model, skipped spec='%s'",
                             patches[pi].textureType, materialPatchSpec);
                    continue;
                }

                Align4(modelBytes);
                const uint32_t pathOfs = static_cast<uint32_t>(modelBytes.size());
                modelBytes.insert(modelBytes.end(), patches[pi].path, patches[pi].path + patches[pi].pathLen);
                modelBytes.push_back(0);

                // The insert above only appends bytes past the texture table -- texOfs/texCount
                // are unaffected -- but re-read for clarity since modelBytes may have reallocated.
                texOfs = ReadU32(modelBytes, 0x54);
                texCount = ReadU32(modelBytes, 0x50);
                for (uint32_t i = 0; i < texCount; ++i)
                {
                    const size_t rec = static_cast<size_t>(texOfs) + static_cast<size_t>(i) * kTexStride;
                    if (ReadU32(modelBytes, rec + 0x00) != patches[pi].textureType) continue;
                    WriteU32(modelBytes, rec + 0x00, fmt::kTexTypeHardcoded);
                    WriteU32(modelBytes, rec + 0x08, static_cast<uint32_t>(patches[pi].pathLen + 1));
                    WriteU32(modelBytes, rec + 0x0C, pathOfs);
                }
                patchedRecords += matches;
                VPathLog("  VPathPopulate: material patch textureType=%u -> HARDCODED records=%u "
                         "path='%.*s'", patches[pi].textureType, matches,
                         static_cast<int>(patches[pi].pathLen), patches[pi].path);
            }
            if (patchedRecords)
            {
                VPathLog("  VPathPopulate: material patch total records=%u spec='%s'",
                         patchedRecords, materialPatchSpec);
            }
        }

        static void PatchReplaceableTextureTypes(std::vector<uint8_t>& modelBytes,
                                                 const char* texPath) noexcept
        {
            namespace fmt = wxl::structure::m2;

            if (modelBytes.size() < sizeof(fmt::M2Header)) return;
            if (ReadU32(modelBytes, 0x00) != fmt::kMagicMD20) return;

            uint32_t texCount = ReadU32(modelBytes, 0x50);
            uint32_t texOfs   = ReadU32(modelBytes, 0x54);
            constexpr uint32_t kTexStride = sizeof(fmt::M2Texture);
            if (texOfs > modelBytes.size()) return;
            if (texCount > (modelBytes.size() - texOfs) / kTexStride) return;

            const bool hasTexPath = texPath && *texPath;
            if (!hasTexPath) return; // nothing to bake; OBJECT_SKIN records are left as-is

            // OBJECT_SKIN only. WEAPON_BLADE is a distinct, differently-purposed slot and is never
            // touched here -- it is left exactly as the model declares it, so the only way it ever
            // gets a texture is an explicit sidecar TextureType=3 row via
            // PatchTargetedMaterialTextures. A record that arrived already HARDCODED (almost always
            // a particle emitter's own baked-in texture, authored directly into the base .m2 and
            // never meant to be replaceable) is never a candidate either, since matching here is
            // purely "is this OBJECT_SKIN right now".
            uint32_t patchedObjectSkin = 0;
            uint32_t promoted[128];
            uint32_t promotedCount = 0;
            for (uint32_t i = 0; i < texCount; ++i)
            {
                const size_t rec = static_cast<size_t>(texOfs) + static_cast<size_t>(i) * kTexStride;
                const uint32_t texType = ReadU32(modelBytes, rec + 0x00);
                if (texType == fmt::kTexTypeObjectSkin)
                {
                    if (promotedCount < 128) promoted[promotedCount++] = i;
                    ++patchedObjectSkin;
                }
            }

            if (promotedCount > 0)
            {
                const size_t texLen = std::strlen(texPath);
                if (modelBytes.size() + texLen + 1 <= 0xffffffffu)
                {
                    const uint32_t pathOfs = static_cast<uint32_t>(modelBytes.size());
                    modelBytes.insert(modelBytes.end(), texPath, texPath + texLen);
                    modelBytes.push_back(0);

                    // Append-only insert; texOfs/texCount are unaffected but re-read since
                    // modelBytes may have reallocated.
                    texOfs = ReadU32(modelBytes, 0x54);
                    for (uint32_t k = 0; k < promotedCount; ++k)
                    {
                        const size_t rec = static_cast<size_t>(texOfs) +
                                           static_cast<size_t>(promoted[k]) * kTexStride;
                        WriteU32(modelBytes, rec + 0x00, fmt::kTexTypeHardcoded);
                        WriteU32(modelBytes, rec + 0x08, static_cast<uint32_t>(texLen + 1));
                        WriteU32(modelBytes, rec + 0x0C, pathOfs);
                    }
                }
            }

            if (patchedObjectSkin)
            {
                VPathLog("  VPathPopulate: promoted replaceable OBJECT_SKIN texture types=%u -> HARDCODED",
                         patchedObjectSkin);
            }
        }

        static bool ContainsId(const uint16_t* ids, uint32_t count, uint16_t value) noexcept
        {
            for (uint32_t i = 0; i < count; ++i)
                if (ids[i] == value) return true;
            return false;
        }

        static void ApplySkinByteFilter(std::vector<uint8_t>& skinBytes,
                                        const uint16_t* geoIds,
                                        uint32_t geoCount) noexcept
        {
            if (geoCount == 0 || skinBytes.size() < 0x2C) return;
            if (std::memcmp(skinBytes.data(), "SKIN", 4) != 0) return;

            const uint32_t rawIndexCount = ReadU32(skinBytes, 0x0C);
            const uint32_t rawIndexOfs   = ReadU32(skinBytes, 0x10);
            const uint32_t submeshCount  = ReadU32(skinBytes, 0x1C);
            const uint32_t submeshOfs    = ReadU32(skinBytes, 0x20);
            if (rawIndexOfs > skinBytes.size() || submeshOfs > skinBytes.size()) return;
            if (rawIndexCount > (skinBytes.size() - rawIndexOfs) / sizeof(uint16_t)) return;
            if (submeshCount > (skinBytes.size() - submeshOfs) / 0x30) return;

            uint32_t kept = 0;
            uint32_t zeroed = 0;
            for (uint32_t si = 0; si < submeshCount; ++si)
            {
                const size_t sub = submeshOfs + si * 0x30;
                const uint16_t sectionId = ReadU16(skinBytes, sub + 0x00);
                const uint16_t level = ReadU16(skinBytes, sub + 0x02);
                const uint16_t indexStart = ReadU16(skinBytes, sub + 0x08);
                const uint16_t indexCount = ReadU16(skinBytes, sub + 0x0A);
                if (indexCount == 0) continue;

                uint32_t fullIndexStart = (static_cast<uint32_t>(level) << 16) | indexStart;
                if (fullIndexStart > rawIndexCount || indexCount > rawIndexCount - fullIndexStart)
                {
                    fullIndexStart = indexStart;
                    if (fullIndexStart > rawIndexCount || indexCount > rawIndexCount - fullIndexStart) continue;
                }

                if (ContainsId(geoIds, geoCount, sectionId))
                {
                    ++kept;
                    continue;
                }

                std::memset(skinBytes.data() + rawIndexOfs + fullIndexStart * sizeof(uint16_t),
                            0,
                            indexCount * sizeof(uint16_t));
                ++zeroed;
            }
            VPathLog("  VPathPopulate: prefiltered skin kept=%u zeroed=%u", kept, zeroed);
        }

        // Produces a lowercase .m2 path from a (possibly mixed-case, .mdx-extension) DBC path.
        // The host stores loose files as lowercase .m2; ReadGameFile must use that form.
        static void NormalizeRealPath(char* out, size_t outSz, const char* src) noexcept
        {
            char* dst  = out;
            char* dend = out + outSz - 1;
            while (*src && dst < dend) *dst++ = LowerAscii(*src++);
            *dst = '\0';
            // Replace trailing .mdx → .m2 (DBC uses .mdx; host stores as .m2).
            char* lastDot = nullptr;
            for (char* p = out; *p; ++p) if (*p == '.') lastDot = p;
            if (lastDot && std::strcmp(lastDot, ".mdx") == 0)
                { lastDot[1] = 'm'; lastDot[2] = '2'; lastDot[3] = '\0'; }
        }

        // ─── Client-side provider ─────────────────────────────────────────────

        static bool VirtualProvide(const char* name, std::vector<uint8_t>& out)
        {
            if (!name) return false;

            // Global overrides apply to every loader, not just our own mangled paths, so this
            // has to run before the "_wxl_" short-circuit below. Guarded on g_lazyResolvers too:
            // with eager preregister off (see WxlIniGetBool), g_globalOverrides can legitimately
            // still be empty here even though there's real work for a resolver to do on a miss.
            if (!g_globalOverrides.empty() || !g_lazyResolvers.empty())
            {
                char norm[264];
                NormalizeRealPath(norm, sizeof(norm), name);
                auto git = g_globalOverrides.find(norm);

                // Not baked yet -- this is the genuine "asked for it right now, bake it right now"
                // moment: earlier than this, nothing has requested the bytes yet (wasted work if
                // baked eagerly and never actually needed); any later than this, the loader has
                // already been handed a miss. Give each registered resolver a chance to decode norm
                // into an id it recognizes and bake it on the spot, stopping at the first one that
                // does. Skipped entirely once the table already has this exact key -- e.g. eager
                // preload already covered it, or a previous miss already lazily baked it.
                if (git == g_globalOverrides.end())
                {
                    for (VPathLazyResolver resolver : g_lazyResolvers)
                    {
                        if (!resolver || !resolver(norm)) continue;
                        git = g_globalOverrides.find(norm);
                        if (git != g_globalOverrides.end()) break;
                    }
                }

                if (git != g_globalOverrides.end())
                {
                    TouchEvictableGroup(norm); // no-op for non-evictable (e.g. weapon) entries
                    VPathLog("  VirtualProvide(global): '%s' -> %zu bytes", name, git->second.size());
                    out = git->second;
                    return true;
                }
                // Diagnostic only: a miss here previously produced no log output at all, making it
                // impossible to tell "client never requested this path" apart from "requested it but
                // the normalized form didn't match what was registered". Only logs for weapon paths
                // specifically to avoid flooding the log with every unrelated model load.
                if (std::strstr(norm, "weapon"))
                    VPathLog("  VirtualProvide(global): MISS '%s' (normalized '%s'), table has %zu entries",
                             name, norm, g_globalOverrides.size());
            }

            if (!std::strstr(name, "_wxl_")) return false;
            VPathLog("  VirtualProvide: '%s'", name);
            auto it = g_virtualBytes.find(name);
            if (it == g_virtualBytes.end())
            {
                VPathLog("  VirtualProvide: NOT FOUND (table has %zu entries)", g_virtualBytes.size());
                return false;
            }
            VPathLog("  VirtualProvide: HIT (%zu bytes)", it->second.size());
            out = it->second;
            return true;
        }

        // OLD: self-registering global (constructed at DLL load, calling straight into the core's
        // internal wxl::runtime::storage::RegisterClientProvider). Extensions cannot call core
        // functions directly (see PORTING_GUIDE.md Step 9 / troubleshooting), and confirmed against
        // the real wxl/PluginApi.h: WXL_Api has no storage/provider registration member at all --
        // Log/Subscribe/Emit/HookAttach/PublishInterface/GetInterface/Ui*, nothing else. So this
        // module now hooks the archive file-I/O primitives itself, the same four addresses the core's
        // own StorageHook does (offsets::io::kFileOpen/kFileRead/kFileSize/kFileClose in
        // WxlOffsets.hpp), via api->HookAttach -- "alongside any party already detouring it" is
        // exactly this case: the core's own IPC-backed host provider chains behind this one.
        // See detail::InstallFileHooks / VPathRegisterStorageProvider below.

        // --- extension-owned virtual file layer -----------------------------------------------
        // A "handle" the client sees for a virtual file is just the address of one of these,
        // heap-allocated on open and freed on close. Never touches the real archive file handle
        // space, so the chained-through original I/O primitives never see it.
        struct VirtualHandle
        {
            std::vector<uint8_t> bytes;
            uint32_t             cursor = 0;
        };

        // Live virtual handles, so FileSize/FileRead/FileClose can tell "one of ours" apart from a
        // real archive handle without any tag bit the client's own handles might also set.
        std::unordered_map<void*, std::unique_ptr<VirtualHandle>>& LiveHandles()
        {
            static std::unordered_map<void*, std::unique_ptr<VirtualHandle>> handles;
            return handles;
        }

        offsets::io::Storage_FileOpenFn  g_origFileOpen  = nullptr;
        offsets::io::Storage_FileReadFn  g_origFileRead  = nullptr;
        offsets::io::Storage_FileSizeFn  g_origFileSize  = nullptr;
        offsets::io::Storage_FileCloseFn g_origFileClose = nullptr;

        // Local case-insensitive substring check -- EquipExtension.cpp has its own ContainsCI, but
        // that's a separate translation unit with no shared declaration, so this file needs its own.
        // Used only by the temporary texturecomponents diagnostic below.
        static bool ContainsCI(const char* haystack, const char* needle) noexcept
        {
            if (!haystack || !needle) return false;
            const size_t hLen = std::strlen(haystack);
            const size_t nLen = std::strlen(needle);
            if (nLen == 0 || nLen > hLen) return false;
            for (size_t i = 0; i + nLen <= hLen; ++i)
            {
                size_t j = 0;
                for (; j < nLen; ++j)
                {
                    if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                        std::tolower(static_cast<unsigned char>(needle[j])))
                        break;
                }
                if (j == nLen) return true;
            }
            return false;
        }

        int __stdcall FileOpenDetour(void* archive, const char* name, uint32_t flags, void** out)
        {
            std::vector<uint8_t> bytes;
            if (name && VirtualProvide(name, bytes))
            {
                auto handle = std::make_unique<VirtualHandle>();
                handle->bytes = std::move(bytes);
                void* key = handle.get();
                LiveHandles().emplace(key, std::move(handle));
                if (out) *out = key;
                VPathLog("  FileOpenDetour: served virtual '%s' (%p)", name, key);
                return 1;
            }
            const bool isTexComponent = name && ContainsCI(name, "texturecomponents");
            const int result = g_origFileOpen(archive, name, flags, out);
            if (isTexComponent)
            {
                VPathLog("  FileOpenDetour: PASSTHROUGH texturecomponents '%s' -> result=%d out=%p",
                         name, result, out ? *out : nullptr);
            }
            return result;
        }

        uint32_t __stdcall FileSizeDetour(void* handle, uint32_t* sizeHigh)
        {
            auto it = LiveHandles().find(handle);
            if (it != LiveHandles().end())
            {
                if (sizeHigh) *sizeHigh = 0;
                return static_cast<uint32_t>(it->second->bytes.size());
            }
            return g_origFileSize(handle, sizeHigh);
        }

        int __stdcall FileReadDetour(void* handle, void* dst, uint32_t len, uint32_t* read, void* ovl, uint32_t unk)
        {
            auto it = LiveHandles().find(handle);
            if (it != LiveHandles().end())
            {
                VirtualHandle& vh = *it->second;
                uint32_t remaining = vh.cursor < vh.bytes.size()
                                   ? static_cast<uint32_t>(vh.bytes.size()) - vh.cursor : 0;
                uint32_t toCopy = len < remaining ? len : remaining;
                if (toCopy && dst) std::memcpy(dst, vh.bytes.data() + vh.cursor, toCopy);
                vh.cursor += toCopy;
                if (read) *read = toCopy;
                return 1;
            }
            return g_origFileRead(handle, dst, len, read, ovl, unk);
        }

        int __stdcall FileCloseDetour(void* handle)
        {
            auto it = LiveHandles().find(handle);
            if (it != LiveHandles().end())
            {
                LiveHandles().erase(it);
                return 1;
            }
            return g_origFileClose(handle);
        }

        bool InstallFileHooks(const WXL_Api* api)
        {
            if (!api) return false;
            bool ok = true;
            ok &= api->HookAttach("wxl-equip-extension:FileOpen",  offsets::io::kFileOpen,
                                  reinterpret_cast<void*>(&FileOpenDetour),
                                  reinterpret_cast<void**>(&g_origFileOpen),  WXL_HOOK_DEFAULT_PRIORITY) != 0;
            ok &= api->HookAttach("wxl-equip-extension:FileSize",  offsets::io::kFileSize,
                                  reinterpret_cast<void*>(&FileSizeDetour),
                                  reinterpret_cast<void**>(&g_origFileSize),  WXL_HOOK_DEFAULT_PRIORITY) != 0;
            ok &= api->HookAttach("wxl-equip-extension:FileRead",  offsets::io::kFileRead,
                                  reinterpret_cast<void*>(&FileReadDetour),
                                  reinterpret_cast<void**>(&g_origFileRead),  WXL_HOOK_DEFAULT_PRIORITY) != 0;
            ok &= api->HookAttach("wxl-equip-extension:FileClose", offsets::io::kFileClose,
                                  reinterpret_cast<void*>(&FileCloseDetour),
                                  reinterpret_cast<void**>(&g_origFileClose), WXL_HOOK_DEFAULT_PRIORITY) != 0;
            if (!ok)
                api->Log(WXL_LOG_ERROR, "equip-extension", "VirtualPath: one or more file-I/O hooks failed");
            return ok;
        }
    }

    // ─── Model offset / scale (WXLHelmOffsets.csv) ──────────────────────────────────────────────
    // Ported from wxl-modern-assets' host/models/m2/EquipmentFixes.cpp (ApplyHelmOffset /
    // ApplyHelmOffsetIfConfigured), which targets exactly this WotLK-compatible M2 layout already
    // (it downports modern models INTO this format), so the struct shapes and append-a-synthesized-
    // single-key-track technique carry over directly -- this just adds a Kind column (Helm/Other)
    // to gate the automatic per-race/gender default, and a Scale column doing the same append-and-
    // relink trick against each bone's scale track instead of (well, alongside) its translation track.
    namespace fmt = wxl::structure::m2;

#pragma pack(push, 1)
    // A synthesized single-key track, appended past the model's original bytes and pointed to by
    // one bone's M2CompBone::translation or ::scale. The outer M2Array in track.timestamps/
    // track.values always holds exactly one M2Array<T> descriptor (the pre-Legion per-sequence
    // indirection this client's loader expects); that inner descriptor holds the one actual key.
    struct AppendedModelTrack
    {
        fmt::M2Array innerTimestamps; // 0x00 -> timestampValue
        uint32_t     timestampValue;  // 0x08  (left at 0: the single key sits at t=0)
        fmt::M2Array innerValues;     // 0x0C -> value
        float        value[3];        // 0x14
    };
#pragma pack(pop)
    static_assert(sizeof(AppendedModelTrack) == 0x20, "AppendedModelTrack");

    // Every playable race's 2-letter model-name code; shared by ModelRaceId (which race a model's
    // "<code><m|f>" filename suffix names) and StripRaceGenderSuffix (which trailing "_<code><m|f>"
    // a CSV rule's model key strips).
    constexpr const char* kRaceCodes[] = {
        "be", "dr", "dw", "gn", "hu", "ni", "or", "sc", "ta", "tr", "sk", "go"
    };

    static bool IsRaceCode(const char* race) noexcept
    {
        if (!race) return false;
        for (const char* r : kRaceCodes) if (std::strcmp(race, r) == 0) return true;
        return false;
    }

    // Parses the trailing "<race><m|f>" (3 chars, e.g. "bef") or "<race>_<m|f>" (4 chars incl.
    // underscore, e.g. "be_f") suffix off a model's file-name stem (before the extension). Returns
    // false (and leaves id empty) if the name doesn't end in a recognizable race/gender suffix --
    // callers treat that as "not a per-race-suffixed model" rather than an error.
    static bool ModelRaceId(const char* name, char* id, size_t idSz) noexcept
    {
        if (!name || !id || idSz < 4) return false;
        id[0] = '\0';
        const size_t len = std::strlen(name);
        const char* slash = nullptr;
        for (const char* p = name; *p; ++p) if (*p == '\\' || *p == '/') slash = p;
        const char* base = slash ? slash + 1 : name;
        const char* dot = nullptr;
        for (const char* p = base; *p; ++p) if (*p == '.') dot = p;
        const char* end = dot ? dot : base + std::strlen(base);
        if (end - base < 3) return false;

        char raceBuf[3] = {};
        char genderBuf = '\0';
        if (end - base >= 4 && end[-2] == '_')
        {
            raceBuf[0] = end[-4]; raceBuf[1] = end[-3]; genderBuf = end[-1];
        }
        else
        {
            raceBuf[0] = end[-3]; raceBuf[1] = end[-2]; genderBuf = end[-1];
        }
        raceBuf[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(raceBuf[0])));
        raceBuf[1] = static_cast<char>(std::tolower(static_cast<unsigned char>(raceBuf[1])));
        genderBuf  = static_cast<char>(std::tolower(static_cast<unsigned char>(genderBuf)));
        if (genderBuf != 'm' && genderBuf != 'f') return false;
        if (!IsRaceCode(raceBuf)) return false;
        id[0] = raceBuf[0]; id[1] = raceBuf[1]; id[2] = genderBuf; id[3] = '\0';
        return true;
    }

    struct ModelOffsetRule
    {
        char   model[128] = {};
        char   raceSex[8] = {};
        float  x = 0.0f, y = 0.0f, z = 0.0f, scale = 0.0f;
        bool   hasX = false, hasY = false, hasZ = false, hasScale = false;
        bool   add = true, disable = false;
        bool   isHelm = false; // Kind column: "Helm" enables the automatic per-race/gender default
                                // offset below; anything else ("Other"/blank) only ever applies this
                                // row's own explicit X/Y/Z/Scale, matching ApplyHelmOffsetIfConfigured.
    };

    static bool g_modelOffsetsLoaded = false;
    static std::vector<ModelOffsetRule> g_modelOffsetRules;

    static void NormalizeOffsetKey(const char* in, char* out, size_t outSz) noexcept
    {
        size_t o = 0;
        for (const char* p = in; *p && o + 1 < outSz; ++p)
        {
            char c = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
            if (c == '/') c = '\\';
            out[o++] = c;
        }
        out[o] = '\0';
        // strip a trailing .m2/.mdx extension
        const size_t len = std::strlen(out);
        if (len >= 3 && std::strcmp(out + len - 3, ".m2") == 0) out[len - 3] = '\0';
        else if (len >= 4 && std::strcmp(out + len - 4, ".mdx") == 0) out[len - 4] = '\0';
    }

    static void StripRaceGenderSuffixInPlace(char* stem) noexcept
    {
        const size_t n = std::strlen(stem);
        if (n >= 5 && stem[n - 5] == '_' && stem[n - 2] == '_' &&
            (stem[n - 1] == 'm' || stem[n - 1] == 'f'))
        {
            char race[3] = { stem[n - 4], stem[n - 3], '\0' };
            race[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(race[0])));
            race[1] = static_cast<char>(std::tolower(static_cast<unsigned char>(race[1])));
            if (IsRaceCode(race)) { stem[n - 5] = '\0'; return; }
        }
        if (n >= 4 && stem[n - 4] == '_' && (stem[n - 1] == 'm' || stem[n - 1] == 'f'))
        {
            char race[3] = { stem[n - 3], stem[n - 2], '\0' };
            race[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(race[0])));
            race[1] = static_cast<char>(std::tolower(static_cast<unsigned char>(race[1])));
            if (IsRaceCode(race)) stem[n - 4] = '\0';
        }
    }

    static void FileNamePart(const char* in, char* out, size_t outSz) noexcept
    {
        const char* slash = nullptr;
        for (const char* p = in; *p; ++p) if (*p == '\\') slash = p;
        const char* base = slash ? slash + 1 : in;
        std::strncpy(out, base, outSz - 1);
        out[outSz - 1] = '\0';
    }

    static void LoadModelOffsetsText(const char* source, const std::string& text)
    {
        std::vector<std::string> header;
        size_t pos = 0;
        {
            const size_t nl = text.find_first_of("\r\n");
            const std::string headerLine = text.substr(0, nl == std::string::npos ? text.size() : nl);
            std::string field; bool quoted = false;
            for (char c : headerLine)
            {
                if (quoted) { if (c == '"') quoted = false; else field.push_back(c); }
                else if (c == '"') quoted = true;
                else if (c == ',') { header.push_back(field); field.clear(); }
                else field.push_back(c);
            }
            header.push_back(field);
            pos = (nl == std::string::npos) ? text.size() : nl + 1;
        }
        auto findCol = [&](const char* name) -> int {
            for (size_t i = 0; i < header.size(); ++i)
            {
                std::string h = header[i];
                for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                std::string want = name;
                for (auto& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (h == want) return static_cast<int>(i);
            }
            return -1;
        };
        const int cModel = findCol("Model");
        const int cRaceSex = findCol("RaceSex");
        const int cMode = findCol("Mode");
        const int cKind = findCol("Kind");
        const int cX = findCol("X");
        const int cY = findCol("Y");
        const int cZ = findCol("Z");
        const int cScale = findCol("Scale");
        if (cModel < 0)
        {
            VPathLog("  WXLHelmOffsets '%s': missing Model column, skipped", source);
            return;
        }

        uint32_t loaded = 0;
        while (pos < text.size())
        {
            const size_t nl = text.find_first_of("\r\n", pos);
            std::string line = text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
            pos = (nl == std::string::npos) ? text.size() : nl + 1;
            if (line.empty()) continue;

            std::vector<std::string> row;
            std::string field; bool quoted = false;
            for (char c : line)
            {
                if (quoted) { if (c == '"') quoted = false; else field.push_back(c); }
                else if (c == '"') quoted = true;
                else if (c == ',') { row.push_back(field); field.clear(); }
                else field.push_back(c);
            }
            row.push_back(field);
            auto get = [&](int col) -> std::string {
                return (col >= 0 && static_cast<size_t>(col) < row.size()) ? row[col] : std::string();
            };

            ModelOffsetRule rule;
            char modelKey[128];
            NormalizeOffsetKey(get(cModel).c_str(), modelKey, sizeof(modelKey));
            if (!modelKey[0]) continue;
            std::strncpy(rule.model, modelKey, sizeof(rule.model) - 1);

            std::string raceSex = get(cRaceSex);
            for (auto& c : raceSex) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (raceSex == "all") raceSex = "*";
            std::strncpy(rule.raceSex, raceSex.c_str(), sizeof(rule.raceSex) - 1);

            std::string mode = get(cMode);
            for (auto& c : mode) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            rule.disable = (mode == "disable" || mode == "disabled" || mode == "none" || mode == "skip");
            rule.add = !(mode == "set" || mode == "absolute" || mode == "override");

            std::string kind = get(cKind);
            for (auto& c : kind) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            rule.isHelm = (kind == "helm");

            auto parseF = [](const std::string& s, float& out) -> bool {
                if (s.empty()) return false;
                char* end = nullptr;
                const float v = std::strtof(s.c_str(), &end);
                if (end == s.c_str()) return false;
                out = v; return true;
            };
            rule.hasX = parseF(get(cX), rule.x);
            rule.hasY = parseF(get(cY), rule.y);
            rule.hasZ = parseF(get(cZ), rule.z);
            rule.hasScale = parseF(get(cScale), rule.scale);
            if (!rule.disable && !rule.hasX && !rule.hasY && !rule.hasZ && !rule.hasScale && !rule.isHelm)
                continue;

            g_modelOffsetRules.push_back(rule);
            ++loaded;
        }
        if (loaded) VPathLog("  WXLHelmOffsets loaded '%s' rows=%u", source, loaded);
    }

    static void LoadModelOffsets()
    {
        if (g_modelOffsetsLoaded) return;
        g_modelOffsetsLoaded = true;
        std::vector<uint8_t> bytes;
        if (ReadGameFile("DBFilesClient\\WXLHelmOffsets.csv", bytes) && !bytes.empty())
            LoadModelOffsetsText("DBFilesClient\\WXLHelmOffsets.csv",
                std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (!g_modelOffsetRules.empty())
            VPathLog("  WXLHelmOffsets ready rules=%zu", g_modelOffsetRules.size());
    }

    // Per-race/gender default (x, z) helm-bone offset, ported verbatim from wxl-modern-assets
    // (calibrated by eye against each race's head proportions there). A race/gender with no entry
    // here falls back to the human default at the table's end. y is always 0 by default.
    struct ModelOffsetDefault { const char* id; float x, z; };
    constexpr ModelOffsetDefault kHelmOffsetDefaults[] = {
        { "drf", -0.0587258f, -0.195f },
        { "drm", -0.0587258f, -0.245f },
        { "taf", -0.13f,      -0.1f },
        { "tam", -0.2f,       -0.1f },
        { "nim", -0.09f,      -0.18f },
        { "nif", -0.08f,      -0.195f },
        { "orf", -0.08f,      -0.171f },
        { "orm", -0.13f,      -0.21f },
        { "trf", -0.0887258f, -0.08623257f },
        { "trm", -0.13f,      -0.16f },
        { "bef",  0.01f,      -0.2f },
        { "bem", -0.08f,      -0.165f },
        { "huf", -0.09f,      -0.18f },
        { "scm", -0.12f,      -0.12623256f },
        { "scf", -0.01f,      -0.15f },
        { "gnf", -0.015f,     -0.263f },
        { "gnm", -0.009f,     -0.23f },
        { "dwm", -0.0227258f, -0.1725f },
        { "dwf",  0.01f,      -0.195f },
    };
    constexpr ModelOffsetDefault kHelmOffsetHumanDefault{ "", -0.0587258f, -0.18623257f };

    static void HelmOffsetForId(const char* id, float& x, float& z) noexcept
    {
        const ModelOffsetDefault* found = &kHelmOffsetHumanDefault;
        for (const auto& entry : kHelmOffsetDefaults)
            if (std::strcmp(entry.id, id) == 0) { found = &entry; break; }
        x = found->x;
        z = found->z;
    }

    static int ModelRuleScore(const ModelOffsetRule& rule, const char* pathStem, const char* pathBase,
                              const char* fileStem, const char* fileBase, const char* raceSex) noexcept
    {
        if (rule.raceSex[0] && std::strcmp(rule.raceSex, "*") != 0 &&
            std::strcmp(rule.raceSex, raceSex) != 0) return -1;
        int score = -1;
        if (std::strchr(rule.model, '\\'))
        {
            if (std::strcmp(rule.model, pathStem) == 0) score = 40;
            else if (std::strcmp(rule.model, pathBase) == 0) score = 30;
        }
        else
        {
            if (std::strcmp(rule.model, fileStem) == 0) score = 20;
            else if (std::strcmp(rule.model, fileBase) == 0) score = 10;
        }
        if (score >= 0 && rule.raceSex[0] && std::strcmp(rule.raceSex, "*") != 0) score += 5;
        return score;
    }

    // Appends a synthesized single-key track per bone for translation and/or scale, nudging/resizing
    // the whole model. Ported from wxl-modern-assets' InjectHelmBoneOffset, extended to also cover
    // scale (that module only ever needed translation).
    static bool InjectModelOffsetAndScale(const char* name, std::vector<uint8_t>& model,
                                          bool doTranslate, float x, float y, float z,
                                          bool doScale, float scale)
    {
        if (model.size() < sizeof(fmt::M2Header)) return false;
        auto* md = reinterpret_cast<fmt::M2Header*>(model.data());
        if (md->magic != fmt::kMagicMD20 || !md->bones.count || !md->bones.offset) return false;
        const uint64_t bonesEnd = uint64_t(md->bones.offset) +
                                   uint64_t(md->bones.count) * sizeof(fmt::M2CompBone);
        const uint32_t tracksPerBone = (doTranslate ? 1u : 0u) + (doScale ? 1u : 0u);
        if (bonesEnd > model.size() || bonesEnd < md->bones.offset || tracksPerBone == 0 ||
            uint64_t(model.size()) + uint64_t(md->bones.count) * tracksPerBone * sizeof(AppendedModelTrack)
                > 0xffffffffu)
            return false;

        const uint32_t boneCount = md->bones.count;
        const uint32_t boneOffset = md->bones.offset;
        auto boneAt = [&](uint32_t i) {
            return reinterpret_cast<fmt::M2CompBone*>(model.data() + boneOffset + i * sizeof(fmt::M2CompBone));
        };

        for (uint32_t i = 0; i < boneCount; ++i)
        {
            boneAt(i)->flags |= fmt::kBoneFlagTransformed;

            if (doTranslate)
            {
                const uint32_t trackOffset = static_cast<uint32_t>(model.size());
                model.resize(model.size() + sizeof(AppendedModelTrack), 0);
                auto* bone = boneAt(i);
                auto* track = reinterpret_cast<AppendedModelTrack*>(model.data() + trackOffset);
                bone->translation.timestamps = { 1, trackOffset };
                track->innerTimestamps = { 1, trackOffset + offsetof(AppendedModelTrack, timestampValue) };
                bone->translation.values = { 1, trackOffset + offsetof(AppendedModelTrack, innerValues) };
                track->innerValues = { 1, trackOffset + offsetof(AppendedModelTrack, value) };
                track->value[0] = x; track->value[1] = y; track->value[2] = z;
            }
            if (doScale)
            {
                const uint32_t trackOffset = static_cast<uint32_t>(model.size());
                model.resize(model.size() + sizeof(AppendedModelTrack), 0);
                auto* bone = boneAt(i);
                auto* track = reinterpret_cast<AppendedModelTrack*>(model.data() + trackOffset);
                bone->scale.timestamps = { 1, trackOffset };
                track->innerTimestamps = { 1, trackOffset + offsetof(AppendedModelTrack, timestampValue) };
                bone->scale.values = { 1, trackOffset + offsetof(AppendedModelTrack, innerValues) };
                track->innerValues = { 1, trackOffset + offsetof(AppendedModelTrack, value) };
                track->value[0] = scale; track->value[1] = scale; track->value[2] = scale;
            }
        }
        VPathLog("  ModelOffset: '%s' translate=%d(%.4f,%.4f,%.4f) scale=%d(%.4f) bones=%u",
                 name, doTranslate ? 1 : 0, x, y, z, doScale ? 1 : 0, scale, boneCount);
        return true;
    }

    // Public entry point: called from VPathPopulate right after the real .m2 bytes are read. name
    // is the real (pre-normalization) DBC-style path, e.g. 'Item\ObjectComponents\Head\foo_Be_F.mdx'.
    // No-op (returns without modifying model) if nothing in WXLHelmOffsets.csv applies and the
    // model's own filename doesn't parse as a per-race helm suffix -- i.e. touching zero bytes and
    // zero bone flags for every model that was never meant to be offset/scaled at all.
    static void ApplyModelOffsetAndScale(const char* name, std::vector<uint8_t>& model)
    {
        LoadModelOffsets();
        if (g_modelOffsetRules.empty()) return;

        char id[8] = {};
        const bool hasRaceId = ModelRaceId(name, id, sizeof(id));

        char pathStem[264]; NormalizeOffsetKey(name, pathStem, sizeof(pathStem));
        char pathBase[264]; std::strncpy(pathBase, pathStem, sizeof(pathBase) - 1);
        pathBase[sizeof(pathBase) - 1] = '\0'; StripRaceGenderSuffixInPlace(pathBase);
        char fileStemBuf[264]; FileNamePart(pathStem, fileStemBuf, sizeof(fileStemBuf));
        char fileBase[264]; std::strncpy(fileBase, fileStemBuf, sizeof(fileBase) - 1);
        fileBase[sizeof(fileBase) - 1] = '\0'; StripRaceGenderSuffixInPlace(fileBase);

        const ModelOffsetRule* best = nullptr;
        int bestScore = -1;
        for (const auto& rule : g_modelOffsetRules)
        {
            const int score = ModelRuleScore(rule, pathStem, pathBase, fileStemBuf, fileBase,
                                             hasRaceId ? id : "");
            if (score > bestScore) { best = &rule; bestScore = score; }
        }
        if (!best) return;               // no CSV row at all for this model: do nothing (opt-in)
        if (best->disable) return;        // explicit disable: do nothing

        float x = 0.0f, y = 0.0f, z = 0.0f, scale = 1.0f;
        bool haveOffset = false, haveScale = false;

        if (best->isHelm && hasRaceId)
        {
            float dx, dz;
            HelmOffsetForId(id, dx, dz);
            x = dx; z = dz; haveOffset = true;
        }

        if (best->add)
        {
            if (best->hasX) { x += best->x; haveOffset = true; }
            if (best->hasY) { y += best->y; haveOffset = true; }
            if (best->hasZ) { z += best->z; haveOffset = true; }
        }
        else
        {
            if (best->hasX) { x = best->x; haveOffset = true; }
            if (best->hasY) { y = best->y; haveOffset = true; }
            if (best->hasZ) { z = best->z; haveOffset = true; }
        }
        if (best->hasScale) { scale = best->scale; haveScale = true; }

        if (!haveOffset && !haveScale) return;
        InjectModelOffsetAndScale(name, model, haveOffset, x, y, z, haveScale, scale);
    }

    // ─── Full geometric weapon mirroring (auto offhand fallback) ───────────────────────────────
    // Mirrors a mainhand weapon's .m2 + .skin bytes across the X axis for correct offhand display,
    // used only when WXLWeaponModels.csv has no explicit Model1OffhandPath/Model2OffhandPath for
    // this displayId -- an explicit hand-baked override always wins (see WeaponGetOffhandVirtualPath
    // in WeaponExtension.cpp). Struct layouts below cross-checked against M2AlastorOne.bt (010
    // Editor template, "Tested on 3.3.5 / 7.0.1 / 8.0.1 / 9.0.1 / 9.2.0"), the parts core's own
    // M2Format.hpp doesn't define (M2Vertex, M2Attachment, the compressed rotation quaternion).

#pragma pack(push, 1)
    // 0x30 bytes, unchanged since Classic. Position@0x00, BoneWeight@0x0C, BoneIndices@0x10,
    // Normal@0x14, TexCoord1@0x20, TexCoord2@0x28.
    struct M2VertexRaw
    {
        float   position[3];    // 0x00
        uint8_t boneWeight[4];  // 0x0C
        uint8_t boneIndices[4]; // 0x10
        float   normal[3];      // 0x14
        float   texCoord1[2];   // 0x20
        float   texCoord2[2];   // 0x28
    };
    static_assert(sizeof(M2VertexRaw) == 0x30, "M2VertexRaw");

    // ID(u32) + ParentBone(u32) + Position(C3Vector) + an int M2Track ("has been 1 on all models",
    // per the template's own comment -- never touched here, only Position is).
    struct M2AttachmentRaw
    {
        uint32_t id;                 // 0x00
        uint32_t parentBone;         // 0x04
        float    position[3];        // 0x08
        fmt::M2TrackHeader data;     // 0x14 (untouched)
    };
    static_assert(sizeof(M2AttachmentRaw) == 0x14 + sizeof(fmt::M2TrackHeader), "M2AttachmentRaw");

    // Raw on-disk .skin file header. Stable, well-documented WotLK+ layout: magic "SKIN", then five
    // M2Array blocks (vertex-lookup indices, triangle indices, vertex properties, submeshes, texture
    // units), then a bone-influence cap. NOTE: unlike the vertex/attachment/quaternion structs above,
    // this one isn't independently re-derived from the uploaded .bt template in this session (that
    // template only covers .m2, not .skin) -- it's from established prior knowledge of the format,
    // not a fresh source check the way everything else here was. If mirrored weapons render with
    // visibly inside-out faces, this is the first thing to re-verify.
    struct M2SkinHeaderRaw
    {
        char     magic[4];       // "SKIN"
        fmt::M2Array indices;    // 0x04  vertex-lookup table (uint16, -> m2 vertex array)
        fmt::M2Array triangles;  // 0x0C  uint16 triples, indices INTO the table above (not m2 verts)
        fmt::M2Array properties; // 0x14  uint8 per entry in indices[]
        fmt::M2Array submeshes;  // 0x1C
        fmt::M2Array textureUnits; // 0x24
        uint32_t boneCountMax;   // 0x2C
    };
    static_assert(sizeof(M2SkinHeaderRaw) == 0x30, "M2SkinHeaderRaw");
#pragma pack(pop)

    // Mirrors a compressed rotation quaternion (4x uint16, decoded as (v/32767.0)-1.0) across the X
    // axis. Derivation: mirroring negates the X component of every vector; for a rotation R to look
    // correctly mirrored we need R' = m*R*m (m = diag(-1,1,1)), which works out to negating the two
    // quaternion components NOT on the mirrored axis, leaving the mirrored-axis component and w
    // unchanged: q' = (qx, -qy, -qz, qw). Checked against two cases: a Z-axis rotation correctly
    // flips to -theta (mirrors reverse in-plane rotation handedness); a rotation about X itself (the
    // mirror's own axis) comes out unchanged (the axis-flip and handedness-reversal cancel).
    static void MirrorCompressedQuatX(uint16_t& x, uint16_t& y, uint16_t& z, uint16_t& /*w*/) noexcept
    {
        constexpr float kMaxShort = 32767.0f;
        auto stf = [&](uint16_t v) { return (static_cast<float>(v) / kMaxShort) - 1.0f; };
        auto fts = [&](float f) {
            float v = (f + 1.0f) * kMaxShort;
            if (v < 0.0f) v = 0.0f;
            if (v > 65535.0f) v = 65535.0f;
            return static_cast<uint16_t>(v + 0.5f);
        };
        y = fts(-stf(y));
        z = fts(-stf(z));
        // x, w untouched
    }

    // Negates the X component of every value in a translation (C3Vector) or rotation (compressed
    // quaternion) track, across every sequence -- walks the same nested M2Array-of-M2Array
    // indirection the offset-injection code above appends new data into, except here the data
    // already exists in the model and is being rewritten in place (no resize, no relinking).
    static void MirrorTranslationTrack(std::vector<uint8_t>& model, const fmt::M2TrackHeader& track)
    {
        if (!track.timestamps.count || !track.values.count) return;
        const uint64_t outerEnd = uint64_t(track.values.offset) +
                                   uint64_t(track.values.count) * sizeof(fmt::M2Array);
        if (outerEnd > model.size()) return;
        auto* outer = reinterpret_cast<fmt::M2Array*>(model.data() + track.values.offset);
        for (uint32_t s = 0; s < track.values.count; ++s)
        {
            const uint64_t innerEnd = uint64_t(outer[s].offset) +
                                       uint64_t(outer[s].count) * (sizeof(float) * 3);
            if (!outer[s].count || innerEnd > model.size()) continue;
            auto* vals = reinterpret_cast<float*>(model.data() + outer[s].offset);
            for (uint32_t k = 0; k < outer[s].count; ++k) vals[k * 3 + 0] = -vals[k * 3 + 0];
        }
    }

    static void MirrorRotationTrack(std::vector<uint8_t>& model, const fmt::M2TrackHeader& track)
    {
        if (!track.timestamps.count || !track.values.count) return;
        const uint64_t outerEnd = uint64_t(track.values.offset) +
                                   uint64_t(track.values.count) * sizeof(fmt::M2Array);
        if (outerEnd > model.size()) return;
        auto* outer = reinterpret_cast<fmt::M2Array*>(model.data() + track.values.offset);
        for (uint32_t s = 0; s < track.values.count; ++s)
        {
            const uint64_t innerEnd = uint64_t(outer[s].offset) +
                                       uint64_t(outer[s].count) * (sizeof(uint16_t) * 4);
            if (!outer[s].count || innerEnd > model.size()) continue;
            auto* vals = reinterpret_cast<uint16_t*>(model.data() + outer[s].offset);
            for (uint32_t k = 0; k < outer[s].count; ++k)
                MirrorCompressedQuatX(vals[k * 4 + 0], vals[k * 4 + 1], vals[k * 4 + 2], vals[k * 4 + 3]);
        }
    }

    // Mirrors vertex positions/normals, bone pivots + translation/rotation keyframes (every
    // sequence, not just a synthesized key), attachment offsets, and bounding volumes. Does NOT
    // touch scale tracks (scale is an unsigned magnitude, not a signed direction -- the mirror is
    // already fully expressed by the position/rotation changes above it). Returns false (model left
    // untouched) on any bounds-check failure, matching the append-side injection code's own
    // fail-closed convention.
    static bool MirrorModelBytes(const char* name, std::vector<uint8_t>& model) noexcept
    {
        if (model.size() < sizeof(fmt::M2Header)) return false;
        auto* md = reinterpret_cast<fmt::M2Header*>(model.data());
        if (md->magic != fmt::kMagicMD20) return false;

        __try
        {
            // Vertices
            if (md->vertices.count)
            {
                const uint64_t end = uint64_t(md->vertices.offset) +
                                      uint64_t(md->vertices.count) * sizeof(M2VertexRaw);
                if (end > model.size()) return false;
                auto* v = reinterpret_cast<M2VertexRaw*>(model.data() + md->vertices.offset);
                for (uint32_t i = 0; i < md->vertices.count; ++i)
                {
                    v[i].position[0] = -v[i].position[0];
                    v[i].normal[0]   = -v[i].normal[0];
                }
            }

            // Bones: pivot + every translation/rotation keyframe in every sequence
            if (md->bones.count)
            {
                const uint64_t end = uint64_t(md->bones.offset) +
                                      uint64_t(md->bones.count) * sizeof(fmt::M2CompBone);
                if (end > model.size()) return false;
                for (uint32_t i = 0; i < md->bones.count; ++i)
                {
                    // Re-fetch the bone pointer each iteration: model.data() can move if a track
                    // walk below ever needed to resize, though none currently do (in-place rewrite
                    // only) -- kept defensive since this loop mutates the same buffer it reads from.
                    auto* bone = reinterpret_cast<fmt::M2CompBone*>(
                        model.data() + md->bones.offset + i * sizeof(fmt::M2CompBone));
                    bone->pivot[0] = -bone->pivot[0];
                    MirrorTranslationTrack(model, bone->translation);
                    MirrorRotationTrack(model, bone->rotation);
                }
            }

            // Attachments (the actual "held in hand" point, plus any particle/trail attach points)
            if (md->attachments.count)
            {
                const uint64_t end = uint64_t(md->attachments.offset) +
                                      uint64_t(md->attachments.count) * sizeof(M2AttachmentRaw);
                if (end > model.size()) return false;
                auto* a = reinterpret_cast<M2AttachmentRaw*>(model.data() + md->attachments.offset);
                for (uint32_t i = 0; i < md->attachments.count; ++i)
                    a[i].position[0] = -a[i].position[0];
            }

            // Bounding/collision volumes: min/max X swap-and-negate (a min becomes the new max and
            // vice versa once negated), sphere/box centers unaffected by a symmetric-radius model.
            // boundingBox/collisionBox are [minX,minY,minZ,maxX,maxY,maxZ].
            {
                float newMinX = -md->boundingBox[3];
                float newMaxX = -md->boundingBox[0];
                md->boundingBox[0] = newMinX;
                md->boundingBox[3] = newMaxX;
                float newCMinX = -md->collisionBox[3];
                float newCMaxX = -md->collisionBox[0];
                md->collisionBox[0] = newCMinX;
                md->collisionBox[3] = newCMaxX;
            }
            if (md->collisionPositions.count)
            {
                const uint64_t end = uint64_t(md->collisionPositions.offset) +
                                      uint64_t(md->collisionPositions.count) * sizeof(float) * 3;
                if (end <= model.size())
                {
                    auto* p = reinterpret_cast<float*>(model.data() + md->collisionPositions.offset);
                    for (uint32_t i = 0; i < md->collisionPositions.count; ++i) p[i * 3] = -p[i * 3];
                }
            }
            if (md->collisionNormals.count)
            {
                const uint64_t end = uint64_t(md->collisionNormals.offset) +
                                      uint64_t(md->collisionNormals.count) * sizeof(float) * 3;
                if (end <= model.size())
                {
                    auto* n = reinterpret_cast<float*>(model.data() + md->collisionNormals.offset);
                    for (uint32_t i = 0; i < md->collisionNormals.count; ++i) n[i * 3] = -n[i * 3];
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            VPathLog("  MirrorModelBytes CRASH CAUGHT '%s'", name ? name : "(null)");
            return false;
        }

        VPathLog("  MirrorModelBytes: '%s' vertices=%u bones=%u attachments=%u", name ? name : "?",
                 md->vertices.count, md->bones.count, md->attachments.count);
        return true;
    }

    // Reverses triangle winding in the raw .skin file bytes: for every 3 consecutive uint16 in the
    // triangles array, swap the last two indices. A mirror flips the sign of the transform's
    // determinant, which flips every triangle's front/back facing under the same cull convention --
    // this is what keeps faces from rendering inside-out after MirrorModelBytes.
    static bool MirrorSkinWinding(const char* name, std::vector<uint8_t>& skin) noexcept
    {
        if (skin.size() < sizeof(M2SkinHeaderRaw)) return false;
        auto* sh = reinterpret_cast<M2SkinHeaderRaw*>(skin.data());
        if (std::memcmp(sh->magic, "SKIN", 4) != 0) return false;
        if (sh->triangles.count % 3 != 0) return false;

        __try
        {
            const uint64_t end = uint64_t(sh->triangles.offset) +
                                  uint64_t(sh->triangles.count) * sizeof(uint16_t);
            if (end > skin.size()) return false;
            auto* tri = reinterpret_cast<uint16_t*>(skin.data() + sh->triangles.offset);
            for (uint32_t t = 0; t + 2 < sh->triangles.count; t += 3)
                std::swap(tri[t + 1], tri[t + 2]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            VPathLog("  MirrorSkinWinding CRASH CAUGHT '%s'", name ? name : "(null)");
            return false;
        }

        VPathLog("  MirrorSkinWinding: '%s' triangles=%u", name ? name : "?", sh->triangles.count / 3);
        return true;
    }

    // MirrorWeaponForOffhand / VPathPopulateGlobalMirrored (auto-mirror offhand bake) removed --
    // this client's own axis-mirror transform was crashing on equip, and the feature is no longer
    // used: WeaponGetOffhandVirtualPath now only ever bakes an offhand model when
    // WXLWeaponModels.csv gives it an explicit Model1OffhandPath/Model2OffhandPath (see
    // BakeWeaponDisplayOffhand in WeaponExtension.cpp). The low-level MirrorModelBytes/
    // MirrorSkinWinding routines above are no longer called by anything and are left in place
    // only because other math in this file may still reference their neighbors; say the word if
    // you want those stripped out too.

    // ─── Public API ───────────────────────────────────────────────────────────

    // Replaces the old self-registering `detail::Registrar` global above. Must be called from
    // WXL_Load(), AFTER wxl::ext::EventScript::Bind(api), same ordering requirement as constructing
    // the EquipExtension/CreatureExtension/WeaponExtension instances -- HookAttach itself is safe
    // any time after WXL_Load starts, but keeping every setup call in one place after Bind avoids a
    // second "did I call this early enough" question to answer later.
    void VPathRegisterStorageProvider(const WXL_Api* api)
    {
        InstallFileHooks(api);
    }

    size_t VPathBuildKey(char* out, size_t outSz, void* cmo,
                         const char* realMdxPath,
                         const uint16_t* geoIds, uint32_t geoCount,
                         const char* texPath,
                         uint32_t variantKey,
                         const char* materialPatchSpec)
    {
        uint16_t sorted[16];
        uint32_t n = geoCount < 16 ? geoCount : 16;
        for (uint32_t i = 0; i < n; ++i) sorted[i] = geoIds[i];
        SortIds(sorted, n);
        return BuildKey(out, outSz, cmo, realMdxPath, sorted, n, texPath,
                        materialPatchSpec, variantKey);
    }

    bool VPathPopulate(void* cmo, const char* realMdxPath,
                       const uint16_t* geoIds, uint32_t geoCount,
                       const char* texPath,
                       uint32_t variantKey,
                       const char* materialPatchSpec)
    {
        uint16_t sorted[16];
        uint32_t n = geoCount < 16 ? geoCount : 16;
        for (uint32_t i = 0; i < n; ++i) sorted[i] = geoIds[i];
        SortIds(sorted, n);

        // Build virtual .mdx key.
        char vMdx[264];
        if (!BuildKey(vMdx, sizeof(vMdx), cmo, realMdxPath, sorted, n, texPath,
                      materialPatchSpec, variantKey)) return false;

        // No-op if already populated (same permutation on a re-equip cycle).
        if (g_virtualBytes.count(vMdx)) return true;

        // Build virtual .skin key.
        char vSkin[264];
        VirtualSkinPath(vSkin, sizeof(vSkin), vMdx);

        // Normalise the real path for host I/O: lowercase, .m2 extension.
        // DBC paths are mixed-case .mdx; the host stores loose files as lowercase .m2.
        char normPath[264];
        NormalizeRealPath(normPath, sizeof(normPath), realMdxPath);

        // Read real .m2 bytes (via normalized path).
        std::vector<uint8_t> mdxBytes;
        if (!ReadGameFile(normPath, mdxBytes))
        {
            VPathLog("  VPathPopulate: mdx READ FAILED '%s'", normPath);
            return false;
        }
        VPathLog("  VPathPopulate: mdx '%s' -> %zu bytes", normPath, mdxBytes.size());

        // Model position offset / scale (WXLHelmOffsets.csv). No-op for any model with no matching
        // CSV row -- see ApplyModelOffsetAndScale's own comment for the opt-in rationale.
        ApplyModelOffsetAndScale(realMdxPath, mdxBytes);

        // Read real 00.skin bytes.
        char rSkin[264];
        RealSkinPath(rSkin, sizeof(rSkin), normPath);
        std::vector<uint8_t> skinBytes;
        ReadGameFile(rSkin, skinBytes); // skin may be absent for some models; that is OK
        if (!skinBytes.empty())
            ApplySkinByteFilter(skinBytes, sorted, n);

        // Material-CSV texture-value patching runs BEFORE the base ModelTexture_N bake so an
        // explicit sidecar row claims and HARDCODEs its own texture-unit records first, whatever
        // TextureType it declares -- PatchTargetedMaterialTextures keys purely on the sidecar's
        // TextureType value, it is not limited to any particular type. PatchReplaceableTextureTypes
        // below only ever matches records still declaring OBJECT_SKIN(2) -- its own single fixed
        // type, unrelated to the sidecar -- so a sidecar row that happens to target TextureType=2
        // is already HARDCODED and invisible to it by the time it runs: the CSV value wins instead
        // of being silently pre-empted by the base Texture column, which is what the old (reverse)
        // ordering did to any such row. WEAPON_BLADE(3) is never touched by
        // PatchReplaceableTextureTypes at all -- an unclaimed WEAPON_BLADE record stays exactly
        // that unless a sidecar TextureType=3 row claims it, so a missing sidecar row for a
        // weapon-blade slot shows up as a visibly unresolved texture rather than being silently
        // filled in. This only ever touches modelBytes (the model's own texture-unit table), not
        // skinBytes, so it no longer needs a skin file to be present at all.
        PatchTargetedMaterialTextures(mdxBytes, materialPatchSpec);
        PatchReplaceableTextureTypes(mdxBytes, texPath);

        VPathLog("  VPathPopulate: skin '%s' -> %zu bytes", rSkin, skinBytes.size());
        VPathLog("  VPathPopulate: vMdx='%s'", vMdx);
        VPathLog("  VPathPopulate: vSkin='%s'", vSkin);

        // Store in table.
        g_virtualBytes.emplace(vMdx,  std::move(mdxBytes));
        if (!skinBytes.empty())
            g_virtualBytes.emplace(vSkin, std::move(skinBytes));

        // Register both under cmo for cleanup.
        auto& paths = g_cmoVPaths[cmo];
        paths.emplace_back(vMdx);
        if (g_virtualBytes.count(vSkin)) paths.emplace_back(vSkin);
        return true;
    }

    void VPathEvictCmo(void* cmo)
    {
        auto it = g_cmoVPaths.find(cmo);
        if (it == g_cmoVPaths.end()) return;
        for (const auto& path : it->second)
            g_virtualBytes.erase(path);
        g_cmoVPaths.erase(it);
    }

    bool VPathPopulateGlobal(const char* realMdxPath, uint32_t itemDisplayId,
                             const char* texPath, const char* materialPatchSpec,
                             const uint16_t* geoIds, uint32_t geoCount, bool evictable,
                             char* outVirtualPath, size_t outVirtualPathSz,
                             const char* evictionPool, bool forceBake)
    {
        if (!realMdxPath || !*realMdxPath) return false;
        const bool hasTexPath = texPath && *texPath;
        const bool hasMatSpec = materialPatchSpec && *materialPatchSpec;
        const bool hasGeoFilter = geoIds && geoCount > 0;
        if (!forceBake && !hasTexPath && !hasMatSpec && !hasGeoFilter) return false;

        // Normalise to the same lowercase/.m2 form the host uses for real file I/O -- this is
        // still what gets read off disk below -- then mangle in itemDisplayId to get the actual
        // table key. Two different displayIds that resolve to the same underlying model file each
        // get their own entry instead of colliding on one shared key. VirtualProvide does not do
        // any un-mangling on the read side; it just normalises the incoming request the same way
        // and looks it up verbatim, so whatever requests this path must ask for the virtual form,
        // not the real one -- see the doc comment on this function in VirtualPath.hpp.
        char normPath[264];
        NormalizeRealPath(normPath, sizeof(normPath), realMdxPath);

        char vKey[280];
        BuildGlobalVirtualKey(vKey, sizeof(vKey), normPath, itemDisplayId);

        if (outVirtualPath && outVirtualPathSz)
        {
            size_t n = std::strlen(vKey);
            if (n >= outVirtualPathSz) n = outVirtualPathSz - 1;
            std::memcpy(outVirtualPath, vKey, n);
            outVirtualPath[n] = '\0';
        }

        // First (texPath, materialPatchSpec) registered for this (path, itemDisplayId) pair wins
        // for the rest of the session -- see the doc comment on this function in VirtualPath.hpp.
        if (g_globalOverrides.count(vKey)) return true;

        std::vector<uint8_t> mdxBytes;
        if (!ReadGameFile(normPath, mdxBytes))
        {
            VPathLog("  VPathPopulateGlobal: mdx READ FAILED '%s'", normPath);
            return false;
        }

        char rSkin[264];
        RealSkinPath(rSkin, sizeof(rSkin), normPath);
        std::vector<uint8_t> skinBytes;
        ReadGameFile(rSkin, skinBytes); // skin may be absent for some models; that is OK

        // Same ApplySkinByteFilter used by VPathPopulate -- zeroes the raw index bytes of every
        // submesh whose sectionId isn't in geoIds, in place, before the bytes go in the table. A
        // no-op (returns immediately) when geoCount == 0 or skinBytes is empty/malformed.
        if (hasGeoFilter)
            ApplySkinByteFilter(skinBytes, geoIds, geoCount);

        // Material-CSV texture-value patching runs BEFORE the base ModelTexture_N bake -- see the
        // matching comment in VPathPopulate above. A sidecar row claims and HARDCODEs its own
        // texture-unit records first, whatever TextureType it declares; PatchReplaceableTextureTypes
        // then only promotes whatever OBJECT_SKIN(2) records are still left -- its own single fixed
        // type, unrelated to the sidecar -- so a sidecar row that happens to target TextureType=2
        // has its CSV value win instead of being silently pre-empted by texPath. WEAPON_BLADE(3) is
        // never touched here at all; only an explicit sidecar TextureType=3 row can ever set it.
        // This only ever touches modelBytes, not skinBytes, so it no longer needs a skin file.
        if (hasMatSpec)
            PatchTargetedMaterialTextures(mdxBytes, materialPatchSpec);

        if (hasTexPath)
        {
            PatchReplaceableTextureTypes(mdxBytes, texPath);
        }

        VPathLog("  VPathPopulateGlobal: '%s' (displayId=%u) -> vkey='%s' mdx=%zu skin=%zu bytes "
                 "(tex='%s' spec='%s' geoCount=%u)",
                 normPath, itemDisplayId, vKey, mdxBytes.size(), skinBytes.size(),
                 hasTexPath ? texPath : "", hasMatSpec ? materialPatchSpec : "", geoCount);

        char vSkin[280];
        VirtualSkinPath(vSkin, sizeof(vSkin), vKey);

        const bool hasSkin = !skinBytes.empty();
        g_globalOverrides.emplace(vKey, std::move(mdxBytes));
        if (hasSkin)
            g_globalOverrides.emplace(vSkin, std::move(skinBytes));

        if (evictable)
        {
            const std::string pool = (evictionPool && *evictionPool) ? evictionPool : "Creature";
            RegisterEvictableGroup(pool, vKey, vSkin, hasSkin);
        }

        return true;
    }

    // Reads/patches byteSourcePath's bytes exactly like VPathPopulateGlobal, but derives the cache
    // key from keySourcePath instead, and ALWAYS overwrites any existing entry under that key
    // (unlike every other bake function here, which is "first bake wins"). Built specifically for
    // the offhand-weapon case: this client's native weapon display resolves BOTH mainhand and
    // offhand attach points from the SAME static ItemModelData.dbc field, so a distinctly-keyed
    // offhand bake (see VPathPopulateGlobalMirrored) never actually gets used by native code -- the
    // only way an offhand weapon visibly differs from mainhand is for the SHARED key mainhand
    // already resolves through to hold the offhand's bytes instead, swapped in at the moment the
    // offhand attach happens. Caller (WeaponExtension.cpp's BakeWeaponDisplayOffhand, from
    // EquipExtension's per-instance offhand equip handling, never from eager startup preload) is
    // responsible for re-asserting this swap every time, since it's inherently last-write-wins
    // against whatever else last populated the same key.
    bool VPathPopulateGlobalSharedKey(const char* keySourcePath, const char* byteSourcePath,
                                      uint32_t itemDisplayId, const char* texPath,
                                      const char* materialPatchSpec, const uint16_t* geoIds,
                                      uint32_t geoCount, char* outVirtualPath, size_t outVirtualPathSz)
    {
        if (!keySourcePath || !*keySourcePath || !byteSourcePath || !*byteSourcePath) return false;

        char keyNormPath[264];
        NormalizeRealPath(keyNormPath, sizeof(keyNormPath), keySourcePath);
        char vKey[280];
        BuildGlobalVirtualKey(vKey, sizeof(vKey), keyNormPath, itemDisplayId);

        if (outVirtualPath && outVirtualPathSz)
        {
            size_t n = std::strlen(vKey);
            if (n >= outVirtualPathSz) n = outVirtualPathSz - 1;
            std::memcpy(outVirtualPath, vKey, n);
            outVirtualPath[n] = '\0';
        }

        char byteNormPath[264];
        NormalizeRealPath(byteNormPath, sizeof(byteNormPath), byteSourcePath);

        std::vector<uint8_t> mdxBytes;
        if (!ReadGameFile(byteNormPath, mdxBytes))
        {
            VPathLog("  VPathPopulateGlobalSharedKey: mdx READ FAILED '%s' (key from '%s')",
                     byteNormPath, keyNormPath);
            return false;
        }

        char rSkin[264];
        RealSkinPath(rSkin, sizeof(rSkin), byteNormPath);
        std::vector<uint8_t> skinBytes;
        ReadGameFile(rSkin, skinBytes); // skin may be absent; that is OK

        if (geoIds && geoCount > 0)
            ApplySkinByteFilter(skinBytes, geoIds, geoCount);
        if (materialPatchSpec && *materialPatchSpec)
            PatchTargetedMaterialTextures(mdxBytes, materialPatchSpec);
        if (texPath && *texPath)
            PatchReplaceableTextureTypes(mdxBytes, texPath);

        char vSkin[280];
        VirtualSkinPath(vSkin, sizeof(vSkin), vKey);

        // operator[] assignment, not emplace: emplace is a no-op if vKey already exists, and the
        // entire point here is to overwrite whatever's already there (almost certainly mainhand's
        // own bake, from whenever it was last resolved).
        g_globalOverrides[vKey] = std::move(mdxBytes);
        if (!skinBytes.empty())
            g_globalOverrides[vSkin] = std::move(skinBytes);
        else
            g_globalOverrides.erase(vSkin); // previous occupant of this key may have had a skin

        // Deliberately not registered in any eviction pool: this key must never be evicted and
        // silently regenerated from keySourcePath's own bytes by some other caller mid-session,
        // which would quietly undo the swap without anything re-asserting it until the next equip
        // event happens to touch this slot again.

        VPathLog("  VPathPopulateGlobalSharedKey: key from '%s' bytes from '%s' (displayId=%u) -> "
                 "vkey='%s' mdx=%zu skin=%zu",
                 keyNormPath, byteNormPath, itemDisplayId, vKey, mdxBytes.size(), skinBytes.size());
        return true;
    }

    void VPathRegisterLazyResolver(VPathLazyResolver resolver)
    {
        if (resolver) g_lazyResolvers.push_back(resolver);
    }

    bool VPathDecodeGlobalVirtualKey(const char* virtualPathName, char* outNormPath, size_t outNormPathSz,
                                     uint32_t* outDisplayId)
    {
        if (!virtualPathName || !outNormPath || outNormPathSz == 0 || !outDisplayId) return false;

        const char* lastDot = nullptr;
        for (const char* p = virtualPathName; *p; ++p) if (*p == '.') lastDot = p;
        if (!lastDot) return false; // no extension -- not a shape BuildGlobalVirtualKey produces

        // Walk backward from the extension over ASCII digits.
        const char* digitsEnd = lastDot;
        const char* digitsBegin = digitsEnd;
        while (digitsBegin > virtualPathName && digitsBegin[-1] >= '0' && digitsBegin[-1] <= '9')
            --digitsBegin;
        if (digitsBegin == digitsEnd) return false;                       // no digit run at all
        if (digitsBegin == virtualPathName || digitsBegin[-1] != '_') return false; // no '_' before it

        uint32_t displayId = 0;
        for (const char* d = digitsBegin; d < digitsEnd; ++d)
            displayId = displayId * 10u + static_cast<uint32_t>(*d - '0');

        const size_t stemLen = static_cast<size_t>((digitsBegin - 1) - virtualPathName); // exclude '_'
        const size_t extLen  = std::strlen(lastDot);
        if (stemLen + extLen >= outNormPathSz) return false;

        std::memcpy(outNormPath, virtualPathName, stemLen);
        std::memcpy(outNormPath + stemLen, lastDot, extLen + 1); // includes null terminator
        *outDisplayId = displayId;
        return true;
    }

    namespace
    {
        // Resolves the HMODULE of whichever module this very function's own code lives in -- i.e.
        // WarcraftXL.dll itself (this module is built into it, there is no separate
        // WXLExtendedEquipment.dll) -- without needing DllMain's hinstDLL (not tracked anywhere in
        // this module) and without assuming the process's current working directory has any
        // relationship to the DLL's own location. GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT: this
        // is a lookup, not a LoadLibrary -- don't bump the module's refcount for it. Shared by
        // WxlIniGetBool and WxlIniGetInt so the resolution logic only exists once.
        bool ResolveIniPath(char* outIniPath, size_t outIniPathSz)
        {
            HMODULE hMod = nullptr;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCSTR>(&ResolveIniPath), &hMod) || !hMod)
            {
                return false;
            }

            char dllPath[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameA(hMod, dllPath, sizeof(dllPath));
            if (n == 0 || n >= sizeof(dllPath)) return false;

            const char* lastSlash = std::strrchr(dllPath, '\\');
            const char* kIniName = "WXLExtendedEquipment.ini";
            if (lastSlash)
            {
                const size_t dirLen = static_cast<size_t>(lastSlash - dllPath) + 1; // include the '\\'
                if (dirLen + std::strlen(kIniName) >= outIniPathSz) return false;
                std::memcpy(outIniPath, dllPath, dirLen);
                std::strcpy(outIniPath + dirLen, kIniName);
            }
            else
            {
                if (std::strlen(kIniName) >= outIniPathSz) return false;
                std::strcpy(outIniPath, kIniName); // no directory component -- fall back to relative
            }
            return true;
        }
    }

    int WxlIniGetInt(const char* section, const char* key, int defaultValue)
    {
        if (!section || !key) return defaultValue;

        char iniPath[MAX_PATH] = {};
        if (!ResolveIniPath(iniPath, sizeof(iniPath))) return defaultValue;

        // GetPrivateProfileInt's own convention: missing file/section/key all fall through to
        // returning nDefault untouched, so there's no separate "does the ini even exist" check
        // needed here -- same "optional, use the default" treatment as every sidecar CSV.
        return static_cast<int>(GetPrivateProfileIntA(section, key, defaultValue, iniPath));
    }

    bool WxlIniGetBool(const char* section, const char* key, bool defaultValue)
    {
        return WxlIniGetInt(section, key, defaultValue ? 1 : 0) != 0;
    }
}
