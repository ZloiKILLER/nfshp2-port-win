Need for Speed: Hot Pursuit 2 - Windows port

INSTALLING
  You need your own copy of the game: Need for Speed: Hot Pursuit 2 for PlayStation 2, USA (SLUS-20362), Europe (SLES-50731, v1.00 or v2.00), Asia (SLPM-65181) or Korea (SLPM-67527),
  as a disc image (.iso) or as the disc itself in a DVD drive.
  Start the downloaded speed.exe and choose Install. It asks for the disc image (Browse) or takes the disc from the drive (From drive),
  and for the folder to install into (any folder you like), and puts everything the game needs there (about 3.6 GB). The disc is only read.
  Then the game starts.

UPDATING
  Start the new speed.exe (anywhere but in the game's folder) and choose Update. It asks for the folder the game is installed in and
  replaces the program and the port's own files there. Your settings, saves, replays and data\replace stay as they are; no disc is needed.

RUNNING
  Start speed.exe in the game's folder (or the shortcut on the desktop). The folder can be moved anywhere: the game needs nothing outside it.
  Profiles: the game asks at every start. Each is a folder of saves\ with its save.dat and controls.ini.
  Two players: Hot Pursuit > Challenge > Players.

SETTINGS
  Options > Graphics   frame rate, draw distance, reflections, frame counter
                       Resolution: window mode, resolution, aspect ratio, render scale up to 8x, picture scaling
                       Advanced: antialiasing (off, MSAA or SMAA, 2x or 4x), anisotropic filtering, brightness, contrast, gamma.
                       They all apply at once, no restart.
  Options > HUD        map, mirror, units, standings and other race display elements
  Options > Audio      master volume and volumes by sound type
  Options > Languages  English, French, German, Italian, Spanish or Swedish: the texts and the speech, at once
  Alt+Enter toggles fullscreen.
  draw_distance in hp2.ini (Options, Graphics): 100 to 400 percent, 100 is the PS2's.
  A cursor.cur or cursor.ani file in data\ui replaces the mouse pointer.

WHAT IS IN THIS FOLDER
  speed.exe            the game
  README.txt           this file
  data\                the game's files from your disc, one file each (data\index.txt lists them), and data\SLUS_203.62, its program.
                       What the disc keeps compressed (tracks, cars, menus) is unpacked here: plain files, ready to edit
  data\lang\           the port's own texts in each language (Options > Languages)
  data\textures\       every texture as a PNG, for viewing and as a base for edits
  data\text\           all of the game's text in every language, for reading
  data\replace\        your replacement textures (see below); empty by default
  saves\               your profiles, a folder each with its save.dat and controls.ini
  hp2.ini              settings
  speed.log            log of the last run, 4 MB at most. If the game crashed, the reason is usually near the end (a FATAL
                       or a "crash:" line). log=off in hp2.ini turns it off
  speed_prev.log       log of the run before the last. After a crash, send this one if the game has been started again since.

REPLACING TEXTURES
  Put a PNG in data\replace\ and restart the game.
    <NAME>.png         replaces every texture with that name
    <NAME>@<crc>.png   replaces one exact texture, when several share a name; the crc is in data\textures\index.csv
  Its size must be the original's times a whole number (x1, x2, x4 ...). Find names in data\textures\.
