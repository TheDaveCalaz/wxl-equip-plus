#!/usr/bin/env python3
"""Heritage vendors for AzerothCore, written from a backport kit project.

    python make_vendors.py --work C:\\Backport\\MyProject [--armour-gold 250] [--back-gold 1000]

Every item the project has backported (the kit's own items and any looks brought back) is matched
to the race whose heritage set it belongs to. The SQL this writes:

  * locks each piece to that race (AllowableRace), as retail does;
  * prices each piece in gold and makes it sell back for nothing, so the gold leaves the economy;
  * creates two vendors - an Alliance one (990100) and a Horde one (990101) - and stocks each with
    its own races' pieces, plus any extra items listed in <work>\\vendor-extra.txt.

Output: <work>\\out\\sql\\vendors.sql. Safe to run again: it rebuilds both vendors' stock each time.
Items whose set it does not recognise are listed and left off both vendors.
"""
import argparse
import json
import os
import re
import sys

VENDOR_ALLIANCE, VENDOR_HORDE = 990100, 990101

# race, AllowableRace bit, side, words that identify its heritage sets and pieces (lower case)
RACES = [
    ("Human",     1,    "A", ["lion's heritage"]),
    ("Dwarf",     4,    "A", ["bronzebeard"]),
    ("Night Elf", 8,    "A", ["kaldorei protector"]),
    ("Gnome",     64,   "A", ["gnomeregan"]),
    ("Draenei",   1024, "A", ["telhamat", "embaari"]),
    ("Orc",       2,    "H", ["heritage of draenor"]),
    ("Undead",    16,   "H", ["forsaken", "lordaeron"]),
    ("Tauren",    32,   "H", ["shu'halo"]),
    ("Troll",     128,  "H", ["darkspear", "kevo ya siti", "lukou"]),
    ("Blood Elf", 512,  "H", ["sin'dorei"]),
]

EXTRA_TEMPLATE = """# Extra items for the heritage vendors, one line per vendor.
# List item ids after "alliance:" or "horde:", separated by commas. Lines starting with # are ignored.
# These items keep their own price and rules.
alliance:
horde:
"""


def _json(path, default=None):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def race_for(texts):
    low = " | ".join(t.lower() for t in texts if t)
    for race in RACES:
        if any(w in low for w in race[3]):
            return race
    return None


def collect(work):
    """[(item id, retail display, texts naming it)] for everything the project has backported."""
    sel = _json(os.path.join(work, "selection.json"), {}) or {}
    sets = sel.get("sets", {})
    displays = sel.get("displays", {})
    plan = _json(os.path.join(work, "build", "plan.json"), {}) or {}
    ids = (_json(os.path.join(work, "ids.json"), {}) or {}).get("item", {})
    rescued = _json(os.path.join(work, "rescued.json"), {}) or {}

    def texts(d):
        e = displays.get(str(d), {})
        out = [sets.get(str(s), {}).get("name", "") for s in e.get("sets", [])]
        out += [i.get("name", "") for i in e.get("items", [])]
        return out

    found = []
    for d, iid in ids.items():
        if plan.get(d, {}).get("ok"):
            found.append((int(iid), d, texts(d)))
    for d, rec in rescued.items():
        for iid in rec.get("items", []):
            found.append((int(iid), d, texts(d)))
    return found, plan


def read_extra(work):
    path = os.path.join(work, "vendor-extra.txt")
    if not os.path.isfile(path):
        with open(path, "w", encoding="utf-8") as f:
            f.write(EXTRA_TEMPLATE)
    extra = {"A": [], "H": []}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if ":" not in line:
                continue
            side, rest = line.split(":", 1)
            key = {"alliance": "A", "horde": "H"}.get(side.strip().lower())
            if key:
                extra[key] += [int(x) for x in re.findall(r"\d+", rest)]
    return extra, path


