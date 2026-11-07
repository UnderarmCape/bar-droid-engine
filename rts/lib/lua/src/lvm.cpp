/*
** $Id: lvm.c,v 2.63.1.5 2011/08/17 20:43:11 roberto Exp $
** Lua virtual machine
** See Copyright Notice in lua.h
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//SPRING
#include <algorithm>
#include "streflop_cond.h"

#define lvm_c
#define LUA_CORE

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lvm.h"



/* limit for table tag-method chains (to avoid loops) */
#define MAXTAGLOOP	100


const TValue *luaV_tonumber (const TValue *obj, TValue *n) {
  lua_Number num;
  if (ttisnumber(obj)) return obj;
  if (ttisstring(obj) && luaO_str2d(svalue(obj), &num)) {
    setnvalue(n, num);
    return n;
  }
  else
    return NULL;
}


int luaV_tostring (lua_State *L, StkId obj) {
  if (!ttisnumber(obj))
    return 0;
  else {
    char s[LUAI_MAXNUMBER2STR];
    lua_Number n = nvalue(obj);
    // SPRING -- synced safety change
    //        -- need a custom number formatter?
    if (math::isfinite(n)) {
      lua_number2str(s, n);
    }
    else {
      if (math::isnan(n)) {
        strcpy(s, "nan");
      }
      else {
        const int inf_type = math::isinf(n);
        if (inf_type == 1) {
          strcpy(s, "+inf");
        } else if (inf_type == -1) {
          strcpy(s, "-inf");
        } else {
          strcpy(s, "weird_number");
        }
      }
    } 
    setsvalue2s(L, obj, luaS_new(L, s));
    return 1;
  }
}


static void traceexec (lua_State *L, const Instruction *pc) {
  lu_byte mask = L->hookmask;
  const Instruction *oldpc = L->savedpc;
  L->savedpc = pc;
  if ((mask & LUA_MASKCOUNT) && L->hookcount == 0) {
    resethookcount(L);
    luaD_callhook(L, LUA_HOOKCOUNT, -1);
  }
  if (mask & LUA_MASKLINE) {
    Proto *p = ci_func(L->ci)->l.p;
    int npc = pcRel(pc, p);
    int newline = getline(p, npc);
    /* call linehook when enter a new function, when jump back (loop),
       or when enter a new line */
    if (npc == 0 || pc <= oldpc || newline != getline(p, pcRel(oldpc, p)))
      luaD_callhook(L, LUA_HOOKLINE, newline);
  }
}


static void callTMres (lua_State *L, StkId res, const TValue *f,
                        const TValue *p1, const TValue *p2) {
  ptrdiff_t result = savestack(L, res);
  setobj2s(L, L->top, f);  /* push function */
  setobj2s(L, L->top+1, p1);  /* 1st argument */
  setobj2s(L, L->top+2, p2);  /* 2nd argument */
  luaD_checkstack(L, 3);
  L->top += 3;
  luaD_call(L, L->top - 3, 1);
  res = restorestack(L, result);
  L->top--;
  setobjs2s(L, res, L->top);
}



static void callTM (lua_State *L, const TValue *f, const TValue *p1,
                    const TValue *p2, const TValue *p3) {
  setobj2s(L, L->top, f);  /* push function */
  setobj2s(L, L->top+1, p1);  /* 1st argument */
  setobj2s(L, L->top+2, p2);  /* 2nd argument */
  setobj2s(L, L->top+3, p3);  /* 3th argument */
  luaD_checkstack(L, 4);
  L->top += 4;
  luaD_call(L, L->top - 4, 0);
}


void luaV_gettable (lua_State *L, const TValue *t, TValue *key, StkId val) {
  int loop;
  for (loop = 0; loop < MAXTAGLOOP; loop++) {
    const TValue *tm;
    if (ttistable(t)) {  /* `t' is a table? */
      Table *h = hvalue(t);
      const TValue *res = luaH_get(h, key); /* do a primitive get */
      if (!ttisnil(res) ||  /* result is no nil? */
          (tm = fasttm(L, h->metatable, TM_INDEX)) == NULL) { /* or no TM? */
        setobj2s(L, val, res);
        return;
      }
      /* else will try the tag method */
    }
    else if (ttisnil(tm = luaT_gettmbyobj(L, t, TM_INDEX)))
      luaG_typeerror(L, t, "index");
    if (ttisfunction(tm)) {
      callTMres(L, val, tm, t, key);
      return;
    }
    t = tm;  /* else repeat with `tm' */ 
  }
  luaG_runerror(L, "loop in gettable");
}


void luaV_settable (lua_State *L, const TValue *t, TValue *key, StkId val) {
  int loop;
  TValue temp;
  for (loop = 0; loop < MAXTAGLOOP; loop++) {
    const TValue *tm;
    if (ttistable(t)) {  /* `t' is a table? */
      Table *h = hvalue(t);
      TValue *oldval = luaH_set(L, h, key); /* do a primitive set */
      if (!ttisnil(oldval) ||  /* result is no nil? */
          (tm = fasttm(L, h->metatable, TM_NEWINDEX)) == NULL) { /* or no TM? */
        setobj2t(L, oldval, val);
        h->flags = 0;
        luaC_barriert(L, h, val);
        return;
      }
      /* else will try the tag method */
    }
    else if (ttisnil(tm = luaT_gettmbyobj(L, t, TM_NEWINDEX)))
      luaG_typeerror(L, t, "index");
    if (ttisfunction(tm)) {
      callTM(L, tm, t, key, val);
      return;
    }
    /* else repeat with `tm' */
    setobj(L, &temp, tm);  /* avoid pointing inside table (may rehash) */
    t = &temp;
  }
  luaG_runerror(L, "loop in settable");
}


static int call_binTM (lua_State *L, const TValue *p1, const TValue *p2,
                       StkId res, TMS event) {
  const TValue *tm = luaT_gettmbyobj(L, p1, event);  /* try first operand */
  if (ttisnil(tm))
    tm = luaT_gettmbyobj(L, p2, event);  /* try second operand */
  if (ttisnil(tm)) return 0;
  callTMres(L, res, tm, p1, p2);
  return 1;
}


static const TValue *get_compTM (lua_State *L, Table *mt1, Table *mt2,
                                  TMS event) {
  const TValue *tm1 = fasttm(L, mt1, event);
  const TValue *tm2;
  if (tm1 == NULL) return NULL;  /* no metamethod */
  if (mt1 == mt2) return tm1;  /* same metatables => same metamethods */
  tm2 = fasttm(L, mt2, event);
  if (tm2 == NULL) return NULL;  /* no metamethod */
  if (luaO_rawequalObj(tm1, tm2))  /* same metamethods? */
    return tm1;
  return NULL;
}


static int call_orderTM (lua_State *L, const TValue *p1, const TValue *p2,
                         TMS event) {
  const TValue *tm1 = luaT_gettmbyobj(L, p1, event);
  const TValue *tm2;
  if (ttisnil(tm1)) return -1;  /* no metamethod? */
  tm2 = luaT_gettmbyobj(L, p2, event);
  if (!luaO_rawequalObj(tm1, tm2))  /* different metamethods? */
    return -1;
  callTMres(L, L->top, tm1, p1, p2);
  return !l_isfalse(L->top);
}


static int l_strcmp (const TString *ls, const TString *rs) {
  const char *l = getstr(ls);
  size_t ll = ls->tsv.len;
  const char *r = getstr(rs);
  size_t lr = rs->tsv.len;
  //SPRING
  const size_t n = std::min(ll, lr) + 1;
  //SPRING
  for (;;) {
    //SPRING
    //int temp = strcoll(l, r);
    int temp = strncmp(l, r, n);
    //SPRING
    if (temp != 0) return temp;
    else {  /* strings are equal up to a `\0' */
      size_t len = strlen(l);  /* index of first `\0' in both strings */
      if (len == lr)  /* r is finished? */
        return (len == ll) ? 0 : 1;
      else if (len == ll)  /* l is finished? */
        return -1;  /* l is smaller than r (because r is not finished) */
      /* both strings longer than `len'; go on comparing (after the `\0') */
      len++;
      l += len; ll -= len; r += len; lr -= len;
    }
  }
}


int luaV_lessthan (lua_State *L, const TValue *l, const TValue *r) {
  int res;
  if (ttype(l) != ttype(r))
    return luaG_ordererror(L, l, r);
  else if (ttisnumber(l))
    return luai_numlt(nvalue(l), nvalue(r));
  else if (ttisstring(l))
    return l_strcmp(rawtsvalue(l), rawtsvalue(r)) < 0;
  else if ((res = call_orderTM(L, l, r, TM_LT)) != -1)
    return res;
  return luaG_ordererror(L, l, r);
}


