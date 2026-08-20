// wxl-weapon-extension: sidecar-driven weapon retexturing for WarcraftXL.
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
#define WXL_EXTENSION 1
#include "wxl/PluginApi.h"
#include "wxl/EventScript.hpp"

#include "engine/events/Event.hpp"

namespace wxl::scripts::weaponextension
{
    class WeaponExtension : public wxl::ext::EventScript
    {
    public:
        WeaponExtension();

    private:
        // Not model-load-related at all -- reused purely as the earliest possible, unconditional
        // trigger to kick the one-time weapon sidecar load off, exactly the same rationale as
        // CreatureExtension::OnModelLoadPre (see that class's doc comment for the full case).
        // OnModelLoadPre fires for ANY model init anywhere in the client, including the
        // login/char-select glue scene's own preview models -- pets, mounts, AND their held weapons
        // can all render there, well before the client ever enters the actual game world.
        //
        // This is WeaponExtension's ONLY job: kick off sidecar loading, which itself decides (via
        // WXLExtendedEquipment.ini's [EagerPreload] Weapons key) whether to also eagerly bake every
        // sidecar-known displayId's Model1/Model2 virtual .m2 bytes into the process-lifetime
        // override table ahead of time. Either way, VirtualPath.cpp's VirtualProvide falls back to a
        // lazy resolver (WeaponLazyResolve, registered once from this class's constructor) on any
        // displayId/column that wasn't already baked -- decoding the exact virtual name the loader
        // just missed on and baking it on the spot, before that miss is ever handed back.
        //
        // Unlike EquipExtension's older weapon path, this module never touches ItemDisplayInfo at
        // runtime and never hooks OnItemDisplayLookup: ItemModelData.dbc's Model1/Model2 fields are
        // patched at the data level (outside this module, same convention as CreatureModelData's
        // ModelName field for creatures) to already name the correct per-displayId virtual path
        // directly -- see WXLWeaponModels.csv/WXLWeaponTextures.csv's column comments in
        // WeaponExtension.cpp for the exact deterministic virtual-path shape to write there. The
        // native loader then always just asks for the right file on its own, eager or lazy, with no
        // race/gender variant handling (weapons here are never race/gender suffixed) and no
        // shared-row corruption risk between displayIds sharing one base model file.
        //
        // Bakes are registered evictable, in their own "Weapon" pool -- see VPathPopulateGlobal's
        // evictionPool parameter in VirtualPath.hpp. Budgeted independently from
        // CreatureExtension's pool via WXLExtendedEquipment.ini's [Memory] MaxWeaponCacheMB (default
        // 512 MB if unset; <= 0 means uncapped). A weapon evicted for being over budget isn't gone
        // for good: the next load of it simply misses and WeaponLazyResolve rebakes it on the spot.
        void OnModelLoadPre(const wxl::events::ModelLoadArgs& a);
    };

    /**
     * @brief Looks up the already-baked offhand-mirror virtual .m2 path for (displayId, column), if
     *        WXLWeaponModels.csv's Model1OffhandPath/Model2OffhandPath supplied one for it. Baked
     *        lazily, on first actual offhand equip (see WeaponGetOffhandVirtualPath's own doc
     *        comment in WeaponExtension.cpp for why it isn't eager-baked alongside the mainhand
     *        columns) -- by the time anything could call this after that, the answer is either
     *        already cached or the sidecar genuinely has nothing for this column. Bakes under its
     *        own distinct virtual key, separate from the mainhand bake for the same displayId/
     *        column -- the two can never collide or overwrite each other.
     * @param column 0 = Model1OffhandPath, 1 = Model2OffhandPath.
     * @return true and fills out/outSz if a baked offhand virtual path exists for this
     *         (displayId, column); false (out left untouched) if this weapon has no offhand-specific
     *         model configured at all, in which case the caller should fall back to whatever
     *         ItemModelData.dbc's Model1/Model2 already names (the mainhand bake, unchanged).
     */
    bool WeaponGetOffhandVirtualPath(uint32_t displayId, uint32_t column, char* out, size_t outSz);

    /**
     * @brief Mainhand/ranged counterpart to WeaponGetOffhandVirtualPath -- looks up the already-
     *        baked virtual .m2 path for (displayId, column) that ItemModelData.dbc's Model1/Model2
     *        field is ALSO patched to at the data level for a baked entry (see PreregisterSidecar-
     *        Weapons/BakeWeaponDisplay). Exposed here as well so the runtime attach point
     *        (CharAddHandItemDetour, EquipExtension.cpp) can apply the same override directly to
     *        the resolved record for every equip slot -- unifying mainhand, offhand, and ranged
     *        onto one mechanism instead of leaving mainhand/ranged solely dependent on the static
     *        dbc patch.
     * @param column 0 = Model1Path, 1 = Model2Path.
     * @return true and fills out/outSz if a baked virtual path exists for this (displayId, column);
     *         false (out left untouched) if this displayId/column has nothing to bake (no texture
     *         row or geoset filter -- see BakeWeaponDisplay's own doc comment), in which case the
     *         caller should leave the already-resolved record untouched.
     */
    bool WeaponGetVirtualPath(uint32_t displayId, uint32_t column, char* out, size_t outSz);
}
