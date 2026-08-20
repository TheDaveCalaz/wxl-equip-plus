// wxl-creature-extension: sidecar-driven creature retexturing for WarcraftXL.
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

#include "CreatureExtension.hpp"
#include "WxlOffsets.hpp"
#include "VirtualPath.hpp"
#include "engine/events/Event.hpp"
#include "game/Io.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// VPathPopulateGlobal (and the whole virtual-model-serving mechanism it belongs to) lives under
// wxl::scripts::equipextension -- historically named after its first consumer, but it was always a
// generic (path, id) -> baked-bytes mechanism, not anything weapon-specific. This script reuses it
// as-is via its current namespace rather than doing an unrequested rename/move; worth revisiting as
// a cleanup if a third consumer ever shows up and the naming gets more confusing still.
using wxl::scripts::equipextension::VPathPopulateGlobal;
using wxl::scripts::equipextension::VPathRegisterLazyResolver;
using wxl::scripts::equipextension::VPathDecodeGlobalVirtualKey;
using wxl::scripts::equipextension::WxlIniGetBool;

namespace wxl::scripts::creatureextension
{
    namespace ev  = wxl::events;
	namespace offsets = wxl::scripts::equipextension::offsets;

    // ─── Logging ────────────────────────────────────────────────────────────────
    // Same opt-in-file-log shape as EquipExtension's EquipLog/EquipLogEnabled, kept separate (own
    // env var, own log file) so enabling one doesn't spam the other feature's log.
    static bool CreatureLogEnabled() noexcept
    {
        static int enabled = []() noexcept -> int {
#pragma warning(suppress: 4996)
            const char* env = std::getenv("WXL_CREATURE_LOG");
            if (env && *env && *env != '0' && *env != 'n' && *env != 'N')
                return 1;

#pragma warning(suppress: 4996)
            FILE* flag = std::fopen("WarcraftXL_creature.log.enable", "rb");
            if (!flag) return 0;
            std::fclose(flag);
            return 1;
        }();
        return enabled != 0;
    }

    static void CreatureLog(const char* fmt, ...) noexcept
    {
        if (!CreatureLogEnabled()) return;
#pragma warning(suppress: 4996)
        FILE* f = std::fopen("WarcraftXL_creature.log", "a");
        if (!f) return;
        va_list ap; va_start(ap, fmt);
        std::vfprintf(f, fmt, ap);
        va_end(ap);
        std::fputc('\n', f);
        std::fclose(f);
    }

    // ─── CSV helpers ────────────────────────────────────────────────────────────
    // Deliberately duplicated (not shared) from EquipExtension.cpp's equivalents: those are file-
    // local `static` functions with internal linkage, not exposed anywhere a second translation
    // unit could reach them. Small and self-contained enough that duplicating is simpler than
    // introducing a shared header just for this; worth factoring out if a third sidecar-driven
    // script shows up and this becomes a third copy.

    static void CopyString(char* out, size_t outSz, const char* value) noexcept
    {
        if (!out || outSz == 0) return;
        out[0] = '\0';
        if (!value) return;
        std::strncpy(out, value, outSz - 1);
        out[outSz - 1] = '\0';
    }