static int lessequal (lua_State *L, const TValue *l, const TValue *r) {
  int res;
  if (ttype(l) != ttype(r))
    return luaG_ordererror(L, l, r);
  else if (ttisnumber(l))
    return luai_numle(nvalue(l), nvalue(r));
  else if (ttisstring(l))
    return l_strcmp(rawtsvalue(l), rawtsvalue(r)) <= 0;
  else if ((res = call_orderTM(L, l, r, TM_LE)) != -1)  /* first try `le' */
    return res;
  else if ((res = call_orderTM(L, r, l, TM_LT)) != -1)  /* else try `lt' */
    return !res;
  return luaG_ordererror(L, l, r);
}


int luaV_equalval (lua_State *L, const TValue *t1, const TValue *t2) {
  const TValue *tm;
  lua_assert(ttype(t1) == ttype(t2));
  switch (ttype(t1)) {
    case LUA_TNIL: return 1;
    case LUA_TNUMBER: return luai_numeq(nvalue(t1), nvalue(t2));
    case LUA_TBOOLEAN: return bvalue(t1) == bvalue(t2);  /* true must be 1 !! */
    case LUA_TLIGHTUSERDATA: return pvalue(t1) == pvalue(t2);
    case LUA_TUSERDATA: {
      if (uvalue(t1) == uvalue(t2)) return 1;
      tm = get_compTM(L, uvalue(t1)->metatable, uvalue(t2)->metatable,
                         TM_EQ);
      break;  /* will try TM */
    }
    case LUA_TTABLE: {
      if (hvalue(t1) == hvalue(t2)) return 1;
      tm = get_compTM(L, hvalue(t1)->metatable, hvalue(t2)->metatable, TM_EQ);
      break;  /* will try TM */
    }
    default: return gcvalue(t1) == gcvalue(t2);
  }
  if (tm == NULL) return 0;  /* no TM? */
  callTMres(L, L->top, tm, t1, t2);  /* call TM */
  return !l_isfalse(L->top);
}


void luaV_concat (lua_State *L, int total, int last) {
  do {
    StkId top = L->base + last + 1;
    int n = 2;  /* number of elements handled in this pass (at least 2) */
    if (!(ttisstring(top-2) || ttisnumber(top-2)) || !tostring(L, top-1)) {
      if (!call_binTM(L, top-2, top-1, top-2, TM_CONCAT))
        luaG_concaterror(L, top-2, top-1);
    } else if (tsvalue(top-1)->len == 0)  /* second op is empty? */
      (void)tostring(L, top - 2);  /* result is first op (as string) */
    else {
      /* at least two string values; get as many as possible */
      size_t tl = tsvalue(top-1)->len;
      char *buffer;
      int i;
      /* collect total length */
      for (n = 1; n < total && tostring(L, top-n-1); n++) {
        size_t l = tsvalue(top-n-1)->len;
        if (l >= MAX_SIZET - tl) luaG_runerror(L, "string length overflow");
        tl += l;
      }
      buffer = luaZ_openspace(L, &G(L)->buff, tl);
      tl = 0;
      for (i=n; i>0; i--) {  /* concat all strings */
        size_t l = tsvalue(top-i)->len;
        memcpy(buffer+tl, svalue(top-i), l);
        tl += l;
      }
      setsvalue2s(L, top-n, luaS_newlstr(L, buffer, tl));
    }
    total -= n-1;  /* got `n' strings to create 1 new */
    last -= n-1;
  } while (total > 1);  /* repeat until only 1 result left */
}


static void Arith (lua_State *L, StkId ra, const TValue *rb,
                   const TValue *rc, TMS op) {
  TValue tempb, tempc;
  const TValue *b, *c;
  if ((b = luaV_tonumber(rb, &tempb)) != NULL &&
      (c = luaV_tonumber(rc, &tempc)) != NULL) {
    lua_Number nb = nvalue(b), nc = nvalue(c);
    switch (op) {
      case TM_ADD: setnvalue(ra, luai_numadd(nb, nc)); break;
      case TM_SUB: setnvalue(ra, luai_numsub(nb, nc)); break;
      case TM_MUL: setnvalue(ra, luai_nummul(nb, nc)); break;
      case TM_DIV: setnvalue(ra, luai_numdiv(nb, nc)); break;
      case TM_MOD: setnvalue(ra, luai_nummod(nb, nc)); break;
      case TM_POW: setnvalue(ra, luai_numpow(nb, nc)); break;
      case TM_UNM: setnvalue(ra, luai_numunm(nb)); break;
      default: lua_assert(0); break;
    }
  }
  else if (!call_binTM(L, rb, rc, ra, op))
    luaG_aritherror(L, rb, rc);
}



/*
** some macros for common tasks in `luaV_execute'
*/

#define runtime_check(L, c)	{ if (!(c)) break; }

#define RA(i)	(base+GETARG_A(i))
/* to be used after possible stack reallocation */
#define RB(i)	check_exp(getBMode(GET_OPCODE(i)) == OpArgR, base+GETARG_B(i))
#define RC(i)	check_exp(getCMode(GET_OPCODE(i)) == OpArgR, base+GETARG_C(i))
#define RKB(i)	check_exp(getBMode(GET_OPCODE(i)) == OpArgK, \
	ISK(GETARG_B(i)) ? k+INDEXK(GETARG_B(i)) : base+GETARG_B(i))
#define RKC(i)	check_exp(getCMode(GET_OPCODE(i)) == OpArgK, \
	ISK(GETARG_C(i)) ? k+INDEXK(GETARG_C(i)) : base+GETARG_C(i))
#define KBx(i)	check_exp(getBMode(GET_OPCODE(i)) == OpArgK, k+GETARG_Bx(i))


#define dojump(L,pc,i)	{(pc) += (i); luai_threadyield(L);}


#define Protect(x)	{ L->savedpc = pc; {x;}; base = L->base; }


#define arith_op(op,tm) { \
        TValue *rb = RKB(i); \
        TValue *rc = RKC(i); \
        if (ttisnumber(rb) && ttisnumber(rc)) { \
          lua_Number nb = nvalue(rb), nc = nvalue(rc); \
          setnvalue(ra, op(nb, nc)); \
        } \
        else \
          Protect(Arith(L, ra, rb, rc, tm)); \
      }



