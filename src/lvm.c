/*
** $Id: lvm.c,v 2.2 2004/03/16 12:31:40 roberto Exp $
** Lua virtual machine
** See Copyright Notice in lua.h
*/


#include <stdlib.h>
#include <string.h>

/* needed only when `lua_number2str' uses `sprintf' */
#include <stdio.h>

#define lvm_c

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



/* function to convert a lua_Number to a string */
#ifndef lua_number2str
#define lua_number2str(s,n)     sprintf((s), LUA_NUMBER_FMT, (n))
#endif


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
    char s[32];  /* 16 digits, sign, point and \0  (+ some extra...) */
    lua_number2str(s, nvalue(obj));
    setsvalue2s(L, obj, luaS_new(L, s));
    return 1;
  }
}


static void traceexec (lua_State *L, const Instruction *pc) {
  lu_byte mask = L->hookmask;
  CallInfo *ci = L->ci;
  const Instruction *oldpc = ci->u.l.savedpc;
  ci->u.l.savedpc = pc;
  if (mask > LUA_MASKLINE) {  /* instruction-hook set? */
    if (L->hookcount == 0) {
      resethookcount(L);
      luaD_callhook(L, LUA_HOOKCOUNT, -1);
      return;
    }
  }
  if (mask & LUA_MASKLINE) {
    Proto *p = ci_func(ci)->l.p;
    int npc = pcRel(pc, p);
    int newline = getline(p, npc);
    /* call linehook when enter a new function, when jump back (loop),
       or when enter a new line */
    if (npc == 0 || pc <= oldpc || newline != getline(p, pcRel(oldpc, p)))
      luaD_callhook(L, LUA_HOOKLINE, newline);
  }
}


static void prepTMcall (lua_State *L, const TValue *f,
                        const TValue *p1, const TValue *p2) {
  setobj2s(L, L->top, f);  /* push function */
  setobj2s(L, L->top+1, p1);  /* 1st argument */
  setobj2s(L, L->top+2, p2);  /* 2nd argument */
}


static void callTMres (lua_State *L, StkId res) {
  ptrdiff_t result = savestack(L, res);
  luaD_checkstack(L, 3);
  L->top += 3;
  luaD_call(L, L->top - 3, 1);
  res = restorestack(L, result);
  L->top--;
  setobjs2s(L, res, L->top);
}



static void callTM (lua_State *L) {
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
      const TValue *res = luaH_get(h, key); /* do a primitive set */
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
      prepTMcall(L, tm, t, key);
      callTMres(L, val);
      return;
    }
    t = tm;  /* else repeat with `tm' */ 
  }
  luaG_runerror(L, "loop in gettable");
}


void luaV_settable (lua_State *L, const TValue *t, TValue *key, StkId val) {
  int loop;
  for (loop = 0; loop < MAXTAGLOOP; loop++) {
    const TValue *tm;
    if (ttistable(t)) {  /* `t' is a table? */
      Table *h = hvalue(t);
      TValue *oldval = luaH_set(L, h, key); /* do a primitive set */
      if (!ttisnil(oldval) ||  /* result is no nil? */
          (tm = fasttm(L, h->metatable, TM_NEWINDEX)) == NULL) { /* or no TM? */
        setobj2t(L, oldval, val);
        luaC_barrier(L, h, val);
        return;
      }
      /* else will try the tag method */
    }
    else if (ttisnil(tm = luaT_gettmbyobj(L, t, TM_NEWINDEX)))
      luaG_typeerror(L, t, "index");
    if (ttisfunction(tm)) {
      prepTMcall(L, tm, t, key);
      setobj2s(L, L->top+3, val);  /* 3th argument */
      callTM(L);
      return;
    }
    t = tm;  /* else repeat with `tm' */ 
  }
  luaG_runerror(L, "loop in settable");
}


static int call_binTM (lua_State *L, const TValue *p1, const TValue *p2,
                       StkId res, TMS event) {
  const TValue *tm = luaT_gettmbyobj(L, p1, event);  /* try first operand */
  if (ttisnil(tm))
    tm = luaT_gettmbyobj(L, p2, event);  /* try second operand */
  if (!ttisfunction(tm)) return 0;
  prepTMcall(L, tm, p1, p2);
  callTMres(L, res);
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
  prepTMcall(L, tm1, p1, p2);
  callTMres(L, L->top);
  return !l_isfalse(L->top);
}


