# Changelog

## 1.2.3

- **Fixed: retail-rigged pieces floating behind the character in the character window** (reported with
  gloves). A CSV collection row with an empty Attach column was hung on its slot's own attachment point
  (the hand, for gloves) instead of the character root (19) that collections use. In the world Equip Plus
  still fitted the piece to the body, but the character window draws a copy of the model, and only
  root-attached collections were fitted on that copy. Empty Attach on a collection row now means the
  root, as it already did for DBC rows; the converter also writes 19 for collections.

## 1.2.2

- **Fixed: nothing in a cloak row ever drew.** The client hands Equip Plus the tabard on model slot 9
  and the cloak on slot 10; the slot table inherited from earlier versions had them the other way
  round, so a `Back`/`Cape` row was looked for on the tabard and never matched (and a `Tabard` row on
  the cloak). Found from an in-game log of the Forsaken Champion's Backtomb.
- **Fixed: every CSV piece was drawn twice.** A folder patch's CSV is reachable under two names, so
  each row was loaded twice and each model attached twice on top of itself. Identical rows for the same
  display are now kept once, which halves the draw work for sidecar pieces.
- Bring back left-out looks: brought-back items now keep their retail name, quality and material
  (selecting a look by display id alone left them as "Rescued look <id>").
- Backport Helper: **Bring back left-out looks** (menu option 8). Looks the kit cannot build for a stock
  client - purely 3D pieces such as the Forsaken Champion's Backtomb - are converted in a side project
  and given their own display ids (from 88000) and item ids (from 990000). Their table rows ship as
  `wxl-extended-dbc` add-on tables inside the Equip Plus patch, so the kit's tables are untouched; the
  next Install adds them to the server tables and database as well. Undo removes them.
- New tool `tools/rescue_left_out.py`; the converter and `merge_server_dbc.py` pick up its output.

## 1.2.1

- **Fixed: only one shoulder showed** on two-part shoulders named `..._shoulder_left` / `..._shoulder_right`.
  The case-insensitive "contains" check inherited from earlier versions reported a match when every
  character but the last agreed, so `_shoulder_right` matched `_shoulder_l` and both pauldrons were hung on
  the left shoulder. The same fault could misplace any model whose name nearly matched another slot's
  naming rule. Found from an in-game log of the Lionguard set.
- Backport Helper: **Settings > Update Equip Plus** replaces an installed copy with the latest release.
- Converter: **a piece no longer drags the rest of the outfit with it.** Retail keeps a whole outfit in one
  file per race; the backport kit's cut of the Forsaken heritage gloves and chestguard still held the belt,
  boots and gloves, so each of those pieces drew a second copy of all of them (doubled straps, extra
  claws). Each CSV row now carries a geoset filter whenever its model holds other slots' parts, keeping
  the piece to its own groups. Clean pieces - all of Lionguard - get no filter and are unchanged.
- Converter: retail's own race-and-sex tag in newer file names (`_sc_m`, including borrowed cuts) is
  dropped, so each look gets one CSV row instead of one per sex.
- Converter: **a belt rigged to the skeleton is no longer drawn twice.** Belts were always hung rigidly on
  the belt-buckle point, which is right for a simple buckle (Lionguard) but not for a belt skinned to
  retail's bones (Forsaken heritage): that one also appeared floating behind the shoulders in its bind
  pose. Placement now follows the piece - rigged pieces bind to the skeleton, rigid ones hang on the
  waist (53) or back (12) point.
- Backport Helper: when a build fails only because some looks have **nothing a stock client can draw**
  (a purely 3D back piece such as the Forsaken heritage Backtomb, or a quiver), it lists them and offers to
  leave them out, then finishes the build. The choice is remembered, so adding more sets later does not
  bring them back. The kit itself cannot drop a single look once selected.

## 1.2.0

- **In-game panel** in the WarcraftXL overlay (F9): CSV rows loaded, models attached, stale stock rows
  and missing model files caught this session; log on/off; **Reload CSV files** without restarting.
- **Backport kit converter** (`tools/backport_to_equipplus.py`): turns a wow-retail-backport project's
  3D-piece export into a ready-to-copy folder patch with renamed models, colourway textures and CSV rows.
- **Engine addresses come from WarcraftXL core** instead of private copies, so a core change becomes a
  build error rather than a crash in game; a weekly build against the newest core reports breakage early.

## 1.1.0

- First release as **wxl-equip-plus**, continuing Morfium-G's wxl-equip-extension and Atraxian123's
  WarcraftXL 1.1 port.
- **Safe with stock gear:** stale helm/shoulder model names left in stock `ItemDisplayInfo.dbc` rows are
  ignored on other slots, and every model must exist before it is loaded - no more checkered boxes,
  crashes or shoulder pads on belts from bot and NPC gear.
- Hub packaging (`wxl.json`, store listing), automated build and release, rewritten documentation.
