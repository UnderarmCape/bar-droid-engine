/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// #undef NDEBUG

#include "GeneralMoveSystem.h"

#include "Sim/Ecs/Registry.h"
#include "Sim/MoveTypes/Components/MoveTypesComponents.h"
#include "Sim/MoveTypes/MoveMath/MoveMath.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"

#include "System/EventHandler.h"
#include "System/TimeProfiler.h"
#include "System/Threading/ThreadPool.h"
#include "Sim/Units/UnitDef.h"

#include "System/Misc/TracyDefs.h"

#ifdef BARDROID_SIM
#include <typeinfo>
#include "Sim/MoveTypes/BarDroidMove.h"
#include "Sim/MoveTypes/StaticMoveType.h"
#endif

using namespace MoveTypes;

void GeneralMoveSystem::Init() {
    RECOIL_DETAILED_TRACY_ZONE;
    CMoveMath::InitRangeIsBlockedHashes();
    Sim::systemUtils.OnPostLoad().connect<&CMoveMath::InitRangeIsBlockedHashes>();
}

#ifdef BARDROID_SIM
namespace {
	// BAR Droid (BarDroidMovePrefetch): the units in the order the loop below meets them (gathered by the same view
	// iteration just before it); the loop asks for unit k+AHEAD's lines at step k. Hints only.
	static constexpr size_t BARDROID_GMS_AHEAD = 8;
	std::vector<CUnit*> barDroidGeneralOrder;

	inline void BarDroidPrefetchGeneral(const CUnit* u)
	{
		BarDroidMove::Prefetch(&u->pos);
		BarDroidMove::Prefetch(&u->speed);
		BarDroidMove::Prefetch(&u->moveType);
		BarDroidMove::Prefetch(u->amtMemBuffer);
	}
}
#endif

void GeneralMoveSystem::Update() {
    RECOIL_DETAILED_TRACY_ZONE;
    auto view = Sim::registry.view<GeneralMoveType>();
	{
        SCOPED_TIMER("Sim::Unit::MoveType::5::Update");
#ifdef BARDROID_SIM
        const bool barDroidPrefetch = BarDroidMove::prefetch;
        const bool barDroidStaticSkip = BarDroidMove::staticSkip;
        size_t k = 0;

        if (barDroidPrefetch) {
            barDroidGeneralOrder.clear();
            view.each([](GeneralMoveType& unitId){ barDroidGeneralOrder.push_back(unitHandler.GetUnit(unitId.value)); });

            for (size_t i = 0, n = std::min(BARDROID_GMS_AHEAD, barDroidGeneralOrder.size()); i < n; ++i) {
                BarDroidPrefetchGeneral(barDroidGeneralOrder[i]);
            }
        }

        view.each([barDroidPrefetch, barDroidStaticSkip, &k](GeneralMoveType& unitId){
            if (barDroidPrefetch && (k + BARDROID_GMS_AHEAD) < barDroidGeneralOrder.size())
                BarDroidPrefetchGeneral(barDroidGeneralOrder[k + BARDROID_GMS_AHEAD]);
            ++k;

            CUnit* unit = unitHandler.GetUnit(unitId.value);
            AMoveType* moveType = unit->moveType;

            #ifndef NDEBUG
            unit->SanityCheck();
            #endif

            // BAR Droid (BarDroidMoveStaticSkip): CStaticMoveType::Update only returns false (exactly that class, not
            // one derived from it)
            if ((barDroidStaticSkip && typeid(*moveType) == typeid(CStaticMoveType))? false: moveType->Update())
                eventHandler.UnitMoved(unit);
#else
        view.each([](GeneralMoveType& unitId){
            CUnit* unit = unitHandler.GetUnit(unitId.value);
            AMoveType* moveType = unit->moveType;

            #ifndef NDEBUG
            unit->SanityCheck();
            #endif

            if (moveType->Update())
                eventHandler.UnitMoved(unit);
#endif

            // this unit is not coming back, kill it now without any death
            // sequence (s.t. deathScriptFinished becomes true immediately)
            if (!unit->pos.IsInBounds() && (unit->speed.w > MAX_UNIT_SPEED))
                unit->ForcedKillUnit(nullptr, false, true, -CSolidObject::DAMAGE_KILLED_OOB);

            #ifndef NDEBUG
            unit->SanityCheck();
            #endif
        });
	}
}

void GeneralMoveSystem::Shutdown() {
    Sim::systemUtils.OnPostLoad().disconnect<&CMoveMath::InitRangeIsBlockedHashes>();
}
