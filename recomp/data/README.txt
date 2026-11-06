Need for Speed: Hot Pursuit 2 - Windows port

INSTALLING
  You need your own copy of the game: the disc image (.iso) of Need for Speed: Hot Pursuit 2 for PlayStation 2, USA (SLUS-20362).
  Start speed.exe. The first time it asks for the disc image and where to install, and makes the folder nfshp2port there with
  everything the game needs (about 3.6 GB). The disc image is only read. Then the game starts.

RUNNING
  Start speed.exe in nfshp2port (or the shortcut on the desktop). The folder can be moved anywhere: the game needs nothing outside it.

CONTROLS
  In menus (fixed)
    Keyboard:             arrow keys, Enter to confirm, Esc to go back, F1 for help
    Mouse:                point to highlight, left click to confirm
    Xbox gamepad:         D-pad or stick, A to confirm, B to go back, Y for help
    DualSense, DualShock: D-pad or stick, Cross to confirm, Triangle to go back, Circle for help

  In a race (change them in Options > Controls > Keyboard or Gamepad)
                     Keyboard           Xbox                  PlayStation
    Accelerate       Up / W             RT / A                R2 / Cross
    Brake, reverse   Down / S           LT / X                L2 / Square
    Steer            Left/A  Right/D    left stick, D-pad     left stick, D-pad
    Handbrake        Space              RB                    R1
    Speed boost      Left Shift         D-pad down            D-pad down
    Change view      C                  Y                     Triangle
    Look back        Q                  LB                    L1
    Horn, siren      H                  B                     Circle
    Shift up         X                  right stick press     R3
    Shift down       Z                  left stick press      L3
    Reset car        R                  Back                  Create
    Cycle HUD        Tab                D-pad up              D-pad up
    Pause            Esc                Start                 Options

  Alt+Enter toggles fullscreen. On-screen hints show the buttons of the device you are using right now.
  Button icons and the menu layout can be set by hand: Options > Controls > Gamepad > Button Icons.
  A cursor.cur or cursor.ani file in data\ui replaces the mouse pointer.
  draw_distance in hp2.ini (Options, Graphics): 100 to 400 percent, 100 is the PS2's.
  Profiles: the game asks at every start. Each is a folder of saves\ with its save.dat and controls.ini.

TWO PLAYERS (Hot Pursuit > Challenge > Players)
  Two gamepads: one each.
  One gamepad or none: the keyboard is shared in a two-player race. No key is both players': binding one takes it from the other.
                     Player 1           Player 2
    Accelerate       Up                 W
    Brake, reverse   Down               S
    Steer            Left  Right        A  D
    Handbrake        Space              F
    Speed boost      Left Shift         Left Ctrl
    Change view      C                  V
    Look back        Q                  E
    Horn, siren      H                  G
    Shift up         X                  2
    Shift down       Z                  1
    Reset car        R                  B
    Cycle HUD        Tab                T
  On the car select screen, where both choose at once:
    Player 1: arrow keys, Enter to confirm, Esc to go back. Player 2: W A S D, Space to confirm, C to go back.
  Options > Controls > Player 2: player 2's keys, and who gets the only gamepad.

SETTINGS
  Options > Graphics   frame rate, draw distance, frame counter
                       Resolution: window mode, resolution, aspect ratio, render scale up to 8x, picture scaling
                       Advanced: anti-aliasing, anisotropic filtering, brightness, contrast, gamma.
                       They all apply at once, no restart.
  Options > HUD        map, mirror, units, standings and other race display elements
  Options > Audio      master volume and volumes by sound type
  Options > Languages  English, French, German, Italian, Spanish or Swedish: the texts and the speech, at once

WHAT IS IN THIS FOLDER
  speed.exe            the game
  README.txt           this file
  data\                the game's files from your disc, one file each (data\index.txt lists them), and data\SLUS_203.62, its program
  data\lang\           the port's own texts in each language (Options > Languages)
  data\textures\       every texture as a PNG, for viewing and as a base for edits
  data\text\           all of the game's text in every language, for reading
  data\unused\         files on the disc that the game does not load
  data\replace\        your replacement textures (see below); empty by default
  saves\               your profiles, a folder each with its save.dat and controls.ini
  hp2.ini              settings
  speed.log            log of the last run. If the game crashed, the reason is usually near the end (a FATAL line)

REPLACING TEXTURES
  Put a PNG in data\replace\ and restart the game.
    <NAME>.png         replaces every texture with that name
    <NAME>@<crc>.png   replaces one exact texture, when several share a name; the crc is in data\textures\index.csv
  Its size must be the original's times a whole number (x1, x2, x4 ...). Find names in data\textures\.
