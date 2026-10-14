#!/usr/bin/env python3
"""Turn a wow-retail-backport project's Tier 2 export into something Equip Plus draws.

The backport kit (https://github.com/Kirazul/wow-retail-backport) converts a retail set for a stock
3.3.5a client and, with `build --extension`, also exports the retail 3D pieces a stock client cannot
show (belt buckles, boot shells, gauntlet cuffs, robe skirts) as `out/extension/extension.json` plus
`out/extension/<patch>-ext.MPQ`. Those files follow the kit's own naming and carry no texture for the
item's colourway, so Equip Plus cannot use them as they are.

This tool reads that export and writes an open-folder patch, `Data/patch-<L>.MPQ/`, holding:

  * every piece model and its skin, renamed to the `<stem>_<Race><Sex>.m2` form Equip Plus resolves;
  * the textures those models name, and each display's colourway texture (looked up in the kit's
    own resolved selection and taken from its download cache, trimmed to the client's size limit);
  * `DBFilesClient/WXLItemDisplayModels.csv`, one row per piece, keyed by the display ids the kit
    assigned (`ids.json`), so no DBC is edited.

Run it after the kit's `pack` step:

    python backport_to_equipplus.py --work C:\\Backport\\MyProject

and copy the folder it reports into the client's `Data` folder. By default only belts are converted:
they are race-agnostic models on the belt-buckle point and fit every body. The other pieces are skinned
to retail's HD skeletons and only fit HD bodies; add `--armor-pieces` to include them.

Python 3.9+, standard library only. Nothing is downloaded: everything comes from the kit's project.
"""
import argparse
import csv
import io
import json
import os
import re
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mpqread import MpqReader  # noqa: E402
import physics_table  # noqa: E402

CSV_HEADER = ["DisplayID", "ItemID", "Slot", "Layer", "Side", "Folder", "Model", "Texture", "Geosets",
              "Attach", "SuffixPolicy", "TexturePolicy", "Flags"]

# kit slot name -> Equip Plus slot name
SLOTS = {"waist": "Waist", "chest": "Chest", "legs": "Legs", "feet": "Feet", "hands": "Hands",
         "wrist": "Wrist", "back": "Back"}

# How a piece is attached depends on the piece, not just its slot:
#  * a piece skinned to retail's bones (boot shells, gauntlets, a skirt, most modern belts) is a COLLECTION:
#    Equip Plus binds it to the body's skeleton by bone name, so it bends with the body;
#  * a rigid, race-agnostic piece (Lionguard's buckle) hangs on one attachment point. Equip Plus allows
#    rigid models on the waist (belt buckle, 53) and back (12); on the other body slots it only draws
#    collections, so rigid pieces there go the collection route too. Collections hang on the character
#    root (19), as Equip Plus's own sample CSVs do: on any other point the character window's copy of the
#    piece is not fitted and floats behind the character.
RIGID_ATTACH = {"waist": ("Waist", "53"), "back": ("Cape", "12")}


def placement(slot, skinned):
    """(folder under Item\\ObjectComponents, Attach column) for one piece."""
    if not skinned and slot in RIGID_ATTACH:
        return RIGID_ATTACH[slot]
    return "Collections", "19"   # the character root: Equip Plus fits a collection to the body by bone name

MAX_TEXTURE_EDGE = 1024  # the 3.3.5a client's limit, as the kit uses


class ConvertError(Exception):
    pass


# --------------------------------------------------------------------------------------------------
# kit project

def load_json(path, default=None):
    if not os.path.exists(path):
        if default is not None:
            return default
        raise ConvertError(f"missing {path} - run the backport kit's steps up to `pack` first")
    with open(path, encoding="utf-8") as f:
        return json.load(f)


