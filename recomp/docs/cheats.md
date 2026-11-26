# The game's cheat codes

The game's own table, read from the program (`EasterEggs`, the table at 0x31d828: 78 entries of 56 bytes). An entry is eight buttons
(the events 0x85 to 0x8c: L1, L2, R1, R2, Left, Right, Square, Triangle, read by `EasterEggs::JoyHandler`, 0x24f748, in the menus),
its kind (+32), the number of what it opens (+36), its group (+40) and the text shown when it is entered (+44, a LanguageID).
Entering a code first turns off every entry of the same group that is not 0: the five effects of group 1 are one at a time. `IsCarUnlocked`, `IsTrackUnlocked` and `IsEasterEggUnlocked` ask the table.

The port's Cheat Menu (Options > Extras > Cheat Menu, then F8 in the menus) switches these entries on and off, as typing the code does.

As a track is loaded the game takes one render mode, by the first of these that is on: Motion Blur, Black & White, Heavy Fog, R/C Car,
Video Mode (the mode of Black & White, with an overlay of its own in the HUD), Chrome Cars. Chrome Cars is in no group, but it is the
last of them: with any of the five on it does not show. So the Cheat Menu has these six as one choice. Pet Helicopter, the two bonus
events, the cars and the tracks are each on their own.

| # | Kind | What | Code | Group 1 |
|---|---|---|---|---|
| 0 | Easter egg | Motion Blur | L2, L1, R1, Square, R1, R1, R1, L1 | yes |
| 1 | Easter egg | Black & White | R1, L1, Left, Triangle, Left, Left, Left, L1 | yes |
| 2 | Easter egg | Heavy Fog | Triangle, Square, Right, R2, Right, Right, Right, Square | yes |
| 3 | Easter egg | R/C Car | Right, Triangle, L1, R1, L1, L1, L1, Triangle | yes |
| 4 | Easter egg | Video Mode | L1, R1, Right, Square, Right, Right, Right, R1 | yes |
| 5 | Easter egg | Pet Helicopter | (no code) |  |
| 6 | Easter egg | Championship Bonus Event | L1, Square, R2, R2, R2, R1, L1, Square |  |
| 7 | Easter egg | Ultimate Racer Bonus Event | Right, Square, Left, Left, Left, R2, Right, Square |  |
| 8 | Easter egg | Chrome Cars | L2, R2, Left, Square, Left, Left, Left, R2 |  |
| 9 | Car | McLaren F1 | Triangle, L1, Triangle, L1, R1, Left, R1, Left |  |
| 10 | Car | McLaren F1: NFS | Left, Triangle, Triangle, Triangle, L2, Square, Square, Triangle |  |
| 11 | Car | McLaren F1 LM | Square, L1, Square, L1, Triangle, Right, Triangle, Right |  |
| 12 | Car | McLaren F1 LM: NFS | R1, Square, Square, Square, R2, Left, Left, Square |  |
| 13 | Car | Mercedes CLK-GTR | R2, R1, R2, R1, Left, Triangle, Left, Triangle |  |
| 14 | Car | Mercedes CLK-GTR: NFS | Triangle, Left, Left, Left, L1, L2, L2, Left |  |
| 15 | Car | Chevrolet Corvette Z06 | Left, R2, Left, R2, L1, R1, L1, R1 |  |
| 16 | Car | Chevrolet Corvette: NFS | Square, Right, Right, Right, R2, R1, R1, Right |  |
| 17 | Car | Dodge Viper GTS: NFS | L1, L2, L2, L2, Right, Triangle, Triangle, L2 |  |
| 18 | Car | Ferrari F50 | L1, Triangle, L1, Triangle, Right, L2, Right, L2 |  |
| 19 | Car | Ferrari F50: NFS | Triangle, L1, L1, L1, Left, Right, Right, L1 |  |
| 20 | Car | Lamborghini Diablo 6.0 VT | Right, R2, Right, R2, R1, L1, R1, L1 |  |
| 21 | Car | Lamborghini Diablo: NFS | Square, R2, R2, R2, Triangle, R1, R1, R2 |  |
| 22 | Car | Lamborghini Murcielago | R1, L2, R1, L2, L1, Right, L1, Right |  |
| 23 | Car | Lamborghini Murcielago: NFS | L1, R2, R2, R2, Left, Square, Square, R2 |  |
| 24 | Car | Porsche 911 Turbo: NFS | Right, Left, Left, Left, Triangle, R1, R1, Left |  |
| 25 | Car | Porsche Carrera GT | Left, Right, Left, Right, R1, R2, R1, R2 |  |
| 26 | Car | Porsche Carrera GT: NFS | R2, Triangle, Triangle, Triangle, R1, Square, Square, Triangle |  |
| 27 | Car | Aston Martin V12 Vanquish | R2, Right, R2, Right, Triangle, Left, Triangle, Left |  |
| 28 | Car | V12 Vanquish: NFS | Square, Left, Left, Left, R1, R2, R2, Left |  |
| 29 | Car | BMW Z8 | Square, Right, Square, Right, R2, Triangle, R2, Triangle |  |
| 30 | Car | Ferrari 360 Challenge | Left, Square, Square, Square, R1, L2, L2, Square |  |
| 31 | Car | Ferrari 360 Spider | R2, Square, R2, Square, Triangle, L2, Triangle, L2 |  |
| 32 | Car | Ferrari 550 Barchetta | L1, Square, L1, Square, Right, R1, Right, R1 |  |
| 33 | Car | Ferrari 550 Barchetta: NFS | Square, R1, R1, R1, R2, Left, Left, R1 |  |
| 34 | Car | Mustang Cobra R: NFS | R1, Right, Right, Right, Left, R2, R2, Right |  |
| 35 | Car | HSV Coupe GTS | L1, L2, L1, L2, R1, Triangle, R1, Triangle |  |
| 36 | Car | HSV Coupe GTS: NFS | Right, L2, L2, L2, L1, Left, Left, L2 |  |
| 37 | Car | Jaguar XKR: NFS | L1, Right, Right, Right, L2, R1, R1, Right |  |
| 38 | Car | Ford TS50 | Right, Left, Right, Left, R2, Square, R2, Square |  |
| 39 | Car | Ford TS50: NFS | Right, L1, L1, L1, R1, Left, Left, L1 |  |
| 40 | Car | Lotus Elise | Triangle, R2, Triangle, R2, Left, Square, Left, Square |  |
| 41 | Car | Lotus Elise: NFS | Square, L1, L1, L1, R2, L2, L2, L1 |  |
| 42 | Car | Mercedes CL55 AMG | L1, Right, L1, Right, L2, Square, L2, Square |  |
| 43 | Car | Mercedes CL55 AMG: NFS | Triangle, R2, R2, R2, R1, L1, L1, R2 |  |
| 44 | Car | Opel Speedster: NFS | L2, R2, R2, R2, Square, Left, Left, L2 |  |
| 45 | Car | Vauxhall VX220: NFS | L1, R1, R1, R1, Left, Right, Right, L1 |  |
| 46 | Cop car in You're The Cop | Pursuit Corvette Z06 in You're The Cop Mode | R2, L1, L1, R2, R1, Right, Right, R1 |  |
| 47 | Car | Pursuit Corvette Z06 in all Challenge Modes | L2, R2, R2, L2, Triangle, Right, Triangle, Right |  |
| 48 | Cop car in You're The Cop | Pursuit Murcielago in You're The Cop Mode | Right, L2, L2, Right, Square, Left, Left, Square |  |
| 49 | Car | Pursuit Murcielago in all Challenge Modes | R1, L1, L1, R1, Right, R2, Right, R2 |  |
| 50 | Cop car in You're The Cop | Pursuit Mustang Cobra R in You're The Cop Mode | Left, Triangle, Triangle, Left, R1, R2, R2, R1 |  |
| 51 | Car | Pursuit Mustang Cobra R in all Challenge Modes | Left, Square, Left, Square, R2, R1, R2, R1 |  |
| 52 | Cop car in You're The Cop | Pursuit M5 in You're The Cop Mode | Triangle, Square, Square, Triangle, L1, R1, R1, L1 |  |
| 53 | Car | Pursuit M5 in all Challenge Modes | Square, Triangle, Triangle, Square, Left, R1, Left, R1 |  |
| 54 | Car | Pursuit Crown Victoria in all Challenge Modes | Right, Left, Left, Right, Square, Triangle, Square, Triangle |  |
| 55 | Track, the other way | Coastal Parklands II | R2, R2, Left, Left, Left, R2, R1, L1 |  |
| 56 | Track | National Forest | Square, Square, L2, L2, R1, R1, Left, Left |  |
| 57 | Track, the other way | National Forest II | Left, Left, L1, L1, L1, Left, Right, Square |  |
| 58 | Track, the other way | Scenic Drive II | L2, L2, L1, L1, L1, L2, Triangle, Square |  |
| 59 | Track, the other way | Desert Heat II | R1, R1, R2, R2, R2, R1, L1, Left |  |
| 60 | Track | Outback | Triangle, Triangle, Right, Right, L1, L1, R1, R1 |  |
| 61 | Track, the other way | Outback II | L1, L1, Triangle, Triangle, Triangle, L1, Square, Right |  |
| 62 | Track | Rocky Canyons | R1, R1, Left, Left, Square, Square, Right, Right |  |
| 63 | Track, the other way | Rocky Canyons II | Square, Square, Triangle, Triangle, Triangle, Square, L1, Left |  |
| 64 | Track, the other way | Ancient Ruins II | Right, Right, Left, Left, Left, Right, L1, Square |  |
| 65 | Track | Calypso Coast | R2, R2, R1, R1, L1, L1, Triangle, Triangle |  |
| 66 | Track, the other way | Calypso Coast II | Triangle, Triangle, Right, Right, Right, Triangle, Square, L1 |  |
| 67 | Track, the other way | Mediterranean Paradise II | Square, Square, Right, Right, Right, Square, Left, L2 |  |
| 68 | Track, the other way | Fall Winds II | R2, R2, Square, Square, Square, R2, Left, R1 |  |
| 69 | Track | Alpine Trail | Square, Square, Triangle, Triangle, Left, Left, L2, L2 |  |
| 70 | Track, the other way | Alpine Trail II | Left, Left, R2, R2, R2, Left, R1, Square |  |
| 71 | Track | Autumn Crossing | Right, Right, Triangle, Triangle, R1, R1, Left, Left |  |
| 72 | Track, the other way | Autumn Crossing II | R1, R1, Right, Right, Right, R1, R2, Triangle |  |
| 73 | Track, the other way | Island Outskirts II | L2, L2, R1, R1, R1, L2, Left, R2 |  |
| 74 | Track | Palm City Island | Left, Left, R1, R1, Right, Right, Square, Square |  |
| 75 | Track, the other way | Palm City Island II | Right, Right, R2, R2, R2, Right, R1, Left |  |
| 76 | Track | Tropical Circuit | R2, R2, Triangle, Triangle, Right, Right, R1, R1 |  |
| 77 | Track, the other way | Tropical Circuit II | L1, L1, Right, Right, Right, L1, Square, R2 |  |

The nine easter eggs are the effects and extras: Motion Blur, Black & White, Heavy Fog, R/C Car, Video Mode, Pet Helicopter (it has
no code in the table), the two bonus events and Chrome Cars. The first five are marked open from the start.

Beside the table the pause menu has an item of the developers', Debug (`MENUITEM_8` of the pause screen), which the released game
hides; what it opens has not been looked at.
