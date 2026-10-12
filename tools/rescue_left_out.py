#!/usr/bin/env python3
"""Bring back looks the backport kit could not ship - a purely 3D back piece such as a coffin, say.

The kit refuses to make an item for a look with "nothing a 3.3.5 client can draw": no flat texture, no
body region, only a retail 3D piece. With Equip Plus installed the client CAN draw it, so this tool:

  1. runs the kit on a small side project (<work>/rescue) holding only those looks, reusing the main
     project's client scan, measurements, retail tables and download cache - the kit converts every race's
     cut of the 3D piece and its icon even though it still reports the look as undrawable;
  2. gives each look its own display and item ids, from a range of its own (displays 88000+, items
     990000+) so they never meet ids the kit hands out later;
  3. stages everything Equip Plus and the server need in <work>/out/equip-plus-rescue/:
       files/      models, skins, textures, icons (archive paths)
       rows.json   the CSV rows for WXLItemDisplayModels.csv
       dbc/        ItemDisplayInfo.dbc and Item.dbc holding only the new rows (WotLK layouts)
       sql/        item_template.sql for the new items
     backport_to_equipplus.py adds these to patch-W (the tables as wxl-extended-dbc add-ons, the way custom
     mounts are added), and merge_server_dbc.py merges the rows into the server's tables.

Which looks: the ones the Backport Helper recorded as left out (<work>/helper-excluded.json), or --display.

    python rescue_left_out.py --work C:\\Backport\\MyProject --kit C:\\Tools\\wow-retail-backport

Python 3.9+, standard library only (the kit itself needs numpy and Pillow, as always).
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import backport_to_equipplus as conv  # noqa: E402

DISPLAY_START = 88000
ITEM_START = 990000
STAGE = ("out", "equip-plus-rescue")
IDI_FIELDS, ITEM_FIELDS = 25, 8
SIDE_STEPS = ("select", "fetch", "newtech", "build")
SLOT_INVENTORY = {"back": 16, "waist": 6, "chest": 5, "legs": 7, "feet": 8, "hands": 10, "wrist": 9,
                  "head": 1, "shoulder": 3, "tabard": 19, "shirt": 4}


class RescueError(Exception):
    pass


def _json(path, default=None):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        if default is None:
            raise RescueError(f"cannot read {path}")
        return default


def _save(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1)
    os.replace(tmp, path)


# --------------------------------------------------------------------------------------------------
# WDBC writer (only what the two tables need)

class DbcOut:
    def __init__(self, fields):
        self.fields = fields
        self.rows = []
        self.strings = bytearray(b"\0")
        self._idx = {b"": 0}

    def s(self, text):
        b = text.encode("utf-8")
        if b not in self._idx:
            self._idx[b] = len(self.strings)
            self.strings += b + b"\0"
        return self._idx[b]

    def add(self, values):
        if len(values) != self.fields:
            raise RescueError(f"row has {len(values)} fields, table has {self.fields}")
        self.rows.append([int(v) & 0xFFFFFFFF for v in values])

    def to_bytes(self):
        rows = sorted(self.rows, key=lambda r: r[0])
        out = bytearray(struct.pack("<4s4I", b"WDBC", len(rows), self.fields, self.fields * 4, len(self.strings)))
        for r in rows:
            out += struct.pack("<%dI" % self.fields, *r)
        return bytes(out + self.strings)


# --------------------------------------------------------------------------------------------------
# the side project

def kit_bp(kit):
    p = os.path.join(kit, "wow-retail-backport", "scripts", "bp.py")
    if not os.path.isfile(p):
        raise RescueError(f"no backport kit at {kit}")
    return p


def run_kit(kit, *args, log=print):
    bp = kit_bp(kit)
    proc = subprocess.run([sys.executable, bp, *args], cwd=os.path.dirname(bp), capture_output=True,
                          text=True, encoding="utf-8", errors="replace")
    for line in (proc.stdout + proc.stderr).splitlines():
        log("      " + line)
    return proc.returncode


def prepare_side_project(work, side):
    """A kit project sharing the main project's client state, tables and download cache."""
    main_cfg = _json(os.path.join(work, "bp.json"))
    build = main_cfg.get("build")
    if not build:
        raise RescueError("the main project has no pinned build")
    if os.path.isdir(side):
        shutil.rmtree(side)
    os.makedirs(side)
    cfg = dict(main_cfg)
    cfg["cache"] = main_cfg.get("cache") or os.path.join(work, "cache", build)
    cfg["listfile"] = main_cfg.get("listfile") or os.path.join(work, "listfile.csv")
    cfg["patch_name"] = "patch-rescue.MPQ"  # never packed; only to keep the kit from choosing a letter
    _save(os.path.join(side, "bp.json"), cfg)
    for sub in ("client", os.path.join("db2", build)):
        src = os.path.join(work, sub)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(side, sub))
    led = _json(os.path.join(work, "reports", "steps.json"), {})
    if not (led.get("scan", {}).get("ok") and led.get("measure", {}).get("ok")):
        raise RescueError("the main project's scan and measure must have passed first (build it once)")
    _save(os.path.join(side, "reports", "steps.json"),
          {"scan": led["scan"], "measure": led["measure"]})


