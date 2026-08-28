koboy: the arcade core, FinalBurn Neo

fbneo_libretro.so is in .adds/koboy/ and .zip files in roms/ run
without you doing anything. An arcade "ROM" is a zip of one board
dump, named the way FinalBurn Neo names it -- galaga.zip, dkong.zip,
mspacman.zip.

IT IS 41 MB ON THE CARD, MOST OF THIS INSTALL. If you have no arcade
romset, delete .adds/koboy/fbneo_libretro.so. Nothing else depends on
it and no other system changes; .zip files simply stop listing.

THE SET HAS TO MATCH THE CORE. This build is FinalBurn Neo v1.0.0.03
(revision of 2025-07-24). A set built for a different FBNeo release,
and a MAME set of any vintage, will not load -- the failure looks
exactly like a broken emulator and is not one.

SOME ZIPS ARE NOT GAMES. A complete set carries device and BIOS zips
(neogeo.zip, midssio.zip, namcoc69.zip, ...) that games load beside
themselves. They list in the browser and cannot be started. Leave
them there: tapper does not run without midssio.zip.

COIN, THEN START. A board will not begin until it has been paid:
SELECT is Coin and START is Start. The faceplate B and A are the
board buttons 1 and 2, and the two extra discs are 3 and 4. Note that
a board spends its first ten or fifteen seconds on a self-test and
ignores the coin slot while it does -- Galaga takes about 800 frames.
A coin inserted then is lost, which looks exactly like a dead button.

WHAT THIS PANEL IS GOOD AT. The 1980s classics turned their monitor
on its side, so Galaga, Dig Dug, Donkey Kong, Frogger and Ms. Pac-Man
are PORTRAIT games on a portrait e-reader. Single-screen boards with
little motion (Dig Dug, Donkey Kong, Ms. Pac-Man) change 1.5 to 2.6%
of the picture per frame and look their best. Galaga and Galaxian are
single-screen and still change 67% and 86%, because the STARFIELD
scrolls -- turn MOTION on for those.

NO BIOS FILE IS NEEDED for the 1980s boards -- an arcade PCB has its
whole program on the board. Later hardware (Neo Geo, CPS) wants a
BIOS zip beside the games in roms/, not in the koboy directory.

SAVING. An arcade board has no battery, so there is no .srm and the
in-game MENU save states are the way to keep a game mid-play.

HIGH SCORES are the other half, and they need one file you supply:
put hiscore.dat in .adds/koboy/fbneo/ (make the directory). koboy
turns the feature on; without that file it is simply inert. With it,
each board writes .adds/koboy/fbneo/<board>.hi when you leave the
game and reads it back next time -- verified on Ms. Pac-Man, whose
attract screen shows the saved score instead of a blank one.
