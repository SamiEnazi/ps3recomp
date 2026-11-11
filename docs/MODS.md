# Game mods

The port runs the community's inFAMOUS 2 mods: the ones made for tiffsomniac's
**inFAMOUS Mod Manager** (0.2), as they are. You don't need the Manager or its
Unlocker, and your game files are never changed.

## Using mods

1. Unzip the mod into the `mods` folder next to the game (`Play inFAMOUS 2`
   creates it). Each mod is a folder with a `mod.json` in it, for example
   `mods\Red Jacket\mod.json`. A zip that unpacks into a folder of its own name
   one level deeper (`mods\Red Jacket\Red Jacket\mod.json`) works too.
2. Start the game. **F2 > Mods** lists every mod with its name, version,
   author and what it changes. From there you can:
   - turn each mod on or off,
   - pick its options, if the mod has any (`modular.json`: toggles and
     drop-downs),
   - switch every mod off at once with **Use mods**,
   - use **Open the mods folder** and **Look for new mods**.
3. Changes apply the next time the game starts. New mods are on by default,
   with their default options: every toggle, and each drop-down's first option
   by name, as in the Manager.

The page also says why a mod is not used:

- it was made for inFAMOUS or Festival of Blood,
- its `mod.json` doesn't name the game,
- one of its files is broken (for example, a texture whose header doesn't
  match its name),
- it changes the same file as a mod that comes before it.

When two mods change the same thing, the Manager refuses to turn on the second
one. Here the first one by folder name is used and the other is left out
whole. "The same thing" means the same whole file, a texture at the same
offset, an overlapping packed-file patch, or a patch onto a file another mod
replaces. If a file cannot be built, only the mod at fault is left out: each
mod on that file is tried alone, and if each works alone, the last one by
folder name is left out.

Downloaded mods are treated as untrusted:

- every patch is bounds-checked, and a packed-file patch that would replace
  part of a file's own tables is refused;
- a texture patch is written only where this game's copy of the file has a
  texture of exactly that size, format and mip count (found as
  xpps-texture-tools' exporter finds them). A mod made from another version of
  a file is refused rather than written beside the texture: the US digital
  game's `cole_gold_amp.xpps` has its textures 0x110 bytes further on than the
  EU disc's, so Sasha's Black Tar's Solid Gold Amp option does not fit the EU
  disc (untick that option and the rest of the mod works);
- a mod file over 512 MB, or a `mod.json`/`modular.json` over 1 MB, is
  refused;
- junctions and symbolic links inside a mod folder are ignored;
- a mod's files are only ever handed to the game for reading.

With a text language chosen (F2 > Audio > Language, `PS3_TEXT_LANG`), every
`lang_*.xpps` the game asks for is the chosen language's, so a text mod counts
for the language it replaces: a mod's `lang_italian.xpps` is used while the
text is Italian, and a mod's other `lang_*.xpps` is not.

The choices are saved in `mods.ini` beside the menu's `settings.ini`
(`%APPDATA%\inFAMOUS2-port`). **Use mods** is `PS3_GAME_MODS` in
`settings.ini`.

## The mod format (the Manager's)

```
<mod>/mod.json        {"game": "inf2", "name": ..., "author": ..., "version": ...}
<mod>/files/          .xpps files that replace the game's (by file name, any subfolder;
                      a mod with no files/ folder keeps them in the mod folder itself)
<files>/texture_patches/<target>.xpps/[data]<OFFSET>_..._<W>x<H>_<DXTn>_m<mips>.dds
                      a texture written into the target, OFFSET bytes past its data start
                      (the big-endian u32 at +40); the DDS rows are flipped back, as the
                      xpps-texture-tools exporter flips them
<files>/xpps_patches/<target>.xpps/file_<i>_<abs>.bin
                      packed file i of the target replaced whole: its size, the later
                      files' offsets, its chunk's size, later chunks' offsets and the data
                      size are fixed up
<files>/xpps_patches/<target>.xpps/file_<i>_<abs>_offset_<o>.bin
                      these bytes written at offset o inside packed file i
<mod>/modular.json    {"modules": [{"id", "name", "paths": [...]}],
                       "groups":  [{"id", "name", "type": "toggle"|"dropdown",
                                    "options": [{"id", "name", "paths": [...]}]}]}
                      the chosen options' paths (folders of the mod) are the parts used
```

The game's file names are flat (`cole_default.xpps`, `zeke.xpps`), so a file is
matched by its name.

## How it works

The game reads a file from its PSARC archives when one of them has it, and
otherwise reads the loose file under `USRDIR`. The Unlocker relies on this:
it unpacks the five big archives (`infamous1/2`, `patch1/3/4.psarc_s`) into
loose files and moves the archives away. The port does neither
(`runtime/ppu/ppu_fs_mods.h`):

- **Each changed file is hidden from the archives.** The game looks a file up
  by the MD5 of its upper-case name. When an archive is opened, that entry's
  MD5 in the table of contents is made one larger. This happens in the read
  buffer, never on disk. The table isn't sorted, so no other lookup is
  affected.
- **The game then asks for the loose file** (for example
  `/dev_hdd0/game/BCES01143_INSTALL/USRDIR/cole_default.xpps`). The port
  answers with one of:
  - the mod's own file, or
  - for patches, a build of the original with the patches written in. The
    original is read out of the highest archive that has it, in the order
    patch4, patch3, patch1, infamous2, infamous1 (the order in which the
    Unlocker's copies overwrite each other). Builds are cached in
    `mods\.port_cache` and only rebuilt when something changes.

Builds were compared byte for byte with the Manager's own patching (texture
flips, packed-file resizes and chunk patches) and are identical.

| Variable | |
|---|---|
| `PS3_GAME_MODS_DIR` | the mods folder; unset means no mods (tests, the harness) |
| `PS3_GAME_MODS=0` | use none of them |
| `PS3_GAME_MODS_STATE` | the saved choices (default: `mods.ini` beside `settings.ini`) |
| `PS3_GAME_MODS_CACHE` | where builds go (default: `<mods>/.port_cache`) |

Log lines start with `[mods]`: what is used and why something is not, what
was built, what was hidden in which archive, and each file served.

Texture packs (PNG/DDS replacements by content, `PS3_MODS`) are a separate,
port-only feature, also on the Mods page.
