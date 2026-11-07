/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#ifdef BARDROID_SIM
#include <atomic>

class CUnit;

// BAR Droid (engine branch bardroid/move): unit movement and collision speed-ups. Each one is exact: the same objects
// in the same order, the same float operations in the same order, the same synced writes in the same order as the
// stock code, so the simulation and its checksums stay bit-identical. Each has its own springsettings switch (default
// on, 0: the stock code path), read once a simulation frame at the start of GroundMoveSystem::Update (main thread,
// before any of the frame's parallel sections), so one binary can A/B every change in one game.
namespace BarDroidMove {
	// BarDroidMoveQuadStamp: CQuadField's Get{Units,Features,Solids}Exact and GetUnits remember the objects a query has
	// already passed in per-thread stamp arrays (by id) instead of writing the query's number into every object they
	// pass (CWorldObject::mtTempNum, a 32-slot array whose first slots share a cache line with pos and radius: every
	// thread's queries dirtied the lines the other threads were reading in the parallel movement sections).
	extern bool quadStamp;
	// BarDroidMoveBlockMemo: CMoveMath's per-thread memo of each obstacle's block type (RangeIsBlockedHashedMt,
	// FloodFillRangeIsBlocked) in an epoch-tagged open-addressing table (clearing = a new epoch) instead of
	// emilib::HashMap (its clear walks every bucket, and it was cleared on nearly every call); RangeIsBlockedMt and
	// IsBlockedNoSpeedModCheckDiff remember the obstacles they passed in a per-thread table, not in the objects.
	extern bool blockMemo;
	// BarDroidMoveExitOnly: CMoveMath::RangeHasExitOnly first asks a per-tile count of exit-only squares (kept by
	// YardmapStatusEffectsMap): a footprint whose tiles have none is answered without its squares (most of the map).
	extern bool exitOnly;
	// BarDroidMovePrefetch: the single-threaded movement loops (heading changes, pre-collision updates, the final
	// position updates, the general move types) prefetch the units a few steps ahead, and the ground cells they read.
	extern bool prefetch;
	// BarDroidMoveStaticSkip: GeneralMoveSystem does not call CStaticMoveType::Update (an empty virtual call that
	// returns false) for every building each frame.
	extern bool staticSkip;
	// BarDroidMoveAvoid: obstacle avoidance asks the quadfield for units only (it skipped every feature the solids query
	// returned) and does its dynamic_cast only for the obstacles it does not skip anyway.
	extern bool avoid;
	// BarDroidMoveRawSearch: MoveDef::DoRawSearch and TestMoveSquareRange call the speed-mod function directly instead
	// of through a std::function made for every call.
	extern bool rawSearch;
	// BarDroidMoveParallelUpdate: GroundMoveSystem's stage 5 (each ground unit's final Update) on every thread, each
	// unit's synced writes recorded and hashed afterwards in the stock order (CSyncChecker::BarDroidReplay).
	extern bool parallelUpdate;
	// BarDroidMoveParallelHeading: the heading changes (stage 2, ChangeHeadingEvent) the same way, in frames where no
	// unit's ChangeHeading can call its unit script (headingNeedsSerial, set by FollowPath in stage 1 for a unit whose
	// turn rate could reach minScriptChangeHeading); otherwise that frame runs the stock loop.
	extern bool parallelHeading;
	extern std::atomic<bool> headingNeedsSerial;

	void ReadConfig();

	static inline void Prefetch(const void* p) { __builtin_prefetch(p); }

	// (BarDroidMovePrefetch, the traversal plans' parallel section; GroundMoveType.cpp) FollowPath's
	// UNIT_HAS_MOVE_CMD reads the unit's first queued command: a cache miss for nearly every moving unit. Read the
	// unit's commandAI pointer and prefetch its queue's header; read the queue's front and prefetch that command.
	// Reads only (as FollowPath's own, in the same section, where no thread changes a command queue).
	void PrefetchCommandAI(const CUnit* unit);
	void PrefetchCommand(const CUnit* unit);
	// (the unit positions' parallel section) the map cells CMoveMath::GetPosSpeedMod reads at the unit's square
	void PrefetchSpeedModCells(const CUnit* unit);
}
#endif
