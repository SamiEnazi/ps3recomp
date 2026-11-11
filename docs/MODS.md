# Game mods

The port runs the community's inFAMOUS 2 mods: the ones made for tiffsomniac's
**inFAMOUS Mod Manager** (0.2), as they are. You don't need the Manager or its
Unlocker, and your game files are never changed.

## Using mods

1. Add the mod. Either:
   - in the game, **F2 > Mods > Add a mod from a zip...**: the page lists the
     zip files in your Downloads folder and in the mods folder, newest first,
     each already checked ("Sasha's Black Tar 2.0 by sweetasnz -> mods/Sasha's
     Black Tar", or why it cannot be installed; a zip with no `mod.json` in it
     at all is only counted). **Install** puts it in the
     mods folder; a mod whose folder is there already shows **Replace**, which
     keeps the old one until the new one is in place. **Choose another
     file...** opens the Windows file window (over the game; Alt+Tab if it
     stays behind), and the folder you take a zip from is listed from then
     on. After an install the page says what the mod's own checks find, for
     example "15 textures were made for another version of
     cole_gold_amp.xpps: untick Solid Gold Amp";
   - or unzip it yourself into the `mods` folder next to the game (`Play
     inFAMOUS 2` creates it). Each mod is a folder with a `mod.json` in it,
     for example `mods\Red Jacket\mod.json`. A zip that unpacks into a folder
     of its own name one level deeper (`mods\Red Jacket\Red Jacket\mod.json`)
     works too.
2. **F2 > Mods** lists every mod with its preview picture (if its `mod.json`
   names one: `"image": "red_jacket.png"`; click it for a large view, hover
   for a bigger one), name, version, author and what it changes. From there
   you can:
   - turn each mod on or off,
   - pick its options, if the mod has any (`modular.json`: toggles and
     drop-downs), shown under their group's name. A drop-down group, and a
     toggle group whose options all change the same file (one T-shirt
     texture in seven designs), is one at a time: radio buttons. Other
     toggle groups (black tar on the sword, the amp and the outfit) are
     check boxes,
   - choose which mod wins when two clash: **Use this one instead**, on a mod
     left out by a clash, says first what the others lose ("Climbing Gloves
     and Karmic Logo Tshirt will be left out"),
   - switch every mod off at once with **Use mods**,
   - use **Open the mods folder** and **Look for new mods**.
3. Every change is saved at once, and the page shows at once what the next
   start will do with it: "Used from the next start: changes ...", "Left out
   from the next start: it changes the same files as ...". The game uses the
   changes when it starts. **Restart to apply** appears when they differ from
   what this session uses: it saves, closes the game and starts it again. The
   game comes back at its title screen; choose **Continue** there to go on
   from your last save (progress since that save is lost, as when quitting).
   New mods are on by default, with their default options: every toggle, and
   each drop-down's first option by name, as in the Manager.

The page also says why a mod is not used:

- it was made for inFAMOUS or Festival of Blood,
- its `mod.json` doesn't name the game,
- one of its files is broken (for example, a texture whose header doesn't
  match its name),
- it changes the same file as a mod that comes before it.

When two mods change the same thing, the Manager refuses to turn on the second
one. Here the one that comes first is used and the other is left out whole.
First means: the mods you put first with **Use this one instead** (each goes
above every mod it clashes with), then the rest by folder name. "The same thing" means the same
whole file, a texture at the same offset, an overlapping packed-file patch, or
a patch onto a file another mod replaces. If a file cannot be built, only the
mod at fault is left out: each mod on that file is tried alone, and if each
works alone, the last one in that order is left out. The page's view of the
next start uses the clash rule only (nothing is built while you choose); the
start itself builds everything, and a mod whose file could not be built this
time stays shown as not used until it or the mods it failed with change.

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
- a mod's files are only ever handed to the game for reading;
- a preview picture is shown only when `"image"` is a relative path inside
  the mod's folder (no `..`, absolute path, drive letter, network share,
  `:` stream or device name, through no junction or link) to a PNG or JPEG of
  at most 8 MB and 64 M pixels; it is decoded on its own thread and shrunk to
  512 pixels at most. A picture that fails is simply not shown.

