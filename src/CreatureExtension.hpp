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

#pragma once
#define WXL_EXTENSION 1
#include "wxl/PluginApi.h"
#include "wxl/EventScript.hpp"

#include "engine/events/Event.hpp"

namespace wxl::scripts::creatureextension
{
    class CreatureExtension : public wxl::ext::EventScript
    {
    public:
        CreatureExtension();

    private:
        // Not model-load-related at all -- reused purely as the earliest possible, unconditional
        // trigger to kick the one-time sidecar load off. OnModelLoadPre fires for ANY model init
        // anywhere in the client (see hkM2Init in GameHooks.cpp), including the login/char-select
        // glue scene's own preview models -- pets, mounts, and weapons can all render there, well
        // before the client ever enters the actual game world. OnWorldEnter was the previous choice,
        // but it wraps CWorldEnter specifically, which never fires until character select is already
        // behind you -- too late for anything shown at the glue screen. Even a completely bare,
        // unequipped character still has to load its own base body mesh to render at all, so this
        // fires reliably regardless of gear, unlike OnItemSlotChange before it.
        //
        // This is now CreatureExtension's ONLY job: kick off sidecar loading, which itself decides
        // (via WXLExtendedEquipment.ini's [EagerPreload] Creatures key) whether to also eagerly bake
        // every sidecar-known displayId's virtual .m2 bytes into the process-lifetime override table
        // ahead of time. Either way, VirtualPath.cpp's VirtualProvide falls back to a lazy resolver
        // (CreatureLazyResolve, registered once from this class's constructor) on any displayId that
        // wasn't already baked -- decoding the exact virtual name the loader just missed on and baking
        // it on the spot, before that miss is ever handed back. CreatureModelData's ModelName field is
        // patched at the data level (outside this module) to already name the correct per-displayId
        // virtual path directly, so the native loader always just asks for the right file on its own,
        // eager or lazy, with no shared-row corruption risk and no model-instance-cache collision
        // between displayIds sharing one model file.
        void OnModelLoadPre(const wxl::events::ModelLoadArgs& a);
    };
}
