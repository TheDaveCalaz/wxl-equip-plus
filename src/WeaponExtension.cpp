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

#include "WeaponExtension.hpp"
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

// VPathPopulateGlobal/VPathRegisterLazyResolver/VPathDecodeGlobalVirtualKey/WxlIniGetBool all live
// under wxl::scripts::equipextension -- historically named after their first consumer, but they were
// always a generic (path, id) -> baked-bytes mechanism, not anything armor-specific. Reused as-is via
// their current namespace, same convention CreatureExtension.cpp already follows.
using wxl::scripts::equipextension::VPathPopulateGlobal;
using wxl::scripts::equipextension::VPathRegisterLazyResolver;
using wxl::scripts::equipextension::VPathDecodeGlobalVirtualKey;
using wxl::scripts::equipextension::WxlIniGetBool;

namespace wxl::scripts::weaponextension
{
	namespace offsets = wxl::scripts::equipextension::offsets;
    namespace ev = wxl::events;

    // ─── Logging ────────────────────────────────────────────────────────────────
    // Same opt-in-file-log shape as EquipExtension's EquipLog / CreatureExtension's CreatureLog,
    // kept separate (own env var, own log file) so enabling one doesn't spam the others.
    static bool WeaponLogEnabled() noexcept
    {
        static int enabled = []() noexcept -> int {
#pragma warning(suppress: 4996)
            const char* env = std::getenv("WXL_WEAPON_LOG");
            if (env && *env && *env != '0' && *env != 'n' && *env != 'N')
                return 1;

#pragma warning(suppress: 4996)
            FILE* flag = std::fopen("WarcraftXL_weapon.log.enable", "rb");
            if (!flag) return 0;
            std::fclose(flag);
            return 1;
        }();
        return enabled != 0;
    }

    static void WeaponLog(const char* fmt, ...) noexcept
    {
        if (!WeaponLogEnabled()) return;
#pragma warning(suppress: 4996)
        FILE* f = std::fopen("WarcraftXL_weapon.log", "a");
        if (!f) return;
        va_list ap; va_start(ap, fmt);
        std::vfprintf(f, fmt, ap);
        va_end(ap);
        std::fputc('\n', f);
        std::fclose(f);
    }

    // ─── CSV helpers ────────────────────────────────────────────────────────────
    // Deliberately duplicated (not shared) from EquipExtension.cpp/CreatureExtension.cpp's
    // equivalents -- see CreatureExtension.cpp's identical comment for why.

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

    // Local copy of VirtualPath.cpp's (file-local, unreachable) NormalizeRealPath: lowercase, and
    // a trailing ".mdx" extension becomes ".m2" -- the host stores loose files as lowercase .m2, and
    // this is exactly the normalization VPathPopulateGlobal itself applies to realMdxPath before
    // mangling in the displayId, and therefore exactly what VPathDecodeGlobalVirtualKey hands back
    // as outNormPath. Needed here so WeaponLazyResolve can tell Model1Path from Model2Path apart by
    // comparing a decoded virtual name's real-path half against each column's sidecar path in the
    // same normalized form.
    static void NormalizeForCompare(char* out, size_t outSz, const char* src) noexcept
    {
        if (!out || outSz == 0) return;
        char* dst  = out;
        char* dend = out + outSz - 1;
        while (src && *src && dst < dend)
        {
            char c = *src++;
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            *dst++ = c;
        }
        *dst = '\0';

        char* lastDot = nullptr;
        for (char* p = out; *p; ++p) if (*p == '.') lastDot = p;
        if (lastDot && std::strcmp(lastDot, ".mdx") == 0 && (lastDot + 4) <= dend)
        {
            lastDot[1] = 'm'; lastDot[2] = '2'; lastDot[3] = '\0';
        }
    }

