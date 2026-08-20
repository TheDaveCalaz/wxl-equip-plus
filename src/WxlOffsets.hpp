// wxl-equip-extension: hardcoded offset table for the extension build.
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
//
// Per OFFSET_HANDLING.md's "Option 2: Hardcode Offset Values" -- extensions cannot #include the
// core's private offsets/game/*.hpp / offsets/engine/*.hpp headers, so these constants are copied
// out of them directly (offsets/game/M2.hpp, offsets/game/DB2.hpp, offsets/engine/Io.hpp), verified
// against wxl-core-1.1's own src/offsets/ copy of the same tables -- every value used below is
// byte-identical between the two. Target client: WoW 3.3.5a build 12340 (WXL_CLIENT_BUILD); these
// are only valid for that exact build.

#pragma once

#include <cstdint>
#include <cstddef>

namespace wxl::scripts::equipextension::offsets
{
    // ---- from offsets/game/M2.hpp: CharModelObject / SceneNode ----
    constexpr size_t kOffCmoRace        = 0x18; // uint32 race id
    constexpr size_t kOffCmoGender      = 0x1C; // uint32 gender (0 = male, 1 = female)
    constexpr size_t kOffCmoSceneNode   = 0x38; // -> SceneNode
    constexpr size_t kOffSceneNodeOwner = 0x28; // -> CharModelObject that owns this scene node

    // ---- from offsets/game/M2.hpp: runtime instance object fields ----
    constexpr size_t kOffInstInitFlags  = 0x10; // bit 0 = anim init done; bit 6 = char-select present
    constexpr size_t kOffInstModel      = 0x2C; // -> runtime model
    constexpr size_t kOffInstBonePalette= 0x98; // -> bone matrices, row-major 4x4
    constexpr size_t kOffInstParent     = 0x48; // -> parent M2 instance (null for root); verified
                                                 // against the core's M2.hpp static_assert on
                                                 // offsetof(M2Instance, parent)
    constexpr size_t kBonePaletteStride = 0x40; // one bone matrix

    // ---- from offsets/game/M2.hpp: runtime model object fields ----
    constexpr size_t kOffModelPathStem  = 0x3C;  // model path stem (no extension)
    constexpr size_t kOffModelHeader    = 0x150; // -> raw .m2 file buffer (parsed in place -> header)

    // ---- from offsets/game/M2.hpp: parsed file-header fields ----
    constexpr size_t kOffHdrBoneCount       = 0x2C;
    constexpr size_t kOffHdrBoneArray       = 0x30; // -> bone records (post-fixup data ptr)
    constexpr size_t kOffHdrBoneIdxLutCount = 0xF8; // count of bone-index-by-id LUT entries
    constexpr size_t kOffHdrBoneIdxLutPtr   = 0xFC; // -> bone-index-by-id LUT (uint16[], by key_bone_id)

    // ---- from offsets/game/M2.hpp: bone record fields (stride kBoneStride) ----
    constexpr size_t kBoneStride     = 0x58;
    constexpr size_t kOffBoneKeyId   = 0x00; // key_bone_id: canonical slot id (negative = none)
    constexpr size_t kOffBoneParent  = 0x08; // int16 parent index (0xFFFF = root)
    constexpr size_t kOffBoneNameCrc = 0x0C; // CRC32 of the bone name string (name-based remap)

    // ---- from offsets/game/M2.hpp: per-frame / bone-palette hook points ----
    // The core's own hook for these two events was dropped when GameHooks.cpp was retired during
    // the 1.1 port (see PORTING_GUIDE.md) and never carved into a replacement file, unlike
    // CharModelSlotDispatch/Clear which moved to CharModel.cpp. EquipExtension.cpp installs its
    // own raw HookAttach on these addresses instead of relying on ev::Emit(OnM2PerFrameUpdate /
    // OnBuildBonePalette, ...) ever firing.
    namespace m2hooks
    {
        constexpr uintptr_t kM2PerFrameUpdate = 0x00828A00;
        constexpr uintptr_t kBuildBonePalette = 0x0082F0F0;

        using PerFrameUpdateFn   = void(__fastcall*)(void* renderCtx, void* edx);
        using BuildBonePaletteFn = void(__fastcall*)(void* renderCtx, void* edx,
                                                       void* sa1, void* sa2, void* sa3,
                                                       uint32_t sa4, uint32_t sa5);
    }

