/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/* heavily based on CobEngine.cpp */

#include "UnitScriptEngine.h"

#include "CobEngine.h"
#include "CobFileHandler.h"
#include "UnitScript.h"
#include "UnitScriptFactory.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitHandler.h"
#include "System/ContainerUtil.h"
#include "System/HashSpec.h"
#include "System/SafeUtil.h"
#include "System/Config/ConfigHandler.h"

#include "System/Misc/TracyDefs.h"

CONFIG(bool, AnimationMT).deprecated(true);

#ifdef BARDROID_SIM
#include "Sim/Misc/BarDroidUnits.h"

// BAR Droid (bardroid/units): the switches of Sim/Misc/BarDroidUnits.h; each 0 gives the stock code, each 1 the same
// results by a faster way
CONFIG(bool, BarDroidAnimPrefetch).defaultValue(true).description("BAR Droid: the unit scripts' animation step in blocks of 32 scripts a thread, with memory prefetch hints a few scripts ahead (the same TickAllAnims per script)");
CONFIG(bool, BarDroidAnimDispatch).defaultValue(true).description("BAR Droid: an animation's step called directly by its type (a switch) instead of through a member-function pointer (the same functions)");
CONFIG(bool, BarDroidAnimDirtyRange).defaultValue(true).description("BAR Droid: the animation step's piece walk only over the index range its model's dirty pieces lie in (LocalModel keeps it; the pieces outside are clean)");
CONFIG(bool, BarDroidPieceTrigCache).defaultValue(true).description("BAR Droid: a piece's half-angle sines and cosines kept per axis by the angle's bits, and its piece-space transform kept while position, rotation and scale are bit-equal (memos of pure functions)");
CONFIG(bool, BarDroidLosGather).defaultValue(true).description("BAR Droid: which units each of the 7 LOS types must update found in one parallel pass over the units; every type then calls the stock update only for those, in the same order (the others' calls change nothing)");
CONFIG(bool, BarDroidCobDispatch).defaultValue(true).description("BAR Droid: the COB interpreter's opcode switch through a dense index (one jump table) instead of a search over the sparse opcodes (the same handlers)");
CONFIG(bool, BarDroidWeaponPrefetch).defaultValue(true).description("BAR Droid: memory prefetch hints a few units ahead in the units' weapon loops (UpdateWeaponVectors, UpdateWeapons)");
CONFIG(bool, BarDroidLosStatePrefetch).defaultValue(true).description("BAR Droid: memory prefetch hints a few units ahead in the units' LOS status loops");

void BarDroidUnits::ReadConfig()
{
	animPrefetch     = configHandler->GetBool("BarDroidAnimPrefetch");
	animDispatch     = configHandler->GetBool("BarDroidAnimDispatch");
	animDirtyRange   = configHandler->GetBool("BarDroidAnimDirtyRange");
	pieceTrigCache   = configHandler->GetBool("BarDroidPieceTrigCache");
	losGather        = configHandler->GetBool("BarDroidLosGather");
	cobDispatch      = configHandler->GetBool("BarDroidCobDispatch");
	weaponPrefetch   = configHandler->GetBool("BarDroidWeaponPrefetch");
	losStatePrefetch = configHandler->GetBool("BarDroidLosStatePrefetch");
}
#endif

static CCobEngine gCobEngine;
static CCobFileHandler gCobFileHandler;
static CUnitScriptEngine gUnitScriptEngine;

CCobEngine* cobEngine = nullptr;
CCobFileHandler* cobFileHandler = nullptr;
CUnitScriptEngine* unitScriptEngine = nullptr;


CR_BIND(CUnitScriptEngine, )

CR_REG_METADATA(CUnitScriptEngine, (
	CR_MEMBER(animating),

	// always null when saving
	CR_IGNORED(currentScript)
))


void CUnitScriptEngine::InitStatic() {
	RECOIL_DETAILED_TRACY_ZONE;
	cobEngine = &gCobEngine;
	cobFileHandler = &gCobFileHandler;
	unitScriptEngine = &gUnitScriptEngine;

	cobEngine->Init();
	cobFileHandler->Init();
	unitScriptEngine->Init();
}

void CUnitScriptEngine::KillStatic() {
	RECOIL_DETAILED_TRACY_ZONE;
	cobEngine->Kill();
	cobFileHandler->Kill();
	unitScriptEngine->Kill();

	cobEngine = nullptr;
	cobFileHandler = nullptr;
	unitScriptEngine = nullptr;
}



