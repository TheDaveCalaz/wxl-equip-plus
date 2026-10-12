#!/usr/bin/env python3
"""Merge a backport kit project's item rows into a server's own DBC files, keeping everything else.

The kit writes server tables (`<work>/out/dbc/`) built from the *client's* copies. A server whose tables
carry rows the client's do not - custom items, mounts added by hand - would lose them if those files were
simply copied over. This tool starts from the server's own files and changes only the rows the kit owns:

  * every server row is kept byte-for-byte, except rows whose id the kit assigned (its `ids.json`),
    which are replaced by the kit's current version, so re-running after a rebuild picks up changes;
  * kit rows the server lacks are added;
  * stock rows - every id the kit did not assign - always keep the server's version, even where a client
    patch (an HD pack, say) edited the client's copy.

Nothing on the server is touched. Merged copies go to `<work>/out/server-dbc/` (or `--out`); back up
the server's `dbc` folder, copy them in, run the kit's `item_template.sql`, restart the worldserver.

    python merge_server_dbc.py --work C:\\Backport\\MyProject --server "C:\\AzerothCore\\dbc"

Python 3.9+, standard library only.
"""
import argparse
import json
import os
import struct
import sys

# String columns per table (WotLK 3.3.5a 12340 layouts). Tables not listed are merged as plain numbers.
STRING_COLUMNS = {
    "ItemDisplayInfo": (1, 2, 3, 4, 5, 6, 15, 16, 17, 18, 19, 20, 21, 22),
}
# kit ids.json section that owns each table's ids
OWNER = {"Item": "item", "ItemDisplayInfo": "display", "HelmetGeosetVisData": "helmvis"}
SERVER_TABLES = ("Item", "ItemDisplayInfo")  # HelmetGeosetVisData only matters to the client


class MergeError(Exception):
    pass


class Dbc:
    def __init__(self, path):
        with open(path, "rb") as f:
            d = f.read()
        magic, n, fields, size, ssize = struct.unpack_from("<4s4I", d, 0)
        if magic != b"WDBC" or size != fields * 4:
            raise MergeError(f"{path}: not a WDBC file this tool understands")
        self.fields = fields
        self.rows = [list(struct.unpack_from("<%dI" % fields, d, 20 + i * size)) for i in range(n)]
        self.strings = bytearray(d[20 + n * size:20 + n * size + ssize]) or bytearray(b"\0")
        self._index = {}

    def string(self, off):
        return bytes(self.strings[off:self.strings.index(b"\0", off)])

    def add_string(self, b):
        if not b:
            return 0
        if not self._index:
            pos = 0
            while pos < len(self.strings):
                end = self.strings.index(b"\0", pos)
                self._index.setdefault(bytes(self.strings[pos:end]), pos)
                pos = end + 1
        if b not in self._index:
            self._index[b] = len(self.strings)
            self.strings += b + b"\0"
        return self._index[b]

    def resolved(self, row, string_cols):
        return [self.string(v) if i in string_cols else v for i, v in enumerate(row)]

    def to_bytes(self):
        rows = sorted(self.rows, key=lambda r: r[0])
        out = bytearray(struct.pack("<4s4I", b"WDBC", len(rows), self.fields, self.fields * 4, len(self.strings)))
        for r in rows:
            out += struct.pack("<%dI" % self.fields, *r)
        return bytes(out + self.strings)


