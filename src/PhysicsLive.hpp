// wxl-equip-plus: live retail physics for equipped pieces (plumes, ribbons, chains).
// Copyright (C) 2026 WarcraftXL contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// The physics engine itself (physics/physics_domino.*) is Kirazul's port of the retail client's
// engine and is NOT covered by this notice; see physics/README.md.

#pragma once

#include <cstddef>
#include <cstdint>

namespace wxl::scripts::equipextension::physics_live
{
    // Off unless WXLExtendedEquipment.ini has [Physics] Enabled=1, or the F9 panel turns it on.
    void Init();
    bool Enabled();
    void SetEnabled(bool on);

    // Reads DBFilesClient\WXLEquipPlusPhysics.phy2 (written by the converter). Main thread only.
    void LoadTable();
    void ReloadTable();

    // Called right after the client built an instance's bone matrices (CM2Model, 0x0082F0F0).
    // Does nothing for a model the table has no row for. May run on the client's animate threads.
    void AfterBonePalette(void* instance);

    struct Stats
    {
        uint32_t tableRows = 0;      // rows in the loaded table
        uint32_t live = 0;           // model instances being simulated now
        uint32_t steps = 0;          // simulation steps run (all instances)
        uint32_t faults = 0;         // reads/writes that hit bad memory (instance skipped)
        uint32_t dropped = 0;        // instances forgotten after 2 s unseen
        double   lastFrameMs = 0;    // time spent in physics for the last simulated frame
        double   worstFrameMs = 0;
    };
    Stats GetStats();

    // Where diagnostics go (Equip Plus's own log). Optional.
    void SetLogger(void (*log)(const char* line));
}
