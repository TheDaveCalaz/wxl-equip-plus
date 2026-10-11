# Equip Plus

A stock 3.3.5a client draws a model only for helms, shoulders and held items. Everything else - chest,
belt, legs, boots, gloves, bracers, cloak, shirt, tabard - is painted onto the body as a texture.
**Equip Plus gives every armour slot real 3D models**, so modern armour sets can look the way they were
made to.

- **Two models per armour slot**, on any character or NPC wearing the item.
- **Retail collection models with geoset filtering**: one collection file can dress the chest, legs,
  boots and gloves of a whole set, each slot showing only its own pieces.
- **Bone remapping**, so collection pieces built for a retail skeleton follow the 3.3.5a body.
- **Belts with real buckles**, capes and tabards with their own models.
- **Custom weapons**, including off-hand variants and extra texture layers.
- **Creatures with more than three textures**, with geoset filtering.
- **Equipment animations** synchronised to the wearer.

Everything is driven by data: `ItemDisplayInfo.dbc` and `CreatureDisplayInfo.dbc` rows, plus optional
CSV sidecars for retail data the 3.3.5a tables have no column for. No code changes per item.

**Do not install alongside `wxl-equip-extension`** - Equip Plus is its maintained continuation and uses
the same hooks.

Built on the original wxl-equip-extension by **Morfium-G** and the WarcraftXL 1.1 port by
**Atraxian123**. Full documentation, the CSV column reference and samples are in the repository README.
