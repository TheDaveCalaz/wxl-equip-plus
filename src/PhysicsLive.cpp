// wxl-equip-plus: live retail physics for equipped pieces (plumes, ribbons, chains).
// Copyright (C) 2026 WarcraftXL contributors
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
// The glue between the 3.3.5a client and Kirazul's port of retail's physics engine
// (physics/physics_domino.*, not covered by this notice). Written from the kit's
// references/client-extension.md section 5 and references/rules.md section 6c:
//
//   * right after the client builds a model instance's bone matrices (0x0082F0F0), a model whose
//     file has a row in the physics table is stepped once per frame;
//   * bone world = palette bone x the scene matrix (scene + 0xC4, a MIRROR: never decomposed);
//     the placement = the instance's own matrix (instance + 0xF4) x the scene matrix;
//   * each dynamic body's model-space bone goes back as bone x (instance + 0xF4), and every bone
//     under a moved one is carried by its parent's delta;
//   * time comes from the scene's clock (scene + 0x0C, ms); a second build in one frame takes 0 s;
//   * state per instance, forgotten when unseen for 2 s; every client read and write is guarded.

#include "PhysicsLive.hpp"
#include "VirtualPath.hpp"     // WxlIniGetBool
#include "WxlOffsets.hpp"
#include "game/Io.hpp"
#include "physics/physics_domino.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::scripts::equipextension::physics_live
{
    namespace phys = wow::ec::physics;
    namespace core_m2 = wxl::offsets::game::m2;

    namespace
    {
        // client layout (wxl-core offsets where core names them; the rest from the kit's notes)
        constexpr size_t kOffInstScene   = core_m2::kOffInstScene;      // instance -> its scene
        constexpr size_t kOffInstShared  = core_m2::kOffInstShared;     // instance -> shared model
        constexpr size_t kOffInstPalette = core_m2::kOffInstBonePalette;// instance -> bone matrices
        constexpr size_t kOffInstOwnMat  = core_m2::kOffInstViewRoot;   // instance's own matrix (inline)
        constexpr size_t kOffSceneClock  = core_m2::kOffSceneClock;     // scene clock, ms
        constexpr size_t kOffSceneMatrix = 0xC4;                        // scene matrix (inline, mirror)
        constexpr size_t kOffModelStem   = core_m2::kOffModelPathStem;  // inline path stem
        constexpr size_t kOffModelHeader = core_m2::kOffModelHeader;    // -> parsed .m2
        constexpr size_t kOffHdrBones    = core_m2::kOffHdrBoneCount;   // u32 count, then -> records
        constexpr size_t kOffHdrBoneArr  = core_m2::kOffHdrBoneArray;
        constexpr size_t kBoneStride     = core_m2::kBoneStride;        // 0x58
        constexpr size_t kOffBoneParent  = core_m2::kOffBoneParent;     // int16
        constexpr size_t kOffBonePivot   = core_m2::kOffBonePivot;      // float[3]
        constexpr uint32_t kMaxBones     = 256;
        constexpr uint32_t kForgetMs     = 2000;

        const char* const kTablePath = "DBFilesClient\\WXLEquipPlusPhysics.phy2";

        struct InstState
        {
            void*                            shared = nullptr;
            std::unique_ptr<phys::Instance>  sim;
            uint32_t lastClock = 0;       // scene clock at the last step
            uint32_t lastSeen  = 0;       // GetTickCount at the last build
            LARGE_INTEGER stillSince{};   // when the scene clock stopped moving
            LARGE_INTEGER lastQpc{};
            bool     haveClock = false;
        };

        std::mutex g_lock;
        bool g_enabled = false;
        bool g_inited = false;
        std::vector<uint8_t> g_table;                                     // the PHY2 blob
        std::unordered_map<void*, std::shared_ptr<phys::Model>> g_models; // shared -> row (null: none)
        std::unordered_map<void*, std::string> g_modelStems;              // shared -> stem it was keyed by
        std::unordered_map<void*, InstState> g_inst;
        Stats g_stats;
        uint32_t g_lastSweep = 0;
        void (*g_log)(const char*) = nullptr;
        LARGE_INTEGER g_qpcFreq{};

        void Log(const char* fmt, ...)
        {
            if (!g_log) return;
            char buf[512];
            va_list ap;
            va_start(ap, fmt);
            std::vsnprintf(buf, sizeof(buf), fmt, ap);
            va_end(ap);
            g_log(buf);
        }

        // ---- guarded client memory access (no C++ objects inside __try) ----
        bool SafeCopy(void* dst, const void* src, size_t n) noexcept
        {
            __try { std::memcpy(dst, src, n); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool SafeReadPtr(const void* at, void** out) noexcept { return SafeCopy(out, at, sizeof(void*)); }

        // The instance's inputs, copied out of client memory in one guarded pass.
        struct Snapshot
        {
            void*    shared = nullptr;
            float*   palette = nullptr;
            uint32_t boneCount = 0;
            uint32_t clock = 0;
            float    sceneMat[16];
            float    ownMat[16];
            char     stem[260];
        };

        int ReadSnapshotStage(void* inst, Snapshot& s) noexcept
        {
            // The shared model and its path first, so a later failure can name the model.
            uint8_t* i = static_cast<uint8_t*>(inst);
            void* scene = nullptr;
            void* header = nullptr;
            void* boneArr = nullptr;
            s.stem[0] = 0;
            if (!SafeReadPtr(i + kOffInstShared, &s.shared) || !s.shared) return 1;
            uint8_t* sh = static_cast<uint8_t*>(s.shared);
            if (!SafeCopy(s.stem, sh + kOffModelStem, sizeof(s.stem) - 1)) { s.stem[0] = 0; return 11; }
            s.stem[sizeof(s.stem) - 1] = 0;
            if (!SafeReadPtr(sh + kOffModelHeader, &header) || !header) return 4;
            uint8_t* hd = static_cast<uint8_t*>(header);
            if (!SafeCopy(&s.boneCount, hd + kOffHdrBones, 4)) return 5;
            if (s.boneCount < 2 || s.boneCount > kMaxBones) return 6;   // one bone: nothing can swing
            if (!SafeReadPtr(i + kOffInstScene, &scene) || !scene) return 2;
            if (!SafeReadPtr(i + kOffInstPalette, reinterpret_cast<void**>(&s.palette)) || !s.palette) return 3;
            if (!SafeReadPtr(hd + kOffHdrBoneArr, &boneArr) || !boneArr) return 7;
            if (!SafeCopy(&s.clock, static_cast<uint8_t*>(scene) + kOffSceneClock, 4)) return 8;
            if (!SafeCopy(s.sceneMat, static_cast<uint8_t*>(scene) + kOffSceneMatrix, sizeof(s.sceneMat))) return 9;
            if (!SafeCopy(s.ownMat, i + kOffInstOwnMat, sizeof(s.ownMat))) return 10;
            return 0;
        }

        bool ReadBones(const Snapshot& s, void* boneArr, float* bones, int16_t* parents, phys::V3* pivots) noexcept
        {
            if (!SafeCopy(bones, s.palette, size_t(s.boneCount) * 64)) return false;
            uint8_t* b = static_cast<uint8_t*>(boneArr);
            for (uint32_t k = 0; k < s.boneCount; ++k)
            {
                if (!SafeCopy(&parents[k], b + k * kBoneStride + kOffBoneParent, 2)) return false;
                if (!SafeCopy(&pivots[k], b + k * kBoneStride + kOffBonePivot, 12)) return false;
            }
            return true;
        }

        void* BoneArray(void* shared) noexcept
        {
            void* header = nullptr;
            void* arr = nullptr;
            if (!SafeReadPtr(static_cast<uint8_t*>(shared) + kOffModelHeader, &header) || !header) return nullptr;
            if (!SafeReadPtr(static_cast<uint8_t*>(header) + kOffHdrBoneArr, &arr)) return nullptr;
            return arr;
        }

        // ---- the table ----
        std::shared_ptr<phys::Model> FindRow(const char* stem)
        {
            if (g_table.empty() || !stem || !stem[0]) return nullptr;
            // The stem may or may not carry the folder and the extension: try it as it is, then
            // as a .m2, then its file name alone (the converter writes a row under each).
            std::string base = stem;
            const char* candidates[3] = {};
            std::string withExt = base;
            const size_t dot = base.find_last_of('.');
            const size_t slash = base.find_last_of("\\/");
            if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) withExt += ".m2";
            std::string name = withExt.substr(withExt.find_last_of("\\/") == std::string::npos
                                                  ? 0 : withExt.find_last_of("\\/") + 1);
            candidates[0] = base.c_str();
            candidates[1] = withExt.c_str();
            candidates[2] = name.c_str();
            for (const char* c : candidates)
            {
                char norm[300];
                if (!phys::NormPath(c, norm, sizeof(norm))) continue;
                auto m = std::make_shared<phys::Model>();
                if (phys::Find(g_table.data(), g_table.size(), norm, *m)) return m;
            }
            return nullptr;
        }

        void SweepLocked(uint32_t now)
        {
            if (now - g_lastSweep < 500) return;
            g_lastSweep = now;
            for (auto it = g_inst.begin(); it != g_inst.end();)
            {
                if (now - it->second.lastSeen > kForgetMs) { it = g_inst.erase(it); ++g_stats.dropped; }
                else ++it;
            }
            g_stats.live = uint32_t(g_inst.size());
        }
    }

    void SetLogger(void (*log)(const char*)) { g_log = log; }

    void Init()
    {
        if (g_inited) return;
        g_inited = true;
        QueryPerformanceFrequency(&g_qpcFreq);
        g_enabled = WxlIniGetBool("Physics", "Enabled", false);
        Log("physics: %s (WXLExtendedEquipment.ini [Physics] Enabled)", g_enabled ? "on" : "off");
    }

    bool Enabled() { return g_enabled; }

    void SetEnabled(bool on)
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_enabled = on;
        g_inst.clear();   // parts go back to their pose next frame
        g_stats.live = 0;
    }

    void LoadTable()
    {
        namespace io = wxl::game::io;
        std::vector<uint8_t> blob;
        void* handle = nullptr;
        if (io::FileOpen(kTablePath, wxl::scripts::equipextension::offsets::io::kOpenWholeFile, &handle) && handle)
        {
            uint32_t high = 0;
            const uint32_t size = io::FileSize(handle, &high);
            if (size >= 8 && high == 0)
            {
                blob.resize(size);
                uint32_t got = 0;
                io::FileRead(handle, blob.data(), size, &got);
                if (got != size) blob.clear();
            }
            io::FileClose(handle);
        }
        uint32_t rows = 0;
        if (blob.size() >= 8 && std::memcmp(blob.data(), "PHY2", 4) == 0) std::memcpy(&rows, blob.data() + 4, 4);
        else blob.clear();

        std::lock_guard<std::mutex> g(g_lock);
        g_table.swap(blob);
        g_models.clear();
        g_modelStems.clear();
        g_inst.clear();
        g_stats.tableRows = rows;
        Log("physics: table %s (%u rows)", g_table.empty() ? "not found" : kTablePath, rows);
    }

    void ReloadTable() { LoadTable(); }

    Stats GetStats()
    {
        std::lock_guard<std::mutex> g(g_lock);
        return g_stats;
    }

    void AfterBonePalette(void* instance)
    {
        if (!g_enabled || !instance || g_table.empty()) return;

        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);

        Snapshot s;
        std::vector<float> before;
        std::vector<int16_t> parents;
        std::vector<phys::V3> pivots;
        static volatile long calls = 0;
        if (InterlockedIncrement(&calls) == 1) Log("physics: first bone build seen (hook is live)");
        const int stage = ReadSnapshotStage(instance, s);
        if (stage)
        {
            // Name each model once (by its shared data) with the stage that stopped it; one-bone
            // models (stage 6) are the common, harmless case and are only counted.
            if (stage == 6 && s.stem[0])
            {
                std::lock_guard<std::mutex> g(g_lock);
                static std::unordered_map<void*, int> told;
                if (told.size() < 400 && told.emplace(s.shared, stage).second)
                    Log("physics: skip '%s' (%u bone(s): nothing to swing)", s.stem, s.boneCount);
            }
            else if (s.shared)
            {
                std::lock_guard<std::mutex> g(g_lock);
                static std::unordered_map<void*, int> told;
                if (told.size() < 400 && told.emplace(s.shared, stage).second)
                    Log("physics: skip '%s' at read stage %d", s.stem[0] ? s.stem : "(no path)", stage);
            }
            return;
        }

        std::lock_guard<std::mutex> g(g_lock);
        const uint32_t now = GetTickCount();
        SweepLocked(now);

        // the shared model's row, looked up once (and again if the slot was reused by another file)
        std::shared_ptr<phys::Model> model;
        auto mi = g_models.find(s.shared);
        if (mi == g_models.end() || g_modelStems[s.shared] != s.stem)
        {
            model = FindRow(s.stem);
            g_models[s.shared] = model;
            g_modelStems[s.shared] = s.stem;
            static int listed = 0;
            if (model) Log("physics: MATCH '%s' (%u bones): %zu bodies, %zu joints", s.stem, s.boneCount,
                           model->bodies.size(), model->joints.size());
            else if (listed < 300) { ++listed; Log("physics: no row for '%s' (%u bones)", s.stem, s.boneCount); }
        }
        else model = mi->second;
        if (!model) return;

        void* boneArr = BoneArray(s.shared);
        if (!boneArr) { ++g_stats.faults; return; }
        before.resize(size_t(s.boneCount) * 16);
        parents.resize(s.boneCount);
        pivots.resize(s.boneCount);
        if (!ReadBones(s, boneArr, before.data(), parents.data(), pivots.data())) { ++g_stats.faults; return; }
        for (const phys::BodyDef& b : model->bodies)
            if (b.bone != 0xFFFF && b.bone >= s.boneCount)
            {
                static int warned = 0;
                if (warned++ < 20) Log("physics: row for '%s' names bone %u but the file has %u", s.stem, b.bone, s.boneCount);
                return;   // row does not fit this file
            }

        InstState& st = g_inst[instance];
        if (st.shared != s.shared || !st.sim)
        {
            st = InstState{};
            st.shared = s.shared;
            st.sim = std::make_unique<phys::Instance>(model.get());
            g_models[s.shared] = model;   // keep the row alive while an instance uses it
        }
        st.lastSeen = now;

        // the frame's time: the scene clock; a second build in one frame takes 0 s; a clock standing
        // still for 0.1 s (paused scenes, UI models) falls back to the high-resolution clock
        float dt = 0;
        if (st.haveClock)
        {
            if (s.clock != st.lastClock)
            {
                dt = float(s.clock - st.lastClock) * 0.001f;
                st.stillSince = t0;
            }
            else if (g_qpcFreq.QuadPart && double(t0.QuadPart - st.stillSince.QuadPart) / g_qpcFreq.QuadPart > 0.1)
            {
                dt = float(double(t0.QuadPart - st.lastQpc.QuadPart) / g_qpcFreq.QuadPart);
            }
        }
        else st.stillSince = t0;
        if (dt < 0 || dt > 1.0f) dt = 0;   // clock wrapped or the instance was away
        st.haveClock = true;
        st.lastClock = s.clock;
        st.lastQpc = t0;

        // inputs
        std::vector<double> world(size_t(s.boneCount) * 16);
        for (uint32_t k = 0; k < s.boneCount; ++k)
            phys::World(&before[size_t(k) * 16], s.sceneMat, &world[size_t(k) * 16]);
        phys::Frame f;
        f.boneWorld = reinterpret_cast<const double (*)[16]>(world.data());
        f.pivots = pivots.data();
        f.boneCount = s.boneCount;
        phys::World(s.ownMat, s.sceneMat, f.placement);
        f.dt = dt;
        st.sim->Step(f);
        if (dt > 0 && ++g_stats.steps == 1) Log("physics: first simulation step ran ('%s')", s.stem);

        // outputs: each dynamic body's bone, then everything under a moved bone
        std::vector<float> bones(before);
        std::vector<uint8_t> movedBytes(s.boneCount, 0);
        bool any = false;
        for (size_t b = 0; b < model->bodies.size(); ++b)
        {
            const uint16_t bone = model->bodies[b].bone;
            if (bone >= s.boneCount) continue;
            double M[16];
            if (!st.sim->Output(b, M)) continue;
            phys::FromModel(M, s.ownMat, &bones[size_t(bone) * 16]);
            movedBytes[bone] = 1;
            any = true;
        }
        if (!any) return;
        std::unique_ptr<bool[]> moved(new bool[s.boneCount]);
        for (uint32_t k = 0; k < s.boneCount; ++k) moved[k] = movedBytes[k] != 0;
        phys::Carry(parents.data(), s.boneCount, before.data(), bones.data(), moved.get());
        for (float v : bones)
            if (!(v == v)) { ++g_stats.faults; g_inst.erase(instance); return; }   // never write a NaN
        if (!SafeCopy(s.palette, bones.data(), bones.size() * sizeof(float)))
        {
            ++g_stats.faults;
            g_inst.erase(instance);
            return;
        }

        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        if (g_qpcFreq.QuadPart)
        {
            const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / g_qpcFreq.QuadPart;
            g_stats.lastFrameMs = ms;
            if (ms > g_stats.worstFrameMs) g_stats.worstFrameMs = ms;
        }
        g_stats.live = uint32_t(g_inst.size());
    }
}
