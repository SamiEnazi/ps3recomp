# Input: controllers, keyboard and mouse

`libs/input/cellPad.c` turns the PC's controllers, keyboard and mouse into the
DualShock 3 the game asks `cellPadGetData` for. Everything here is the built-in
behaviour; one optional text file (`input.toml`) and a few environment variables
change it.

## What drives the game

| Source | How it is read | When |
|---|---|---|
| Controllers | SDL2 GameController (DualShock 4/DualSense, Switch Pro and 8BitDo in Switch mode, Xbox, and every pad in SDL_GameControllerDB) | always (also when the window has no focus) |
| Keyboard | by key **position** (US names), sampled at the game's own pad read | while a window of the game has the focus |
| Mouse | raw input on a thread of its own: camera (right stick) and buttons | after a click in the game window ("captured"), while it has the focus and no menu is open |
| `PAD_SCRIPT`, `PAD_FILE`, `PS3_PAD_FILE`, ... | scripted test input | test runs |

Controller, keys and mouse all feed PS3 port 0 together (buttons OR-ed; a stick the
keys or mouse move wins over the pad's). Further controllers go to ports 1-3.

## Default bindings

The keyboard/mouse layout follows the game's own controls page (its
`u_controller_help_*` strings): **L1 is "Aim Mode"** and turns the face buttons into
powers, so aim sits on the right mouse button and the powers under the left hand.

| PS3 | Game action (on foot / while aiming with L1) | Controller (by position) | Keyboard | Mouse |
|---|---|---|---|---|
| Left stick | Move Cole | left stick | W A S D | |
| Right stick | Camera | right stick | I J K L | movement |
| Cross | Jump / *Blast* | bottom (PS Cross, Xbox A, Nintendo B) | Space | |
| Circle | Dive Roll, Drop, cover | right (Circle, B, A) | Left Shift | |
| Square | Melee / *Grenade* | left (Square, X, Y) | F | |
| Triangle | Melee Finisher / *Rocket*; with R1: Pulse Heal | top (Triangle, Y, X) | Q | |
| L1 | Aim Mode | L1 / LB / L | Z | right button |
| R1 | Static Thrusters, Use Object / *Bolt* | R1 / RB / R | E | left button |
| L2 | Electric Drain (hold), Radar Pulse (tap) / *Find Electric Sources* | L2 / LT / ZL | C | button 5 (forward) |
| R2 | Kinetic Pulse (hold to lift, tap to throw) | R2 / RT / ZR | R | button 4 (back) |
| L3 | Ping | left stick click | V | |
| R3 | *Switch Reticule Side* | right stick click | X | middle button |
| D-pad up | Precision Mode | d-pad | 1, Up arrow | wheel up |
| D-pad down | Ionic Vortex | d-pad | 2, Down arrow | |
| D-pad left | Purchase Powers | d-pad | 3, Left arrow | |
| D-pad right | Switch Powers | d-pad | 4, Right arrow | |
| START | Pause Menu | Options / Menu / + | Enter (both) | |
| SELECT | World Map | Share / View / -, **touchpad click** | M, Tab | |

- **Esc** is not a game key: it opens/closes the in-game menu (when there is one) and
  releases a captured mouse. Quit with **Alt+F4** or the window's close button.
  (`PS3_ESC=quit` brings back Esc = quit; binding `Escape` to a control in `[keyboard]`
  gives it to the game instead.)
- **F9** marks the play log ("this felt bad", `PS3RECOMP_PLAYLOG`).
- While Alt (unless bound) or a Windows key is held, keys are a system chord and do not
  reach the game (Alt+Tab does not open the World Map on the way out).
- Controllers are bound by **position**: SDL is told `SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0`,
  so a Nintendo-layout pad's bottom button is Cross like on a DualShock, whatever it is
  labelled. `nintendo_layout = "label"` (below) switches to labels (A = Cross).
- The prompt glyphs follow the device used last: keyboard/mouse prompts show the bound
  key or mouse button (`libs/video/rsx_button_icons.c`).

## Sticks and triggers

Each PS3 stick is shaped from the physical one (`libs/input/pad_stick.h`):

| Setting | Default | Meaning |
|---|---|---|
| dead zone | 0.08 | radial; inside it the stick reads exactly centre (128,128). Past it the output is rescaled from 0, no jump |
| anti dead zone | 0 | where the output starts past the dead zone. The game applies its **own** dead zone after ours; raise this to shrink the total |
| outer | 0.97 | the deflection that already counts as full |
| squircle | 0.5 | 0 = round, 1 = square diagonals. A DualShock 3 reports nearly full X *and* Y on a diagonal; a modern round-gate pad gives 0.71 each, which makes per-axis camera turning slow on diagonals |
| trigger threshold | 0.12 | where L2/R2 count as pressed (digital); the analog pressure is the trigger's travel |
| trigger dead zone | 0.02 | analog L2/R2 below this read 0 |