def convert_on_side(kit, side, displays, log=print):
    sel = ["select", "--work", side]
    for d in displays:
        sel += ["--display", str(d)]
    for args in (sel, ["fetch", "--work", side], ["newtech", "--work", side],
                 ["build", "--work", side, "--extension"]):
        log(f"   -> kit {args[0]} ...")
        code = run_kit(kit, *args, log=log)
        if args[0] != "build" and code != 0:
            raise RescueError(f"the kit's {args[0]} step failed on the side project (see above)")
    # build is EXPECTED to fail ("nothing a 3.3.5 client can draw"); what matters is its export
    ext = os.path.join(side, "out", "extension", "extension.json")
    if not os.path.isfile(ext):
        raise RescueError("the kit exported no 3D pieces for these looks")
    return _json(ext)


# --------------------------------------------------------------------------------------------------
# staging

def allocate(rescued, kind, start):
    """The lowest id from `start` not yet used by any brought-back look ('display' or 'items')."""
    if kind == "items":
        used = {i for v in rescued.values() for i in v.get("items", [])}
    else:
        used = {v[kind] for v in rescued.values() if kind in v}
    n = start
    while n in used:
        n += 1
    return n


def stage(work, side, ext, log=print):
    sel = _json(os.path.join(side, "selection.json"))
    plan = _json(os.path.join(side, "build", "plan.json"), {})
    side_cfg = _json(os.path.join(side, "bp.json"))
    ns = side_cfg.get("namespace") or "Backport"
    build_dir = os.path.join(side, "build")
    stage_dir = os.path.join(work, *STAGE)
    if os.path.isdir(stage_dir):
        shutil.rmtree(stage_dir)
    files_dir = os.path.join(stage_dir, "files")
    rescued_path = os.path.join(work, "rescued.json")
    rescued = _json(rescued_path, {})
    idi, item = DbcOut(IDI_FIELDS), DbcOut(ITEM_FIELDS)
    rows, sql, problems = [], [], []

    def put(rel, data):
        dest = os.path.join(files_dir, *rel.split("\\"))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(data)

    def built(rel):
        p = os.path.join(build_dir, *rel.split("\\"))
        with open(p, "rb") as f:
            return f.read()

    for d, entry in sorted(ext.items(), key=lambda kv: int(kv[0])):
        slot = entry.get("slot")
        e = sel.get("displays", {}).get(d)
        if not e or slot not in conv.SLOTS:
            problems.append(f"look {d}: slot {slot} is not one Equip Plus draws")
            continue
        pieces = [p for p in entry.get("pieces", []) if not p.get("error")]
        if not pieces:
            problems.append(f"look {d}: the kit converted no piece")
            continue
        rec = rescued.get(d) or {}
        if "display" not in rec:
            rec["display"] = allocate(rescued, "display", DISPLAY_START)
        rescued[d] = rec
        did = rec["display"]

        # 3D pieces: same naming and placement rules as the converter
        by_stem = {}
        for p in pieces:
            stem, base = conv.equip_plus_name(p["model"], p["prefix"], p["sex"])
            by_stem.setdefault((p.get("k", 0), stem), []).append((p, base))
        for (k, stem), group in sorted(by_stem.items()):
            folder_root, attach = conv.placement(slot, any(p.get("skinned_to_retail_bones") for p, _ in group))
            folder = f"{folder_root}\\{ns}"
            geosets, needs_tex = set(), False
            for p, base in group:
                model = built(p["model"])
                skin = built(p["model"][:-3] + "00.skin")
                dest = f"Item\\ObjectComponents\\{folder}\\{base}"
                put(dest + ".m2", model)
                put(dest + "00.skin", skin)
                geosets |= conv.skin_geosets(skin)
                needs_tex |= conv.m2_uses_item_texture(model)
                for tex in conv.m2_texture_paths(model):
                    try:
                        put(tex, built(tex))
                    except OSError:
                        problems.append(f"look {d}: {base} names {tex}, which the kit did not build")
            texture_name = ""
            if needs_tex:
                fdid = None
                for t in e.get("model_tex", []) + [x for x in e.get("extra_tex", []) if x.get("type") == 2]:
                    if t.get("k", t.get("model")) == k:
                        fdid = int(t["fdid"])
                        if int(t.get("race") or 0) == 0:
                            break
                raw = None
                if fdid:
                    for c in (side_cfg.get("cache") or "",):
                        pth = os.path.join(c, str(fdid))
                        if os.path.isfile(pth):
                            with open(pth, "rb") as f:
                                raw = f.read()
                if raw is None:
                    problems.append(f"look {d}: colourway texture {fdid} not in the download cache - left out")
                    continue
                try:
                    blp = conv.blp_for_client(raw)
                except conv.ConvertError as ex:
                    problems.append(f"look {d}: texture {fdid}: {ex}")
                    continue
                texture_name = f"{conv.safe_component(ns).lower()}_{fdid}"
                put(f"Item\\ObjectComponents\\{folder}\\{texture_name}.blp", blp)
            rows.append({"DisplayID": did, "ItemID": "", "Slot": conv.SLOTS[slot], "Layer": "Base", "Side": "C",
                         "Folder": folder, "Model": f"{stem}.mdx", "Texture": texture_name,
                         "Geosets": conv.geoset_filter(slot, geosets), "Attach": attach,
                         "SuffixPolicy": "Legacy", "TexturePolicy": "Exact", "Flags": ""})

        # icon the kit wrote while building the look's row
        icon = (plan.get(d, {}).get("row") or {}).get("icon") or ""
        if icon:
            rel = f"Interface\\Icons\\{icon}.blp"
            try:
                put(rel, built(rel))
            except OSError:
                icon = ""
        icon = icon or "INV_Misc_QuestionMark"

        # display row: an icon and nothing for a stock client to draw; Equip Plus adds the model
        v = [0] * IDI_FIELDS
        v[0] = did
        v[5] = idi.s(icon)
        idi.add(v)

        # one item per retail item that wears this look
        items = e.get("items") or [{"item": 0, "name": f"Rescued look {d}", "quality": 1}]
        rec.setdefault("items", [])
        for n, it in enumerate(items):
            if n >= len(rec["items"]):
                rec["items"].append(allocate(rescued, "items", ITEM_START))
            iid = rec["items"][n]
            inv = int(it.get("inv") or SLOT_INVENTORY.get(slot, 0)) or SLOT_INVENTORY.get(slot, 0)
            cls, sub = (4, 1) if slot == "back" else (4, 0)
            item.add([iid, cls, sub, 0xFFFFFFFF, max(0, int(it.get("material") or 0)), did, inv, 0])
            sql.append((iid, cls, sub, it.get("name") or f"Rescued look {d}", did,
                        min(7, max(0, int(it.get("quality") or 1))), inv, max(0, int(it.get("material") or 0))))
        log(f"   look {d} ({slot}): display {did}, item(s) {', '.join(map(str, rec['items'][:len(items)]))}")

    if not rows:
        raise RescueError("nothing could be staged" + ("" if not problems else ": " + "; ".join(problems)))

    os.makedirs(os.path.join(stage_dir, "dbc"), exist_ok=True)
    with open(os.path.join(stage_dir, "dbc", "ItemDisplayInfo.dbc"), "wb") as f:
        f.write(idi.to_bytes())
    with open(os.path.join(stage_dir, "dbc", "Item.dbc"), "wb") as f:
        f.write(item.to_bytes())
    _save(os.path.join(stage_dir, "rows.json"), rows)
    write_sql(os.path.join(stage_dir, "sql", "item_template.sql"), sql)
    _save(rescued_path, rescued)
    return {"rows": rows, "problems": problems, "rescued": rescued}


