// wxl-equip-extension: client-side virtual M2 path table.
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

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "wxl/PluginApi.h"

namespace wxl::scripts::equipextension
{
    /**
     * @brief Registers VirtualProvide with the core's storage-provider hook via the extension API
     *        table. Replaces the old self-registering global (see VirtualPath.cpp's removed
     *        detail::Registrar). MUST be called from WXL_Load(), after
     *        wxl::ext::EventScript::Bind(api) -- see EquipExtension.cpp.
     */
    void VPathRegisterStorageProvider(const WXL_Api* api);

    /**
     * @brief Builds the virtual .mdx key for a collection M2 into out[outSz].
     *
     * Format: <stem>_wxl_<sorted_geosets>_tex_<texbasename>[_grp<HEX>]_cmo<HEX>.mdx
     * geoIds are sorted ascending internally; caller need not sort them.
     * @param out         destination buffer
     * @param outSz       destination buffer size in bytes
     * @param cmo         CharModelObject pointer, used as the character identifier
     * @param realMdxPath real archive path of the collection .mdx
     * @param geoIds      geoset IDs included in the filter (unsorted)
     * @param geoCount    number of IDs
     * @param texPath     full BLP path for the texture slot (may be empty)
     * @param variantKey  optional logical-model discriminator for entries that must not share cache keys
     * @return number of characters written (excluding null), or 0 on truncation
     */
    size_t VPathBuildKey(char* out, size_t outSz, void* cmo,
                         const char* realMdxPath,
                         const uint16_t* geoIds, uint32_t geoCount,
                         const char* texPath,
                         uint32_t variantKey = 0,
                         const char* materialPatchSpec = nullptr);

    /**
     * @brief Ensures the virtual .mdx and .skin bytes are in the client serve table.
     *
     * Reads the real .mdx and its 00.skin from the archive on first call for this key;
     * subsequent calls with the same key are no-ops. Registers both paths under cmo for
     * cleanup via VPathEvictCmo.
     * @param cmo         CharModelObject pointer
     * @param realMdxPath real archive path of the collection .mdx
     * @param geoIds      geoset IDs (unsorted)
     * @param geoCount    number of IDs
     * @param texPath     full BLP path for the texture slot (may be empty)
     * @param variantKey  optional logical-model discriminator for entries that must not share cache keys
     */
    bool VPathPopulate(void* cmo, const char* realMdxPath,
                       const uint16_t* geoIds, uint32_t geoCount,
                       const char* texPath,
                       uint32_t variantKey = 0,
                       const char* materialPatchSpec = nullptr);

    /**
     * @brief Removes all virtual table entries owned by cmo.
     *
     * Call when the cmo's sceneNode goes null (character evicted from the scene).
     * @param cmo  CharModelObject pointer to evict
     */
    void VPathEvictCmo(void* cmo);

