// wxl-equip-extension: equipment slot extension features for WarcraftXL.
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

#include "EquipExtension.hpp"
#include "CreatureExtension.hpp"
#include "WeaponExtension.hpp"
#include "WxlOffsets.hpp"
#include "VirtualPath.hpp"
#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "game/Binding.hpp"
#include "game/Io.hpp"
#include "game/M2.hpp"
#include "offsets/game/M2.hpp"
#include "game/Unit.hpp"

#include <windows.h>
#include <intrin.h> // _ReturnAddress() -- used by the diagnostic hooks below

#include <cstddef>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::scripts::equipextension
{
    namespace ev   = wxl::events;
    namespace gm2  = wxl::game::m2;
    using wxl::game::Native;

    // ─── Data types ──────────────────────────────────────────────────────────────

    // List of skinSectionIds to show. count == 0 means "show all" (no filter applied).
    // WoW geoset IDs are two-part values (e.g. 1301 = group 13, option 01).
    struct GeosetFilter
    {
        uint16_t ids[16];
        uint32_t count;
    };

    // Maps collection bone index → character bone index (0xFF = unmatched).
    // Built by 3-pass matching: (1a) BoneNameCRC, (1b) key_bone_id raw scan, (1c) BoneIndicesByID LUT,
    //  then (2) parent-chain propagation.
    // Retail collection M2s (full-body chest pieces etc.) routinely have 60-100+ bones;
    // 48 was too small and caused the early-exit, leaving count=0 for all collection models.
    struct BoneRemap
    {
        uint16_t count;
        uint8_t  collToChar[256];
        void*    collectionModel; // live M2 model this remap was built for (collection side)
        void*    characterModel;  // live M2 model this remap was built for (character/pose side)
    };

    struct SidecarModelEntry
    {
        uint32_t modelSlot    = static_cast<uint32_t>(-1);
        uint32_t attachId     = static_cast<uint32_t>(-1);
        uint32_t modelFlags   = 0x2 | 0x4; // exact path unless the sidecar asks for race/gender suffixes
        uint32_t textureFlags = 0;
        char     folder[32]   = {};
        char     model[264]   = {};
        char     texture[264] = {};
        GeosetFilter geoFilter = {};
    };

    struct SidecarMaterialEntry
    {
        uint32_t modelIndex  = static_cast<uint32_t>(-1);
        uint32_t modelColumn = static_cast<uint32_t>(-1);
        uint32_t textureType = static_cast<uint32_t>(-1);
        char     folder[32]  = {};
        char     model[264]  = {};
        char     texture[264] = {};
        char     targetMode[32] = {}; // only "Hide" is consulted now, to skip a row entirely
    };

    // One attachment entry per attached M2. Keyed by CharModelObject pointer in g_attached.
    struct AttachEntry
    {
        uint32_t    equipSlot = static_cast<uint32_t>(-1); // internal model slot (0-10)
        uint32_t    attachId  = static_cast<uint32_t>(-1); // WoW attachment point id
        void*       subObj    = nullptr; // cmo->sceneNode stamped at last rebuild
        void*       renderCtx = nullptr; // live render context; null = evicted/dead
        char        keyBuf[264] = {};        // real M2 path (used for matching/dedup)
        char        mangledKeyBuf[264] = {}; // virtual _wxl_ path for GetRenderCtx (collection only)
        char        texBuf[264] = {};    // BLP path for BindTexSlot on re-attach
        char        matTexBuf[2048] = {}; // TextureType-keyed material texture patches for virtual M2 bytes
        GeosetFilter geoFilter  = {};
        BoneRemap    boneRemap  = {};
        uint32_t    mergeKey = 0; // non-zero keeps logical semicolon collection models separate
        bool        perFrameLogged    = false; // suppress repeated per-frame noise after first log
        bool        charSweepApplied = false; // set when character-model PerFrame sweep first copies bones
        bool        bbpLogDone       = false; // suppress OnBuildBonePalette remap dump after first fire
        bool        cloneBbpLogDone  = false; // suppress duplicate-ModelFrame fallback noise
        bool        virtualPopulateFailed = false; // NeedsVirtualModel() was true but VPathPopulate
            // could not read the real backing file (mdx/skin read failure -- see VPathPopulate log).
            // Phase1 must NOT fall back to e.keyBuf (the raw, unpatched path) for these: that path was
            // already proven unreadable by the same failed populate call, the native GetRenderCtx can
            // still return a non-null placeholder/error-model context for it regardless, and feeding
            // that placeholder into BuildBoneRemap/geoset-filter code that assumes the real collection
            // mesh's layout is a memory-safety violation waiting to happen, not a cosmetic miss.
        uint16_t    texAnimProbeFrames = 0; // short targeted UV-animation diagnostic
        uint32_t    texAnimProbeFirst[4] = {};
        uint8_t     texAnimProbeChanged = 0;											
    };

    // ─── File-scope state ─────────────────────────────────────────────────────────
    //
    // The weapon attachment scaffold (WeaponAttachEntry / g_weaponAttached) and the weapon
    // item-display/folder/vpath-override sidecar tables (g_sidecarItemDisplay, g_sidecarModelFolder,
    // WeaponVPathOverride / g_weaponVPaths) that used to live here are gone -- they existed solely
    // to feed EquipExtension's old runtime weapon model-swap path (LookupItemDisplayId,
    // PatchWeaponModelByDisplayId, OnWeaponVisualChange, OnItemDisplayLookup), all removed now that
    // WeaponExtension patches ItemModelData.dbc at the data level instead. See WeaponExtension.cpp.

static EquipExtension* g_equipInstance = nullptr; // set in the constructor, used by the raw hooks below

    // cmo (CharModelObject*) → attached M2 entries for that character
    static std::unordered_map<void*, std::vector<AttachEntry>> g_attached;

    static bool g_sidecarLoaded = false;
    static std::unordered_map<uint32_t, std::vector<SidecarModelEntry>> g_sidecarModels;
    static std::unordered_map<uint32_t, std::vector<SidecarMaterialEntry>> g_sidecarMaterials;

    // Set to the cmo being processed in RebuildAllModels Phase1. OnM2SkinFinalize uses
    // this to apply the filter during a synchronous load that fires kFinalizeSkin inside
    // GetRenderCtx, before Phase1 has set entry.renderCtx.
    static void* g_currentRebuildCmo = nullptr;

    // ─── Slot config ──────────────────────────────────────────────────────────────

    struct SlotConfig
    {
        const char* folder;
        uint32_t    defAttach1;
        uint32_t    defAttach2;
        bool        defUseRace;
        bool        defUseGender;
    };

    // Indexed by internal model slot (0-13), matching sub_4f2640's first arg.
    //
    // Slots 11-13 (weapons) are new: unlike armor slots, the client already natively
    // attaches the weapon's own ModelName_1/2 at these attach points, so these entries
    // mainly matter for (a) sidecar-driven material patches on the native weapon model
    // and (b) any *additional* attached M2 the sidecar/DBC asks for. Default attach IDs
    // follow the classic ItemVisual convention: 1=HandRight (mainhand), 2=HandLeft
    // (offhand/shield), 3/4=ElbowRight/Left, dual-purposed as the ranged weapon + quiver
    // attach points. Verify against WXL_EQUIP_LOG output for your client build; the
    // upstream WarcraftXL core hook must also dispatch OnItemSlotChange/OnItemSlotClear
    // for these WoW equip slots (15/16/17) with a matching modelSlot value for any of
    // this to fire at all -- see the caveat in the PR/README notes.
    static const SlotConfig kSlotConfig[14] = {
        { "Head",     11,  55, true,  true  }, // 0  HEAD
        { "Shoulder",  6,   5, false, false }, // 1  SHOULDER (Model1=left/6, Model2=right/5)
        { "Shirt",    34,  34, false, false }, // 2  SHIRT
        { "Chest",    34,  34, false, false }, // 3  CHEST
        { "Waist",    53,  53, false, false }, // 4  WAIST
        { "Leg",       9,  10, false, false }, // 5  LEGS
        { "Foot",     47,  48, false, false }, // 6  FEET
        { "Bracer",    3,   4, false, false }, // 7  WRIST
        { "Glove",     1,   2, false, false }, // 8  HAND
        { "Cape",     12,  12, false, false }, // 9  BACK
        { "Tabard",   34,  34, false, false }, // 10 TABARD
        { "Weapon",    1,   1, false, false }, // 11 MAINHAND (HandRight_ItemVisual1)
        { "OffHand",    2,   2, false, false }, // 12 OFFHAND  (HandLeft_ItemVisual2, also shields)
        { "Ranged",    3,   4, false, false }, // 13 RANGED   (bow/gun=3, quiver/ammo=4)
    };
    constexpr uint32_t kCollectionAttach = 19;
    constexpr uint32_t kFlagForceModelRaceGender = 0x80;

    // WoW C++ equip slot (0-18) → internal model slot. UINT32_MAX = not handled here.
    static const uint32_t kEquipToModelSlot[19] = {
        0,                       // 0  HEAD
        static_cast<uint32_t>(-1), // 1  NECK
        1,                       // 2  SHOULDER
        2,                       // 3  SHIRT
        3,                       // 4  CHEST
        4,                       // 5  WAIST
        5,                       // 6  LEGS
        6,                       // 7  FEET
        7,                       // 8  WRIST
        8,                       // 9  HAND
        static_cast<uint32_t>(-1), // 10 deferred (FINGER1)
        static_cast<uint32_t>(-1), // 11 deferred (FINGER2)
        static_cast<uint32_t>(-1), // 12 deferred (TRINKET1)
        static_cast<uint32_t>(-1), // 13 deferred (TRINKET2)
        9,                       // 14 BACK
        15,                      // 15 MAINHAND
        16,                      // 16 OFFHAND
        17,                      // 17 RANGED
        10,                      // 18 TABARD
    };

    // ─── Diagnostic call-order sequence (temporary -- weapon mainhand/offhand investigation) ─────
    // Shared by OnItemSlotChange/OnItemSlotClear and the diagnostic hooks below (ItemDisplayInfo-
    // LookupDetour, CharAddHandItemDetour) so their WarcraftXL_equip.log lines can be sorted back
    // into true call order even when GetTickCount()'s ~15ms resolution can't tell two calls apart.
    // InterlockedIncrement rather than a plain ++ since detours can in principle run off the main
    // thread (loader/streaming threads touch some of these paths) and this counter must never hand
    // out the same value twice. Remove alongside the diagnostic hooks once the investigation is
    // done -- a normal build has no use for this.
    static LONG g_diagSeq = 0;
    static uint32_t NextDiagSeq() noexcept { return static_cast<uint32_t>(InterlockedIncrement(&g_diagSeq)); }

    // ─── Debug logging ───────────────────────────────────────────────────────────

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

    // Verbose equip diagnostics are opt-in. They touch very hot model/skin/per-frame paths, and opening
    // + flushing the log for every line can dominate frame time on HD-equipment-heavy scenes.
    static void EquipLog(const char* fmt, ...) noexcept
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

    static bool SameAttachModelGroup(const AttachEntry& a, const AttachEntry& b) noexcept
    {
        const bool aFiltered = a.geoFilter.count > 0;
        const bool bFiltered = b.geoFilter.count > 0;
        return a.mergeKey == b.mergeKey &&
               a.attachId == b.attachId &&
               aFiltered == bFiltered &&
               std::strcmp(a.keyBuf, b.keyBuf) == 0 &&
               std::strcmp(a.texBuf, b.texBuf) == 0 &&
               std::strcmp(a.matTexBuf, b.matTexBuf) == 0;
    }

    // ─── SEH helpers (no C++ objects — safe to use __try/__except) ───────────────

    static uint32_t GuardedReadU32(const void* ptr) noexcept
    {
        uint32_t v = 0;
        __try { v = *static_cast<const uint32_t*>(ptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return v;
    }

    static void* GuardedReadPtr(const void* addr) noexcept
    {
        void* v = nullptr;
        __try { v = *static_cast<void* const*>(addr); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return v;
    }

    static void* NativeParentGuarded(void* instance) noexcept
    {
        if (!instance) return nullptr;
        void* parent = GuardedReadPtr(
            reinterpret_cast<uint8_t*>(instance) + offsets::kOffInstParent);
        return parent != instance ? parent : nullptr;
    }

    // Locate the character instance inside a duplicated/parented M2 tree by shared-model identity.
    // We deliberately do not choose the absolute root: a character can itself be attached to a
    // mount or vehicle, while the collection mesh still needs the character's bone palette.
    static void* FindAncestorWithModelGuarded(void* instance, void* wantedModel) noexcept
    {
        if (!instance || !wantedModel) return nullptr;

        void* seen[32] = {};
        void* current = NativeParentGuarded(instance);
        for (uint32_t depth = 0; depth < 32; ++depth)
        {
            if (!current) return nullptr;
            for (uint32_t i = 0; i < depth; ++i)
                if (seen[i] == current) return nullptr;
            seen[depth] = current;

            if (GuardedReadPtr(reinterpret_cast<uint8_t*>(current) + offsets::kOffInstModel) ==
                wantedModel)
                return current;
            current = NativeParentGuarded(current);
        }
        return nullptr;
    }

    static bool BoneRemapHasMatch(const BoneRemap& remap) noexcept
    {
        if (remap.count == 0 || remap.count > 65535) return false;
        for (uint32_t i = 0; i < remap.count; ++i)
            if (remap.collToChar[i] != 0xFF) return true;
        return false;
    }

    static bool BoneRemapMatchesContextsGuarded(const BoneRemap& remap,
                                                void* collectionCtx,
                                                void* characterCtx) noexcept
    {
        if (!BoneRemapHasMatch(remap) || !collectionCtx || !characterCtx ||
            collectionCtx == characterCtx || !remap.collectionModel || !remap.characterModel)
            return false;

        return GuardedReadPtr(reinterpret_cast<uint8_t*>(collectionCtx) + offsets::kOffInstModel) ==
                   remap.collectionModel &&
               GuardedReadPtr(reinterpret_cast<uint8_t*>(characterCtx) + offsets::kOffInstModel) ==
                   remap.characterModel;
    }

    static uint32_t RenderCtxBoneCountGuarded(void* renderCtx) noexcept
    {
        if (!renderCtx) return 0;
        __try
        {
            void* model = *reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(renderCtx) + offsets::kOffInstModel);
            if (!model) return 0;
            auto* header = *reinterpret_cast<uint8_t**>(
                reinterpret_cast<uint8_t*>(model) + offsets::kOffModelHeader);
            if (!header) return 0;
            const uint32_t count = *reinterpret_cast<uint32_t*>(header + offsets::kOffHdrBoneCount);
            return count <= 65535 ? count : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    static uint32_t GuardedMatrixHash(const void* matrix) noexcept
    {
        if (!matrix) return 0;
        uint32_t hash = 2166136261u;
        __try
        {
            const auto* words = static_cast<const uint32_t*>(matrix);
            for (uint32_t i = 0; i < 16; ++i)
                hash = (hash ^ words[i]) * 16777619u;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
													  
													 
        {
            return 0;
																						
								  
																									   
							   
												   
        }
        return hash;
    }

    static void ProbeRaidWarriorTextureAnimation(AttachEntry& entry, void* renderCtx) noexcept
    {
        if (entry.texAnimProbeFrames >= 1200 ||
            !std::strstr(entry.keyBuf, "raidwarriorprogenitor_d_01")) return;

        void* evaluated = GuardedReadPtr(reinterpret_cast<uint8_t*>(renderCtx) + 0xAC);
        void* matrices  = GuardedReadPtr(reinterpret_cast<uint8_t*>(renderCtx) + 0xB0);
        const uint32_t hashes[4] = {
            GuardedMatrixHash(evaluated),
            GuardedMatrixHash(evaluated ? reinterpret_cast<uint8_t*>(evaluated) + 0x40 : nullptr),
            GuardedMatrixHash(matrices),
            GuardedMatrixHash(matrices ? reinterpret_cast<uint8_t*>(matrices) + 0x40 : nullptr)
        };

        if (entry.texAnimProbeFrames == 0)
            std::memcpy(entry.texAnimProbeFirst, hashes, sizeof(hashes));
        else
            for (uint32_t i = 0; i < 4; ++i)
                if (hashes[i] && hashes[i] != entry.texAnimProbeFirst[i])
                    entry.texAnimProbeChanged |= static_cast<uint8_t>(1u << i);
																													
	 

        ++entry.texAnimProbeFrames;
        if (entry.texAnimProbeFrames == 1 || entry.texAnimProbeFrames == 30 ||
            entry.texAnimProbeFrames == 90 || entry.texAnimProbeFrames == 300 ||
            entry.texAnimProbeFrames == 600 || entry.texAnimProbeFrames == 900 ||
            entry.texAnimProbeFrames == 1200)
        {
            EquipLog("  TexAnimProbe frame=%u key='%s' eval=[%08X %08X] out=[%08X %08X] changed=0x%X",
                     static_cast<uint32_t>(entry.texAnimProbeFrames), entry.keyBuf,
                     hashes[0], hashes[1], hashes[2], hashes[3],
                     static_cast<uint32_t>(entry.texAnimProbeChanged));
																							   
											
        }
														  
    }

    static bool CopyRemappedBonesGuarded(uint8_t* dstBuf,
                                         uint32_t dstBoneCount,
                                         const uint8_t* srcBuf,
                                         uint32_t srcBoneCount,
                                         const BoneRemap& remap,
                                         void* dstRenderCtx,
                                         void* srcRenderCtx) noexcept
    {
        // The palette is allocated as modelBoneCount * 0x40 by the engine. A stale render-context
        // address can be recycled for a smaller model while g_attached still holds the old remap;
        // an unguarded copy loop would then write beyond that new palette and corrupt the heap.
        // Require the live destination model to have the exact count the remap was built for and
        // independently bind every source index to the live character model.
        if (!dstBuf || !srcBuf || dstBuf == srcBuf || !dstRenderCtx || !srcRenderCtx ||
            remap.count == 0 || remap.count > 65535 ||
            dstBoneCount == 0 || srcBoneCount == 0 ||
            remap.count != dstBoneCount || srcBoneCount > 65535 ||
            !remap.collectionModel || !remap.characterModel ||
            GuardedReadPtr(reinterpret_cast<uint8_t*>(dstRenderCtx) + offsets::kOffInstModel) !=
                remap.collectionModel ||
            GuardedReadPtr(reinterpret_cast<uint8_t*>(srcRenderCtx) + offsets::kOffInstModel) !=
                remap.characterModel)
            return false;

        __try
        {
            for (uint32_t bi = 0; bi < remap.count; ++bi)
            {
                uint8_t ci = remap.collToChar[bi];
                if (ci == 0xFF) continue;
                if (ci >= srcBoneCount) continue;
                std::memcpy(dstBuf + bi * offsets::kBonePaletteStride,
                            srcBuf + ci * offsets::kBonePaletteStride,
                            offsets::kBonePaletteStride);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    // SEH-only wrapper: no C++ objects in this function, __try is safe.
    // Catches access violations from garbage/dangling hash-table buckets — see RebuildAllModels.
    // owner28 = *(subObj + kOffSceneNodeOwner): the actual CharModelObject that owns the
    // M2 hash table — NOT the slot-dispatch 'cmo' from hkSlotDispatch, which is a different
    // (wrapper) object whose +4 hash-table pointer is always null.
    static void* SafeGetRenderCtx(void* owner28, const char* keyBuf) noexcept
    {
        void* result = nullptr;
        __try { result = gm2::GetRenderCtx(owner28, const_cast<char*>(keyBuf)); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return result;
    }

    // SEH-only wrapper: no C++ objects in this function, __try is safe.
    // Mirrors SafeGetRenderCtx's rationale, but for DetachSlot: this is the first place in the whole
    // porting effort where a non-virtual entry's Phase1 key has been observed to exactly match
    // vanilla's own already-attached key (see the "hkSlotDispatch runs vanilla first" comment above
    // Phase1). If gm2::GetRenderCtx's refcount semantics for an already-resident model differ even
    // slightly under core 1.1 from what that get-ref-before-detach ordering assumes, our extra ref
    // from Phase1 would be illusory and this call would free the node down to zero, leaving the
    // dangling-pointer crash the original comment predicted for the opposite (wrong) ordering. This
    // wrapper can't fix that if it's a real refcount regression, but it stops a fault here from
    // taking the whole client down, and the log line pinpoints exactly which attachId faulted.
    static void SafeDetachSlot(void* subObj, uint32_t attachId) noexcept
    {
        __try { gm2::DetachSlot(subObj, attachId); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            EquipLog("  DetachSlot CRASH CAUGHT attachId=%u subObj=%p (see comment at SafeDetachSlot)",
                     attachId, subObj);
        }
    }

    // SEH-only wrappers: no C++ objects in these functions, __try is safe.
    // gm2::LoadResource and gm2::BindTexSlot had zero exception protection -- the crash trail so
    // far has ruled out GetRenderCtx and DetachSlot in turn (both already guarded, both now proven
    // clean via logging), and the client stops dead before the very first Phase3 log line, which is
    // printed *after* both of these calls. This is the next narrowest window to isolate.
    static void* SafeLoadResource(const char* texBuf, int flag) noexcept
    {
        void* result = nullptr;
        __try { result = gm2::LoadResource(const_cast<char*>(texBuf), flag); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            EquipLog("  LoadResource CRASH CAUGHT tex='%s'", texBuf ? texBuf : "(null)");
        }
        return result;
    }

    static bool SafeBindTexSlot(void* rctx, void* tex) noexcept
    {
        __try { gm2::BindTexSlot(rctx, tex); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            EquipLog("  BindTexSlot CRASH CAUGHT rctx=0x%p tex=0x%p", rctx, tex);
            return false;
        }
    }


    // SEH wrapper for BuildBoneRemap's pointer walks.
    // BoneRemap is a POD struct (trivially destructible), so __try is safe inside this function.
    static BoneRemap BuildBoneRemapGuarded(void* collRc, void* charRc) noexcept
    {
        BoneRemap r = {};
        std::memset(r.collToChar, 0xFF, sizeof(r.collToChar));
        __try
        {
            auto* collBytes = reinterpret_cast<uint8_t*>(collRc);
            auto* charBytes = reinterpret_cast<uint8_t*>(charRc);

            void* collM2  = *reinterpret_cast<void**>(collBytes + offsets::kOffInstModel);
            void* charM2  = *reinterpret_cast<void**>(charBytes + offsets::kOffInstModel);
            if (!collM2 || !charM2) return r;
            r.collectionModel = collM2;
            r.characterModel  = charM2;

            // M2AnimData = raw M2 file buffer = M2FileHeader (at m2_inst+0x150)
            auto* collHdr = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(collM2) + offsets::kOffModelHeader);
            auto* charHdr = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(charM2) + offsets::kOffModelHeader);
            if (!collHdr || !charHdr) return r;

            uint32_t collN    = *reinterpret_cast<uint32_t*>(collHdr + offsets::kOffHdrBoneCount);
            uint8_t* collBase = *reinterpret_cast<uint8_t**>(collHdr + offsets::kOffHdrBoneArray);

            uint32_t charN    = *reinterpret_cast<uint32_t*>(charHdr + offsets::kOffHdrBoneCount);
            uint8_t* charBase = *reinterpret_cast<uint8_t**>(charHdr + offsets::kOffHdrBoneArray);

            uint32_t charLutN = *reinterpret_cast<uint32_t*>(charHdr + offsets::kOffHdrBoneIdxLutCount);
            int16_t* charLut  = *reinterpret_cast<int16_t**>(charHdr + offsets::kOffHdrBoneIdxLutPtr);

            EquipLog("  BuildBoneRemap: collN=%u charN=%u collBase=%p charBase=%p charLut=%p",
                     collN, charN, collBase, charBase, charLut);
//if (!collBase || collN == 0 || collN > 256 || charN == 0) return r;
r.count = static_cast<uint16_t>(collN);

            // Pass 1a: bone name CRC match for remaining unmatched bones
            if (charBase)
            {
                for (uint32_t i = 0; i < collN; ++i)
                {
                    if (r.collToChar[i] != 0xFF) continue;
                    uint32_t collCrc = *reinterpret_cast<uint32_t*>(collBase + i * offsets::kBoneStride + offsets::kOffBoneNameCrc);
                    if (collCrc == 0) continue;
                    for (uint32_t j = 0; j < charN && j < 255; ++j)
                    {
                        uint32_t charCrc = *reinterpret_cast<uint32_t*>(charBase + j * offsets::kBoneStride + offsets::kOffBoneNameCrc);
                        if (charCrc == collCrc) { r.collToChar[i] = static_cast<uint8_t>(j); break; }
                    }
                }
            }


            // Pass 1b: match by key_bone_id scan of char bone array
            if (charBase)
            {
                for (uint32_t i = 0; i < collN; ++i)
                {
                    int32_t key = *reinterpret_cast<int32_t*>(collBase + i * offsets::kBoneStride + offsets::kOffBoneKeyId);
                    if (key < 0) continue;
                    for (uint32_t j = 0; j < charN && j < 255; ++j)
                    {
                        if (*reinterpret_cast<int32_t*>(charBase + j * offsets::kBoneStride + offsets::kOffBoneKeyId) == key)
                        {
                            r.collToChar[i] = static_cast<uint8_t>(j);
                            break;
                        }
                    }
                }
            }

            // Pass 1c: BoneIndicesByID LUT for unmatched bones
            if (charLut)
            {
                for (uint32_t i = 0; i < collN; ++i)
                {
                    if (r.collToChar[i] != 0xFF) continue;
                    int32_t key = *reinterpret_cast<int32_t*>(collBase + i * offsets::kBoneStride + offsets::kOffBoneKeyId);
                    if (key < 0 || static_cast<uint32_t>(key) >= charLutN) continue;
                    int16_t ci = charLut[key];
                    if (ci >= 0 && static_cast<uint32_t>(ci) < charN && ci < 255)
                        r.collToChar[i] = static_cast<uint8_t>(ci);
                }
            }

            // Accessory chains in paper-doll collection models can be deeper than eight bones.
            for (uint32_t pass = 0; pass < collN; ++pass)
            {
                bool changed = false;
                for (uint32_t i = 0; i < collN; ++i)
                {
                    if (r.collToChar[i] != 0xFF) continue;
                    int16_t parent = *reinterpret_cast<int16_t*>(collBase + i * offsets::kBoneStride + offsets::kOffBoneParent);
                    if (parent < 0 || static_cast<uint32_t>(parent) >= collN) continue;
                    if (r.collToChar[parent] != 0xFF)
                    {
                        r.collToChar[i] = r.collToChar[parent];
                        changed = true;
                    }
                }
                if (!changed) break;
            }

            // If no bone was matched at all, signal "not ready" by resetting count to 0.
            // This happens when charBase is null (character M2 not yet parsed, e.g. initFlags=0x40).
            // count=0 causes all callers' retry guards to re-fire until the character is ready.
            {
                uint32_t matched = 0;
                for (uint32_t bi = 0; bi < r.count; ++bi)
                    if (r.collToChar[bi] != 0xFF) ++matched;
                if (matched == 0) r.count = 0;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return r;
    }


    // ─── Parse helpers ────────────────────────────────────────────────────────────

    // flags: 0x1=hide geoset, 0x2=no gender, 0x4=no race,
    //        0x8=append race to tex, 0x10=append gender to tex,
    //        0x20=model in subfolder, 0x40=new _Hu_F suffix,
    //        0x80=force model race/gender suffix

    static GeosetFilter ParseGeosetFilter(const char* spec)
    {
        GeosetFilter f = {};
        if (!spec || !*spec) return f;
        const char* p = spec;
        while (*p && f.count < 16)
        {
            uint32_t v = 0;
            while (*p >= '0' && *p <= '9') v = v * 10 + static_cast<uint32_t>(*p++ - '0');
            f.ids[f.count++] = static_cast<uint16_t>(v);
            if (*p == ',') p++;
            else break;
        }
        return f;
    }

    static bool CopyListPart(const char* list, uint32_t index, char* out, size_t outSz)
    {
        if (outSz == 0) return false;
        out[0] = '\0';
        if (!list) return false;

        const char* part = list;
        uint32_t cur = 0;
        for (;;)
        {
            const char* end = part;
            while (*end && *end != ';') ++end;

            if (cur == index)
            {
                while (part < end && (*part == ' ' || *part == '\t')) ++part;
                while (end > part && (end[-1] == ' ' || end[-1] == '\t')) --end;
                size_t len = static_cast<size_t>(end - part);
                if (len >= outSz) len = outSz - 1;
                if (len) std::memcpy(out, part, len);
                out[len] = '\0';
                return true;
            }

            if (!*end) break;
            part = end + 1;
            ++cur;
        }
        return false;
    }

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

    static bool StartsWithCI(const char* s, const char* prefix) noexcept;
    static bool ContainsCI(const char* s, const char* needle) noexcept;

    static uint32_t ParseModelColumn(const char* value) noexcept
    {
        if (!value || !*value) return static_cast<uint32_t>(-1);
        uint32_t numeric = 0;
        if (ParseU32(value, &numeric))
        {
            if (numeric == 0 || numeric == 1) return numeric;
            if (numeric == 2) return 1;
            return static_cast<uint32_t>(-1);
        }

        if (ContainsCI(value, "ModelName_1") || ContainsCI(value, "ModelTexture_1") ||
            ContainsCI(value, "Model_1") || ContainsCI(value, "Texture_1"))
            return 0;
        if (ContainsCI(value, "ModelName_2") || ContainsCI(value, "ModelTexture_2") ||
            ContainsCI(value, "Model_2") || ContainsCI(value, "Texture_2"))
            return 1;
        return static_cast<uint32_t>(-1);
    }

    static void NormalizedStemKey(const char* value, char* out, size_t outSz) noexcept
    {
        if (!out || outSz == 0) return;
        out[0] = '\0';
        if (!value) return;

        const char* base = value;
        for (const char* p = value; *p; ++p)
            if (*p == '\\' || *p == '/') base = p + 1;

        const char* end = base + std::strlen(base);
        for (const char* p = base; *p; ++p)
        {
            if (*p == ':' || *p == '.') { end = p; break; }
        }

        size_t n = 0;
        for (const char* p = base; p < end && n + 1 < outSz; ++p)
        {
            char c = *p;
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
                out[n++] = c;
        }
        out[n] = '\0';
    }

    static bool ModelStemMatches(const char* sidecarModel, const char* model) noexcept
    {
        if (!sidecarModel || !*sidecarModel) return true;
        char a[264], b[264];
        NormalizedStemKey(sidecarModel, a, sizeof(a));
        NormalizedStemKey(model, b, sizeof(b));
        return a[0] && b[0] && std::strcmp(a, b) == 0;
    }

    static uint32_t ParseSidecarSlot(const char* slot) noexcept
    {
        if (!slot || !*slot) return static_cast<uint32_t>(-1);
        uint32_t numeric = 0;
        if (ParseU32(slot, &numeric) && numeric < 14) return numeric;

        if (StartsWithCI(slot, "Head")) return 0;
        if (StartsWithCI(slot, "Shoulder")) return 1;
        if (StartsWithCI(slot, "Shirt")) return 2;
        if (StartsWithCI(slot, "Chest") || StartsWithCI(slot, "Robe")) return 3;
        if (StartsWithCI(slot, "Waist") || StartsWithCI(slot, "Belt")) return 4;
        if (StartsWithCI(slot, "Leg") || StartsWithCI(slot, "Pant")) return 5;
        if (StartsWithCI(slot, "Foot") || StartsWithCI(slot, "Feet") || StartsWithCI(slot, "Boot")) return 6;
        if (StartsWithCI(slot, "Bracer") || StartsWithCI(slot, "Wrist")) return 7;
        if (StartsWithCI(slot, "Glove") || StartsWithCI(slot, "Hand")) return 8;
        if (StartsWithCI(slot, "Cape") || StartsWithCI(slot, "Back") || StartsWithCI(slot, "Cloak")) return 9;
        if (StartsWithCI(slot, "Tabard")) return 10;
        // Weapon slots. Bare "Weapon" is intentionally left unmapped (returns -1, i.e.
        // "any slot") since a display ID is normally only ever equipped in one weapon
        // slot anyway; use the specific names below when a display can appear in more
        // than one hand (e.g. a shield display that could be Offhand vs. Ranged).
        if (StartsWithCI(slot, "MainHand") || StartsWithCI(slot, "Main")) return 15;
        if (StartsWithCI(slot, "OffHand") || StartsWithCI(slot, "Off") || StartsWithCI(slot, "Shield")) return 16;
        if (StartsWithCI(slot, "Ranged") || StartsWithCI(slot, "Bow") || StartsWithCI(slot, "Gun")
            || StartsWithCI(slot, "Quiver") || StartsWithCI(slot, "Ammo")) return 17;
        return static_cast<uint32_t>(-1);
    }

    static uint32_t ModelFlagsForSuffixPolicy(const char* policy) noexcept
    {
        if (policy && (StartsWithCI(policy, "DBC") || StartsWithCI(policy, "Slot"))) return 0xffffffffu;
        if (!policy || !*policy || StartsWithCI(policy, "Exact")) return 0x2 | 0x4;
        if (StartsWithCI(policy, "None")) return 0x2 | 0x4;
        if (StartsWithCI(policy, "Retail")) return 0x40;
        if (StartsWithCI(policy, "New")) return 0x40;
        if (StartsWithCI(policy, "Legacy")) return 0;
        if (StartsWithCI(policy, "Race")) return 0;
        return 0x2 | 0x4;
    }

    static uint32_t TextureFlagsForPolicy(const char* policy) noexcept
    {
        if (policy && (StartsWithCI(policy, "DBC") || StartsWithCI(policy, "Slot"))) return 0xffffffffu;
        if (!policy || !*policy || StartsWithCI(policy, "Exact")) return 0;
        if (StartsWithCI(policy, "None")) return 0;
        if (StartsWithCI(policy, "Retail")) return 0x8 | 0x10 | 0x40;
        if (StartsWithCI(policy, "New")) return 0x8 | 0x10 | 0x40;
        if (StartsWithCI(policy, "Legacy")) return 0x8 | 0x10;
        if (StartsWithCI(policy, "Race")) return 0x8 | 0x10;
        return 0;
    }

    static void ParseAttachField(const char* start, const char* end,
                                  uint32_t* leftOut, uint32_t* rightOut)
    {
        uint32_t a = 0;
        const char* p = start;
        while (p < end && *p >= '0' && *p <= '9') a = a * 10 + static_cast<uint32_t>(*p++ - '0');
        *leftOut = a;
        if (p < end && *p == '|')
        {
            p++;
            uint32_t b = 0;
            while (p < end && *p >= '0' && *p <= '9') b = b * 10 + static_cast<uint32_t>(*p++ - '0');
            *rightOut = b;
        }
        else { *rightOut = a; }
    }

    // Icon2 format: "<attach1>:<attach2>:<flags>:<customfolder>" — each field optional.
    // Caller sets attach/flag defaults; only fields present in the string override them.
    // Guard: only call when icon2str starts with ':' or a digit.
    static void ParseIcon2(const char* icon2str,
                            uint32_t* attachA_l, uint32_t* attachA_r,
                            uint32_t* attachB_l, uint32_t* attachB_r,
                            uint32_t* flags,
                            char* customFolder, size_t customFolderSz)
    {
        *flags = 0;
        if (customFolder) customFolder[0] = '\0';
        if (!icon2str || !*icon2str) return;

        const char* f2 = icon2str;
        while (*f2 && *f2 != ':') f2++;
        if (icon2str != f2) ParseAttachField(icon2str, f2, attachA_l, attachA_r);
        if (*f2 != ':') return;

        const char* f3 = f2 + 1;
        while (*f3 && *f3 != ':') f3++;
        if (f2 + 1 != f3) ParseAttachField(f2 + 1, f3, attachB_l, attachB_r);
        if (*f3 != ':') return;

        const char* f4 = f3 + 1;
        while (*f4 && *f4 != ':') f4++;
        if (f3 + 1 != f4)
        {
            uint32_t v = 0;
            for (const char* p = f3 + 1; p < f4; p++) v = v * 10 + static_cast<uint32_t>(*p - '0');
            *flags = v;
        }
        if (*f4 != ':') return;

        if (customFolder && customFolderSz > 1)
        {
            size_t len = std::strlen(f4 + 1);
            if (len >= customFolderSz) len = customFolderSz - 1;
            std::memcpy(customFolder, f4 + 1, len);
            customFolder[len] = '\0';
        }
    }

    static bool Icon2FieldHasValue(const char* icon2str, uint32_t fieldIndex)
    {
        if (!icon2str) return false;
        const char* start = icon2str;
        for (uint32_t i = 0; i < fieldIndex; ++i)
        {
            while (*start && *start != ':') ++start;
            if (*start != ':') return false;
            ++start;
        }
        const char* end = start;
        while (*end && *end != ':') ++end;
        return end > start;
    }

    static bool StartsWithCI(const char* s, const char* prefix) noexcept
    {
        if (!s || !prefix) return false;
        while (*prefix)
        {
            char a = *s++;
            char b = *prefix++;
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) return false;
        }
        return true;
    }

    static bool ContainsCI(const char* s, const char* needle) noexcept
    {
        if (!s || !needle || !*needle) return false;
        for (const char* p = s; *p; ++p)
        {
            const char* a = p;
            const char* b = needle;
            while (*a && *b)
            {
                char ca = *a++;
                char cb = *b++;
                if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
                if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
                if (ca != cb) break;
            }
            if (!*b) return true;
        }
        return false;
    }

    static bool IsKnownObjectComponentFolder(const char* folder, size_t len) noexcept
    {
        static const char* kFolders[] = {
            "Head", "Shoulder", "Shirt", "Chest", "Waist", "Leg", "Foot",
            "Bracer", "Glove", "Cape", "Tabard", "Collections", "Weapon",
        };

        for (const char* known : kFolders)
        {
            size_t knownLen = std::strlen(known);
            if (knownLen != len) continue;

            bool match = true;
            for (size_t i = 0; i < len; ++i)
            {
                char a = folder[i];
                char b = known[i];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
                if (a != b)
                {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        return false;
    }

    static const char* InferObjectComponentFolder(const char* name, bool isCollection,
                                                  const char* customFolder,
                                                  const char* slotFolder) noexcept
    {
        if (customFolder && *customFolder) return customFolder;
        if (isCollection) return "Collections";
        // Do NOT infer the folder from a "collections_" (or similar) filename prefix: the
        // sidecar CSV (WXLItemDisplayModels.csv) proves that prefix is just a naming
        // convention for the model's *set*, not its on-disk folder -- e.g. displayId=71052's
        // "collections_leather_raiddruiddragon_d_01_shoulder_l.mdx" carries the "collections_"
        // prefix but its authoritative Folder column says "Shoulder", not "Collections". The
        // sidecar path (addSidecarModels) already passes that column through as customFolder,
        // which is handled above; this fallback only ever applies to non-sidecar DBC-only rows,
        // where slotFolder (the slot's own default) is the correct answer, not a name guess.
        if (StartsWithCI(name, "lshoulder_") || StartsWithCI(name, "rshoulder_") ||
            StartsWithCI(name, "shoulder_") || ContainsCI(name, "_shoulder_l") ||
            ContainsCI(name, "_shoulder_r"))
            return "Shoulder";
        if (StartsWithCI(name, "cape_")) return "Cape";
        if (StartsWithCI(name, "tabard_")) return "Tabard";
        return slotFolder;
    }

    static uint32_t InferObjectComponentAttach(const char* name, bool isCollection,
                                               bool explicitAttach,
                                               uint32_t attach) noexcept
    {
        if (explicitAttach || isCollection) return attach;
        if (ContainsCI(name, "lshoulder_") || ContainsCI(name, "_shoulder_l")) return 6;
        if (ContainsCI(name, "rshoulder_") || ContainsCI(name, "_shoulder_r")) return 5;
        // Custom shoulder models often aren't named with a "_shoulder_l"/"_shoulder_r" infix at
        // all -- just a bare "_l"/"_r" side suffix right before the extension (e.g.
        // "leather_raiddruidt2_d_01_shoulder_l.mdx" already matches above via the infix check,
        // but simpler stems like "myitem_shoulder_left_r.mdx" or "custom_pad_l.mdx" only carry
        // the suffix). Only applied to names that look shoulder-related to avoid false positives
        // on unrelated "_l"/"_r"-suffixed models from other slots.
        if (ContainsCI(name, "shoulder"))
        {
            if (ContainsCI(name, "_l.") || ContainsCI(name, "_l_")) return 6;
            if (ContainsCI(name, "_r.") || ContainsCI(name, "_r_")) return 5;
        }
        if (ContainsCI(name, "collections_"))
        {
            if (ContainsCI(name, "_shoulder_l")) return 6;
            if (ContainsCI(name, "_shoulder_r")) return 5;
            if (ContainsCI(name, "collections_belt_") || ContainsCI(name, "_belt")) return 53;
        }
        if (ContainsCI(name, "cape_")) return 12;
        if (ContainsCI(name, "tabard_")) return 34;
        return attach;
    }

    static bool SlotAllowsNormalObjectModel(uint32_t modelSlot) noexcept
    {
        switch (modelSlot)
        {
            case 0:  // Head
            case 1:  // Shoulder
            case 4:  // Waist / 3D belts
            case 9:  // Back/cape
            case 10: // Tabard
            case 15: // MainHand
            case 16: // OffHand
            case 17: // Ranged
                return true;
            default:
                return false;
        }
    }

    static bool IsCollectionObjectPath(const char* path) noexcept
    {
        return ContainsCI(path, "objectcomponents\\collections\\") ||
               ContainsCI(path, "objectcomponents/collections/");
    }

    static bool IsCollectionGloveBodyModel(uint32_t modelSlot, const char* stem) noexcept
    {
        return modelSlot == 8 && stem && ContainsCI(stem, "_glove");
    }

    static bool NeedsVirtualModel(const AttachEntry& e) noexcept
    {
        return e.matTexBuf[0] ||
               e.geoFilter.count > 0 ||
               (e.texBuf[0] && IsCollectionObjectPath(e.keyBuf));
    }

    static bool IsCollectionEntry(const AttachEntry& e) noexcept
    {
        return e.geoFilter.count > 0 || IsCollectionObjectPath(e.keyBuf);
    }

    static bool IsRootSkinnedEntry(const AttachEntry& e) noexcept
    {
        // Retail ModelType=1 cape M2s are full character-space skinned models. The DB2
        // builder marks them with the synthetic root attachment even though their path is
        // under ObjectComponents\Cape rather than ObjectComponents\Collections.
        return e.attachId == kCollectionAttach &&
               (IsCollectionEntry(e) || e.equipSlot == 10);
    }

    static bool NeedsManualBoneRemap(const AttachEntry& e) noexcept
    {
        // Full-body collection overlays are attached to the character root and need their
        // palette driven from the character skeleton. Dedicated helm/shoulder/belt models use
        // native attachment points and must retain their own accessory-bone animation chains.
        return IsRootSkinnedEntry(e);
    }

    static bool ShouldDetachDefaultAttachPoints(const AttachEntry& e) noexcept
    {
        if (e.equipSlot >= 11) return false;
        if (e.equipSlot == 8 && e.attachId == kCollectionAttach) return false;
        return true;
    }

    static void AnalyzeModelList(const char* list, bool* hasCollection, bool* hasNormal)
    {
        char part[264] = {};
        for (uint32_t idx = 0; idx < 16; ++idx)
        {
            if (!CopyListPart(list, idx, part, sizeof(part))) break;
            if (!part[0]) continue;
            if (std::strchr(part, ':')) *hasCollection = true;
            else                        *hasNormal = true;
        }
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

    static void LoadSidecarFile(const char* path)
    {
        std::vector<std::string> lines;
        if (!ReadSidecarLines(path, lines)) return;

        const std::vector<std::string> header = ParseCsvLine(lines[0].c_str());
        const int cDisplay = FindCsvColumn(header, "DisplayID");
        const int cSlot = FindCsvColumn(header, "Slot");
        int cModel = FindCsvColumn(header, "Model");
        if (cModel < 0) cModel = FindCsvColumn(header, "ModelStem");
        if (cModel < 0) cModel = FindCsvColumn(header, "ModelName");
        int cTexture = FindCsvColumn(header, "Texture");
        if (cTexture < 0) cTexture = FindCsvColumn(header, "TextureStem");
        if (cTexture < 0) cTexture = FindCsvColumn(header, "ModelTexture");
        const int cFolder = FindCsvColumn(header, "Folder");
        const int cGeosets = FindCsvColumn(header, "Geosets");
        const int cAttach = FindCsvColumn(header, "Attach");
        const int cSuffixPolicy = FindCsvColumn(header, "SuffixPolicy");
        const int cTexturePolicy = FindCsvColumn(header, "TexturePolicy");
        const int cFlags = FindCsvColumn(header, "Flags");

        if (cDisplay < 0 || cModel < 0)
        {
            EquipLog("sidecar '%s': missing DisplayID or Model column", path);
            return;
        }

        uint32_t loaded = 0;
        for (size_t lineIndex = 1; lineIndex < lines.size(); ++lineIndex)
        {
            if (lines[lineIndex].empty()) continue;
            const std::vector<std::string> row = ParseCsvLine(lines[lineIndex].c_str());
            uint32_t displayId = 0;
            if (!ParseU32(CsvField(row, cDisplay), &displayId) || displayId == 0) continue;

            SidecarModelEntry e = {};
            e.modelSlot = ParseSidecarSlot(CsvField(row, cSlot));
            e.modelFlags = ModelFlagsForSuffixPolicy(CsvField(row, cSuffixPolicy));
            e.textureFlags = TextureFlagsForPolicy(CsvField(row, cTexturePolicy));

            uint32_t attach = static_cast<uint32_t>(-1);
            if (ParseU32(CsvField(row, cAttach), &attach)) e.attachId = attach;

            uint32_t extraFlags = 0;
            if (ParseU32(CsvField(row, cFlags), &extraFlags)) e.modelFlags |= extraFlags;

            CopyString(e.folder, sizeof(e.folder), CsvField(row, cFolder));
            CopyString(e.model, sizeof(e.model), CsvField(row, cModel));
            CopyString(e.texture, sizeof(e.texture), CsvField(row, cTexture));

            char* colon = std::strchr(e.model, ':');
            if (colon)
            {
                *colon = '\0';
                e.geoFilter = ParseGeosetFilter(colon + 1);
            }
            const char* geosets = CsvField(row, cGeosets);
            if (geosets && *geosets) e.geoFilter = ParseGeosetFilter(geosets);
            if (!e.model[0]) continue;

            g_sidecarModels[displayId].push_back(e);
            ++loaded;
        }

        if (loaded)
            EquipLog("sidecar loaded '%s' rows=%u", path, loaded);
    }

    static void LoadMaterialSidecarFile(const char* path)
    {
        std::vector<std::string> lines;
        if (!ReadSidecarLines(path, lines)) return;

        const std::vector<std::string> header = ParseCsvLine(lines[0].c_str());
        const int cDisplay = FindCsvColumn(header, "DisplayID");
        const int cModelIndex = FindCsvColumn(header, "ModelIndex");
        const int cModelColumn = FindCsvColumn(header, "ModelColumn");
        const int cModel = FindCsvColumn(header, "Model");
        const int cTextureType = FindCsvColumn(header, "TextureType");
        const int cFolder = FindCsvColumn(header, "Folder");
        const int cTexture = FindCsvColumn(header, "Texture");
        const int cTargetMode = FindCsvColumn(header, "TargetMode");
        // NOTE: Layer, SkinSectionIDs, BatchIndexes, TargetSkinSectionIDs, and TargetBatchIndexes
        // are intentionally not looked up here -- texture-value patching is TextureType-keyed only
        // (see PatchTargetedMaterialTextures in VirtualPath.cpp), and the hide-by-batch machinery
        // those columns used to feed has been removed along with them. MaterialResourcesID and
        // FileDataID were never read by this loader either; all seven of those columns can be
        // dropped from the CSV entirely.

        if (cDisplay < 0 || cTexture < 0)
        {
            EquipLog("material sidecar '%s': missing DisplayID or Texture column", path);
            return;
        }

        uint32_t loaded = 0;
        for (size_t lineIndex = 1; lineIndex < lines.size(); ++lineIndex)
        {
            if (lines[lineIndex].empty()) continue;
            const std::vector<std::string> row = ParseCsvLine(lines[lineIndex].c_str());
            uint32_t displayId = 0;
            if (!ParseU32(CsvField(row, cDisplay), &displayId) || displayId == 0) continue;

            SidecarMaterialEntry e = {};
            ParseU32(CsvField(row, cModelIndex), &e.modelIndex);
            e.modelColumn = ParseModelColumn(CsvField(row, cModelColumn));
            if (e.modelColumn == static_cast<uint32_t>(-1))
                e.modelColumn = ParseModelColumn(CsvField(row, cModelIndex));
            if (cTextureType >= 0)
                ParseU32(CsvField(row, cTextureType), &e.textureType);

            CopyString(e.folder, sizeof(e.folder), CsvField(row, cFolder));
            CopyString(e.model, sizeof(e.model), CsvField(row, cModel));
            CopyString(e.texture, sizeof(e.texture), CsvField(row, cTexture));
            CopyString(e.targetMode, sizeof(e.targetMode), CsvField(row, cTargetMode));
            if (!e.texture[0]) continue;

            g_sidecarMaterials[displayId].push_back(e);
            ++loaded;
        }

        if (loaded)
            EquipLog("material sidecar loaded '%s' rows=%u", path, loaded);
    }

    // WXLItemEntryDisplay.csv / LoadItemDisplaySidecarFile (ItemEntry -> DisplayID -> Folder,
    // feeding g_sidecarItemDisplay / g_sidecarModelFolder) used to load here, solely for the old
    // weapon model-swap path's LookupItemDisplayId/PatchWeaponModelByDisplayId lookups. Both are
    // gone -- WeaponExtension.cpp's own WXLWeaponModels.csv/WXLWeaponTextures.csv sidecars replace
    // this entirely for weapons, keyed directly by DisplayID with no itemEntry indirection needed.
    static void LoadSidecarModels()
    {
        if (g_sidecarLoaded) return;
        g_sidecarLoaded = true;

        LoadSidecarFile("WXLItemDisplayModels.csv");
        LoadSidecarFile("DBFilesClient\\WXLItemDisplayModels.csv");
        LoadMaterialSidecarFile("WXLItemDisplayModelMaterials.csv");
        LoadMaterialSidecarFile("DBFilesClient\\WXLItemDisplayModelMaterials.csv");

        WIN32_FIND_DATAA fd = {};
        HANDLE h = FindFirstFileA("Data\\*.MPQ", &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                std::string path = "Data\\";
                path += fd.cFileName;
                path += "\\DBFilesClient\\WXLItemDisplayModels.csv";
                LoadSidecarFile(path.c_str());
                path = "Data\\";
                path += fd.cFileName;
                path += "\\DBFilesClient\\WXLItemDisplayModelMaterials.csv";
                LoadMaterialSidecarFile(path.c_str());
            }
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }

        EquipLog("sidecar table ready: displays=%zu materialDisplays=%zu",
                 g_sidecarModels.size(), g_sidecarMaterials.size());

        // Weapon eager preload (formerly this function's [EagerPreload] Weapons check calling
        // PreregisterSidecarWeapons) has moved to WeaponExtension.cpp's own LoadWeaponSidecar,
        // which reads the same ini key independently.
    }

    // ─── Path builders ────────────────────────────────────────────────────────────

    // Builds the virtual M2 path: "Item\ObjectComponents\<folder>\[<stem>\]<stem>_<race>[_]<gender>.mdx"
    // Folder priority: customFolder > collection/cape stem inference > slotFolder.
    // flags: 0x2=no gender, 0x4=no race, 0x20=subfolder, 0x40=new _Hu_F underscore between race+gender.
    static void BuildSlotPath(char* buf, const char* modelName,
                               const char* raceCode, const char* genderStr,
                               uint32_t flags, const char* slotFolder, bool isCollection,
                               const char* customFolder = nullptr)
    {
        bool appendRace   = !(flags & 0x4);
        bool appendGender = !(flags & 0x2);
        bool subFolder    = (flags & 0x20) != 0;
        bool newSuffix    = (flags & 0x40) != 0;

        const char* folder = InferObjectComponentFolder(modelName, isCollection, customFolder, slotFolder);

        char* p = buf;
        for (const char* s = "Item\\ObjectComponents\\"; *s; ) *p++ = *s++;
        for (const char* s = folder; *s; )                     *p++ = *s++;
        *p++ = '\\';

        const char* lastDot = nullptr;
        for (const char* s = modelName; *s; s++) if (*s == '.') lastDot = s;
        size_t stemLen = lastDot ? static_cast<size_t>(lastDot - modelName) : std::strlen(modelName);

        if (subFolder)
        {
            for (size_t i = 0; i < stemLen; ++i) *p++ = modelName[i];
            *p++ = '\\';
        }
        for (size_t i = 0; i < stemLen; ++i) *p++ = modelName[i];

        if (!appendRace && !appendGender)
        {
            *p++ = '.'; *p++ = 'm'; *p++ = 'd'; *p++ = 'x'; *p = '\0';
            return;
        }

        *p++ = '_';
        if (appendRace)   for (const char* s = raceCode;  *s; ) *p++ = *s++;
        if (newSuffix && appendRace && appendGender) *p++ = '_';
        if (appendGender) for (const char* s = genderStr; *s; ) *p++ = *s++;
        *p++ = '.'; *p++ = 'm'; *p++ = 'd'; *p++ = 'x'; *p = '\0';
    }

    // Builds the virtual BLP texture path.
    // flags: 0x8=append race, 0x10=append gender, 0x20=subfolder, 0x40=new underscore.
    // modelStem: when 0x20 set, names the subfolder after the model stem (not the texture name).
    static void BuildTexPath(char* buf, size_t bufSz, const char* texName,
                              const char* raceCode, const char* genderStr,
                              uint32_t flags, const char* slotFolder, bool isCollection,
                              const char* customFolder = nullptr, const char* modelStem = nullptr)
    {
        bool appendRace   = (flags & 0x8) != 0;
        bool appendGender = (flags & 0x10) != 0;
        bool subFolder    = (flags & 0x20) != 0;
        bool newSuffix    = (flags & 0x40) != 0;

        char explicitFolder[32] = {};
        const char* texStem = texName;
        for (const char* s = texName; *s; ++s)
        {
            if (*s != '\\' && *s != '/') continue;
            size_t folderLen = static_cast<size_t>(s - texName);
            if (folderLen > 0 && folderLen < sizeof(explicitFolder)
                && IsKnownObjectComponentFolder(texName, folderLen))
            {
                std::memcpy(explicitFolder, texName, folderLen);
                explicitFolder[folderLen] = '\0';
                texStem = s + 1;
            }
            break;
        }

        const char* subBase = (modelStem && *modelStem) ? modelStem : texStem;
        const char* folder = InferObjectComponentFolder(subBase, isCollection, customFolder, slotFolder);
        if (explicitFolder[0]) folder = explicitFolder;
        size_t subBaseLen = std::strlen(subBase);
        for (const char* s = subBase; *s; s++)
            if (*s == '.') { subBaseLen = static_cast<size_t>(s - subBase); break; }

        char tmp[264];
        char* p = tmp;
        for (const char* s = "Item\\ObjectComponents\\"; *s; ) *p++ = *s++;
        for (const char* s = folder; *s; )                     *p++ = *s++;
        *p++ = '\\';
        if (subFolder)
        {
            for (size_t i = 0; i < subBaseLen; ++i) *p++ = subBase[i];
            *p++ = '\\';
        }
        for (const char* s = texStem; *s; ) *p++ = *s++;
        if (appendRace || appendGender)
        {
            *p++ = '_';
            if (appendRace)   for (const char* s = raceCode;  *s; ) *p++ = *s++;
            if (newSuffix && appendRace && appendGender) *p++ = '_';
            if (appendGender) for (const char* s = genderStr; *s; ) *p++ = *s++;
        }
        *p++ = '.'; *p++ = 'b'; *p++ = 'l'; *p++ = 'p'; *p = '\0';
        std::strncpy(buf, tmp, bufSz - 1);
        buf[bufSz - 1] = '\0';
    }

    static bool MaterialEntryMatches(const SidecarMaterialEntry& e,
                                     uint32_t modelColumn,
                                     uint32_t partIndex,
                                     const char* modelName) noexcept
    {
        const bool hasModelName = e.model[0] != '\0';
        if (hasModelName && !ModelStemMatches(e.model, modelName)) return false;
        if (!hasModelName &&
            e.modelColumn != static_cast<uint32_t>(-1) &&
            e.modelColumn != modelColumn)
            return false;
        if (hasModelName &&
            modelColumn != static_cast<uint32_t>(-1) &&
            e.modelColumn != static_cast<uint32_t>(-1) &&
            e.modelColumn != modelColumn)
            return false;

        if (!hasModelName && e.modelColumn == static_cast<uint32_t>(-1) &&
            e.modelIndex != static_cast<uint32_t>(-1))
        {
            const bool columnKnown = modelColumn != static_cast<uint32_t>(-1);
            const bool columnMatch = columnKnown &&
                (e.modelIndex == modelColumn || e.modelIndex == modelColumn + 1);
            const bool partMatch = e.modelIndex == partIndex || e.modelIndex == partIndex + 1;
            if (!columnMatch && !partMatch)
                return false;
        }

        return e.texture[0] != '\0';
    }

    // Armor-side texture-value-baking loop. Bakes each matching sidecar material row's Texture
    // into every texture-unit record on the model declaring that row's TextureType, via
    // PatchTargetedMaterialTextures's TextureType-only matching in VirtualPath.cpp. The CSV's
    // Layer, SkinSectionIDs, BatchIndexes, TargetSkinSectionIDs, and TargetBatchIndexes columns are
    // no longer read at all (see LoadMaterialSidecarFile / SidecarMaterialEntry) -- there is no
    // hide/edgefade/targeted-batch overlay any more. A row with TargetMode=Hide (or a
    // __hide__/hide texture value) is simply skipped, since there is no longer any data source to
    // say which batches such a row would apply to. Weapons no longer use this table at all --
    // WeaponExtension.cpp's own material patch spec builder replaced the old weapon fallback
    // counterpart that used to sit here (BuildMaterialPatchSpecWeaponFallback, removed).
    static void BuildMaterialPatchSpecArmor(char* out, size_t outSz,
                                            uint32_t displayId,
                                            uint32_t modelColumn,
                                            uint32_t partIndex,
                                            const char* modelName,
                                            const char* raceCode,
                                            const char* genderStr,
                                            const char* slotFolder,
                                            bool isCollection,
                                            const char* customFolder,
                                            const char* modelStem)
    {
        if (!out || outSz == 0) return;
        out[0] = '\0';

        auto it = g_sidecarMaterials.find(displayId);
        if (it == g_sidecarMaterials.end()) return;

        size_t used = 0;
        for (const SidecarMaterialEntry& m : it->second)
        {
            if (!MaterialEntryMatches(m, modelColumn, partIndex, modelName)) continue;
            if (m.textureType == static_cast<uint32_t>(-1)) continue; // no type, nothing to key off
            if (!m.texture[0]) continue;
            if (ContainsCI(m.targetMode, "Hide") ||
                StartsWithCI(m.texture, "__hide__") || StartsWithCI(m.texture, "hide"))
                continue; // hide is no longer supported; skip rather than emit a dead entry

            char texPath[264] = {};
            const char* folder = m.folder[0] ? m.folder : customFolder;
            BuildTexPath(texPath, sizeof(texPath), m.texture, raceCode, genderStr, 0,
                         slotFolder, isCollection, folder, modelStem);
            if (!texPath[0]) continue;

            char item[768];
            int n = std::snprintf(item, sizeof(item), "%s%u=%s",
                                  used ? "|" : "", m.textureType, texPath);
            if (n <= 0) continue;
            const size_t len = static_cast<size_t>(n);
            if (len >= sizeof(item) || used + len >= outSz) break;
            std::memcpy(out + used, item, len);
            used += len;
            out[used] = '\0';
        }
    }


    // ─── Geoset filter ────────────────────────────────────────────────────────────

    // Zeros rawTri (skin->indices) for submeshes whose skinSectionId is not in the filter.
    // Must run before kFinalizeSkin builds the D3D index buffer — the GPU IB is static after
    // that point. Called from OnM2SkinFinalize (the primary path) and Phase3 (fallback for
    // the rare sync-load case where the model is already available at equip time).
    static void ApplyRawTriFilter(gm2::M2SkinProfile* skin, const GeosetFilter& filter) noexcept
    {
        if (!skin || filter.count == 0) return;
        __try
        {
            if (!skin->indices || !skin->submeshes || skin->submeshCount == 0) return;
            EquipLog("  ApplyRawTriFilter: submeshCount=%u indexCount=%u filter=[%u %u %u %u](n=%u)",
                     skin->submeshCount, skin->indexCount,
                     filter.count > 0 ? filter.ids[0] : 0,
                     filter.count > 1 ? filter.ids[1] : 0,
                     filter.count > 2 ? filter.ids[2] : 0,
                     filter.count > 3 ? filter.ids[3] : 0,
                     filter.count);
            for (uint32_t si = 0; si < skin->submeshCount; ++si)
            {
                uint16_t secId = skin->submeshes[si].skinSectionId;
                uint16_t lvl   = skin->submeshes[si].level;
                uint16_t start16 = skin->submeshes[si].indexStart;
                uint16_t count16 = skin->submeshes[si].indexCount;
                bool visible = false;
                for (uint32_t fi = 0; fi < filter.count; ++fi)
                    if (filter.ids[fi] == secId) { visible = true; break; }
                EquipLog("    submesh[%u]: skinSectionId=%u level=%u indexStart=%u indexCount=%u -> %s",
                         si, (uint32_t)secId, (uint32_t)lvl, (uint32_t)start16, (uint32_t)count16,
                         visible ? "KEEP" : "ZERO");
                if (!visible)
                {
                    uint32_t count = count16;
                    if (count == 0) continue;

                    uint32_t start = (static_cast<uint32_t>(lvl) << 16) | start16;
                    if (start > skin->indexCount || count > skin->indexCount - start)
                    {
                        start = start16;
                        if (start > skin->indexCount || count > skin->indexCount - start) continue;
                    }

                    std::memset(skin->indices + start, 0, count * sizeof(uint16_t));
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ─── Detach / rebuild ─────────────────────────────────────────────────────────

    // Erases all entries for (cmo, equipSlot) and issues the minimal set of DetachSlot calls
    // (deduplicating by attach_id to avoid double-detach corrupting the scene-graph linked list).
    static void DetachSlotEntries(void* cmo, uint32_t equipSlot)
    {
        auto it = g_attached.find(cmo);
        if (it == g_attached.end()) return;

        // Current live sub_obj (read fresh; may differ from stored sub_obj after re-login).
        void* curSubObj = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + offsets::kOffCmoSceneNode);

        struct DetachPair { void* subObj; uint32_t attachId; };
        DetachPair pairs[8]; uint32_t nPairs = 0;
        auto addPair = [&](void* so, uint32_t aid)
        {
            if (!so || aid == static_cast<uint32_t>(-1) || nPairs >= 8) return;
            for (uint32_t k = 0; k < nPairs; ++k)
                if (pairs[k].subObj == so && pairs[k].attachId == aid) return;
            pairs[nPairs++] = { so, aid };
        };

        auto& vec = it->second;
        for (size_t i = 0; i < vec.size(); )
        {
            AttachEntry& e = vec[i];
            if (e.equipSlot != equipSlot) { ++i; continue; }
            if (e.renderCtx)
                addPair(curSubObj ? curSubObj : e.subObj, e.attachId);
            vec.erase(vec.begin() + static_cast<ptrdiff_t>(i));
        }

        for (uint32_t k = 0; k < nPairs; ++k)
            gm2::DetachSlot(pairs[k].subObj, pairs[k].attachId);

        if (vec.empty()) g_attached.erase(it);
    }

    // Detaches all unique attach_ids in g_attached[cmo], then re-attaches each unique
    // (keyBuf, attachId, texBuf) combination via GetRenderCtx → (BindTexSlot) → AttachToScene.
    // render_ctx values in entries are rebuilt transient each time; called on every equip/unequip.
    static void RebuildAllModels(void* cmo)
    {
        auto it = g_attached.find(cmo);
        if (it == g_attached.end() || it->second.empty()) return;
        auto& entries = it->second;

        void* subObj = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + offsets::kOffCmoSceneNode);
        if (!subObj) { EquipLog("  RebuildAllModels: subObj null, bail"); return; }

        // GetRenderCtx (sub_81f8f0) must be called with the scene-node's owner (owner28), NOT
        // with our slot-dispatch 'cmo'. The slot-dispatch ECX is a wrapper object whose +4
        // hash-table pointer is always null. The real CMO — the object that owns the M2 hash
        // table — is the scene node's back-pointer at kOffSceneNodeOwner (+0x28). This matches
        // how vanilla's sub_4eaa70 calls sub_81f8f0: it dereferences *(sub_obj+0x28) first.
        // If owner28 is null the CMO is not yet fully initialised; bail so OnM2PerFrameUpdate
        // can retry via the pending-attach sweep.
        void* owner28 = GuardedReadPtr(reinterpret_cast<uint8_t*>(subObj) + offsets::kOffSceneNodeOwner);
        if (!owner28) { EquipLog("  RebuildAllModels: owner28 null, bail (deferred)"); return; }

        EquipLog("  RebuildAllModels: %zu entries, subObj=0x%p owner28=0x%p",
                 entries.size(), subObj, owner28);

 for (auto& e : entries) { e.renderCtx = nullptr; e.subObj = subObj; e.perFrameLogged = false; }
//        // Reset all state tied to the previous render context.
//        for (auto& e : entries)
//        {
//            e.renderCtx = nullptr;
//            e.subObj = subObj;
//            e.boneRemap = {};
//            e.perFrameLogged = false;
//            e.charSweepApplied = false;
//            e.bbpLogDone = false;
//            e.cloneBbpLogDone = false;
//        }
        // Pre-phase: build virtual paths for collection entries and populate the serve table.
        // Each unique (keyBuf, attachId, texBuf) collection group gets one merged geoset filter and one
        // virtual key encoding (cmo × model × geosets × texture). GetRenderCtx with this key
        // guarantees a cache miss on every distinct filter/texture combination, forcing a fresh
        // async load and an OnM2SkinFinalize fire with the correct merged filter.
        for (size_t i = 0; i < entries.size(); ++i)
        {
            AttachEntry& e = entries[i];
            if (!NeedsVirtualModel(e)) continue;
            // Reset every pass: AttachEntry persists across RebuildAllModels calls (it's mutated in
            // place in g_attached[cmo], not reallocated), so a stale true here from a previous failed
            // attempt would permanently skip this slot even after the underlying file/data problem is
            // fixed and a later populate call succeeds. Only this pass's result should count. Done as
            // its own pass (not inline below) because the group-dedup skip further down means a later
            // sibling's turn in the main loop can run after the group owner already set this true for
            // it -- resetting inline there would silently undo that.
            e.virtualPopulateFailed = false;
        }
        for (size_t i = 0; i < entries.size(); ++i)
        {
            AttachEntry& e = entries[i];
            if (!NeedsVirtualModel(e)) continue;
            // Skip if a sibling already processed this (keyBuf, attachId, texBuf) group.
            bool done = false;
            for (size_t j = 0; j < i; ++j)
            {
                AttachEntry& prev = entries[j];
                if (NeedsVirtualModel(prev) && SameAttachModelGroup(prev, e)) { done = true; break; }
            }
            if (done) continue;

            // Merge geoset IDs within this (keyBuf, attachId, texBuf) group.
            // texBuf is fixed per group — e.texBuf is the canonical texture for this virtual model.
            uint16_t mergedIds[16]; uint32_t mergedCount = 0;
            for (auto& e2 : entries)
            {
                if (!SameAttachModelGroup(e2, e)) continue;
                for (uint32_t fi = 0; fi < e2.geoFilter.count && mergedCount < 16; ++fi)
                {
                    bool dup = false;
                    for (uint32_t fj = 0; fj < mergedCount; ++fj)
                        if (mergedIds[fj] == e2.geoFilter.ids[fi]) { dup = true; break; }
                    if (!dup) mergedIds[mergedCount++] = e2.geoFilter.ids[fi];
                }
            }

            // Build the virtual key and ensure bytes are in the serve table.
            char mangled[264];
            const size_t mangledLen =
                VPathBuildKey(mangled, sizeof(mangled), cmo, e.keyBuf, mergedIds, mergedCount,
                              e.texBuf, e.mergeKey, e.matTexBuf);
            const bool vpathReady = mangledLen != 0 &&
                VPathPopulate(cmo, e.keyBuf, mergedIds, mergedCount, e.texBuf, e.mergeKey, e.matTexBuf);
            EquipLog("  VPath: '%s' tex='%s' mat='%s' -> '%s' (merged=%u mergeKey=0x%X)",
                     e.keyBuf, e.texBuf, e.matTexBuf, vpathReady ? mangled : "(disabled)",
                     mergedCount, e.mergeKey);
            if (!vpathReady)
            {
                // Real backing file could not be read (see the VPathPopulate log line just above).
                // Mark this whole (keyBuf, attachId, texBuf) group so Phase1 skips it outright instead
                // of silently retrying the same already-proven-bad raw path.
                for (auto& e2 : entries)
                    if (NeedsVirtualModel(e2) && SameAttachModelGroup(e2, e))
                        e2.virtualPopulateFailed = true;
                continue;
            }

            // Propagate the mangled key to all entries in this (keyBuf, attachId, texBuf) group.
            for (auto& e2 : entries)
            {
                if (NeedsVirtualModel(e2) && SameAttachModelGroup(e2, e))
                    std::memcpy(e2.mangledKeyBuf, mangled, sizeof(e2.mangledKeyBuf));
            }
        }

        // Phase 1 — GetRenderCtx before DetachSlot.
        // hkSlotDispatch (GameHooks.cpp) runs vanilla first: it calls GetRenderCtx + AttachToScene
        // + ReleaseRenderCtx, leaving refcount=1 held by the scene. If we called DetachSlot first
        // the scene would release its ref (refcount→0, node freed), leaving a dangling pointer in
        // the hash bucket. Subsequent GetRenderCtx reads that dangling value and crashes at
        // sub_81c390+0x170 (mov eax,[esi+0x14c]) where esi=0x206 (freed-heap garbage).
        // Getting our ref first keeps the node alive through the detach step.
        //
        // g_currentRebuildCmo enables OnM2SkinFinalize to apply the geoset filter during the
        // synchronous load that fires kFinalizeSkin inside GetRenderCtx for virtual-path entries
        // (in-process byte serving has no IPC delay, so the model loads and finalizes before
        // GetRenderCtx returns — before entry.renderCtx is set). Cleared after Phase1.
        g_currentRebuildCmo = cmo;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            AttachEntry& e = entries[i];
            if (e.renderCtx) continue;  // already set by a sibling
            if (e.virtualPopulateFailed)
            {
                // The real backing file for this entry was already proven unreadable when the
                // virtual-path population pass ran above -- do not retry via e.keyBuf. The native
                // GetRenderCtx can return a non-null placeholder/error-model context for a path it
                // can't actually load, and that placeholder's internal layout does not match what
                // BuildBoneRemap/the geoset filter downstream assumes for this slot, which is what
                // was crashing here. Skip the attach entirely instead: no model for this slot this
                // frame is a visible miss, not a memory-safety violation.
                EquipLog("  Phase1 SKIP (virtual populate failed) key='%s'", e.keyBuf);
                continue;
            }

            const char* p1Key = e.mangledKeyBuf[0] ? e.mangledKeyBuf : e.keyBuf;
            void* rctx = SafeGetRenderCtx(owner28, p1Key);
            EquipLog("  Phase1 GetRenderCtx key='%s' -> 0x%p", p1Key, rctx);
            if (!rctx) continue;

            // Propagate to all siblings sharing (keyBuf, attachId, texBuf).
            for (size_t j = i; j < entries.size(); ++j)
            {
                AttachEntry& e2 = entries[j];
                if (!e2.renderCtx && SameAttachModelGroup(e2, e))
                    e2.renderCtx = rctx;
            }
        }
        g_currentRebuildCmo = nullptr;

        // Phase 2 — DetachSlot all unique attach_ids (dedup avoids double-detach crash in sub_827560).
        // Also detach the slot config's vanilla default attach points: g_origSlotDispatch (which runs
        // before our event) calls sub_4eaa70 which attaches to defAttach1/defAttach2. Without removing
        // these, the vanilla model shows at its default position alongside our custom attachments.
        uint32_t detached[22] = {}; uint32_t nDetached = 0;
        auto detachOnce = [&](uint32_t aid) {
            if (aid == static_cast<uint32_t>(-1) || nDetached >= 22) return;
            for (uint32_t i = 0; i < nDetached; ++i) if (detached[i] == aid) return;
            EquipLog("  Phase2 DetachSlot attachId=%u subObj=%p", aid, subObj);
            SafeDetachSlot(subObj, aid);
            EquipLog("  Phase2 DetachSlot attachId=%u done", aid);
            detached[nDetached++] = aid;
        };
        for (auto& e : entries)
        {
            detachOnce(e.attachId);
            if (ShouldDetachDefaultAttachPoints(e))
            {
                detachOnce(kSlotConfig[e.equipSlot].defAttach1);
                detachOnce(kSlotConfig[e.equipSlot].defAttach2);
            }
        }

        // Phase 3 — Bind texture, AttachToScene, geoset filter, release ref.
        // Process each unique (keyBuf, attachId, texBuf) once; siblings share the same render_ctx.
        for (size_t i = 0; i < entries.size(); ++i)
        {
            AttachEntry& e = entries[i];
            if (!e.renderCtx)
            {
                // renderCtx is null for two reasons:
                //   (a) Collection entry evicted by Phase 2.5 (model was loaded): re-acquire fresh rctx.
                //   (b) Non-collection entry whose Phase1 GetRenderCtx failed: skip.
                // Loading collection entries are NOT nulled by Phase 2.5; they reach here with a
                // non-null renderCtx (Phase1's rctx) and fall through to the attach below.
                if (!NeedsVirtualModel(e)) continue;
                const char* p3Key = e.mangledKeyBuf[0] ? e.mangledKeyBuf : e.keyBuf;
                void* rctx = SafeGetRenderCtx(owner28, p3Key);
                EquipLog("  Phase3[%zu] collection re-acquire key='%s' -> 0x%p", i, p3Key, rctx);
                if (!rctx) continue;
                e.renderCtx = rctx;
                for (size_t j = i + 1; j < entries.size(); ++j)
                {
                    AttachEntry& e2 = entries[j];
                    if (!e2.renderCtx && NeedsVirtualModel(e2) && SameAttachModelGroup(e2, e))
                        e2.renderCtx = rctx;
                }
            }

            // Skip siblings — only the first occurrence of each (keyBuf, attachId, texBuf) does the attach.
            bool alreadyAttached = false;
            for (size_t j = 0; j < i; ++j)
            {
                AttachEntry& prev = entries[j];
                if (prev.renderCtx && SameAttachModelGroup(prev, e))
                { alreadyAttached = true; break; }
            }
            if (alreadyAttached) continue;

            void* rctx = e.renderCtx;
            bool isCollection = IsCollectionEntry(e);

            if (e.texBuf[0])
            {
                EquipLog("  Phase3[%zu] LoadResource tex='%s'", i, e.texBuf);
                void* tex = SafeLoadResource(e.texBuf, 0);
                EquipLog("  Phase3[%zu] LoadResource -> 0x%p", i, tex);
                if (!tex)
                {
                    EquipLog("  Phase3[%zu] texture load failed '%s', skip attach", i, e.texBuf);
                    for (auto& e2 : entries)
                        if (SameAttachModelGroup(e2, e)) e2.renderCtx = nullptr;
                    gm2::ReleaseRenderCtx(rctx);
                    continue;
                }
                else
                {
                    EquipLog("  Phase3[%zu] BindTexSlot rctx=0x%p tex=0x%p", i, rctx, tex);
                    SafeBindTexSlot(rctx, tex);
                    EquipLog("  Phase3[%zu] BindTexSlot done", i);
                    // Do not release here. TextureCreate returns a handle that the render context
                    // continues to use after BindTexSlot; releasing immediately can recycle it and
                    // leave the attached M2 sampling the client's missing-texture green.
                }
            }

            // subObj->initFlags & 1: when set (character fully initialised), sub_831630 checks the
            // BoneIndicesByID LUT; if the attach point is absent and zero2==0 it exits silently.
            // Passing forceAttach=true (zero2=1) bypasses the early-exit for non-standard points
            // (e.g. attach_id=19) that collection M2s use.
            uint32_t subInitFlags = GuardedReadU32(reinterpret_cast<uint8_t*>(subObj) + offsets::kOffInstInitFlags);
            EquipLog("  Phase3[%zu] attach=%u rctx=0x%p subObj_initFlags=0x%X isCollection=%d",
                     i, e.attachId, rctx, subInitFlags, (int)isCollection);
            gm2::AttachToScene(rctx, subObj, e.attachId, isCollection);

//            void* nativeParent = NativeParentGuarded(rctx);
            BoneRemap remap = BuildBoneRemapGuarded(rctx, subObj);
//            EquipLog("  Phase3[%zu] boneRemap.count=%u requestedParent=0x%p nativeParent=0x%p",
//                     i, (uint32_t)remap.count, subObj, nativeParent);
            EquipLog("  Phase3[%zu] boneRemap.count=%u", i, (uint32_t)remap.count);
            // Propagate boneRemap to siblings sharing (keyBuf, attachId, texBuf).
            for (auto& e2 : entries)
            {
                if (!SameAttachModelGroup(e2, e)) continue;
                if (remap.count > 0) e2.boneRemap = remap;
            }

            // Apply merged geoset filter for entries sharing this (keyBuf, attachId, texBuf).
            if (isCollection)
            {
                GeosetFilter merged = {};
                for (auto& e2 : entries)
                {
                    if (!SameAttachModelGroup(e2, e)) continue;
                    for (uint32_t fi = 0; fi < e2.geoFilter.count && merged.count < 16; ++fi)
                    {
                        bool dup = false;
                        for (uint32_t fj = 0; fj < merged.count; ++fj)
                            if (merged.ids[fj] == e2.geoFilter.ids[fi]) { dup = true; break; }
                        if (!dup) merged.ids[merged.count++] = e2.geoFilter.ids[fi];
                    }
                }
                EquipLog("  Phase3[%zu] geoFilter merged.count=%u ids=[%u %u %u %u]",
                         i, merged.count,
                         merged.count > 0 ? merged.ids[0] : 0,
                         merged.count > 1 ? merged.ids[1] : 0,
                         merged.count > 2 ? merged.ids[2] : 0,
                         merged.count > 3 ? merged.ids[3] : 0);
                void* mdl = GuardedReadPtr(reinterpret_cast<uint8_t*>(rctx) + offsets::kOffInstModel);
                auto* skin = mdl ? gm2::M2Model(mdl).GetSkin() : nullptr;
                if (skin)
                {
                    // The GPU IB was already built by kFinalizeSkin during Phase1's synchronous
                    // GetRenderCtx load. OnM2SkinFinalize (fallback path via g_currentRebuildCmo)
                    // applied the merged filter before the IB was written. Do not call
                    // gm2::FinalizeSkin here: it calls kFinalizeSkin (0x837A40) directly,
                    // bypassing sub_838490's post-call buffer-fill step. That creates a new empty
                    // D3D buffer that never gets filled, making the model invisible.
                    EquipLog("  Phase3[%zu] skin present: IB already built by synchronous load, skip re-finalize", i);
                }
                else
                {
                    EquipLog("  Phase3[%zu] skin null: OnM2SkinFinalize will apply filter async", i);
                }
            }

            // Release our Phase-1 ref. The scene holds a ref from AttachToScene, so rctx stays
            // alive. e.renderCtx continues to point to it and is used by OnM2PerFrameUpdate.
            gm2::ReleaseRenderCtx(rctx);
        }
    }

    // ─── [REMOVED] weapon ItemDisplayInfo model-swap path ──────────────────────────
    //
    // LookupItemDisplayIdNative, LookupItemDisplayId, PatchWeaponModelByDisplayId, and
    // PreregisterSidecarWeapons used to live here, feeding EquipExtension::OnWeaponVisualChange
    // and EquipExtension::OnItemDisplayLookup further down (both also removed). All of it has been
    // superseded by WeaponExtension (see WeaponExtension.cpp), which patches ItemModelData.dbc's
    // Model1/Model2 fields at the data level instead of substituting them at runtime via a live
    // OnItemDisplayLookup hook -- the same "data names the virtual path directly" convention
    // CreatureModelData already uses for creatures. That data-level approach needs no per-wearer
    // race/gender resolution (weapons are never race/gender suffixed here) and no reactive
    // per-equip trigger, so the owner28/ChrRaces/Icon2-forced-variant handling this block used to
    // do is gone with it, not just relocated.
    //
    // Also removed as dead weight once this went: the WXLItemEntryDisplay.csv sidecar table
    // (g_sidecarItemDisplay, g_sidecarModelFolder, LoadItemDisplaySidecarFile -- these existed
    // solely to feed this block's itemEntry->displayId and displayId->folder lookups),
    // BuildMaterialPatchSpecWeaponFallback (this block's only caller), WeaponVPathOverride /
    // g_weaponVPaths (only ever written here, only ever read by OnItemDisplayLookup), and
    // WeaponAttachEntry / g_weaponAttached including its OnM2PerFrameUpdate owner28 retry sweep
    // (that sweep existed only to backfill owner28 for this block's forced-race/gender path).
    //
    // Still alive and unrelated to any of this: kSlotConfig's Weapon/OffHand/Ranged rows and
    // OnItemSlotChange's handling of them (WXLItemDisplayModels.csv-driven *additional* M2
    // attachment on a weapon slot -- a different feature from swapping the weapon's own native
    // model), ParseIcon2/kFlagForceModelRaceGender/ItemDisplayInfoLookupNative (still used by the
    // armor path below), and MaterialEntryMatches/BuildMaterialPatchSpecArmor (armor's own,
    // separate material-patch builder). See source control history if the old runtime-
    // substitution approach ever needs revisiting.


    // ─── Event handler implementations ───────────────────────────────────────────

    // See the doc comment on the declaration in EquipExtension.hpp for why this exists alongside
    // OnItemSlotChange. Weapon model swapping no longer runs through this module at all -- see
    // WeaponExtension.cpp's own OnModelLoadPre for that -- so this now only kicks off the armor-
    // side sidecar load (WXLItemDisplayModels.csv / WXLItemDisplayModelMaterials.csv).
    void EquipExtension::OnModelLoadPre(const ev::ModelLoadArgs&)
    {
        LoadSidecarModels(); // no-op after the first call
    }

    // Offhand weapon-mirror override. This client has no native offhand model mirroring (a weapon
    // dual-wielded in the offhand renders identically to mainhand, backwards-looking blade/hilt and
    // all). Two mirror sources, in priority order:
    //   1. WXLWeaponModels.csv's Model1OffhandPath/Model2OffhandPath/Geoset1Offhand/Geoset2Offhand
    //      -- a correctly pre-baked mirrored model, authored and supplied externally. Always wins
    //      when present (e.g. for a weapon whose hilt/engraving detail an automatic mirror gets
    //      visibly wrong).
    //   2. Otherwise, WeaponExtension.cpp's BakeWeaponDisplayOffhand auto-mirrors the mainhand
    //      model on the spot (VirtualPath.cpp's MirrorWeaponForOffhand: every vertex, normal,
    //      triangle winding, bone pivot, and translation/rotation keyframe across every animation
    //      sequence, plus attachment point offsets), so an offhand weapon is never left unmirrored
    //      by default.
    // Either way, WeaponGetOffhandVirtualPath is the single entry point this function calls --
    // (WXLWeaponModels.csv's Model1OffhandPath/Model2OffhandPath/Geoset1Offhand/Geoset2Offhand).
    //
    // ASSUMPTION FLAGGED FOR VERIFICATION: attach IDs 0 (mainhand/ITEM_VISUAL0) and 1
    // (offhand/ITEM_VISUAL1) below are the standard WoW client attachment-point convention, not
    // something confirmed against this specific client build/offset table the way every other
    // attach ID in this file (kSlotConfig, InferObjectComponentAttach) has been. If the offhand
    // weapon doesn't swap to the mirrored model, or swaps but attaches to the wrong hand bone, this
    // is the first thing to check.
    [[maybe_unused]] constexpr uint32_t kWeaponAttachMainHand = 0; // documents the pairing; only
                                                                     // kWeaponAttachOffHand is used below
    constexpr uint32_t kWeaponAttachOffHand  = 1;

    static void HandleOffhandWeaponOverride(void* cmo, void* subObj, uint32_t displayId)
    {
        if (!subObj || displayId == 0) return; // unequip: nothing to override, native detach handles it

        char vPath[2][280] = {};
        const bool has0 = wxl::scripts::weaponextension::WeaponGetOffhandVirtualPath(displayId, 0, vPath[0], sizeof(vPath[0]));
        const bool has1 = wxl::scripts::weaponextension::WeaponGetOffhandVirtualPath(displayId, 1, vPath[1], sizeof(vPath[1]));
        if (!has0 && !has1) return; // no offhand-mirror configured for this weapon -- leave native
                                     // vanilla attach (the mainhand bake) exactly as it already is

        EquipLog("  HandleOffhandWeaponOverride: displayId=%u col0='%s' col1='%s'",
                 displayId, has0 ? vPath[0] : "(none)", has1 ? vPath[1] : "(none)");

        // hkSlotDispatch already ran vanilla first (same ordering as every other slot -- see the
        // comment above Phase1 in RebuildAllModels) and attached the WRONG (mainhand-baked) model
        // at the offhand attach point before this handler ever runs. Detach it, then attach the
        // correct mirrored bake(s) in its place. Get our ref before detaching, same rationale as
        // Phase1/Phase2 for armor -- see that comment for the full explanation of why the ordering
        // matters.
        void* rctx[2] = { nullptr, nullptr };
        if (has0) rctx[0] = SafeGetRenderCtx(subObj, vPath[0]);
        if (has1) rctx[1] = SafeGetRenderCtx(subObj, vPath[1]);
        if (!rctx[0] && !rctx[1])
        {
            EquipLog("  HandleOffhandWeaponOverride: GetRenderCtx failed for both columns, aborting");
            return;
        }

        SafeDetachSlot(subObj, kWeaponAttachOffHand);

        if (rctx[0]) gm2::AttachToScene(rctx[0], subObj, kWeaponAttachOffHand, false);
        if (rctx[1]) gm2::AttachToScene(rctx[1], subObj, kWeaponAttachOffHand, false);
        EquipLog("  HandleOffhandWeaponOverride: attached mirrored model(s) at offhand attach point");
    }

    void EquipExtension::OnItemSlotChange(const ev::ItemSlotChangeArgs& a)
    {
        // DIAG (temporary -- weapon mainhand/offhand investigation): logged unconditionally, before
        // any of this function's existing branches/early-returns, specifically because the
        // modelSlot >= 14 cutoff further down means slots 15 (MainHand) and 17 (Ranged) currently
        // produce NO log output at all past this point -- only 16 (OffHand) reaches the "---
        // OnItemSlotChange" line below, via HandleOffhandWeaponOverride's own EquipLog calls. If
        // this line never appears for a mainhand/ranged equip, that's proof this event doesn't fire
        // for that slot at all (supporting the CharAddHandItem-instead-of-CharModelSlotDispatch
        // theory); if it DOES appear, the event fires but this function's own early-return is why
        // nothing further gets logged for those two slots today.
        EquipLog("[DIAG seq=%u tick=%u] OnItemSlotChange ENTRY cmo=0x%p slot=%u itemDataPtr=0x%p",
                 NextDiagSeq(), GetTickCount(), a.charModelObj, a.modelSlot, a.itemDataPtr);

        if (a.modelSlot == 16) // OffHand -- see HandleOffhandWeaponOverride's own comment. Handled
                                // separately from, and before, the armor-only cutoff just below:
                                // a weapon slot has exactly one attach point and none of the
                                // multi-entry/geoset-merge machinery below (entries/Phase1-3) applies.
                                // Purely additive on top of whatever native/WeaponExtension handling
                                // already happens for this slot -- falls through to the cutoff after.
        {
            void* subObjEarly = GuardedReadPtr(reinterpret_cast<uint8_t*>(a.charModelObj) +
                                                offsets::kOffCmoSceneNode);
            uint32_t displayIdEarly = a.itemDataPtr ? GuardedReadU32(a.itemDataPtr) : 0;
            HandleOffhandWeaponOverride(a.charModelObj, subObjEarly, displayIdEarly);
        }
        if (a.modelSlot >= 14) return;
        const SlotConfig& cfg = kSlotConfig[a.modelSlot];

        void* cmo = a.charModelObj;
        void* subObj = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + offsets::kOffCmoSceneNode);

        // Unequip path: display_id == 0 when the slot is being cleared via the equip hook.
        uint32_t displayId = 0;
        if (a.itemDataPtr)
            displayId = GuardedReadU32(a.itemDataPtr);

        EquipLog("--- OnItemSlotChange cmo=0x%p slot=%u idp=0x%p displayId=%u subObj=0x%p",
                 cmo, a.modelSlot, a.itemDataPtr, displayId, subObj);

        if (displayId == 0)
        {
            EquipLog("  unequip: detach+rebuild");
            DetachSlotEntries(cmo, a.modelSlot);
            if (subObj) RebuildAllModels(cmo);
            return;
        }

        // Look up the ItemDisplayInfo record. Calls the native storage lookup directly by address
        // (offsets::itemdisplayinfo::kLookup) rather than through the core's internal
        // wxl::runtime::game::ItemDisplayInfoLookupNative trampoline, which extensions cannot reach
        // (see PORTING_GUIDE.md: "Extensions cannot call core functions directly"). This is a direct
        // call into the client's own already-mapped code -- the same category as an SDK facade
        // reading the fixed-base image -- not a hook, so no HookAttach/original chain is involved.
        // Needs the true native fields, not whatever a weapon-slot override may have substituted for
        // some other display id, same reasoning as PatchWeaponModelByDisplayId above.
        alignas(4) uint8_t dispBuf[offsets::itemdisplayinfo::kRecordSize] = {};
        auto lookupFn = reinterpret_cast<offsets::itemdisplayinfo::LookupFn>(offsets::itemdisplayinfo::kLookup);
        void* storageObj = reinterpret_cast<void*>(offsets::itemdisplayinfo::kStorageObject);
        uint32_t ok = lookupFn(storageObj, nullptr, displayId, dispBuf);
        const char* modelName1 = nullptr;
        const char* modelName2 = nullptr;
        const char* texName1   = nullptr;
        const char* texName2   = nullptr;
        const char* icon2str   = nullptr;
        if (ok)
        {
            modelName1 = *reinterpret_cast<const char**>(dispBuf + offsets::itemdisplayinfo::kOffModel1);
            modelName2 = *reinterpret_cast<const char**>(dispBuf + offsets::itemdisplayinfo::kOffModel2);
            texName1   = *reinterpret_cast<const char**>(dispBuf + offsets::itemdisplayinfo::kOffTex1);
            texName2   = *reinterpret_cast<const char**>(dispBuf + offsets::itemdisplayinfo::kOffTex2);
            icon2str   = *reinterpret_cast<const char**>(dispBuf + offsets::itemdisplayinfo::kOffIcon2);
            EquipLog("  DBC ok: model1='%s' model2='%s' tex1='%s' tex2='%s' icon2ptr=0x%p icon2='%s'",
                     modelName1 ? modelName1 : "(null)", modelName2 ? modelName2 : "(null)",
                     texName1 ? texName1 : "(null)", texName2 ? texName2 : "(null)",
                     static_cast<const void*>(icon2str),
                     (icon2str && reinterpret_cast<uintptr_t>(icon2str) > 0x10000 && *icon2str)
                         ? icon2str : "(empty/invalid)");

            // DIAGNOSTIC ONLY -- temporary, for tracking down the character skin-composite
            // texture-component fields (ArmUpperTexture/ArmLowerTexture/TorsoUpperTexture/
            // TorsoLowerTexture/LegUpperTexture/LegLowerTexture/HandTexture/FootTexture etc.).
            // Core's offsets::itemdisplayinfo table only documents Model1/Model2/Tex1/Tex2/Icon2/
            // ParticleId -- it does not expose these fields, because nothing in this extension has
            // ever needed to read or patch them (they drive the client's own skin compositing, not
            // the equipped .m2's own texture-unit table, which is all VirtualPath.cpp touches).
            // dispBuf already holds the full 256-byte native record; every 4-byte slot in a WotLK-era
            // ItemDisplayInfo record that isn't a small integer is materialized as a char* into the
            // live DBC string block, so probing each slot as a pointer and printing it when it looks
            // valid (in the DBC's mapped string-block range and non-empty) lets us find the *real*
            // field offsets for this build without guessing wrong values into a byte-patch. Remove
            // this block once those offsets are confirmed and added to WxlOffsets.hpp properly.
            EquipLog("  DBC raw dump (displayId=%u), scanning for texture-component string fields:", displayId);
            for (size_t off = 0; off + 4 <= sizeof(dispBuf); off += 4)
            {
                uint32_t raw = 0;
                std::memcpy(&raw, dispBuf + off, sizeof(raw));
                if (raw <= 0x10000) continue; // too small to be a valid mapped pointer, skip quietly
                const char* asStr = reinterpret_cast<const char*>(static_cast<uintptr_t>(raw));
                __try
                {
                    if (*asStr && std::strlen(asStr) < 128)
                        EquipLog("    off=0x%02zX raw=0x%08X str='%s'", off, raw, asStr);
                    else
                        EquipLog("    off=0x%02zX raw=0x%08X (unreadable/empty)", off, raw);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    EquipLog("    off=0x%02zX raw=0x%08X (not a valid pointer)", off, raw);
                }
            }
        }
        else
        {
            EquipLog("  DBC lookup FAILED (displayId=%u not found)", displayId);
            return;
        }

        // Parse Icon2 attach config (start with defaults from slot config).
        uint32_t attachA_l = cfg.defAttach1, attachA_r = cfg.defAttach1;
        uint32_t attachB_l = cfg.defAttach2, attachB_r = cfg.defAttach2;
        uint32_t icon2flags = 0;
        char customFolder[64] = {};
        bool icon2AttachAExplicit = false;
        bool icon2AttachBExplicit = false;
        // Guard pointer range before dereferencing: a raw integer in g_disp+0x18 could be a
        // small non-null value (GeosetGroup integer) that would AV if dereferenced directly.
        if (icon2str && reinterpret_cast<uintptr_t>(icon2str) > 0x10000
            && *icon2str && (*icon2str == ':' || (*icon2str >= '0' && *icon2str <= '9')))
        {
            icon2AttachAExplicit = Icon2FieldHasValue(icon2str, 0);
            icon2AttachBExplicit = Icon2FieldHasValue(icon2str, 1);
            ParseIcon2(icon2str, &attachA_l, &attachA_r, &attachB_l, &attachB_r,
                       &icon2flags, customFolder, sizeof(customFolder));
        }

        // Use left-side attach values (0x80 sided-slot selection not yet implemented).
        uint32_t attachA = attachA_l;
        uint32_t attachB = attachB_l;

        EquipLog("  attachA=%u attachB=%u icon2flags=0x%X folder='%s'",
                 attachA, attachB, icon2flags, customFolder[0] ? customFolder : "(default)");

        // ChrRaces lookup for race code and gender string.
        uint32_t raceId = GuardedReadU32(reinterpret_cast<uint8_t*>(cmo) + offsets::kOffCmoRace);
        uint32_t low    = *reinterpret_cast<uint32_t*>(offsets::chrraces::kMinId);
        uint32_t high   = *reinterpret_cast<uint32_t*>(offsets::chrraces::kMaxId);
        if (raceId < low || raceId > high)
        {
            EquipLog("  raceId=%u out of range [%u,%u], bail", raceId, low, high);
            return;
        }
        uint8_t* idTable = *reinterpret_cast<uint8_t**>(offsets::chrraces::kIdTable);
        void*    chrRec  = *reinterpret_cast<void**>(idTable + (raceId - low) * sizeof(void*));
        if (!chrRec)
        {
            EquipLog("  chrRec null for raceId=%u, bail", raceId);
            return;
        }

        const char* raceCode  = *reinterpret_cast<const char**>(reinterpret_cast<uint8_t*>(chrRec) + offsets::chrraces::kOffRecordPrefix);
        uint32_t    genderIdx = GuardedReadU32(reinterpret_cast<uint8_t*>(cmo) + offsets::kOffCmoGender);
        const char* genderStr = *reinterpret_cast<const char**>(offsets::genderstrings::kTable + genderIdx * sizeof(void*));
        if (!raceCode || !genderStr)
        {
            EquipLog("  raceCode or genderStr null (raceCode=0x%p genderStr=0x%p), bail",
                     static_cast<const void*>(raceCode), static_cast<const void*>(genderStr));
            return;
        }

        EquipLog("  race='%s' gender='%s'", raceCode, genderStr);

        if (!subObj)
        {
            EquipLog("  subObj null, bail");
            return;
        }

        // Remove stale entries for this model slot before adding new ones.
        DetachSlotEntries(cmo, a.modelSlot);

        // Helper: strip optional ":N[,M]" geoset suffix from the model name field.
        auto splitModelName = [](const char* name, char* stemOut, size_t stemSz, GeosetFilter* filterOut)
        {
            *filterOut = {};
            if (!name || !*name) { stemOut[0] = '\0'; return; }
            const char* colon = std::strchr(name, ':');
            if (colon)
            {
                size_t len = static_cast<size_t>(colon - name);
                if (len >= stemSz) len = stemSz - 1;
                std::memcpy(stemOut, name, len);
                stemOut[len] = '\0';
                *filterOut = ParseGeosetFilter(colon + 1);
            }
            else { std::strncpy(stemOut, name, stemSz - 1); stemOut[stemSz - 1] = '\0'; }
        };

        bool rowHasCollection = false;
        bool rowHasNormal = false;
        AnalyzeModelList(modelName1, &rowHasCollection, &rowHasNormal);
        AnalyzeModelList(modelName2, &rowHasCollection, &rowHasNormal);
        const bool mixedCollectionRow = rowHasCollection && rowHasNormal;

        // Effective path flags: collection models always use race+gender; normal models use slot defaults.
        // Icon2 flags 0x2/0x4 can still suppress race/gender regardless.
        auto effectiveFlags = [&](bool isCollection) -> uint32_t
        {
            uint32_t ef = icon2flags;
            if (isCollection && !(ef & 0x40))
            {
                const bool appendsRace = (ef & 0x4) == 0;
                const bool appendsGender = (ef & 0x2) == 0;
                if (appendsRace && appendsGender)
                    ef |= 0x40;
            }
            if (!isCollection)
            {
                const bool forceModelRaceGender =
                    (ef & kFlagForceModelRaceGender) || ((ef & 0x40) && !mixedCollectionRow);
                if (!forceModelRaceGender)
                {
                    if (!cfg.defUseRace)   ef |= 0x4;
                    if (!cfg.defUseGender) ef |= 0x2;
                }
            }
            return ef;
        };

        auto attachForEntry = [](bool isCollection, bool mixedRow, uint32_t columnAttach,
                                 uint32_t defaultAttach, bool explicitAttach) -> uint32_t
        {
            if (mixedRow) return isCollection ? kCollectionAttach : defaultAttach;
            if (isCollection && !explicitAttach) return kCollectionAttach;
            return columnAttach;
        };

        auto addModelList = [&](const char* label, const char* modelList, const char* texList,
                                uint32_t columnAttach, uint32_t defaultAttach, bool explicitAttach)
        {
            bool anyModel = false;
            for (uint32_t idx = 0; idx < 16; ++idx)
            {
                char modelPart[264] = {};
                if (!CopyListPart(modelList, idx, modelPart, sizeof(modelPart))) break;
                if (!modelPart[0]) continue;
                anyModel = true;

                char texPart[264] = {};
                if (!CopyListPart(texList, idx, texPart, sizeof(texPart)))
                    CopyListPart(texList, 0, texPart, sizeof(texPart));

                char stem[264]; GeosetFilter geo;
                splitModelName(modelPart, stem, sizeof(stem), &geo);
                if (!stem[0])
                {
                    EquipLog("  %s[%u]: empty stem after split, skip", label, idx);
                    continue;
                }

                const bool isCollection = geo.count > 0;
                if (!isCollection && !SlotAllowsNormalObjectModel(a.modelSlot))
                {
                    EquipLog("  %s[%u]: normal model '%s' ignored for texture-only slot %u",
                             label, idx, stem, a.modelSlot);
                    continue;
                }

                const uint32_t ef = effectiveFlags(isCollection);
                uint32_t attach = attachForEntry(isCollection, mixedCollectionRow, columnAttach,
                                                 defaultAttach, explicitAttach);
                attach = InferObjectComponentAttach(stem, isCollection, explicitAttach, attach);
                if (isCollection && IsCollectionGloveBodyModel(a.modelSlot, stem))
                    attach = kCollectionAttach;
                if (attach == static_cast<uint32_t>(-1))
                {
                    EquipLog("  %s[%u]: attach==-1, skip", label, idx);
                    continue;
                }

                char modelPath[264];
                BuildSlotPath(modelPath, stem, raceCode, genderStr, ef, cfg.folder,
                              isCollection, customFolder);

                char texPath[264] = {};
                if (texPart[0])
                    BuildTexPath(texPath, sizeof(texPath), texPart, raceCode, genderStr,
                                 ef, cfg.folder, isCollection, customFolder, stem);

                char matTexSpec[2048] = {};
                const uint32_t modelColumn = (label && label[1] == '1') ? 0u : 1u;
                BuildMaterialPatchSpecArmor(matTexSpec, sizeof(matTexSpec), displayId,
                                            modelColumn, idx, stem, raceCode, genderStr,
                                            cfg.folder, isCollection, customFolder, stem);

                EquipLog("  %s[%u]: attach=%u path='%s' tex='%s' mat='%s' geoCount=%u",
                         label, idx, attach, modelPath, texPath[0] ? texPath : "(none)",
                         matTexSpec, geo.count);

                AttachEntry e = {};
                e.equipSlot = a.modelSlot;
                e.attachId  = attach;
                e.subObj    = subObj;
                std::memcpy(e.keyBuf, modelPath, sizeof(e.keyBuf));
                std::memcpy(e.texBuf, texPath,   sizeof(e.texBuf));
                std::memcpy(e.matTexBuf, matTexSpec, sizeof(e.matTexBuf));
                e.geoFilter = geo;
                if (isCollection && mixedCollectionRow)
                    e.mergeKey = ((a.modelSlot + 1) << 16) |
                                 ((label && label[1] == '1' ? 1u : 2u) << 8) |
                                 (idx & 0xFFu);
                if (e.mergeKey)
                    EquipLog("  %s[%u]: isolated collection mergeKey=0x%X", label, idx, e.mergeKey);
                g_attached[cmo].push_back(e);
            }

            if (!anyModel) EquipLog("  %s: no model name, skip", label);
        };

        auto addSidecarModels = [&]() -> bool
        {
            LoadSidecarModels();
            auto it = g_sidecarModels.find(displayId);
            if (it == g_sidecarModels.end() || it->second.empty()) return false;

            bool added = false;
            uint32_t sidecarIndex = 0;
            for (const SidecarModelEntry& sc : it->second)
            {
                if (sc.modelSlot != static_cast<uint32_t>(-1) && sc.modelSlot != a.modelSlot)
                    continue;

                uint32_t attach = sc.attachId;
                if (attach == static_cast<uint32_t>(-1))
                    attach = cfg.defAttach1;
                if (attach == static_cast<uint32_t>(-1))
                    continue;

                const bool isCollectionPath =
                    (sc.folder[0] && StartsWithCI(sc.folder, "Collections")) ||
                    sc.geoFilter.count > 0 ||
                    StartsWithCI(sc.model, "collections_");
                if (isCollectionPath && IsCollectionGloveBodyModel(a.modelSlot, sc.model))
                    attach = kCollectionAttach;
                if (!isCollectionPath && !SlotAllowsNormalObjectModel(a.modelSlot))
                {
                    EquipLog("  SC[%u]: normal model '%s' ignored for texture-only slot %u",
                             sidecarIndex, sc.model, a.modelSlot);
                    ++sidecarIndex;
                    continue;
                }
                const uint32_t modelFlags =
                    (sc.modelFlags == 0xffffffffu) ? effectiveFlags(isCollectionPath) : sc.modelFlags;
                const uint32_t textureFlags =
                    (sc.textureFlags == 0xffffffffu) ? effectiveFlags(isCollectionPath) : sc.textureFlags;

                char modelPath[264];
                BuildSlotPath(modelPath, sc.model, raceCode, genderStr, modelFlags, cfg.folder,
                              isCollectionPath, sc.folder[0] ? sc.folder : nullptr);

                char texPath[264] = {};
                if (sc.texture[0])
                    BuildTexPath(texPath, sizeof(texPath), sc.texture, raceCode, genderStr,
                                 textureFlags, cfg.folder, isCollectionPath,
                                 sc.folder[0] ? sc.folder : nullptr, sc.model);

                char matTexSpec[2048] = {};
                BuildMaterialPatchSpecArmor(matTexSpec, sizeof(matTexSpec), displayId,
                                            static_cast<uint32_t>(-1), sidecarIndex, sc.model,
                                            raceCode, genderStr, cfg.folder, isCollectionPath,
                                            sc.folder[0] ? sc.folder : nullptr, sc.model);

                EquipLog("  SC[%u]: attach=%u path='%s' tex='%s' mat='%s' geoCount=%u slot=%u",
                         sidecarIndex, attach, modelPath, texPath[0] ? texPath : "(none)",
                         matTexSpec, sc.geoFilter.count, sc.modelSlot);

                AttachEntry e = {};
                e.equipSlot = a.modelSlot;
                e.attachId  = attach;
                e.subObj    = subObj;
                std::memcpy(e.keyBuf, modelPath, sizeof(e.keyBuf));
                std::memcpy(e.texBuf, texPath,   sizeof(e.texBuf));
                std::memcpy(e.matTexBuf, matTexSpec, sizeof(e.matTexBuf));
                e.geoFilter = sc.geoFilter;
                g_attached[cmo].push_back(e);
                added = true;
                ++sidecarIndex;
            }

            if (added)
                EquipLog("  sidecar used for displayId=%u, DBC model lists skipped", displayId);
            return added;
        };

        if (!addSidecarModels())
        {
            addModelList("M1", modelName1, texName1, attachA, cfg.defAttach1, icon2AttachAExplicit);
            addModelList("M2", modelName2, texName2, attachB, cfg.defAttach2, icon2AttachBExplicit);
        }

        EquipLog("  calling RebuildAllModels");
        RebuildAllModels(cmo);
    }

    void EquipExtension::OnItemSlotClear(const ev::ItemSlotClearArgs& a)
    {
        // DIAG (temporary -- weapon mainhand/offhand investigation): see OnItemSlotChange's
        // matching comment. equipSlotWow here is the WoW slot index directly (15/16/17 for
        // weapons), not yet translated through kEquipToModelSlot.
        EquipLog("[DIAG seq=%u tick=%u] OnItemSlotClear ENTRY cmo=0x%p equipSlotWow=%u",
                 NextDiagSeq(), GetTickCount(), a.charModelObj, a.equipSlotWow);

        if (a.equipSlotWow >= 19) return;
        uint32_t modelSlot = kEquipToModelSlot[a.equipSlotWow];
        if (modelSlot == static_cast<uint32_t>(-1)) return;

        void* cmo = a.charModelObj;

        DetachSlotEntries(cmo, modelSlot);

        void* subObj = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + offsets::kOffCmoSceneNode);
        if (subObj) RebuildAllModels(cmo);
    }

    void EquipExtension::OnM2PerFrameUpdate(const ev::M2PerFrameUpdateArgs& a)
    {
        void* renderCtx = a.renderCtx;

        // The weapon owner28 retry sweep that used to run here is gone -- it only ever existed to
        // backfill owner28 for the old PatchWeaponModelByDisplayId forced-race/gender path
        // (g_weaponAttached), both removed now that WeaponExtension handles weapon models at the
        // data level with no per-wearer race/gender resolution at all.

        if (g_attached.empty()) return;

        // Purge dead CMOs: cmo->sceneNode is zeroed when WoW frees the character model object.
        // Without purging, a stale entry can mistakenly match a reused CMO address.
        for (auto it = g_attached.begin(); it != g_attached.end(); )
        {
            void* sub = GuardedReadPtr(reinterpret_cast<uint8_t*>(it->first) + offsets::kOffCmoSceneNode);
            if (!sub)
            {
                void* deadCmo = it->first;
                VPathEvictCmo(deadCmo);
                it = g_attached.erase(it);
            }
            else      ++it;
        }
        if (g_attached.empty()) return;

        // Character-model PerFrame sweep: fires when the character's sceneNode is the current
        // renderCtx. Two responsibilities:
        //   1. Pending-attach rebuild (entries with null renderCtx).
        //   2. Per-frame bone copy driven from the character model itself.
        // Driving bone copy here guarantees it runs every frame regardless of whether the
        // collection model's own PerFrame has fired yet, avoiding the race where collBuf is
        // null on the collection model's first PerFrame and the entry's renderCtx changes
        // before collBuf becomes non-null.
        for (auto& [cmo, entries] : g_attached)
        {
            void* sceneNode = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + offsets::kOffCmoSceneNode);
            if (sceneNode != renderCtx) continue;

            bool hasPending = false;
            for (const auto& e : entries)
                if (!e.renderCtx) { hasPending = true; break; }
            if (hasPending) { RebuildAllModels(cmo); return; }

            // charBuf: character bone palette. Valid when the character model is rendering.
            auto* charBuf = reinterpret_cast<uint8_t*>(
                GuardedReadPtr(reinterpret_cast<uint8_t*>(renderCtx) + offsets::kOffInstBonePalette));
            if (charBuf)
            {
                for (auto& entry : entries)
                {
                    if (!NeedsManualBoneRemap(entry)) continue;
                    if (!entry.renderCtx || entry.boneRemap.count == 0) continue;
                    auto* collBuf = reinterpret_cast<uint8_t*>(
                        GuardedReadPtr(reinterpret_cast<uint8_t*>(entry.renderCtx) + offsets::kOffInstBonePalette));
                    if (!collBuf) continue;
                    if (!entry.charSweepApplied)
                    {
                        uint32_t charInitF = GuardedReadU32(
                            reinterpret_cast<uint8_t*>(renderCtx) + offsets::kOffInstInitFlags);
                        uint32_t collInitF = GuardedReadU32(
                            reinterpret_cast<uint8_t*>(entry.renderCtx) + offsets::kOffInstInitFlags);
                        EquipLog("  CharSweep(bone): rctx=0x%p charInitFlags=0x%X collBuf=0x%p collInitFlags=0x%X -> APPLYING",
                                 entry.renderCtx, charInitF, static_cast<void*>(collBuf), collInitF);
                        entry.charSweepApplied = true;
                    }
                    const BoneRemap& remap = entry.boneRemap;
                    const uint32_t collBoneCount = RenderCtxBoneCountGuarded(entry.renderCtx);
                    const uint32_t charBoneCount = RenderCtxBoneCountGuarded(renderCtx);
                    if (!CopyRemappedBonesGuarded(collBuf, collBoneCount, charBuf, charBoneCount,
                                                  remap, entry.renderCtx, renderCtx))
                    {
                        EquipLog("  CharSweep(bone): guarded copy failed rctx=0x%p count=%u",
                                 entry.renderCtx, (uint32_t)remap.count);
                        entry.boneRemap = {};
                        entry.renderCtx = nullptr;
                    }
                }
            }
            return;
        }

        for (auto& [cmo, entries] : g_attached)
        {
            for (auto& entry : entries)
            {
                if (entry.renderCtx != renderCtx) continue;

                // Liveness check: DetachSlot zeroes the model pointer (render_ctx+0x2c) without
                // freeing the render_ctx allocation. If evicted (by vanilla slot handlers sharing
                // the same attach point), re-attach immediately.
                void* m2Live = GuardedReadPtr(reinterpret_cast<uint8_t*>(renderCtx) + offsets::kOffInstModel);
                if (!m2Live)
                {
                    // Use owner28 (scene-node's back-pointer to its owning CMO) for GetRenderCtx,
                    // not the slot-dispatch 'cmo'. See RebuildAllModels for the full explanation.
                    void* owner28 = GuardedReadPtr(reinterpret_cast<uint8_t*>(entry.subObj) + offsets::kOffSceneNodeOwner);
                    if (owner28 && entry.keyBuf[0])
                    {
                        // SafeGetRenderCtx guards against dangling hash-table buckets (same
                        // root cause as the RebuildAllModels equip crash — see that function).
                        const char* pfKey = entry.mangledKeyBuf[0] ? entry.mangledKeyBuf : entry.keyBuf;
                        void* rctx = SafeGetRenderCtx(owner28, pfKey);
                        if (rctx)
                        {
                            if (entry.texBuf[0])
                            {
                                void* tex = gm2::LoadResource(entry.texBuf, 0);
                                if (!tex)
                                {
                                    EquipLog("  PerFrame reattach texture load failed '%s', skip", entry.texBuf);
                                    for (auto& e2 : entries)
                                        if (e2.renderCtx == renderCtx) e2.renderCtx = nullptr;
                                    gm2::ReleaseRenderCtx(rctx);
                                    continue;
                                }
                                else
                                {
                                    gm2::BindTexSlot(rctx, tex);
                                    // Keep the texture handle alive for the attached render context.
                                }
                            }
                            gm2::AttachToScene(rctx, entry.subObj, entry.attachId, IsCollectionEntry(entry));
                            for (auto& e2 : entries)
                                if (e2.renderCtx == renderCtx) e2.renderCtx = rctx;
                            gm2::ReleaseRenderCtx(rctx);
                        }
                        else
                        {
                            for (auto& e2 : entries)
                                if (e2.renderCtx == renderCtx) e2.renderCtx = nullptr;
                        }
                    }
                    else
                    {
                        for (auto& e2 : entries)
                            if (e2.renderCtx == renderCtx) e2.renderCtx = nullptr;
                    }
                    return;
                }

                // Bone matrix copy via 3-pass remap table.
                // charCtx is the character's render_ctx (= cmo->sceneNode = the outer frame call).
                // The collection M2's bone buffer is overwritten with corresponding char matrices.
                if (!IsCollectionEntry(entry)) return;
                if (entry.boneRemap.count == 0) return;													   
                void* charCtx = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + offsets::kOffCmoSceneNode);
                if (charCtx && charCtx != renderCtx)
                {
                    BoneRemap& remap = entry.boneRemap;
                    // Retry if: (a) never built (count=0), or (b) all entries are 0xFF.
                    // Case (b) occurs when Phase3 runs while charBase is uninitialized:
                    // the function throws inside a pass, __except catches it and returns
                    // count=N/all-0xFF. Phase3's propagation doesn't overwrite it on
                    // subsequent rebuilds because the count != 0 guard prevents updates.
                    bool remapBroken = (remap.count == 0);
                    if (!remapBroken && remap.count > 0)
                    {
                        bool allFF = true;
                        for (uint32_t bi = 0; allFF && bi < remap.count; ++bi)
                            allFF = (remap.collToChar[bi] == 0xFF);
                        remapBroken = allFF;
                    }
                    if (remapBroken)
                    {
                        BoneRemap tryRemap = BuildBoneRemapGuarded(renderCtx, charCtx);
                        if (tryRemap.count > 0)
                        {
                            // Only adopt if at least one bone was matched; otherwise charBase
                            // is still unavailable and we'd just pin the same broken remap.
                            bool hasMatch = false;
                            for (uint32_t bi = 0; !hasMatch && bi < tryRemap.count; ++bi)
                                hasMatch = (tryRemap.collToChar[bi] != 0xFF);
                            if (hasMatch)
                            {
                                remap = tryRemap;
                                // Propagate to ALL entries sharing this renderCtx — not just
                                // count==0 ones, since they may be stuck with the broken remap.
                                for (auto& e2 : entries)
                                    if (e2.renderCtx == renderCtx)
                                        e2.boneRemap = tryRemap;
                                uint32_t xff2 = 0;
                                for (uint32_t bi = 0; bi < tryRemap.count; ++bi)
                                    if (tryRemap.collToChar[bi] == 0xFF) ++xff2;
                                EquipLog("  PerFrame[retry-ok] rctx=0x%p count=%u xff=%u"
                                         " map0-7=[%u %u %u %u %u %u %u %u]",
                                         renderCtx, (uint32_t)tryRemap.count, xff2,
                                         tryRemap.count>0 ? (uint32_t)tryRemap.collToChar[0] : 255u,
                                         tryRemap.count>1 ? (uint32_t)tryRemap.collToChar[1] : 255u,
                                         tryRemap.count>2 ? (uint32_t)tryRemap.collToChar[2] : 255u,
                                         tryRemap.count>3 ? (uint32_t)tryRemap.collToChar[3] : 255u,
                                         tryRemap.count>4 ? (uint32_t)tryRemap.collToChar[4] : 255u,
                                         tryRemap.count>5 ? (uint32_t)tryRemap.collToChar[5] : 255u,
                                         tryRemap.count>6 ? (uint32_t)tryRemap.collToChar[6] : 255u,
                                         tryRemap.count>7 ? (uint32_t)tryRemap.collToChar[7] : 255u);
                            }
                        }
                    }

                    if (remap.count == 0)
                    {
                        entry.perFrameLogged = true;
                        return;
                    }

                    auto* charBuf = reinterpret_cast<uint8_t*>(
                        GuardedReadPtr(reinterpret_cast<uint8_t*>(charCtx) + offsets::kOffInstBonePalette));
                    auto* collBuf = reinterpret_cast<uint8_t*>(
                        GuardedReadPtr(reinterpret_cast<uint8_t*>(renderCtx) + offsets::kOffInstBonePalette));

                    if (!entry.perFrameLogged)
                    {
                        uint32_t collInitF = GuardedReadU32(reinterpret_cast<uint8_t*>(renderCtx) + offsets::kOffInstInitFlags);
                        uint32_t xff = 0;
                        for (uint32_t bi = 0; bi < remap.count; ++bi)
                            if (remap.collToChar[bi] == 0xFF) ++xff;
                        EquipLog("  PerFrame[first] rctx=0x%p flags=0x%X remap.count=%u xff=%u"
                                 " map0-7=[%u %u %u %u %u %u %u %u]"
                                 " charBuf=0x%p collBuf=0x%p",
                                 renderCtx, collInitF, (uint32_t)remap.count, xff,
                                 remap.count>0 ? (uint32_t)remap.collToChar[0] : 255u,
                                 remap.count>1 ? (uint32_t)remap.collToChar[1] : 255u,
                                 remap.count>2 ? (uint32_t)remap.collToChar[2] : 255u,
                                 remap.count>3 ? (uint32_t)remap.collToChar[3] : 255u,
                                 remap.count>4 ? (uint32_t)remap.collToChar[4] : 255u,
                                 remap.count>5 ? (uint32_t)remap.collToChar[5] : 255u,
                                 remap.count>6 ? (uint32_t)remap.collToChar[6] : 255u,
                                 remap.count>7 ? (uint32_t)remap.collToChar[7] : 255u,
                                 static_cast<void*>(charBuf), static_cast<void*>(collBuf));
                    }

                    if (charBuf && collBuf)
                    {
                        const uint32_t collBoneCount = RenderCtxBoneCountGuarded(renderCtx);
                        const uint32_t charBoneCount = RenderCtxBoneCountGuarded(charCtx);
                        if (!CopyRemappedBonesGuarded(collBuf, collBoneCount, charBuf, charBoneCount,
                                                      remap, renderCtx, charCtx))
                        {
                            EquipLog("  PerFrame(copy): guarded copy failed rctx=0x%p count=%u",
                                     renderCtx, (uint32_t)remap.count);
                            entry.boneRemap = {};
                            entry.renderCtx = nullptr;
                        }
                    }
                }

                entry.perFrameLogged = true;
                return;
            }
        }
    }

    void EquipExtension::OnM2SkinFinalize(const ev::M2SkinFinalizeArgs& a)
    {
        void* model = a.model;
        if (!model || g_attached.empty()) return;

        // Primary path: match by renderCtx → model pointer. Reliable for async loads where
        // entry.renderCtx is set by Phase1 before kFinalizeSkin fires.
        for (auto& [cmo, entries] : g_attached)
        {
            for (auto& entry : entries)
            {
                if (!entry.renderCtx || !NeedsManualBoneRemap(entry)) continue;
                void* entryModel = GuardedReadPtr(
                    reinterpret_cast<uint8_t*>(entry.renderCtx) + offsets::kOffInstModel);
                if (entryModel != model) continue;

                GeosetFilter merged = {};
                for (auto& e2 : entries)
                {
                    if (e2.renderCtx != entry.renderCtx) continue;
                    for (uint32_t fi = 0; fi < e2.geoFilter.count && merged.count < 16; ++fi)
                    {
                        bool dup = false;
                        for (uint32_t fj = 0; fj < merged.count; ++fj)
                            if (merged.ids[fj] == e2.geoFilter.ids[fi]) { dup = true; break; }
                        if (!dup) merged.ids[merged.count++] = e2.geoFilter.ids[fi];
                    }
                }


                    auto* skin = gm2::M2Model(model).GetSkin();
                    EquipLog("  OnM2SkinFinalize(primary): model=0x%p skin=0x%p merged.count=%u ids=[%u %u %u %u]",
                             model, static_cast<void*>(skin), merged.count,
                             merged.count > 0 ? merged.ids[0] : 0,
                             merged.count > 1 ? merged.ids[1] : 0,
                             merged.count > 2 ? merged.ids[2] : 0,
                             merged.count > 3 ? merged.ids[3] : 0);
                    if (skin) ApplyRawTriFilter(skin, merged);


                // Bone remap: apply immediately on async load completion.
                // OnM2SkinFinalize is the earliest moment entry.renderCtx is valid and the
                // collection model is parsed. PerFrame retries if collBuf is still null here.
                void* charCtx = GuardedReadPtr(
                    reinterpret_cast<uint8_t*>(cmo) + offsets::kOffCmoSceneNode);
                if (charCtx && charCtx != entry.renderCtx)
                {
                    if (entry.boneRemap.count == 0)
                    {
                        BoneRemap tryRemap = BuildBoneRemapGuarded(entry.renderCtx, charCtx);
                        if (tryRemap.count > 0)
                        {
                            for (auto& e2 : entries)
                                if (e2.renderCtx == entry.renderCtx) e2.boneRemap = tryRemap;
                        }
                    }
                    const BoneRemap& remap = entry.boneRemap;
                    if (remap.count > 0)
                    {
                        auto* charBuf = reinterpret_cast<uint8_t*>(GuardedReadPtr(
                            reinterpret_cast<uint8_t*>(charCtx) + offsets::kOffInstBonePalette));
                        auto* collBuf = reinterpret_cast<uint8_t*>(GuardedReadPtr(
                            reinterpret_cast<uint8_t*>(entry.renderCtx) + offsets::kOffInstBonePalette));
                        EquipLog("  OnM2SkinFinalize(bone): charCtx=0x%p charBuf=0x%p collBuf=0x%p count=%u -> %s",
                             charCtx, static_cast<void*>(charBuf), static_cast<void*>(collBuf),
                             (uint32_t)remap.count,
                             (charBuf && collBuf) ? "APPLIED" : "DEFERRED-to-PerFrame");
                    if (charBuf && collBuf)
                        {
                            const uint32_t collBoneCount = RenderCtxBoneCountGuarded(entry.renderCtx);
                            const uint32_t charBoneCount = RenderCtxBoneCountGuarded(charCtx);
                            if (!CopyRemappedBonesGuarded(collBuf, collBoneCount, charBuf, charBoneCount,
                                                          remap, entry.renderCtx, charCtx))
                            {
                                EquipLog("  OnM2SkinFinalize(copy): guarded copy failed rctx=0x%p count=%u",
                                         entry.renderCtx, (uint32_t)remap.count);
                                entry.boneRemap = {};
                                entry.renderCtx = nullptr;
                            }
                        }
                    }
                }
                return;
            }
        }

        // Fallback path: virtual-path models load SYNCHRONOUSLY inside Phase1's GetRenderCtx
        // (in-process VirtualProvide has no IPC delay), so kFinalizeSkin fires before Phase1
        // sets entry.renderCtx. Match by model path stem (kOffModelPathStem = inline char buf
        // at model+0x3C, set by kInit from the M2 file's internal name = the real path stem).
        if (!g_currentRebuildCmo) return;
        auto it2 = g_attached.find(g_currentRebuildCmo);
        if (it2 == g_attached.end()) return;

        __try
        {
            const char* modelStem = reinterpret_cast<const char*>(
                reinterpret_cast<uint8_t*>(model) + offsets::kOffModelPathStem);
            // Sanity-check: first char must be printable ASCII (path starts with 'I' for
            // Item\... paths). Garbage pointers or uninitialised memory fail this check.
            if (!modelStem || static_cast<unsigned char>(modelStem[0]) < 0x20
                           || static_cast<unsigned char>(modelStem[0]) > 0x7E)
                return;

            EquipLog("  OnM2SkinFinalize(fallback) cmo=0x%p modelStem='%.80s'",
                     g_currentRebuildCmo, modelStem);

            GeosetFilter merged = {};
            bool found = false;
            for (auto& entry : it2->second)
            {
                if (entry.geoFilter.count == 0 || entry.renderCtx) continue;
                // modelStem (model+0x3C) may be a full path or just a filename stem, and may
                // differ in case from keyBuf (DBC uses mixed case; host files are lowercase).
                // Extract filename part from both sides, strip any extension, compare ignoring case.
                const char* msBase = modelStem;
                for (const char* p = modelStem; *p; ++p)
                    if (*p == '\\' || *p == '/') msBase = p + 1;
                const char* msDot = nullptr;
                for (const char* p = msBase; *p; ++p) if (*p == '.') msDot = p;
                size_t msLen = msDot ? static_cast<size_t>(msDot - msBase) : std::strlen(msBase);

                const char* fileBase = entry.keyBuf;
                for (const char* p = entry.keyBuf; *p; ++p)
                    if (*p == '\\' || *p == '/') fileBase = p + 1;
                const char* lastDot = nullptr;
                for (const char* p = fileBase; *p; ++p) if (*p == '.') lastDot = p;
                size_t stemLen = lastDot ? static_cast<size_t>(lastDot - fileBase)
                                         : std::strlen(fileBase);
                if (msLen != stemLen) continue;
                if (_strnicmp(msBase, fileBase, stemLen) != 0) continue;

                found = true;
                for (uint32_t fi = 0; fi < entry.geoFilter.count && merged.count < 16; ++fi)
                {
                    bool dup = false;
                    for (uint32_t fj = 0; fj < merged.count; ++fj)
                        if (merged.ids[fj] == entry.geoFilter.ids[fi]) { dup = true; break; }
                    if (!dup) merged.ids[merged.count++] = entry.geoFilter.ids[fi];
                }
            }

            if (found)
            {
                auto* skin = gm2::M2Model(model).GetSkin();
                EquipLog("  OnM2SkinFinalize(fallback): skin=0x%p merged.count=%u ids=[%u %u %u %u]",
                         static_cast<void*>(skin), merged.count,
                         merged.count > 0 ? merged.ids[0] : 0,
                         merged.count > 1 ? merged.ids[1] : 0,
                         merged.count > 2 ? merged.ids[2] : 0,
                         merged.count > 3 ? merged.ids[3] : 0);
                if (skin) ApplyRawTriFilter(skin, merged);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ─── OnBuildBonePalette ───────────────────────────────────────────────────────
    // Re-apply the character remap after the engine's native palette build. ModelFrame clones do not fire
    // equip-slot events, but preserve the shared collection/character model pair, so handle those too.
    void EquipExtension::OnBuildBonePalette(const ev::BuildBonePaletteArgs& a)
    {
        if (g_attached.empty() || !a.renderCtx) return;
        void* renderCtx = a.renderCtx;
        void* childModel = GuardedReadPtr(
            reinterpret_cast<uint8_t*>(renderCtx) + offsets::kOffInstModel);
        AttachEntry* cloneCandidate = nullptr;
        void* clonePoseSource = nullptr;

        // Handle exact tracked instances immediately while remembering at most one validated
        // ModelFrame clone candidate. This keeps the hot callback to one g_attached traversal.
        for (auto& [cmo, entries] : g_attached)
        {
            void* charRctx = GuardedReadPtr(
                reinterpret_cast<uint8_t*>(cmo) + offsets::kOffCmoSceneNode);

            for (auto& entry : entries)
            {
                if (!NeedsManualBoneRemap(entry)) continue;

                if (entry.renderCtx != renderCtx)
                {
                    if (!cloneCandidate && childModel &&
                        BoneRemapHasMatch(entry.boneRemap) &&
                        entry.boneRemap.collectionModel == childModel &&
                        entry.boneRemap.characterModel)
                    {
                        void* candidateSource = FindAncestorWithModelGuarded(
                            renderCtx, entry.boneRemap.characterModel);
                        if (candidateSource)
                        {
                            cloneCandidate = &entry;
                            clonePoseSource = candidateSource;
                        }
                    }
                    continue;
                }

                // The native parent is authoritative after attachment. Resolve by model identity
                // so a mounted character is selected instead of an outer vehicle/root instance.
                void* poseSource = FindAncestorWithModelGuarded(
                    renderCtx, entry.boneRemap.characterModel);
                if (!poseSource) poseSource = NativeParentGuarded(renderCtx);
                if (!poseSource) poseSource = charRctx;
                if (!poseSource || poseSource == renderCtx) return;

                if (!BoneRemapMatchesContextsGuarded(
                        entry.boneRemap, renderCtx, poseSource))
                {
                    BoneRemap rebuilt = BuildBoneRemapGuarded(renderCtx, poseSource);
                    if (!BoneRemapHasMatch(rebuilt)) return;
                    for (auto& sibling : entries)
                        if (sibling.renderCtx == renderCtx) sibling.boneRemap = rebuilt;
                }

                const BoneRemap& remap = entry.boneRemap;
                auto* charBuf = reinterpret_cast<uint8_t*>(GuardedReadPtr(
                    reinterpret_cast<uint8_t*>(poseSource) + offsets::kOffInstBonePalette));
                auto* collBuf = reinterpret_cast<uint8_t*>(
                    GuardedReadPtr(reinterpret_cast<uint8_t*>(renderCtx) + offsets::kOffInstBonePalette));
                if (!charBuf || !collBuf) return;

                if (!entry.bbpLogDone)
                {
                    uint32_t initF = GuardedReadU32(
                        reinterpret_cast<uint8_t*>(renderCtx) + offsets::kOffInstInitFlags);
                    uint32_t xff = 0;
                    for (uint32_t bi = 0; bi < remap.count; ++bi)
                        if (remap.collToChar[bi] == 0xFF) ++xff;
                    EquipLog("  OnBBP[first] collRctx=0x%p cmoParent=0x%p nativeSource=0x%p"
                             " initF=0x%X count=%u xff=%u"
                             " map0-7=[%u %u %u %u %u %u %u %u]",
                             renderCtx, charRctx, poseSource,
                             initF, (uint32_t)remap.count, xff,
                             remap.count>0 ? (uint32_t)remap.collToChar[0] : 255u,
                             remap.count>1 ? (uint32_t)remap.collToChar[1] : 255u,
                             remap.count>2 ? (uint32_t)remap.collToChar[2] : 255u,
                             remap.count>3 ? (uint32_t)remap.collToChar[3] : 255u,
                             remap.count>4 ? (uint32_t)remap.collToChar[4] : 255u,
                             remap.count>5 ? (uint32_t)remap.collToChar[5] : 255u,
                             remap.count>6 ? (uint32_t)remap.collToChar[6] : 255u,
                             remap.count>7 ? (uint32_t)remap.collToChar[7] : 255u);
                    entry.bbpLogDone = true;
                }

                const uint32_t collBoneCount = RenderCtxBoneCountGuarded(renderCtx);
                const uint32_t charBoneCount = RenderCtxBoneCountGuarded(poseSource);
                if (!CopyRemappedBonesGuarded(collBuf, collBoneCount,
                                              charBuf, charBoneCount, remap,
                                              renderCtx, poseSource))
                {
                    EquipLog("  OnBBP(copy): guarded copy failed rctx=0x%p count=%u",
                             renderCtx, (uint32_t)remap.count);
                    const bool staleDestination =
                        GuardedReadPtr(reinterpret_cast<uint8_t*>(renderCtx) +
                                       offsets::kOffInstModel) != remap.collectionModel;
                    for (auto& sibling : entries)
                    {
                        if (sibling.renderCtx != renderCtx) continue;
                        sibling.boneRemap = {};
                        if (staleDestination) sibling.renderCtx = nullptr;
                    }
                }
                return; // renderCtx is unique per (keyBuf,attachId,texBuf) group; one copy suffices
            }
        }

        // CharacterModelFrame/PlayerModel duplicates the complete character M2 tree. Those clone
        // children never pass through CharModelSlotDispatch and therefore have new renderCtx
        // pointers absent from g_attached. DuplicateModel does preserve both shared model pointers,
        // so the (collection model, character ancestor model) pair safely identifies the clone.
        if (!cloneCandidate || !clonePoseSource) return;
        const BoneRemap& remap = cloneCandidate->boneRemap;
        auto* charBuf = reinterpret_cast<uint8_t*>(GuardedReadPtr(
            reinterpret_cast<uint8_t*>(clonePoseSource) + offsets::kOffInstBonePalette));
        auto* collBuf = reinterpret_cast<uint8_t*>(GuardedReadPtr(
            reinterpret_cast<uint8_t*>(renderCtx) + offsets::kOffInstBonePalette));
        if (!charBuf || !collBuf) return;

        const uint32_t collBoneCount = RenderCtxBoneCountGuarded(renderCtx);
        const uint32_t charBoneCount = RenderCtxBoneCountGuarded(clonePoseSource);
        if (!CopyRemappedBonesGuarded(collBuf, collBoneCount,
                                      charBuf, charBoneCount, remap,
                                      renderCtx, clonePoseSource))
            return;

        if (!cloneCandidate->cloneBbpLogDone)
        {
            EquipLog("  OnBBP[clone] collRctx=0x%p source=0x%p collModel=0x%p"
                     " charModel=0x%p count=%u",
                     renderCtx, clonePoseSource, remap.collectionModel,
                     remap.characterModel, (uint32_t)remap.count);
            cloneCandidate->cloneBbpLogDone = true;
        }
    }
    // ─── Constructor ──────────────────────────────────────────────────────────────

    EquipExtension::EquipExtension()
    {
        if (GetFileAttributesA("WarcraftXL_equip-extension.disable") != INVALID_FILE_ATTRIBUTES)
        {
//    api->Log(WXL_LOG_WARN, "equip-extension", "disabled by client flag");
    return;
        }
        on<&EquipExtension::OnItemSlotChange>(ev::Event::OnItemSlotChange);
        on<&EquipExtension::OnItemSlotClear>(ev::Event::OnItemSlotClear);
        on<&EquipExtension::OnM2SkinFinalize>(ev::Event::OnM2SkinFinalize);
        on<&EquipExtension::OnM2PerFrameUpdate>(ev::Event::OnM2PerFrameUpdate);
        on<&EquipExtension::OnBuildBonePalette>(ev::Event::OnBuildBonePalette);
        on<&EquipExtension::OnModelLoadPre>(ev::Event::OnModelLoadPre);
        // OnWeaponVisualChange / OnItemDisplayLookup are no longer subscribed here -- weapon model
        // resolution moved to WeaponExtension, which patches ItemModelData.dbc at the data level
        // instead. Nothing subscribes to either event any more: with the virtual path named
        // directly in the data, the native loader never needs a live substitution hook to catch.
        g_equipInstance = this;
    }

    // OLD: file-scope self-registering global (constructed at DLL load, before EventScript::Bind
    // could ever have run). Construction now happens inside WXL_Load(), below, after Bind() --
    // see PORTING_GUIDE.md Step 9.

    namespace
    {
    offsets::m2hooks::PerFrameUpdateFn   g_origM2PerFrame       = nullptr;
    offsets::m2hooks::BuildBonePaletteFn g_origBuildBonePalette = nullptr;

    // DIAGNOSTIC ONLY -- signature genuinely unknown. Core's own offset table documents only the
    // address and calling convention: "Take over weapon and off-hand model placement, including its
    // sheathe behaviour. __cdecl, caller-cleaned." (wxl::offsets::game::m2::kCharAddHandItem). Built
    // to find whether/which argument distinguishes mainhand from offhand at the point the native
    // client resolves this, as an alternative to the shared-virtual-key mechanism in
    // WeaponExtension.cpp, which has a known cross-character cache-poisoning risk that a real
    // attach-point-aware hook here could avoid entirely.
    //
    // Over-declaring 8 uint32_t stack args is safe for __cdecl regardless of the real count: on
    // entry, any slots beyond the real arguments just read into the *caller's* own stack frame
    // (a safe read, garbage values, never written to). On call-through, the original function's own
    // compiled code only ever reads however many args IT was built to expect -- it silently ignores
    // whatever extra values sit further up the stack, since it never has a reason to look there.
    // Only the final call-through's return value matters for correctness here (EAX passes through
    // to the real caller by being the last thing this function does, with nothing after it to
    // clobber the register) -- no return type is modeled, since it's unknown and this only needs to
    // be transparent, not to consume the result itself.
    using CharAddHandItemFn = void(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t,
                                              uint32_t, uint32_t, uint32_t, uint32_t);
    CharAddHandItemFn g_origCharAddHandItem = nullptr;

    // WOW EQUIPMENT_SLOT_* values as seen on a2 -- confirmed empirically, see the offhand-mirror
    // investigation notes (a2 tracks MainHand(15)/OffHand(16)/Ranged(17) exactly, across every
    // logged equip event). All three are now wired through this same override point -- see the
    // call site's own comment for why -- unifying what used to be two separate mechanisms (a
    // static ItemModelData.dbc patch for mainhand/ranged, a runtime hook for offhand only) into
    // one.
    constexpr uint32_t kEquipSlotMainHand = 15;
    constexpr uint32_t kEquipSlotOffHand  = 16;
    constexpr uint32_t kEquipSlotRanged   = 17;

    // Return address(es) identifying an ItemDisplayInfoLookup call site that's a real weapon
    // mainhand/offhand dispatch path -- confirmed empirically (see the offhand-mirror investigation
    // notes): fires once per hand immediately before CharAddHandItem, with this call's outBuf being
    // the exact same pointer CharAddHandItem receives as its a1.
    //
    // TWO distinct sites are known, not one: 0x006DC800 is the in-world equip path (the original
    // investigation); 0x004E3F90 is a SEPARATE dispatch site the char-select/glue-scene preview
    // character resolves weapons through instead -- confirmed from a char-select capture showing
    // the identical outBuf/a1 pointer-identity relationship, just reached via that other retAddr.
    // Both call sites carry the exact same (displayId in, resolved-record out) contract as far as
    // this stash cares, so both are accepted; without 0x004E3F90 here, char-select's dispatch calls
    // never get stashed, CharAddHandItemDetour always misses for them, and the char-select preview
    // silently keeps whatever Model1 the native lookup resolved (the mainhand model, for both
    // hands) instead of ever reaching the offhand override.
    //
    // The two known non-dispatch sites (0x00759077 sheathe-visual resolve, 0x0059800B paperdoll
    // polling) are still deliberately NOT stashed, so they can never be mistaken for a dispatch
    // call downstream. If a THIRD scene (e.g. some other preview/mirror path) turns out to reach
    // CharAddHandItem through yet another retAddr, the fix is the same shape: add it here.
    constexpr uintptr_t kWeaponDispatchRetAddrs[] = { 0x006DC800, 0x004E3F90 };

    bool IsWeaponDispatchRetAddr(void* returnAddr) noexcept
    {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(returnAddr);
        for (uintptr_t known : kWeaponDispatchRetAddrs)
            if (addr == known) return true;
        return false;
    }

    // outBuf -> displayId stash, populated by ItemDisplayInfoLookupDetour (below) for
    // kWeaponDispatchRetAddrs calls only, consumed (and erased) by CharAddHandItemDetour to recover
    // which displayId its a1 belongs to -- a1 itself carries no displayId, only the already-
    // resolved record. Erased on consumption rather than left to expire naturally: outBuf is a
    // caller stack address, so it WILL get reused by an unrelated later call, and a stale entry
    // could otherwise mis-attribute that later a1 to this displayId. Declared here, ahead of both
    // functions that touch it, so declaration order doesn't matter for either.
    std::unordered_map<void*, uint32_t> g_weaponDispatchOutBufDisplayId;

    void __cdecl CharAddHandItemDetour(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                                       uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7)
    {
        const uint32_t seq = NextDiagSeq();
        const uint32_t args[8] = { a0, a1, a2, a3, a4, a5, a6, a7 };
        EquipLog("[DIAG seq=%u tick=%u] kCharAddHandItem CALL: a0=0x%08X a1=0x%08X a2=0x%08X "
                 "a3=0x%08X a4=0x%08X a5=0x%08X a6=0x%08X a7=0x%08X",
                 seq, GetTickCount(), a0, a1, a2, a3, a4, a5, a6, a7);
        for (int i = 0; i < 8; ++i)
        {
            if (args[i] <= 0x10000 || args[i] >= 0x7fffffffu) continue; // not a plausible pointer
            __try
            {
//                const uint32_t* asStr = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(args[i]));
//                if (*asStr && std::strlen(asStr) < 128)
//                {
//                    EquipLog("    a%d as string: %08X", i, asStr);
//                }

//                const char* asStr = reinterpret_cast<const char*>(static_cast<uintptr_t>(args[i]));
//                if (*asStr && std::strlen(asStr) < 128)
//                {
//                    EquipLog("    a%d as string: '%s'", i, asStr);
//                }
//                else
//                {
                    const uint32_t* asPtr =
                        reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(args[i]));
                    EquipLog("    a%d as ptr: [0]=0x%08X [1]=0x%08X [2]=0x%08X [3]=0x%08X",
                             i, asPtr[0], asPtr[1], asPtr[2], asPtr[3]);
//                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                EquipLog("    a%d not a valid pointer", i);
            }
        }

        // Weapon model-variant override, unified across all three weapon slots -- see this file's
        // top-of-investigation notes and kEquipSlotMainHand's own doc comment. a1 is the resolved
        // ItemDisplayInfo record (the same outBuf ItemDisplayInfoLookupDetour stashed a displayId
        // for, at the dispatch call site only); a2 is the WOW equip-slot index.
        if (a2 == kEquipSlotMainHand || a2 == kEquipSlotOffHand || a2 == kEquipSlotRanged)
        {
            void* outBuf = reinterpret_cast<void*>(static_cast<uintptr_t>(a1));
            auto stashIt = g_weaponDispatchOutBufDisplayId.find(outBuf);
            if (stashIt != g_weaponDispatchOutBufDisplayId.end())
            {
                const uint32_t displayId = stashIt->second;
                g_weaponDispatchOutBufDisplayId.erase(stashIt); // one-shot -- see stash doc comment

                // Column 0 == Model1/Model1Path -- the only column CharAddHandItem's a1 record
                // needs overridden here, on any of the three slots (see the doc comments on
                // WeaponGetVirtualPath/WeaponGetOffhandVirtualPath for why Model2 never needs this
                // treatment). Persists across calls (not stack-local) since a1's Model1 field is
                // left pointing at it after this function returns, and the native call-through
                // below reads it back out of a1 synchronously -- a per-call local would be
                // dangling before that read happens. Separate buffers per slot-kind so a mainhand
                // and an offhand override landing back-to-back (e.g. two different weapons equipped
                // in the same tick) can never clobber each other's still-pending string.
                static char s_mainhandVirtualPath[280];
                static char s_offhandVirtualPath[280];

                // Offhand keeps its original, narrower behavior: ONLY override Model1 when the
                // sidecar has an explicit offhand model configured for this displayId
                // (WXLWeaponModels.csv's Model1OffhandPath) -- WeaponGetOffhandVirtualPath already
                // encodes exactly that condition, so no separate check is needed here. Mainhand and
                // ranged use the ordinary mainhand bake instead (WeaponGetVirtualPath), which mirrors
                // whatever ItemModelData.dbc's own Model1 patch already names for a baked entry.
                const bool isOffhand = (a2 == kEquipSlotOffHand);
                const bool overridden = isOffhand
                    ? wxl::scripts::weaponextension::WeaponGetOffhandVirtualPath(
                          displayId, 0, s_offhandVirtualPath, sizeof(s_offhandVirtualPath))
                    : wxl::scripts::weaponextension::WeaponGetVirtualPath(
                          displayId, 0, s_mainhandVirtualPath, sizeof(s_mainhandVirtualPath));
                char* const vpath = isOffhand ? s_offhandVirtualPath : s_mainhandVirtualPath;

                // BUG FIX (missing-model cubes on both hands): WeaponGetVirtualPath/
                // WeaponGetOffhandVirtualPath return the FULL directory-qualified vkey --
                // "item\objectcomponents\weapon\...m2" -- because that's the string
                // ItemModelData.dbc's own Model1/Model2 field needs (see BakeWeaponDisplay's doc
                // comment). But the record CharAddHandItem's a1 comes from is ItemDisplayInfo, not
                // ItemModelData -- and per every logged native lookup, ItemDisplayInfo::Model1 is
                // always a BARE FILENAME with no directory at all (e.g.
                // 'glaive_1h_blackdragonoutdoor_d_01_R_71029.mdx') -- the client prepends
                // "Item\ObjectComponents\Weapon\" itself when it turns that field into an actual
                // file request. Writing the full vkey here made the client double the prefix
                // (VirtualProvide's own MISS log showed the literal doubled path), so the request
                // never matched the single-prefixed key VPathPopulateGlobal actually registered --
                // hence cubes on both hands, since both wrote the same over-qualified string.
                // Fix: reduce vpath to its filename component (last path separator onward) before
                // handing it to the native record -- points into the same persistent buffer, so no
                // extra copy or lifetime concern.
                char* vpathFilename = vpath;
                if (overridden)
                {
                    for (char* p = vpath; *p; ++p)
                        if (*p == '\\' || *p == '/')
                            vpathFilename = p + 1;
                }

                if (overridden)
                {
                    __try
                    {
                        auto* base = reinterpret_cast<uint8_t*>(outBuf);
                        *reinterpret_cast<char**>(base + offsets::itemdisplayinfo::kOffModel1) =
                            vpathFilename;
                        EquipLog("[DIAG seq=%u] kCharAddHandItem: %s override applied, "
                                 "displayId=%u slot=%u Model1<-'%s' (full vkey='%s')", seq,
                                 isOffhand ? "offhand" : "mainhand/ranged", displayId, a2,
                                 vpathFilename, vpath);
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        EquipLog("[DIAG seq=%u] kCharAddHandItem: %s override write faulted, "
                                 "displayId=%u a1=0x%08X", seq,
                                 isOffhand ? "offhand" : "mainhand/ranged", displayId, a1);
                    }
                }
                else
                {
                    // Offhand: no sidecar Model1OffhandPath for this displayId -- per spec, skip
                    // the bake and leave Model1 as ItemModelData.dbc already resolved it.
                    // Mainhand/ranged: nothing to bake (no texture row/geoset filter) -- same
                    // outcome, leave the already-resolved record untouched.
                    EquipLog("[DIAG seq=%u] kCharAddHandItem: no sidecar %s model for displayId=%u, "
                             "leaving Model1 as-is", seq, isOffhand ? "offhand" : "mainhand/ranged",
                             displayId);
                }
            }
            else
            {
                EquipLog("[DIAG seq=%u] kCharAddHandItem: slot=%u but no stashed displayId "
                         "for a1=0x%08X (outBuf not from the expected dispatch lookup?)", seq, a2, a1);
            }
        }

        if (g_origCharAddHandItem)
            g_origCharAddHandItem(a0, a1, a2, a3, a4, a5, a6, a7);
        EquipLog("[DIAG seq=%u] kCharAddHandItem RETURN", seq);
    }

    // DIAGNOSTIC ONLY -- answers three open questions from the weapon mainhand/offhand
    // investigation:
    //   1. Does Db2.ItemDisplayInfoLookup fire at all for a weapon equip, given
    //      OnItemSlotChange apparently doesn't (see its own DIAG comment)?
    //   2. Where does it land, in call order, relative to OnItemSlotChange/OnItemSlotClear and
    //      CharAddHandItem -- before, after, nested inside one of them, or on a separate thread
    //      entirely? g_diagSeq (shared with all four) and the logged return address answer this
    //      without needing a debugger attached.
    //   3. Does the (displayId, outBuf) shape carry anything that distinguishes mainhand from
    //      offhand? Per the signature, the answer is almost certainly no -- there is no slot
    //      parameter at all, only displayId in and a record out -- but this logs the actual
    //      resolved Model1/Model2/Tex1/Tex2 strings per call so that's confirmed empirically
    //      rather than assumed from the signature alone.
    //
    // Remove once these are answered: this is a per-lookup hook on a path every equipped item
    // (armor included, not just weapons) resolves through, so it is far too hot to leave in a
    // normal build.
    offsets::itemdisplayinfo::LookupFn g_origItemDisplayLookup = nullptr;

    uint32_t __fastcall ItemDisplayInfoLookupDetour(void* storageObj, void* edx, uint32_t displayId, void* outBuf)
    {
        const uint32_t seq  = NextDiagSeq();
        const DWORD    tick = GetTickCount();
        // Return address of the call instruction that reached this detour -- lets log lines be
        // bucketed by caller (CharModelSlotDispatch vs CharAddHandItem vs anything else) even
        // without a debugger: compare this value against known function-body address ranges
        // afterward (it points just past the call site, inside whichever function issued it, not
        // at that function's own entry point, so it needs a disassembly listing to resolve to a
        // name -- it's still enough to tell two different callers apart from each other).
        void* returnAddr = _ReturnAddress();

        EquipLog("[DIAG seq=%u tick=%u] ItemDisplayInfoLookup CALL: displayId=%u storageObj=0x%p "
                 "outBuf=0x%p retAddr=0x%p", seq, tick, displayId, storageObj, outBuf, returnAddr);

        const uint32_t result = g_origItemDisplayLookup
            ? g_origItemDisplayLookup(storageObj, edx, displayId, outBuf)
            : 0;

        // Stash for CharAddHandItemDetour -- see kWeaponDispatchRetAddrs' own doc comment. Only a
        // known dispatch call site's outBuf is worth remembering; anything else stashed here would
        // just be dead weight CharAddHandItemDetour can never look up (its a1 only ever matches an
        // outBuf from one of these specific call sites).
        if (result && outBuf && IsWeaponDispatchRetAddr(returnAddr))
            g_weaponDispatchOutBufDisplayId[outBuf] = displayId;

        if (result && outBuf)
        {
            __try
            {
                auto* base = reinterpret_cast<const uint8_t*>(outBuf);
                const char* model1 = *reinterpret_cast<char* const*>(base + offsets::itemdisplayinfo::kOffModel1);
                const char* model2 = *reinterpret_cast<char* const*>(base + offsets::itemdisplayinfo::kOffModel2);
                const char* tex1   = *reinterpret_cast<char* const*>(base + offsets::itemdisplayinfo::kOffTex1);
                const char* tex2   = *reinterpret_cast<char* const*>(base + offsets::itemdisplayinfo::kOffTex2);
                EquipLog("[DIAG seq=%u] ItemDisplayInfoLookup RESULT: displayId=%u model1='%s' "
                         "model2='%s' tex1='%s' tex2='%s'", seq, displayId,
                         model1 ? model1 : "(null)", model2 ? model2 : "(null)",
                         tex1 ? tex1 : "(null)", tex2 ? tex2 : "(null)");
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                EquipLog("[DIAG seq=%u] ItemDisplayInfoLookup RESULT: displayId=%u "
                         "(outBuf read faulted)", seq, displayId);
            }
        }
        else
        {
            EquipLog("[DIAG seq=%u] ItemDisplayInfoLookup RESULT: displayId=%u NOT FOUND (result=0)",
                     seq, displayId);
        }

        return result;
    }
    }

    // Defined directly in wxl::scripts::equipextension (NOT in the anonymous namespace above) so
    // these are the same entities the friend declarations in EquipExtension.hpp refer to -- a
    // definition inside an anonymous namespace would be a distinct entity from the one the friend
    // grant was bound to, and access to the private On*/OnBuildBonePalette members would fail.
    void __fastcall M2PerFrameUpdateDetour(void* renderCtx, void* edx)
    {
        g_origM2PerFrame(renderCtx, edx);
        if (g_equipInstance)
        {
            ev::M2PerFrameUpdateArgs a{ renderCtx };
            g_equipInstance->OnM2PerFrameUpdate(a);
        }
    }

    void __fastcall BuildBonePaletteDetour(void* renderCtx, void* edx,
        void* sa1, void* sa2, void* sa3, uint32_t sa4, uint32_t sa5)
    {
        g_origBuildBonePalette(renderCtx, edx, sa1, sa2, sa3, sa4, sa5);
        if (g_equipInstance)
        {
            ev::BuildBonePaletteArgs a{ renderCtx };
            g_equipInstance->OnBuildBonePalette(a);
        }
    }

    namespace
    {
    bool InstallM2RenderHooks(const WXL_Api* api)
    {
        bool ok = true;
        ok &= api->HookAttach("wxl-equip-extension:M2PerFrameUpdate",
                              offsets::m2hooks::kM2PerFrameUpdate,
                              reinterpret_cast<void*>(&M2PerFrameUpdateDetour),
                              reinterpret_cast<void**>(&g_origM2PerFrame),
                              WXL_HOOK_DEFAULT_PRIORITY) != 0;
        ok &= api->HookAttach("wxl-equip-extension:BuildBonePalette",
                              offsets::m2hooks::kBuildBonePalette,
                              reinterpret_cast<void*>(&BuildBonePaletteDetour),
                              reinterpret_cast<void**>(&g_origBuildBonePalette),
                              WXL_HOOK_DEFAULT_PRIORITY) != 0;
        // DIAGNOSTIC ONLY -- see CharAddHandItemDetour's own comment. Was previously a raw
        // api->HookAttach against wxl::offsets::game::m2::kCharAddHandItem, a core-private header
        // this extension cannot actually #include (see WxlOffsets.hpp's own top-of-file comment on
        // why the extension-local offsets table exists at all) -- that reference doesn't compile.
        // "M2.CharAddHandItem" is registered in the core's own HookPoints.cpp table, so
        // HookAttachByName reaches the same address without needing the raw value on this side at
        // all, exactly like M2PerFrameUpdate/BuildBonePalette should eventually move to as well.
        ok &= api->HookAttachByName("M2.CharAddHandItem",
                              reinterpret_cast<void*>(&CharAddHandItemDetour),
                              reinterpret_cast<void**>(&g_origCharAddHandItem),
                              WXL_HOOK_DEFAULT_PRIORITY) != 0;
        // DIAGNOSTIC ONLY -- see ItemDisplayInfoLookupDetour's own comment. "Db2.ItemDisplayInfo-
        // Lookup" is also a named hook point in the core's own table (HookPoints.cpp), so this is
        // HookAttachByName the same way, not a raw address.
        ok &= api->HookAttachByName("Db2.ItemDisplayInfoLookup",
                              reinterpret_cast<void*>(&ItemDisplayInfoLookupDetour),
                              reinterpret_cast<void**>(&g_origItemDisplayLookup),
                              WXL_HOOK_DEFAULT_PRIORITY) != 0;
        if (!ok)
            api->Log(WXL_LOG_ERROR, "equip-extension", "M2Render: one or more hooks failed");
        return ok;
    }
    }
}

// ========== Extension entry points (new in the 1.1 extension architecture) ==========
//
// wxl-equip-extension is one DLL covering three classes (EquipExtension, CreatureExtension,
// WeaponExtension); per PORTING_GUIDE.md / QUICK_REFERENCE.md, all three bind and construct from
// this single WXL_Load(), even though their implementations live in separate .cpp files.
extern "C"
{
    const WXL_PluginInfo* __cdecl WXL_Query(void)
    {
        static const WXL_PluginInfo info = {
            sizeof(WXL_PluginInfo),
            WXL_API_VERSION,
            "wxl-equip-extension",
            1,                      // extension version (1.0)
            WXL_CLIENT_BUILD,
        };
        return &info;
    }

    int __cdecl WXL_Load(const WXL_Api* api)
    {
        if (!api || api->apiVersion != WXL_API_VERSION)
            return 0;

        // Must happen before any EventScript-derived instance is constructed.
        wxl::ext::EventScript::Bind(api);

        // Replaces VirtualPath.cpp's old self-registering detail::Registrar global (which called
        // straight into the core-internal wxl::runtime::storage::RegisterClientProvider). Installs
        // this extension's own file-I/O hooks via api->HookAttach, since WXL_Api has no dedicated
        // storage-provider registration.
        wxl::scripts::equipextension::VPathRegisterStorageProvider(api);
wxl::scripts::equipextension::InstallM2RenderHooks(api);
        // Construct all three extension classes here (equip + creature + weapon), all sharing
        // this one DLL / one WXL_Load().
        static wxl::scripts::equipextension::EquipExtension       g_equip;
        static wxl::scripts::creatureextension::CreatureExtension g_creature;
        static wxl::scripts::weaponextension::WeaponExtension     g_weapon;

        api->Log(WXL_LOG_INFO, "equip-extension", "loaded (equip + creature + weapon)");
        return 1;
    }
}
// ======================================================================================