    // ─── Sidecar table ──────────────────────────────────────────────────────────
    //
    // WXLWeaponModels.csv columns: DisplayID, Model1Path, Model2Path, Geoset1, Geoset2 (Geoset1/2
    // optional). One row per DisplayID -- Model1Path/Model2Path are the real archive paths of the
    // base .mdx files that displayId's ItemDisplayInfo natively names in its ModelName_1/ModelName_2
    // fields (author-supplied, e.g. pulled from a DBC/SQL dump) -- same "known-good pair, no
    // in-memory guesswork" convention as CreatureModelPath in WXLCreatureModels.csv. Model2Path is
    // optional (many weapons are single-model; leave the column blank). Deliberately no Race/Gender
    // columns at all: weapon virtual models here are never race/gender suffixed -- one baked model
    // per DisplayID per column, full stop -- which is the whole point of moving weapons onto this
    // creature-style path instead of EquipExtension's older per-wearer race/gender-aware weapon
    // patch.
    //
    // Geoset1/Geoset2 are each parsed exactly like WXLCreatureModels.csv's own "Geoset" column (see
    // ParseWeaponGeosetSpec below) and apply only to their matching model column -- Geoset1 filters
    // Model1Path's submeshes, Geoset2 filters Model2Path's, independently. This matters more for
    // weapons than for creatures: Model2 is very often a wholly separate glow/particle mesh (see the
    // Bloodfang example in WXLWeaponModels.csv), so its geoset needs are typically nothing like
    // Model1's.
    //
    // Only DisplayIDs listed in this file are ever eligible for baking, eager or lazy -- exactly
    // the same "sidecar-only" convention as WXLCreatureModels.csv.
    struct WeaponModelPaths
    {
        std::string path[2];        // [0]=Model1Path, [1]=Model2Path; empty string = column not in use
        std::string offhandPath[2]; // [0]=Model1OffhandPath, [1]=Model2OffhandPath; empty = no
            // explicit hand-baked offhand model for this column -- BakeWeaponDisplayOffhand then
            // has no offhand model at all -- auto-mirroring was removed (crashed on equip), so an
            // explicit value here is now the only way a displayId/column gets an offhand bake.
        bool noAutoMirror = false; // NoAutoMirror column: true suppresses the auto-mirror fallback
            // above entirely for this displayId (offhandPath still wins if explicitly set). For
            // items that are inherently offhand-only and never meant to be mirrored -- shields
            // above all: a shield's Model1Path IS its offhand model already, there's no "mainhand
            // shield" this would be mirroring FROM, and mirroring its emblem/boss/heraldry would
            // just render it backwards for no reason. Defaults to false (auto-mirror stays on) so
            // existing rows are unaffected; set true on any row that occupies the OffHand slot by
            // its own nature rather than as a genuine mirrored dual-wield of a one-handed weapon.
    };
    static std::unordered_map<uint32_t, WeaponModelPaths> g_sidecarWeaponModelPath;

    // Parsed form of Geoset1/Geoset2. count == 0 means "no filter, render every geoset natively
    // present" -- same meaning as VPathPopulate/VPathPopulateGlobal's own geoCount == 0. Cap of 16
    // mirrors CreatureExtension's CreatureGeosetSpec::ids[16] / EquipExtension's GeosetFilter.ids[16].
    struct WeaponGeosetSpec
    {
        uint16_t ids[16] = {};
        uint32_t count   = 0;
    };
    struct WeaponGeosetSpecs
    {
        WeaponGeosetSpec spec[2];        // [0]=Geoset1 (for Model1Path), [1]=Geoset2 (for Model2Path)
        WeaponGeosetSpec offhandSpec[2]; // [0]=Geoset1Offhand (for Model1OffhandPath),
                                          // [1]=Geoset2Offhand (for Model2OffhandPath)
    };
    static std::unordered_map<uint32_t, WeaponGeosetSpecs> g_sidecarWeaponGeoset;