    /**
     * @brief Registers a texture/material-patched version of a model under a per-itemDisplayId
     *        VIRTUAL archive path (real path with "_<itemDisplayId>" inserted before the
     *        extension), so every future load of that virtual path -- from any character, any
     *        client, any owner -- is transparently served the patched bytes.
     *
     * Unlike VPathPopulate, this is NOT scoped to a cmo and NOT keyed by a mangled "_wxl_" name.
     * It IS still keyed by itemDisplayId, though: two different ItemDisplayInfo rows that happen
     * to point at the same underlying model file (e.g. shared weapon geometry with a
     * displayId-specific texture/material patch) each get their own distinct virtual path and
     * therefore their own table entry, rather than colliding on one shared key the way registering
     * under the bare real path would. The patch itself is still treated as a property of the
     * model file -- not of the wearer -- for a given displayId; it is not scoped to a cmo, e.g. a
     * weapon's base ModelTexture_1/2 (vanilla only auto-binds that natively for Head/Shoulder;
     * everywhere else, including weapons, something has to bake or bind it) and/or a weapon's
     * extra glow/emissive layers. There is no per-cmo eviction for these entries: once registered,
     * the override stays for the life of the process, and the first (texPath, materialPatchSpec)
     * pair registered for a given (path, itemDisplayId) wins for the rest of the session -- later
     * calls for the same pair with different arguments are no-ops (see g_globalOverrides).
     *
     * Because the served bytes now live under a virtual path rather than the model's own real
     * name, nothing will transparently intercept the native loader's request for the original
     * path any more -- the caller is responsible for getting that virtual path in front of the
     * loader instead (e.g. by pointing the resolved ItemDisplayInfo model field at it).
     * outVirtualPath, if non-null, receives that path so the caller can do so.
     *
     * No per-batch texture-slot override -- callers that need that should still go through
     * VPathPopulate on a per-attach basis. A geoset filter IS supported here (geoIds/geoCount),
     * unlike the original version of this function -- it behaves the same as VPathPopulate's own
     * geoIds/geoCount: submesh sections whose sectionId isn't in the list get their indices zeroed
     * out of the .skin bytes, geoCount == 0 means "no filter, keep every section". Like texPath and
     * materialPatchSpec, it's part of the (path, itemDisplayId) pair's first-registration-wins bake --
     * not re-appliable later with different ids for an already-registered pair.
     *
     * @param realMdxPath        real archive path of the .mdx (as ItemDisplayInfo/sidecar would give it)
     * @param itemDisplayId      ItemDisplayInfo display id this patch belongs to; mangled into the
     *                           virtual path so distinct displayIds sharing a model file don't
     *                           collide on the same table entry
     * @param texPath            full BLP path to bake into the model's replaceable/hardcoded texture
     *                           slots (OBJECT_SKIN records only, promoted to hardcoded, same as
     *                           VPathPopulate's PatchReplaceableTextureTypes step -- WEAPON_BLADE is
     *                           never touched by this and needs an explicit sidecar TextureType=3
     *                           row instead) -- pass nullptr/empty to skip this and only apply
     *                           materialPatchSpec
     * @param materialPatchSpec  batch-scoped material texture patch spec (see BuildMaterialPatchSpec) --
     *                           pass nullptr/empty to skip this and only bake texPath
     * @param geoIds             skinSectionIds to keep; every other section's indices are zeroed out of
     *                           the .skin bytes -- pass nullptr/0 (with geoCount) to skip this and keep
     *                           every section as-is
     * @param geoCount           number of entries in geoIds
     * @param evictable          if true, this bake is registered as reclaimable: once the combined
     *                           size of all evictable entries exceeds WXLExtendedEquipment.ini's
     *                           [Memory] MaxCreatureCacheMB budget, the least-recently-served
     *                           evictable entries are erased from the table (freeing their memory)
     *                           to make room, on a first-come basis as new evictable bakes come in --
     *                           see EvictOverBudget in VirtualPath.cpp. An evicted entry is NOT gone
     *                           forever: the next VirtualProvide request for it simply misses and, if
     *                           a VPathLazyResolver is registered that still recognizes it, gets
     *                           rebaked on the spot. Passing evictable=true WITHOUT also having a
     *                           lazy resolver in place for whatever id space this call uses is a
     *                           trap: the entry can still be evicted, but nothing will ever bring it
     *                           back, so any caller doing this needs its own VPathRegisterLazyResolver
     *                           registration covering the same ids (e.g. CreatureExtension's
     *                           CreatureLazyResolve for its own creature displayIds). Default false --
     *                           matches every existing caller's prior behavior (permanent for the
     *                           life of the process) unchanged.
     * @param outVirtualPath     optional destination buffer that receives the virtual path the
     *                           patched bytes were registered under (e.g. "...\\model_12345.m2")
     * @param outVirtualPathSz   size in bytes of outVirtualPath
     * @param evictionPool       which independent eviction budget this evictable=true bake counts
     *                           against, e.g. "Creature" or "Weapon". Each distinct pool name gets
     *                           its own LRU group table, its own running byte total, and its own
     *                           budget read from WXLExtendedEquipment.ini's [Memory] section under
     *                           the key "Max<Pool>CacheMB" (e.g. "MaxCreatureCacheMB",
     *                           "MaxWeaponCacheMB") -- see GetEvictableByteBudget in
     *                           VirtualPath.cpp. Pools never evict each other: a burst of weapon
     *                           bakes filling the weapon pool cannot push a creature entry out, and
     *                           vice versa, so one busy feature's cache pressure can't starve
     *                           another's. Ignored entirely when evictable is false. Defaults to
     *                           "Creature" for source compatibility with every caller written
     *                           before this parameter existed (CreatureExtension.cpp's calls, in
     *                           particular, are unchanged and keep reading MaxCreatureCacheMB
     *                           exactly as before); WeaponExtension.cpp passes "Weapon" explicitly.
     * @return true if the override was registered (or was already registered for this path/id pair)
     */
    bool VPathPopulateGlobal(const char* realMdxPath, uint32_t itemDisplayId,
                             const char* texPath, const char* materialPatchSpec,
                             const uint16_t* geoIds = nullptr, uint32_t geoCount = 0,
                             bool evictable = false,
                             char* outVirtualPath = nullptr, size_t outVirtualPathSz = 0,
                             const char* evictionPool = "Creature",
                             bool forceBake = false);
    // forceBake: bypasses the "nothing to change" guard (normally, a call with no texPath, no
    // materialPatchSpec, and no geoset filter is a no-op, since there'd be nothing to actually
    // patch versus just naming the real file directly). Set true when the caller's whole reason for
    // baking is to serve a DIFFERENT real file under its own virtual key -- e.g. an offhand-mirrored
    // weapon model with identical textures to its mainhand counterpart -- where the point isn't to
    // patch anything, just to mint a distinct virtual name pointing at different bytes on disk.

