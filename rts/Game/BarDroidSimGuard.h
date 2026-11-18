/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#ifdef BARDROID_SIM
// BAR Droid: whether this game may run the simulation rewrite's exact speed-ups (the BarDroidMove*, BarDroidUnits*,
// BarDroidLua* switches, BarDroidForGrain and BarDroidPrevSkip). Where other machines' simulations check ours (an
// online game: a remote host, or a local host with more than one player) they stay off, stock code, until the rewrite
// has passed many more server replays (the app's docs/ENGINE-DEFAULT.md). Allowed with no game (the menu), in a replay
// and in a game this engine hosts for one player (a skirmish, a scenario). BarDroidSimOnline = 1 allows them online
// too (tests). Read once a frame by each package's ReadConfig; Game.cpp.
bool BarDroidSimAllowed();
#endif