def write_sql(path, rows):
    def q(s):
        return "'" + str(s).replace("\\", "\\\\").replace("'", "''") + "'"
    os.makedirs(os.path.dirname(path), exist_ok=True)
    lines = ["-- generated by wxl-equip-plus rescue_left_out.py: looks the backport kit could not ship,",
             "-- drawn by Equip Plus. Cosmetic copies: any class, any race, level 1, no stats.",
             "DELETE FROM `item_template` WHERE `entry` IN (%s);" % ",".join(str(r[0]) for r in rows),
             "INSERT INTO `item_template` (`entry`,`class`,`subclass`,`SoundOverrideSubclass`,`name`,`displayid`,"
             "`Quality`,`InventoryType`,`AllowableClass`,`AllowableRace`,`ItemLevel`,`RequiredLevel`,`Material`,"
             "`sheath`,`bonding`,`stackable`,`MaxDurability`) VALUES",
             ",\n".join("(%d,%d,%d,-1,%s,%d,%d,%d,-1,-1,1,0,%d,0,0,1,0)" % (i, c, s, q(n), d, qu, inv, m)
                        for i, c, s, n, d, qu, inv, m in rows) + ";"]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def rescue(work, kit, displays=None, log=print):
    work = os.path.abspath(work)
    if displays is None:
        displays = sorted(_json(os.path.join(work, "helper-excluded.json"), {}), key=int)
    if not displays:
        raise RescueError("no left-out looks to bring back")
    side = os.path.join(work, "rescue")
    log(f"Bringing back {len(displays)} look(s): {', '.join(map(str, displays))}")
    prepare_side_project(work, side)
    ext = convert_on_side(kit, side, displays, log=log)
    return stage(work, side, ext, log=log)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--work", required=True, help="the backport kit project")
    ap.add_argument("--kit", required=True, help="the backport kit folder (holds wow-retail-backport\\scripts)")
    ap.add_argument("--display", action="append", help="a retail look id (default: the helper's left-out list)")
    a = ap.parse_args(argv)
    try:
        r = rescue(a.work, a.kit, a.display)
    except RescueError as ex:
        print(f"rescue: FAIL - {ex}", file=sys.stderr)
        return 1
    for p in r["problems"]:
        print(f"  PROBLEM {p}")
    print(f"rescue: staged {len(r['rows'])} CSV row(s) in {os.path.join(os.path.abspath(a.work), *STAGE)}; "
          "build patch-W and merge the server tables to use them")
    return 2 if r["problems"] else 0


if __name__ == "__main__":
    sys.exit(main())
