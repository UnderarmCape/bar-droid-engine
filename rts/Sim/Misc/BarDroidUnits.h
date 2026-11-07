/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

// BAR Droid (engine branch bardroid/units): runtime switches for the exact speed-ups of the unit scripts (COB and the
// animations), the units' weapons and line of sight. Each switch changes only how the same results are reached: the
// synced state stays bit-identical, so 0 and 1 can be compared in one game (an A/B) and 0 everywhere is stock code.
// They are engine settings read once a simulation frame on the main thread (ReadConfig, from CUnitScriptEngine::Tick;
// the workers read them only inside the frame's parallel loops, after that).
#ifdef BARDROID_SIM

namespace BarDroidUnits {
	inline bool animPrefetch     = true; // BarDroidAnimPrefetch: the animation step in blocks, memory hints ahead
	inline bool animDispatch     = true; // BarDroidAnimDispatch: an anim's step called directly (a switch), not via a member pointer
	inline bool animDirtyRange   = true; // BarDroidAnimDirtyRange: the piece walk over the dirty pieces' index range only
	inline bool pieceTrigCache   = true; // BarDroidPieceTrigCache: a piece's half-angle sines/cosines and transform reused for unchanged inputs
	inline bool losGather        = true; // BarDroidLosGather: which units each LOS type must update, found in one parallel pass
	inline bool cobDispatch      = true; // BarDroidCobDispatch: the COB interpreter's opcode switch through a dense jump table
	inline bool weaponPrefetch   = true; // BarDroidWeaponPrefetch: memory hints ahead in the units' weapon loops
	inline bool losStatePrefetch = true; // BarDroidLosStatePrefetch: memory hints ahead in the units' LOS status loops

	// (UnitScriptEngine.cpp)
	void ReadConfig();
}

// a hint to load the cache line holding p (never faults, changes no result)
#define BARDROID_PREFETCH(p) __builtin_prefetch(static_cast<const void*>(p), 0, 3)

#endif
