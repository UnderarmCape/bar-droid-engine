/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef SPRING_LUA_USER_H
#define SPRING_LUA_USER_H

#include "lua.h"

extern void LuaCreateMutex(lua_State* L);
extern void LuaDestroyMutex(lua_State* L);
extern void LuaLinkMutex(lua_State* L_parent, lua_State* L_child);
extern void LuaMutexLock(lua_State* L);
extern void LuaMutexUnlock(lua_State* L);
extern void LuaMutexYield(lua_State* L);

extern const char* spring_lua_get_handle_name(lua_State* L);

struct SLuaAllocState;
struct SLuaAllocError {
	// includes space for multiple messages, since we do not record them immediately
	char msgBuf[16384] = {0};
	char* msgPtr = nullptr;
};

extern void* spring_lua_alloc(void* ud, void* ptr, size_t osize, size_t nsize);
extern void spring_lua_alloc_get_stats(SLuaAllocState* state);
extern bool spring_lua_alloc_get_error(SLuaAllocError* error);
extern bool spring_lua_alloc_skip_gc(float gcLoadMult);
extern void spring_lua_alloc_update_stats(int clearStatsFrame);


extern void spring_lua_ftoa(float f, char *buf, int precision = -1);
extern void spring_lua_format(float f, const char* fmt, char *buf);

// (these should) never (be) called from synced Lua states
extern int spring_lua_unsynced_rand(lua_State* L);
extern int spring_lua_unsynced_srand(lua_State* L);

#ifdef BARDROID_SIM
// BAR Droid (engine branch): runtime switches for faster code paths of the Lua VM, each computing exactly what stock
// computes (same values, same table layouts and iteration order, same allocations, same error messages); 0 = stock.
// Read from the engine's config (BarDroidLuaReadConfig, LuaHandle.cpp) as each Lua state is created (LUA_OPEN) and once
// a drawn frame (CLuaHandle::Update), so Spring.SetConfigInt switches them in a running game; LuaUser.cpp holds them.
//   vm:           BarDroidLuaVM, the interpreter loop with computed-goto dispatch (luaV_execute_bd); the next four
//                 work inside it only
//   fastTable:    BarDroidLuaFastTable, table reads/writes in the VM without the luaV_gettable/luaH_get calls
//   fastCall:     BarDroidLuaFastCall, Lua-to-Lua and Lua-to-C calls and returns set up in the VM (no luaD_precall /
//                 luaD_poscall calls) when no stack or CallInfo growth and no hook is involved
//   fastCompare:  BarDroidLuaFastCompare, ==, <, <= of two numbers (and == of strings, booleans, nil) in the VM
//   prefetch:     BarDroidLuaPrefetch, cache prefetches (no other effect): a called function's code, constants and
//                 first upvalues, and a function value as it is loaded into a register
//   strHash:      BarDroidLuaStrHash, string interning compares the stored hash before length and bytes
//   allocRelaxed: BarDroidLuaAllocRelaxed, the allocator's byte and call counters with one relaxed atomic add each
//   fastIter:     BarDroidLuaFastIter, a generic for's call of a C iterator (next, ipairs' step) set up in the VM (no
//                 luaD_call / luaD_precall / luaD_poscall calls), as fastCall does for calls; needs vm
struct BarDroidLuaSwitches {
	int vm;
	int fastTable;
	int fastCall;
	int fastCompare;
	int prefetch;
	int strHash;
	int allocRelaxed;
	int fastIter;
};
extern BarDroidLuaSwitches barDroidLua;
// (LuaHandle.cpp; engine builds only)
extern void BarDroidLuaReadConfig(bool force);
#endif

#endif // SPRING_LUA_USER_H