    // Parses one Geoset1/Geoset2 cell -- identical rules to CreatureExtension's
    // ParseCreatureGeosetSpec:
    //   empty            -> count=0, no filter applied, every geoset renders exactly as the model
    //                       file natively has it
    //   "0"              -> count=1, ids=[0] -- keep ONLY skinSectionId 0 (the base geoset; every
    //                       other section gets zeroed out of the .skin bytes)
    //   "xxxx,yyyy,..."  -> count=N+1, ids=[0, xxxx, yyyy, ...] -- geoset 0 is always implied
    //                       alongside whatever ids are explicitly listed, same way a weapon's own
    //                       base blade/haft geometry is never something you have to ask for
    //                       separately; an explicit "0" in the list is just a no-op duplicate of the
    //                       implied one, not a second entry
    static WeaponGeosetSpec ParseWeaponGeosetSpec(const char* spec)
    {
        WeaponGeosetSpec f = {};
        if (!spec || !*spec) return f; // empty column -- no filter, render every geoset

        f.ids[f.count++] = 0; // base geoset is always implied once any filtering is requested at all

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

    // WXLWeaponTextures.csv columns: DisplayID, ModelColumn, TextureType, TexturePath. Multiple
    // rows per (DisplayID, ModelColumn) are expected -- one per baked texture layer (e.g. base
    // skin + a separate glow/emissive layer, as in the Bloodfang example). ModelColumn is 0
    // (Model1Path) or 1 (Model2Path), matching WeaponModelPaths::path's index -- this is what lets
    // one DisplayID's two model columns each get their own independent set of baked textures
    // instead of sharing one spec. TextureType is whatever numeric texture-unit type
    // PatchTargetedMaterialTextures in VirtualPath.cpp expects (same convention as
    // WXLCreatureTextures.csv / EquipExtension's own WXLItemDisplayModelMaterials.csv). TexturePath
    // is a literal archive-relative BLP path -- given directly, same as creature texture rows.
    struct SidecarWeaponTextureEntry
    {
        uint32_t modelColumn = static_cast<uint32_t>(-1);
        uint32_t textureType = static_cast<uint32_t>(-1);
        char     texturePath[264] = {};
    };
    static std::unordered_map<uint32_t, std::vector<SidecarWeaponTextureEntry>> g_sidecarWeaponTextures;

    static bool g_sidecarLoaded = false;

    static void LoadWeaponModelSidecarFile(const char* path)
    {
        std::vector<std::string> lines;
        if (!ReadSidecarLines(path, lines)) return;

        const std::vector<std::string> header = ParseCsvLine(lines[0].c_str());
        const int cDisplay = FindCsvColumn(header, "DisplayID");
        const int cModel1  = FindCsvColumn(header, "Model1Path");
        const int cModel2  = FindCsvColumn(header, "Model2Path"); // optional -- blank column is fine
        const int cGeoset1 = FindCsvColumn(header, "Geoset1");    // optional -- absent is fine, not an error
        const int cGeoset2 = FindCsvColumn(header, "Geoset2");    // optional -- absent is fine, not an error
        const int cModel1Oh = FindCsvColumn(header, "Model1OffhandPath"); // optional -- see
        const int cModel2Oh = FindCsvColumn(header, "Model2OffhandPath"); // WeaponModelPaths::offhandPath
        const int cGeoset1Oh = FindCsvColumn(header, "Geoset1Offhand");   // optional -- see
        const int cGeoset2Oh = FindCsvColumn(header, "Geoset2Offhand");   // WeaponGeosetSpecs::offhandSpec
        const int cNoAutoMirror = FindCsvColumn(header, "NoAutoMirror");  // optional -- see
            // WeaponModelPaths::noAutoMirror. Absent column = false for every row (unchanged
            // behavior for CSVs written before this column existed).

        if (cDisplay < 0 || cModel1 < 0)
        {
            WeaponLog("weapon model sidecar '%s': missing DisplayID or Model1Path column", path);
            return;
        }

        uint32_t loaded = 0;
        for (size_t lineIndex = 1; lineIndex < lines.size(); ++lineIndex)
        {
            if (lines[lineIndex].empty()) continue;
            const std::vector<std::string> row = ParseCsvLine(lines[lineIndex].c_str());

            uint32_t displayId = 0;
            if (!ParseU32(CsvField(row, cDisplay), &displayId) || displayId == 0) continue;

            char model1[264] = {};
            CopyString(model1, sizeof(model1), CsvField(row, cModel1));
            if (!model1[0]) continue; // Model1Path is required; a row with nothing at all is useless

            // First-seen wins, same dedup convention as CreatureExtension's model-path sidecar --
            // guards against the same physical CSV being reachable (and re-parsed) via more than one
            // of LoadWeaponSidecar's search paths.
            if (g_sidecarWeaponModelPath.find(displayId) != g_sidecarWeaponModelPath.end())
                continue;

            WeaponModelPaths entry;
            entry.path[0] = model1;
            if (cModel2 >= 0)
            {
                char model2[264] = {};
                CopyString(model2, sizeof(model2), CsvField(row, cModel2));
                entry.path[1] = model2; // stays empty if the column is blank/absent
            }
            if (cModel1Oh >= 0)
            {
                char model1Oh[264] = {};
                CopyString(model1Oh, sizeof(model1Oh), CsvField(row, cModel1Oh));
                entry.offhandPath[0] = model1Oh; // stays empty if blank/absent -- no offhand
                                                   // override for column 0, mainhand bake used as-is
            }
            if (cModel2Oh >= 0)
            {
                char model2Oh[264] = {};
                CopyString(model2Oh, sizeof(model2Oh), CsvField(row, cModel2Oh));
                entry.offhandPath[1] = model2Oh;
            }
            if (cNoAutoMirror >= 0)
            {
                const std::string v = NormalizeCsvName(CsvField(row, cNoAutoMirror));
                entry.noAutoMirror = (v == "true" || v == "yes" || v == "1" || v == "shield");
            }

            g_sidecarWeaponModelPath.emplace(displayId, std::move(entry));

            if (cGeoset1 >= 0 || cGeoset2 >= 0 || cGeoset1Oh >= 0 || cGeoset2Oh >= 0)
            {
                WeaponGeosetSpecs geo;
                if (cGeoset1 >= 0) geo.spec[0] = ParseWeaponGeosetSpec(CsvField(row, cGeoset1));
                if (cGeoset2 >= 0) geo.spec[1] = ParseWeaponGeosetSpec(CsvField(row, cGeoset2));
                if (cGeoset1Oh >= 0) geo.offhandSpec[0] = ParseWeaponGeosetSpec(CsvField(row, cGeoset1Oh));
                if (cGeoset2Oh >= 0) geo.offhandSpec[1] = ParseWeaponGeosetSpec(CsvField(row, cGeoset2Oh));
                g_sidecarWeaponGeoset.emplace(displayId, geo);
            }

            ++loaded;
        }

        if (loaded)
            WeaponLog("weapon model sidecar loaded '%s' rows=%u", path, loaded);
    }

    static void LoadWeaponTextureSidecarFile(const char* path)
    {
        std::vector<std::string> lines;
        if (!ReadSidecarLines(path, lines)) return;

        const std::vector<std::string> header = ParseCsvLine(lines[0].c_str());
        const int cDisplay = FindCsvColumn(header, "DisplayID");
        const int cColumn  = FindCsvColumn(header, "ModelColumn");
        const int cTexType = FindCsvColumn(header, "TextureType");
        const int cTexPath = FindCsvColumn(header, "TexturePath");

        if (cDisplay < 0 || cColumn < 0 || cTexType < 0 || cTexPath < 0)
        {
            WeaponLog("weapon texture sidecar '%s': missing DisplayID, ModelColumn, TextureType, "
                      "or TexturePath column", path);
            return;
        }

        uint32_t loaded = 0;
        for (size_t lineIndex = 1; lineIndex < lines.size(); ++lineIndex)
        {
            if (lines[lineIndex].empty()) continue;
            const std::vector<std::string> row = ParseCsvLine(lines[lineIndex].c_str());

            uint32_t displayId = 0;
            if (!ParseU32(CsvField(row, cDisplay), &displayId) || displayId == 0) continue;

            SidecarWeaponTextureEntry e = {};
            if (!ParseU32(CsvField(row, cColumn), &e.modelColumn) || e.modelColumn > 1) continue;
            if (!ParseU32(CsvField(row, cTexType), &e.textureType)) continue;
            CopyString(e.texturePath, sizeof(e.texturePath), CsvField(row, cTexPath));
            if (!e.texturePath[0]) continue;

            // Dedup by (displayId, modelColumn, textureType), first-seen wins -- same reasoning as
            // CreatureExtension's texture sidecar dedup.
            auto& rows = g_sidecarWeaponTextures[displayId];
            bool duplicate = false;
            for (const SidecarWeaponTextureEntry& existing : rows)
            {
                if (existing.modelColumn == e.modelColumn && existing.textureType == e.textureType)
                {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            rows.push_back(e);
            ++loaded;
        }

        if (loaded)
            WeaponLog("weapon texture sidecar loaded '%s' rows=%u", path, loaded);
    }

    // Forward decl: full definition sits after BuildWeaponMaterialPatchSpec (needs it), but must be
    // called from the end of LoadWeaponSidecar below -- same shape as CreatureExtension's
    // LoadCreatureSidecar calling PreregisterSidecarCreatures at its own end.
    static void PreregisterSidecarWeapons();

    // Same search pattern as CreatureExtension's LoadCreatureSidecar / EquipExtension's
    // LoadSidecarModels: bare filename, DBFilesClient\ beside it, and DBFilesClient\ inside every
    // mounted Data\*.MPQ directory (patch overrides).
    static void LoadWeaponSidecar()
    {
        if (g_sidecarLoaded) return;
        g_sidecarLoaded = true;

        LoadWeaponModelSidecarFile("WXLWeaponModels.csv");
        LoadWeaponModelSidecarFile("DBFilesClient\\WXLWeaponModels.csv");
        LoadWeaponTextureSidecarFile("WXLWeaponTextures.csv");
        LoadWeaponTextureSidecarFile("DBFilesClient\\WXLWeaponTextures.csv");

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
                LoadWeaponModelSidecarFile((base + "WXLWeaponModels.csv").c_str());
                LoadWeaponTextureSidecarFile((base + "WXLWeaponTextures.csv").c_str());
            }
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }

        WeaponLog("weapon sidecar table ready: displays=%zu model-paths, %zu with texture rows",
                  g_sidecarWeaponModelPath.size(), g_sidecarWeaponTextures.size());

        // Deliberately called here, at the end of the CSV load, NOT from WeaponExtension's
        // constructor -- same static-init-timing reasoning as CreatureExtension's identical call
        // site: the constructor runs on the file-scope global at DLL static-init time, before the
        // client's own archive/file-I/O subsystems are guaranteed up; LoadWeaponSidecar only ever
        // runs lazily, the first time OnModelLoadPre fires.
        //
        // Reuses the same [EagerPreload] Weapons ini key EquipExtension's older weapon path already
        // reads -- conceptually the same setting ("bake known weapon displays ahead of char-select")
        // just now driving this module's bake instead (or as well, if both modules are still active
        // side by side during a migration). Default true if the ini/section/key is missing -- see
        // WxlIniGetBool's doc comment. Turning this off relies entirely on WeaponLazyResolve (see its
        // own doc comment, and its registration in the constructor below) to still bake anything at
        // all.
//        if (WxlIniGetBool("EagerPreload", "Weapons", true))
//            PreregisterSidecarWeapons();
    }

    // Builds a "TextureType=TexturePath|TextureType=TexturePath|..." spec for (displayId,
    // modelColumn), in the exact format PatchTargetedMaterialTextures (VirtualPath.cpp) expects --
    // same format CreatureExtension's BuildCreatureMaterialPatchSpec and EquipExtension's weapon
    // material sidecar both produce. Only rows whose ModelColumn matches are included, so Model1 and
    // Model2 each get an independent, non-overlapping set of baked layers from one shared
    // g_sidecarWeaponTextures[displayId] vector.
    static void BuildWeaponMaterialPatchSpec(char* out, size_t outSz, uint32_t displayId,
                                              uint32_t modelColumn)
    {
        if (!out || outSz == 0) return;
        out[0] = '\0';

        auto it = g_sidecarWeaponTextures.find(displayId);
        if (it == g_sidecarWeaponTextures.end()) return;

        size_t used = 0;
        for (const SidecarWeaponTextureEntry& e : it->second)
        {
            if (e.modelColumn != modelColumn) continue;
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

    // Cache of already-baked mainhand virtual paths, filled by BakeWeaponDisplay and read back by
    // the public WeaponGetVirtualPath accessor below. This is the SAME string ItemModelData.dbc's
    // Model1/Model2 field is patched to at the data level for a baked displayId/column (see this
    // function's own doc comment) -- exposing it here as well lets CharAddHandItemDetour
    // (EquipExtension.cpp) apply it directly to the resolved record at the runtime attach point,
    // instead of relying solely on the static dbc patch, so mainhand/ranged go through the same
    // override mechanism offhand already does (see WeaponGetVirtualPath's own doc comment for why).
    static std::unordered_map<uint64_t, std::string> g_mainhandVirtualPath;
    static uint64_t WeaponCacheKey(uint32_t displayId, uint32_t column) noexcept
    {
        return (static_cast<uint64_t>(displayId) << 8) | column;
    }

    // Bakes (displayId, modelColumn)'s patched bytes into the process-lifetime override table, if
    // the sidecar tables know a model path for that column (WXLWeaponModels.csv) and there's at
    // least one matching texture row (WXLWeaponTextures.csv) and/or a Geoset1/Geoset2 filter for
    // that column to bake. Shared by both PreregisterSidecarWeapons (eager, walks every known
    // displayId/column at startup) and WeaponLazyResolve (lazy, one displayId/column at a time, on
    // demand) so the bake logic only exists once.
    //
    // Deliberately requires at least one texture row OR a non-empty Geoset1/Geoset2 entry before
    // baking anything: a column with a known model path but nothing to actually change has nothing
    // this module would alter versus the base model, so there is no point minting a virtual copy of
    // it. If ItemModelData.dbc is pointed at a virtual name for such a column anyway, the resulting
    // VirtualProvide miss simply falls through to the client's normal archive lookup of the
    // (never-baked) virtual name and fails to load -- give that column a texture row or a geoset
    // filter, or leave ItemModelData.dbc naming the real base model instead.
    static bool BakeWeaponDisplay(uint32_t displayId, uint32_t modelColumn)
    {
        auto pathIt = g_sidecarWeaponModelPath.find(displayId);
        if (pathIt == g_sidecarWeaponModelPath.end()) return false;
        if (modelColumn > 1 || pathIt->second.path[modelColumn].empty()) return false;

        char matSpec[2048] = {};
        BuildWeaponMaterialPatchSpec(matSpec, sizeof(matSpec), displayId, modelColumn);

        const WeaponGeosetSpec* geoSpec = nullptr;
        auto geoIt = g_sidecarWeaponGeoset.find(displayId);
        if (geoIt != g_sidecarWeaponGeoset.end() && geoIt->second.spec[modelColumn].count > 0)
            geoSpec = &geoIt->second.spec[modelColumn];

        if (!matSpec[0] && !geoSpec)
        {
            WeaponLog("  bake skipped: display=%u column=%u -- no matching texture rows and no "
                      "geoset filter", displayId, modelColumn);
            return false;
        }

        // No texPath (only materialPatchSpec) -- weapon texture rows here are all TextureType-keyed,
        // same convention as creature texture rows. geoSpec (from Geoset1/Geoset2) zeroes out the
        // .skin bytes of every submesh whose skinSectionId isn't in it, same as
        // VPathPopulate/BakeCreatureDisplay's own geoset filtering -- see ParseWeaponGeosetSpec's
        // doc comment for the column's exact syntax. evictionPool="Weapon" gives weapon bakes their
        // own budget (WXLExtendedEquipment.ini's [Memory] MaxWeaponCacheMB, default 512 if unset),
        // tracked and enforced completely independently of CreatureExtension's MaxCreatureCacheMB
        // pool -- a burst of newly-equipped weapons can't evict a creature's baked model, or vice
        // versa.
        char vModelPath[280] = {};
        bool registered = VPathPopulateGlobal(pathIt->second.path[modelColumn].c_str(), displayId,
                                               nullptr, matSpec,
                                               geoSpec ? geoSpec->ids : nullptr,
                                               geoSpec ? geoSpec->count : 0,
                                               true, // evictable -- WeaponLazyResolve rebakes on miss
                                               vModelPath, sizeof(vModelPath),
                                               "Weapon");
        WeaponLog("  bake: display=%u column=%u real='%s' vpath='%s' spec='%s' geoCount=%u "
                  "registered=%d",
                  displayId, modelColumn, pathIt->second.path[modelColumn].c_str(), vModelPath,
                  matSpec, geoSpec ? geoSpec->count : 0u, registered ? 1 : 0);

        // vModelPath (when registered) is the exact string to write into ItemModelData.dbc's
        // Model1/Model2 field for this displayId/column -- lowercase, .mdx already normalized to
        // .m2, "_<displayId>" inserted before the extension (see VirtualPath.cpp's
        // BuildGlobalVirtualKey). E.g. sidecar Model1Path
        // "Item\ObjectComponents\Weapon\Bloodfang\Bloodfang.mdx" with displayId 53212 bakes to
        // "item\objectcomponents\weapon\bloodfang\bloodfang_53212.m2".
        if (registered && vModelPath[0])
        {
            g_mainhandVirtualPath[WeaponCacheKey(displayId, modelColumn)] = vModelPath;
            return true;
        }
        return false;
    }

    // Cache of already-baked offhand-mirror virtual paths, filled by BakeWeaponDisplayOffhand and
    // read back by the public WeaponGetOffhandVirtualPath accessor (EquipExtension.cpp calls that,
    // not this map, directly -- separate translation unit).
    static std::unordered_map<uint64_t, std::string> g_offhandVirtualPath;
    static uint64_t OffhandCacheKey(uint32_t displayId, uint32_t column) noexcept
    {
        return (static_cast<uint64_t>(displayId) << 8) | column;
    }

    // Offhand counterpart to BakeWeaponDisplay. Reads WeaponModelPaths::offhandPath /
    // WeaponGeosetSpecs::offhandSpec instead of the mainhand columns. Auto-mirroring (generating a
    // mirrored offhand model in-memory when WXLWeaponModels.csv gives no explicit
    // Model1OffhandPath/Model2OffhandPath) has been removed -- it was crashing on equip. Only an
    // explicit, author-supplied OffhandPath row is ever baked now; a displayId/column with no such
    // row simply keeps whatever the native client already attaches at the offhand point.
    // NoAutoMirror is therefore a no-op now (nothing left to suppress) but the column is left in
    // WeaponModelPaths/the CSV parser for now in case auto-mirroring comes back later.
    //
    // No longer uses VPathPopulateGlobalSharedKey: that mechanism existed only because, on the old
    // dbc-patch-only path, native code resolved BOTH hands from the SAME static ItemModelData.dbc
    // field, so a distinctly-keyed offhand bake was never actually read -- the offhand bytes had to
    // squat on the mainhand's own key. CharAddHandItemDetour (EquipExtension.cpp) now overrides
    // Model1 independently per hand at the runtime attach point instead, so that constraint is
    // gone: offhand bakes under its OWN virtual key here (derived from offhandPath itself, via a
    // plain VPathPopulateGlobal call), genuinely distinct from whatever mainhand's key is. This
    // also means mainhand and offhand can safely be baked/evicted/rebaked independently, with
    // neither able to silently clobber the other's registered bytes.
    static bool BakeWeaponDisplayOffhand(uint32_t displayId, uint32_t modelColumn)
    {
        auto pathIt = g_sidecarWeaponModelPath.find(displayId);
        if (pathIt == g_sidecarWeaponModelPath.end()) return false;
        if (modelColumn > 1) return false;

        if (pathIt->second.offhandPath[modelColumn].empty())
        {
            WeaponLog("  bake(offhand): display=%u column=%u no explicit OffhandPath -- "
                      "auto-mirror removed, leaving native attach as-is", displayId, modelColumn);
            return false;
        }

        char matSpec[2048] = {};
        BuildWeaponMaterialPatchSpec(matSpec, sizeof(matSpec), displayId, modelColumn);

        const WeaponGeosetSpec* geoSpec = nullptr;
        auto geoIt = g_sidecarWeaponGeoset.find(displayId);
        if (geoIt != g_sidecarWeaponGeoset.end() && geoIt->second.offhandSpec[modelColumn].count > 0)
            geoSpec = &geoIt->second.offhandSpec[modelColumn];

        char vModelPath[280] = {};
        // Keyed from the OFFHAND path itself now (not the mainhand's), so its virtual name is
        // naturally distinct from BakeWeaponDisplay's mainhand bake for the same displayId/column
        // -- different real source file in, different vkey out, no key collision possible.
        // forceBake=true: the point of this call is to mint a distinct virtual name pointing at
        // the offhand's own model bytes even when there's no texture/geoset change to actually
        // patch (see VPathPopulateGlobal's own forceBake doc comment, which describes this exact
        // offhand-mirrored-model case).
        bool registered = VPathPopulateGlobal(pathIt->second.offhandPath[modelColumn].c_str(),
                                               displayId, nullptr, matSpec,
                                               geoSpec ? geoSpec->ids : nullptr,
                                               geoSpec ? geoSpec->count : 0,
                                               true, // evictable -- WeaponLazyResolve rebakes on miss
                                               vModelPath, sizeof(vModelPath),
                                               "Weapon",
                                               true); // forceBake
        WeaponLog("  bake(offhand): display=%u column=%u real='%s' vpath='%s' spec='%s' "
                  "geoCount=%u registered=%d",
                  displayId, modelColumn, pathIt->second.offhandPath[modelColumn].c_str(),
                  vModelPath, matSpec, geoSpec ? geoSpec->count : 0u, registered ? 1 : 0);

        if (registered && vModelPath[0])
        {
            g_offhandVirtualPath[OffhandCacheKey(displayId, modelColumn)] = vModelPath;
            return true;
        }
        return false;
    }

    bool WeaponGetOffhandVirtualPath(uint32_t displayId, uint32_t column, char* out, size_t outSz)
    {
        if (!out || outSz == 0 || column > 1) return false;
        LoadWeaponSidecar(); // no-op after the first call; guards against this being called before
                              // WeaponExtension's own OnModelLoadPre has ever fired

        // Now an ordinary bake-and-cache accessor, same shape as WeaponGetVirtualPath: offhand
        // bakes under its own distinct, never-shared key (see BakeWeaponDisplayOffhand's own doc
        // comment for why the old shared-key special case is gone), so nothing else can silently
        // overwrite it between calls -- a cached "already baked" result stays correct, and
        // VPathPopulateGlobal is idempotent for an already-registered key, so re-calling
        // BakeWeaponDisplayOffhand here is just a safe, cheap no-op once baked.
        BakeWeaponDisplayOffhand(displayId, column);

        auto it = g_offhandVirtualPath.find(OffhandCacheKey(displayId, column));
        if (it == g_offhandVirtualPath.end()) return false;
        if (it->second.size() >= outSz) return false;
        std::memcpy(out, it->second.c_str(), it->second.size() + 1);
        return true;
    }

    // Mainhand/ranged counterpart to WeaponGetOffhandVirtualPath. Unifies the weapon rendering
    // pipeline: mainhand and ranged used to rely SOLELY on ItemModelData.dbc's data-level Model1/
    // Model2 patch (see OnModelLoadPre's doc comment in WeaponExtension.hpp) with nothing runtime-
    // side to call, while offhand needed a runtime override at CharAddHandItem because the native
    // client resolves both hands from that one static dbc field. This accessor lets
    // CharAddHandItemDetour (EquipExtension.cpp) apply the SAME baked virtual path to a1's Model1
    // at the runtime attach point for every slot -- mainhand and ranged included -- instead of only
    // offhand, so all three go through one mechanism rather than two.
    //
    // Simply an ordinary bake-and-cache accessor (no shared-key mechanics like the offhand
    // counterpart): a mainhand/ranged bake's virtual key is never shared with anything else, so
    // once baked it stays correct until evicted, and BakeWeaponDisplay is safe to call redundantly
    // on every invocation here (VPathPopulateGlobal is idempotent for an already-registered key).
    //
    // @param column 0 = Model1Path, 1 = Model2Path.
    // @return true and fills out/outSz if a baked virtual path exists for this (displayId, column)
    //         (i.e. the sidecar has a texture row or geoset filter for it -- see BakeWeaponDisplay's
    //         own doc comment); false (out left untouched) if this displayId/column has nothing to
    //         bake, in which case the caller should leave the already-resolved record untouched --
    //         it already names the correct base model, same as ItemModelData.dbc does statically.
    bool WeaponGetVirtualPath(uint32_t displayId, uint32_t column, char* out, size_t outSz)
    {
        if (!out || outSz == 0 || column > 1) return false;
        LoadWeaponSidecar(); // no-op after the first call; guards against this being called before
                              // WeaponExtension's own OnModelLoadPre has ever fired

        BakeWeaponDisplay(displayId, column); // no-op re-bake if already cached; see doc comment

        auto it = g_mainhandVirtualPath.find(WeaponCacheKey(displayId, column));
        if (it == g_mainhandVirtualPath.end()) return false;
        if (it->second.size() >= outSz) return false;
        std::memcpy(out, it->second.c_str(), it->second.size() + 1);
        return true;
    }


    // Walks every displayId listed in WXLWeaponModels.csv and bakes both of its columns (whichever
    // are non-empty and have a matching texture row and/or geoset filter -- see BakeWeaponDisplay)
    // into the process-lifetime override table under the exact virtual .m2 names ItemModelData.dbc's
    // Model1/Model2 fields already name for that displayId (patched at the data level, outside this
    // module -- see OnModelLoadPre's doc comment in WeaponExtension.hpp). No race to lose here: the
    // native loader always asks for that virtual name directly, on every equip/relog/char-select
    // preview, so this only needs to run once, sometime before the first such request.
    //
    // Gated behind WXLExtendedEquipment.ini's [EagerPreload] Weapons key (default true) -- see its
    // call site's comment in LoadWeaponSidecar. Deliberately sidecar-only either way: only
    // displayIds with an explicit WXLWeaponModels.csv row are ever eligible, eager or lazy.
    static void PreregisterSidecarWeapons()
    {
        // No LoadWeaponSidecar() call here -- this function is only ever invoked from the end of
        // LoadWeaponSidecar itself, so the sidecar tables below are already guaranteed populated by
        // the time we get here.
        if (g_sidecarWeaponModelPath.empty()) return;

        WeaponLog("weapon preregister: %zu display(s) with a known model path, %zu with texture rows",
                  g_sidecarWeaponModelPath.size(), g_sidecarWeaponTextures.size());

        uint32_t registeredCount = 0;
        for (const auto& [displayId, paths] : g_sidecarWeaponModelPath)
        {
            for (uint32_t col = 0; col < 2; ++col)
            {
                if (paths.path[col].empty()) continue;
                if (BakeWeaponDisplay(displayId, col)) ++registeredCount;
                // Deliberately NOT eager-baking offhand here: BakeWeaponDisplayOffhand now bakes
                // under its own distinct key (see its doc comment -- the old shared-key mainhand-
                // collision risk that used to justify this comment is gone), but eagerly baking a
                // weapon's offhand model isn't worth the startup cost for the (likely common) case
                // where that displayId never actually ends up dual-wielded. WeaponGetOffhandVirtual
                // Path bakes it lazily instead, on the first actual offhand equip that needs it.
            }
        }

        WeaponLog("weapon preregister: done, %u display/column combo(s) registered ahead of first "
                  "equip", registeredCount);
    }

    // Lazy on-demand counterpart to PreregisterSidecarWeapons, registered with VirtualPath.cpp via
    // VPathRegisterLazyResolver and invoked from VirtualProvide on a g_globalOverrides miss. Runs
    // unconditionally regardless of the [EagerPreload] Weapons ini setting -- with eager preload on,
    // this is a safety net for any displayId/column that slips past the startup sweep; with it off,
    // this is the only path that ever bakes a weapon override at all.
    //
    // normVirtualPath is whatever VirtualProvide's own NormalizeRealPath produced from the raw
    // loader request -- already lowercase with any .mdx normalized to .m2, the form
    // VPathDecodeGlobalVirtualKey expects. A successful decode only confirms the name has the right
    // *shape* (see that function's doc comment) and recovers the real base path alongside the
    // mangled-in displayId; unlike CreatureLazyResolve (one model path per displayId, no ambiguity),
    // a weapon displayId has TWO candidate columns, so the decoded real path is compared -- in the
    // same normalized form -- against both WeaponModelPaths::path entries to find out which column
    // actually matched, before BakeWeaponDisplay re-derives everything it bakes from the sidecar
    // tables (not from the decoded path itself), so a coincidental decode of an unrelated real
    // archive path can't smuggle in bytes for the wrong weapon.
    static bool WeaponLazyResolve(const char* normVirtualPath)
    {
        LoadWeaponSidecar(); // no-op after the first call; must run before the sidecar tables are read

        char decodedPath[264] = {};
        uint32_t displayId = 0;
        if (!VPathDecodeGlobalVirtualKey(normVirtualPath, decodedPath, sizeof(decodedPath), &displayId))
            return false;

        auto it = g_sidecarWeaponModelPath.find(displayId);
        if (it == g_sidecarWeaponModelPath.end())
            return false; // not a displayId this module knows about

        for (uint32_t col = 0; col < 2; ++col)
        {
            if (it->second.path[col].empty()) continue;

            char candidate[264] = {};
            NormalizeForCompare(candidate, sizeof(candidate), it->second.path[col].c_str());
            if (std::strcmp(candidate, decodedPath) != 0) continue;

            WeaponLog("  WeaponLazyResolve: miss '%s' decoded displayId=%u column=%u -- baking now",
                      normVirtualPath, displayId, col);
            return BakeWeaponDisplay(displayId, col);
        }

        return false; // decoded displayId is known, but neither column's real path matches
    }

    WeaponExtension::WeaponExtension()
    {
        // Deliberately does NOT call LoadWeaponSidecar/PreregisterSidecarWeapons here -- this
        // constructor runs on the file-scope global at DLL static-init time (DllMain), before the
        // client's own archive/file-I/O subsystems are guaranteed up.
        on<&WeaponExtension::OnModelLoadPre>(ev::Event::OnModelLoadPre);

        // Cheap (one vector push_back, no I/O) so it's safe here regardless of engine subsystem init
        // order -- see VPathRegisterLazyResolver's doc comment in VirtualPath.hpp. Registered
        // unconditionally, independent of the [EagerPreload] Weapons ini setting, so lazy baking
        // works whether or not eager preload is turned on.
        VPathRegisterLazyResolver(&WeaponLazyResolve);
    }

    // See the doc comment on the declaration in WeaponExtension.hpp for why OnModelLoadPre and not
    // OnItemSlotChange/OnWeaponVisualChange.
    void WeaponExtension::OnModelLoadPre(const ev::ModelLoadArgs&)
    {
        static bool kicked = false;
        if (kicked) return;
        kicked = true;
        LoadWeaponSidecar(); // no-op after the first call; runs PreregisterSidecarWeapons at its end
    }

    // Self-registration: file-scope instance binds handlers at DLL load via EventScript ctor.
    // OLD: file-scope self-registering global. Removed -- WeaponExtension is now constructed
    // from EquipExtension.cpp's shared WXL_Load(), after wxl::ext::EventScript::Bind(api). This
    // file defines no entry points of its own (only one WXL_Query/WXL_Load pair per DLL).
}