    static std::string TrimCopy(std::string value)
    {
        size_t first = 0;
        while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) ++first;
        size_t last = value.size();
        while (last > first)
        {
            const char c = value[last - 1];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            --last;
        }
        return value.substr(first, last - first);
    }

    static std::string NormalizeCsvName(const std::string& value)
    {
        std::string out;
        out.reserve(value.size());
        for (char c : value)
        {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out.push_back(c);
        }
        return out;
    }

    static std::vector<std::string> ParseCsvLine(const char* line)
    {
        std::vector<std::string> fields;
        std::string field;
        bool quoted = false;
        for (const char* p = line; *p; ++p)
        {
            char c = *p;
            if (c == '\r' || c == '\n')
            {
                if (!quoted) break;
            }
            if (quoted)
            {
                if (c == '"')
                {
                    if (p[1] == '"') { field.push_back('"'); ++p; }
                    else quoted = false;
                }
                else field.push_back(c);
            }
            else
            {
                if (c == '"') quoted = true;
                else if (c == ',') { fields.push_back(TrimCopy(field)); field.clear(); }
                else field.push_back(c);
            }
        }
        fields.push_back(TrimCopy(field));
        return fields;
    }

    static int FindCsvColumn(const std::vector<std::string>& header, const char* name)
    {
        const std::string wanted = NormalizeCsvName(name);
        for (size_t i = 0; i < header.size(); ++i)
            if (NormalizeCsvName(header[i]) == wanted)
                return static_cast<int>(i);
        return -1;
    }

    static const char* CsvField(const std::vector<std::string>& row, int column) noexcept
    {
        if (column < 0 || static_cast<size_t>(column) >= row.size()) return "";
        return row[static_cast<size_t>(column)].c_str();
    }

    static bool ParseU32(const char* text, uint32_t* out) noexcept
    {
        if (!text || !*text || !out) return false;
        char* end = nullptr;
        unsigned long value = std::strtoul(text, &end, 10);
        if (end == text) return false;
        *out = static_cast<uint32_t>(value);
        return true;
    }

    static bool ReadSidecarLines(const char* path, std::vector<std::string>& lines)
    {
        lines.clear();
        if (!path || !*path) return false;

        if (FILE* f = std::fopen(path, "rb"))
        {
            char line[4096];
            while (std::fgets(line, sizeof(line), f))
                lines.emplace_back(line);
            std::fclose(f);
            return !lines.empty();
        }

        namespace io    = wxl::game::io;

        void* handle = nullptr;
        if (!io::FileOpen(path, offsets::io::kOpenWholeFile, &handle) || !handle)
            return false;

        uint32_t sizeHigh = 0;
        const uint32_t size = io::FileSize(handle, &sizeHigh);
        std::string bytes;
        bool ok = false;
        if (size > 0 && sizeHigh == 0)
        {
            bytes.resize(size);
            uint32_t got = 0;
            io::FileRead(handle, &bytes[0], size, &got);
            ok = (got == size);
        }
        io::FileClose(handle);
        if (!ok) return false;

        size_t start = 0;
        for (size_t i = 0; i <= bytes.size(); ++i)
        {
            if (i != bytes.size() && bytes[i] != '\n') continue;
            std::string line = bytes.substr(start, i - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            start = i + 1;
        }
        return !lines.empty();
    }

    // ─── Sidecar table ──────────────────────────────────────────────────────────
    // WXLCreatureTextures.csv columns: DisplayID, TextureType, TexturePath. Multiple rows per
    // DisplayID are expected -- one per baked texture layer (e.g. skin + extra glow/emissive).
    // TextureType is whatever numeric texture-unit type PatchTargetedMaterialTextures in
    // VirtualPath.cpp expects (same convention as the weapon material sidecar's TextureType column).
    // TexturePath is a literal archive-relative BLP path -- unlike the item sidecars, there is no
    // race/gender/folder-inference step here; creature texture variants don't follow that
    // convention, so the sidecar author is expected to give the real path directly.
    struct SidecarCreatureTextureEntry
    {
        uint32_t textureType = static_cast<uint32_t>(-1);
        char     texturePath[264] = {};
    };

    static bool g_sidecarLoaded = false;
    static std::unordered_map<uint32_t, std::vector<SidecarCreatureTextureEntry>> g_sidecarCreatureTextures;

    // WXLCreatureModels.csv columns: DisplayID, CreatureModelPath, Geoset (optional). One row per
    // DisplayID -- CreatureModelPath is the real archive path of the .mdx that displayId natively
    // resolves to (author-supplied, e.g. pulled from a DBC/SQL dump), same "known-good pair, no
    // in-memory guesswork" convention as EquipExtension's WXLItemEntryDisplay.csv. This is what makes eager
    // preregistration possible for creatures: without it, the only way to learn a displayId's real
    // model path is the native CreatureDisplayInfo -> CreatureModelData lookup chain, which isn't a
    // real callable function (see DB2.hpp's kCaptureDisplayId/kResolveMerge comments) -- walking its
    // id tables by hand at module-load time is fragile against whatever state the client's own DB2
    // storage happens to be in that early, and empirically has taken the client down outright rather
    // than failing safely inside the __try/__except guards. Only displayIds listed in this file are
    // eligible for eager preregistration; anything else simply isn't patched (there is no reactive
    // fallback anymore -- see OnModelLoadPre's doc comment for why).
    static std::unordered_map<uint32_t, std::string> g_sidecarCreatureModelPath;

    // Parsed form of WXLCreatureModels.csv's "Geoset" column. count == 0 means "no filter, render
    // every geoset natively present" -- same meaning as VPathPopulate/VPathPopulateGlobal's own
    // geoCount == 0. Cap of 16 mirrors EquipExtension's GeosetFilter.ids[16].
    struct CreatureGeosetSpec
    {
        uint16_t ids[16] = {};
        uint32_t count   = 0;
    };

    // Parses the "Geoset" column:
    //   empty            -> count=0, no filter applied, every geoset renders exactly as the model
    //                       file natively has it
    //   "0"              -> count=1, ids=[0] -- keep ONLY skinSectionId 0 (the Skin/base geoset;
    //                       every other section gets zeroed out of the .skin bytes)
    //   "xxxx,yyyy,..."  -> count=N+1, ids=[0, xxxx, yyyy, ...] -- Skin (0) is always implied
    //                       alongside whatever ids are explicitly listed, same way a creature's own
    //                       base body is never something you have to ask for separately; an
    //                       explicit "0" in the list is just a no-op duplicate of the implied one,
    //                       not a second entry
    static CreatureGeosetSpec ParseCreatureGeosetSpec(const char* spec)
    {
        CreatureGeosetSpec f = {};
        if (!spec || !*spec) return f; // empty column -- no filter, render every geoset

        f.ids[f.count++] = 0; // Skin geoset is always implied once any filtering is requested at all

        const char* p = spec;
        while (*p && f.count < 16)
        {
            while (*p == ' ' || *p == '\t' || *p == ',') ++p;
            if (*p < '0' || *p > '9') break;

            uint32_t v = 0;
            while (*p >= '0' && *p <= '9') v = v * 10u + static_cast<uint32_t>(*p++ - '0');

            bool dup = false;
            for (uint32_t i = 0; i < f.count; ++i)
                if (f.ids[i] == static_cast<uint16_t>(v)) { dup = true; break; }
            if (!dup) f.ids[f.count++] = static_cast<uint16_t>(v);
        }
        return f;
    }

    // Keyed the same way, and dedup'd the same first-seen-wins way, as g_sidecarCreatureModelPath --
    // populated from the same WXLCreatureModels.csv row, just a different column.
    static std::unordered_map<uint32_t, CreatureGeosetSpec> g_sidecarCreatureGeoset;

    static void LoadCreatureTextureSidecarFile(const char* path)
    {
        std::vector<std::string> lines;
        if (!ReadSidecarLines(path, lines)) return;

        const std::vector<std::string> header = ParseCsvLine(lines[0].c_str());
        const int cDisplay     = FindCsvColumn(header, "DisplayID");
        const int cTextureType = FindCsvColumn(header, "TextureType");
        const int cTexturePath = FindCsvColumn(header, "TexturePath");

        if (cDisplay < 0 || cTextureType < 0 || cTexturePath < 0)
        {
            CreatureLog("creature texture sidecar '%s': missing DisplayID, TextureType, or "
                        "TexturePath column", path);
            return;
        }

        uint32_t loaded = 0;
        for (size_t lineIndex = 1; lineIndex < lines.size(); ++lineIndex)
        {
            if (lines[lineIndex].empty()) continue;
            const std::vector<std::string> row = ParseCsvLine(lines[lineIndex].c_str());

            uint32_t displayId = 0;
            if (!ParseU32(CsvField(row, cDisplay), &displayId) || displayId == 0) continue;

            SidecarCreatureTextureEntry e = {};
            if (!ParseU32(CsvField(row, cTextureType), &e.textureType)) continue;
            CopyString(e.texturePath, sizeof(e.texturePath), CsvField(row, cTexturePath));
            if (!e.texturePath[0]) continue;

            // Dedup by (displayId, textureType), first-seen wins. LoadCreatureSidecar() checks
            // several search paths (bare filename, DBFilesClient\, every mounted patch MPQ's
            // DBFilesClient\) -- if the same physical CSV is reachable via more than one of those,
            // every row would otherwise be parsed once per path that resolves to it, silently
            // doubling (or worse) the material patch spec built from this table.
            auto& rows = g_sidecarCreatureTextures[displayId];
            bool duplicate = false;
            for (const SidecarCreatureTextureEntry& existing : rows)
            {
                if (existing.textureType == e.textureType) { duplicate = true; break; }
            }
            if (duplicate) continue;

            rows.push_back(e);
            ++loaded;
        }

        if (loaded)
            CreatureLog("creature texture sidecar loaded '%s' rows=%u", path, loaded);
    }

    static void LoadCreatureModelPathSidecarFile(const char* path)
    {
        std::vector<std::string> lines;
        if (!ReadSidecarLines(path, lines)) return;

        const std::vector<std::string> header = ParseCsvLine(lines[0].c_str());
        const int cDisplay = FindCsvColumn(header, "DisplayID");
        const int cModel   = FindCsvColumn(header, "CreatureModelPath");
        const int cGeoset  = FindCsvColumn(header, "Geoset"); // optional -- absent is fine, not an error

        if (cDisplay < 0 || cModel < 0)
        {
            CreatureLog("creature model-path sidecar '%s': missing DisplayID or CreatureModelPath "
                        "column", path);
            return;
        }

        uint32_t loaded = 0;
        for (size_t lineIndex = 1; lineIndex < lines.size(); ++lineIndex)
        {
            if (lines[lineIndex].empty()) continue;
            const std::vector<std::string> row = ParseCsvLine(lines[lineIndex].c_str());

            uint32_t displayId = 0;
            if (!ParseU32(CsvField(row, cDisplay), &displayId) || displayId == 0) continue;

            char modelPath[264];
            CopyString(modelPath, sizeof(modelPath), CsvField(row, cModel));
            if (!modelPath[0]) continue;

            // First-seen wins, same dedup convention as the texture sidecar -- guards against the
            // same physical CSV being reachable (and therefore re-parsed) via more than one of
            // LoadCreatureSidecar's search paths.
            if (g_sidecarCreatureModelPath.find(displayId) != g_sidecarCreatureModelPath.end())
                continue;

            g_sidecarCreatureModelPath.emplace(displayId, modelPath);
            if (cGeoset >= 0)
                g_sidecarCreatureGeoset.emplace(displayId, ParseCreatureGeosetSpec(CsvField(row, cGeoset)));
            ++loaded;
        }

        if (loaded)
            CreatureLog("creature model-path sidecar loaded '%s' rows=%u", path, loaded);
    }

    // Forward decl: full definition sits after BuildCreatureMaterialPatchSpec (needs it), but must
    // be called from the end of LoadCreatureSidecar below -- same shape as EquipExtension's
    // LoadSidecarModels calling PreregisterSidecarWeapons at its own end.
    static void PreregisterSidecarCreatures();

    // Same search pattern as EquipExtension's LoadSidecarModels: bare filename, DBFilesClient\
    // beside it, and DBFilesClient\ inside every mounted Data\*.MPQ directory (patch overrides).
    static void LoadCreatureSidecar()
    {
        if (g_sidecarLoaded) return;
        g_sidecarLoaded = true;

        LoadCreatureTextureSidecarFile("WXLCreatureTextures.csv");
        LoadCreatureTextureSidecarFile("DBFilesClient\\WXLCreatureTextures.csv");
        LoadCreatureModelPathSidecarFile("WXLCreatureModels.csv");
        LoadCreatureModelPathSidecarFile("DBFilesClient\\WXLCreatureModels.csv");

        WIN32_FIND_DATAA fd = {};
        HANDLE h = FindFirstFileA("Data\\*.MPQ", &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                std::string base = "Data\\";
                base += fd.cFileName;
                base += "\\DBFilesClient\\";
                LoadCreatureTextureSidecarFile((base + "WXLCreatureTextures.csv").c_str());
                LoadCreatureModelPathSidecarFile((base + "WXLCreatureModels.csv").c_str());
            }
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }

        CreatureLog("creature sidecar table ready: displays=%zu textures, %zu model-paths",
                    g_sidecarCreatureTextures.size(), g_sidecarCreatureModelPath.size());

        // Deliberately called here, at the end of the CSV load, NOT from CreatureExtension's
        // constructor. The constructor runs on the file-scope global at DLL static-init time
        // (DllMain/DLL_PROCESS_ATTACH) -- before the client's own engine subsystems (archive
        // mounting, file I/O) are guaranteed to be up. LoadCreatureSidecar, by contrast, only ever
        // runs lazily, the first time OnModelLoadPre fires -- i.e. as soon as anything at all is
        // rendered, including the login/char-select glue scene, well before any world/gear state
        // exists. Same reasoning as EquipExtension: PreregisterSidecarWeapons is called from the end
        // of LoadSidecarModels, not from EquipExtension's constructor, for the same
        // static-init-timing reason.
        //
        // Default true (matches the old always-on behavior) if the ini/section/key is missing -- see
        // WxlIniGetBool's doc comment. Turning this off relies entirely on CreatureLazyResolve (see
        // its own doc comment, and its registration in the constructor below) to still bake anything
        // at all -- unlike weapons, there is no separate reactive per-equip hook for creature models
        // to fall back on.
//        if (WxlIniGetBool("EagerPreload", "Creatures", true))
//            PreregisterSidecarCreatures();
    }

    // Builds a "TextureType=TexturePath|TextureType=TexturePath|..." spec for displayId, in the
    // exact format PatchTargetedMaterialTextures (VirtualPath.cpp) expects -- same format
    // EquipExtension's weapon material sidecar produces, just without any modelColumn/stem matching
    // since creature texture rows aren't disambiguated by anything but DisplayID.
    static void BuildCreatureMaterialPatchSpec(char* out, size_t outSz, uint32_t displayId)
    {
        if (!out || outSz == 0) return;
        out[0] = '\0';

        auto it = g_sidecarCreatureTextures.find(displayId);
        if (it == g_sidecarCreatureTextures.end()) return;

        size_t used = 0;
        for (const SidecarCreatureTextureEntry& e : it->second)
        {
            if (e.textureType == static_cast<uint32_t>(-1) || !e.texturePath[0]) continue;

            char item[300];
            int n = std::snprintf(item, sizeof(item), "%s%u=%s",
                                  used ? "|" : "", e.textureType, e.texturePath);
            if (n <= 0) continue;
            const size_t len = static_cast<size_t>(n);
            if (len >= sizeof(item) || used + len >= outSz) break;
            std::memcpy(out + used, item, len);
            used += len;
            out[used] = '\0';
        }
    }

    // Bakes displayId's patched bytes into the process-lifetime override table, if the sidecar tables
    // know a model path for it (WXLCreatureModels.csv) and there's something to actually bake --
    // texture rows (WXLCreatureTextures.csv) and/or a non-empty Geoset column entry. Shared by both
    // PreregisterSidecarCreatures (eager, walks every known displayId at startup) and
    // CreatureLazyResolve (lazy, one displayId at a time, on demand) so the bake logic only exists
    // once. Assumes the sidecar tables are already populated -- both current callers route through
    // LoadCreatureSidecar first.
    static bool BakeCreatureDisplay(uint32_t displayId)
    {
        auto pathIt = g_sidecarCreatureModelPath.find(displayId);
        if (pathIt == g_sidecarCreatureModelPath.end() || pathIt->second.empty()) return false;

        char matSpec[2048] = {};
        BuildCreatureMaterialPatchSpec(matSpec, sizeof(matSpec), displayId);

        const CreatureGeosetSpec* geoSpec = nullptr;
        auto geoIt = g_sidecarCreatureGeoset.find(displayId);
        if (geoIt != g_sidecarCreatureGeoset.end() && geoIt->second.count > 0) geoSpec = &geoIt->second;

        if (!matSpec[0] && !geoSpec)
        {
            // Model path known, but no texture rows AND no (non-empty) Geoset column entry --
            // nothing to bake.
            return false;
        }

        // No texPath (only materialPatchSpec) -- creature texture rows are all TextureType-keyed.
        char vModelPath[280] = {};
        bool registered = VPathPopulateGlobal(pathIt->second.c_str(), displayId, nullptr, matSpec,
                                               geoSpec ? geoSpec->ids : nullptr,
                                               geoSpec ? geoSpec->count : 0,
                                               true, // evictable -- CreatureLazyResolve rebakes on miss
                                               vModelPath, sizeof(vModelPath));
        CreatureLog("  bake: display=%u real='%s' vpath='%s' spec='%s' geoCount=%u registered=%d",
                    displayId, pathIt->second.c_str(), vModelPath, matSpec,
                    geoSpec ? geoSpec->count : 0u, registered ? 1 : 0);
        return registered && vModelPath[0];
    }

    // Walks every displayId listed in WXLCreatureModels.csv (with something to actually bake -- see
    // BakeCreatureDisplay) and bakes its patched bytes into the process-lifetime override table under
    // the exact virtual .m2 name CreatureModelData's ModelName field already names for that displayId
    // (patched at the data level, outside this module -- see OnModelLoadPre's doc comment). There is
    // no race to lose here anymore: the native loader always asks for that virtual name directly, on
    // every spawn/transform/relog, so this only needs to run once, sometime before the first such
    // request -- it doesn't matter which creature asks first.
    //
    // Gated behind WXLExtendedEquipment.ini's [EagerPreload] Creatures key (default true) -- see its
    // call site's comment for why turning it off, unlike weapons, needs CreatureLazyResolve alongside
    // it. Deliberately sidecar-only either way: only displayIds with an explicit WXLCreatureModels.csv
    // row are ever eligible, eager or lazy.
    static void PreregisterSidecarCreatures()
    {
        // No LoadCreatureSidecar() call here -- this function is only ever invoked from the end of
        // LoadCreatureSidecar itself (see its call site's comment), so the sidecar tables below are
        // already guaranteed populated by the time we get here.
        if (g_sidecarCreatureModelPath.empty()) return;

        CreatureLog("creature preregister: %zu display(s) with a known model path, %zu with texture rows",
                    g_sidecarCreatureModelPath.size(), g_sidecarCreatureTextures.size());

        uint32_t registeredCount = 0;
        for (const auto& [displayId, realModelPath] : g_sidecarCreatureModelPath)
        {
            if (BakeCreatureDisplay(displayId)) ++registeredCount;
        }

        CreatureLog("creature preregister: done, %u display(s) registered ahead of first spawn",
                    registeredCount);
    }

    // Lazy on-demand counterpart to PreregisterSidecarCreatures, registered with VirtualPath.cpp via
    // VPathRegisterLazyResolver and invoked from VirtualProvide on a g_globalOverrides miss. Runs
    // unconditionally regardless of the [EagerPreload] Creatures ini setting -- with eager preload on,
    // this is a safety net for any displayId that slips past the startup sweep; with it off, this is
    // the ONLY path that ever bakes a creature override at all (unlike weapons, there is no separate
    // reactive per-equip hook creature models can fall back on).
    //
    // normVirtualPath is whatever VirtualProvide's own NormalizeRealPath produced from the raw loader
    // request -- already lowercase with a .m2 extension, the form VPathDecodeGlobalVirtualKey expects.
    // A successful decode only confirms the name has the right *shape* (see that function's doc
    // comment); the decoded displayId is only trusted once it's confirmed present in
    // g_sidecarCreatureModelPath -- the same table PreregisterSidecarCreatures itself walks -- and
    // BakeCreatureDisplay re-derives everything it bakes from that table (and g_sidecarCreatureTextures
    // / g_sidecarCreatureGeoset), not from the decoded path, so a coincidental decode of an unrelated
    // real archive path can't smuggle in bytes for the wrong model.
    static bool CreatureLazyResolve(const char* normVirtualPath)
    {
        LoadCreatureSidecar(); // no-op after the first call; must run before the sidecar tables are read

        char decodedPath[264] = {};
        uint32_t displayId = 0;
        if (!VPathDecodeGlobalVirtualKey(normVirtualPath, decodedPath, sizeof(decodedPath), &displayId))
            return false;

        if (g_sidecarCreatureModelPath.find(displayId) == g_sidecarCreatureModelPath.end())
            return false; // not a displayId this module knows about

        CreatureLog("  CreatureLazyResolve: miss '%s' decoded displayId=%u -- baking now",
                    normVirtualPath, displayId);
        return BakeCreatureDisplay(displayId);
    }

    CreatureExtension::CreatureExtension()
    {
        // Deliberately does NOT call LoadCreatureSidecar/PreregisterSidecarCreatures here -- this
        // constructor runs on the file-scope global at DLL static-init time (DllMain), before the
        // client's own archive/file-I/O subsystems are guaranteed up.
        on<&CreatureExtension::OnModelLoadPre>(ev::Event::OnModelLoadPre);

        // Cheap (one vector push_back, no I/O) so it's safe here regardless of engine subsystem init
        // order -- see VPathRegisterLazyResolver's doc comment in VirtualPath.hpp. Registered
        // unconditionally, independent of the [EagerPreload] Creatures ini setting, so lazy baking
        // works whether or not eager preload is turned on.
        VPathRegisterLazyResolver(&CreatureLazyResolve);
    }

    // See the doc comment on the declaration in CreatureExtension.hpp for why OnModelLoadPre and not
    // OnWorldEnter/OnItemSlotChange.
    void CreatureExtension::OnModelLoadPre(const ev::ModelLoadArgs&)
    {
        static bool kicked = false;
        if (kicked) return;
        kicked = true;
        LoadCreatureSidecar(); // no-op after the first call; runs PreregisterSidecarCreatures at its end
    }

    // Self-registration: file-scope instance binds handlers at DLL load via EventScript ctor.
    // OLD: file-scope self-registering global. Removed -- CreatureExtension is now constructed
    // from EquipExtension.cpp's shared WXL_Load(), after wxl::ext::EventScript::Bind(api). This
    // file defines no entry points of its own (only one WXL_Query/WXL_Load pair per DLL).
}
