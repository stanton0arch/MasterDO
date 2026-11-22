# Master System compatibility

Estimated fluidity of the Master System set on the 3DO, measured in the ARM60 cycle model of the emulator (commit 39b2ab3, 2026-11-22), not by playing the games. The model predicts the emulator's frame times on the console within a few percent.

- One image per title: 365 titles for 488 images (the regions, revisions and translations of a title run alike). The image measured is the one with a recorded play if any, else an untranslated one, US / European first; the data file in `tools/arm60model/reports/compat/` names it.
- Each title runs 90 s several times: without a button (title and attract mode, whose demos run the game) and with three generic pad sequences that start the game and play it blindly; the titles with a recorded play run it too. Only the scenes these runs reach are measured.
- **Fluidity**: the speed held nine tenths of the time with the display on, in percent of 60 pictures per second (every game runs on a 60 Hz machine; a PAL 3DO shows 50 of its 60 pictures), to the nearest 5%; the lowest of the runs. Brief freezes are left out of it and given as remarks.
- Titles: 100%: 308; 90-95%: 27; 75-85%: 15; 70% or less: 3; not measured: 12.

| Game | Fluidity | Remarks |
|------|----------|---------|
| 20em1 | 100% |  |
| Ace Of Aces | - | Black screen |
| Action Fighter | 100% |  |
| Addams Family | 100% |  |
| Aerial Assault | 100% | Hitches at scene starts |
| After Burner | 100% | Hitches at scene starts |
| Air Rescue | 100% |  |
| Akai Koudan Zillion | 100% |  |
| Aladdin | 100% | Hitches at scene starts |
| Aleste | 100% |  |
| Alex Kidd Bmx Trial | 100% |  |
| Alex Kidd Hightech | 100% |  |
| Alex Kidd Lost Stars | 100% | Hitches at scene starts |
| Alex Kidd Miracle | 100% |  |
| Alex Kidd Shinobi | 100% | Hitches at scene starts |
| Alf | 85% |  |
| Alien 3 | 100% | Hitches at scene starts |
| Alien Storm | 100% | Hitches at scene starts |
| Alien Syndrome | 100% | Hitches at scene starts |
| Altered Beast | 100% |  |
| American Baseball | 100% | Hitches at scene starts |
| American Pro Football | 100% | Hitches at scene starts |
| Andre Agassi Tennis | 100% |  |
| Anmitsu Hime | 100% |  |
| Arcade Smash Hits | 90% |  |
| Argos No Juujiken | 100% |  |
| Ariel | 100% |  |
| Ashura | 100% |  |
| Assault City Pad | 100% | Hitches at scene starts |
| Assault City Phaser | 100% | Hitches at scene starts |
| Asterix | 100% | Hitches at scene starts |
| Asterix Great Rescue | 100% | Hitches at scene starts |
| Asterix Secret Mission | 100% | Hitches at scene starts |
| Astro Flash | 100% |  |
| Astro Warrior | 100% |  |
| Astro Warrior Pit Pot | 100% |  |
| Aztec Adventure | 100% |  |
| Back To The Future 2 | 100% | Hitches at scene starts |
| Back To The Future 3 | - | Black screen |
| Baku Baku Animals | 100% |  |
| Bank Panic | 100% |  |
| Bart Vs Space Mutants | 100% | Hitches at scene starts |
| Bart Vs The World | 100% | Hitches at scene starts |
| Basket Ball Nightmare | 100% | Hitches at scene starts |
| Batman Returns | 100% |  |
| Battle Out Run | 100% |  |
| Battlemaniacs | 100% |  |
| Black Belt | 100% | Hitches at scene starts |
| Blade Eagle 3d | 100% |  |
| Bomber Raid | 100% |  |
| Bonanza Bros | 100% | Hitches at scene starts |
| Bonkers Wax Up | 100% |  |
| Bubble Bobble | 100% | Hitches at scene starts |
| Buggy Run | 100% | Hitches at scene starts |
| Buster Douglas Boxing | 85% |  |
| California Games | 100% | Graphical artifacts, Hitches at scene starts |
| California Games 2 | 85% | Hitches at scene starts |
| Captain Silver | 100% |  |
| Carmen Sandiego | 100% | Graphical artifacts |
| Casino Games | 100% |  |
| Castelo Ra Tin Bum | 95% | Hitches at scene starts |
| Castle Of Illusion | 100% |  |
| Champions Of Europe | 100% |  |
| Championship Hockey | 100% | Slowdown in some scenes, Hitches at scene starts |
| Chapolim X Dracula | 100% | Hitches at scene starts |
| Chase Hq | 90% | Hitches at scene starts |
| Cheese Catastrophe | 100% | Hitches at scene starts |
| Choplifter | 100% | Graphical artifacts |
| Chouon Senshi Borgman | 100% | Hitches at scene starts |
| Chuck Rock | 95% | Slowdown in some scenes, Hitches at scene starts |
| Chuck Rock 2 | 100% | Slowdown in some scenes |
| Circuit | 80% | Hitches at scene starts |
| Cloud Master | 100% |  |
| Columns | 100% |  |
| Cool Spot | - | Black screen |
| Cosmic Spacehead | 100% | Hitches at scene starts |
| Cyber Shinobi | 100% |  |
| Cyborg Hunter | 100% | Hitches at scene starts |
| Daffy Duck Hollywood | 100% | Hitches at scene starts |
| Danan The Jungle Fighter | 100% |  |
| Dead Angle | 100% | Hitches at scene starts |
| Deep Duck Trouble | 95% | Slowdown in some scenes, Hitches at scene starts |
| Desert Speedtrap | 95% | Slowdown in some scenes, Hitches at scene starts |
| Desert Strike | 100% | Hitches at scene starts |
| Dick Tracy | 100% |  |
| Dodgeball King | - | Black screen |
| Double Dragon | 100% |  |
| Double Hawk | 100% |  |
| Double Target | 100% |  |
| Dr Hello | - | Unsupported video mode |
| Dracula | 100% |  |
| Dragon Bruce Lee | 100% |  |
| Dragon Crystal | 100% | Hitches at scene starts |
| Dump Matsumoto | 100% | Hitches at scene starts |
| Dynamite Duke | 100% | Hitches at scene starts |
| Dynamite Dux | 100% |  |
| Dynamite Headdy | 100% | Hitches at scene starts |
| Earthworm Jim | 100% |  |
| Ecco | 100% | Hitches at scene starts |
| Ecco 2 | 100% | Graphical artifacts, Hitches at scene starts |
| Enduro Racer | 100% |  |
| Eswat | 100% | Hitches at scene starts |
| F1 Championship | 100% | Slowdown in some scenes, Hitches at scene starts |
| F16 Fighter | - | Unsupported video mode |
| F16 Fighting Falcon | - | Unsupported video mode |
| Fantastic Dizzy | 100% |  |
| Fantasy Zone | 100% |  |
| Fantasy Zone 2 | 95% |  |
| Fantasy Zone Maze | 100% | Hitches at scene starts |
| Felipe Em Acao | 100% |  |
| Ferias Frustradas Picapau | 100% | Hitches at scene starts |
| Fifa International Soccer | 100% | Hitches at scene starts |
| Final Bubble Bobble | 100% | Hitches at scene starts |
| Fire Forget 2 | 90% |  |
| Fire Ice | 100% | Hitches at scene starts |
| Flash | 100% | Hitches at scene starts |
| Flintstones | 100% |  |
| Foreman Ko Boxing | 90% | Hitches at scene starts |
| Forgotten Worlds | 100% | Hitches at scene starts |
| Fushigi No Oshiro Pit Pot | 100% |  |
| Gain Ground | 100% |  |
| Galactic Protector | 100% |  |
| Galaxy Force | 100% | Hitches at scene starts |
| Gamebox Esportes Radicais | 50% | Hitches at scene starts |
| Gangster Town | 100% | Graphical artifacts |
| Gauntlet | 80% | Hitches at scene starts |
| Geraldinho | 100% |  |
| Ghost House | 100% | Hitches at scene starts |
| Ghostbusters | 100% |  |
| Ghouls N Ghosts | 100% | Hitches at scene starts |
| Global Defense | 100% |  |
| Global Gladiators | 100% |  |
| Gloc Air Battle | 100% | Hitches at scene starts |
| Golden Axe | 100% | Hitches at scene starts |
| Golden Axe Warrior | 95% | Slowdown in some scenes, Hitches at scene starts |
| Golf Mania | 100% |  |
| Golvellius | 100% |  |
| Gp Rider | 95% | Hitches at scene starts |
| Great Baseball | 100% | Hitches at scene starts |
| Great Basketball | 100% | Hitches at scene starts |
| Great Football | 100% | Hitches at scene starts |
| Great Golf | 100% | Hitches at scene starts |
| Great Ice Hockey | 100% | Hitches at scene starts |
| Great Soccer | 100% |  |
| Great Volleyball | 100% |  |
| Haja No Fuuin | 100% |  |
| Hangon | 85% | Hitches at scene starts |
| Hangon Astro Warrior | 85% |  |
| Hangon Safari Hunt | 80% | Hitches at scene starts |
| Heavyweight Champ | 80% | Hitches at scene starts |
| Heroes Of The Lance | 100% | Hitches at scene starts |
| Hokuto No Ken | 100% | Hitches at scene starts |
| Home Alone | 100% | Hitches at scene starts |
| Hoshi Wo Sagasite | 100% |  |
| Impossible Mission | 100% | Slowdown in some scenes, Hitches at scene starts |
| Incredible Crash Dummies | 95% |  |
| Incredible Hulk | 100% | Hitches at scene starts |
| Indiana Jones Crusade | 100% |  |
| James Bond 007 | 100% | Slowdown in some scenes, Frequent hitches |
| James Pond 2 | 100% |  |
| Joe Montana Football | 100% |  |
| Jogos De Verao 2 | 100% | Hitches at scene starts |
| Jungle Book | 100% |  |
| Jurassic Park | 100% | Hitches at scene starts |
| Kenseiden | 100% |  |
| Kimengumi | 100% |  |
| Kings Quest | 100% |  |
| Klax | 100% | Hitches at scene starts |
| Krustys Fun House | 100% | Hitches at scene starts |
| Kujaku Ou | 100% |  |
| Kung Fu Kid | 100% |  |
| Land Of Illusion | 100% |  |
| Laser Ghost | 100% | Hitches at scene starts |
| Legend Of Illusion | 100% |  |
| Lemmings | 100% |  |
| Line Of Fire | 100% | Hitches at scene starts |
| Lion King | 100% | Hitches at scene starts |
| Loletta No Syouzou | - | Unsupported video mode |
| Lord Of The Sword | 100% | Hitches at scene starts |
| Lucky Dime Caper | 100% | Hitches at scene starts |
| Machine Gun Joe | 100% |  |
| Mahjong Sengoku Jidai | 100% |  |
| Makai Retsuden | 100% |  |
| Marble Madness | 100% |  |
| Marksman Shooting | 100% | Stuck on one screen |
| Master Of Darkness | 100% |  |
| Masters Of Combat | 100% | Hitches at scene starts |
| Maze Hunter 3d | 100% |  |
| Maze Walker | 100% | Hitches at scene starts |
| Mean Bean Machine | 100% | Hitches at scene starts |
| Megumi Rescue | 100% | Hitches at scene starts |
| Mercs | 100% | Hitches at scene starts |
| Mickey Ultimate Challenge | 100% |  |
| Micro Machines | 100% | Hitches at scene starts |
| Miracle Warriors | 100% |  |
| Missile Defense 3d | 100% | Hitches at scene starts |
| Monica Castelo Dragao | 100% | Hitches at scene starts |
| Monica O Resgate | 100% |  |
| Monopoly | 100% | Hitches at scene starts |
| Montezumas Revenge | 100% |  |
| Moonwalker | 100% |  |
| Mortal Kombat | 100% | Hitches at scene starts |
| Mortal Kombat 2 | 100% | Hitches at scene starts |
| Mortal Kombat 3 | 100% | Hitches at scene starts |
| Ms Pacman | 95% | Slowdown in some scenes |
| My Hero | 100% |  |
| Nekyuu Kousien | 100% | Hitches at scene starts |
| New Zealand Story | 100% |  |
| Ninja | 100% |  |
| Ninja Gaiden | 100% |  |
| Opa Opa | 100% | Hitches at scene starts |
| Operation Wolf | 100% |  |
| Ottifants | 100% | Hitches at scene starts |
| Outrun | 100% | Hitches at scene starts |
| Outrun 3d | 90% | Hitches at scene starts |
| Outrun Europa | 100% |  |
| Pacmania | 100% |  |
| Paperboy | 100% |  |
| Parlour Games | 100% |  |
| Party Games | 100% |  |
| Penguin Land | 100% | Hitches at scene starts |
| Pga Tour Golf | 100% |  |
| Phantasy Star | 95% |  |
| Pit Fighter | 85% |  |
| Populous | 100% | Hitches at scene starts |
| Poseidon Wars 3d | 100% | Graphical artifacts, Hitches at scene starts |
| Power Strike | 100% |  |
| Power Strike 2 | 100% |  |
| Predator 2 | 95% | Hitches at scene starts |
| Prince Of Persia | 100% | Hitches at scene starts |
| Pro Wrestling | 100% | Hitches at scene starts |
| Pro Yakyuu Pennant Race | 100% | Hitches at scene starts |
| Promocao Ms 3 Compact | 100% |  |
| Psychic World | 100% | Hitches at scene starts |
| Psycho Fox | 100% | Hitches at scene starts |
| Putt Putter | 100% | Hitches at scene starts |
| Quartet | 100% |  |
| Rainbow Islands | 100% | Hitches at scene starts |
| Rambo 2 | 100% |  |
| Rambo 3 | 100% |  |
| Rampage | 100% |  |
| Rampart | 100% |  |
| Rastan | 95% | Slowdown in some scenes, Hitches at scene starts |
| Rc Grand Prix | 100% | Occasional hitches |
| Reggie Jackson Baseball | 100% | Hitches at scene starts |
| Ren Stimpy | 100% |  |
| Renegade | 100% | Hitches at scene starts |
| Rescue Mission | 100% | Hitches at scene starts |
| Road Rash | 80% | Frequent hitches |
| Robocop 3 | 100% | Hitches at scene starts |
| Robocop Vs Terminator | 100% |  |
| Rocky | 100% | Hitches at scene starts |
| Rtype | 100% |  |
| Running Battle | 100% |  |
| Sagaia | 100% |  |
| Sangokushi 3 | - | Unsupported mapper |
| Sapo Xule Invasores Brejo | 100% | Hitches at scene starts |
| Sapo Xule Lagoa Poluida | 100% |  |
| Sapo Xule Mestre Kung Fu | 100% | Hitches at scene starts |
| Satellite 7 | 100% |  |
| Scramble Spirits | 100% | Hitches at scene starts |
| Sdi | 100% |  |
| Secret Commando | 100% |  |
| Sega Chess | 100% |  |
| Sega World Tourn Golf | 85% | Hitches at scene starts |
| Seishyun Scandal | 100% |  |
| Sensible Soccer | 100% |  |
| Shadow Dancer | - | Black screen |
| Shadow Of The Beast | 70% | Graphical artifacts, Hitches at scene starts |
| Shanghai | 95% | Hitches at scene starts |
| Shinobi | 95% |  |
| Shooting Gallery | 100% |  |
| Sitio Do Picapau Amarelo | 95% | Frequent hitches |
| Slap Shot | 100% |  |
| Smurfs | 100% | Hitches at scene starts |
| Smurfs Travel The World | 100% |  |
| Solomon No Kagi | 100% |  |
| Sonic | 100% |  |
| Sonic 2 | 100% |  |
| Sonic Blast | 95% | Frequent hitches |
| Sonic Chaos | 100% | Hitches at scene starts |
| Sonic Spinball | 100% | Hitches at scene starts |
| Space Harrier | 100% |  |
| Space Harrier 3d | 100% | Hitches at scene starts |
| Spacegun | 100% |  |
| Special Criminal | 100% | Hitches at scene starts |
| Speedball | 100% |  |
| Speedball 2 | 95% | Slowdown in some scenes, Hitches at scene starts |
| Spellcaster | 100% |  |
| Spiderman Sinister Six | 100% |  |
| Spiderman Vs Kingpin | 100% | Slowdown in some scenes |
| Sports Pad Football | 100% | Hitches at scene starts |
| Sports Pad Soccer | 80% |  |
| Spy Vs Spy | 100% | Hitches at scene starts |
| Star Wars | 100% | Hitches at scene starts |
| Street Fighter 2 | 100% | Occasional hitches |
| Streets Of Rage | 100% |  |
| Streets Of Rage 2 | 100% |  |
| Strider | 100% | Hitches at scene starts |
| Strider 2 | 100% |  |
| Submarine Attack | 100% |  |
| Sukeban Deka 2 | 100% |  |
| Summer Games | 100% |  |
| Super Kick Off | 100% | Hitches at scene starts |
| Super Mario Bros | 100% |  |
| Super Monaco Gp | 80% | Hitches at scene starts |
| Super Monaco Gp 2 | 90% | Hitches at scene starts |
| Super Offroad | 100% | Hitches at scene starts |
| Super Racing | 100% |  |
| Super Smash Tv | 95% |  |
| Super Space Invaders | 100% | Slowdown in some scenes, Hitches at scene starts |
| Super Tennis | 100% | Hitches at scene starts |
| Super Wonder Boy | 100% |  |
| Super Wonder Boy Mw | 100% |  |
| Superman | 100% |  |
| Taz Escape From Mars | 100% |  |
| Tazmania | 100% |  |
| Tecmo World Cup 93 | 100% | Hitches at scene starts |
| Teddy Boy | 100% |  |
| Teddy Boy Blues | 100% |  |
| Tennis Ace | 100% |  |
| Tensai Bakabon | 100% |  |
| Terminator | 100% |  |
| Terminator 2 | 100% |  |
| Terminator 2 Arcade | - | Black screen |
| Three Dragon Story | - | Unsupported video mode |
| Thunder Blade | 100% | Hitches at scene starts |
| Time Soldiers | 100% |  |
| Tom And Jerry | 100% | Hitches at scene starts |
| Toto World 3 | 100% |  |
| Transbot | 100% |  |
| Trap Marksman Safari | 100% | Stuck on one screen |
| Treinamento Do Mymo | 100% | Hitches at scene starts |
| Trivial Pursuit | 100% |  |
| Tv Colosso | 100% | Hitches at scene starts |
| Ultima 4 | 95% | Slowdown in some scenes, Hitches at scene starts |
| Ultimate Soccer | 100% | Hitches at scene starts |
| Vampire | 100% |  |
| Vigilante | 100% |  |
| Virtua Fighter Animation | 100% |  |
| Walter Payton Football | 100% | Hitches at scene starts |
| Wanted | 100% |  |
| Wimbledon | 100% |  |
| Wimbledon 2 | 100% | Hitches at scene starts |
| Winter Olympics 94 | 100% |  |
| Wolf Child | 100% |  |
| Wonder Boy | 100% |  |
| Wonder Boy 3 | 100% |  |
| Wonder Boy Ml | 100% | Hitches at scene starts |
| Wonder Boy Mw | 100% |  |
| Woody Pop | 100% |  |
| World Class Leaderboard | 90% | Hitches at scene starts |
| World Cup 94 | 100% |  |
| World Cup Italia 90 | 100% | Hitches at scene starts |
| World Games | 100% |  |
| World Grand Prix | 80% | Hitches at scene starts |
| World Soccer | 100% |  |
| Wwf Steel Cage | 100% | Hitches at scene starts |
| Xenon 2 | 95% | Hitches at scene starts |
| Xmen Mojo World | 100% | Hitches at scene starts |
| Ys | 100% |  |
| Zaxxon 3d | 100% |  |
| Zillion | 100% |  |
| Zillion 2 | 100% |  |
| Zool | 70% | Hitches at scene starts |

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
