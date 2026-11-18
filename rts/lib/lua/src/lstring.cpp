/*
** $Id: lstring.c,v 2.8.1.1 2007/12/27 13:02:25 roberto Exp $
** String table (keeps all strings handled by Lua)
** See Copyright Notice in lua.h
*/


#include <string.h>

#define lstring_c
#define LUA_CORE

#include "lua.h"

#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"



void luaS_resize (lua_State *L, int newsize) {
  GCObject **newhash;
  stringtable *tb;
  int i;
  if (G(L)->gcstate == GCSsweepstring)
    return;  /* cannot resize during GC traverse */
  newhash = luaM_newvector(L, newsize, GCObject *);
  tb = &G(L)->strt;
  for (i=0; i<newsize; i++) newhash[i] = NULL;
  /* rehash */
  for (i=0; i<tb->size; i++) {
    GCObject *p = tb->hash[i];
    while (p) {  /* for each node in the list */
      GCObject *next = p->gch.next;  /* save next */
      unsigned int h = gco2ts(p)->hash;
      int h1 = lmod(h, newsize);  /* new position */
      lua_assert(cast_int(h%newsize) == lmod(h, newsize));
      p->gch.next = newhash[h1];  /* chain it */
      newhash[h1] = p;
      p = next;
    }
  }
  luaM_freearray(L, tb->hash, tb->size, TString *);
  tb->size = newsize;
  tb->hash = newhash;
}


static TString *newlstr (lua_State *L, const char *str, size_t l,
                                       unsigned int h) {
  TString *ts;
  stringtable *tb;
  if (l+1 > (MAX_SIZET - sizeof(TString))/sizeof(char))
    luaM_toobig(L);
  ts = lua_cast(TString *, luaM_malloc(L, (l+1)*sizeof(char)+sizeof(TString)));
  ts->tsv.len = l;
  ts->tsv.hash = h;
  ts->tsv.marked = luaC_white(G(L));
  ts->tsv.tt = LUA_TSTRING;
  ts->tsv.reserved = 0;
  memcpy(ts+1, str, l*sizeof(char));
  ((char *)(ts+1))[l] = '\0';  /* ending 0 */
  tb = &G(L)->strt;
  h = lmod(h, tb->size);
  ts->tsv.next = tb->hash[h];  /* chain new entry */
  tb->hash[h] = obj2gco(ts);
  tb->nuse++;
  if (tb->nuse > lua_cast(lu_int32, tb->size) && tb->size <= MAX_INT/2)
    luaS_resize(L, tb->size*2);  /* too crowded */
  return ts;
}


//SPRING
static inline lua_Hash calchash(const char *str, size_t l) {
  lua_Hash h = lua_cast(unsigned int, l);  /* seed */
  size_t step = (l>>5)+1;  /* if string is too long, don't hash all its chars */
  size_t l1;
  for (l1=l; l1>=step; l1-=step) {  /* compute hash */
    h = h ^ ((h<<5)+(h>>2)+lua_cast(unsigned char, str[l1-1]));
  }
  return h;
}


//SPRING
LUA_API lua_Hash lua_calchash(const char *str, size_t l) {
  return calchash(str, l);
}

#ifdef BARDROID_SIM
__attribute__((hot))  /* (the changed code no longer matches its PGO profile: kept with the hot code, as it was) */
#endif
TString *luaS_newhstr (lua_State *L, lua_Hash h, const char *str, size_t l) {
  GCObject *o;
#ifdef BARDROID_SIM
  /* BAR Droid: BarDroidLuaStrHash, each chained string's stored hash compared before its length and bytes. Every
     interned string's hash is calchash of its bytes (newlstr stores what its caller passed: calchash in luaS_newlstr,
     lua_calchash or CompileTimeHash, the same function, from the engine's lua_pushhstring callers; creg's
     SerializeLuaState recomputes it with lua_calchash), so a string whose hash differs cannot be equal: the same
     string is found, or none and the same new one is made */
  const int strHash = barDroidLua.strHash;
#endif
  for (o = G(L)->strt.hash[lmod(h, G(L)->strt.size)];
       o != NULL;
       o = o->gch.next) {
    TString *ts = rawgco2ts(o);
#ifdef BARDROID_SIM
    if (strHash && ts->tsv.hash != h) continue;
#endif
    if (ts->tsv.len == l && (memcmp(str, getstr(ts), l) == 0)) {
      /* string may be dead */
      if (isdead(G(L), o)) changewhite(o);
      return ts;
    }
  }
  return newlstr(L, str, l, h);  /* not found */
}


//SPRING
#ifdef BARDROID_SIM
__attribute__((hot))  /* (as luaS_newhstr) */
#endif
TString *luaS_newlstr (lua_State *L, const char *str, size_t l) {
  GCObject *o;
  const lua_Hash h = calchash(str, l);
#ifdef BARDROID_SIM
  const int strHash = barDroidLua.strHash;  /* (BarDroidLuaStrHash: as in luaS_newhstr) */
#endif
  for (o = G(L)->strt.hash[lmod(h, G(L)->strt.size)];
       o != NULL;
       o = o->gch.next) {
    TString *ts = rawgco2ts(o);
#ifdef BARDROID_SIM
    if (strHash && ts->tsv.hash != h) continue;
#endif
    if (ts->tsv.len == l && (memcmp(str, getstr(ts), l) == 0)) {
      /* string may be dead */
      if (isdead(G(L), o)) changewhite(o);
      return ts;
    }
  }
  return newlstr(L, str, l, h);  /* not found */
}


Udata *luaS_newudata (lua_State *L, size_t s, Table *e) {
  Udata *u;
  if (s > MAX_SIZET - sizeof(Udata))
    luaM_toobig(L);
  u = lua_cast(Udata *, luaM_malloc(L, s + sizeof(Udata)));
  u->uv.marked = luaC_white(G(L));  /* is not finalized */
  u->uv.tt = LUA_TUSERDATA;
  u->uv.len = s;
  u->uv.metatable = NULL;
  u->uv.env = e;
  /* chain it on udata list (after main thread) */
  u->uv.next = G(L)->mainthread->next;
  G(L)->mainthread->next = obj2gco(u);
  return u;
}

