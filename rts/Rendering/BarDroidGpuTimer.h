/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

// BAR Droid (engine branch): how long the GPU takes for each drawing pass. While the engine's profiler is on
// ("/debug 1 0"; drawDebug goes with it), a GL_TIME_ELAPSED query around each pass; its result, read a few drawn frames
// later when the GPU has finished, goes to the profiler as "GPU::<pass>". Passes must not nest (one time-elapsed query
// at a time); a pass met a second time in a frame is timed the first time only. Off, nothing is issued.

#ifdef BARDROID_SIM

#include <cstdint>
#include <string>
#include <unordered_map>

#include "Rendering/GL/myGL.h"
#include "Rendering/GlobalRendering.h"
#include "System/Misc/SpringTime.h"
#include "System/StringHash.h"
#include "System/TimeProfiler.h"

class BarDroidGpuTimer
{
public:
	explicit BarDroidGpuTimer(const char* pass)
	{
		if (!globalRendering->drawDebug || !GLAD_GL_ARB_timer_query || namedActive)
			return;

		Entry& e = Entries()[pass];
		if (e.hash == 0) {
			e.name = std::string("GPU::") + pass;
			CTimeProfiler::RegisterTimer(e.name.c_str());
			e.hash = hashString(e.name.c_str());
			glGenQueries(SLOTS, e.queries);
		}
		const uint32_t frame = globalRendering->drawFrame;
		if (e.lastFrame == frame)
			return;
		e.lastFrame = frame;

		const uint32_t slot = frame % SLOTS;
		if (e.used[slot]) {
			// the result of SLOTS frames ago, if the GPU is done with it (never waited for)
			GLint ready = 0;
			glGetQueryObjectiv(e.queries[slot], GL_QUERY_RESULT_AVAILABLE, &ready);
			if (ready) {
				AddResult(e, slot);
			}
		}
		glBeginQuery(GL_TIME_ELAPSED, e.queries[slot]);
		e.used[slot] = true;
		active = true;
		namedActive = true; // (one time-elapsed query at a time: none nested inside this one)
	}

	~BarDroidGpuTimer()
	{
		if (!active)
			return;
		glEndQuery(GL_TIME_ELAPSED);
		namedActive = false;
	}

private:
	static constexpr int SLOTS = 4;
	struct Entry {
		std::string name;
		unsigned hash = 0;
		uint32_t lastFrame = ~0u;
		GLuint queries[SLOTS] = {};
		bool used[SLOTS] = {};
	};
	// a slot's result to the profiler; over a second is no GPU time but a query the driver never ran (a widget's pass
	// in the minimap's drawing read ~5e8 s on the phone's Mesa), left out
	static void AddResult(const Entry& e, uint32_t slot)
	{
		GLuint64 ns = 0;
		glGetQueryObjectui64v(e.queries[slot], GL_QUERY_RESULT, &ns);
		if (ns < GLuint64(1000000000))
			CTimeProfiler::GetInstance().AddTime(e.hash, spring_gettime(), spring_time::fromNanoSecs(int64_t(ns)), false, false, false);
	}

	// (keyed by the pass name's pointer: the callers pass string literals; GL on the main thread only)
	static std::unordered_map<const char*, Entry>& Entries()
	{
		static std::unordered_map<const char*, Entry> entries;
		return entries;
	}

	bool active = false;

public:
	// For BAR's own marks around each widget's and gadget's drawing (LuaHandle.cpp's Tracy zones): begin and end as
	// two calls, keyed by the mark's text ("W:DrawWorld:Defense Range GL4" -> "GPU::W:DrawWorld:Defense Range GL4").
	static bool BeginNamed(const std::string& mark)
	{
		if (!globalRendering->drawDebug || !GLAD_GL_ARB_timer_query || namedActive)
			return false;

		Entry& e = NamedEntries()[mark];
		if (e.hash == 0) {
			e.name = "GPU::" + mark;
			CTimeProfiler::RegisterTimer(e.name.c_str());
			e.hash = hashString(e.name.c_str());
			glGenQueries(SLOTS, e.queries);
		}
		const uint32_t frame = globalRendering->drawFrame;
		if (e.lastFrame == frame)
			return false;
		e.lastFrame = frame;

		const uint32_t slot = frame % SLOTS;
		if (e.used[slot]) {
			GLint ready = 0;
			glGetQueryObjectiv(e.queries[slot], GL_QUERY_RESULT_AVAILABLE, &ready);
			if (ready) {
				AddResult(e, slot);
			}
		}
		glBeginQuery(GL_TIME_ELAPSED, e.queries[slot]);
		e.used[slot] = true;
		namedActive = true;
		return true;
	}
	static void EndNamed()
	{
		if (!namedActive)
			return;
		glEndQuery(GL_TIME_ELAPSED);
		namedActive = false;
	}

private:
	static inline bool namedActive = false;
	static std::unordered_map<std::string, Entry>& NamedEntries()
	{
		static std::unordered_map<std::string, Entry> entries;
		return entries;
	}
};

#define BARDROID_GPU_TIMER(pass) BarDroidGpuTimer __barDroidGpuTimer(pass)

#else
#define BARDROID_GPU_TIMER(pass)
#endif
