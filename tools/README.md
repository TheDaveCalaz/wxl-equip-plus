# Tools

## `backport_to_equipplus.py` - retail sets from the backport kit, in 3D

[wow-retail-backport](https://github.com/Kirazul/wow-retail-backport) converts a retail armour set for a
stock 3.3.5a client. With `build --extension` it also exports the retail 3D pieces a stock client cannot
show - belt buckles, boot shells, gauntlet cuffs, robe skirts - but in its own naming, and without the
colourway texture each piece needs. This tool turns that export into a folder patch Equip Plus draws.

**Requirements:** Python 3.9 or later (standard library only), and a backport kit project that has been
run through `pack` with `build --extension`.

```powershell
python tools\backport_to_equipplus.py --work C:\Backport\Lionguard
```

It writes `<work>\out\equip-plus\patch-<L>.MPQ\` - a folder, which the client mounts exactly like an
archive - containing:

- every piece model and its skin, renamed to the `<stem>_<Race><Sex>.m2` form Equip Plus looks for;
- the textures the models name, plus each display's colourway texture, taken from the kit's download
  cache and trimmed to the client's 1024-pixel limit;
- `DBFilesClient\WXLItemDisplayModels.csv`, one row per display, keyed by the display ids the kit
  assigned. No DBC is edited and nothing is downloaded.

Copy that folder into the client's `Data` folder and delete the client's `Cache` folder. The patch letter
is the first one your `Data` folder does not already use (`--letter` picks one yourself).

**Belts by default.** Belt buckles are race-agnostic models on the belt-buckle attachment, so they fit
every body. The other pieces are skinned to retail's HD skeletons: on HD bodies they follow the body, on
stock low-poly bodies they will not fit. Add `--armor-pieces` to convert them as well.

**Exit codes:** 0 done; 2 done but with problems listed (for example a colourway texture missing from
the kit's cache - that piece is left out rather than drawn untextured); 1 failed.

## `mpqread.py`

A small read-only MPQ reader (hash and block tables, sector files, zlib and stored data) used by the
converter. It refuses compression it does not implement rather than misreading it.