A centred stick is 128 exactly (the old conversion put SDL's 0 at 127).

**Tilt (SIXAXIS).** The game turns on the DualShock 3's motion sensor and steers the
Ionic Storm with it ("hold ... then tilt the controller left and right"). A pad with an
accelerometer (DualShock 4, DualSense, Switch Pro, 8BitDo in Switch mode) feeds it
directly (SDL sensors, converted to DS3 units: 512 level, ~113 per g, Y 399 at rest).
Without one -- an Xbox pad, the keyboard -- the left stick's X stands in for the tilt
(A/D on the keyboard). `tilt = "left_stick"` uses the stick even on a pad with a
sensor, `tilt = "none"` keeps the pad level, `tilt_invert = true` flips the direction.

## The controls file

`PS3_INPUT_MAP=<path>` names it; without the variable, `input.toml` next to the exe is
used if it exists (`PS3_INPUT_MAP=` set but empty = no file). `docs/input.example.toml`
lists every default binding and setting: copy it and edit. Format: a TOML subset,
sections and `name = value` lines; every section and line is optional, anything left
out keeps its default. Names are case-insensitive; unknown ones are reported on stderr
(`[cellPad] ...: unknown ...`) and ignored. The full reference is the comment at the top
of `libs/input/pad_input_map.h`.

```toml
[keyboard]                  # PS3 control = keys ("" = unbound)
cross    = "Space"
square   = "F"
start    = "Return, Escape" # several keys: a list (Escape here = the game's, not the menu's)

[mouse_buttons]             # Left Right Middle Button4..Button8 WheelUp WheelDown
r1 = "Left"
l1 = "Right"

[mouse]                     # the mouse camera
camera      = true
sensitivity = 1.0           # speed multiplier
invert_y    = false
invert_x    = false
deadzone    = 30            # first stick step past centre (0-120): skips the game's own dead zone
capture     = true          # false: no cursor capture, motion counts whenever the window has focus

[controller]                # PS3 control = controller inputs (SDL position names)
cross = "a"                 # a b x y back guide start leftstick rightstick leftshoulder
r2    = "righttrigger"      # rightshoulder dpup dpdown dpleft dpright misc1 paddle1-4 touchpad
                            # lefttrigger righttrigger leftx- leftx+ lefty- lefty+ rightx- ...

[controller.xbox]           # only for one family: .playstation .xbox .nintendo .other
select = "back"             # (lists only what differs; the rest follows [controller])

[controller_settings]
deadzone          = 0.08    # both sticks, or deadzone_left / deadzone_right
anti_deadzone     = 0.0     # or anti_deadzone_left / _right
outer             = 0.97    # or outer_left / _right
squircle          = 0.5     # or squircle_left / _right
trigger_threshold = 0.12
trigger_deadzone  = 0.02
left_stick        = "left"  # physical stick for the PS3 left stick: left, right, none
right_stick       = "right" # (left_stick = "right" + right_stick = "left" = southpaw)
invert_right_y    = false   # invert_left_x/_left_y/_right_x/_right_y
nintendo_layout   = "position"   # or "label": Nintendo pads by label, A = Cross
tilt              = "sensor"     # SIXAXIS tilt: "sensor" (else left stick), "left_stick", "none"
tilt_invert       = false
```

Rules: an input bound in the file is taken off every control the file does not also
give it to (`cross = "F"` frees F from Square). An input listed under several controls
presses them together: in `[keyboard]`, `l1 = "Z, G"` + `square = "F, G"` makes G a
one-key grenade (L1 + Square; if the game takes it as a melee, aim first). Values may be
quoted or bare; fractions accept `0.08`, `8` or `8%`.

Key names (US positions on any layout): `A`-`Z`, `0`-`9`, `F1`-`F20`, `Space Return Tab
Escape Backspace Delete Insert Home End PageUp PageDown Up Down Left Right`, `Minus Equal
LeftBracket RightBracket Backslash Semicolon Quote Comma Period Slash Grave`, `Shift
RightShift Ctrl RightCtrl Alt RightAlt CapsLock PrintScreen ScrollLock Pause Apps`,
`Numpad0`-`Numpad9 NumpadPlus NumpadMinus NumpadMultiply NumpadDivide NumpadDecimal
NumLock`. (Enter and keypad Enter are one key on Windows.) The Mac names (`Option`,
`KeypadEnter`, ...) and raw macOS key codes `0x00-0x7F` still work.

### Rebinding in the game (F2 > Controls)

The "Buttons and keys" table lists every PS3 control with two key cells, a mouse cell
and a controller cell. Click a cell (Cross on a controller) and press the new input;
right-click (Square) clears a cell; "Reset to defaults" puts every binding back. The
rules are `libs/input/pad_remap.h`:

- One input drives one PS3 control. Taking one another control has asks first:
  **Swap** (that control gets what the cell had; with nothing to give it just loses
  the input) or **Cancel**. A file written by hand may still bind one key to two
  controls (a chord); the menu never makes one.
- Kept for the window and the menu: F1 (HUD), F2 (menu), F3 (lighting A/B), F9
  (play-log mark), F11 and Alt+Enter (fullscreen), Esc (cancels a change), Alt, the
  Windows keys, Print Screen. The PS / Xbox / Home button is the system's.
- Enter and keypad Enter are one key (both are VK_RETURN to Windows): they bind,
  clear and swap together, and the file lists both.
- Controller cells take buttons and triggers by position (bottom = Cross), written to
  `[controller]`; a `[controller.<family>]` line still wins for its family.
- A change applies at once (the next poll and the next prompt glyphs) and is written
  into the same file: `PS3_INPUT_MAP`, else `input.toml` next to the exe (`""` = this
  session only). The file stays as it was -- comments, blank lines, other sections,
  line endings -- only binding values change, and each binding section ends up listing
  every control. It is written to `<file>.tmp` and moved over the old one in one step.
- Tests: the menu's timed script (`PS3RECOMP_OVERLAY_MENU_AT`) takes
  `<s>:bind=<control>=<input>` (`key:F17`, `mouse:Right`, `pad:b`; the control's keys
  become that one key, a mouse or controller input replaces that column) and
  `<s>:bind=reset`.

## Environment variables

| Variable | Default | Effect |
|---|---|---|
| `PS3_INPUT_MAP` | `input.toml` next to the exe | the controls file |
| `PS3_INPUT_MODE` | both | `controller` (keys/mouse ignored) or `keyboard` (pads ignored) |
| `PS3_PAD_SDL` | 1 | `0` = ignore real controllers (test runs). Also turns keyboard/mouse off unless `PS3_KBM=1` |
| `PS3_KBM` | 1 | `0` = keyboard and mouse never reach the game |
| `PS3_PAD_DEADZONE`, `_L`, `_R` | 0.08 | stick dead zone |
| `PS3_PAD_ANTI_DEADZONE`, `_L`, `_R` | 0 | anti dead zone |
| `PS3_PAD_OUTER`, `_L`, `_R` | 0.97 | saturation |
| `PS3_PAD_SQUIRCLE`, `_L`, `_R` | 0.5 | diagonal shape |
| `PS3_PAD_TRIGGER_THRESHOLD` / `_DEADZONE` | 0.12 / 0.02 | triggers |
| `PS3_PAD_SWAP_STICKS` | 0 | 1 = southpaw |
| `PS3_PAD_INVERT` | | any of `lx,ly,rx,ry` |
| `PS3_PAD_TILT`, `PS3_PAD_TILT_INVERT` | sensor, 0 | SIXAXIS tilt source (`sensor`, `left_stick`, `none`) |
| `PS3_PAD_DB` | | extra SDL mapping files (`;`-separated); `gamecontrollerdb.txt` next to the exe is read too |
| `PS3_PAD_POLL_US` | 1000 | minimum gap between two host polls (0 = every read) |
| `PS3_MOUSE`, `PS3_MOUSE_SENS`, `PS3_MOUSE_INVERT_X/Y`, `PS3_MOUSE_DEADZONE`, `PS3_MOUSE_CAPTURE` | | the `[mouse]` settings |
| `PS3_ESC` | menu | `quit` = Esc quits (old behaviour) |
| `PS3_RUMBLE`, `PS3_RUMBLE_STRENGTH` | on, 100 | vibration (0 off, up to 200 %) |
| `PS3_OVERLAY_PAD` | 1 | `0` = L3+R3 does not open the in-game menu |
| `SDL_*` hints | set in code | `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1`, `SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0`, `SDL_JOYSTICK_HIDAPI_PS4_RUMBLE=1`, `..._PS5_RUMBLE=1`, `SDL_JOYSTICK_THREAD=1`; an environment variable of the same name still wins |

Debugging:

| Variable | Prints (stdout) |
|---|---|
| `PS3_PAD_TRACE=1` | `[padtrace]` the packet the game receives on port 0, whenever it changes (buttons by name, sticks, L2/R2 pressure, ms since the last packet) |
| `PS3_PAD_TRACE=2` | the same for every packet (one per game frame) |
| `PS3_PAD_TRACE=all` | changes on every port |
| `PS3_PAD_STATS=1` | `[padstats]` once a second: reads per port, packets, host polls, the cost of the controller read and of the key read |
| `PS3_MOUSE_LOG=1` | mouse motion and buttons as they become the right stick |
| `PS3_RUMBLE_LOG=1` | every vibration request of the game |

Scripted controller through the real SDL path (tests):
`PS3_PAD_VIRTUAL="<sec>:<input>=<value>,..."` attaches an SDL virtual "Xbox One" pad and
sets its inputs at the given seconds after `cellPadInit` (`a=1`, `leftx=16384`,
`righttrigger=32767`, ...). Mapping, bindings, dead zones and triggers then run exactly as
for a real pad; SDL's hardware drivers are off in this mode, so a real controller is
neither read nor touched. (`PAD_SCRIPT` instead injects into the packet after all of it.)
The virtual pad's triggers take 0..32767 like a real one; SDL's automatic mapping for a
virtual pad has no touchpad, so cellPad gives it a full mapping of its own.

Keyboard and mouse without the focus (tests): `PS3_KBM_TEST=1` (with `PS3_KBM=1`) reads
the bound keys without the focus check and counts raw mouse motion without a capture
(the cursor is never grabbed). An injector can then `SendInput` keys nobody types
(bind e.g. `cross = "F17"`) and small mouse moves into a game window that is not in
front, without taking the focus from the desktop.

Start-up lines on stderr name the controls file, the stick settings, the controller
database size and each controller as it connects (name, SDL type, USB id, GUID, whether
SDL's HIDAPI driver reads it, and the mapping SDL applied). `tools/pad_probe.py` shows
what SDL sees outside the game (`--watch 10` for live input).

## The mouse and the window

- Nothing is captured until a click in the game window (a test run's window that takes
  the focus never grabs anyone's cursor). That click does not reach the game.
- Captured = cursor hidden and kept in the window; raw mouse motion turns the camera.
  Released when the window loses focus (Alt+Tab; it comes back with the focus), when a
  menu opens, or with Esc (click again to recapture).
- Motion becomes right-stick deflection proportional to the mouse's speed (a stick is a
  rate), smoothed lightly, starting past the game's own dead zone. The time base is the
  performance counter (it was `GetTickCount64`, 15.6 ms steps: the camera speed jumped
  2x between frames).
- An in-game menu that wants Esc watches `g_pad_overlay_toggle` (also counted up by
  L3+R3 on a controller) and sets `g_pad_overlay_present = 1`; while it is open it sets
  `g_pad_overlay_open` (or adds its own flag to `cellPad_host_overlay_open()`), which
  holds every port neutral and releases the mouse.

## Latency

The game reads the pad once per frame. That read samples the host directly:
`SDL_GameControllerUpdate` (which reads the pads' latest HID reports) and the keyboard
state happen inside `cellPadGetData`, rate-limited to 1 ms on the performance counter
(the old 4 ms gate ran on `GetTickCount64`, so at 60+ fps a read could get a state up to
15.6 ms old). Mouse motion is accumulated by the raw-input thread as the mouse reports
it. Cost per read: ~22 us controller + ~22 us keys (`PS3_PAD_STATS`).

What remains between a press and the picture: the controller's own report interval
(Bluetooth: Switch Pro / 8BitDo ~8-15 ms, a generic Bluetooth dongle ~30 ms; USB 1-8 ms)
-> up to one game frame until the next read (0-16.7 ms at 60) -> the game's own
processing (its simulation/render pipeline, typically 1-2 frames) -> the RSX FIFO and
present (mailbox, frames in flight) -> the display.

## The controller database

`third_party/SDL_GameControllerDB/gamecontrollerdb.txt` (zlib license, see its
`README.txt`) is compiled in: `tools/gen_gamecontrollerdb.py` turns its Windows lines into
`libs/input/gamecontrollerdb_win.h`, and cellPad adds them after SDL's built-in list at
start-up. Pads SDL reads through its HIDAPI drivers keep SDL's own mapping (their GUIDs
carry a driver signature no database line matches). To update: replace the .txt, rerun
the script, note the commit in the README; or drop a newer `gamecontrollerdb.txt` next to
the exe, or point `PS3_PAD_DB` at one.
