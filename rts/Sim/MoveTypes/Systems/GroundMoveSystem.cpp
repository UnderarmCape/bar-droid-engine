/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// #undef NDEBUG

#include "GroundMoveSystem.h"

#include "Sim/Ecs/Registry.h"
#include "Sim/Features/Feature.h"
#include "Sim/Misc/QuadField.h"
#include "Sim/MoveTypes/Components/MoveTypesComponents.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"

#include "System/EventHandler.h"
#include "System/TimeProfiler.h"
#include "System/Threading/ThreadPool.h"

#ifdef BARDROID_SIM
#include "Sim/MoveTypes/BarDroidMove.h"
#include "Sim/MoveTypes/GroundMoveType.h"
#include "System/Config/ConfigHandler.h"
#ifdef SYNCCHECK
#include "System/Sync/SyncChecker.h"
#endif

CONFIG(bool, BarDroidMoveAll).defaultValue(true).description("BAR Droid: 0 turns every BarDroidMove* switch and BarDroidSyncFast off at once (an A/B of the whole movement package in one run); 1: each as its own switch says");
CONFIG(bool, BarDroidMoveQuadStamp).defaultValue(true).description("BAR Droid: quadfield queries remember the objects they passed in per-thread arrays, not in the objects (exact); 0: as stock");
CONFIG(bool, BarDroidMoveBlockMemo).defaultValue(true).description("BAR Droid: the movement's blocking-map queries memoize obstacles in epoch-tagged per-thread tables (exact); 0: as stock");
CONFIG(bool, BarDroidMoveExitOnly).defaultValue(true).description("BAR Droid: exit-only footprint tests skip tiles without exit-only squares (exact); 0: as stock");
CONFIG(bool, BarDroidMovePrefetch).defaultValue(true).description("BAR Droid: the single-threaded movement loops prefetch units and ground cells ahead (hints only); 0: as stock");
CONFIG(bool, BarDroidMoveStaticSkip).defaultValue(true).description("BAR Droid: the general move system skips the empty Update of static move types (exact); 0: as stock");
CONFIG(bool, BarDroidMoveAvoid).defaultValue(true).description("BAR Droid: obstacle avoidance queries units only (it skipped every feature) (exact); 0: as stock");
CONFIG(bool, BarDroidMoveRawSearch).defaultValue(true).description("BAR Droid: raw path searches call the speed-mod function directly, without a std::function (exact); 0: as stock");
CONFIG(bool, BarDroidSyncFast).defaultValue(true).description("BAR Droid: the sync checker hashes 1-16 byte writes with xxHash's short-input function inlined (the same hash); 0: as stock");

namespace BarDroidMove {
	bool quadStamp = true;
	bool blockMemo = true;
	bool exitOnly = true;
	bool prefetch = true;
	bool staticSkip = true;
	bool avoid = true;
	bool rawSearch = true;

	void ReadConfig() {
		const bool all = configHandler->GetBool("BarDroidMoveAll");

		quadStamp  = all && configHandler->GetBool("BarDroidMoveQuadStamp");
		blockMemo  = all && configHandler->GetBool("BarDroidMoveBlockMemo");
		exitOnly   = all && configHandler->GetBool("BarDroidMoveExitOnly");
		prefetch   = all && configHandler->GetBool("BarDroidMovePrefetch");
		staticSkip = all && configHandler->GetBool("BarDroidMoveStaticSkip");
		avoid      = all && configHandler->GetBool("BarDroidMoveAvoid");
		rawSearch  = all && configHandler->GetBool("BarDroidMoveRawSearch");
	#ifdef SYNCCHECK
		CSyncChecker::barDroidSyncFast = all && configHandler->GetBool("BarDroidSyncFast");
	#endif
	}
}

namespace {
	// BAR Droid (BarDroidMovePrefetch): the units of a single-threaded loop in the order that loop meets them (gathered
	// by the same view iteration just before it). Step k asks for unit k+FAR's lines and for unit k+NEAR's ground cells
	// (its position arrived meanwhile). A wrong guess (the loop's view changed under a Lua call-in) costs only time.
	static constexpr size_t BARDROID_PF_FAR = 8;
	static constexpr size_t BARDROID_PF_NEAR = 3;

	std::vector<CUnit*> barDroidOrder;

	inline void BarDroidPrefetchStart(int stage)
	{
		for (size_t i = 0, n = std::min(BARDROID_PF_FAR, barDroidOrder.size()); i < n; ++i) {
			CGroundMoveType::BarDroidPrefetchLines(barDroidOrder[i], stage);
		}
	}

