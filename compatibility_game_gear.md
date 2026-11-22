# Game Gear compatibility

Estimated fluidity of the Game Gear set on the 3DO, measured in the ARM60 cycle model of the emulator (commit 55b71b4, 2026-11-22), not by playing the games. The model predicts the emulator's frame times on the console within a few percent.

- One image per title: 369 titles for 433 images (the regions, revisions and translations of a title run alike). The image measured is the one with a recorded play if any, else an untranslated one, US / European first; the data file in `tools/arm60model/reports/compat/` names it.
- Each title runs 90 s several times: without a button (title and attract mode, whose demos run the game) and with three generic pad sequences that start the game and play it blindly (START 7, 8 or 9 times, so that one of them leaves the game unpaused); the titles with a recorded play run it too. Only the scenes these runs reach are measured.
- **Fluidity**: the speed held nine tenths of the time with the display on, in percent of 60 pictures per second (every game runs on a 60 Hz machine; a PAL 3DO shows 50 of its 60 pictures), to the nearest 5%; the lowest of the runs. Brief freezes are left out of it and given as remarks.
- Titles: 100%: 313; 90-95%: 19; 75-85%: 13; 70% or less: 9; not measured: 15.

| Game | Fluidity | Remarks |
|------|----------|---------|
| 5in1 Funpak | 100% |  |
| Aah Harimanada | 100% |  |
| Addams Family | 100% |  |
| Aerial Assault | 100% |  |
| Aladdin | 100% | Hitches at scene starts |
| Alien 3 | 100% | Hitches at scene starts |
| Alien Syndrome | 100% | Hitches at scene starts |
| Andre Agassi Tennis | 100% | Hitches at scene starts |
| Arcade Classics | 100% | Hitches at scene starts |
| Arch Rivals | 100% |  |
| Arena | 100% | Hitches at scene starts |
| Ariel | 100% |  |
| Arliel | 100% |  |
| Asterix Great Rescue | 100% | Hitches at scene starts |
| Ax Battler | 100% |  |
| Baku Baku Animal | 100% |  |
| Bart Vs Space Mutants | 100% |  |
| Bart Vs The World | 100% |  |
| Bartman | 100% | Hitches at scene starts |
| Batman And Robin | 100% | Hitches at scene starts |
| Batman Forever | 100% | Hitches at scene starts |
| Batman Returns | 100% | Hitches at scene starts |
| Batter Up | 100% | Hitches at scene starts |
| Battleship | 100% |  |
| Battletoads | 100% |  |
| Beavis And Butthead | 100% |  |
| Berenstain Bears | 100% |  |
| Berlin No Kabe | 100% |  |
| Big Hurt Baseball | 100% |  |
| Bonkers Wax Up | 100% |  |
| Bubble Bobble | 100% | Hitches at scene starts |
| Bugs Bunny Double Trouble | 95% | Slowdown in some scenes, Hitches at scene starts |
| Bustamove | 100% | Hitches at scene starts |
| Buster Ball | 100% |  |
| Buster Fight | 100% | Hitches at scene starts |
| Caesars Palace | 100% |  |
| Captain America Avengers | 100% |  |
| Car Licence | 100% |  |
| Casino Funpak | 100% |  |
| Castle Of Illusion | 100% |  |
| Chakan | 100% | Hitches at scene starts |
| Chase Hq | 95% | Slowdown in some scenes, Hitches at scene starts |
| Chessmaster | 100% | Hitches at scene starts |
| Chicago Syndicate | 100% |  |
| Choplifter 3 | 100% | Hitches at scene starts |
| Chuck Rock | 100% |  |
| Chuck Rock 2 | 95% |  |
| Cliffhanger | 100% |  |
| Clutch Hitter | 100% | Hitches at scene starts |
| Coca Cola Kid | 100% |  |
| Columns | 100% |  |
| Cool Spot | - | Black screen |
| Cosmic Spacehead | 100% | Hitches at scene starts |
| Crayon Shinchan | 100% |  |
| Crystal Warriors | 100% |  |
| Cutthroat Island | 100% |  |
| Daffy Duck Hollywood | 100% | Hitches at scene starts |
| Deep Duck Trouble | 100% |  |
| Defenders Of Oasis | 90% | Hitches at scene starts |
| Desert Speedtrap | 100% | Hitches at scene starts |
| Desert Strike | 70% | Hitches at scene starts |
| Devilish | 100% |  |
| Dodge Danpei | 100% | Hitches at scene starts |
| Doraemon | 100% |  |
| Double Dragon | 90% |  |
| Dracula | 100% |  |
| Dragon Bruce Lee | 100% |  |
| Dragon Crystal | 100% |  |
| Drop Zone | 100% |  |
| Dunk Kids | 100% | Hitches at scene starts |
| Dynamite Headdy | 100% | Hitches at scene starts |
| Earthworm Jim | 100% |  |
| Ecco | 100% |  |
| Ecco 2 | 100% | Hitches at scene starts |
| Ernie Els Golf | 55% | Frequent hitches |
| Eternal Legend | 100% |  |
| Excellent Dizzy Collection | 100% |  |
| F1 | 100% | Hitches at scene starts |
| F15 Strike Eagle | 100% |  |
| Faceball 2000 | 100% | Hitches at scene starts |
| Factory Panic | 100% |  |
| Fantastic Dizzy | 100% | Slowdown in some scenes |
| Fantasy Zone Gear | 85% | Hitches at scene starts |
| Fatal Fury Special | 100% | Hitches at scene starts |
| Fifa International Soccer | 100% | Hitches at scene starts |
| Fifa Soccer 96 | 100% | Frequent hitches |
| Foreman For Real | 100% |  |
| Foreman Ko Boxing | 100% |  |
| Fred Couples Golf | 100% |  |
| Galaga 91 | 100% |  |
| Gamble Panic | 100% |  |
| Gambler Jikochuushin Ha | 90% | Hitches at scene starts |
| Ganbare Gorby | 100% |  |
| Garfield | 100% | Frequent hitches |
| Gear Stadium | 100% | Hitches at scene starts |
| Gear Stadium Heiseiban | 100% | Hitches at scene starts |
| Gear Works | 100% |  |
| GG Aleste | 100% |  |
| GG Doraemon | 100% | Hitches at scene starts |
| GG Portrait Pai Chan | 100% |  |
| GG Portrait Yuuki Akira | 100% |  |
| GG Shinobi | 100% | Hitches at scene starts |
| Global Gladiators | - | Black screen |
| Gloc Air Battle | 100% | Hitches at scene starts |
| Godzilla | 100% |  |
| Gp Rider | 80% | Hitches at scene starts |
| Greendog | 80% | Hitches at scene starts |
| Griffin | 100% | Hitches at scene starts |
| Gunstar Heroes | 100% | Hitches at scene starts |
| Halley Wars | 100% |  |
| Head Buster | 100% |  |
| Holyfield Boxing | - | Emulator freezes |
| Home Alone | 100% |  |
| Hook | 100% |  |
| House Of Tarot | 80% |  |
| Hurricanes | 100% | Hitches at scene starts |
| Hyokkori Hyoutan Jima | 100% |  |
| Hyper Chou Pro Yakyuu 92 | 100% |  |
| Ichidant R GG | 100% |  |
| In The Wake Of Vampire | 100% |  |
| Incredible Crash Dummies | 100% |  |
| Incredible Hulk | 100% | Hitches at scene starts |
| Indiana Jones Crusade | 100% |  |
| Iron Man Xo Manowar | 100% | Hitches at scene starts |
| Itchy Scratchy Game | 100% | Hitches at scene starts |
| James Bond 007 | 100% | Hitches at scene starts |
| James Pond 2 | 100% | Hitches at scene starts |
| James Pond 3 | 85% |  |
| Jang Pung 2 | - | Unsupported mapper |
| Jeopardy | 70% |  |
| Jeopardy Sports | 85% |  |
| Jleague Soccer | 100% | Hitches at scene starts |
| Joe Montanas Football | 100% |  |
| Judge Dredd | 100% |  |
| Junction | 100% |  |
| Jungle Book | 70% |  |
| Jurassic Park | 100% | Hitches at scene starts |
| Kaitou Saint Tail | 100% | Hitches at scene starts |
| Kawasaki Superbike | 100% | Hitches at scene starts |
| Kenyuu Densetsu Yaiba | 100% | Hitches at scene starts |
| Kick Rush | 100% | Hitches at scene starts |
| Kinetic Connection | 100% |  |
| Kishin Douji Zenki | 100% | Hitches at scene starts |
| Klax | 100% | Hitches at scene starts |
| Krustys Fun House | 100% | Hitches at scene starts |
| Kunichan Game Tengoku | 100% |  |
| Kunichan Game Tengoku 2 | 100% |  |
| Land Of Illusion | 100% |  |
| Last Action Hero | 100% |  |
| Last Bible | 100% |  |
| Last Bible Special | - | Unsupported video mode |
| Legend Of Illusion | 100% |  |
| Lemmings | 100% |  |
| Lion King | 100% | Hitches at scene starts |
| Lost World | 100% | Occasional hitches |
| Lucky Dime Caper | 100% |  |
| Lunar | 100% |  |
| Madden Nfl 95 | 100% |  |
| Madden Nfl 96 | 100% |  |
| Madou Monogatari 1 | 100% |  |
| Madou Monogatari 2 | 95% | Unsupported video mode, Slowdown in some scenes |
| Madou Monogatari 3 | - | Black screen |
| Madou Monogatari A | 100% |  |
| Magic Knight Rayearth | 100% |  |
| Magic Knight Rayearth 2 | 100% |  |
| Magical Puzzle Popils | 100% |  |
| Magical Taruruutokun | 100% |  |
| Majors Pro Baseball | 100% |  |
| Mappy | 100% |  |
| Marble Madness | 90% |  |
| Markos Magic Football | 100% | Hitches at scene starts |
| Mean Bean Machine | 100% | Hitches at scene starts |
| Megaman | 100% |  |
| Mickey Ultimate Challenge | 100% |  |
| Micro Machines | 100% | Hitches at scene starts |
| Micro Machines 2 | 100% | Hitches at scene starts |
| Mlbpa Baseball | 100% | Hitches at scene starts |
| Moldorian | 100% |  |
| Monster Truck Wars | - | Black screen |
| Monster World 2 | 100% |  |
| Mortal Kombat | 100% | Hitches at scene starts |
| Mortal Kombat 2 | 100% | Hitches at scene starts |
| Mortal Kombat 3 | 100% |  |
| Ms Pacman | 100% | Hitches at scene starts |
| Nazo Puyo | 100% |  |
| Nazo Puyo 2 | 100% | Hitches at scene starts |
| Nazo Puyo Aruru No Ru | 100% | Hitches at scene starts |
| Nba Action | 100% | Hitches at scene starts |
| Nba Jam | 100% | Hitches at scene starts |
| Nba Jam Te | 100% | Hitches at scene starts |
| Neko Daisuki | 100% |  |
| Nfl 95 | 100% | Frequent hitches |
| Nfl Quarterback Club | 100% |  |
| Nfl Quarterback Club 96 | 95% | Hitches at scene starts |
| Nhl Allstar Hockey | 85% |  |
| Nhl Hockey | 100% |  |
| Ninja Gaiden | 100% |  |
| Ninku | 100% |  |
| Ninku 2 | 100% | Hitches at scene starts |
| Ninku Gaiden | 100% |  |
| Nomos Ws Baseball | 100% | Hitches at scene starts |
| Olympic Gold | 100% |  |
| Ottifants | 100% |  |
| Outrun | 75% | Hitches at scene starts |
| Outrun Europa | 100% |  |
| Pacattack | 100% |  |
| Pacman | 100% |  |
| Panzer Dragoon Mini | 100% |  |
| Paperboy | 100% |  |
| Paperboy 2 | 85% |  |
| Pengo | 100% |  |
| Pet Club Inu Daisuki | 100% |  |
| Pete Sampras Tennis | 100% |  |
| Pga Tour 96 | 100% |  |
| Pga Tour Golf | 100% |  |
| Pga Tour Golf 2 | 100% |  |
| Phantasy Star Adv | 100% |  |
| Phantasy Star Gaiden | 90% |  |
| Phantom 2040 | 100% | Hitches at scene starts |
| Pinball Dreams | 100% | Hitches at scene starts |
| Pocket Jansou | 100% |  |
| Poker Faced Paul Blackjack | 100% |  |
| Poker Faced Paul Solitaire | 100% |  |
| Poker Faced Pauls Gin | 100% |  |
| Poker Faced Pauls Poker | 100% |  |
| Pop Breaker | 100% | Slowdown in some scenes |
| Popeyes Beach Volleyball | 100% |  |
| Power Rangers | 100% |  |
| Power Rangers Movie | 100% | Hitches at scene starts |
| Power Strike 2 | 100% | Hitches at scene starts |
| Predator 2 | 100% | Hitches at scene starts |
| Primal Rage | 100% | Hitches at scene starts |
| Prince Of Persia | 95% | Hitches at scene starts |
| Pro Yakyuu 91 | 100% | Hitches at scene starts |
| Pro Yakyuu GG League 94 | 100% | Hitches at scene starts |
| Psychic World | 100% |  |
| Putt Putter | 100% |  |
| Puyo Puyo 2 | 100% | Hitches at scene starts |
| Puzlow Kids | 100% | Hitches at scene starts |
| Puzzle Bobble | 100% | Hitches at scene starts |
| Quiz Gear Fight | 100% |  |
| Rastan Saga | 95% | Slowdown in some scenes, Hitches at scene starts |
| Rbi Baseball 94 | 100% | Hitches at scene starts |
| Rc Grand Prix | 100% |  |
| Ren Stimpy | 100% |  |
| Revenge Of Drancon | 100% |  |
| Riddick Bowe Boxing | 100% |  |
| Rise Of The Robots | 80% | Hitches at scene starts |
| Ristar | 100% | Hitches at scene starts |
| Road Rash | 80% | Frequent hitches |
| Robocop 3 | 100% | Hitches at scene starts |
| Robocop Vs Terminator | 70% |  |
| Ronald Magical World | 100% | Hitches at scene starts |
| Royal Stone | 100% |  |
| Ryu Kyu | 100% |  |
| Sailor Moon S | 100% | Hitches at scene starts |
| Samurai Shodown | 100% | Hitches at scene starts |
| Samurai Spirits | 100% | Hitches at scene starts |
| Scratch Golf | 100% |  |
| Sd Gundam | 100% | Hitches at scene starts |
| Sega Game Pack 4in1 | 95% | Hitches at scene starts |
| Sensible Soccer | 100% | Hitches at scene starts |
| Shadam Crusader | 90% | Hitches at scene starts |
| Shanghai 2 | 100% |  |
| Shaq Fu | 100% |  |
| Shikinjou | 100% |  |
| Shining Force 2 | - | Black screen |
| Shining Force Fc | - | Black screen |
| Shining Force Gaiden | 100% |  |
| Shining Force Gaiden 2 | - | Black screen |
| Shinobi 2 | 100% |  |
| Side Pocket | 100% |  |
| Skweek | 100% |  |
| Slam Dunk | - | Unsupported video mode |
| Slider | 100% |  |
| Smurfs | 100% | Slowdown in some scenes, Hitches at scene starts |
| Solitaire Funpak | 100% |  |
| Solitaire Poker | 100% |  |
| Sonic | 100% |  |
| Sonic 2 | 100% |  |
| Sonic Blast | 100% | Frequent hitches |
| Sonic Chaos | 100% |  |
| Sonic Drift | 100% | Hitches at scene starts |
| Sonic Drift 2 | 90% | Hitches at scene starts |
| Sonic Labyrinth | 100% |  |
| Sonic Spinball | 100% | Hitches at scene starts |
| Sonic Tails | 100% |  |
| Sonic Tails 2 | 100% | Hitches at scene starts |
| Sonic Triple Trouble | 100% | Hitches at scene starts |
| Soukoban | 100% |  |
| Space Harrier | 100% | Stuck on one screen |
| Spiderman Sinister Six | 75% |  |
| Spiderman Vs Kingpin | 100% | Hitches at scene starts |
| Spiderman Xmen | 100% |  |
| Sports Illustrated | 95% |  |
| Sports Trivia | 100% |  |
| Sports Trivia Champ | 100% |  |
| Ss Lucifer | 95% | Graphical artifacts, Hitches at scene starts |
| Star Trek Generations | 100% | Hitches at scene starts |
| Star Trek Tng | 100% |  |
| Star Wars | 100% | Hitches at scene starts |
| Stargate | 100% |  |
| Streets Of Rage | 100% |  |
| Streets Of Rage 2 | 100% |  |
| Strider Returns | 100% |  |
| Super Battletank | - | Black screen |
| Super Columns | 100% |  |
| Super Golf | 100% |  |
| Super Kick Off | 100% | Hitches at scene starts |
| Super Monaco Gp | 100% | Hitches at scene starts |
| Super Monaco Gp 2 | 85% | Hitches at scene starts |
| Super Offroad | 100% | Hitches at scene starts |
| Super Return Of The Jedi | 100% |  |
| Super Smash Tv | 100% |  |
| Super Space Invaders | 100% | Hitches at scene starts |
| Super Tetris | 100% |  |
| Superman | 100% |  |
| Surf Ninjas | 100% |  |
| Sylvan Tale | 100% |  |
| Tails Adventures | 100% |  |
| Tails Sky Patrol | 100% |  |
| Taisen Mahjong Haopai | 100% |  |
| Taisen Mahjong Haopai 2 | 100% |  |
| Taisengata Daisenryaku G | 90% | Hitches at scene starts |
| Tale Spin | 100% |  |
| Tamalympics | 100% |  |
| Tantor | 100% |  |
| Taz Escape From Mars | 100% |  |
| Tazmania | 100% |  |
| Tempo Jr | 100% |  |
| Terminator | 70% | Hitches at scene starts |
| Terminator 2 | 100% |  |
| Terminator 2 Arcade | - | Black screen |
| Tesserae | 100% |  |
| Tintin Au Tibet | 100% |  |
| Tom And Jerry | 100% |  |
| Torarete Tamaruka | 100% |  |
| True Lies | 50% | Hitches at scene starts |
| Ultimate Soccer | 100% |  |
| Urban Strike | 100% |  |
| Vampire | 100% |  |
| Virtua Fighter Animation | 100% |  |
| Virtua Fighter Mini | 100% | Occasional hitches |
| Vr Troopers | 70% |  |
| Wagyan Land | 100% |  |
| Wheel Of Fortune | 100% |  |
| Wimbledon | 100% |  |
| Winter Olympics 94 | 100% |  |
| Wizard Pinball | 100% |  |
| Wolfchild | 100% |  |
| Wonder Boy | 100% |  |
| Wonder Boy Dragons Trap | 100% |  |
| Woody Pop | 100% |  |
| World Class Leader Golf | 95% |  |
| World Cup 94 | 100% |  |
| World Cup Soccer | 100% | Hitches at scene starts |
| World Derby | 100% |  |
| World Series Baseball | - | Black screen |
| World Series Baseball 95 | - | Black screen |
| Wwf Raw | 100% |  |
| Wwf Steel Cage | 100% | Hitches at scene starts |
| Xmen | 100% | Hitches at scene starts |
| Xmen Gamemasters Legacy | 100% | Hitches at scene starts |
| Xmen Mojo World | 100% |  |
| Yuu Yuu Hakusho | 100% |  |
| Yuu Yuu Hakusho 2 | 100% |  |
| Zan Gear | 100% |  |
| Zool | 65% | Hitches at scene starts |
| Zoop | 100% |  |