A zip is checked whole before anything is written (`runtime/ppu/ppu_fs_mods_zip.h`):

- every name in it must stay inside the mod's folder: no absolute path, `..`,
  drive letter, `:` stream, device name (`CON`, `NUL`, `COM1`...), name
  ending in a dot or space, or character outside plain ASCII; `\` is read as
  `/` and checked the same; no two entries may be the same file;
- a zip with a program anywhere in it (`exe`, `dll`, `bat`, `cmd`, `ps1`,
  `vbs`, `js`, `msi`, `scr`, `com`, `lnk`, `py`, `jar`, `sys`, ...) is refused
  whole: "contains a program, not a mod" (the inFAMOUS 2 Ability Manager zip,
  for one);
- at most 20,000 entries, 512 MB per file and 2 GB in all; ZIP64, zips split
  into parts, password-protected zips and compression other than stored or
  deflate are refused;
- one `mod.json`, at the top or one folder down, that names inFAMOUS 2;
- only mod files are taken out (`json`, `xpps`, `xpp`, `dds`, `bin`, `png`,
  `jpg`, `jpeg`, `txt`, `md`); others are left out and the page says so;
- every file's CRC is checked as it is unpacked. The mod is unpacked into a
  hidden folder inside the mods folder (`.installing-...`) and only renamed
  into place when all of it is there; nothing outside the mods folder is ever
  written or deleted.

With a text language chosen (F2 > Audio > Language, `PS3_TEXT_LANG`), every
`lang_*.xpps` the game asks for is the chosen language's, so a text mod counts
for the language it replaces: a mod's `lang_italian.xpps` is used while the
text is Italian, and a mod's other `lang_*.xpps` is not.

The choices are saved in `mods.ini` beside the menu's `settings.ini`
(`%APPDATA%\inFAMOUS2-port`). **Use mods** is `PS3_GAME_MODS` in
`settings.ini`. `mods.ini` has a section for each mod that is not at its
defaults, and the folders the zip list also looks in:

```
[Sasha's Black Tar]
pick = default,jbintro      the options picked
priority = 2                put first (Use this one instead): a mod with a priority
                            comes before every mod without one, a higher one first
[Young Kessler]
on = 0                      turned off
[.zips]
folder = D:/Downloads       a folder "Choose another file" took a zip from
```

Older builds read the same file: they skip `priority` and `[.zips]`.

**Restart to apply** goes the way **Quit game** does (the game is asked to
exit, a save being written is waited for), then: started by `Play inFAMOUS 2`
(`tools\play.ps1`, which sets `PS3_LAUNCHER_RESTART=1`), the game ends with
exit code `0x52455354` and the launcher starts it again with the same
settings, in the same session folder; started any other way, the game starts
its own exe again with the command line, environment and working directory it
started with (the environment as it was before the menu's settings were
applied), and the new one waits for the old one to end.

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
| `PS3_GAME_MODS_ZIP_DIRS` | more folders the zip list looks in (`;`-separated) |
| `PS3_LAUNCHER_RESTART=1` | a launcher answers **Restart to apply** (exit code `0x52455354`) |
| `PS3_GAME_MODS_TEST` | tests: the page's actions as steps, once, `PS3_GAME_MODS_TEST_AT` s (15) after start: `install=<zip>`, `replace=<zip>`, `pick=<folder>=<id,id>`, `prefer=<folder>`, `plan`, `zips`, `restart`, separated by `\|` |
| `PS3_RESTARTED` | set by a restart in the new game (the count); `PS3_RESTART_WAIT_PID` makes it wait for the old one |

Log lines start with `[mods]`: what is used and why something is not, what
was built, what was hidden in which archive, each file served, each install
(`[mods] zip: ...`) and each mod put first. A restart logs `[restart] ...`.

Tests: `runtime/ppu/tests/test_game_mods.cpp` (the zip reader and every
refusal on crafted zips, install and Replace, the clash plan with priorities,
`mods.ini` old and new, the picture path rules and decoding; with a downloads
folder and the game, the real community zips and the version check).

Texture packs (PNG/DDS replacements by content, `PS3_MODS`) are a separate,
port-only feature, also on the Mods page.
