# wxl-equip-extension-csv

A [WarcraftXL](https://github.com/WarcraftXL/WarcraftXL) module that dramatically expands what `ItemDisplayInfo.dbc` can do. It extends M2 model attachment support to every equippable slot, adds a second model channel to all slots, and introduces collection M2 support with per-geoset filtering — all driven entirely by DBC data, with no client code changes per item.
Uses .csv sidecars to declare the extra textures newer models have.
---

## Personal Notes

This was a personal project that branched off the main equip extension in order to allow players (me in particular) to use custom models for armors/weapons and/or custom texture variations and still have the funcionality for the extra textures those models may rquire.
Since the original equip extensions at the time was not working for weapons and the db2 powered version is quite hard to use for injecting new models with multiple changeable textures, this side branch was born.
Since I was at it, creatures support was added.

This is mainly aimed at people who want to implement custom models in an easy way. 
If you are interested only in running the retail stuff in your client, then the base module or the db2 module are the better choice.

Needless to say, this is NOT compatible with withe either the base module or the DB2 one, so you have to choose which one to build your WarcraftXL with.

---

## Requirements
- **WarcraftXL** installed in the WoW 3.3.5a client directory (`d3d9.dll` proxy + `WarcraftXL.dll`).
- This module is compiled into `WarcraftXL.dll` automatically — no separate DLL, no additional files needed at runtime.
- If possible, also use the Host extension.

- Take the WXLExtendedEquipment.ini from the samples and instructions folder and put it into the root of the client.
You may now select the size of the creatures models cache and disable eager loading for creatures and weapons.

**NOTE: this specific module uses an event implemented in this branch of the WXL-Core, (has ben pulled into the main branch, so you might not need it anymore).**
https://github.com/Atraxian123/wxl-core
**This fork of the modern assets module that solves an issue with WEAPON_BLADE textures on non weapon items**
https://github.com/Atraxian123/wxl-modern-assets

---
## Credits

https://github.com/Morfium-G
For the original wxl-equipextension module

https://github.com/Furioz420
For the original .csv implementation

---

## Building

This module is auto-globbed by the WarcraftXL CMake build. To include it, clone or copy the `scripts/wxl-equip-extension/` directory into your WarcraftXL source tree, then build normally:

```powershell
$env:PATH = "C:\path\to\cmake\bin;$env:PATH"
.\build.ps1
```

Deploy the resulting `WarcraftXL.dll` and `d3d9.dll` into your WoW client directory as usual.

---

## What changes in `ItemDisplayInfo.dbc`

### Slot coverage

Vanilla 3.3.5a only supports `ModelName_1`, `ModelName_2`, `ModelTexture_1`, and `ModelTexture_2` for Head and Shoulder slots. This module extends support to the slots listed below. Slots not listed (Neck, Rings, Trinkets, weapons) are deferred and not yet handled.

| Slot | Model1 | Model2 | Texture1 | Texture2 | Icon2 settings |
|------|--------|--------|----------|----------|----------------|
| Head | was partial | **added** | - | **added** | **added** |
| Shoulder | was partial | was partial | -  | - | **added** |
| Shirt | **new** | **new** | **new** | **new** | **new** |
| Chest | **new** | **new** | **new** | **new** | **new** |
| Waist | **new** | **new** | **new** | **new** | **new** |
| Legs | **new** | **new** | **new** | **new** | **new** |
| Feet | **new** | **new** | **new** | **new** | **new** |
| Wrist | **new** | **new** | **new** | **new** | **new** |
| Hands | **new** | **new** | **new** | **new** | **new** |
| Back | **new** | **new** | **new** | **new** | **new** |
| Tabard | **new** | **new** | **new** | **new** | **new** |

Each equipped item can now attach up to **two independent M2 models** to any character or NPC wearing that armor.

---

### `ModelName_1` and `ModelName_2` — geoset filtering and collections

The model name fields work as before for simple per-slot models. Two new capabilities are added:

#### Collection M2s

Append a colon and a comma-separated list of geoset IDs to activate a collection:

```
chestplate_paladin_tier2.mdx:1201,2301
```

- The `:` switches the model lookup path from the per-slot directory (`Item\ObjectComponent\<SlotName>\`) to the shared collections directory (`Item\ObjectComponent\collections\`). This matches the retail pathing, so retail-exported collection M2s can be used without renaming.
- Geoset IDs listed after `:` are the **only** geosets that will render. All others are hidden. Geoset `0` (always-on geometry) is never filtered out.
- Combine with **flag `0x40`** (new suffix format) if the collection uses the modern `Race_Gender` naming convention.

#### Standard (non-collection) models

No colon in the name: the lookup path is the normal per-slot directory, suffixed with race and gender as usual (controlled by flags in Icon2, see below).

---

### `ModelTexture_1` and `ModelTexture_2`

These columns work exactly as they did for Head and Shoulders. No format change — they are now honoured for all equippable slots.

---

## Optional WXL sidecar CSVs

DBC still remains the primary source, but the module can also read sidecar CSVs when retail data needs more structure than `ItemDisplayInfo.dbc` provides.

Sidecars are searched in:

- `WXLItemDisplayModels.csv`
- `DBFilesClient\WXLItemDisplayModels.csv`
- `WXLItemDisplayModelMaterials.csv`
- `DBFilesClient\WXLItemDisplayModelMaterials.csv`
- `WXLItemEntryDisplay.csv`
- `DBFilesClient\WXLItemEntryDisplay.csv`
- `WXLCreatureModels.csv`
- `DBFilesClient\WXLCreatureModels.csv`
- `WXLCreatureTextures.csv`
- `DBFilesClient\WXLCreatureTextures.csv`
- `WXLWeaponModels.csv`
- `DBFilesClient\WXLWeaponModels.csv`
- `WXLWeaponTextures.csv`
- `DBFilesClient\WXLWeaponTextures.csv`

- the same `DBFilesClient\...` paths inside every `Data\*.MPQ` patch, including open-folder `.MPQ` patches

Sidecar reads use the client file system first and then the game's IO layer, so packed MPQs and open `.MPQ` folders both work.

### `WXLItemDisplayModels.csv`

Adds structured model rows for a display ID. If at least one model sidecar row exists for a display, the DBC model lists for that display are skipped.

Common columns:

| Column | Meaning |
|--------|---------|
| `DisplayID` | ItemDisplayInfo ID. Required. |
| `Slot` | Slot name or internal model slot index. Examples: `Head`, `Shoulder`, `Chest`, `Glove`, `8`. |
| `Model` / `ModelStem` / `ModelName` | Model stem or path. Required. May include `:geosets`. |
| `Texture` / `TextureStem` / `ModelTexture` | Optional model texture stem. |
| `Folder` | Optional object component folder override, e.g. `Head`, `Shoulder`, `Collections`. |
| `Geosets` | Optional comma-separated geoset filter. Overrides any `:geosets` suffix in `Model`. |
| `Attach` | Optional explicit attachment ID. |
| `SuffixPolicy` | `Exact`/`None`, `Retail`/`New`, `Legacy`/`Race`, or `DBC`/`Slot`. |
| `TexturePolicy` | Same idea as `SuffixPolicy`, but for texture suffixing. |
| `Flags` | Extra Icon2-style model flags ORed into the row. |

### `WXLItemDisplayModelMaterials.csv`

Applies model-specific material textures, including special retail layers that should not be packed into `ModelTexture_1/2`.

Common columns:

| Column | Meaning |
|--------|---------|
| `DisplayID` | ItemDisplayInfo ID. Required. |
| `ModelIndex` | Retail model index or part index. |
| `ModelColumn` | `ModelName_1`, `ModelName_2`, `1`, or `2`. |
| `Model` | Optional model stem filter. |
| `TextureType` | Retail texture type for the material row. |
| `Folder` | Optional object component folder for texture pathing. |
| `Texture` | Texture stem, or `__hide__...` for intentional hidden material rows. Required. |
| `TargetMode` | Targeting behavior, keep`SkinMapOnly`. |

Targeted material rows are applied to virtual collection models instead of globally replacing the model's main texture.

### `WXLItemEntryDisplay.csv`

Sidecar used to declare weapon item IDs and their Display IDs, it was necessary since weapons behave a little differently from armor pieces in the client.

| Column | Meaning |
|--------|---------|
| `ItemID` | Item Id matching the entry in the .dbc |
| `DisplayID` | Item Display Id matching the entry in the .dbc |
| `Folder` | Folder where the model is, either Weapon, Shield or Collections in rare cases when someone put fist weapons there in the listfile |

### `WXLCreatureModels.csv`

Sidecar used to specify which CreatureDisplayID and Creature Models have to be pre-loaded and whith which visible geosets

| Column | Meaning |
|--------|---------|
| `DisplayID` | creature display ID entry as in the CreatureDisplayInfo.dbc |
| `CreatureModelPath` | full path of the model to use for pre-loading ending with the .mdx extension eg. creature\manawyrm2mount\manawyrm2mount.mdx |
| `Geoset` | geosets of the model that will be rendered: empty for all geosets, 0 for only the base skin geosets, "xxxx, yyyy, ..." to render multiple geosets |

The creation of the virtual models is doen eagerly at the start of the client. Note that having many models pre-loaded this way will be very memory intensive. Beware of the 4GB ram cap for the 32bit client.

### `WXLCreatureTextures.csv`

Sidecar used to specify which textures will be baked into the virtual pre-loaded creature model.

| Column | Meaning |
|--------|---------|
| `DisplayID` | creature display ID entry as in the CreatureDisplayInfo.dbc |
| `TextureType` | Texture Type of the texture that will be baked into the pre-loaded model, see the table later in the readme |
| `TexturePath` |  full path of the texture to bake in the pre-loaded model. eg. creature\manawyrm2mount\manawyrm2mount_armor_1.blp |

Note that if the monster model in the CreatureDisplayInfo.dbc entry already has textures declared (for example Monster_1 _2 or _3) adding them into this sidecar will get precedente over the .dbc entry. 

### `Changes to the CreatureModelData.dbc and CreatureDisplayInfo.dbc`

To allow for easy loading of the pre-loaded models by the client, a suffix must be added to the name of the model in the CreatureModelData.dbc.
The suffix is _xxxx there xxxx is the CreatureDisplayID that is specified in the WXLCreatureModel.csv sidecar.

For example: creature\manawyrm2mount\manawyrm2mount.mdx with display ID 33113 will become creature\manawyrm2mount\manawyrm2mount_33113.mdx
Otherwise the new virtual model will be pre-loaded, but never used by the client.

The corresponding CreatureDisplayInfo.dbc etry will have to specify the correct model id from the CreatureModelData.dbc.

### `WXLWeaponModels.csv`

Sidecar used to declare weapon models to bake and their geosets

| Column | Meaning |
|--------|---------|
| `DisplayID` | item display ID entry as in the ItemDisplayInfo.dbc |
| `ModelPath` | full path of the model to use for pre-loading ending with the .mdx extension |
| `Model2Path` | full path of the model to use for pre-loading ending with the .mdx extension |
| `Geoset1` | geosets of the model 1 that will be rendered: empty for all geosets, 0 for only the base skin geosets, "xxxx, yyyy, ..." to render multiple geosets |
| `Geoset1` | geosets of the model 2 that will be rendered: empty for all geosets, 0 for only the base skin geosets, "xxxx, yyyy, ..." to render multiple geosets |

Generally weapons only have 1 model and only skin geosets, but I wanted to give the option to modders to make complex weapons if they wanted.

### `WXLWeaponTextures.csv`

Sidecar for declaring the textures to bake into the weapons

| Column | Meaning |
|--------|---------|
| `DisplayID` | item display ID entry as in the ItemDisplayInfo.dbc |
| `ModelColumn` | 0 or 1 depending if the textures must be baked on the first or second model declared in the previous .csv |
| `TextureType` | Texture Type of the texture to be baked. See tables below for reference. |
| `TexturePath` | full path of the texture file to be baked |

### `Changes to the ItemDisplayInfo.dbc`

In order to make lazy loading and eviction logic for weapons to work, you must add a suffix to the model name similar to creatures: for example, staff_2h_ulatek_d_01.mdx on itemdisplayID = 71027 becomes staff_2h_ulatek_d_01_71027.mdx

---


### `Icon2` — attachment point and flag configuration

The `Icon2` field is unused in stock 3.3.5a and is repurposed to configure how Model1 and Model2 are loaded and attached.

**Format:** `<model1_attachment>:<model2_attachment>:<flags>`

All three parts are optional. Omit a value to keep the slot default. Examples:

| Icon2 value | Meaning |
|-------------|---------|
| *(empty)* | Use slot defaults for both attachment points and all flags |
| `11:55` | Model1 on attachment 11, Model2 on attachment 55 |
| `:34` | Model2 on attachment 34, Model1 uses slot default |
| `::6` | Flags = `0x4 \| 0x2` (no gender or race suffix), attachments at defaults |
| `19:19` | Both models on attachment 19 (Ground_Base) |
| `::96` | Flags = `0x20 \| 0x40` (subfolder + new suffix format), attachments at defaults |

#### Flags

| Flag | Name | Effect |
|------|------|--------|
| `0x1` | Hide built-in geosets | Skip the normal slot geoset rendering. Use when the M2 model contains all the geometry and the built-in slot geosets should not show. |
| `0x2` | No gender suffix | Do not append `_M` / `_F` to the model path. Use for gender-neutral models. Combine with `0x4` to strip both suffixes entirely (the separator `_` is also skipped). |
| `0x4` | No race suffix | Do not append the race code to the model path. Combine with `0x2` to strip both suffixes. |
| `0x8` | Append race to texture | Append the race code to the texture path (textures are normally not suffixed). |
| `0x10` | Append gender to texture | Append the gender code to the texture path. |
| `0x20` | Model in subfolder | Place the model in a subfolder named after the base model (without suffixes). For example, `chestplate_paladin_tier2_HuF.mdx` resolves to `Item\ObjectComponent\Chest\chestplate_paladin_tier2\chestplate_paladin_tier2_HuF.mdx`. Texture paths follow the same subfolder. Useful for packaging models for easy distribution and merging. |
| `0x40` | New suffix format | Use the retail naming convention with an extra underscore between race and gender: `Hu_F` instead of `HuF`. Only applies when the model or texture actually needs suffixing (i.e. `0x2`/`0x4` are not both set, or `0x8`/`0x10` are both set). Lets you use retail-exported models without renaming. |

---

## Default attachment points per slot

If no attachment is specified in `Icon2`, the following defaults apply:

| Slot | Model1 attachment | Model2 attachment |
|------|-------------------|-------------------|
| HEAD | 11 (Helmet) | 55 (HeadTop) |
| SHOULDER | 6 (Left Shoulder) | 5 (Right Shoulder) |
| SHIRT | 34 (Chest) | 34 (Chest) |
| CHEST | 34 (Chest) | 34 (Chest) |
| WAIST | 53 (BeltBuckle) | 53 (BeltBuckle) |
| LEGS | 9 (HipRight) | 10 (HipLeft) |
| FEET | 47 (LeftFoot) | 48 (RightFoot) |
| WRIST | 3 (ElbowRight) | 4 (ElbowLeft) |
| HANDS | 1 (HandRight) | 2 (HandLeft) |
| BACK | 12 (Back) | 12 (Back) |
| TABARD | 34 (Chest) | 34 (Chest) |

---

## Sample DBC rows — Judgement armor set

The examples below are a real two-colour Judgement armor set implemented with this module. Several pieces share a single collection M2 (`plate_raidpaladint2_d_01_chest.mdx`), using geoset filtering to show only the geometry relevant to each slot. Collection models live under `Item\ObjectComponent\collections\palat2\`. The belt uses a standalone per-slot model with the subfolder flag (`0x20`) so it can be packaged in its own directory.
This is mostly meant to show of the various ways to use it, not how it is best to be used.

### Judgement (original)

| ID | Slot | Model1 | Model2 | ModelTexture1 | ModelTexture2 | Icon1 | Icon2 |
|----|------|--------|--------|---------------|---------------|-------|-------|
| 45888 | Head | `palat2\plate_raidpaladint2_d_01_helm.mdx` | | `palat2\plate_raidpaladint2_d_01_helm_be_m_6037071` | | inv_plate_raidpaladint2_d_01_HELM | |
| 34258 | Shoulder | `palat2\plate_raidpaladint2_d_01_shoulder_l.mdx` | `palat2\plate_raidpaladint2_d_01_shoulder_r.mdx` | `palat2\plate_raidpaladint2_d_01_shoulder_l_6037139` | `palat2\plate_raidpaladint2_d_01_shoulder_l_6037139` | inv_plate_raidpaladint2_d_01_shoulder | |
| 33635 | Chest | `palat2\plate_raidpaladint2_d_01_chest.mdx:2201` | | `palat2\plate_raidpaladint2_d_01_chest_be_m_6037063` | | inv_plate_raidpaladint2_d_01_chest | `19` |
| 33633 | Belt | `plate_raidpaladint2_d_01_belt.mdx` | | `plate_raidpaladint2_d_01_belt_6037176` | | inv_plate_raidpaladint2_d_01_belt | `53::38` |
| 33634 | Bracers | | | | | inv_plate_raidpaladint2_d_01_bracer | |
| 33636 | Gloves | `palat2\plate_raidpaladint2_d_01_chest.mdx:401` | `palat2\plate_raidpaladint2_d_01_chest.mdx:2301` | `palat2\plate_raidpaladint2_d_01_chest_be_m_6037063` | `palat2\plate_raidpaladint2_d_01_chest_be_m_6037063` | inv_plate_raidpaladint2_d_01_glove | `19:19` |
| 33637 | Legs | `palat2\plate_raidpaladint2_d_01_chest.mdx:1301` | | `palat2\plate_raidpaladint2_d_01_chest_be_m_6037063` | | inv_plate_raidpaladint2_d_01_pant | `19` |
| 33639 | Boots | `palat2\plate_raidpaladint2_d_01_chest.mdx:2001` | | `palat2\plate_raidpaladint2_d_01_chest_be_m_6037063` | | inv_plate_raidpaladint2_d_01_boot | `19` |

### Judgement Purple (recolor)

| ID | Slot | Model1 | Model2 | ModelTexture1 | ModelTexture2 | Icon1 | Icon2 |
|----|------|--------|--------|---------------|---------------|-------|-------|
| 42853 | Shoulder | `palat2\plate_raidpaladint2_d_01_shoulder_l.mdx` | `palat2\plate_raidpaladint2_d_01_shoulder_r.mdx` | `palat2\plate_raidpaladint2_d_01_shoulder_purple` | `palat2\plate_raidpaladint2_d_01_shoulder_purple` | INV_Shoulder_37 | |
| 42863 | Chest | `palat2\plate_raidpaladint2_d_01_chest.mdx:2201` | | `palat2\plate_raidpaladint2_d_01_chest_purple` | | INV_Chest_Plate11 | `19` |
| 42862 | Belt | `plate_raidpaladint2_d_01_belt.mdx` | | `plate_raidpaladint2_d_01_belt_purple` | | INV_Belt_23 | `53::38` |
| 42851 | Bracers | | | | | INV_Bracer_13 | |
| 43639 | Gloves | `palat2\plate_raidpaladint2_d_01_chest.mdx:401` | `palat2\plate_raidpaladint2_d_01_chest.mdx:2301` | `palat2\plate_raidpaladint2_d_01_chest_purple` | `palat2\plate_raidpaladint2_d_01_chest_purple` | INV_Gauntlets_09 | `19:19` |
| 42859 | Legs | `palat2\plate_raidpaladint2_d_01_chest.mdx:1301` | | `palat2\plate_raidpaladint2_d_01_chest_purple` | | INV_Chest_Cloth_59 | `19` |
| 42864 | Boots | `palat2\plate_raidpaladint2_d_01_chest.mdx:2001` | | `palat2\plate_raidpaladint2_d_01_chest_purple` | | INV_Boots_Chain_08 | `19` |

**Notes on this set:**

- **Chest, Legs, Boots, Gloves** all load geosets from the same shared collection file. Attachment point `19` (Ground_Base) places them relative to the character root rather than a specific bone, which works because the collection M2 already has all bone transforms baked in.
- **Gloves** split into two geoset groups (`401` + `2301`) across Model1 and Model2, both on attachment `19:19`, letting each half of the mesh be textured or toggled independently.
- **Shoulders** use two separate M2 files for left (`_l`) and right (`_r`) pauldrons — the classic per-shoulder split. No Icon2 config needed; the defaults for the Shoulder slot already assign attachment 5 (right) to Model1 and attachment 6 (left) to Model2.
- **Belt** uses Icon2 `53::38` — attachment 53 (BeltBuckle), no attachment for Model2, flags `0x26` (`0x20 | 0x04 | 0x02`): model is in its own subfolder, no race or gender suffix appended.
- **Bracers** have no model entries. The bracer geometry is provided by the character's own skin geosets and does not need an attached M2.

---

## Known issues

**Dirty vanilla DBC entries:** Many stock 3.3.5a `ItemDisplayInfo.dbc` rows have `ModelName_1` or `ModelName_2` populated with values copied from other items (typically shoulders) that were never cleaned up because they had no visual effect in the base client. With this module active, those values will resolve and cause "missing model" placeholder boxes to appear on characters.

**Fix:** Clear `ModelName_1` and `ModelName_2` for any non-head/shoulder item that should not have an attached model. A cleanup script may be provided in a future release.

**Unequipping collections**
Unequipping does not clean up the attachment properly. I'll look into that soon.

---

## Future Plans

Support all slots, like neck/rings/trinkets/etc...

---

## Attachment point reference

The IDs below are the attachment slots present on retroported HD character models:

```c
typedef enum<uint32> {
    AT_Shield_MountMain_ItemVisual0 = 0,
    AT_HandRight_ItemVisual1        = 1,
    AT_HandLeft_ItemVisual2         = 2,
    AT_ElbowRight_ItemVisual3       = 3,
    AT_ElbowLeft_ItemVisual4        = 4,
    AT_Right_Shoulder               = 5,
    AT_Left_Shoulder                = 6,
    AT_Right_Knee                   = 7,
    AT_Left_Knee                    = 8,
    AT_HipRight                     = 9,
    AT_HipLeft                      = 10,
    AT_Helmet                       = 11,
    AT_Back                         = 12,
    AT_ShoulderFlapRight            = 13,
    AT_ShoulderFlapLeft             = 14,
    AT_Bust_ChestBloodFront         = 15,
    AT_Bust2_ChestBloodBack         = 16,
    AT_Face_Breath                  = 17,
    AT_AboveChar_PlayerName         = 18,
    AT_Ground_Base                  = 19,
    AT_Top_of_Head                  = 20,
    AT_SpellLeftHand                = 21,
    AT_SpellRightHand               = 22,
    AT_Special1                     = 23,
    AT_Special2                     = 24,
    AT_Special3                     = 25,
    AT_SheathMainHand               = 26,
    AT_SheathOffHand                = 27,
    AT_SheathShield                 = 28,
    AT_Belly_PlayerNameMounted      = 29,
    AT_LargeWeaponLeft              = 30,
    AT_LargeWeaponRight             = 31,
    AT_HipWeaponLeft                = 32,
    AT_HipWeaponRight               = 33,
    AT_Chest                        = 34,
    AT_HandArrow                    = 35,
    AT_Bullet                       = 36,
    AT_DemolisherVehicle1           = 37,
    AT_DemolisherVehicle2           = 38,
    AT_Vehicle_Seat1                = 39,
    AT_Vehicle_Seat2                = 40,
    AT_Vehicle_Seat3                = 41,
    AT_Vehicle_Seat4                = 42,
    AT_Vehicle_Seat5                = 43,
    AT_Vehicle_Seat6                = 44,
    AT_Vehicle_Seat7                = 45,
    AT_Vehicle_Seat8                = 46,
    AT_LeftFoot                     = 47,
    AT_RightFoot                    = 48,
    AT_ShieldNoGlove                = 49,
    AT_SpineLow                     = 50,
    AT_AlteredShoulderR             = 51,
    AT_AlteredShoulderL             = 52,
    AT_BeltBuckle                   = 53,
    AT_SheathCrossbow               = 54,
    AT_HeadTop                      = 55,
    AT_VirtualSpellDirected         = 56,
    AT_Backpack                     = 57,
    AT_Unk_58                       = 58,
    AT_Unk_59                       = 59,
    AT_Unk_60                       = 60,
} ATTACHMENT_ID;
```
----

## Texture Names Reference

Textures in models have a known enumerator, if you for example are using WoW Blender Studio to edit models you would have surely noticed that changeable textures come with different names instead of a numeric type, those names point to an enumerator and the .csv sidecar cares about that enumerator Texturee.

|Texture Name | Texture Type |Texture Name | Texture Type |
|-------------|--------------|-------------|--------------|
|Hardcoded|0|Monster 3|13|
|Skin|1|Item Icon|14|
|Object Skin|2|Guild Background Color|15|
|Weapon Blade|3|Guild Emblem Color|16|
|Weapon Handle|4|Guild Border Color|17|
|Environment|5|Guild Emblem|18|
|Hair|6|Eyes|19|
|Facial Hair|7|Accessory|20|
|Skin Extra|8|Secondary Skin|21|
|UI Skin|9|Secondary Hair|22|
|Tauren Mane|10|Unknown 23|23|
|Monster 1|11|Unknown 24|24|
|Monster 2|12|-|-|

Ths repository has been tested with ulatek weapons that have Object Skin, Weapon Blade, Weapon Handle and Unknown 24 changeable textures.

---

## Geoset Names Reference

Like textures, also geosets in models have a known name attached to an enumerator. This table will save you the headache of figuring out what enumerator corresponds to what name. You can then associate the geoset number with the geoset name. Woe.export uses geoset names instead of geoset numbers, for example, but the .dbc edits will use the geoset number.

|Geoset Name|Geoset Number|Geoset Name|Geoset Number|Geoset Name|Geoset Number|
|-----------|-------------|-----------|-------------|-----------|-------------|
|Skin|0|Eye Effect|17xx|Necklace|36xx|
|Hair|1xx|Belt|18xx|Headdress|37xx|
|Facial 1|1xx|Bone/Trail|19xx|Tails|38xx|
|Facial 2|2xx|Feet|20xx|MiscAccessory|39xx|
|Facial 3|3xx|BE_NE_Hands/HandAttach|23xx|MiscFeature|40xx|
|Glove|4xx|Horns/HeadAttach|24xx|Noses|41xx|
|Boots|5xx|Head/DHBlindfolds|25xx|HairDecoration|42xx|
|Shirt|6xx|Torso|22xx|HornDecoration|43xx|
|Ears|7xx|Shoulders|26xx|BodySize|44xx|
|Wristbands|8xx|Helmet|27xx|Unknown1|45xx|
|Kneepads|9xx|ArmUpper|28xx|Unknown2|46xx|
|Chest|10xx|ArmsReplace/MechagnomeArms|29xx|Unknown3|47xx|
|Pants|11xx|LegsReplace/MechagnomeLegs|30xx|Unknown4|48xx|
|Tabard|12xx|FeetReplace/MechagnomeFeet|31xx|Unknown5|49xx|
|Trousers|13xx|HeadSwap|32xx|Unknown6|50xx|
|Loincloth|14xx|Eyes|33xx|EyeGlows|51xx|
|Cape|15xx|Eyebrows|34xx|-|-|
|Facialjewelry|16xx|Piercing/Earring|35xx|-|-|

In short, if you are looking at a collections model in Wow.Expor and you see a model with a Chest1 geoset, that geoset corresponds to 1001 or HandAttach3 that geoset corresponds to 2303

---