    /**
     * @brief Reads/patches byteSourcePath's bytes like VPathPopulateGlobal, but derives the virtual
     *        key from keySourcePath instead, and ALWAYS overwrites any existing entry under that
     *        key -- the only bake function here that isn't "first bake wins". Exists because this
     *        client's native weapon display resolves both mainhand and offhand attach points from
     *        the SAME static ItemModelData.dbc field, so a distinctly-keyed offhand bake is never
     *        actually read by native code; the only way to make an offhand weapon look different is
     *        for the key mainhand already resolves through to hold the offhand's bytes instead, at
     *        the moment the offhand attach happens. Must only ever be called live, per equip event
     *        (from EquipExtension's offhand handling) -- never from eager startup preload, which
     *        would permanently overwrite the shared key before anyone had equipped anything.
     * @param keySourcePath  Real path whose virtual key this bake will occupy -- normally the
     *                       MAINHAND model's own path, so the key matches what native code already
     *                       resolves through for this displayId.
     * @param byteSourcePath Real path to actually read bytes from -- normally the OFFHAND model.
     * @param itemDisplayId  Folded into the key exactly as in VPathPopulateGlobal.
     * @param texPath/materialPatchSpec/geoIds/geoCount  Same meaning as VPathPopulateGlobal, applied
     *                       to byteSourcePath's bytes before storing.
     * @param outVirtualPath/outVirtualPathSz  Receives the (shared) virtual path.
     * @return true if the bake succeeded (byteSourcePath's file was read and the key was written).
     */
    bool VPathPopulateGlobalSharedKey(const char* keySourcePath, const char* byteSourcePath,
                                      uint32_t itemDisplayId, const char* texPath,
                                      const char* materialPatchSpec, const uint16_t* geoIds,
                                      uint32_t geoCount, char* outVirtualPath, size_t outVirtualPathSz);

    /**
     * @brief Signature for a lazy-bake resolver registered via VPathRegisterLazyResolver.
     *
     * Invoked from VirtualProvide on a g_globalOverrides miss, with the already-normalized (lowercase,
     * .m2-extension) virtual path name the loader just requested. A resolver should try to decode that
     * name (see VPathDecodeGlobalVirtualKey) into a (real path, id) pair it recognizes, and if so, bake
     * it right now via VPathPopulateGlobal and return true so VirtualProvide retries the lookup. Return
     * false if this name doesn't decode to anything the resolver knows about -- the next resolver (if
     * any) gets a turn.
     */
    using VPathLazyResolver = bool (*)(const char* normalizedVirtualPathName);