void CUnitScriptEngine::ReloadScripts(const UnitDef* udef)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const CCobFile* oldScriptFile = cobFileHandler->GetScriptFile(udef->scriptName);

	if (oldScriptFile == nullptr) {
		LOG_L(L_WARNING, "[UnitScriptEngine::%s] unknown COB script for unit \"%s\": %s", __func__, udef->name.c_str(), udef->scriptName.c_str());
		return;
	}

	CCobFile* newScriptFile = cobFileHandler->ReloadCobFile(udef->scriptName);

	if (newScriptFile == nullptr) {
		LOG_L(L_WARNING, "[UnitScriptEngine::%s] could not load COB script for unit \"%s\" from: %s", __func__, udef->name.c_str(), udef->scriptName.c_str());
		return;
	}

	unsigned int count = 0;

	for (unsigned int i = 0, n = unitHandler.MaxUnits(); i < n; i++) {
		CUnit* unit = unitHandler.GetUnit(i);

		if (unit == nullptr)
			continue;

		CUnitScript*& unitScript = unit->script;
		CCobInstance* cobInstance = dynamic_cast<CCobInstance*>(unitScript);

		if (cobInstance == nullptr || cobInstance->GetFile() != oldScriptFile)
			continue;

		count++;

		spring::SafeDestruct(unitScript);

		unitScript = CUnitScriptFactory::CreateCOBScript(unit, newScriptFile);
		unitScript->Create();
	}

	LOG("[UnitScriptEngine::%s] reloaded COB scripts for %i units", __func__, count);
}


void CUnitScriptEngine::AddInstance(CUnitScript* instance)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (instance == currentScript)
		return;

	spring::VectorInsertUnique(animating, instance/*, true*/);
}

void CUnitScriptEngine::RemoveInstance(CUnitScript* instance)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (instance == currentScript)
		return;

	spring::VectorErase(animating, instance);
}

void CUnitScriptEngine::Tick(int deltaTime)
{
	SCOPED_TIMER("CUnitScriptEngine::Tick");

#ifdef BARDROID_SIM
	// (this frame's switches; here, before the frame's anim, LOS and next frame's unit loops)
	BarDroidUnits::ReadConfig();
#endif

	{
#ifdef BARDROID_SIM
		SCOPED_TIMER("CUnitScriptEngine::Tick::Cob");   // (BAR Droid: the parts timed one by one)
#endif
		cobEngine->Tick(deltaTime);
	}

	// tick all (COB or LUS) script instances that have registered themselves as animating
	{
		ZoneScopedN("CUnitScriptEngine::Tick(MT)");
#ifdef BARDROID_SIM
		SCOPED_TIMER("CUnitScriptEngine::Tick::Anims");
#endif

		// setting currentScript = animating[i]; is not required here, only in ST section below
#ifdef BARDROID_SIM
		if (BarDroidUnits::animPrefetch) {
			// BAR Droid (BarDroidAnimPrefetch): the same TickAllAnims for every animating script (each one touches
			// only its own script and model, so which thread ran which never mattered), in blocks of 32 a thread takes
			// at once, with prefetch hints inside the block a few scripts ahead: the script object three ahead, its
			// anim list and piece table two ahead, the pieces its anims turn or move one ahead. The step mostly waited
			// on memory there (simpleperf, late 8v8, 2026-11-07: the loads of the script, its anims and pieces).
			static constexpr int BLOCK = 32;
			const int numAnimating = animating.size();

			for_mt(0, (numAnimating + BLOCK - 1) / BLOCK, [&](const int block) {
				const int beg = block * BLOCK;
				const int end = std::min(numAnimating, beg + BLOCK);

				for (int i = beg; i < end; ++i) {
					if (i + 3 < end) animating[i + 3]->BarDroidPrefetchObject();
					if (i + 2 < end) animating[i + 2]->BarDroidPrefetchArrays();
					if (i + 1 < end) animating[i + 1]->BarDroidPrefetchPieces();

					animating[i]->TickAllAnims(deltaTime);
				}
			});
		} else
#endif
		for_mt(0, animating.size(), [&](const int i) {
			animating[i]->TickAllAnims(deltaTime);
		});
	}
	{
		ZoneScopedN("CUnitScriptEngine::Tick(ST)");
#ifdef BARDROID_SIM
		SCOPED_TIMER("CUnitScriptEngine::Tick::Finished");
#endif

		uint32_t cs = 0;
#ifdef BARDROID_SIM
		// (BarDroidAnimPrefetch: a hint for the script four ahead, whose checksum and finished anims are read next;
		// the workers had them last)
		const bool prefetch = BarDroidUnits::animPrefetch;
#endif
		for (size_t i = 0; i < animating.size(); /*NO-OP*/) {
#ifdef BARDROID_SIM
			if (prefetch && i + 4 < animating.size())
				animating[i + 4]->BarDroidPrefetchObject();
#endif
			currentScript = animating[i];
			// deal with synced checksum here, before animating is possibly popped below
			cs = spring::hash_combine(currentScript->GetAnimArrayChecksum(), cs);

			if (!currentScript->TickAnimFinished()) {
				animating[i] = animating.back();
				animating.pop_back();
				continue;
			}
			i++;
		}

		currentScript = nullptr;
		Sync::Assert(cs, "animating");
	}

	{
#ifdef BARDROID_SIM
		SCOPED_TIMER("CUnitScriptEngine::Tick::Callins");
#endif
		cobEngine->RunDeferredCallins();
	}
}