def merge_table(name, server_path, kit_path, owned):
    srv = server_path if isinstance(server_path, Dbc) else Dbc(server_path)
    kit = Dbc(kit_path)
    if srv.fields != kit.fields:
        raise MergeError(f"{name}: server has {srv.fields} fields, kit has {kit.fields}")
    cols = set(STRING_COLUMNS.get(name, ()))
    by_id = {r[0]: i for i, r in enumerate(srv.rows)}
    added = replaced = same = stock_differs = 0
    for krow in kit.rows:
        rid = krow[0]
        i = by_id.get(rid)
        if i is not None and rid not in owned:
            # A stock row: the kit's table is the client's copy, which client patches (HD packs) may have
            # edited. Those edits are client-side; the server keeps its own row.
            if srv.resolved(srv.rows[i], cols) != kit.resolved(krow, cols):
                stock_differs += 1
            continue
        want = kit.resolved(krow, cols)
        if i is not None and srv.resolved(srv.rows[i], cols) == want:
            same += 1
            continue
        new = [srv.add_string(v) if c in cols else v for c, v in enumerate(want)]
        if i is None:
            srv.rows.append(new)
            by_id[rid] = len(srv.rows) - 1
            added += 1
        else:
            srv.rows[i] = new
            replaced += 1
    return srv, {"added": added, "replaced": replaced, "same": same, "stock_differs": stock_differs}


def _rescued_ids(work, table):
    """Display or item ids rescue_left_out.py assigned (its rescued.json)."""
    try:
        with open(os.path.join(work, "rescued.json"), encoding="utf-8") as f:
            rec = json.load(f)
    except (OSError, ValueError):
        return set()
    if table == "ItemDisplayInfo":
        return {int(v["display"]) for v in rec.values() if "display" in v}
    return {int(i) for v in rec.values() for i in v.get("items", [])}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--work", required=True, help="the backport kit's project folder")
    ap.add_argument("--server", required=True, help="the server's dbc folder (read only)")
    ap.add_argument("--out", help="where to write merged copies (default: <work>/out/server-dbc)")
    a = ap.parse_args(argv)
    work = os.path.abspath(a.work)
    out = os.path.abspath(a.out or os.path.join(work, "out", "server-dbc"))
    try:
        with open(os.path.join(work, "ids.json"), encoding="utf-8") as f:
            ids = json.load(f)
    except OSError:
        print("merge: FAIL - no ids.json in the project: run the kit's `tables` first", file=sys.stderr)
        return 1
    os.makedirs(out, exist_ok=True)
    for name in SERVER_TABLES:
        sp = os.path.join(a.server, name + ".dbc")
        kp = os.path.join(work, "out", "dbc", name + ".dbc")
        if not os.path.isfile(sp) or not os.path.isfile(kp):
            print(f"merge: FAIL - missing {sp if not os.path.isfile(sp) else kp}", file=sys.stderr)
            return 1
        owned = {int(v) for v in (ids.get(OWNER[name]) or {}).values()}
        try:
            table, rep = merge_table(name, sp, kp, owned)
        except MergeError as ex:
            print(f"merge: FAIL - {ex}", file=sys.stderr)
            return 1
        print(f"merge: {name}.dbc  {len(table.rows)} rows  (kit rows: +{rep['added']} added, "
              f"{rep['replaced']} updated, {rep['same']} already up to date)")
        # looks brought back by rescue_left_out.py: their own rows, their own ids
        rp = os.path.join(work, "out", "equip-plus-rescue", "dbc", name + ".dbc")
        if os.path.isfile(rp):
            rescued = _rescued_ids(work, name)
            try:
                table, rrep = merge_table(name, table, rp, rescued)
            except MergeError as ex:
                print(f"merge: FAIL - {ex}", file=sys.stderr)
                return 1
            print(f"merge: {name}.dbc  brought-back looks: +{rrep['added']} added, {rrep['replaced']} updated, "
                  f"{rrep['same']} already up to date")
        with open(os.path.join(out, name + ".dbc"), "wb") as f:
            f.write(table.to_bytes())
        if rep["stock_differs"]:
            print(f"  note: {rep['stock_differs']} stock row(s) differ between your client and server copies "
                  "(a client patch edited them); the server's rows were kept")
    extra = os.path.join(work, "out", "equip-plus-rescue", "sql", "item_template.sql")
    print(f"merge: written to {out} - back up the server's dbc folder, copy these in, run the kit's "
          "out\\sql\\item_template.sql" + (" and out\\equip-plus-rescue\\sql\\item_template.sql"
                                            if os.path.isfile(extra) else "") + ", restart the worldserver")
    return 0


if __name__ == "__main__":
    sys.exit(main())