class KitProject:
    def __init__(self, work):
        self.work = os.path.abspath(work)
        self.cfg = load_json(self.path("bp.json"))
        self.build = self.cfg.get("build") or ""
        self.ns = self.cfg.get("namespace") or "Backport"
        self.extension = load_json(self.path("out", "extension", "extension.json"))
        self.selection = load_json(self.path("selection.json"), {"displays": {}})
        ids = load_json(self.path("ids.json"))
        self.display_ids = {str(k): int(v) for k, v in (ids.get("display") or {}).items()}
        ext_dir = self.path("out", "extension")
        mpqs = [f for f in os.listdir(ext_dir) if f.lower().endswith("-ext.mpq")]
        if not mpqs:
            raise ConvertError(f"no *-ext.MPQ in {ext_dir} - run the kit's `build --extension` and `pack`")
        self.ext_mpq = os.path.join(ext_dir, sorted(mpqs)[0])
        cache = self.cfg.get("cache") or ""
        self.cache_dirs = [d for d in (os.path.join(cache, self.build) if cache else "", cache,
                                       self.path("cache", self.build), self.path("cache")) if d]

    def path(self, *parts):
        return os.path.join(self.work, *parts)

    def retail_file(self, fdid):
        for d in self.cache_dirs:
            p = os.path.join(d, str(int(fdid)))
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    return f.read()
        return None

    def colourway_texture(self, retail_display, k, race_id):
        """The texture retail puts in a piece model's item-texture slot (type 2) for this display."""
        e = (self.selection.get("displays") or {}).get(str(retail_display)) or {}
        rows = [t for t in e.get("model_tex", []) if t.get("k") == k]
        rows += [t for t in e.get("extra_tex", []) if t.get("model") == k and t.get("type") == 2]
        for want in (race_id, 0):
            for t in rows:
                if int(t.get("race") or 0) == want:
                    return int(t["fdid"])
        return int(rows[0]["fdid"]) if rows else None


# --------------------------------------------------------------------------------------------------
# M2 / BLP helpers

def m2_texture_paths(data):
    """Hard-coded (type 0) texture paths an MD20 model names."""
    if data[:4] != b"MD20":
        raise ConvertError("not an MD20 model")
    n, off = struct.unpack_from("<II", data, 0x50)
    out = []
    for i in range(n):
        typ, _flags, ln, lo = struct.unpack_from("<IIII", data, off + i * 16)
        if typ == 0 and ln:
            out.append(data[lo:lo + ln].rstrip(b"\0").decode("latin-1"))
    return out


# Geoset groups (geoset id // 100) each slot's 3D piece may draw. Retail cuts one outfit file per race and
# leaves it to each item to show only its own groups; when the kit hands over a cut that still holds other
# slots' parts (seen on the Forsaken heritage gloves and chestguard: belt, boots and gloves inside the
# chest file), Equip Plus would draw the whole outfit for each piece. A Geosets filter on the CSV row
# keeps each piece to its own groups.
SLOT_GEOSET_GROUPS = {
    "chest": {8, 10, 13, 22, 28},   # sleeves, undershirt, robe skirt, chest attachments, chest 28xx
    "legs": {9, 11, 13},             # kneepads, trousers, robe-length legs
    "feet": {5, 20},                 # boots, boot attachments
    "hands": {4, 23},                # gloves, hand attachments
    "wrist": {8},                    # sleeves / bracers
    "waist": {18},                   # belt
    "back": {15},                    # cloak
}


def skin_geosets(skin):
    """Geoset ids (skin section ids) a .skin draws."""
    if skin[:4] != b"SKIN":
        return set()
    n, off = struct.unpack_from("<II", skin, 4 + 8 * 3)
    return {struct.unpack_from("<H", skin, off + i * 48)[0] for i in range(n)}


