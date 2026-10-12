# Changelog

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