def write_sql(work, armour_gold=250, back_gold=1000, log=print):
    found, plan = collect(work)
    if not found:
        raise SystemExit("No backported items found in this project - run Build and Install first.")
    extra, extra_path = read_extra(work)

    by_side = {"A": [], "H": []}
    by_race, unknown = {}, []
    for iid, d, texts in sorted(found):
        race = race_for(texts)
        if not race:
            unknown.append((iid, next((t for t in texts if t), f"display {d}")))
            continue
        by_race.setdefault(race[0], []).append(iid)
        by_side[race[2]].append(iid)

    q = lambda s: "'" + s.replace("\\", "\\\\").replace("'", "''") + "'"
    out = [
        "-- Heritage vendors, written by wxl-equip-plus tools/make_vendors.py. Safe to run again.",
        f"-- Alliance vendor {VENDOR_ALLIANCE}, Horde vendor {VENDOR_HORDE}. Place them in game with .npc add <id>.",
        "",
        f"SET @PRICE_ARMOUR := {int(armour_gold * 10000)};",
        f"SET @PRICE_BACK   := {int(back_gold * 10000)};",
        "",
    ]
    for race in RACES:
        items = by_race.get(race[0])
        if not items:
            continue
        ids = ",".join(map(str, items))
        out.append(f"-- {race[0]}: {len(items)} piece(s)")
        out.append(f"UPDATE item_template SET AllowableRace = {race[1]}, SellPrice = 0, BuyCount = 1,"
                   f" BuyPrice = IF(InventoryType = 16, @PRICE_BACK, @PRICE_ARMOUR) WHERE entry IN ({ids});")
    out += [
        "",
        f"DELETE FROM creature_template       WHERE entry IN ({VENDOR_ALLIANCE}, {VENDOR_HORDE});",
        f"DELETE FROM creature_template_model WHERE CreatureID IN ({VENDOR_ALLIANCE}, {VENDOR_HORDE});",
        f"DELETE FROM npc_vendor              WHERE entry IN ({VENDOR_ALLIANCE}, {VENDOR_HORDE});",
        "-- faction 12 = Stormwind (Alliance only), 68 = Undercity (Horde only); npcflag 128 = vendor",
        "INSERT INTO creature_template (entry, name, subname, minlevel, maxlevel, faction, npcflag, unit_class, type) VALUES",
        f"  ({VENDOR_ALLIANCE}, 'Heritage Outfitter', 'Alliance Heritage', 80, 80, 12, 128, 1, 7),",
        f"  ({VENDOR_HORDE}, 'Heritage Outfitter', 'Horde Heritage', 80, 80, 68, 128, 1, 7);",
        "-- dressed as a Stormwind City Guard (68) and an Undercity guardian (5624)",
        "INSERT INTO creature_template_model (CreatureID, Idx, CreatureDisplayID, DisplayScale, Probability)",
        f"  SELECT {VENDOR_ALLIANCE}, 0, CreatureDisplayID, 1, 1 FROM creature_template_model WHERE CreatureID = 68 AND Idx = 0;",
        "INSERT INTO creature_template_model (CreatureID, Idx, CreatureDisplayID, DisplayScale, Probability)",
        f"  SELECT {VENDOR_HORDE}, 0, CreatureDisplayID, 1, 1 FROM creature_template_model WHERE CreatureID = 5624 AND Idx = 0;",
    ]
    for side, vendor in (("A", VENDOR_ALLIANCE), ("H", VENDOR_HORDE)):
        items = list(dict.fromkeys(by_side[side] + extra[side]))
        if items:
            out.append("INSERT IGNORE INTO npc_vendor (entry, slot, item, maxcount, incrtime, ExtendedCost)")
            out.append(f"  SELECT {vendor}, 0, entry, 0, 0, 0 FROM item_template WHERE entry IN ({','.join(map(str, items))});")
    out += [
        "",
        "SELECT v.entry AS vendor, i.entry AS item, i.name, i.BuyPrice / 10000 AS gold, i.AllowableRace AS races",
        f"  FROM npc_vendor v JOIN item_template i ON i.entry = v.item WHERE v.entry IN ({VENDOR_ALLIANCE}, {VENDOR_HORDE})",
        "  ORDER BY v.entry, i.name;",
    ]
    path = os.path.join(work, "out", "sql", "vendors.sql")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")

    for race in RACES:
        n = len(by_race.get(race[0], []))
        if n:
            log(f"   {race[0]:<10} {n:>3} piece(s)  -> {'Alliance' if race[2] == 'A' else 'Horde'} vendor")
    if extra["A"] or extra["H"]:
        log(f"   extra items: {len(extra['A'])} Alliance, {len(extra['H'])} Horde (from {extra_path})")
    if unknown:
        log(f"   {len(unknown)} item(s) matched no heritage set and are left off both vendors:")
        for iid, name in unknown:
            log(f"      {iid}  {name}")
    log(f"   written: {path}")
    return path, unknown


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", required=True, help="the backport kit project folder")
    ap.add_argument("--armour-gold", type=float, default=250, help="price of each armour piece (gold)")
    ap.add_argument("--back-gold", type=float, default=1000, help="price of each back piece (gold)")
    a = ap.parse_args()
    write_sql(a.work, a.armour_gold, a.back_gold)
    return 0


if __name__ == "__main__":
    sys.exit(main())