void luaV_execute (lua_State *L, int nexeccalls) {
  LClosure *cl;
  StkId base;
  TValue *k;
  const Instruction *pc;
 reentry:  /* entry point */
  lua_assert(isLua(L->ci));
  pc = L->savedpc;
  cl = &clvalue(L->ci->func)->l;
  base = L->base;
  k = cl->p->k;
  /* main loop of interpreter */
  for (;;) {
    const Instruction i = *pc++;
    StkId ra;
    if ((L->hookmask & (LUA_MASKLINE | LUA_MASKCOUNT)) &&
        (--L->hookcount == 0 || L->hookmask & LUA_MASKLINE)) {
      traceexec(L, pc);
      if (L->status == LUA_YIELD) {  /* did hook yield? */
        L->savedpc = pc - 1;
        return;
      }
      base = L->base;
    }
    /* warning!! several calls may realloc the stack and invalidate `ra' */
    ra = RA(i);
    lua_assert(base == L->base && L->base == L->ci->base);
    lua_assert(base <= L->top && L->top <= L->stack + L->stacksize);
    lua_assert(L->top == L->ci->top || luaG_checkopenop(i));
    switch (GET_OPCODE(i)) {
      case OP_MOVE: {
        setobjs2s(L, ra, RB(i));
        continue;
      }
      case OP_LOADK: {
        setobj2s(L, ra, KBx(i));
        continue;
      }
      case OP_LOADBOOL: {
        setbvalue(ra, GETARG_B(i));
        if (GETARG_C(i)) pc++;  /* skip next instruction (if C) */
        continue;
      }
      case OP_LOADNIL: {
        TValue *rb = RB(i);
        do {
          setnilvalue(rb--);
        } while (rb >= ra);
        continue;
      }
      case OP_GETUPVAL: {
        int b = GETARG_B(i);
        setobj2s(L, ra, cl->upvals[b]->v);
        continue;
      }
      case OP_GETGLOBAL: {
        TValue g;
        TValue *rb = KBx(i);
        sethvalue(L, &g, cl->env);
        lua_assert(ttisstring(rb));
        Protect(luaV_gettable(L, &g, rb, ra));
        continue;
      }
      case OP_GETTABLE: {
        Protect(luaV_gettable(L, RB(i), RKC(i), ra));
        continue;
      }
      case OP_SETGLOBAL: {
        TValue g;
        sethvalue(L, &g, cl->env);
        lua_assert(ttisstring(KBx(i)));
        Protect(luaV_settable(L, &g, KBx(i), ra));
        continue;
      }
      case OP_SETUPVAL: {
        UpVal *uv = cl->upvals[GETARG_B(i)];
        setobj(L, uv->v, ra);
        luaC_barrier(L, uv, ra);
        continue;
      }
      case OP_SETTABLE: {
        Protect(luaV_settable(L, ra, RKB(i), RKC(i)));
        continue;
      }
      case OP_NEWTABLE: {
        int b = GETARG_B(i);
        int c = GETARG_C(i);
        sethvalue(L, ra, luaH_new(L, luaO_fb2int(b), luaO_fb2int(c)));
        Protect(luaC_checkGC(L));
        continue;
      }
      case OP_SELF: {
        StkId rb = RB(i);
        setobjs2s(L, ra+1, rb);
        Protect(luaV_gettable(L, rb, RKC(i), ra));
        continue;
      }
      case OP_ADD: {
        arith_op(luai_numadd, TM_ADD);
        continue;
      }
      case OP_SUB: {
        arith_op(luai_numsub, TM_SUB);
        continue;
      }
      case OP_MUL: {
        arith_op(luai_nummul, TM_MUL);
        continue;
      }
      case OP_DIV: {
        arith_op(luai_numdiv, TM_DIV);
        continue;
      }
      case OP_MOD: {
        arith_op(luai_nummod, TM_MOD);
        continue;
      }
      case OP_POW: {
        arith_op(luai_numpow, TM_POW);
        continue;
      }
      case OP_UNM: {
        TValue *rb = RB(i);
        if (ttisnumber(rb)) {
          lua_Number nb = nvalue(rb);
          setnvalue(ra, luai_numunm(nb));
        }
        else {
          Protect(Arith(L, ra, rb, rb, TM_UNM));
        }
        continue;
      }
      case OP_NOT: {
        int res = l_isfalse(RB(i));  /* next assignment may change this value */
        setbvalue(ra, res);
        continue;
      }
      case OP_LEN: {
        const TValue *rb = RB(i);
        switch (ttype(rb)) {
          case LUA_TTABLE: {
            setnvalue(ra, cast_num(luaH_getn(hvalue(rb))));
            break;
          }
          case LUA_TSTRING: {
            setnvalue(ra, cast_num(tsvalue(rb)->len));
            break;
          }
          default: {  /* try metamethod */
            Protect(
              if (!call_binTM(L, rb, luaO_nilobject, ra, TM_LEN))
                luaG_typeerror(L, rb, "get length of");
            )
          }
        }
        continue;
      }
      case OP_CONCAT: {
        int b = GETARG_B(i);
        int c = GETARG_C(i);
        Protect(luaV_concat(L, c-b+1, c); luaC_checkGC(L));
        setobjs2s(L, RA(i), base+b);
        continue;
      }
      case OP_JMP: {
        dojump(L, pc, GETARG_sBx(i));
        continue;
      }
      case OP_EQ: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        Protect(
          if (equalobj(L, rb, rc) == GETARG_A(i))
            dojump(L, pc, GETARG_sBx(*pc));
        )
        pc++;
        continue;
      }
      case OP_LT: {
        Protect(
          if (luaV_lessthan(L, RKB(i), RKC(i)) == GETARG_A(i))
            dojump(L, pc, GETARG_sBx(*pc));
        )
        pc++;
        continue;
      }
      case OP_LE: {
        Protect(
          if (lessequal(L, RKB(i), RKC(i)) == GETARG_A(i))
            dojump(L, pc, GETARG_sBx(*pc));
        )
        pc++;
        continue;
      }
      case OP_TEST: {
        if (l_isfalse(ra) != GETARG_C(i))
          dojump(L, pc, GETARG_sBx(*pc));
        pc++;
        continue;
      }
      case OP_TESTSET: {
        TValue *rb = RB(i);
        if (l_isfalse(rb) != GETARG_C(i)) {
          setobjs2s(L, ra, rb);
          dojump(L, pc, GETARG_sBx(*pc));
        }
        pc++;
        continue;
      }
      case OP_CALL: {
        int b = GETARG_B(i);
        int nresults = GETARG_C(i) - 1;
        if (b != 0) L->top = ra+b;  /* else previous instruction set top */
        L->savedpc = pc;
        switch (luaD_precall(L, ra, nresults)) {
          case PCRLUA: {
            nexeccalls++;
            goto reentry;  /* restart luaV_execute over new Lua function */
          }
          case PCRC: {
            /* it was a C function (`precall' called it); adjust results */
            if (nresults >= 0) L->top = L->ci->top;
            base = L->base;
            continue;
          }
          default: {
            return;  /* yield */
          }
        }
      }
      case OP_TAILCALL: {
        int b = GETARG_B(i);
        if (b != 0) L->top = ra+b;  /* else previous instruction set top */
        L->savedpc = pc;
        lua_assert(GETARG_C(i) - 1 == LUA_MULTRET);
        switch (luaD_precall(L, ra, LUA_MULTRET)) {
          case PCRLUA: {
            /* tail call: put new frame in place of previous one */
            CallInfo *ci = L->ci - 1;  /* previous frame */
            int aux;
            StkId func = ci->func;
            StkId pfunc = (ci+1)->func;  /* previous function index */
            if (L->openupval) luaF_close(L, ci->base);
            L->base = ci->base = ci->func + ((ci+1)->base - pfunc);
            for (aux = 0; pfunc+aux < L->top; aux++)  /* move frame down */
              setobjs2s(L, func+aux, pfunc+aux);
            ci->top = L->top = func+aux;  /* correct top */
            lua_assert(L->top == L->base + clvalue(func)->l.p->maxstacksize);
            ci->savedpc = L->savedpc;
            ci->tailcalls++;  /* one more call lost */
            L->ci--;  /* remove new frame */
            goto reentry;
          }
          case PCRC: {  /* it was a C function (`precall' called it) */
            base = L->base;
            continue;
          }
          default: {
            return;  /* yield */
          }
        }
      }
      case OP_RETURN: {
        int b = GETARG_B(i);
        if (b != 0) L->top = ra+b-1;
        if (L->openupval) luaF_close(L, base);
        L->savedpc = pc;
        b = luaD_poscall(L, ra);
        if (--nexeccalls == 0)  /* was previous function running `here'? */
          return;  /* no: return */
        else {  /* yes: continue its execution */
          if (b) L->top = L->ci->top;
          lua_assert(isLua(L->ci));
          lua_assert(GET_OPCODE(*((L->ci)->savedpc - 1)) == OP_CALL);
          goto reentry;
        }
      }
      case OP_FORLOOP: {
        lua_Number step = nvalue(ra+2);
        lua_Number idx = luai_numadd(nvalue(ra), step); /* increment index */
        lua_Number limit = nvalue(ra+1);
        if (luai_numlt(0, step) ? luai_numle(idx, limit)
                                : luai_numle(limit, idx)) {
          dojump(L, pc, GETARG_sBx(i));  /* jump back */
          setnvalue(ra, idx);  /* update internal index... */
          setnvalue(ra+3, idx);  /* ...and external index */
        }
        continue;
      }
      case OP_FORPREP: {
        const TValue *init = ra;
        const TValue *plimit = ra+1;
        const TValue *pstep = ra+2;
        L->savedpc = pc;  /* next steps may throw errors */
        if (!tonumber(init, ra))
          luaG_runerror(L, LUA_QL("for") " initial value must be a number");
        else if (!tonumber(plimit, ra+1))
          luaG_runerror(L, LUA_QL("for") " limit must be a number");
        else if (!tonumber(pstep, ra+2))
          luaG_runerror(L, LUA_QL("for") " step must be a number");
        setnvalue(ra, luai_numsub(nvalue(ra), nvalue(pstep)));
        dojump(L, pc, GETARG_sBx(i));
        continue;
      }
      case OP_TFORLOOP: {
        StkId cb = ra + 3;  /* call base */
        setobjs2s(L, cb+2, ra+2);
        setobjs2s(L, cb+1, ra+1);
        setobjs2s(L, cb, ra);
        L->top = cb+3;  /* func. + 2 args (state and index) */
        Protect(luaD_call(L, cb, GETARG_C(i)));
        L->top = L->ci->top;
        cb = RA(i) + 3;  /* previous call may change the stack */
        if (!ttisnil(cb)) {  /* continue loop? */
          setobjs2s(L, cb-1, cb);  /* save control variable */
          dojump(L, pc, GETARG_sBx(*pc));  /* jump back */
        }
        pc++;
        continue;
      }
      case OP_SETLIST: {
        int n = GETARG_B(i);
        int c = GETARG_C(i);
        int last;
        Table *h;
        if (n == 0) {
          n = cast_int(L->top - ra) - 1;
          L->top = L->ci->top;
        }
        if (c == 0) c = cast_int(*pc++);
        runtime_check(L, ttistable(ra));
        h = hvalue(ra);
        last = ((c-1)*LFIELDS_PER_FLUSH) + n;
        if (last > h->sizearray)  /* needs more space? */
          luaH_resizearray(L, h, last);  /* pre-alloc it at once */
        for (; n > 0; n--) {
          TValue *val = ra+n;
          setobj2t(L, luaH_setnum(L, h, last--), val);
          luaC_barriert(L, h, val);
        }
        continue;
      }
      case OP_CLOSE: {
        luaF_close(L, ra);
        continue;
      }
      case OP_CLOSURE: {
        Proto *p;
        Closure *ncl;
        int nup, j;
        p = cl->p->p[GETARG_Bx(i)];
        nup = p->nups;
        ncl = luaF_newLclosure(L, nup, cl->env);
        ncl->l.p = p;
        for (j=0; j<nup; j++, pc++) {
          if (GET_OPCODE(*pc) == OP_GETUPVAL)
            ncl->l.upvals[j] = cl->upvals[GETARG_B(*pc)];
          else {
            lua_assert(GET_OPCODE(*pc) == OP_MOVE);
            ncl->l.upvals[j] = luaF_findupval(L, base + GETARG_B(*pc));
          }
        }
        setclvalue(L, ra, ncl);
        Protect(luaC_checkGC(L));
        continue;
      }
      case OP_VARARG: {
        int b = GETARG_B(i) - 1;
        int j;
        CallInfo *ci = L->ci;
        int n = cast_int(ci->base - ci->func) - cl->p->numparams - 1;
        if (b == LUA_MULTRET) {
          Protect(luaD_checkstack(L, n));
          ra = RA(i);  /* previous call may change the stack */
          b = n;
          L->top = ra + n;
        }
        for (j = 0; j < b; j++) {
          if (j < n) {
            setobjs2s(L, ra + j, ci->base - n + j);
          }
          else {
            setnilvalue(ra + j);
          }
        }
        continue;
      }
    }
  }
}



