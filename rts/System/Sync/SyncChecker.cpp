/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */


#ifdef SYNCCHECK

#include "SyncChecker.h"

// This cannot be included in the header file (SyncChecker.h) because include conflicts will occur.
#include "System/Threading/ThreadPool.h"
#include "System/HashSpec.h"


unsigned CSyncChecker::g_checksum;
unsigned CSyncChecker::g_prevChecksum;
int CSyncChecker::inSyncedCode;
#ifdef BARDROID_SIM
#include <cstring>

bool CSyncChecker::barDroidSyncFast = true;

// this thread's record of synced writes (BarDroidMoveParallelUpdate), or none
static thread_local std::vector<unsigned char>* barDroidRecorder = nullptr;

// a record: per write, a kind byte (0: bytes, 1: a value), a 4-byte length or value, and for bytes the bytes
static inline void BarDroidRecord(unsigned char kind, uint32_t lenOrVal, const void* p)
{
	std::vector<unsigned char>& r = *barDroidRecorder;
	const size_t o = r.size();
	const size_t n = (kind == 0)? lenOrVal: 0;

	r.resize(o + 5 + n);
	r[o] = kind;
	std::memcpy(&r[o + 1], &lenOrVal, 4);

	if (n != 0)
		std::memcpy(&r[o + 5], p, n);
}

void CSyncChecker::BarDroidSetRecorder(std::vector<unsigned char>* recorder) { barDroidRecorder = recorder; }

void CSyncChecker::BarDroidReplay(const std::vector<unsigned char>& record)
{
	assert(barDroidRecorder == nullptr);

	for (size_t i = 0, n = record.size(); i < n; ) {
		const unsigned char kind = record[i];
		uint32_t lenOrVal;
		std::memcpy(&lenOrVal, &record[i + 1], 4);
		i += 5;

		if (kind == 0) {
			Sync(&record[i], lenOrVal);
			i += lenOrVal;
		} else {
			Sync(lenOrVal);
		}
	}
}
#endif

void CSyncChecker::NewFrame()
{
	g_checksum = 0xfade1eaf;
#ifdef SYNC_HISTORY
	LogHistory();
#endif // SYNC_HISTORY
}

void CSyncChecker::debugSyncCheckThreading()
{
	assert(ThreadPool::GetThreadNum() == 0);
}

void CSyncChecker::Sync(uint32_t val)
{
#ifdef DEBUG_SYNC_MT_CHECK
	// Sync calls should not be occurring in multi-threaded sections
	debugSyncCheckThreading();
#endif
#ifdef BARDROID_SIM
	if (barDroidRecorder != nullptr) {
		BarDroidRecord(1, val, nullptr);
		return;
	}
#endif
	g_checksum = spring::hash_combine(val, g_checksum);
	//LOG("[Sync::Checker] chksum=%u\n", g_checksum);

#ifdef SYNC_HISTORY
	LogHistory();
#endif // SYNC_HISTORY
}

void CSyncChecker::Sync(const void* p, unsigned size)
{
#ifdef DEBUG_SYNC_MT_CHECK
	// Sync calls should not be occurring in multi-threaded sections
	debugSyncCheckThreading();
#endif
	// most common cases first, make it easy for compiler to optimize for it
	// simple xor is not enough to detect multiple zeroes, e.g.
#ifdef BARDROID_SIM
	if (barDroidRecorder != nullptr) {
		BarDroidRecord(0, size, p);
		return;
	}

	// BAR Droid (BarDroidSyncFast): nearly every synced write is 1-16 bytes; take xxHash's own short-input function
	// directly (inlined here), exactly the branch XXH3_64bits_withSeed -> XXH3_64bits_internal takes for len <= 16 with
	// the default secret, without the out-of-line call and its length dispatch. The same 64-bit hash, cut the same way.
	if (barDroidSyncFast && size <= 16) {
		g_checksum = static_cast<uint32_t>(XXH3_len_0to16_64b(static_cast<const xxh_u8*>(p), static_cast<size_t>(size), XXH3_kSecret, static_cast<XXH64_hash_t>(g_checksum)));
	} else {
		g_checksum = spring::LiteHash(p, size, g_checksum);
	}
#else
	g_checksum = spring::LiteHash(p, size, g_checksum);
#endif
	//LOG("[Sync::Checker] chksum=%u\n", g_checksum);

#ifdef SYNC_HISTORY
	LogHistory();
#endif // SYNC_HISTORY
}

#ifdef SYNC_HISTORY

unsigned CSyncChecker::nextHistoryIndex = 0;
unsigned CSyncChecker::nextFrameIndex = 0;
std::array<unsigned, MAX_SYNC_HISTORY> CSyncChecker::logs;
std::array<unsigned, MAX_SYNC_HISTORY_FRAMES> CSyncChecker::logFrames;

void CSyncChecker::NewGameFrame()
{
	logFrames[nextFrameIndex++] = nextHistoryIndex;
	if (nextFrameIndex == MAX_SYNC_HISTORY_FRAMES)
		nextFrameIndex = 0;
}

void CSyncChecker::LogHistory()
{
	logs[nextHistoryIndex++] = g_checksum;
	if (nextHistoryIndex == MAX_SYNC_HISTORY)
		nextHistoryIndex = 0;
}

std::tuple<unsigned, unsigned, unsigned*> CSyncChecker::GetFrameHistory(unsigned rewindFrames)
{
	int endFrameIndex = nextFrameIndex - rewindFrames;
	int startFrameIndex = endFrameIndex - 1;

	if (endFrameIndex < 0)
		endFrameIndex = MAX_SYNC_HISTORY_FRAMES + endFrameIndex;
	if (startFrameIndex < 0)
		startFrameIndex = MAX_SYNC_HISTORY_FRAMES + startFrameIndex;

	return std::make_tuple(logFrames[startFrameIndex], logFrames[endFrameIndex], logs.data());
}

#endif // SYNC_HISTORY

#endif // SYNCCHECK
