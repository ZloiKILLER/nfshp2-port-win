Need for Speed: Hot Pursuit 2 - Windows port

INSTALLING
  You need your own copy of the game: Need for Speed: Hot Pursuit 2 for PlayStation 2, USA (SLUS-20362), Europe (SLES-50731, v1.00 or v2.00), Asia (SLPM-65181) or Korea (SLPM-67527),
  as a disc image (.iso) or as the disc itself in a DVD drive.
  Start the downloaded speed.exe and choose Install. It asks for the disc image (Browse) or takes the disc from the drive (From drive),
  and for the folder to install into (any folder you like), and puts everything the game needs there (about 3 GB). The disc is only read.
  Then the game starts.

UPDATING
  Start the new speed.exe (anywhere but in the game's folder) and choose Update. It asks for the folder the game is installed in and
  replaces the program and the port's own files there. Your settings, saves and replays stay as they are; no disc is needed.

RUNNING
  Start speed.exe in the game's folder (or the shortcut on the desktop). The folder can be moved anywhere: the game needs nothing outside it.
  Profiles: the game asks at every start. Each is a folder of saves\ with its save.dat and controls.ini.
  Two players: Hot Pursuit > Challenge > Players.

SAVES FROM A PS2 OR AN EMULATOR (savestransfer.exe)
  The Saves Manager in the game's folder moves saves between the port and PS2 memory cards. It reads PCSX2's memory cards by itself,
  and other cards and save files you add or drop on its window: memory card images .ps2 and .mcd (PCSX2, AetherSX2, NetherSX2),
  PCSX2 folder cards, and .psu, .max and .cbs save files. Saves of every disc the port installs from are taken.
  Import: choose a save, type a name, press Import as new profile. It becomes a new profile; profiles you have are never changed.
  Export: choose a profile, press Export to a card or file..., choose the disc the card is played with and where the save goes
  (a card image, a folder card, or a .psu, .max or .cbs file). It is written as the game writes it, with its icon.
  Delete from card... takes a save off a card. Before a card image is changed, its copy as it was is kept beside it (.bak).
  Help in its window has the details. Close PCSX2, or eject the card, before writing on a card.

SETTINGS
  Options > Graphics   frame rate (30, 60, 120, 144, 180, 240, 300, 360, 500 or Unlimited; above 60 the race still runs at 60
                       with pictures drawn between its frames; with VSync no more than the screen's rate),
                       draw distance (Original, Advanced: twice as far, High: four times, Maximum: six times), level of detail,
                       reflections, shadows (PS2, or High, Maximum, Ultra: in a race the car's own outline from the sun,
                       each finer than the one before), frame counter
                       Resolution: window mode, resolution (in fullscreen too: the picture is stretched to the monitor;
                       ultrawide ones, 21:9 and 32:9, are there too), monitor (the screen the game is shown on),
                       aspect ratio (as wide as the resolution, or 4:3), render scale (20 to 200 % of the resolution),
                       Select GPU (the graphics card that draws the game; changing it restarts the game)
                       Advanced: antialiasing (off, MSAA or SMAA, 2x or 4x), particles (smoke, sparks, dust: Original,
                       or Advanced: the same with soft edges), anisotropic filtering, ambient occlusion (soft shading
                       where surfaces meet), dithering, brightness, contrast, gamma.
                       They all apply at once, no restart (Select GPU alone restarts the game).
  Options > HUD        map, mirror, units, standings and other race display elements
  Options > Audio      master volume and volumes by sound type
  Options > Languages  English, French, German, Italian, Spanish or Swedish: the texts and the speech, at once
  Options > Extras     bonus things. DEBUG.not.pretty: the developers' own Debug menu, an item in the pause menu, as they left it
                       Cheat Menu: with it on, F8 in the menus opens the game's cheats (effects, cars, cop cars, tracks),
                       each On or Off; what is on is remembered
  Alt+Enter toggles fullscreen.
  In hp2.ini only: ambient_occlusion_reach (centimetres, 20 to 400) and ambient_occlusion_strength (percent, 10 to 300),
  shadow_softness (the soft edge of the shadows at High and up, percent, 25 to 200),
  draw_distance (100 to 600 percent, 100 is the PS2's; the menu has 100, 200, 400 and 600).
  A cursor.cur or cursor.ani file in data\ui replaces the mouse pointer.

WHAT IS IN THIS FOLDER
  speed.exe            the game
  savestransfer.exe    the Saves Manager: saves to and from PS2 memory cards and save files (see above); the game does not need it
  README.txt           this file
  data\                the game's files from your disc, one file each (data\index.txt lists them), and data\SLUS_203.62, its program.
                       What the disc keeps compressed (tracks, cars, menus) is unpacked here: plain files, ready to edit
  data\lang\           the port's own texts in each language (Options > Languages)
  saves\               your profiles, a folder each with its save.dat and controls.ini
  hp2.ini              settings
  speed.log            log of the last run, 4 MB at most. If the game crashed, the reason is usually near the end (a FATAL
                       or a "crash:" line). log=off in hp2.ini turns it off
  speed_prev.log       log of the run before the last. After a crash, send this one if the game has been started again since.