#ifdef BARDROID_SIM
/*
** {==================================================================
** BAR Droid (engine branch): luaV_execute_bd, luaV_execute with
**  - computed-goto dispatch: each opcode's handler ends in its own indirect jump (BarDroidLuaVM);
**  - BarDroidLuaFastTable: luaV_gettable/luaV_settable's first step inline in the VM, with luaH_get's string and
**    array-part lookups inline (the same lookups as luaH_getstr/luaH_getnum; luaH_get/luaH_set for every other
**    key, every new key and every metamethod step, which run the stock loops' remaining steps);
**  - BarDroidLuaFastCall: luaD_precall's and luaD_poscall's work inline for calls that need no stack or CallInfo
**    growth, no call or return hook, no __call and no vararg adjustment (everything else calls luaD_precall);
**  - BarDroidLuaFastCompare: == of numbers, strings, booleans and nil, < and <= of numbers inline;
**  - BarDroidLuaPrefetch: cache prefetches only (a called Lua function's second code line, constants and first
**    upvalues; a function value as it is loaded into a register, before the call that usually follows).
** Each fast path writes what the stock path writes, in the same order where anything could observe it (stack slots,
** CallInfo fields, L->savedpc, L->base, L->top, table flags, GC barriers), so errors, hooks, tracebacks, the GC and
** C functions see the same state. Nothing here allocates where stock does not, or the other way round.
** luaV_execute itself is left as it is: gcc keys PGO counts by function name and checks the control flow, so it
** keeps its profile (this function has none until the profile is trained again).
** ===================================================================
*/

#if defined(LUA_COMPAT_VARARG)
#error "luaV_execute_bd's vararg calls (BarDroidLuaFastCall) do adjust_varargs without LUA_COMPAT_VARARG's arg table"
#endif

#define BD_INLINE	static inline __attribute__((always_inline))
/* (branch hints for the fast paths' layout: the function has no PGO profile yet) */
#define likely_bd(x)	__builtin_expect(!!(x), 1)
#define unlikely_bd(x)	__builtin_expect(!!(x), 0)
#define BD_NOINLINE	static __attribute__((noinline, hot))

#if (ENABLE_USERSTATE_LOCKS == 0)
/* lua_lock/lua_unlock are LuaMutexLock/LuaMutexUnlock (LuaUser.cpp), empty without ENABLE_USERSTATE_LOCKS: the stock
   VM calls both on every jump (dojump's luai_threadyield) and around every C call; here they are left out */
#define bd_dojump(L,pc,i)	{(pc) += (i);}
#define bd_lock(L)	((void)0)
#define bd_unlock(L)	((void)0)
#else
#define bd_dojump(L,pc,i)	dojump(L,pc,i)
#define bd_lock(L)	lua_lock(L)
#define bd_unlock(L)	lua_unlock(L)
#endif

/* luaH_getstr (ltable.cpp) */
BD_INLINE const TValue *bd_getstr (Table *t, TString *key) {
  Node *n = gnode(t, lmod(key->tsv.hash, sizenode(t)));
  do {  /* check whether `key' is somewhere in the chain */
    if (ttisstring(gkey(n)) && rawtsvalue(gkey(n)) == key)
      return gval(n);  /* that's it */
    else n = gnext(n);
  } while (n);
  return luaO_nilobject;
}


/* luaH_get: its string case and the array part of its integer case here, the rest in luaH_get itself */
BD_INLINE const TValue *bd_rawget (Table *t, const TValue *key) {
  if (likely_bd(ttisstring(key)))
    return bd_getstr(t, rawtsvalue(key));
  if (ttisnumber(key)) {
    int k;
    lua_Number n = nvalue(key);
    lua_number2int(k, n);
    /* luaH_getnum's (1 <= key && key <= t->sizearray), in unsigned arithmetic */
    if (luai_numeq(cast_num(k), n) &&
        lua_cast(unsigned int, k) - 1u < lua_cast(unsigned int, t->sizearray))
      return &t->array[k-1];
  }
  return luaH_get(t, key);
}


/* luaH_set: a key already present here, a new one (or the nil/NaN key errors) in luaH_set, which looks it up again */
BD_INLINE TValue *bd_rawset (lua_State *L, Table *t, const TValue *key) {
  const TValue *p = bd_rawget(t, key);
  t->flags = 0;
  if (likely_bd(p != luaO_nilobject))
    return lua_cast(TValue *, p);
  return luaH_set(L, t, key);
}


/* luaV_gettable's loop from where its step loop0 - 1 found metamethod `tm' of `t' (loop counter loop0 on) */
BD_NOINLINE void bd_gettable_tm (lua_State *L, const TValue *t, const TValue *tm,
                                 TValue *key, StkId val, int loop0) {
  int loop;
  if (ttisfunction(tm)) {
    callTMres(L, val, tm, t, key);
    return;
  }
  t = tm;  /* else repeat with `tm' */
  for (loop = loop0; loop < MAXTAGLOOP; loop++) {
    if (ttistable(t)) {  /* `t' is a table? */
      Table *h = hvalue(t);
      const TValue *res = bd_rawget(h, key); /* do a primitive get */
      if (!ttisnil(res) ||  /* result is no nil? */
          (tm = fasttm(L, h->metatable, TM_INDEX)) == NULL) { /* or no TM? */
        setobj2s(L, val, res);
        return;
      }
      /* else will try the tag method */
    }
    else if (ttisnil(tm = luaT_gettmbyobj(L, t, TM_INDEX)))
      luaG_typeerror(L, t, "index");
    if (ttisfunction(tm)) {
      callTMres(L, val, tm, t, key);
      return;
    }
    t = tm;  /* else repeat with `tm' */
  }
  luaG_runerror(L, "loop in gettable");
}