def geoset_filter(slot, ids):
    """Geosets column for a piece: '' when the model holds only this slot's parts, else the ids to keep."""
    groups = SLOT_GEOSET_GROUPS.get(slot)
    if not groups or not ids:
        return ""
    keep = {g for g in ids if g // 100 in groups}
    if not keep or keep == {g for g in ids if g != 0}:
        return ""
    return ",".join(str(g) for g in sorted(keep))


def m2_uses_item_texture(data):
    n, off = struct.unpack_from("<II", data, 0x50)
    return any(struct.unpack_from("<I", data, off + i * 16)[0] == 2 for i in range(n))


def blp_for_client(data):
    """A retail BLP2 the 3.3.5a client can read: mip levels above 1024 px dropped, header rebuilt.

    Handles the palettised and DXT encodings retail item textures use. Anything else (raw ARGB, or a
    non-BLP2 file) is refused with an error rather than guessed at.
    """
    if data[:4] != b"BLP2":
        raise ConvertError("not a BLP2 texture")
    typ, enc = struct.unpack_from("<IB", data, 4)
    w, h = struct.unpack_from("<II", data, 12)
    if typ != 1 or enc not in (1, 2):
        raise ConvertError(f"BLP encoding {enc} (type {typ}) needs re-encoding, which this tool does not do")
    offs = list(struct.unpack_from("<16I", data, 20))
    sizes = list(struct.unpack_from("<16I", data, 84))
    chain = []
    for i in range(16):
        if not (offs[i] and sizes[i]) or offs[i] + sizes[i] > len(data):
            break
        chain.append(i)
    if not chain or chain[0] != 0:
        raise ConvertError("BLP has no usable top mip level")
    drop = 0
    while (w >> drop) > MAX_TEXTURE_EDGE or (h >> drop) > MAX_TEXTURE_EDGE:
        drop += 1
    keep = chain[drop:]
    if not keep:
        raise ConvertError(f"BLP has no mip level at or below {MAX_TEXTURE_EDGE} px")
    head = bytearray(data[:148])
    struct.pack_into("<II", head, 12, max(1, w >> drop), max(1, h >> drop))
    palette = (data[148:148 + 1024] + bytes(1024))[:1024]
    body = bytearray()
    new_offs, new_sizes = [0] * 16, [0] * 16
    for j, i in enumerate(keep):
        new_offs[j] = 148 + 1024 + len(body)
        new_sizes[j] = sizes[i]
        body += data[offs[i]:offs[i] + sizes[i]]
    struct.pack_into("<16I", head, 20, *new_offs)
    struct.pack_into("<16I", head, 84, *new_sizes)
    head[11] = 1 if len(keep) > 1 else 0  # has-mips flag
    return bytes(head) + palette + bytes(body)


# --------------------------------------------------------------------------------------------------
# naming

def equip_plus_name(kit_model_path, prefix, sex):
    """Kit:  ...\\<stem>_<Prefix><Sex>__src__s<hash>.m2   ->   ( <stem>__src__s<hash> , file base )

    Equip Plus builds `<Model stem>_<Race><Sex>.m2`, so the race and sex move to the end.
    """
    base = kit_model_path.replace("/", "\\").split("\\")[-1]
    if not base.lower().endswith(".m2"):
        raise ConvertError(f"unexpected model name {base}")
    base = base[:-3]
    token = f"_{prefix}{sex}"
    at = base.rfind(token)
    if at < 0:
        raise ConvertError(f"{base}: no {token} in the name")
    stem = base[:at] + base[at + len(token):]
    # Retail cuts per-sex pieces into files named "..._male..." / "..._female...". Equip Plus already
    # adds the sex as a suffix, so drop the word when it matches the piece's sex: both sexes then share
    # one stem and one CSV row.
    word = "female" if sex == "F" else "male"
    stem = re.sub(r"_%s(?=_|$)" % word, "", stem, count=1, flags=re.I)
    # Newer retail cuts carry the race and sex retail's way instead ("..._sc_m..."), and a race with no cut
    # of its own borrows another race's (a Human wearing the Undead cut keeps "_sc_m"). The file itself is
    # already per race and sex (the "_<Race><Sex>" suffix), so drop any such tag: one stem, one CSV row.
    stem = re.sub(r"_(%s)_[mf](?=_|$)" % "|".join(sorted(RACE_TAGS)), "", stem, count=1, flags=re.I)
    return stem, f"{stem}{token}"


# Two-letter race codes retail and the 3.3.5a client use in model names (client prefixes, lowercased).
RACE_TAGS = {"hu", "or", "dw", "ni", "sc", "ta", "gn", "tr", "go", "be", "dr", "fo", "na", "br", "sk", "vr",
             "tu", "ft", "wt", "ns", "it", "wo", "pa", "nb", "hn", "lf", "vm", "dh", "ma", "kt", "vu", "dt",
             "ea", "zt", "mh", "ko", "dv", "ea"}


def safe_component(text):
    return re.sub(r"[^A-Za-z0-9_]+", "_", text).strip("_")


# --------------------------------------------------------------------------------------------------

MARKER = "WXLEquipPlusSource.json"  # written at the root of every patch this tool makes


def _marker_work(patch_path):
    try:
        with open(os.path.join(patch_path, MARKER), encoding="utf-8") as f:
            return os.path.normcase(os.path.abspath(json.load(f).get("work", "")))
    except (OSError, ValueError):
        return None


def choose_patch_letter(data_dir, wanted, work):
    """The letter for this project's folder patch.

    A patch this tool already made for the same kit project is reused, so running the tool again after
    adding sets replaces that patch instead of creating a second one. Otherwise the first free letter.
    """
    taken, ours = {}, None
    me = os.path.normcase(os.path.abspath(work))
    if data_dir and os.path.isdir(data_dir):
        for n in os.listdir(data_dir):
            m = re.fullmatch(r"patch-(.)\.mpq", n, re.I)
            if not m:
                continue
            letter = m.group(1).upper()
            taken[letter] = n
            if _marker_work(os.path.join(data_dir, n)) == me:
                ours = letter
    if wanted:
        wanted = wanted.upper()
        if wanted in taken and wanted != ours:
            raise ConvertError(f"{taken[wanted]} already exists in {data_dir} and is not this project's")
        return wanted
    if ours:
        return ours
    for letter in "WVUTSRQOLKJHF":
        if letter not in taken:
            return letter
    raise ConvertError("no free patch letter")


def convert(work, out_root=None, letter=None, armor_pieces=False, log=print):
    kit = KitProject(work)
    data_dir = kit.cfg.get("client_data") or ""
    letter = choose_patch_letter(data_dir, letter, work)
    patch_dir_name = f"patch-{letter}.MPQ"
    out_root = os.path.abspath(out_root or kit.path("out", "equip-plus"))
    patch = os.path.join(out_root, patch_dir_name)
    files = {}          # archive path -> bytes
    shipped_as = {}     # kit archive path -> the paths this patch ships the same model under
    rows = []
    problems = []
    skipped_slots = {}

    with MpqReader(kit.ext_mpq) as mpq:
        for retail, entry in sorted(kit.extension.items(), key=lambda kv: int(kv[0])):
            slot = entry.get("slot")
            if slot not in SLOTS:
                skipped_slots[slot] = skipped_slots.get(slot, 0) + 1
                continue
            new_id = kit.display_ids.get(str(retail)) or entry.get("new_display")
            if not new_id:
                problems.append(f"display {retail}: no new display id in ids.json (run the kit's `tables`)")
                continue
            ep_slot = SLOTS[slot]
            pieces = [p for p in entry.get("pieces", []) if not p.get("error")]
            skinned = [p for p in pieces if p.get("skinned_to_retail_bones")]
            if skinned and not armor_pieces:
                skipped_slots[f"{slot} (skinned, needs --armor-pieces)"] = \
                    skipped_slots.get(f"{slot} (skinned, needs --armor-pieces)", 0) + 1
                continue
            by_stem = {}
            for p in pieces:
                stem, base = equip_plus_name(p["model"], p["prefix"], p["sex"])
                by_stem.setdefault((p.get("k", 0), stem), []).append((p, base))

            for (k, stem), group in sorted(by_stem.items()):
                # every cut of one stem is the same kind of piece; treat it as skinned if any cut is
                folder_root, attach = placement(slot, any(p.get("skinned_to_retail_bones") for p, _ in group))
                folder = f"{folder_root}\\{kit.ns}"
                texture_name = ""
                needs_item_texture = False
                geosets_present = set()
                for p, base in group:
                    src = p["model"]
                    model = mpq.read(src)
                    skin = mpq.read(src[:-3] + "00.skin")
                    dest = f"Item\\ObjectComponents\\{folder}\\{base}"
                    files[dest + ".m2"] = model
                    shipped_as.setdefault(src, []).append(dest + ".m2")
                    files[dest + "00.skin"] = skin
                    geosets_present |= skin_geosets(skin)
                    needs_item_texture |= m2_uses_item_texture(model)
                    for tex in m2_texture_paths(model):
                        if tex not in files:
                            if not mpq.has(tex):
                                problems.append(f"{base}: names {tex}, which the kit's archive lacks")
                                continue
                            files[tex] = mpq.read(tex)
                if needs_item_texture:
                    fdid = kit.colourway_texture(retail, k, int(group[0][0].get("retail_race") or 0))
                    raw = kit.retail_file(fdid) if fdid else None
                    if raw is None:
                        problems.append(f"display {retail} ({ep_slot}): colourway texture "
                                        f"{fdid or '(not in selection.json)'} not in the kit's cache - "
                                        "the piece would draw untextured, so it was left out")
                        continue
                    try:
                        blp = blp_for_client(raw)
                    except ConvertError as ex:
                        problems.append(f"display {retail}: texture {fdid}: {ex}")
                        continue
                    texture_name = f"{safe_component(kit.ns).lower()}_{fdid}"
                    files[f"Item\\ObjectComponents\\{folder}\\{texture_name}.blp"] = blp
                rows.append({
                    "DisplayID": new_id, "ItemID": "", "Slot": ep_slot, "Layer": "Base", "Side": "C",
                    "Folder": folder, "Model": f"{stem}.mdx", "Texture": texture_name,
                    "Geosets": geoset_filter(slot, geosets_present),
                    "Attach": attach, "SuffixPolicy": "Legacy", "TexturePolicy": "Exact", "Flags": "",
                })

    # Looks the kit could not ship, brought back by rescue_left_out.py: their staged models, textures and
    # icons, their CSV rows, and their table rows - shipped as wxl-extended-dbc add-ons
    # (<Table>.dbc1-equipplus) so the kit's own tables in patch-X stay untouched.
    rescue_dir = kit.path("out", "equip-plus-rescue")
    rescued_rows = 0
    if os.path.isfile(os.path.join(rescue_dir, "rows.json")):
        with open(os.path.join(rescue_dir, "rows.json"), encoding="utf-8") as f:
            extra = json.load(f)
        rows.extend(extra)
        rescued_rows = len(extra)
        staged = os.path.join(rescue_dir, "files")
        for dirpath, _dirs, names in os.walk(staged):
            for n in names:
                src = os.path.join(dirpath, n)
                rel = os.path.relpath(src, staged).replace(os.sep, "\\")
                with open(src, "rb") as f:
                    files[rel] = f.read()
        for table in ("ItemDisplayInfo", "Item"):
            p = os.path.join(rescue_dir, "dbc", table + ".dbc")
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    files[f"DBFilesClient\\{table}.dbc1-equipplus"] = f.read()

    if not rows:
        raise ConvertError("nothing to convert" + ("" if armor_pieces else
                           " (only belts are converted by default; --armor-pieces adds the rest)"))

    buf = io.StringIO()
    w = csv.DictWriter(buf, fieldnames=CSV_HEADER, lineterminator="\n")
    w.writeheader()
    for r in sorted(rows, key=lambda r: (int(r["DisplayID"]), r["Model"])):
        w.writerow(r)
    files["DBFilesClient\\WXLItemDisplayModels.csv"] = buf.getvalue().encode("utf-8")

    # Retail physics for every shipped piece that has it (plumes, ribbons, chains): one table Equip Plus
    # runs live when its physics option is on. Pieces the kit shipped in patch-X (helms, shoulders) are
    # covered too - they are found by their own archive paths.
    physics_models = 0
    try:
        prow, prep = physics_table.build_rows(kit.work, kit.retail_file, shipped_as)
        if prow:
            files["DBFilesClient\\WXLEquipPlusPhysics.phy2"] = physics_table.write_table(prow)
        physics_models = prep["models"]
        problems.extend("physics: " + x for x in prep["skipped"] if "merged" not in x)
        merged = sum(1 for x in prep["skipped"] if "merged" in x)
    except (OSError, ValueError) as ex:
        merged = 0
        problems.append(f"physics table not written: {ex}")

    files[MARKER] = json.dumps({"tool": "wxl-equip-plus backport_to_equipplus", "work": kit.work},
                               indent=2).encode("utf-8")
    if os.path.isdir(patch):  # a previous run: start clean so nothing stale survives
        if _marker_work(patch) != os.path.normcase(kit.work):
            raise ConvertError(f"{patch} exists and was not made for this project - remove it or use --out")
        shutil.rmtree(patch)
    for rel, data in files.items():
        dest = os.path.join(patch, *rel.split("\\"))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(data)

    models = sum(1 for k in files if k.lower().endswith(".m2"))
    textures = sum(1 for k in files if k.lower().endswith(".blp"))
    log(f"equip-plus: {len(rows)} CSV rows, {models} models, {textures} textures -> {patch}")
    if physics_models:
        log(f"  physics data for {physics_models} model(s) (used when Equip Plus's live physics is on)"
            + (f"; {merged} more cannot move because the kit merged their bones" if merged else ""))
    if rescued_rows:
        log(f"  includes {rescued_rows} row(s) for looks brought back by rescue_left_out.py, with their "
            "table rows as ItemDisplayInfo/Item .dbc1-equipplus add-ons (needs wxl-extended-dbc)")
    for s, n in sorted(skipped_slots.items(), key=lambda kv: str(kv[0])):
        log(f"  skipped {n} display(s): {s}")
    for p in problems:
        log(f"  PROBLEM {p}")
    target = data_dir or "<WoW>\\Data"
    existing = os.path.join(data_dir, patch_dir_name) if data_dir else ""
    verb = "replace" if existing and os.path.isdir(existing) else "copy"
    log(f"install: {verb} the folder {patch_dir_name} in {target} (Equip Plus must be installed); "
        "delete <WoW>\\Cache")
    return {"patch": patch, "rows": rows, "problems": problems, "files": sorted(files)}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--work", required=True, help="the backport kit's project folder (the --work you used)")
    ap.add_argument("--out", help="where to write the folder patch (default: <work>/out/equip-plus)")
    ap.add_argument("--letter", help="patch letter (default: the first free one of W V U T ...)")
    ap.add_argument("--armor-pieces", action="store_true",
                    help="also convert the pieces skinned to retail's HD skeletons (fit HD bodies only)")
    a = ap.parse_args(argv)
    try:
        result = convert(a.work, a.out, a.letter, a.armor_pieces)
    except ConvertError as ex:
        print(f"equip-plus: FAIL - {ex}", file=sys.stderr)
        return 1
    return 2 if result["problems"] else 0


if __name__ == "__main__":
    sys.exit(main())