static int luaV_strcmp (const TString *ls, const TString *rs) {
  const char *l = getstr(ls);
  size_t ll = ls->tsv.len;
  const char *r = getstr(rs);
  size_t lr = rs->tsv.len;
  for (;;) {
    int temp = strcoll(l, r);
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
    return nvalue(l) < nvalue(r);
  else if (ttisstring(l))
    return luaV_strcmp(rawtsvalue(l), rawtsvalue(r)) < 0;
  else if ((res = call_orderTM(L, l, r, TM_LT)) != -1)
    return res;
  return luaG_ordererror(L, l, r);
}


static int luaV_lessequal (lua_State *L, const TValue *l, const TValue *r) {
  int res;
  if (ttype(l) != ttype(r))
    return luaG_ordererror(L, l, r);
  else if (ttisnumber(l))
    return nvalue(l) <= nvalue(r);
  else if (ttisstring(l))
    return luaV_strcmp(rawtsvalue(l), rawtsvalue(r)) <= 0;
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
    case LUA_TNUMBER: return nvalue(t1) == nvalue(t2);
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
  prepTMcall(L, tm, t1, t2);
  callTMres(L, L->top);  /* call TM */
  return !l_isfalse(L->top);
}


void luaV_concat (lua_State *L, int total, int last) {
  do {
    StkId top = L->base + last + 1;
    int n = 2;  /* number of elements handled in this pass (at least 2) */
    if (!tostring(L, top-2) || !tostring(L, top-1)) {
      if (!call_binTM(L, top-2, top-1, top-2, TM_CONCAT))
        luaG_concaterror(L, top-2, top-1);
    } else if (tsvalue(top-1)->len > 0) {  /* if len=0, do nothing */
      /* at least two string values; get as many as possible */
      lu_mem tl = cast(lu_mem, tsvalue(top-1)->len) +
                  cast(lu_mem, tsvalue(top-2)->len);
      char *buffer;
      int i;
      while (n < total && tostring(L, top-n-1)) {  /* collect total length */
        tl += tsvalue(top-n-1)->len;
        n++;
      }
      if (tl > MAX_SIZET) luaG_runerror(L, "string size overflow");
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


static StkId Arith (lua_State *L, StkId ra, const TValue *rb,
                    const TValue *rc, TMS op, const Instruction *pc) {
  TValue tempb, tempc;
  const TValue *b, *c;
  L->ci->u.l.savedpc = pc;
  if ((b = luaV_tonumber(rb, &tempb)) != NULL &&
      (c = luaV_tonumber(rc, &tempc)) != NULL) {
    switch (op) {
      case TM_ADD: setnvalue(ra, nvalue(b) + nvalue(c)); break;
      case TM_SUB: setnvalue(ra, nvalue(b) - nvalue(c)); break;
      case TM_MUL: setnvalue(ra, nvalue(b) * nvalue(c)); break;
      case TM_DIV: setnvalue(ra, nvalue(b) / nvalue(c)); break;
      case TM_POW: {
        const TValue *f = luaH_getstr(hvalue(gt(L)), G(L)->tmname[TM_POW]);
        if (!ttisfunction(f))
          luaG_runerror(L, "`__pow' (`^' operator) is not a function");
        prepTMcall(L, f, b, c);
        callTMres(L, ra);
        break;
      }
      default: lua_assert(0); break;
    }
  }
  else if (!call_binTM(L, rb, rc, ra, op))
    luaG_aritherror(L, rb, rc);
  return L->base;
}



/*
** some macros for common tasks in `luaV_execute'
*/

#define runtime_check(L, c)	{ if (!(c)) return 0; }

#define RA(i)	(base+GETARG_A(i))
/* to be used after possible stack reallocation */
#define RB(i)	check_exp(getBMode(GET_OPCODE(i)) == OpArgR, base+GETARG_B(i))
#define RC(i)	check_exp(getCMode(GET_OPCODE(i)) == OpArgR, base+GETARG_C(i))
#define RKB(i)	check_exp(getBMode(GET_OPCODE(i)) == OpArgK, \
	(GETARG_B(i) < MAXSTACK) ? base+GETARG_B(i) : k+GETARG_B(i)-MAXSTACK)
#define RKC(i)	check_exp(getCMode(GET_OPCODE(i)) == OpArgK, \
	(GETARG_C(i) < MAXSTACK) ? base+GETARG_C(i) : k+GETARG_C(i)-MAXSTACK)
#define KBx(i)	check_exp(getBMode(GET_OPCODE(i)) == OpArgK, k+GETARG_Bx(i))


#define dojump(pc, i)	((pc) += (i))


StkId luaV_execute (lua_State *L, int nexeccalls) {
  LClosure *cl;
  TValue *k;
  StkId base;
  const Instruction *pc;
 callentry:  /* entry point when calling new functions */
  if (L->hookmask & LUA_MASKCALL)
    luaD_callhook(L, LUA_HOOKCALL, -1);
 retentry:  /* entry point when returning to old functions */
  pc = L->ci->u.l.savedpc;
  base = L->base;
  cl = &clvalue(base - 1)->l;
  k = cl->p->k;
  /* main loop of interpreter */
  for (;;) {
    const Instruction i = *pc++;
    StkId ra;
    if ((L->hookmask & (LUA_MASKLINE | LUA_MASKCOUNT)) &&
        (--L->hookcount == 0 || L->hookmask & LUA_MASKLINE)) {
      traceexec(L, pc);  /***/
      if (L->isSuspended) {  /* did hook yield? */
        L->ci->u.l.savedpc = pc - 1;
        return NULL;
      }
      base = L->base;
    }
    /* warning!! several calls may realloc the stack and invalidate `ra' */
    ra = RA(i);
    lua_assert(base == L->ci->base && base == L->base);
    lua_assert(L->top <= L->stack + L->stacksize && L->top >= base);
    lua_assert(L->top == L->ci->top ||
         GET_OPCODE(i) == OP_CALL ||   GET_OPCODE(i) == OP_TAILCALL ||
         GET_OPCODE(i) == OP_RETURN || GET_OPCODE(i) == OP_SETLISTO);
    switch (GET_OPCODE(i)) {
      case OP_MOVE: {
        setobjs2s(L, ra, RB(i));
        break;
      }
      case OP_LOADK: {
        setobj2s(L, ra, KBx(i));
        break;
      }
      case OP_LOADBOOL: {
        setbvalue(ra, GETARG_B(i));
        if (GETARG_C(i)) pc++;  /* skip next instruction (if C) */
        break;
      }
      case OP_LOADNIL: {
        TValue *rb = RB(i);
        do {
          setnilvalue(rb--);
        } while (rb >= ra);
        break;
      }
      case OP_GETUPVAL: {
        int b = GETARG_B(i);
        setobj2s(L, ra, cl->upvals[b]->v);
        break;
      }
      case OP_GETGLOBAL: {
        TValue *rb = KBx(i);
        lua_assert(ttisstring(rb) && ttistable(&cl->g));
        L->ci->u.l.savedpc = pc;
        luaV_gettable(L, &cl->g, rb, ra);  /***/
        base = L->base;
        break;
      }
      case OP_GETTABLE: {
        L->ci->u.l.savedpc = pc;
        luaV_gettable(L, RB(i), RKC(i), ra);  /***/
        base = L->base;
        break;
      }
      case OP_SETGLOBAL: {
        lua_assert(ttisstring(KBx(i)) && ttistable(&cl->g));
        L->ci->u.l.savedpc = pc;
        luaV_settable(L, &cl->g, KBx(i), ra);  /***/
        base = L->base;
        break;
      }
      case OP_SETUPVAL: {
        UpVal *uv = cl->upvals[GETARG_B(i)];
        setobj(L, uv->v, ra);
        luaC_barrier(L, uv, ra);
        break;
      }
      case OP_SETTABLE: {
        L->ci->u.l.savedpc = pc;
        luaV_settable(L, ra, RKB(i), RKC(i));  /***/
        base = L->base;
        break;
      }
      case OP_NEWTABLE: {
        int b = GETARG_B(i);
        b = fb2int(b);
        sethvalue(L, ra, luaH_new(L, b, GETARG_C(i)));
        L->ci->u.l.savedpc = pc;
        luaC_checkGC(L);  /***/
        base = L->base;
        break;
      }
      case OP_SELF: {
        StkId rb = RB(i);
        setobjs2s(L, ra+1, rb);
        L->ci->u.l.savedpc = pc;
        luaV_gettable(L, rb, RKC(i), ra);  /***/
        base = L->base;
        break;
      }
      case OP_ADD: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        if (ttisnumber(rb) && ttisnumber(rc)) {
          setnvalue(ra, nvalue(rb) + nvalue(rc));
        }
        else
          base = Arith(L, ra, rb, rc, TM_ADD, pc);  /***/
        break;
      }
      case OP_SUB: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        if (ttisnumber(rb) && ttisnumber(rc)) {
          setnvalue(ra, nvalue(rb) - nvalue(rc));
        }
        else
          base = Arith(L, ra, rb, rc, TM_SUB, pc);  /***/
        break;
      }
      case OP_MUL: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        if (ttisnumber(rb) && ttisnumber(rc)) {
          setnvalue(ra, nvalue(rb) * nvalue(rc));
        }
        else
          base = Arith(L, ra, rb, rc, TM_MUL, pc);  /***/
        break;
      }
      case OP_DIV: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        if (ttisnumber(rb) && ttisnumber(rc)) {
          setnvalue(ra, nvalue(rb) / nvalue(rc));
        }
        else
          base = Arith(L, ra, rb, rc, TM_DIV, pc);  /***/
        break;
      }
      case OP_POW: {
        base = Arith(L, ra, RKB(i), RKC(i), TM_POW, pc);  /***/
        break;
      }
      case OP_UNM: {
        const TValue *rb = RB(i);
        TValue temp;
        if (tonumber(rb, &temp)) {
          setnvalue(ra, -nvalue(rb));
        }
        else {
          setnilvalue(&temp);
          L->ci->u.l.savedpc = pc;
          if (!call_binTM(L, RB(i), &temp, ra, TM_UNM))  /***/
            luaG_aritherror(L, RB(i), &temp);
          base = L->base;
        }
        break;
      }
      case OP_NOT: {
        int res = l_isfalse(RB(i));  /* next assignment may change this value */
        setbvalue(ra, res);
        break;
      }
      case OP_CONCAT: {
        int b = GETARG_B(i);
        int c = GETARG_C(i);
        L->ci->u.l.savedpc = pc;
        luaV_concat(L, c-b+1, c);  /* may change `base' (and `ra') */  /***/
        luaC_checkGC(L);  /***/
        base = L->base;
        setobjs2s(L, RA(i), base+b);
        break;
      }
      case OP_JMP: {
        dojump(pc, GETARG_sBx(i));
        break;
      }
      case OP_EQ: {
        L->ci->u.l.savedpc = pc;
        if (equalobj(L, RKB(i), RKC(i)) != GETARG_A(i)) pc++;  /***/
        else dojump(pc, GETARG_sBx(*pc) + 1);
        base = L->base;
        break;
      }
      case OP_LT: {
        L->ci->u.l.savedpc = pc;
        if (luaV_lessthan(L, RKB(i), RKC(i)) != GETARG_A(i)) pc++;  /***/
        else dojump(pc, GETARG_sBx(*pc) + 1);
        base = L->base;
        break;
      }
      case OP_LE: {
        L->ci->u.l.savedpc = pc;
        if (luaV_lessequal(L, RKB(i), RKC(i)) != GETARG_A(i)) pc++;  /***/
        else dojump(pc, GETARG_sBx(*pc) + 1);
        base = L->base;
        break;
      }
      case OP_TEST: {
        TValue *rb = RB(i);
        if (l_isfalse(rb) == GETARG_C(i)) pc++;
        else {
          setobjs2s(L, ra, rb);
          dojump(pc, GETARG_sBx(*pc) + 1);
        }
        break;
      }
      case OP_CALL:
      case OP_TAILCALL: {  /***/
        StkId firstResult;
        int b = GETARG_B(i);
        if (b != 0) L->top = ra+b;  /* else previous instruction set top */
        L->ci->u.l.savedpc = pc;
        firstResult = luaD_precall(L, ra);
        if (firstResult) {
          int nresults = GETARG_C(i) - 1;
          if (firstResult > L->top) {  /* yield? */
            (L->ci - 1)->u.l.savedpc = pc;
            return NULL;
          }
          /* it was a C function (`precall' called it); adjust results */
          luaD_poscall(L, nresults, firstResult);
          if (nresults >= 0) L->top = L->ci->top;
        }
        else {  /* it is a Lua function */
          if (GET_OPCODE(i) == OP_CALL)  /* regular call? */
            nexeccalls++;
          else {  /* tail call: put new frame in place of previous one */
            int aux;
            base = (L->ci - 1)->base;  /* `luaD_precall' may change the stack */
            ra = RA(i);
            if (L->openupval) luaF_close(L, base);
            for (aux = 0; ra+aux < L->top; aux++)  /* move frame down */
              setobjs2s(L, base+aux-1, ra+aux);
            (L->ci - 1)->top = L->top = base+aux;  /* correct top */
            (L->ci - 1)->u.l.savedpc = L->ci->u.l.savedpc;
            (L->ci - 1)->u.l.tailcalls++;  /* one more call lost */
            L->ci--;  /* remove new frame */
            L->base = L->ci->base;
          }
          goto callentry;
        }
        base = L->base;
        break;
      }
      case OP_RETURN: {
        CallInfo *ci = L->ci - 1;  /* previous function frame */
        int b = GETARG_B(i);
        if (b != 0) L->top = ra+b-1;
        if (L->openupval) luaF_close(L, base);
        L->ci->u.l.savedpc = pc;
        if (--nexeccalls == 0)  /* was previous function running `here'? */
          return ra;  /* no: return */
        else {  /* yes: continue its execution */
          int nresults;
          lua_assert(isLua(ci));
          lua_assert(GET_OPCODE(*(ci->u.l.savedpc - 1)) == OP_CALL);
          nresults = GETARG_C(*(ci->u.l.savedpc - 1)) - 1;
          luaD_poscall(L, nresults, ra);
          if (nresults >= 0) L->top = L->ci->top;
          goto retentry;
        }
      }
      case OP_FORLOOP: {
        lua_Number step = nvalue(ra+2);
        lua_Number idx = nvalue(ra) + step;  /* increment index */
        lua_Number limit = nvalue(ra+1);
        if (step > 0 ? idx <= limit : idx >= limit) {
          dojump(pc, GETARG_sBx(i));  /* jump back */
          setnvalue(ra, idx);  /* update internal index... */
          setnvalue(ra+3, idx);  /* ...and external index */
        }
        break;
      }
      case OP_FORPREP: {  /***/
        const TValue *init = ra;
        const TValue *plimit = ra+1;
        const TValue *pstep = ra+2;
        L->ci->u.l.savedpc = pc;
        if (!tonumber(init, ra))
          luaG_runerror(L, "`for' initial value must be a number");
        else if (!tonumber(plimit, ra+1))
          luaG_runerror(L, "`for' limit must be a number");
        else if (!tonumber(pstep, ra+2))
          luaG_runerror(L, "`for' step must be a number");
        setnvalue(ra, nvalue(ra) - nvalue(pstep));
        dojump(pc, GETARG_sBx(i));
        break;
      }
      case OP_TFORLOOP: {
        StkId cb = ra + 3;  /* call base */
        setobjs2s(L, cb+2, ra+2);
        setobjs2s(L, cb+1, ra+1);
        setobjs2s(L, cb, ra);
        L->top = cb+3;  /* func. + 2 args (state and index) */
        L->ci->u.l.savedpc = pc;
        luaD_call(L, cb, GETARG_C(i));  /***/
        L->top = L->ci->top;
        base = L->base;
        cb = RA(i) + 3;  /* previous call may change the stack */
        if (ttisnil(cb))  /* break loop? */
          pc++;  /* skip jump (break loop) */
        else {
          setobjs2s(L, cb-1, cb);  /* save control variable */
          dojump(pc, GETARG_sBx(*pc) + 1);  /* jump back */
        }
        break;
      }
      case OP_TFORPREP: {  /* for compatibility only */
        if (ttistable(ra)) {
          setobjs2s(L, ra+1, ra);
          setobj2s(L, ra, luaH_getstr(hvalue(gt(L)), luaS_new(L, "next")));
        }
        dojump(pc, GETARG_sBx(i));
        break;
      }
      case OP_SETLIST:
      case OP_SETLISTO: {
        int bc;
        int n;
        Table *h;
        runtime_check(L, ttistable(ra));
        h = hvalue(ra);
        bc = GETARG_Bx(i);
        if (GET_OPCODE(i) == OP_SETLIST)
          n = (bc&(LFIELDS_PER_FLUSH-1)) + 1;
        else {
          n = L->top - ra - 1;
          L->top = L->ci->top;
        }
        bc &= ~(LFIELDS_PER_FLUSH-1);  /* bc = bc - bc%FPF */
        for (; n > 0; n--) {
          TValue *val = ra+n;
          setobj2t(L, luaH_setnum(L, h, bc+n), val);
          luaC_barrier(L, h, val);
        }
        break;
      }
      case OP_CLOSE: {
        luaF_close(L, ra);
        break;
      }
      case OP_CLOSURE: {
        Proto *p;
        Closure *ncl;
        int nup, j;
        p = cl->p->p[GETARG_Bx(i)];
        nup = p->nups;
        ncl = luaF_newLclosure(L, nup, &cl->g);
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
        L->ci->u.l.savedpc = pc;
        luaC_checkGC(L);  /***/
        base = L->base;
        break;
      }
    }
  }
}