/* luaV_gettable's loop after its first step found metamethod `tm' of `t': its second step inline when `tm' is a table
   (an __index table: a class, a widget's environment over its System table), the rest in bd_gettable_tm; returns
   nonzero when a metamethod may have run (the stack may have moved) */
BD_INLINE int bd_gettable_next (lua_State *L, const TValue *t, const TValue *tm, TValue *key, StkId val) {
  if (likely_bd(ttistable(tm))) {
    Table *h = hvalue(tm);
    const TValue *res = bd_rawget(h, key);  /* (the loop's step 1: t = tm) */
    const TValue *tm2;
    if (likely_bd(!ttisnil(res) || (tm2 = fasttm(L, h->metatable, TM_INDEX)) == NULL)) {
      setobj2s(L, val, res);
      return 0;
    }
    bd_gettable_tm(L, tm, tm2, key, val, 2);
    return 1;
  }
  bd_gettable_tm(L, t, tm, key, val, 1);
  return 1;
}


/* luaV_settable's loop from where its first step found metamethod `tm' of table `t' (loop counter 1 on) */
BD_NOINLINE void bd_settable_tm (lua_State *L, const TValue *t, const TValue *tm,
                                 TValue *key, StkId val) {
  int loop;
  TValue temp;
  if (ttisfunction(tm)) {
    callTM(L, tm, t, key, val);
    return;
  }
  setobj(L, &temp, tm);  /* avoid pointing inside table (may rehash) */
  t = &temp;
  for (loop = 1; loop < MAXTAGLOOP; loop++) {
    if (ttistable(t)) {  /* `t' is a table? */
      Table *h = hvalue(t);
      TValue *oldval = bd_rawset(L, h, key); /* do a primitive set */
      if (!ttisnil(oldval) ||  /* result is no nil? */
          (tm = fasttm(L, h->metatable, TM_NEWINDEX)) == NULL) { /* or no TM? */
        setobj2t(L, oldval, val);
        h->flags = 0;
        luaC_barriert(L, h, val);
        return;
      }
      /* else will try the tag method */
    }
    else if (ttisnil(tm = luaT_gettmbyobj(L, t, TM_NEWINDEX)))
      luaG_typeerror(L, t, "index");
    if (ttisfunction(tm)) {
      callTM(L, tm, t, key, val);
      return;
    }
    /* else repeat with `tm' */
    setobj(L, &temp, tm);  /* avoid pointing inside table (may rehash) */
    t = &temp;
  }
  luaG_runerror(L, "loop in settable");
}


/* luaD_poscall (ldo.cpp); with a return hook set, luaD_poscall itself */
BD_INLINE int bd_poscall (lua_State *L, StkId firstResult) {
  StkId res;
  int wanted, n;
  CallInfo *ci;
  if (unlikely_bd(L->hookmask & LUA_MASKRET))
    return luaD_poscall(L, firstResult);
  ci = L->ci--;
  res = ci->func;  /* res == final position of 1st result */
  wanted = ci->nresults;
  L->base = (ci - 1)->base;  /* restore base */
  L->savedpc = (ci - 1)->savedpc;  /* restore savedpc */
  /* move results to correct place */
  for (n = wanted; n != 0 && firstResult < L->top; n--)
    setobjs2s(L, res++, firstResult++);
  while (n-- > 0)
    setnilvalue(res++);
  L->top = res;
  return (wanted - LUA_MULTRET);  /* 0 iff wanted == LUA_MULTRET */
}


/* the first step of luaV_gettable(L, t, key, val) for a table `t'; anything else as stock (Protect included) */
#define BD_GETTABLE(t_,key_,val_) { \
        const TValue *gt_ = (t_); TValue *gk_ = (key_); StkId gv_ = (val_); \
        if (likely_bd(fastTable && ttistable(gt_))) { \
          Table *gh_ = hvalue(gt_); \
          const TValue *gr_ = bd_rawget(gh_, gk_); \
          const TValue *gtm_; \
          L->savedpc = pc; \
          if (likely_bd(!ttisnil(gr_) || (gtm_ = fasttm(L, gh_->metatable, TM_INDEX)) == NULL)) { \
            setobj2s(L, gv_, gr_); \
            if (prefetch && ttisfunction(gv_)) __builtin_prefetch(clvalue(gv_)); \
          } \
          else { \
            if (bd_gettable_next(L, gt_, gtm_, gk_, gv_)) \
              base = L->base; \
          } \
        } \
        else \
          Protect(luaV_gettable(L, gt_, gk_, gv_)); \
      }

/* the first step of luaV_settable(L, t, key, val) for a table `t'; anything else as stock (Protect included) */
#define BD_SETTABLE(t_,key_,val_) { \
        const TValue *st_ = (t_); TValue *sk_ = (key_); StkId sv_ = (val_); \
        if (likely_bd(fastTable && ttistable(st_))) { \
          Table *sh_ = hvalue(st_); \
          TValue *so_; \
          const TValue *stm_; \
          L->savedpc = pc; \
          so_ = bd_rawset(L, sh_, sk_); \
          if (likely_bd(!ttisnil(so_) || (stm_ = fasttm(L, sh_->metatable, TM_NEWINDEX)) == NULL)) { \
            setobj2t(L, so_, sv_); \
            sh_->flags = 0; \
            luaC_barriert(L, sh_, sv_); \
          } \
          else { \
            bd_settable_tm(L, st_, stm_, sk_, sv_); \
            base = L->base; \
          } \
        } \
        else \
          Protect(luaV_settable(L, st_, sk_, sv_)); \
      }

/* the stock loop's head: fetch, the count/line hook, then the opcode's handler */
#define BD_DISPATCH() { \
        i = *pc++; \
        if (__builtin_expect((L->hookmask & (LUA_MASKLINE | LUA_MASKCOUNT)) != 0, 0)) \
          goto bd_hook; \
        goto *bd_ops[GET_OPCODE(i)]; \
      }

/* a called Lua function's first two code lines, constants and first upvalues, issued as soon as its closure and
   prototype are known, so they load while the call is set up (prefetches only) */
#define BD_PREFETCH_ENTRY(ncl_,p_) { \
        if (prefetch) { \
          const LClosure *pcl_ = (ncl_); \
          const Proto *pp_ = (p_); \
          const int pnup_ = pcl_->nupvalues; \
          __builtin_prefetch(pp_->code); \
          __builtin_prefetch(pp_->code + 16); \
          __builtin_prefetch(pp_->k); \
          if (pnup_ > 0) __builtin_prefetch(pcl_->upvals[0]); \
          if (pnup_ > 1) __builtin_prefetch(pcl_->upvals[1]); \
          if (pnup_ > 2) __builtin_prefetch(pcl_->upvals[2]); \
          if (pnup_ > 3) __builtin_prefetch(pcl_->upvals[3]); \
        } \
      }


/* (hot: with the hot code; no-crossjumping: each handler keeps its own dispatch jump, gcc merges their tails
   otherwise; the attribute keeps the file's FP flags, checked: no fused multiply-adds, the same constants) */
