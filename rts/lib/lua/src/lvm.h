/*
** $Id: lvm.h,v 2.5.1.1 2007/12/27 13:02:25 roberto Exp $
** Lua virtual machine
** See Copyright Notice in lua.h
*/

#ifndef lvm_h
#define lvm_h


#include "ldo.h"
#include "lobject.h"
#include "ltm.h"


#define tostring(L,o) ((ttype(o) == LUA_TSTRING) || (luaV_tostring(L, o)))

#define tonumber(o,n)	(ttype(o) == LUA_TNUMBER || \
                         (((o) = luaV_tonumber(o,n)) != NULL))

#define equalobj(L,o1,o2) \
	(ttype(o1) == ttype(o2) && luaV_equalval(L, o1, o2))


LUAI_FUNC int luaV_lessthan (lua_State *L, const TValue *l, const TValue *r);
LUAI_FUNC int luaV_equalval (lua_State *L, const TValue *t1, const TValue *t2);
LUAI_FUNC const TValue *luaV_tonumber (const TValue *obj, TValue *n);
LUAI_FUNC int luaV_tostring (lua_State *L, StkId obj);
LUAI_FUNC void luaV_gettable (lua_State *L, const TValue *t, TValue *key,
                                            StkId val);
LUAI_FUNC void luaV_settable (lua_State *L, const TValue *t, TValue *key,
                                            StkId val);
LUAI_FUNC void luaV_execute (lua_State *L, int nexeccalls);
LUAI_FUNC void luaV_concat (lua_State *L, int total, int last);
#ifdef BARDROID_SIM
/* BAR Droid (end of lvm.cpp): luaV_execute with computed-goto dispatch and the fast paths of LuaUser.h's barDroidLua,
   the same results. luaV_run (luaD_call and resume, ldo.cpp): the interpreter barDroidLua.vm picks; luaV_execute
   itself stays unchanged (gcc keys PGO profiles by function name and control flow: it keeps its profile) */
LUAI_FUNC void luaV_execute_bd (lua_State *L, int nexeccalls);
#define luaV_run(L,n) \
	{ if (barDroidLua.vm) luaV_execute_bd(L, n); else luaV_execute(L, n); }
#endif

#endif
