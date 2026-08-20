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

#pragma once
#define WXL_EXTENSION 1
#include "wxl/PluginApi.h"
#include "wxl/EventScript.hpp"

#include "engine/events/Event.hpp"

#include <cstdint>

namespace wxl::scripts::equipextension
{
    // Forward-declared so the friend grants below bind to these exact entities (not to new,
    // distinct functions the friend declarations would otherwise silently introduce into this
    // namespace, since M2PerFrameUpdateDetour/BuildBonePaletteDetour aren't visible yet at this
    // point in the header). Defined in EquipExtension.cpp, directly in this namespace (not in an
    // anonymous namespace, so this forward declaration and that definition refer to the same
    // function).
    void __fastcall M2PerFrameUpdateDetour(void* renderCtx, void* edx);
    void __fastcall BuildBonePaletteDetour(void* renderCtx, void* edx,
        void* sa1, void* sa2, void* sa3, uint32_t sa4, uint32_t sa5);

    class EquipExtension : public wxl::ext::EventScript
    {
    public:
        EquipExtension();

    private:
        // M2PerFrameUpdateDetour/BuildBonePaletteDetour (EquipExtension.cpp) call OnM2PerFrameUpdate/
        // OnBuildBonePalette directly via g_equipInstance -- the core's own hook for these two events
        // was dropped when GameHooks.cpp was retired during the 1.1 port (never carved into a
        // replacement file, unlike CharModelSlotDispatch/Clear which moved to CharModel.cpp), so this
        // extension installs its own raw HookAttach on kM2PerFrameUpdate/kBuildBonePalette instead of
        // relying on ev::Emit ever firing for them. See WxlOffsets.hpp's m2hooks namespace.
        friend void __fastcall M2PerFrameUpdateDetour(void* renderCtx, void* edx);
        friend void __fastcall BuildBonePaletteDetour(void* renderCtx, void* edx,
            void* sa1, void* sa2, void* sa3, uint32_t sa4, uint32_t sa5);


        void OnItemSlotChange(const wxl::events::ItemSlotChangeArgs& a);
        void OnItemSlotClear(const wxl::events::ItemSlotClearArgs& a);
        void OnM2SkinFinalize(const wxl::events::M2SkinFinalizeArgs& a);
        void OnM2PerFrameUpdate(const wxl::events::M2PerFrameUpdateArgs& a);
        void OnBuildBonePalette(const wxl::events::BuildBonePaletteArgs& a);

        // OnWeaponVisualChange and OnItemDisplayLookup formerly lived here, driving the old runtime
        // weapon model-swap path (native ItemDisplayInfo lookup + live Model1/Model2 substitution).
        // Both are removed: WeaponExtension (WeaponExtension.hpp/.cpp) now owns weapon model
        // resolution entirely, patching ItemModelData.dbc at the data level instead, the same
        // convention CreatureModelData already uses for creatures. This class no longer subscribes
        // to either event -- see EquipExtension.cpp's constructor.

        // Not model-load-related at all -- reused purely as the earliest possible, unconditional
        // trigger to kick the one-time armor sidecar load off. OnModelLoadPre fires for ANY model
        // init anywhere in the client, including the login/char-select glue scene's own preview
        // models, well before OnItemSlotChange ever fires for the char-select preview character
        // (see CreatureExtension.hpp's identical reasoning -- same fix, same rationale, applied
        // here for consistency). LoadSidecarModels is idempotent (no-op after the first call), so
        // this is purely additive: the existing OnItemSlotChange call site stays in place as a
        // redundant safety net.
        void OnModelLoadPre(const wxl::events::ModelLoadArgs& a);
    };
}
