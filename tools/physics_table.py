#!/usr/bin/env python3
r"""physics_table.py - retail physics data for the pieces a backport kit project shipped.

Retail moves plumes, ribbons, tassels and chains with its runtime physics engine; each such model names
a .phys file (or carries one inline). Equip Plus can run that engine live (Kirazul's port, see
src/physics/README.md), but needs every piece's physics data in one table it can look up by the
model's path. This module writes that table.

    python physics_table.py --work C:\Backport\MyProject [--out table.phy2]

The format is the "PHY2" table the engine's own reader (physics_domino.cpp, Find) decodes:

    "PHY2", u32 row count, then (u32 FNV-1a hash of the key, u32 row offset) sorted by hash;
    a row: u16 key length, key (lower case, '\' separators, .m2), pad to 4, u8 bodies, shapes, joints,
    hulls, then each record as Find reads it.

Every field is the .phys file's own value (the engine makes every conversion the client makes),
copied as the kit's references/rules.md section 6a lays the v6 chunks out. Body origins are moved
by the same shift the kit applied to the model (helms are moved to sit on 3.3.5 heads), so they keep
meeting their bones. A model is keyed three ways - its archive path, its file name, and the name
stored inside the .m2 - because the client may report any of them.

Standard library only.
"""
import argparse
import json
import os
import struct
import sys

# --------------------------------------------------------------------------------------------------
# reading .phys (v6 as measured on 304 retail files; rules.md 6a)

BODY_TYPES = {0: 1, 1: 0}               # file type -> Domino: 0 kinematic (1), 1 dynamic (0), else static
SHAPE_TYPES = {2: 0, 1: 1, 3: 2}        # file SHP2 type -> Domino: sphere 0, capsule 1, hull 2 (box: none)
JOINT_TYPES = {0: 8, 1: 7, 2: 9, 3: 6, 4: 5, 5: 2}   # JOIN type -> Domino joint type


class PhysError(Exception):
    pass


def _chunks(blob):
    out = {}
    o = 0
    while o + 8 <= len(blob):
        tag = blob[o:o + 4][::-1].decode("latin-1")   # stored byte-reversed: "SYHP" is PHYS
        n = struct.unpack_from("<I", blob, o + 4)[0]
        out[tag] = blob[o + 8:o + 8 + n]
        o += 8 + n
    return out


def _f(b, o, n=1):
    v = struct.unpack_from("<%df" % n, b, o)
    return v if n > 1 else v[0]