__attribute__((hot, optimize("no-crossjumping")))
void luaV_execute_bd (lua_State *L, int nexeccalls) {
  /* every 6-bit opcode; the ones Lua 5.1 does not have do nothing, as the stock switch */
  static const void *const bd_ops[64] = {
    &&bd_MOVE, &&bd_LOADK, &&bd_LOADBOOL, &&bd_LOADNIL, &&bd_GETUPVAL, &&bd_GETGLOBAL, &&bd_GETTABLE,
    &&bd_SETGLOBAL, &&bd_SETUPVAL, &&bd_SETTABLE, &&bd_NEWTABLE, &&bd_SELF, &&bd_ADD, &&bd_SUB, &&bd_MUL,
    &&bd_DIV, &&bd_MOD, &&bd_POW, &&bd_UNM, &&bd_NOT, &&bd_LEN, &&bd_CONCAT, &&bd_JMP, &&bd_EQ, &&bd_LT,
    &&bd_LE, &&bd_TEST, &&bd_TESTSET, &&bd_CALL, &&bd_TAILCALL, &&bd_RETURN, &&bd_FORLOOP, &&bd_FORPREP,
    &&bd_TFORLOOP, &&bd_SETLIST, &&bd_CLOSE, &&bd_CLOSURE, &&bd_VARARG,
    &&bd_NOOP, &&bd_NOOP, &&bd_NOOP, &&bd_NOOP, &&bd_NOOP, &&bd_NOOP, &&bd_NOOP, &&bd_NOOP, &&bd_NOOP,
    &&bd_NOOP, &&bd_NOOP, &&bd_NOOP, &&bd_NOOP, &&bd_NOOP, &&bd_NOOP, &&bd_NOOP, &&bd_NOOP, &&bd_NOOP,
    &&bd_NOOP, &&bd_NOOP, &&bd_NOOP, &&bd_NOOP, &&bd_NOOP, &&bd_NOOP, &&bd_NOOP, &&bd_NOOP
  };
  const bool fastTable = barDroidLua.fastTable != 0;
  const bool fastCall = barDroidLua.fastCall != 0;
  const bool fastCompare = barDroidLua.fastCompare != 0;
  const bool prefetch = barDroidLua.prefetch != 0;
  const bool fastIter = barDroidLua.fastIter != 0;
  LClosure *cl;
  StkId base;
  TValue *k;
  const Instruction *pc;
  Instruction i;
  StkId ra;
 bd_reentry:  /* entry point */
  lua_assert(isLua(L->ci));
  pc = L->savedpc;
  cl = &clvalue(L->ci->func)->l;
  base = L->base;
  k = cl->p->k;
  BD_DISPATCH();

 bd_hook: {
    if (--L->hookcount == 0 || L->hookmask & LUA_MASKLINE) {
      traceexec(L, pc);
      if (L->status == LUA_YIELD) {  /* did hook yield? */
        L->savedpc = pc - 1;
        return;
      }
      base = L->base;
    }
    goto *bd_ops[GET_OPCODE(i)];
  }

  /* warning!! several calls may realloc the stack and invalidate `ra' */
 bd_MOVE: {
    ra = RA(i);
    setobjs2s(L, ra, RB(i));
    BD_DISPATCH();
  }
 bd_LOADK: {
    ra = RA(i);
    setobj2s(L, ra, KBx(i));
    BD_DISPATCH();
  }
 bd_LOADBOOL: {
    ra = RA(i);
    setbvalue(ra, GETARG_B(i));
    if (GETARG_C(i)) pc++;  /* skip next instruction (if C) */
    BD_DISPATCH();
  }
 bd_LOADNIL: {
    ra = RA(i);
    TValue *rb = RB(i);
    do {
      setnilvalue(rb--);
    } while (rb >= ra);
    BD_DISPATCH();
  }
 bd_GETUPVAL: {
    ra = RA(i);
    int b = GETARG_B(i);
    setobj2s(L, ra, cl->upvals[b]->v);
    if (prefetch && ttisfunction(ra)) __builtin_prefetch(clvalue(ra));
    BD_DISPATCH();
  }
 bd_GETGLOBAL: {
    ra = RA(i);
    TValue *rb = KBx(i);
    lua_assert(ttisstring(rb));
    if (fastTable) {
      Table *h = cl->env;
      const TValue *res = bd_rawget(h, rb);
      const TValue *tm;
      L->savedpc = pc;
      if (!ttisnil(res) || (tm = fasttm(L, h->metatable, TM_INDEX)) == NULL) {
        setobj2s(L, ra, res);
        if (prefetch && ttisfunction(ra)) __builtin_prefetch(clvalue(ra));
      }
      else {
        TValue g;
        sethvalue(L, &g, h);
        if (bd_gettable_next(L, &g, tm, rb, ra))
          base = L->base;
      }
    }
    else {
      TValue g;
      sethvalue(L, &g, cl->env);
      Protect(luaV_gettable(L, &g, rb, ra));
    }
    BD_DISPATCH();
  }
 bd_GETTABLE: {
    ra = RA(i);
    BD_GETTABLE(RB(i), RKC(i), ra);
    BD_DISPATCH();
  }
 bd_SETGLOBAL: {
    ra = RA(i);
    TValue *rb = KBx(i);
    lua_assert(ttisstring(rb));
    if (fastTable) {
      Table *h = cl->env;
      TValue *oldval;
      const TValue *tm;
      L->savedpc = pc;
      oldval = bd_rawset(L, h, rb);
      if (!ttisnil(oldval) || (tm = fasttm(L, h->metatable, TM_NEWINDEX)) == NULL) {
        setobj2t(L, oldval, ra);
        h->flags = 0;
        luaC_barriert(L, h, ra);
      }
      else {
        TValue g;
        sethvalue(L, &g, h);
        bd_settable_tm(L, &g, tm, rb, ra);
        base = L->base;
      }
    }
    else {
      TValue g;
      sethvalue(L, &g, cl->env);
      Protect(luaV_settable(L, &g, rb, ra));
    }
    BD_DISPATCH();
  }
 bd_SETUPVAL: {
    ra = RA(i);
    UpVal *uv = cl->upvals[GETARG_B(i)];
    setobj(L, uv->v, ra);
    luaC_barrier(L, uv, ra);
    BD_DISPATCH();
  }
 bd_SETTABLE: {
    ra = RA(i);
    BD_SETTABLE(ra, RKB(i), RKC(i));
    BD_DISPATCH();
  }
 bd_NEWTABLE: {
    ra = RA(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    sethvalue(L, ra, luaH_new(L, luaO_fb2int(b), luaO_fb2int(c)));
    Protect(luaC_checkGC(L));
    BD_DISPATCH();
  }
 bd_SELF: {
    ra = RA(i);
    StkId rb = RB(i);
    setobjs2s(L, ra+1, rb);
    BD_GETTABLE(rb, RKC(i), ra);
    BD_DISPATCH();
  }
 bd_ADD: {
    ra = RA(i);
    arith_op(luai_numadd, TM_ADD);
    BD_DISPATCH();
  }
 bd_SUB: {
    ra = RA(i);
    arith_op(luai_numsub, TM_SUB);
    BD_DISPATCH();
  }
 bd_MUL: {
    ra = RA(i);
    arith_op(luai_nummul, TM_MUL);
    BD_DISPATCH();
  }
 bd_DIV: {
    ra = RA(i);
    arith_op(luai_numdiv, TM_DIV);
    BD_DISPATCH();
  }
 bd_MOD: {
    ra = RA(i);
    arith_op(luai_nummod, TM_MOD);
    BD_DISPATCH();
  }
 bd_POW: {
    ra = RA(i);
    arith_op(luai_numpow, TM_POW);
    BD_DISPATCH();
  }
 bd_UNM: {
    ra = RA(i);
    TValue *rb = RB(i);
    if (ttisnumber(rb)) {
      lua_Number nb = nvalue(rb);
      setnvalue(ra, luai_numunm(nb));
    }
    else {
      Protect(Arith(L, ra, rb, rb, TM_UNM));
    }
    BD_DISPATCH();
  }
 bd_NOT: {
    ra = RA(i);
    int res = l_isfalse(RB(i));  /* next assignment may change this value */
    setbvalue(ra, res);
    BD_DISPATCH();
  }
 bd_LEN: {
    ra = RA(i);
    const TValue *rb = RB(i);
    switch (ttype(rb)) {
      case LUA_TTABLE: {
        setnvalue(ra, cast_num(luaH_getn(hvalue(rb))));
        break;
      }
      case LUA_TSTRING: {
        setnvalue(ra, cast_num(tsvalue(rb)->len));
        break;
      }
      default: {  /* try metamethod */
        Protect(
          if (!call_binTM(L, rb, luaO_nilobject, ra, TM_LEN))
            luaG_typeerror(L, rb, "get length of");
        )
      }
    }
    BD_DISPATCH();
  }
 bd_CONCAT: {
    ra = RA(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    Protect(luaV_concat(L, c-b+1, c); luaC_checkGC(L));
    setobjs2s(L, RA(i), base+b);
    BD_DISPATCH();
  }
 bd_JMP: {
    bd_dojump(L, pc, GETARG_sBx(i));
    BD_DISPATCH();
  }
 bd_EQ: {
    ra = RA(i);
    TValue *rb = RKB(i);
    TValue *rc = RKC(i);
    int res;
    L->savedpc = pc;
    if (ttype(rb) != ttype(rc))
      res = 0;  /* (equalobj) */
    else if (likely_bd(fastCompare && ttisnumber(rb)))
      res = luai_numeq(nvalue(rb), nvalue(rc));
    else if (fastCompare && ttisstring(rb))
      res = (rawtsvalue(rb) == rawtsvalue(rc));  /* (luaV_equalval's default: the same object) */
    else if (fastCompare && ttisboolean(rb))
      res = (bvalue(rb) == bvalue(rc));
    else if (fastCompare && ttisnil(rb))
      res = 1;
    else {
      res = luaV_equalval(L, rb, rc);
      base = L->base;
    }
    if (res == GETARG_A(i))
      bd_dojump(L, pc, GETARG_sBx(*pc));
    pc++;
    BD_DISPATCH();
  }
 bd_LT: {
    ra = RA(i);
    TValue *rb = RKB(i);
    TValue *rc = RKC(i);
    int res;
    L->savedpc = pc;
    if (likely_bd(fastCompare && ttisnumber(rb) && ttisnumber(rc)))
      res = luai_numlt(nvalue(rb), nvalue(rc));  /* (luaV_lessthan's number case) */
    else {
      res = luaV_lessthan(L, rb, rc);
      base = L->base;
    }
    if (res == GETARG_A(i))
      bd_dojump(L, pc, GETARG_sBx(*pc));
    pc++;
    BD_DISPATCH();
  }
 bd_LE: {
    ra = RA(i);
    TValue *rb = RKB(i);
    TValue *rc = RKC(i);
    int res;
    L->savedpc = pc;
    if (likely_bd(fastCompare && ttisnumber(rb) && ttisnumber(rc)))
      res = luai_numle(nvalue(rb), nvalue(rc));  /* (lessequal's number case) */
    else {
      res = lessequal(L, rb, rc);
      base = L->base;
    }
    if (res == GETARG_A(i))
      bd_dojump(L, pc, GETARG_sBx(*pc));
    pc++;
    BD_DISPATCH();
  }
 bd_TEST: {
    ra = RA(i);
    if (l_isfalse(ra) != GETARG_C(i))
      bd_dojump(L, pc, GETARG_sBx(*pc));
    pc++;
    BD_DISPATCH();
  }
 bd_TESTSET: {
    ra = RA(i);
    TValue *rb = RB(i);
    if (l_isfalse(rb) != GETARG_C(i)) {
      setobjs2s(L, ra, rb);
      bd_dojump(L, pc, GETARG_sBx(*pc));
    }
    pc++;
    BD_DISPATCH();
  }
 bd_CALL: {
    ra = RA(i);
    int b = GETARG_B(i);
    int nresults = GETARG_C(i) - 1;
    if (b != 0) L->top = ra+b;  /* else previous instruction set top */
    L->savedpc = pc;
    /* luaD_precall without its calls: a function (no __call), no call hook, a free CallInfo (inc_ci without growCI),
       room on the stack (luaD_checkstack without luaD_growstack); a vararg Lua function's adjust_varargs too (this
       Lua has no LUA_COMPAT_VARARG: no `arg' table) */
    if (likely_bd(fastCall && ttisfunction(ra) && !(L->hookmask & LUA_MASKCALL) && L->ci != L->end_ci)) {
      Closure *ncl = clvalue(ra);
      if (!ncl->c.isC) {
        Proto *p = ncl->l.p;
        if (likely_bd(!((char *)L->stack_last - (char *)L->top <= p->maxstacksize*(int)sizeof(TValue)))) {
          CallInfo *ci;
          StkId st, nbase;
          BD_PREFETCH_ENTRY(&ncl->l, p);
          L->ci->savedpc = L->savedpc;
          if (likely_bd(!p->is_vararg)) {  /* no varargs? */
            nbase = ra + 1;
            if (L->top > nbase + p->numparams)
              L->top = nbase + p->numparams;
          }
          else {  /* adjust_varargs (ldo.cpp) without LUA_COMPAT_VARARG */
            const int nfixargs = p->numparams;
            int actual = cast_int(L->top - ra) - 1;
            StkId fixed;
            int j;
            for (; actual < nfixargs; ++actual)
              setnilvalue(L->top++);
            /* move fixed parameters to final position */
            fixed = L->top - actual;  /* first fixed argument */
            nbase = L->top;  /* final position of first argument */
            for (j = 0; j < nfixargs; j++) {
              setobjs2s(L, L->top++, fixed+j);
              setnilvalue(fixed+j);
            }
          }
          ci = ++L->ci;  /* now `enter' new function */
          ci->func = ra;
          L->base = ci->base = nbase;
          ci->top = L->base + p->maxstacksize;
          lua_assert(ci->top <= L->stack_last);
          L->savedpc = p->code;  /* starting point */
          ci->tailcalls = 0;
          ci->nresults = nresults;
          for (st = L->top; st < ci->top; st++)
            setnilvalue(st);
          L->top = ci->top;
          nexeccalls++;
          /* (the stock path's goto reentry, its loads known here) */
          cl = &ncl->l;
          base = nbase;
          k = p->k;
          pc = p->code;
          BD_DISPATCH();
        }
      }
      else if (!((char *)L->stack_last - (char *)L->top <= LUA_MINSTACK*(int)sizeof(TValue))) {
        CallInfo *ci;
        int n;
        L->ci->savedpc = L->savedpc;
        ci = ++L->ci;  /* now `enter' new function */
        ci->func = ra;
        L->base = ci->base = ci->func + 1;
        ci->top = L->top + LUA_MINSTACK;
        lua_assert(ci->top <= L->stack_last);
        ci->nresults = nresults;
        bd_unlock(L);
        n = (*ncl->c.f)(L);  /* do the actual call */
        bd_lock(L);
        if (n < 0)  /* yielding? */
          return;  /* (PCRYIELD) */
        bd_poscall(L, L->top - n);
        /* (PCRC) */
        if (nresults >= 0) L->top = L->ci->top;
        base = L->base;
        BD_DISPATCH();
      }
    }
    switch (luaD_precall(L, ra, nresults)) {
      case PCRLUA: {
        nexeccalls++;
        goto bd_reentry;  /* restart luaV_execute over new Lua function */
      }
      case PCRC: {
        /* it was a C function (`precall' called it); adjust results */
        if (nresults >= 0) L->top = L->ci->top;
        base = L->base;
        BD_DISPATCH();
      }
      default: {
        return;  /* yield */
      }
    }
  }
 bd_TAILCALL: {
    ra = RA(i);
    int b = GETARG_B(i);
    if (b != 0) L->top = ra+b;  /* else previous instruction set top */
    L->savedpc = pc;
    lua_assert(GETARG_C(i) - 1 == LUA_MULTRET);
    /* BarDroidLuaFastCall for a Lua function: luaD_precall's Lua case as in OP_CALL, then the frame move below */
    if (likely_bd(fastCall && ttisfunction(ra) && !clvalue(ra)->c.isC && !(L->hookmask & LUA_MASKCALL) &&
                  L->ci != L->end_ci)) {
      LClosure *ncl = &clvalue(ra)->l;
      Proto *p = ncl->p;
      if (likely_bd(!((char *)L->stack_last - (char *)L->top <= p->maxstacksize*(int)sizeof(TValue)))) {
        CallInfo *ci;
        StkId st, nbase, func, pfunc;
        int aux;
        BD_PREFETCH_ENTRY(ncl, p);
        L->ci->savedpc = L->savedpc;
        if (likely_bd(!p->is_vararg)) {  /* no varargs? */
          nbase = ra + 1;
          if (L->top > nbase + p->numparams)
            L->top = nbase + p->numparams;
        }
        else {  /* adjust_varargs (ldo.cpp) without LUA_COMPAT_VARARG */
          const int nfixargs = p->numparams;
          int actual = cast_int(L->top - ra) - 1;
          StkId fixed;
          int j;
          for (; actual < nfixargs; ++actual)
            setnilvalue(L->top++);
          fixed = L->top - actual;  /* first fixed argument */
          nbase = L->top;  /* final position of first argument */
          for (j = 0; j < nfixargs; j++) {
            setobjs2s(L, L->top++, fixed+j);
            setnilvalue(fixed+j);
          }
        }
        ci = ++L->ci;  /* now `enter' new function */
        ci->func = ra;
        L->base = ci->base = nbase;
        ci->top = L->base + p->maxstacksize;
        lua_assert(ci->top <= L->stack_last);
        L->savedpc = p->code;  /* starting point */
        ci->tailcalls = 0;
        ci->nresults = LUA_MULTRET;
        for (st = L->top; st < ci->top; st++)
          setnilvalue(st);
        L->top = ci->top;
        /* tail call: put new frame in place of previous one (the PCRLUA case below) */
        ci = L->ci - 1;  /* previous frame */
        func = ci->func;
        pfunc = (ci+1)->func;  /* previous function index */
        if (L->openupval) luaF_close(L, ci->base);
        L->base = ci->base = ci->func + ((ci+1)->base - pfunc);
        for (aux = 0; pfunc+aux < L->top; aux++)  /* move frame down */
          setobjs2s(L, func+aux, pfunc+aux);
        ci->top = L->top = func+aux;  /* correct top */
        ci->savedpc = L->savedpc;
        ci->tailcalls++;  /* one more call lost */
        L->ci--;  /* remove new frame */
        /* (bd_reentry's loads, known here) */
        cl = ncl;
        base = L->base;
        k = p->k;
        pc = L->savedpc;
        BD_DISPATCH();
      }
    }
    switch (luaD_precall(L, ra, LUA_MULTRET)) {
      case PCRLUA: {
        /* tail call: put new frame in place of previous one */
        CallInfo *ci = L->ci - 1;  /* previous frame */
        int aux;
        StkId func = ci->func;
        StkId pfunc = (ci+1)->func;  /* previous function index */
        if (L->openupval) luaF_close(L, ci->base);
        L->base = ci->base = ci->func + ((ci+1)->base - pfunc);
        for (aux = 0; pfunc+aux < L->top; aux++)  /* move frame down */
          setobjs2s(L, func+aux, pfunc+aux);
        ci->top = L->top = func+aux;  /* correct top */
        lua_assert(L->top == L->base + clvalue(func)->l.p->maxstacksize);
        ci->savedpc = L->savedpc;
        ci->tailcalls++;  /* one more call lost */
        L->ci--;  /* remove new frame */
        goto bd_reentry;
      }
      case PCRC: {  /* it was a C function (`precall' called it) */
        base = L->base;
        BD_DISPATCH();
      }
      default: {
        return;  /* yield */
      }
    }
  }
 bd_RETURN: {
    ra = RA(i);
    int b = GETARG_B(i);
    if (b != 0) L->top = ra+b-1;
    if (L->openupval) luaF_close(L, base);
    L->savedpc = pc;
    b = fastCall ? bd_poscall(L, ra) : luaD_poscall(L, ra);
    if (--nexeccalls == 0)  /* was previous function running `here'? */
      return;  /* no: return */
    else {  /* yes: continue its execution */
      if (b) L->top = L->ci->top;
      lua_assert(isLua(L->ci));
      lua_assert(GET_OPCODE(*((L->ci)->savedpc - 1)) == OP_CALL);
      goto bd_reentry;
    }
  }
 bd_FORLOOP: {
    ra = RA(i);
    lua_Number step = nvalue(ra+2);
    lua_Number idx = luai_numadd(nvalue(ra), step); /* increment index */
    lua_Number limit = nvalue(ra+1);
    if (luai_numlt(0, step) ? luai_numle(idx, limit)
                            : luai_numle(limit, idx)) {
      bd_dojump(L, pc, GETARG_sBx(i));  /* jump back */
      setnvalue(ra, idx);  /* update internal index... */
      setnvalue(ra+3, idx);  /* ...and external index */
    }
    BD_DISPATCH();
  }
 bd_FORPREP: {
    ra = RA(i);
    const TValue *init = ra;
    const TValue *plimit = ra+1;
    const TValue *pstep = ra+2;
    L->savedpc = pc;  /* next steps may throw errors */
    if (!tonumber(init, ra))
      luaG_runerror(L, LUA_QL("for") " initial value must be a number");
    else if (!tonumber(plimit, ra+1))
      luaG_runerror(L, LUA_QL("for") " limit must be a number");
    else if (!tonumber(pstep, ra+2))
      luaG_runerror(L, LUA_QL("for") " step must be a number");
    setnvalue(ra, luai_numsub(nvalue(ra), nvalue(pstep)));
    bd_dojump(L, pc, GETARG_sBx(i));
    BD_DISPATCH();
  }
 bd_TFORLOOP: {
    ra = RA(i);
    StkId cb = ra + 3;  /* call base */
    setobjs2s(L, cb+2, ra+2);
    setobjs2s(L, cb+1, ra+1);
    setobjs2s(L, cb, ra);
    L->top = cb+3;  /* func. + 2 args (state and index) */
    /* BarDroidLuaFastIter: luaD_call(L, cb, C) for a C iterator (next, ipairs' step) without the luaD_call and
       luaD_precall/poscall calls: the same nCcalls count around it (a yield in it fails as in stock), the same
       CallInfo, the same results, then luaD_call's luaC_checkGC. A Lua iterator, a hook, growth or the C-stack limit:
       luaD_call itself */
    if (likely_bd(fastIter && ttisfunction(cb) && clvalue(cb)->c.isC && !(L->hookmask & LUA_MASKCALL) &&
        L->ci != L->end_ci && L->nCcalls + 1 < LUAI_MAXCCALLS &&
        !((char *)L->stack_last - (char *)L->top <= LUA_MINSTACK*(int)sizeof(TValue)))) {
      CallInfo *ci;
      int n;
      L->savedpc = pc;  /* (Protect) */
      ++L->nCcalls;
      L->ci->savedpc = L->savedpc;
      ci = ++L->ci;  /* now `enter' new function */
      ci->func = cb;
      L->base = ci->base = ci->func + 1;
      ci->top = L->top + LUA_MINSTACK;
      ci->nresults = GETARG_C(i);
      bd_unlock(L);
      n = (*clvalue(cb)->c.f)(L);  /* do the actual call */
      bd_lock(L);
      if (n >= 0)  /* (else PCRYIELD: luaD_call goes on without the poscall) */
        bd_poscall(L, L->top - n);
      L->nCcalls--;
      luaC_checkGC(L);
      base = L->base;  /* (Protect) */
    }
    else
    Protect(luaD_call(L, cb, GETARG_C(i)));
    L->top = L->ci->top;
    cb = RA(i) + 3;  /* previous call may change the stack */
    if (!ttisnil(cb)) {  /* continue loop? */
      setobjs2s(L, cb-1, cb);  /* save control variable */
      bd_dojump(L, pc, GETARG_sBx(*pc));  /* jump back */
    }
    pc++;
    BD_DISPATCH();
  }
 bd_SETLIST: {
    ra = RA(i);
    int n = GETARG_B(i);
    int c = GETARG_C(i);
    int last;
    Table *h;
    if (n == 0) {
      n = cast_int(L->top - ra) - 1;
      L->top = L->ci->top;
    }
    if (c == 0) c = cast_int(*pc++);
    if (!ttistable(ra))  /* (runtime_check: the stock switch's break, on to the next instruction) */
      BD_DISPATCH();
    h = hvalue(ra);
    last = ((c-1)*LFIELDS_PER_FLUSH) + n;
    if (last > h->sizearray)  /* needs more space? */
      luaH_resizearray(L, h, last);  /* pre-alloc it at once */
    for (; n > 0; n--) {
      TValue *val = ra+n;
      setobj2t(L, luaH_setnum(L, h, last--), val);
      luaC_barriert(L, h, val);
    }
    BD_DISPATCH();
  }
 bd_CLOSE: {
    ra = RA(i);
    luaF_close(L, ra);
    BD_DISPATCH();
  }
 bd_CLOSURE: {
    ra = RA(i);
    Proto *p;
    Closure *ncl;
    int nup, j;
    p = cl->p->p[GETARG_Bx(i)];
    nup = p->nups;
    ncl = luaF_newLclosure(L, nup, cl->env);
    ncl->l.p = p;
    for (j=0; j<nup; j++, pc++) {
      if (GET_OPCODE(*pc) == OP_GETUPVAL)
        ncl->l.upvals[j] = cl->upvals[GETARG_B(*pc)];
      else {
        lua_assert(GET_OPCODE(*pc) == OP_MOVE);
        ncl->l.upvals[j] = luaF_findupval(L, base + GETARG_B(*pc));
      }
    }
    setclvalue(L, ra, ncl);
    Protect(luaC_checkGC(L));
    BD_DISPATCH();
  }
 bd_VARARG: {
    ra = RA(i);
    int b = GETARG_B(i) - 1;
    int j;
    CallInfo *ci = L->ci;
    int n = cast_int(ci->base - ci->func) - cl->p->numparams - 1;
    if (b == LUA_MULTRET) {
      Protect(luaD_checkstack(L, n));
      ra = RA(i);  /* previous call may change the stack */
      b = n;
      L->top = ra + n;
    }
    for (j = 0; j < b; j++) {
      if (j < n) {
        setobjs2s(L, ra + j, ci->base - n + j);
      }
      else {
        setnilvalue(ra + j);
      }
    }
    BD_DISPATCH();
  }
 bd_NOOP: {
    BD_DISPATCH();
  }
}


/* }================================================================== */
#endif