	inline void BarDroidPrefetchStep(size_t k, int stage)
	{
		if ((k + BARDROID_PF_FAR) < barDroidOrder.size())
			CGroundMoveType::BarDroidPrefetchLines(barDroidOrder[k + BARDROID_PF_FAR], stage);
		if ((k + BARDROID_PF_NEAR) < barDroidOrder.size())
			CGroundMoveType::BarDroidPrefetchGround(barDroidOrder[k + BARDROID_PF_NEAR], stage);
	}
}

// BAR Droid (BarDroidMovePrefetch): in a parallel section, the unit two indices on (BarDroidForGrain hands a thread
// runs of consecutive indices); its first lines only. The same view lookups the section's body makes (reads only).
#define BARDROID_PREFETCH_MT(i)                                                                                     \
	do {                                                                                                            \
		if (barDroidPrefetch && (static_cast<size_t>(i) + 2) < view.size()) {                                       \
			const auto barDroidEntity = view.storage<GroundMoveType>()[(i) + 2];                                     \
			CGroundMoveType::BarDroidPrefetchLines(unitHandler.GetUnit(view.get<GroundMoveType>(barDroidEntity).value), -1); \
		}                                                                                                           \
	} while (false)

// the traversal plans: the unit 4 on (its lines, the commandAI pointer among them), its commandAI 2 on (now in cache:
// read, its queue's header prefetched), its first command 1 on (the header now in cache: read, the command prefetched)
#define BARDROID_UNIT_AT(j) unitHandler.GetUnit(view.get<GroundMoveType>(view.storage<GroundMoveType>()[(j)]).value)
#define BARDROID_PREFETCH_MT_PLAN(i)                                                                                \
	do {                                                                                                            \
		if (!barDroidPrefetch)                                                                                      \
			break;                                                                                                  \
		const size_t barDroidN = view.size();                                                                       \
		const size_t barDroidI = static_cast<size_t>(i);                                                            \
		if ((barDroidI + 4) < barDroidN)                                                                            \
			CGroundMoveType::BarDroidPrefetchLines(BARDROID_UNIT_AT(barDroidI + 4), -1);                            \
		if ((barDroidI + 2) < barDroidN)                                                                            \
			BarDroidMove::PrefetchCommandAI(BARDROID_UNIT_AT(barDroidI + 2));                                       \
		if ((barDroidI + 1) < barDroidN)                                                                            \
			BarDroidMove::PrefetchCommand(BARDROID_UNIT_AT(barDroidI + 1));                                         \
	} while (false)
#endif

using namespace MoveTypes;

void GroundMoveSystem::Init() {}

template<typename T, typename F>
void issue_events(F func)
{
    auto view = Sim::registry.view<T>();
    view.each([&](T& comp){
        std::for_each(comp.value.begin(), comp.value.end(), func);
        comp.value.clear();
    });
}

