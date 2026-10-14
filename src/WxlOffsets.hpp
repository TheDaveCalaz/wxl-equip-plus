// wxl-equip-plus: engine offset names, resolved through WarcraftXL core's own offset headers.
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

// Every engine address and structure offset this extension uses, under the names its code already
// uses -- but each one now *refers to* WarcraftXL core's own definition instead of carrying a private
// copy of the number. If a core update moves or renames any of them, this extension stops compiling
// (and the weekly CI build reports it) instead of shipping a stale address that crashes in game.
//
// These live in core's offsets/ headers, which an extension may include (the build passes src/ on the
// include path); the SDK-boundary option WXL_STRICT_SDK_BOUNDARY is off by default for that reason.

#include "offsets/engine/Io.hpp"
#include "offsets/game/DB2.hpp"
#include "offsets/game/M2.hpp"

#include <cstddef>
#include <cstdint>

namespace wxl::scripts::equipextension::offsets
{
    namespace core_m2 = wxl::offsets::game::m2;

    // ---- CharModelObject / SceneNode ----
    using core_m2::kOffCmoRace;         // uint32 race id
    using core_m2::kOffCmoGender;       // uint32 gender (0 = male, 1 = female)
    using core_m2::kOffCmoSceneNode;    // -> SceneNode
    using core_m2::kOffSceneNodeOwner;  // -> CharModelObject that owns this scene node

    // ---- runtime instance object ----
    using core_m2::kOffInstInitFlags;   // bit 0 = anim init done; bit 6 = char-select present
    using core_m2::kOffInstModel;       // -> runtime model
    using core_m2::kOffInstBonePalette; // -> bone matrices, row-major 4x4
    using core_m2::kOffInstParent;      // -> parent M2 instance (null for root)
    using core_m2::kBonePaletteStride;  // one bone matrix

    // ---- runtime model object ----
    using core_m2::kOffModelPathStem;   // model path stem (no extension)
    using core_m2::kOffModelHeader;     // -> raw .m2 file buffer (parsed in place -> header)

    // ---- parsed file header ----
    using core_m2::kOffHdrBoneCount;
    using core_m2::kOffHdrBoneArray;       // -> bone records (post-fixup data ptr)
    using core_m2::kOffHdrBoneIdxLutCount; // count of bone-index-by-id LUT entries
    using core_m2::kOffHdrBoneIdxLutPtr;   // -> bone-index-by-id LUT (uint16[], by key_bone_id)

    // ---- bone record (stride kBoneStride) ----
    using core_m2::kBoneStride;
    using core_m2::kOffBoneKeyId;   // key_bone_id: canonical slot id (negative = none)
    using core_m2::kOffBoneParent;  // int16 parent index (0xFFFF = root)
    using core_m2::kOffBoneNameCrc; // CRC32 of the bone name string (name-based remap)
    using core_m2::kOffBonePivot;   // float[3] bone origin in bind space

    // ---- per-frame / bone-palette hook points ----
    namespace m2hooks
    {
        using wxl::offsets::game::m2::kM2PerFrameUpdate;
        using wxl::offsets::game::m2::kBuildBonePalette;
        using PerFrameUpdateFn   = wxl::offsets::game::m2::M2_PerFrameUpdateFn;
        using BuildBonePaletteFn = wxl::offsets::game::m2::M2_BuildBonePaletteFn;
    }

    // ---- ItemDisplayInfo runtime storage ----
    namespace itemdisplayinfo
    {
        using namespace wxl::offsets::game::db2::itemdisplayinfo; // kStorageObject, kLookup, LookupFn,
                                                                   // kRecordSize, kOffModel1/2, kOffTex1/2,
                                                                   // kOffIcon2
    }

    // ---- ChrRaces runtime storage ----
    namespace chrraces
    {
        using namespace wxl::offsets::game::db2::chrraces; // kMinId, kMaxId, kIdTable, kOffRecordPrefix
    }

    namespace genderstrings
    {
        using namespace wxl::offsets::game::db2::genderstrings; // kTable
    }

    // ---- archive file I/O ----
    namespace io
    {
        using namespace wxl::offsets::engine::io; // kOpenWholeFile, kFileOpen/Read/Size/Close and their Fn types
    }
}
