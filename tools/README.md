# Tools

## `backport_helper.py` - the guided way (start here)

Double-click `Backport Helper.bat` at the top of the repository (or run `python tools\backport_helper.py`).
Everything below is what it does for you; you only need the individual tools if you want to run a step
by hand.

First run: it asks for the kit's folder (and offers to download the kit), your client's `Data` folder, the
kit project to use (point it at an existing project to keep what you already have), and optionally your server's
`dbc` folder and database details. Settings are saved in `%USERPROFILE%\.wxl-equip-plus\helper.json`;
your database password never is.

Every install makes a dated backup under `<project>\backups\`, and **Undo last install** restores it.

If a build stops because a look has "nothing a 3.3.5 client can draw" - a purely 3D piece such as a
coffin on the back - the helper offers to leave that look out and finish. It remembers the choice in
`<project>\helper-excluded.json`, and **Where things stand** lists what was left out.

**Bring back left-out looks** turns those purely 3D looks into Equip Plus pieces after all. They get
their own display ids (from 88000) and item ids (from 990000), carried in add-on tables
(`DBFilesClient\Item.dbc1-equipplus` and `ItemDisplayInfo.dbc1-equipplus` inside the Equip Plus patch),
so the kit's own tables are untouched. This needs the `wxl-extended-dbc` extension. The next **Install**
adds them to the patch, the server tables and the database. Rigged pieces are made for retail's bodies,
so on stock bodies the fit can be off; **Undo last install** removes them again.

## `rescue_left_out.py` - left-out looks, the long way

What option 8 runs. It converts the left-out looks in a side project (`<work>\rescue`) that shares the
main project's scan and download cache, then stages models, textures, table rows and SQL under
`<work>\out\equip-plus-rescue\`, which the converter and merger pick up.

```powershell
python tools\rescue_left_out.py --work C:\Backport\MyProject --kit C:\Tools\wow-retail-backport\wow-retail-backport
```

**Remove something** (option 9) lists the sets and single items in the project and takes out the ones you
pick: out of the kit's selection and plan, out of the looks brought back, and - at the next Install - out
of the database (`out\sql\item_template_removed.sql`). Then choose Build and Install. Nothing downloaded
is deleted and the ids stay reserved, so adding something back later is quick and gets the same ids.

## `backport_to_equipplus.py` - retail sets from the backport kit, in 3D

[wow-retail-backport](https://github.com/Kirazul/wow-retail-backport) converts a retail armour set for a
stock 3.3.5a client. With `build --extension` it also exports the retail 3D pieces a stock client cannot
show - belt buckles, boot shells, gauntlet cuffs, robe skirts - but in its own naming, and without the
colourway texture each piece needs. This tool turns that export into a folder patch Equip Plus draws.

**Requirements:** Python 3.9 or later (standard library only), and a backport kit project that has been
run through `pack` with `build --extension`.

```powershell
python tools\backport_to_equipplus.py --work C:\Backport\MyProject
```

It writes `<work>\out\equip-plus\patch-<L>.MPQ\` - a folder, which the client mounts exactly like an
archive - containing:

- every piece model and its skin, renamed to the `<stem>_<Race><Sex>.m2` form Equip Plus looks for;
- the textures the models name, plus each display's colourway texture, taken from the kit's download
  cache and trimmed to the client's 1024-pixel limit;
- `DBFilesClient\WXLItemDisplayModels.csv`, one row per display, keyed by the display ids the kit
  assigned. No DBC is edited and nothing is downloaded.

Copy that folder into the client's `Data` folder and delete the client's `Cache` folder. The patch letter
is the first one your `Data` folder does not already use (`--letter` picks one yourself); on later runs
it reuses the patch it made before for the same project, so you replace one folder rather than gaining a
second. Each patch carries a small `WXLEquipPlusSource.json` recording which project made it.

**Belts by default.** Belt buckles are race-agnostic models on the belt-buckle attachment, so they fit
every body. The other pieces are skinned to retail's HD skeletons: on HD bodies they follow the body, on
stock low-poly bodies they will not fit. Add `--armor-pieces` to convert them as well.

**Exit codes:** 0 done; 2 done but with problems listed (for example a colourway texture missing from
the kit's cache - that piece is left out rather than drawn untextured); 1 failed.

## `physics_table.py` - retail physics data

Called by the converter: for every shipped model whose retail source has physics (.phys), it writes one
row of a table (`DBFilesClient\WXLEquipPlusPhysics.phy2`, the "PHY2" layout the engine's own reader
decodes) keyed by the model's archive path. Equip Plus reads it when live physics is on. Run it alone
with `python tools\physics_table.py --work <project>` to see how many models carry physics.

## `merge_server_dbc.py` - the kit's item rows into your server's tables

The kit's server tables (`<work>\out\dbc\`) are built from the client's copies. If your server's
`Item.dbc` or `ItemDisplayInfo.dbc` hold rows the client's do not - mounts or custom items added by hand -
copying the kit's files over them deletes those rows. This tool starts from the server's own files and
changes only the rows the kit assigned:

```powershell
python tools\merge_server_dbc.py --work C:\Backport\MyProject --server C:\AzerothCore\dbc
```

It reads the server folder but never writes to it: merged copies go to `<work>\out\server-dbc\`. Stock
rows always keep the server's version. Running it again after a rebuild updates the kit's rows and adds
new ones; nothing else changes.

## Adding more sets later

Keep **one kit project** for everything you backport to a client - the kit's own rule, because only one
set of extended tables can win. Add to it rather than starting another:

```powershell
cd C:\Tools\wow-retail-backport\wow-retail-backport\scripts
python bp.py select  --work C:\Backport\MyProject --add --set <id> [--family]
python bp.py fetch   --work C:\Backport\MyProject
python bp.py newtech --work C:\Backport\MyProject
python bp.py measure --work C:\Backport\MyProject
python bp.py build   --work C:\Backport\MyProject --extension
python bp.py tables  --work C:\Backport\MyProject
python bp.py audit   --work C:\Backport\MyProject
python bp.py prove-gates --work C:\Backport\MyProject
python bp.py pack    --work C:\Backport\MyProject
python bp.py sheet   --work C:\Backport\MyProject
```

**Do not re-run `scan`** while the kit's patch is in your `Data` folder: scan would read the patch's own
tables, which already hold your earlier rows, and `tables` would stop on the first repeated id. The scan
from the project's first run is still correct. If you ever must re-scan (another patch changed the
client's tables), take the kit's patch out of `Data` first and put the new one back after `pack`.

Ids are stable: everything already backported keeps its display and item ids, so existing characters and
CSV rows stay valid. Then:

1. Replace `patch-X.MPQ` in `Data` with the new one from `<work>\out\`.
2. `python tools\backport_to_equipplus.py --work <work>` - it recognises its own `patch-W.MPQ` and rebuilds
   it with every set's pieces. Replace the folder in `Data`.
3. `python tools\merge_server_dbc.py --work <work> --server <server dbc folder>`, copy the result into
   the server's `dbc` folder, and run `<work>\out\sql\item_template.sql` again (it only touches the kit's
   item ids).
4. Delete the client's `Cache` folder and restart the worldserver.

## `mpqread.py`

A small read-only MPQ reader (hash and block tables, sector files, zlib and stored data) used by the
converter. It refuses compression it does not implement rather than misreading it.