void GroundMoveSystem::Update() {
#ifdef BARDROID_SIM
	// (the frame's switches, before any of its parallel sections; see BarDroidMove.h)
	BarDroidMove::ReadConfig();
	const bool barDroidPrefetch = BarDroidMove::prefetch;
#endif
    // TODO: GroundMove could become a component (or series of components) and then the extra indirection wouldn't be
    // needed. Though that will be a bigger change.
	{
		SCOPED_TIMER("Sim::Unit::MoveType::1::UpdateTraversalPlan");
        auto view = Sim::registry.view<GroundMoveType>();
#ifdef BARDROID_SIM
        for_mt(0, view.size(), [&view, barDroidPrefetch](const int i){
            BARDROID_PREFETCH_MT_PLAN(i);
#else
        for_mt(0, view.size(), [&view](const int i){
#endif
            auto entity = view.storage<GroundMoveType>()[i];
            auto unitId = view.get<GroundMoveType>(entity);

            CUnit* unit = unitHandler.GetUnit(unitId.value);
			CGroundMoveType* moveType = static_cast<CGroundMoveType*>(unit->moveType);
            assert(moveType != nullptr);

            #ifndef NDEBUG
			unit->SanityCheck();
            #endif

			moveType->UpdateTraversalPlan();
		});
	}
	{
		SCOPED_TIMER("Sim::Unit::MoveType::2::UpdatePreCollisions");

        // These two sections are ST due to the numerous synced vars being changed.
#ifdef BARDROID_SIM
        {
            auto view = Sim::registry.view<ChangeHeadingEvent>();
            size_t k = 0;

            if (barDroidPrefetch) {
                barDroidOrder.clear();
                view.each([](ChangeHeadingEvent& event){
                    if (event.changed)
                        barDroidOrder.push_back(unitHandler.GetUnit(event.unitId));
                });
                BarDroidPrefetchStart(CGroundMoveType::BARDROID_STAGE_HEADING);
            }

            view.each([barDroidPrefetch, &k](ChangeHeadingEvent& event){
                if (event.changed) {
                    if (barDroidPrefetch)
                        BarDroidPrefetchStep(k++, CGroundMoveType::BARDROID_STAGE_HEADING);

                    CUnit* unit = unitHandler.GetUnit(event.unitId);
                    CGroundMoveType* moveType = static_cast<CGroundMoveType*>(unit->moveType);
                    moveType->ChangeHeading(event.deltaHeading);
                    event.changed = false;
                }
            });
        }
        {
            auto view = Sim::registry.view<ChangeMainHeadingEvent>();
            size_t k = 0;

            if (barDroidPrefetch) {
                barDroidOrder.clear();
                view.each([](ChangeMainHeadingEvent& event){
                    if (event.changed)
                        barDroidOrder.push_back(unitHandler.GetUnit(event.unitId));
                });
                BarDroidPrefetchStart(CGroundMoveType::BARDROID_STAGE_HEADING);
            }

            view.each([barDroidPrefetch, &k](ChangeMainHeadingEvent& event){
                if (event.changed) {
                    if (barDroidPrefetch)
                        BarDroidPrefetchStep(k++, CGroundMoveType::BARDROID_STAGE_HEADING);

                    CUnit* unit = unitHandler.GetUnit(event.unitId);
                    CGroundMoveType* moveType = static_cast<CGroundMoveType*>(unit->moveType);
                    moveType->SetMainHeading();
                    event.changed = false;
                }
            });
        }
#else
        {
            auto view = Sim::registry.view<ChangeHeadingEvent>();
            view.each([](ChangeHeadingEvent& event){
                if (event.changed) {
                    CUnit* unit = unitHandler.GetUnit(event.unitId);
                    CGroundMoveType* moveType = static_cast<CGroundMoveType*>(unit->moveType);
                    moveType->ChangeHeading(event.deltaHeading);
                    event.changed = false;
                }
            });
        }
        {
            auto view = Sim::registry.view<ChangeMainHeadingEvent>();
            view.each([](ChangeMainHeadingEvent& event){
                if (event.changed) {
                    CUnit* unit = unitHandler.GetUnit(event.unitId);
                    CGroundMoveType* moveType = static_cast<CGroundMoveType*>(unit->moveType);
                    moveType->SetMainHeading();
                    event.changed = false;
                }
            });
        }
#endif
    }
	{
        auto view = Sim::registry.view<GroundMoveType>();
#ifdef BARDROID_SIM
        {
        // (BAR Droid: these two parts had no timer of their own)
        SCOPED_TIMER("Sim::Unit::MoveType::2b::UnitPosition");
        for_mt(0, view.size(), [&view, barDroidPrefetch](const int i){
            BARDROID_PREFETCH_MT(i);
#else
        for_mt(0, view.size(), [&view](const int i){
#endif
            auto entity = view.storage<GroundMoveType>()[i];
            auto unitId = view.get<GroundMoveType>(entity);

            CUnit* unit = unitHandler.GetUnit(unitId.value);
			CGroundMoveType* moveType = static_cast<CGroundMoveType*>(unit->moveType);
            assert(moveType != nullptr);

			moveType->UpdateUnitPosition();
		});
#ifdef BARDROID_SIM
        }
#endif

#ifdef BARDROID_SIM
		static TimerNameRegistrar barDroidTimerName("Sim::Unit::MoveType::2c::PreCollisionsST");
		ScopedTimer barDroidTimer(hashString("Sim::Unit::MoveType::2c::PreCollisionsST"));
		size_t k = 0;

		if (barDroidPrefetch) {
			barDroidOrder.clear();
			view.each([](GroundMoveType& unitId){ barDroidOrder.push_back(unitHandler.GetUnit(unitId.value)); });
			BarDroidPrefetchStart(CGroundMoveType::BARDROID_STAGE_PRECOLLISIONS);
		}

		view.each([barDroidPrefetch, &k](GroundMoveType& unitId){
			if (barDroidPrefetch)
				BarDroidPrefetchStep(k++, CGroundMoveType::BARDROID_STAGE_PRECOLLISIONS);
#else
		view.each([](GroundMoveType& unitId){
#endif
			CUnit* unit = unitHandler.GetUnit(unitId.value);
			CGroundMoveType* moveType = static_cast<CGroundMoveType*>(unit->moveType);
            assert(moveType != nullptr);

			moveType->UpdatePreCollisions();

            // this unit is not coming back, kill it now without any death
            // sequence (s.t. deathScriptFinished becomes true immediately)
            if (!unit->pos.IsInBounds() && (unit->speed.w > MAX_UNIT_SPEED))
                unit->ForcedKillUnit(nullptr, false, true, -CSolidObject::DAMAGE_KILLED_OOB);
		});
	}
    {
        SCOPED_TIMER("Sim::Unit::MoveType::3::CollisionDetection");
        auto view = Sim::registry.view<GroundMoveType>();
        //size_t count = view.storage<GroundMoveType>().size();
#ifdef BARDROID_SIM
        for_mt(0, view.size(), [&view, barDroidPrefetch](const int i){
            BARDROID_PREFETCH_MT(i);
#else
        for_mt(0, view.size(), [&view](const int i){
#endif
            auto entity = view.storage<GroundMoveType>()[i];
            assert( Sim::registry.valid(entity) );
            assert( Sim::registry.all_of<GroundMoveType>(entity) );
            assert( !Sim::registry.all_of<GeneralMoveType>(entity) );

            auto unitId = view.get<GroundMoveType>(entity);

            CUnit* unit = unitHandler.GetUnit(unitId.value);
            CGroundMoveType* moveType = static_cast<CGroundMoveType*>(unit->moveType);
            assert(moveType != nullptr);

            moveType->SetMtJobId(i);
            moveType->UpdateCollisionDetections();
        });
    }
	{
        SCOPED_TIMER("Sim::Unit::MoveType::4::ProcessCollisionEvents");

        issue_events<UnitCrushEvents>([](const UnitCrushEvent& event) {
            event.collidee->Kill(event.collider, event.crushImpulse, true);
        });
        issue_events<FeatureCrushEvents>([](const FeatureCrushEvent& event) {
            event.collidee->Kill(event.collider, event.crushImpulse, true);
        });
        issue_events<UnitCollisionEvents>([&](const UnitCollisionEvent& event) {
            eventHandler.UnitUnitCollision(event.collider, event.collidee);
        });
        issue_events<FeatureCollisionEvents>([](const FeatureCollisionEvent& event) {
            eventHandler.UnitFeatureCollision(event.collider, event.collidee);
        });
        issue_events<FeatureMoveEvents>([](const FeatureMoveEvent& event) {
            quadField.RemoveFeature(event.collidee);
            event.collidee->Move(event.moveImpulse, true);
            quadField.AddFeature(event.collidee);
        });
	}
	{
        // TODO: the vars are synced and that's what is stopping this being MT'ed.
        // Need an alternative method to support sync values that doesn't stop MT.
        // Same for change heading above as well.
        SCOPED_TIMER("Sim::Unit::MoveType::5::Update");
        auto view = Sim::registry.view<GroundMoveType>();
#ifdef BARDROID_SIM
        size_t k = 0;

        if (barDroidPrefetch) {
            barDroidOrder.clear();
            view.each([](GroundMoveType& unitId){ barDroidOrder.push_back(unitHandler.GetUnit(unitId.value)); });
            BarDroidPrefetchStart(CGroundMoveType::BARDROID_STAGE_UPDATE);
        }

        view.each([barDroidPrefetch, &k](GroundMoveType& unitId){
            if (barDroidPrefetch)
                BarDroidPrefetchStep(k++, CGroundMoveType::BARDROID_STAGE_UPDATE);
#else
        view.each([&view](GroundMoveType& unitId){
#endif
            CUnit* unit = unitHandler.GetUnit(unitId.value);
            CGroundMoveType* moveType = static_cast<CGroundMoveType*>(unit->moveType);
            assert(moveType != nullptr);
            if (moveType->Update()) 
                eventHandler.UnitMoved(unit);

            #ifndef NDEBUG
            unit->SanityCheck();
            #endif
        });
    }
}

void GroundMoveSystem::Shutdown() {}