def parse_phys(blob):
    """The .phys block as the engine's table rows want it: bodies, shapes, joints, hulls."""
    if blob[:4] not in (b"PHYS", b"SYHP"):
        raise PhysError("not a PHYS block")
    ch = _chunks(blob)
    if "BDY4" not in ch or "SHP2" not in ch or "JOIN" not in ch:
        raise PhysError("not a v6 file (needs BDY4, SHP2, JOIN): %s" % sorted(ch))

    bodies, shape_owner = [], {}
    b = ch["BDY4"]
    for i in range(len(b) // 0x30):
        o = i * 0x30
        typ, bone = struct.unpack_from("<HH", b, o)
        pos = _f(b, o + 4, 3)
        first = struct.unpack_from("<H", b, o + 0x10)[0]
        count = struct.unpack_from("<i", b, o + 0x14)[0]
        gravity, inertia, lin, ang, carry = _f(b, o + 0x18, 5)
        group = struct.unpack_from("<H", b, o + 0x2C)[0]
        for s in range(first, first + max(count, 0)):
            shape_owner[s] = i
        bodies.append({"type": BODY_TYPES.get(typ, 2), "bone": bone, "pos": list(pos), "gravity": gravity,
                       "inertia": inertia, "lin": lin, "ang": ang, "carry": carry, "group": group})

    caps = ch.get("CAPS", b"")
    sphs = ch.get("SPHS", b"")
    hulls = _hulls(ch.get("PLYT", b""))
    shapes = []
    s = ch["SHP2"]
    for i in range(len(s) // 0x20):
        o = i * 0x20
        st, si = struct.unpack_from("<hh", s, o)
        fr, rs, den = _f(s, o + 8, 3)
        if st not in SHAPE_TYPES:
            raise PhysError("shape %d is a box, which the engine does not have" % i)
        rec = {"body": shape_owner.get(i, 0), "type": SHAPE_TYPES[st], "hull": 0, "friction": fr,
               "restitution": rs, "density": den, "a": [0.0] * 3, "b": [0.0] * 3, "r": 0.0}
        if st == 1:
            if (si + 1) * 0x1C > len(caps):
                raise PhysError("capsule %d missing" % si)
            rec["a"], rec["b"] = list(_f(caps, si * 0x1C, 3)), list(_f(caps, si * 0x1C + 12, 3))
            rec["r"] = _f(caps, si * 0x1C + 24)
        elif st == 2:
            if (si + 1) * 16 > len(sphs):
                raise PhysError("sphere %d missing" % si)
            rec["a"], rec["r"] = list(_f(sphs, si * 16, 3)), _f(sphs, si * 16 + 12)
        else:
            if si >= len(hulls):
                raise PhysError("hull %d missing" % si)
            rec["hull"] = si
        shapes.append(rec)

    subs = {}
    for tag, kind, size in (("SPHJ", 0, 28), ("SHJ2", 1, 0x7C), ("WLJ3", 2, 0x74), ("REV2", 3, 0x78)):
        if tag in ch:
            subs[kind] = (ch[tag], size)
    joints = []
    J = ch["JOIN"]
    for i in range(len(J) // 16):
        a, bb, _pad, jt, jid = struct.unpack_from("<II4shh", J, i * 16)
        if jt not in subs:
            raise PhysError("joint %d has type %d, not supported here" % (i, jt))
        d, size = subs[jt]
        o = jid * size
        if o + size > len(d):
            raise PhysError("joint %d record missing" % i)
        rec = {"type": JOINT_TYPES[jt], "a": a, "b": bb, "mode": 0, "pa": [0.0] * 3, "pb": [0.0] * 3,
               "fa": [1.0, 0, 0, 0, 1.0, 0, 0, 0, 1.0], "fb": [1.0, 0, 0, 0, 1.0, 0, 0, 0, 1.0], "f": [0.0] * 6}
        if jt == 0:                       # spherical: two anchors
            rec["pa"], rec["pb"] = list(_f(d, o, 3)), list(_f(d, o + 12, 3))
        else:                             # frames: 3x3 (axis images as rows) + position, A then B
            rec["fa"], rec["pa"] = list(_f(d, o, 9)), list(_f(d, o + 0x24, 3))
            rec["fb"], rec["pb"] = list(_f(d, o + 0x30, 9)), list(_f(d, o + 0x54, 3))
            if jt == 1:                   # SHJ2: twistLo, twistHi, cone, maxTorque, mode, Hz, zeta
                tl, th, cone, mt = _f(d, o + 0x60, 4)
                rec["mode"] = struct.unpack_from("<b", d, o + 0x70)[0]
                hz, zeta = _f(d, o + 0x74, 2)
                rec["f"] = [tl, th, cone, mt, hz, zeta]
            elif jt == 3:                 # REV2: lower, upper, maxTorque, mode, Hz, zeta
                lo, hi, mt = _f(d, o + 0x60, 3)
                rec["mode"] = struct.unpack_from("<b", d, o + 0x6C)[0]
                hz, zeta = _f(d, o + 0x70, 2)
                rec["f"] = [lo, hi, mt, hz, zeta, 0.0]
            elif jt == 2:                 # WLJ3: angHz 0x60, linHz 0x64, angZeta 0x68, linZeta 0x6C
                ahz, lhz, az, lz = _f(d, o + 0x60, 4)
                rec["f"] = [ahz, az, lhz, lz, 0.0, 0.0]
        joints.append(rec)

    if len(bodies) > 255 or len(shapes) > 255 or len(joints) > 255 or len(hulls) > 255:
        raise PhysError("too many records for the table")
    for j in joints:
        if j["a"] >= len(bodies) or j["b"] >= len(bodies):
            raise PhysError("a joint names a body the file does not have")
    return {"bodies": bodies, "shapes": shapes, "joints": joints, "hulls": hulls}


def _hulls(b):
    if not b:
        return []
    n = struct.unpack_from("<I", b, 0)[0]
    heads = []
    for i in range(n):
        o = 4 + i * 0x50
        nv = struct.unpack_from("<I", b, o)[0]
        nf = struct.unpack_from("<I", b, o + 0x10)[0]
        ne = struct.unpack_from("<I", b, o + 0x28)[0]
        heads.append((nv, nf, ne, list(_f(b, o + 0x38, 3)), _f(b, o + 0x44), _f(b, o + 0x48)))
    out = []
    o = 4 + n * 0x50
    for nv, nf, ne, cen, vol, area in heads:
        if nv > 255 or nf > 255 or ne > 255:
            raise PhysError("hull too large for the table")
        v = [list(_f(b, o + k * 12, 3)) for k in range(nv)]
        o += nv * 12
        planes = [list(_f(b, o + k * 16, 4)) for k in range(nf)]
        o += nf * 16
        face = list(b[o:o + nf])
        o += nf
        edge = list(b[o:o + ne * 4])
        o += ne * 4
        out.append({"centroid": cen, "volume": vol, "area": area, "v": v, "planes": planes,
                    "face": face, "edge": edge})
    return out


def physics_blob(model_bytes, read_fdid):
    """A retail model's .phys: inline PFDC, else the file its PFID names (read_fdid(fdid) -> bytes)."""
    if model_bytes[:4] == b"MD20":
        return None   # legacy flat file: no chunks, no physics
    o = 0
    pfid = None
    while o + 8 <= len(model_bytes):
        tag = model_bytes[o:o + 4]
        n = struct.unpack_from("<I", model_bytes, o + 4)[0]
        if tag == b"PFDC":
            return model_bytes[o + 8:o + 8 + n]
        if tag == b"PFID":
            pfid = struct.unpack_from("<I", model_bytes, o + 8)[0]
        o += 8 + n
    return read_fdid(pfid) if pfid else None


# --------------------------------------------------------------------------------------------------
# writing the table

def norm_key(path):
    k = path.replace("/", "\\").lower()
    if k.endswith(".mdx") or k.endswith(".mdl"):
        k = k[:-4] + ".m2"
    return k


def fnv1a(s):
    h = 0x811C9DC5
    for c in s.encode("latin-1"):
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h


def _pad4(buf):
    while len(buf) % 4:
        buf.append(0)


def encode_row(key, p, shift=(0.0, 0.0, 0.0)):
    out = bytearray()
    kb = key.encode("latin-1")
    out += struct.pack("<H", len(kb)) + kb
    _pad4(out)
    out += struct.pack("<4B", len(p["bodies"]), len(p["shapes"]), len(p["joints"]), len(p["hulls"]))
    for b in p["bodies"]:
        pos = [b["pos"][i] + shift[i] for i in range(3)]
        out += struct.pack("<BBH3f5fHH", b["type"], 0, b["bone"], *pos, b["gravity"], b["inertia"],
                           b["lin"], b["ang"], b["carry"], b["group"], 0)
    for s in p["shapes"]:
        out += struct.pack("<BBH3f3f3ff", s["body"], s["type"], s["hull"], s["friction"], s["restitution"],
                           s["density"], *s["a"], *s["b"], s["r"])
    for j in p["joints"]:
        out += struct.pack("<BBBb3f3f9f9f6f", j["type"], j["a"], j["b"], j["mode"], *j["pa"], *j["pb"],
                           *j["fa"], *j["fb"], *j["f"])
    for h in p["hulls"]:
        out += struct.pack("<4B3fff", len(h["v"]), len(h["planes"]), len(h["edge"]) // 4, 0,
                           *h["centroid"], h["volume"], h["area"])
        for v in h["v"]:
            out += struct.pack("<3f", *v)
        for pl in h["planes"]:
            out += struct.pack("<4f", *pl)
        out += bytes(h["face"]) + bytes(h["edge"])
        _pad4(out)
    _pad4(out)
    return bytes(out)


def write_table(rows):
    """rows: {key: encoded row}. Keys must already be normalised."""
    items = sorted(((fnv1a(k), k, r) for k, r in rows.items()), key=lambda t: (t[0], t[1]))
    head = bytearray(b"PHY2" + struct.pack("<I", len(items)))
    body = bytearray()
    base = 8 + 8 * len(items)
    for h, _k, r in items:
        head += struct.pack("<II", h, base + len(body))
        body += r
    return bytes(head + body)


def m2_internal_name(model):
    """The name stored inside an MD20 model (header 0x08: length, offset), '' when absent."""
    if model[:4] != b"MD20" or len(model) < 0x10:
        return ""
    n, o = struct.unpack_from("<II", model, 0x08)
    if not n or o + n > len(model):
        return ""
    return model[o:o + n].split(b"\0", 1)[0].decode("latin-1", "replace")


def m2_bone_count(model):
    if model[:4] != b"MD20" or len(model) < 0x34:
        return 0
    return struct.unpack_from("<I", model, 0x2C)[0]


# --------------------------------------------------------------------------------------------------
# from a kit project

def _source_of(man, rel, depth=0):
    """(retail fdid, shift) a built file was made from, following the kit's manifest."""
    e = man.get(rel.lower())
    if not e or depth > 4:
        return None, (0.0, 0.0, 0.0)
    src = e.get("src") or {}
    shift = tuple(src.get("shift") or (0.0, 0.0, 0.0))
    fdid = src.get("fdid")
    if fdid is None and isinstance(src.get("from"), int):
        fdid = src["from"]
    if fdid is None and isinstance(src.get("filtered_from"), str):
        fdid, inner = _source_of(man, src["filtered_from"], depth + 1)
        shift = tuple(a + b for a, b in zip(shift, inner))
    return fdid, shift


def row_body(row):
    """A row without its key (the part two keys can share)."""
    n = struct.unpack_from("<H", row, 0)[0]
    at = 2 + n
    at += (4 - at % 4) % 4
    return row[at:]


def keyed(key, body):
    out = bytearray(struct.pack("<H", len(key)) + key.encode("latin-1"))
    _pad4(out)
    return bytes(out) + body


def build_rows(work, read_retail, renamed=None, log=print):
    """Rows for every built model whose retail source has physics.

    read_retail(fdid) -> bytes or None (the kit's download cache).
    renamed: {kit archive path: [other archive paths the same model is shipped under]}.
    Returns ({key: row bytes}, report dict).
    """
    build_root = os.path.join(work, "build")
    with open(os.path.join(build_root, "manifest.json"), encoding="utf-8") as f:
        man = json.load(f)
    renamed = {k.lower(): v for k, v in (renamed or {}).items()}
    rows, report = {}, {"models": 0, "skipped": []}
    secondary = {}   # file name or internal name -> set of row bodies
    parsed = {}
    for key, e in sorted(man.items()):
        rel = e.get("path") or key
        if not rel.lower().endswith(".m2"):
            continue
        fdid, shift = _source_of(man, rel)
        if not fdid:
            continue
        if fdid not in parsed:
            raw = read_retail(fdid)
            blob = physics_blob(raw, read_retail) if raw else None
            try:
                parsed[fdid] = parse_phys(blob) if blob else None
            except PhysError as ex:
                parsed[fdid] = None
                report["skipped"].append(f"{rel}: {ex}")
        p = parsed[fdid]
        if not p:
            continue
        try:
            with open(os.path.join(build_root, *rel.split("\\")), "rb") as f:
                model = f.read()
        except OSError:
            continue
        nb = m2_bone_count(model)
        need = max((b["bone"] for b in p["bodies"] if b["bone"] != 0xFFFF), default=0)
        if nb < 2 or need >= nb:
            report["skipped"].append(f"{rel}: its bones were merged when it was converted "
                                     f"({nb} bone(s), physics needs {need + 1})")
            continue
        body = row_body(encode_row("", p, shift))
        for k in [norm_key(rel)] + [norm_key(r) for r in renamed.get(rel.lower(), [])]:
            rows[k] = keyed(k, body)
            secondary.setdefault(k.rsplit("\\", 1)[-1], set()).add(body)
        inner = m2_internal_name(model)
        if inner:
            ik = norm_key(inner if inner.lower().endswith((".m2", ".mdx")) else inner + ".m2")
            secondary.setdefault(ik, set()).add(body)
        report["models"] += 1
    # a file name, or a model's internal name, is a key only when everything sharing it moves alike
    for k, bodies in secondary.items():
        if len(bodies) == 1 and k not in rows:
            rows[k] = keyed(k, next(iter(bodies)))
    return rows, report


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", required=True)
    ap.add_argument("--out", help="where to write the table (default <work>\\out\\WXLEquipPlusPhysics.phy2)")
    a = ap.parse_args(argv)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from backport_to_equipplus import KitProject
    kit = KitProject(a.work)
    rows, rep = build_rows(kit.work, kit.retail_file)
    out = a.out or kit.path("out", "WXLEquipPlusPhysics.phy2")
    with open(out, "wb") as f:
        f.write(write_table(rows))
    print(f"physics: {rep['models']} model(s), {len(rows)} key(s) -> {out}")
    for s in rep["skipped"]:
        print("  skipped " + s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