    // ---- from offsets/game/DB2.hpp: ItemDisplayInfo ----
    namespace itemdisplayinfo
    {
        constexpr uintptr_t kStorageObject = 0x00AD3DDC; // storage instance
        // sub_4cfd90: thiscall(ecx=storageObj, displayId, outBuf); fills outBuf with the 256-byte
        // record copy (field pointers point into the live DBC string block); returns non-zero if
        // found. Historically called directly (not detoured) by WeaponExtension.cpp's own code --
        // that usage is unaffected. Also now attached as a TEMPORARY diagnostic hook via
        // api->HookAttachByName("Db2.ItemDisplayInfoLookup", ...) in EquipExtension.cpp
        // (ItemDisplayInfoLookupDetour), to investigate whether/when this fires for weapon slots --
        // see that function's doc comment. kLookup itself is unused by that hook (HookAttachByName
        // resolves the address by name, not from this constant); it's kept here only for the
        // existing direct-call use and kStorageObject below.
        constexpr uintptr_t kLookup = 0x004CFD90;
        using LookupFn = uint32_t(__fastcall*)(void* storageObj, void* edx, uint32_t displayId, void* outBuf);

        constexpr size_t kRecordSize = 256; // byte stride between records in the storage array
        constexpr size_t kOffModel1  = 0x04; // char* primary model filename
        constexpr size_t kOffModel2  = 0x08; // char* secondary model filename (left/right variant)
        constexpr size_t kOffTex1    = 0x0C; // char* primary texture name
        constexpr size_t kOffTex2    = 0x10; // char* secondary texture name
        constexpr size_t kOffIcon2   = 0x18; // char* icon2 string (raw pointer only)
    }

    // ---- from offsets/game/DB2.hpp: ChrRaces (compacted storage, indexed by id - minId) ----
    // kMinId/kMaxId/kIdTable are ABSOLUTE ADDRESSES of core globals, dereferenced directly --
    // not plain integer values -- exactly as the old code used them
    // (*reinterpret_cast<uint32_t*>(kMinId), etc). Keep them as uintptr_t, not size_t.
    namespace chrraces
    {
        constexpr uintptr_t kMinId          = 0x00AD3438; // -> i32 minimum race id in the table
        constexpr uintptr_t kMaxId          = 0x00AD3434; // -> i32 maximum race id
        constexpr uintptr_t kIdTable        = 0x00AD3448; // -> record* table, indexed by (id - minId)
        constexpr size_t    kOffRecordPrefix= 0x18;        // char[4] race client prefix (e.g. "Hum")
    }

    // ---- from offsets/game/DB2.hpp: gender strings table ----
    // Also an absolute address: char*[2] (index 0 = "Male", index 1 = "Female").
    namespace genderstrings
    {
        constexpr uintptr_t kTable = 0x00AC46A0;
    }

    // ---- from offsets/engine/Io.hpp ----
    namespace io
    {
        constexpr uint32_t kOpenWholeFile = 0x20000; // Open flag: load the whole file into the buffer

        // Archive file-I/O primitives (all callee-cleaned / __stdcall except kMopaqOpenArchive).
        // Needed by VirtualPath.cpp's own HookAttach-based file serving -- see its doc comment for
        // why an extension has to hook these directly instead of registering a client-provider
        // callback the way the old in-core module did (there is no such registration in
        // wxl/PluginApi.h's WXL_Api table; RegisterClientProvider is core-internal).
        constexpr uintptr_t kFileOpen  = 0x00424B50; // (archiveOrNull, name, flags, &handle) -> nonzero
        constexpr uintptr_t kFileRead  = 0x00422530; // (handle, dst, len, &read|0, 0, 0) -> nonzero
        constexpr uintptr_t kFileSize  = 0x004218C0; // (handle, &sizeHigh) -> file size low dword
        constexpr uintptr_t kFileClose = 0x00422910; // (handle)

        using Storage_FileOpenFn  = int(__stdcall*)(void* archive, const char* name, uint32_t flags, void** out);
        using Storage_FileReadFn  = int(__stdcall*)(void* handle, void* dst, uint32_t len, uint32_t* read, void* ovl, uint32_t unk);
        using Storage_FileSizeFn  = uint32_t(__stdcall*)(void* handle, uint32_t* sizeHigh);
        using Storage_FileCloseFn = int(__stdcall*)(void* handle);
    }
}
