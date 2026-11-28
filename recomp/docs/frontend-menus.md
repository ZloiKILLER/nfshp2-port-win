# The game's menus (Flailer UI) and the port's own items

How the menus of this disc (SLUS_203.62, NTSC) are built, and how the port adds its own to them.
Code: `recomp/runtime/pcmenu.{h,cpp}` (the mechanism), `pcsettings.cpp` (the PC settings),
`pcconfig.{h,cpp}` (`hp2.ini`). The format is taken apart in `work/ref/NFSHP2-3DS/docs/formats/flailer-*.md`
(the addresses there are PAL / demo ones; everywhere below they are the US retail's).

## How the game's menus are built

- The whole interface is Flailer data in `FRONTEND\FRONTB.BUN` (project `255842fd`, 41 screens; also
  `GLOBALB`, `INGAMEB`). A screen is a list of objects: **text** (`FlailerTextChunk`, a string and a
  `LanguageID` in `FE_*.BIN`) and **bitmap**. In memory these are linked lists: the projects are a `bTList` at
  `gp-22424`, a project has its `ScreenList` (+12), a screen its `ObjectList` (+32); a node has `Next/Prev` at the start
  of its payload, and an empty list points to itself. The retail's object offsets are at the start of `pcmenu.cpp`.
- A menu item is a text object named `MENUITEM_n` (n = 0..15); there are no other tables. The menu is built anew on
  every activation of the screen (`FlailerScreen::Activate`); the navigation, the orange highlight and the Up/Down sound
  are done by the shared code of `FlailerMenu`.
- A screen's behaviour is a C++ class; its `Callback(msg, data)` is registered in a `FlailerScreenConfig`
  (the screen's name for `ChangeToScreen`, the callback, the hash of the Flailer screen). The configs are built by
  `__17FlailerScreenList` (0x16c218) from the list at `gp-21272`. `msg`: `ScreenMessage` from
  `work/ccc/types.cpp` (1 SELECT, 4 HIGHLIGHT, 6/7 PAD_LEFT/RIGHT, 11 ACTIVATE, 12 UNACTIVATE,
  18 TICK); `data` is `ScreenCallbackData {ItemHash, Port, Param, Noise}`; `Noise` is the sound (1 Left, 2 Right).
- The Options, Audio and other screens derive from `OptionsScreenBase` (0x1f4c98): a floating hint at the bottom
  (`tAnimatedBlurb` at `this+100`), two lines `MI_OVERSCORE/MI_UNDERSCORE`, the bars `MI_n_MASK`.
  `tOptionsScreen` (ctor 0x1eebb8, `HandleEvent` 0x1eeca8, `Callback` 0x1eef00): `this+0` is the screen's
  hash, the live instance is at `gp-16888`. On NTSC its ctor hides `MENUITEM_7` ("Languages")
  through `GrayMenuItem(item, 2, screen)`.
- The values on the right (`TRACKMAP`, `UNITS`...) and the arrows `LEFT_ARROW/RIGHT_ARROW` are just objects of Display Options.
  A string can be written directly: `FlailerTextChunk::SetString` (0x140e20) clears the `LanguageID`,
  so `FE_*.BIN` need not be changed. The hint is `tAnimatedBlurb::NewText(char*)` (0x249518).

## The port's mechanism

1. **Hooks on the game's functions.** A line `hook <name>` in `recomp/runtime/hle_names.txt`; `tools/recomp/gen.py`
   keeps the recompiled body as `F_xxxxxxxx_orig` and calls `HOOK_<name>(Ctx &c, void (*orig)(Ctx &))`
   from `pcmenu.cpp`. The hook runs before, after or instead of the original. Names with `$` do not make an
   identifier, so such functions cannot be hooked. `gen.py` writes files only when they change, so a
   new hook rebuilds one piece of the code.
2. **Calls into the game from the host:** `pc_call(address, a0..a3, t0)` (saves and restores the registers).
   The addresses are in `namespace fn` in `pcmenu.cpp`.
3. **Changing the Flailer data in memory**: a hook on `LoaderFlailerChunk`, after `FRONTB` is loaded, clones the
   `OPTIONS` screen (plus the arrows and the value from `DISPLAY`) into a new screen; the memory comes from `bMalloc`
   and **is freed when `FRONTB` is unloaded** (a hook on `UnloaderFlailerChunk`), or the race crashes on a
   large allocation (the heap was left fragmented). Any new allocation in a menu frame goes only this way.
4. **Pages** (`pcmenu.h`): a `PcPage` is a screen of at most 7 rows `PcRow {label, blurb, value(), change(dir)}`.
   `pc_add_page` is called from `pc_settings_register()` before the list of screens is made. A page gets the
   hash `bStringHash("NAME_IN_CAPITALS_WITH_UNDERSCORES")` and its own `FlailerScreenConfig` with the callback
   `tOptionsScreen::Callback`, and `HandleEvent` is hooked: for a page the host handles the events
   (Select/Left/Right → `change`, Highlight → the hint and the arrows), and `TICK` goes to the game.
   `pc_options_row(label, blurb, page)`: the seventh item of Options (once "Languages") opens the page.
5. Helpers: `pc_open_page(name)` (an item that leads to another page; "Back" already works through
   the stack of screens), `pc_refresh_page()`, `pc_set_blurb(text)`, `pc_on_frame(fn)` (once a menu frame).

### How to add a screen (profiles, for example)

```cpp
PcPage &p = pc_add_page("PC Profiles", "Profiles");   // the screen's name and its title
p.rows.push_back({"Driver", "Pick the profile.",
                  [] { return current_profile_name(); },   // the value on the right (null: a row that is an action)
                  [](int dir) { next_profile(dir); }});    // dir -1/+1, 0 for an action row
pc_options_row(...) or a row of another page with pc_open_page("PC Profiles");
```
Limits: no more than 7 rows (the screen is 640x480), a row's text up to 23 characters; for a layout of your own,
clone another screen (`clone_object`/`build_page` in `pcmenu.cpp`).

## Exit in the main menu

The screen `NEW_MAIN_MENU` (`ae0f8aa1`, the class `NewMainMenu`) has three rows 55 apart in its data, but the game's
code finds the rows by their names: the menu takes `MENUITEM_n`, the slide-in `MENUITEM_n_GROUP` and `MI_n_MASK`,
and `SelectionSet` the table at `0x31BE40`, which already has `MI_4_MASK` and `MI_5_MASK`. So when `FRONTB` loads,
`add_exit_row` simply clones the objects of the fourth row and cuts the panel below the third row
(the memory is freed with the bundle, as for the pages). `NewMainMenu::HandleEvent` knows only rows 1..3,
so for the fourth the hook handles Highlight (the animation, the hint) and Select (`window_quit`, the same
path as closing the window). There is no confirmation: Exit closes the game at once.

## Traces of the PS2 (`pcplatform.cpp`)

- **Texts.** `FE_ENGLISH` is changed once when it loads (a hook on `Loader__15LanguageManagerP6bChunk`): `kText` gives a
  new string by `LanguageID`; its entry in the ID table and the same offsets in the table of English hashes are moved
  to a copy in the guest's memory, so every reader sees the new text. On the way, "✕ START" is shortened to "✕" in the
  whole pool: it is one and the same menu button. The title screen's caption is "PRESS ENTER" or "PRESS START" by the device.
- **Icons.** The characters 0x80..0x9d are drawn from the sheet `BUTTONSOUT` (4 bits, 128x256, swizzled, the glyph table at
  `0x2f09c0`). `pc_paint_buttons` paints the whole sheet again in the guest's memory: a key is a keycap with its label in
  the game's `FONT_A`, a gamepad the coloured A/B/X/Y, LB/RB/LT/RT, the d-pad. It is called every menu frame and when the
  player's mode changes, and works only if the bindings, the device or the context changed. The game loads the textures again,
  so the change shows at once (checked).
- **Taken out of the menus.** The Controller screens with the DualShock (their row is now Controls) and Display → Calibrate Display
  (lining the picture up on a TV). The Display screen is called HUD.
- **Saves.** `saves/save.dat` beside the exe, see `work/memcard/README.md`.

## Controls

Two layouts; `pc_car_input()` (`pcmenu.cpp`) chooses between them by `Player::SetInputMode` (a hook) and the state
`CurrentGameFlowState`: the modes 2, 3, 4, 5, 7 are driving, the rest and all of the frontend are the menu.

| | Keyboard | Gamepad |
|---|---|---|
| Menu, fixed | arrows, Enter ✕, Esc △, F1 ○, Space □, Q/E L1/R1, 1/2 L2/R2, Backspace Select | d-pad, stick, A ✕, B △, Y ○ (on PlayStation as on the PS2), X □, LB/RB, LT/RT, Start, Back |
| Driving, can be changed | `key_*` in `hp2.ini`: ↑/W throttle, ↓/S brake, ←/A and →/D steering, Space handbrake, C view, Q look back, H horn, Shift boost, Z/X gears, R reset, Tab HUD | `pad_*`: RT/A throttle, LT/X brake, stick and d-pad steering, RB handbrake, Y view, LB look back, B horn, ↓ boost, L3/R3 gears, Back reset, Start pause |

Esc in a race is always the pause (on the screens inside a race Enter = ✕). In the pause Esc sends START (closes it) and △ (on a
question, "no"). The actions are labelled after the game's layout A; there is no layout screen any more. R3 ("Speed Boost")
had no binding at all before.

## The port's settings

Options: Audio, Controls, Graphics, HUD, Save/Load, Player Names, Credits. All seven rows of the screen are the port's: rows 2 and 3
open the port's pages, rows 4..7 on Select pass the game its item one number lower (`options_event`), the hints are the strings
100021..100026 of `kText`. There is no PC Settings any more.

| Where | Row | What it does | When it takes effect |
|---|---|---|---|
| Audio | Master Volume | the list's first row, 0..100% in steps of 10 (to the speakers only, not into `HP2_WAVOUT`) | at once |
| Graphics | Window | Windowed / Fullscreen (borderless), Alt+Enter as well | at once |
| Graphics | Resolution | the window's size, 800x600..3840x2160; in full screen it is kept for later | at once |
| Graphics | Aspect Ratio | the race's picture 4:3 / 16:10 / 16:9, the menus always 4:3 | after a restart |
| Graphics | Picture Quality | a page: Render Scale 1x..4x of 512x448, Anti-Aliasing Off/2x/4x/8x, Anisotropic Filtering Off/2x/4x/8x/16x | after a restart |
| Graphics | Scaling | Smooth / Sharp | at once |
| Graphics | Show FPS | the frame counter in the corner | at once |
| Controls | Keyboard | three pages of keys for driving: Select, then the key, Esc cancels | at once |
| Controls | Gamepad | Buttons, Stick Dead Zone, Steering, Vibration, Button Icons, Defaults | at once |

Master Volume is the object `MENUITEM_0`: `FlailerMenu` gathers the items 0..15, so it becomes the first. When `FRONTB` loads,
`add_master_row` moves the 26 objects of the table at `0x2b9c30` one row down, and a hook on the constructor sets the visible range
`MENUITEM_0..6` and the starting mask. Audio has its hint at `this+372`, and `this+100` is the volumes: nothing may be written there.

A trap of `gen.py`: a hook by name is silently not made if the frozen table does not give that name to the address (for functions with
`how: neighbour` and a low `ratio`). That happened with `HandleEvent__18AudioOptionsScreen`. The check: `work/recomp/gen` must have
`HOOK_<name>`, otherwise `hle_names.txt` needs the address written out.

The aspect ratio, the 3x-4x scale and the frame's resolution are aimed at the native renderer (`work/gsvk/README.md`); they are not
done on the old path.

## Gamepads (`pad.cpp`, SDL3)

SDL3 3.4.18 is in `work/ref/sdl3` and is built statically, with no sound and no video, the gamepad only. The masks stay XInput bits,
so `hp2.ini` does not depend on the device. `pad_is_playstation` looks at the gamepad's type; `icons` in `hp2.ini` and Button Icons
override it, which is needed, for example, with DS4Windows, where a DualSense shows as an Xbox pad. The icons and the menu layout
depend on the type: PlayStation as on the PS2 (✕ enter, △ back, ○ help), Xbox as on a PC (A, B, Y). On PlayStation `pc_paint_buttons` copies
the disc's original icons into the cells, and for the keyboard and Xbox it draws its own. In a key script the vk `FE` means "as if a
PlayStation gamepad".

`hp2.ini` is beside the exe (made with the defaults; `HP2_INI=<path>` for another file), and environment variables override it.
Only the new renderer draws (`work/gsvk/README.md`): it needs a graphics card with Vulkan, the picture goes out through a swapchain.
Binding: a key that is taken already swaps places with the button that had it.

## Tests

- `HP2_PCGOTO="PC Settings,700"`: on the frontend's 700th tick the game goes to the screen by itself;
  `HP2_PADSCRIPT` (see `hle_iop.cpp`): buttons; `HP2_NATIVESHOT=<file with %d>` and `HP2_NATIVESHOTN=<frames>`: shots of the renderer's
  picture; `HP2_FLDUMP=<file>`: a dump of all Flailer projects (the name, place and text of every object);
  `HP2_CAPTEST=<vk>`: a test key press while binding a key; always `HP2_INI=<test file>`
  and `HP2_MEMCARD=<test folder>`.
- `HP2_KEYSCRIPT=<file>`: the keyboard by a script through the real layouts, without the window's focus: lines
  `<frame or +N> <vk, hex> [frames]`; vk `FF` is "as if a gamepad is in use" (the icons). `HP2_PADSCRIPT` goes around
  the layouts: PS2 buttons directly. The game ignores presses during the animation of a change of screen.
- A race: the command from the task; 30 fps holds, no `FATAL`.
- A trap: bash substitutions eat `\\`, so files with backslashes are written with the Write/Edit tool.

## Not done / known

- Not checked with a real gamepad: the buttons, the stick, the sensitivity and the vibration (`scePadSetActDirect` → `SDL_RumbleGamepad`).
- Display → Player 2 Options stays: the second player is not connected (`scePadRead` of port 1 is empty, except in a test with `HP2_PAD2=1`),
  but the game still counts him.
- The screens inside a race show the driving icons, except ✕ and START; in the FINISH mode, the menu's icons.
- With `HP2_NOSOUND=1`, after going back from a race to the main menu an assert fires in `bSample::LockVoice`
  (2026-11-03, one run, some 30 s into the menu). With the real driver (`HP2_NOAUDIO=1`) the same
  path goes through, so long menu tests are better run with `HP2_NOAUDIO`.

## The mouse in the menus

`mouse_frame` in `pcmenu.cpp`, once a frame from `Tick__17FlailerScreenList`. Pointing at a row of `FlailerMenu` moves the highlight
there, a left click on a row is Enter, the right button does nothing. Everything goes as presses through `window_pad`, so the choice, the
sound and the skipping of grey rows are done by the game's code.
On screens without `FlailerMenu` rows (the car choice, the event tree) there is no pointing, and a click works as Enter. Value arrows:
only the objects `LEFT_ARROW` and `RIGHT_ARROW`; `LEFT_ARROW_SELECT` and the others are not handled yet.
An object's coordinates are in 640x480; on the screen `x * 0.8`, the middle of a row `y * 448 / 480 + 15` (measured on shots of Options and
the pause).
The pointer shows while the player is not driving. `data\ui\cursor.cur` or `data\ui\cursor.ani` replaces the Windows arrow.
A check without a person: `HP2_MOUSESCRIPT=<file>`, lines `<frame> <x> <y> [1 pressed]` in 512x448 coordinates.

## The pause menu

`center_pause_menu`: on the screen `605d2bf1` everything but the darkening is moved by -90 in X, and the logo is centred over the panel.
`HP2_NATIVEEXACT=1` turns it off. The other screens inside the pause are unchanged.

## The event tree's background (issue 39)

The menus' background animation (`tGlobalBackgroundAnimation`, FlailerScreenList +456) takes a random texture of its list at every
screen change and every 50 s; the list is GRID_BLUEPRINT1/2. On the event tree (Championship / Ultimate Racer) the game's
`tCarLogoAnimation::SetEvent` (0x1f8e18) also adds the event's CARLOGO_ textures, and `ChangeToScreen` activates the screen before
it resets the background, so a car logo could slide through the left grid instead of a blueprint (one time in three with a one-car
event; on the PS2 too). `HOOK_SetEvent__17tCarLogoAnimationP9EventInfo` (`pcmenu.cpp`) puts the list back to the two blueprints
after the call; the logo over the map is untouched.