    /**
     * @brief Registers a lazy-bake resolver, tried in registration order on every global-override miss.
     *
     * This is what makes genuine on-demand baking possible: unlike an eager preregister sweep (which
     * walks every sidecar-known id at startup), a resolver only ever does work for an id something
     * actually just asked for. Intended to be called once, from each consumer's own static-init
     * (mirrors RegisterClientProvider's shape in StorageHook.hpp) -- registering costs one vector
     * push_back, no I/O, so it's safe there regardless of engine subsystem init order.
     */
    void VPathRegisterLazyResolver(VPathLazyResolver resolver);

    /**
     * @brief Inverts BuildGlobalVirtualKey: decodes a virtual path of the form "<stem>_<id><ext>" back
     *        into its normalized real path ("<stem><ext>") and the id mangled into it.
     *
     * Finds the extension (text from the last '.'), then the run of ASCII digits immediately before
     * it; if that digit run is itself immediately preceded by '_', the digits decode as outDisplayId
     * and everything before that '_' plus the extension decodes as outNormPath. Returns false if the
     * name doesn't have that shape (no '.', no digit run, or no '_' before it).
     *
     * This is a syntactic decode only -- it does NOT confirm outDisplayId is one the caller actually
     * knows about, or that outNormPath is really the path that id's real model lives at. A real archive
     * path that innocently happens to end in "_<digits>.m2" decodes "successfully" too. Callers (e.g. a
     * VPathLazyResolver) must still check the decoded id against their own known-id table before
     * treating the decode as authoritative.
     * @param outNormPath    destination buffer for the decoded normalized real path
     * @param outNormPathSz  size of outNormPath in bytes
     * @param outDisplayId   destination for the decoded id
     * @return true if virtualPathName had the "<stem>_<digits><ext>" shape and both outputs were written
     */
    bool VPathDecodeGlobalVirtualKey(const char* virtualPathName, char* outNormPath, size_t outNormPathSz,
                                     uint32_t* outDisplayId);

    /**
     * @brief Reads a boolean value out of WXLExtendedEquipment.ini, sitting beside WarcraftXL.dll
     *        itself -- this module is built into WarcraftXL.dll (there is no separate
     *        WXLExtendedEquipment.dll), so in practice that means the game's own root folder.
     *
     * The ini's location is derived from WarcraftXL.dll's own path (via GetModuleHandleEx's
     * from-address lookup on this function's own code, then GetModuleFileName) -- NOT from the
     * process's current working directory, which for a game client is usually the same as the game
     * root anyway, but isn't guaranteed to be (and costs nothing extra to not rely on). "Root folder
     * of the .dll" means exactly that: the same directory WarcraftXL.dll itself lives in, sidecar
     * CSVs and all.
     *
     * Uses the standard Win32 GetPrivateProfileInt convention: any value that parses as a nonzero
     * integer is true, "0" or an unparseable value is false. If the ini file, the section, or the key
     * is missing entirely, defaultValue is returned untouched -- there is no error/warning path,
     * missing config is just "use the default", same as every other sidecar file in this module being
     * entirely optional. Implemented on top of WxlIniGetInt below.
     * @param section       ini section name, e.g. "EagerPreload"
     * @param key           key within that section, e.g. "Creatures"
     * @param defaultValue  value to return if the ini/section/key can't be found or read
     */
    bool WxlIniGetBool(const char* section, const char* key, bool defaultValue);

    /**
     * @brief Same file/section/key resolution as WxlIniGetBool, but for an arbitrary non-negative
     *        integer value (e.g. a byte/MB budget) instead of a 0-or-1 flag.
     *
     * GetPrivateProfileInt's own underlying type is unsigned, so this is only meaningful for
     * non-negative values -- callers wanting a negative sentinel should use 0 or treat "<= 0" as their
     * own "unset" convention instead (see MaxCreatureCacheMB's own <= 0 == "uncapped" convention as an
     * example).
     * @param section       ini section name, e.g. "Memory"
     * @param key           key within that section, e.g. "MaxCreatureCacheMB"
     * @param defaultValue  value to return if the ini/section/key can't be found or read
     */
    int WxlIniGetInt(const char* section, const char* key, int defaultValue);
}