## Remarks

- **Does not load**: the image is refused (size or layout the emulator does not take).
- **Crash**: a run ended on a fault of the emulator.
- **Emulation stops**: a run ended because the emulator could not go on (translation refused).
- **Emulator freezes**: a run stopped making frames: the emulator itself loops without end.
- **Unsupported mapper**: the image holds data past its first 48 KiB but the game never wrote the Sega or Codemasters paging registers (another mapper: pictures garbled or stuck).
- **Unsupported video mode**: the game draws in the TMS9918 modes of the SG-1000 (register 0 without M4 with the display on); the emulator draws mode 4 only.
- **Black screen**: the display never showed anything but one colour in any run.
- **Stuck on one screen**: the picture never changed in any run.
- **Graphical artifacts**: pictures drawn by the console's cel engine differ from the VDP's reference picture.
- **Slowdown in some scenes**: fluid most of the time, but some 100-frame windows run under 90% of full speed.
- **Frequent hitches**: hitches 3 times a minute or more.
- **Hitches at scene starts**: the picture freezes for a moment while the new code of a scene is translated (game, level, race, fight or menu starts).
- **Occasional hitches**: the picture freezes for a moment in play: code buffer evictions, disc reads of large images, heavy picture rewrites.
- **Not measured**: the model did not finish the runs in time.

Not seen by the model: sound, the feel of the pad, peripherals (light phaser, paddle, sports pad, 3-D glasses), the YM2413 FM sound (the games play their PSG music), wrong pictures that the emulated VDP itself makes (the reference picture comes from it), the scenes the runs do not reach, the console's own costs that the model does not run.
