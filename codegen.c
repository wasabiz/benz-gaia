/**
 * See Copyright Notice in picrin.h
 */

#include "picrin.h"
#include "picrin/pair.h"
#include "picrin/irep.h"
#include "picrin/proc.h"
#include "picrin/lib.h"
#include "picrin/macro.h"
#include "picrin/dict.h"
#include "picrin/symbol.h"

#if PIC_NONE_IS_FALSE
# define OP_PUSHNONE OP_PUSHFALSE
#else
# error enable PIC_NONE_IS_FALSE
#endif

typedef xvect_t(pic_sym *) xvect;

#define xv_push_sym(v, x) xv_push(pic_sym *, (v), (x))

/**
 * scope object
 */

typedef struct analyze_scope {
  int depth;
  bool varg;
  xvect args, locals, captures; /* rest args variable is counted as a local */
  pic_value defer;
  struct analyze_scope *up;
} analyze_scope;

/**
 * global analyzer state
 */

typedef struct analyze_state {
  pic_state *pic;
  analyze_scope *scope;
  pic_sym *rCONS, *rCAR, *rCDR, *rNILP;
  pic_sym *rSYMBOLP, *rPAIRP;
  pic_sym *rADD, *rSUB, *rMUL, *rDIV;
  pic_sym *rEQ, *rLT, *rLE, *rGT, *rGE, *rNOT;
  pic_sym *rVALUES, *rCALL_WITH_VALUES;
} analyze_state;

static bool push_scope(analyze_state *, pic_value);
static void pop_scope(analyze_state *);

#define register_symbol(pic, state, slot, name) do {    \
    state->slot = pic_intern_cstr(pic, name);           \
  } while (0)

#define register_renamed_symbol(pic, state, slot, lib, id) do {         \
    pic_sym *sym, *gsym;                                                  \
    sym = pic_intern_cstr(pic, id);                                     \
    if (! pic_find_rename(pic, lib->env, sym, &gsym)) {                 \
      pic_errorf(pic, "internal error! native VM procedure not found: %s", id); \
    }                                                                   \
    state->slot = gsym;                                                 \
  } while (0)

static analyze_state *
new_analyze_state(pic_state *pic)
{
  analyze_state *state;
  pic_sym *sym;
  xh_entry *it;

  state = pic_alloc(pic, sizeof(analyze_state));
  state->pic = pic;
  state->scope = NULL;

  /* native VM procedures */
  register_renamed_symbol(pic, state, rCONS, pic->PICRIN_BASE, "cons");
  register_renamed_symbol(pic, state, rCAR, pic->PICRIN_BASE, "car");
  register_renamed_symbol(pic, state, rCDR, pic->PICRIN_BASE, "cdr");
  register_renamed_symbol(pic, state, rNILP, pic->PICRIN_BASE, "null?");
  register_renamed_symbol(pic, state, rSYMBOLP, pic->PICRIN_BASE, "symbol?");
  register_renamed_symbol(pic, state, rPAIRP, pic->PICRIN_BASE, "pair?");
  register_renamed_symbol(pic, state, rADD, pic->PICRIN_BASE, "+");
  register_renamed_symbol(pic, state, rSUB, pic->PICRIN_BASE, "-");
  register_renamed_symbol(pic, state, rMUL, pic->PICRIN_BASE, "*");
  register_renamed_symbol(pic, state, rDIV, pic->PICRIN_BASE, "/");
  register_renamed_symbol(pic, state, rEQ, pic->PICRIN_BASE, "=");
  register_renamed_symbol(pic, state, rLT, pic->PICRIN_BASE, "<");
  register_renamed_symbol(pic, state, rLE, pic->PICRIN_BASE, "<=");
  register_renamed_symbol(pic, state, rGT, pic->PICRIN_BASE, ">");
  register_renamed_symbol(pic, state, rGE, pic->PICRIN_BASE, ">=");
  register_renamed_symbol(pic, state, rNOT, pic->PICRIN_BASE, "not");
  register_renamed_symbol(pic, state, rVALUES, pic->PICRIN_BASE, "values");
  register_renamed_symbol(pic, state, rCALL_WITH_VALUES, pic->PICRIN_BASE, "call-with-values");

  /* push initial scope */
  push_scope(state, pic_nil_value());

  pic_dict_for_each (sym, pic->globals, it) {
    xv_push_sym(state->scope->locals, sym);
  }

  return state;
}

static void
destroy_analyze_state(analyze_state *state)
{
  pop_scope(state);
  pic_free(state->pic, state);
}

static bool
analyze_args(pic_state *pic, pic_value formals, bool *varg, xvect *args, xvect *locals)
{
  pic_value v, t;
  pic_sym *sym;

  for (v = formals; pic_pair_p(v); v = pic_cdr(pic, v)) {
    t = pic_car(pic, v);
    if (! pic_sym_p(t)) {
      return false;
    }
    sym = pic_sym_ptr(t);
    xv_push_sym(*args, sym);
  }
  if (pic_nil_p(v)) {
    *varg = false;
  }
  else if (pic_sym_p(v)) {
    *varg = true;
    sym = pic_sym_ptr(v);
    xv_push_sym(*locals, sym);
  }
  else {
    return false;
  }

  return true;
}

static bool
push_scope(analyze_state *state, pic_value formals)
{
  pic_state *pic = state->pic;
  analyze_scope *scope = pic_alloc(pic, sizeof(analyze_scope));
  bool varg;

  xv_init(scope->args);
  xv_init(scope->locals);
  xv_init(scope->captures);

  if (analyze_args(pic, formals, &varg, &scope->args, &scope->locals)) {
    scope->up = state->scope;
    scope->depth = scope->up ? scope->up->depth + 1 : 0;
    scope->varg = varg;
    scope->defer = pic_nil_value();

    state->scope = scope;

    return true;
  }
  else {
    xv_destroy(scope->args);
    xv_destroy(scope->locals);
    xv_destroy(scope->captures);
    pic_free(pic, scope);
    return false;
  }
}

static void
pop_scope(analyze_state *state)
{
  pic_state *pic = state->pic;
  analyze_scope *scope;

  scope = state->scope;
  xv_destroy(scope->args);
  xv_destroy(scope->locals);
  xv_destroy(scope->captures);

  scope = scope->up;
  pic_free(state->pic, state->scope);
  state->scope = scope;
}

static bool
lookup_scope(analyze_scope *scope, pic_sym *sym)
{
  size_t i;

  /* args */
  for (i = 0; i < xv_size(scope->args); ++i) {
    if (xv_A(scope->args, i) == sym)
      return true;
  }
  /* locals */
  for (i = 0; i < xv_size(scope->locals); ++i) {
    if (xv_A(scope->locals, i) == sym)
      return true;
  }
  return false;
}

static void
capture_var(pic_state *pic, analyze_scope *scope, pic_sym *sym)
{
  size_t i;

  for (i = 0; i < xv_size(scope->captures); ++i) {
    if (xv_A(scope->captures, i) == sym) {
      break;
    }
  }
  if (i == xv_size(scope->captures)) {
    xv_push_sym(scope->captures, sym);
  }
}

static int
find_var(analyze_state *state, pic_sym *sym)
{
  analyze_scope *scope = state->scope;
  int depth = 0;

  while (scope) {
    if (lookup_scope(scope, sym)) {
      if (depth > 0) {
        capture_var(state->pic, scope, sym);
      }
      return depth;
    }
    depth++;
    scope = scope->up;
  }
  return -1;
}

static void
define_var(analyze_state *state, pic_sym *sym)
{
  pic_state *pic = state->pic;
  analyze_scope *scope = state->scope;

  if (lookup_scope(scope, sym)) {
    pic_warnf(pic, "redefining variable: ~s", pic_obj_value(sym));
    return;
  }

  xv_push_sym(scope->locals, sym);
}

static pic_value analyze_node(analyze_state *, pic_value, bool);
static pic_value analyze_procedure(analyze_state *, pic_value, pic_value, pic_value);

static pic_value
analyze(analyze_state *state, pic_value obj, bool tailpos)
{
  pic_state *pic = state->pic;
  size_t ai = pic_gc_arena_preserve(pic);
  pic_value res;
  pic_sym *tag;

  res = analyze_node(state, obj, tailpos);

  tag = pic_sym_ptr(pic_car(pic, res));
  if (tailpos) {
    if (tag == pic->sIF || tag == pic->sBEGIN || tag == pic->sTAILCALL || tag == pic->sTAILCALL_WITH_VALUES || tag == pic->sRETURN) {
      /* pass through */
    }
    else {
      res = pic_list2(pic, pic_obj_value(pic->sRETURN), res);
    }
  }

  pic_gc_arena_restore(pic, ai);
  pic_gc_protect(pic, res);
  pic_gc_protect(pic, state->scope->defer);
  return res;
}

static pic_value
analyze_global_var(analyze_state *state, pic_sym *sym)
{
  pic_state *pic = state->pic;

  return pic_list2(pic, pic_obj_value(pic->sGREF), pic_obj_value(sym));
}

static pic_value
analyze_local_var(analyze_state *state, pic_sym *sym)
{
  pic_state *pic = state->pic;

  return pic_list2(pic, pic_obj_value(pic->sLREF), pic_obj_value(sym));
}

static pic_value
analyze_free_var(analyze_state *state, pic_sym *sym, int depth)
{
  pic_state *pic = state->pic;

  return pic_list3(pic, pic_obj_value(pic->sCREF), pic_int_value(depth), pic_obj_value(sym));
}

static pic_value
analyze_var(analyze_state *state, pic_sym *sym)
{
  pic_state *pic = state->pic;
  int depth;

  if ((depth = find_var(state, sym)) == -1) {
    pic_errorf(pic, "unbound variable %s", pic_symbol_name(pic, sym));
  }

  if (depth == state->scope->depth) {
    return analyze_global_var(state, sym);
  } else if (depth == 0) {
    return analyze_local_var(state, sym);
  } else {
    return analyze_free_var(state, sym, depth);
  }
}

static pic_value
analyze_defer(analyze_state *state, pic_value name, pic_value formal, pic_value body)
{
  pic_state *pic = state->pic;
  pic_sym *sNOWHERE = pic_intern_cstr(pic, "<<nowhere>>");
  pic_value skel;

  skel = pic_list2(pic, pic_obj_value(pic->sGREF), pic_obj_value(sNOWHERE));

  pic_push(pic, pic_list4(pic, name, formal, body, skel), state->scope->defer);

  return skel;
}

static void
analyze_deferred(analyze_state *state)
{
  pic_state *pic = state->pic;
  pic_value defer, val, name, formal, body, dst, it;

  pic_for_each (defer, pic_reverse(pic, state->scope->defer), it) {
    name = pic_list_ref(pic, defer, 0);
    formal = pic_list_ref(pic, defer, 1);
    body = pic_list_ref(pic, defer, 2);
    dst = pic_list_ref(pic, defer, 3);

    val = analyze_procedure(state, name, formal, body);

    /* copy */
    pic_pair_ptr(dst)->car = pic_car(pic, val);
    pic_pair_ptr(dst)->cdr = pic_cdr(pic, val);
  }

  state->scope->defer = pic_nil_value();
}

static pic_value
analyze_procedure(analyze_state *state, pic_value name, pic_value formals, pic_value body_exprs)
{
  pic_state *pic = state->pic;
  pic_value args, locals, varg, captures, body;

  assert(pic_sym_p(name) || pic_false_p(name));

  if (push_scope(state, formals)) {
    analyze_scope *scope = state->scope;
    size_t i;

    args = pic_nil_value();
    for (i = xv_size(scope->args); i > 0; --i) {
      pic_push(pic, pic_obj_value(xv_A(scope->args, i - 1)), args);
    }

    varg = scope->varg
      ? pic_true_value()
      : pic_false_value();

    /* To know what kind of local variables are defined, analyze body at first. */
    body = analyze(state, pic_cons(pic, pic_obj_value(pic->rBEGIN), body_exprs), true);

    analyze_deferred(state);

    locals = pic_nil_value();
    for (i = xv_size(scope->locals); i > 0; --i) {
      pic_push(pic, pic_obj_value(xv_A(scope->locals, i - 1)), locals);
    }

    captures = pic_nil_value();
    for (i = xv_size(scope->captures); i > 0; --i) {
      pic_push(pic, pic_obj_value(xv_A(scope->captures, i - 1)), captures);
    }

    pop_scope(state);
  }
  else {
    pic_errorf(pic, "invalid formal syntax: ~s", args);
  }

  return pic_list7(pic, pic_obj_value(pic->sLAMBDA), name, args, locals, varg, captures, body);
}

static pic_value
analyze_lambda(analyze_state *state, pic_value obj)
{
  pic_state *pic = state->pic;
  pic_value formals, body_exprs;

  if (pic_length(pic, obj) < 2) {
    pic_errorf(pic, "syntax error");
  }

  formals = pic_list_ref(pic, obj, 1);
  body_exprs = pic_list_tail(pic, obj, 2);

  return analyze_defer(state, pic_false_value(), formals, body_exprs);
}

static pic_value
analyze_declare(analyze_state *state, pic_sym *var)
{
  define_var(state, var);

  return analyze_var(state, var);
}

static pic_value
analyze_define(analyze_state *state, pic_value obj)
{
  pic_state *pic = state->pic;
  pic_value var, val;
  pic_sym *sym;

  if (pic_length(pic, obj) != 3) {
    pic_errorf(pic, "syntax error");
  }

  var = pic_list_ref(pic, obj, 1);
  if (! pic_sym_p(var)) {
    pic_errorf(pic, "syntax error");
  } else {
    sym = pic_sym_ptr(var);
  }
  var = analyze_declare(state, sym);

  if (pic_pair_p(pic_list_ref(pic, obj, 2))
      && pic_sym_p(pic_list_ref(pic, pic_list_ref(pic, obj, 2), 0))
      && pic_sym_ptr(pic_list_ref(pic, pic_list_ref(pic, obj, 2), 0)) == pic->rLAMBDA) {
    pic_value formals, body_exprs;

    formals = pic_list_ref(pic, pic_list_ref(pic, obj, 2), 1);
    body_exprs = pic_list_tail(pic, pic_list_ref(pic, obj, 2), 2);

    val = analyze_defer(state, pic_obj_value(sym), formals, body_exprs);
  } else {
    if (pic_length(pic, obj) != 3) {
      pic_errorf(pic, "syntax error");
    }
    val = analyze(state, pic_list_ref(pic, obj, 2), false);
  }

  return pic_list3(pic, pic_obj_value(pic->sSETBANG), var, val);
}

static pic_value
analyze_if(analyze_state *state, pic_value obj, bool tailpos)
{
  pic_state *pic = state->pic;
  pic_value cond, if_true, if_false;

  if_false = pic_none_value();
  switch (pic_length(pic, obj)) {
  default:
    pic_errorf(pic, "syntax error");
  case 4:
    if_false = pic_list_ref(pic, obj, 3);
    PIC_FALLTHROUGH;
  case 3:
    if_true = pic_list_ref(pic, obj, 2);
  }

  /* analyze in order */
  cond = analyze(state, pic_list_ref(pic, obj, 1), false);
  if_true = analyze(state, if_true, tailpos);
  if_false = analyze(state, if_false, tailpos);

  return pic_list4(pic, pic_obj_value(pic->sIF), cond, if_true, if_false);
}

static pic_value
analyze_begin(analyze_state *state, pic_value obj, bool tailpos)
{
  pic_state *pic = state->pic;
  pic_value seq;
  bool tail;

  switch (pic_length(pic, obj)) {
  case 1:
    return analyze(state, pic_none_value(), tailpos);
  case 2:
    return analyze(state, pic_list_ref(pic, obj, 1), tailpos);
  default:
    seq = pic_list1(pic, pic_obj_value(pic->sBEGIN));
    for (obj = pic_cdr(pic, obj); ! pic_nil_p(obj); obj = pic_cdr(pic, obj)) {
      if (pic_nil_p(pic_cdr(pic, obj))) {
        tail = tailpos;
      } else {
        tail = false;
      }
      seq = pic_cons(pic, analyze(state, pic_car(pic, obj), tail), seq);
    }
    return pic_reverse(pic, seq);
  }
}

static pic_value
analyze_set(analyze_state *state, pic_value obj)
{
  pic_state *pic = state->pic;
  pic_value var, val;

  if (pic_length(pic, obj) != 3) {
    pic_errorf(pic, "syntax error");
  }

  var = pic_list_ref(pic, obj, 1);
  if (! pic_sym_p(var)) {
    pic_errorf(pic, "syntax error");
  }

  val = pic_list_ref(pic, obj, 2);

  var = analyze(state, var, false);
  val = analyze(state, val, false);

  return pic_list3(pic, pic_obj_value(pic->sSETBANG), var, val);
}

static pic_value
analyze_quote(analyze_state *state, pic_value obj)
{
  pic_state *pic = state->pic;

  if (pic_length(pic, obj) != 2) {
    pic_errorf(pic, "syntax error");
  }
  return pic_list2(pic, pic_obj_value(pic->sQUOTE), pic_list_ref(pic, obj, 1));
}

#define ARGC_ASSERT_GE(n) do {				\
    if (pic_length(pic, obj) < (n) + 1) {		\
      pic_errorf(pic, "wrong number of arguments");	\
    }                                                   \
  } while (0)

#define FOLD_ARGS(sym) do {                             \
    obj = analyze(state, pic_car(pic, args), false);    \
    pic_for_each (arg, pic_cdr(pic, args), it) {        \
      obj = pic_list3(pic, pic_obj_value(sym), obj,     \
                      analyze(state, arg, false));      \
    }                                                   \
  } while (0)

static pic_value
analyze_add(analyze_state *state, pic_value obj, bool tailpos)
{
  pic_state *pic = state->pic;
  pic_value args, arg, it;

  ARGC_ASSERT_GE(0);
  switch (pic_length(pic, obj)) {
  case 1:
    return pic_list2(pic, pic_obj_value(pic->sQUOTE), pic_int_value(0));
  case 2:
    return analyze(state, pic_car(pic, pic_cdr(pic, obj)), tailpos);
  default:
    args = pic_cdr(pic, obj);
    FOLD_ARGS(pic->sADD);
    return obj;
  }
}

static pic_value
analyze_sub(analyze_state *state, pic_value obj)
{
  pic_state *pic = state->pic;
  pic_value args, arg, it;

  ARGC_ASSERT_GE(1);
  switch (pic_length(pic, obj)) {
  case 2:
    return pic_list2(pic, pic_obj_value(pic->sMINUS),
                     analyze(state, pic_car(pic, pic_cdr(pic, obj)), false));
  default:
    args = pic_cdr(pic, obj);
    FOLD_ARGS(pic->sSUB);
    return obj;
  }
}

static pic_value
analyze_mul(analyze_state *state, pic_value obj, bool tailpos)
{
  pic_state *pic = state->pic;
  pic_value args, arg, it;

  ARGC_ASSERT_GE(0);
  switch (pic_length(pic, obj)) {
  case 1:
    return pic_list2(pic, pic_obj_value(pic->sQUOTE), pic_int_value(1));
  case 2:
    return analyze(state, pic_car(pic, pic_cdr(pic, obj)), tailpos);
  default:
    args = pic_cdr(pic, obj);
    FOLD_ARGS(pic->sMUL);
    return obj;
  }
}

static pic_value
analyze_div(analyze_state *state, pic_value obj)
{
  pic_state *pic = state->pic;
  pic_value args, arg, it;

  ARGC_ASSERT_GE(1);
  switch (pic_length(pic, obj)) {
  case 2:
    args = pic_cdr(pic, obj);
#if PIC_ENABLE_FLOAT
    obj = pic_list3(pic, pic_car(pic, obj), pic_float_value(1), pic_car(pic, args));
#else
    obj = pic_list3(pic, pic_car(pic, obj), pic_int_value(1), pic_car(pic, args));
#endif
    return analyze(state, obj, false);
  default:
    args = pic_cdr(pic, obj);
    FOLD_ARGS(pic->sDIV);
    return obj;
  }
}

static pic_value
analyze_call(analyze_state *state, pic_value obj, bool tailpos)
{
  pic_state *pic = state->pic;
  pic_value seq, elt, it;
  pic_sym *call;

  if (! tailpos) {
    call = pic->sCALL;
  } else {
    call = pic->sTAILCALL;
  }
  seq = pic_list1(pic, pic_obj_value(call));
  pic_for_each (elt, obj, it) {
    seq = pic_cons(pic, analyze(state, elt, false), seq);
  }
  return pic_reverse(pic, seq);
}

static pic_value
analyze_values(analyze_state *state, pic_value obj, bool tailpos)
{
  pic_state *pic = state->pic;
  pic_value v, seq, it;

  if (! tailpos) {
    return analyze_call(state, obj, false);
  }

  seq = pic_list1(pic, pic_obj_value(pic->sRETURN));
  pic_for_each (v, pic_cdr(pic, obj), it) {
    seq = pic_cons(pic, analyze(state, v, false), seq);
  }
  return pic_reverse(pic, seq);
}

static pic_value
analyze_call_with_values(analyze_state *state, pic_value obj, bool tailpos)
{
  pic_state *pic = state->pic;
  pic_value prod, cnsm;
  pic_sym *call;

  if (pic_length(pic, obj) != 3) {
    pic_errorf(pic, "wrong number of arguments");
  }

  if (! tailpos) {
    call = pic->sCALL_WITH_VALUES;
  } else {
    call = pic->sTAILCALL_WITH_VALUES;
  }
  prod = analyze(state, pic_list_ref(pic, obj, 1), false);
  cnsm = analyze(state, pic_list_ref(pic, obj, 2), false);
  return pic_list3(pic, pic_obj_value(call), prod, cnsm);
}

#define ARGC_ASSERT(n) do {				\
    if (pic_length(pic, obj) != (n) + 1) {		\
      pic_errorf(pic, "wrong number of arguments");	\
    }                                                   \
  } while (0)

#define ARGC_ASSERT_WITH_FALLBACK(n) do {       \
    if (pic_length(pic, obj) != (n) + 1) {      \
      goto fallback;                            \
    }						\
  } while (0)

#define CONSTRUCT_OP1(op)                                       \
  pic_list2(pic,                                                \
            pic_obj_value(op),                                  \
            analyze(state, pic_list_ref(pic, obj, 1), false))

#define CONSTRUCT_OP2(op)                                       \
  pic_list3(pic,                                                \
            pic_obj_value(op),                                  \
            analyze(state, pic_list_ref(pic, obj, 1), false),   \
            analyze(state, pic_list_ref(pic, obj, 2), false))

static pic_value
analyze_node(analyze_state *state, pic_value obj, bool tailpos)
{
  pic_state *pic = state->pic;

  switch (pic_type(obj)) {
  case PIC_TT_SYMBOL: {
    return analyze_var(state, pic_sym_ptr(obj));
  }
  case PIC_TT_PAIR: {
    pic_value proc;

    if (! pic_list_p(obj)) {
      pic_errorf(pic, "invalid expression given: ~s", obj);
    }

    proc = pic_list_ref(pic, obj, 0);
    if (pic_sym_p(proc)) {
      pic_sym *sym = pic_sym_ptr(proc);

      if (sym == pic->rDEFINE) {
        return analyze_define(state, obj);
      }
      else if (sym == pic->rLAMBDA) {
        return analyze_lambda(state, obj);
      }
      else if (sym == pic->rIF) {
        return analyze_if(state, obj, tailpos);
      }
      else if (sym == pic->rBEGIN) {
        return analyze_begin(state, obj, tailpos);
      }
      else if (sym == pic->rSETBANG) {
        return analyze_set(state, obj);
      }
      else if (sym == pic->rQUOTE) {
        return analyze_quote(state, obj);
      }
      else if (sym == state->rCONS) {
	ARGC_ASSERT(2);
        return CONSTRUCT_OP2(pic->sCONS);
      }
      else if (sym == state->rCAR) {
	ARGC_ASSERT(1);
        return CONSTRUCT_OP1(pic->sCAR);
      }
      else if (sym == state->rCDR) {
	ARGC_ASSERT(1);
        return CONSTRUCT_OP1(pic->sCDR);
      }
      else if (sym == state->rNILP) {
	ARGC_ASSERT(1);
        return CONSTRUCT_OP1(pic->sNILP);
      }
      else if (sym == state->rSYMBOLP) {
        ARGC_ASSERT(1);
        return CONSTRUCT_OP1(pic->sSYMBOLP);
      }
      else if (sym == state->rPAIRP) {
        ARGC_ASSERT(1);
        return CONSTRUCT_OP1(pic->sPAIRP);
      }
      else if (sym == state->rADD) {
        return analyze_add(state, obj, tailpos);
      }
      else if (sym == state->rSUB) {
        return analyze_sub(state, obj);
      }
      else if (sym == state->rMUL) {
        return analyze_mul(state, obj, tailpos);
      }
      else if (sym == state->rDIV) {
        return analyze_div(state, obj);
      }
      else if (sym == state->rEQ) {
	ARGC_ASSERT_WITH_FALLBACK(2);
        return CONSTRUCT_OP2(pic->sEQ);
      }
      else if (sym == state->rLT) {
	ARGC_ASSERT_WITH_FALLBACK(2);
        return CONSTRUCT_OP2(pic->sLT);
      }
      else if (sym == state->rLE) {
	ARGC_ASSERT_WITH_FALLBACK(2);
        return CONSTRUCT_OP2(pic->sLE);
      }
      else if (sym == state->rGT) {
	ARGC_ASSERT_WITH_FALLBACK(2);
        return CONSTRUCT_OP2(pic->sGT);
      }
      else if (sym == state->rGE) {
	ARGC_ASSERT_WITH_FALLBACK(2);
        return CONSTRUCT_OP2(pic->sGE);
      }
      else if (sym == state->rNOT) {
        ARGC_ASSERT(1);
        return CONSTRUCT_OP1(pic->sNOT);
      }
      else if (sym == state->rVALUES) {
        return analyze_values(state, obj, tailpos);
      }
      else if (sym == state->rCALL_WITH_VALUES) {
        return analyze_call_with_values(state, obj, tailpos);
      }
    }
    fallback:

    return analyze_call(state, obj, tailpos);
  }
  default:
    return pic_list2(pic, pic_obj_value(pic->sQUOTE), obj);
  }
}

pic_value
pic_analyze(pic_state *pic, pic_value obj)
{
  analyze_state *state;

  state = new_analyze_state(pic);

  obj = analyze(state, obj, true);

  analyze_deferred(state);

  destroy_analyze_state(state);
  return obj;
}

/**
 * scope object
 */

typedef struct codegen_context {
  pic_sym *name;
  /* rest args variable is counted as a local */
  bool varg;
  xvect args, locals, captures;
  /* actual bit code sequence */
  pic_code *code;
  size_t clen, ccapa;
  /* child ireps */
  struct pic_irep **irep;
  size_t ilen, icapa;
  /* constant object pool */
  pic_value *pool;
  size_t plen, pcapa;
  /* symbol pool */
  pic_sym **syms;
  size_t slen, scapa;

  struct codegen_context *up;
} codegen_context;

/**
 * global codegen state
 */

typedef struct codegen_state {
  pic_state *pic;
  codegen_context *cxt;
} codegen_state;

static void push_codegen_context(codegen_state *, pic_value, pic_value, pic_value, bool, pic_value);
static struct pic_irep *pop_codegen_context(codegen_state *);

static codegen_state *
new_codegen_state(pic_state *pic)
{
  codegen_state *state;

  state = pic_alloc(pic, sizeof(codegen_state));
  state->pic = pic;
  state->cxt = NULL;

  push_codegen_context(state, pic_false_value(), pic_nil_value(), pic_nil_value(), false, pic_nil_value());

  return state;
}

static struct pic_irep *
destroy_codegen_state(codegen_state *state)
{
  pic_state *pic = state->pic;
  struct pic_irep *irep;

  irep = pop_codegen_context(state);
  pic_free(pic, state);

  return irep;
}

static void
emit_n(codegen_state *state, enum pic_opcode insn)
{
  pic_state *pic = state->pic;
  codegen_context *cxt = state->cxt;

  if (cxt->clen >= cxt->ccapa) {
    cxt->ccapa *= 2;
    cxt->code = pic_realloc(pic, cxt->code, sizeof(pic_code) * cxt->ccapa);
  }
  cxt->code[cxt->clen].insn = insn;
  cxt->clen++;
}

static void
emit_i(codegen_state *state, enum pic_opcode insn, int i)
{
  pic_state *pic = state->pic;
  codegen_context *cxt = state->cxt;

  if (cxt->clen >= cxt->ccapa) {
    cxt->ccapa *= 2;
    cxt->code = pic_realloc(pic, cxt->code, sizeof(pic_code) * cxt->ccapa);
  }
  cxt->code[cxt->clen].insn = insn;
  cxt->code[cxt->clen].u.i = i;
  cxt->clen++;
}

static void
emit_c(codegen_state *state, enum pic_opcode insn, char c)
{
  pic_state *pic = state->pic;
  codegen_context *cxt = state->cxt;

  if (cxt->clen >= cxt->ccapa) {
    cxt->ccapa *= 2;
    cxt->code = pic_realloc(pic, cxt->code, sizeof(pic_code) * cxt->ccapa);
  }
  cxt->code[cxt->clen].insn = insn;
  cxt->code[cxt->clen].u.c = c;
  cxt->clen++;
}

static void
emit_r(codegen_state *state, enum pic_opcode insn, int d, int i)
{
  pic_state *pic = state->pic;
  codegen_context *cxt = state->cxt;

  if (cxt->clen >= cxt->ccapa) {
    cxt->ccapa *= 2;
    cxt->code = pic_realloc(pic, cxt->code, sizeof(pic_code) * cxt->ccapa);
  }
  cxt->code[cxt->clen].insn = insn;
  cxt->code[cxt->clen].u.r.depth = d;
  cxt->code[cxt->clen].u.r.idx = i;
  cxt->clen++;
}

static void
create_activation(codegen_state *state)
{
  pic_state *pic = state->pic;
  codegen_context *cxt = state->cxt;
  size_t i, n;
  xhash regs;
  size_t offset;

  xh_init_ptr(&regs, sizeof(size_t));

  offset = 1;
  for (i = 0; i < xv_size(cxt->args); ++i) {
    n = i + offset;
    xh_put_ptr(&regs, xv_A(cxt->args, i), &n);
  }
  offset += i;
  for (i = 0; i < xv_size(cxt->locals); ++i) {
    n = i + offset;
    xh_put_ptr(&regs, xv_A(cxt->locals, i), &n);
  }

  for (i = 0; i < xv_size(cxt->captures); ++i) {
    n = xh_val(xh_get_ptr(&regs, xv_A(cxt->captures, i)), size_t);
    if (n <= xv_size(cxt->args) || (cxt->varg && n == xv_size(cxt->args) + 1)) {
      /* copy arguments to capture variable area */
      emit_i(state, OP_LREF, (int)n);
    } else {
      /* otherwise, just extend the stack */
      emit_n(state, OP_PUSHNONE);
    }
  }

  xh_destroy(&regs);
}

static void
push_codegen_context(codegen_state *state, pic_value name, pic_value args, pic_value locals, bool varg, pic_value captures)
{
  pic_state *pic = state->pic;
  codegen_context *cxt;
  pic_value var, it;

  assert(pic_sym_p(name) || pic_false_p(name));

  cxt = pic_alloc(pic, sizeof(codegen_context));
  cxt->up = state->cxt;
  cxt->name = pic_false_p(name)
    ? pic_intern_cstr(pic, "(anonymous lambda)")
    : pic_sym_ptr(name);
  cxt->varg = varg;

  xv_init(cxt->args);
  xv_init(cxt->locals);
  xv_init(cxt->captures);

  pic_for_each (var, args, it) {
    xv_push_sym(cxt->args, pic_sym_ptr(var));
  }
  pic_for_each (var, locals, it) {
    xv_push_sym(cxt->locals, pic_sym_ptr(var));
  }
  pic_for_each (var, captures, it) {
    xv_push_sym(cxt->captures, pic_sym_ptr(var));
  }

  cxt->code = pic_calloc(pic, PIC_ISEQ_SIZE, sizeof(pic_code));
  cxt->clen = 0;
  cxt->ccapa = PIC_ISEQ_SIZE;

  cxt->irep = pic_calloc(pic, PIC_IREP_SIZE, sizeof(struct pic_irep *));
  cxt->ilen = 0;
  cxt->icapa = PIC_IREP_SIZE;

  cxt->pool = pic_calloc(pic, PIC_POOL_SIZE, sizeof(pic_value));
  cxt->plen = 0;
  cxt->pcapa = PIC_POOL_SIZE;

  cxt->syms = pic_calloc(pic, PIC_SYMS_SIZE, sizeof(pic_sym *));
  cxt->slen = 0;
  cxt->scapa = PIC_SYMS_SIZE;

  state->cxt = cxt;

  create_activation(state);
}

static struct pic_irep *
pop_codegen_context(codegen_state *state)
{
  pic_state *pic = state->pic;
  codegen_context *cxt = state->cxt;
  struct pic_irep *irep;

  /* create irep */
  irep = (struct pic_irep *)pic_obj_alloc(pic, sizeof(struct pic_irep), PIC_TT_IREP);
  irep->name = state->cxt->name;
  irep->varg = state->cxt->varg;
  irep->argc = (int)xv_size(state->cxt->args) + 1;
  irep->localc = (int)xv_size(state->cxt->locals);
  irep->capturec = (int)xv_size(state->cxt->captures);
  irep->code = pic_realloc(pic, state->cxt->code, sizeof(pic_code) * state->cxt->clen);
  irep->clen = state->cxt->clen;
  irep->irep = pic_realloc(pic, state->cxt->irep, sizeof(struct pic_irep *) * state->cxt->ilen);
  irep->ilen = state->cxt->ilen;
  irep->pool = pic_realloc(pic, state->cxt->pool, sizeof(pic_value) * state->cxt->plen);
  irep->plen = state->cxt->plen;
  irep->syms = pic_realloc(pic, state->cxt->syms, sizeof(pic_sym *) * state->cxt->slen);
  irep->slen = state->cxt->slen;

  /* finalize */
  xv_destroy(cxt->args);
  xv_destroy(cxt->locals);
  xv_destroy(cxt->captures);

  /* destroy context */
  cxt = cxt->up;
  pic_free(pic, state->cxt);
  state->cxt = cxt;

  return irep;
}

static int
index_capture(codegen_state *state, pic_sym *sym, int depth)
{
  codegen_context *cxt = state->cxt;
  size_t i;

  while (depth-- > 0) {
    cxt = cxt->up;
  }

  for (i = 0; i < xv_size(cxt->captures); ++i) {
    if (xv_A(cxt->captures, i) == sym)
      return (int)i;
  }
  return -1;
}

static int
index_local(codegen_state *state, pic_sym *sym)
{
  codegen_context *cxt = state->cxt;
  size_t i, offset;

  offset = 1;
  for (i = 0; i < xv_size(cxt->args); ++i) {
    if (xv_A(cxt->args, i) == sym)
      return (int)(i + offset);
  }
  offset += i;
  for (i = 0; i < xv_size(cxt->locals); ++i) {
    if (xv_A(cxt->locals, i) == sym)
      return (int)(i + offset);
  }
  return -1;
}

static int
index_symbol(codegen_state *state, pic_sym *sym)
{
  pic_state *pic = state->pic;
  codegen_context *cxt = state->cxt;
  size_t i;

  for (i = 0; i < cxt->slen; ++i) {
    if (cxt->syms[i] == sym) {
      return i;
    }
  }
  if (cxt->slen >= cxt->scapa) {
    cxt->scapa *= 2;
    cxt->syms = pic_realloc(pic, cxt->syms, sizeof(pic_sym *) * cxt->scapa);
  }
  cxt->syms[cxt->slen++] = sym;
  return i;
}

static struct pic_irep *codegen_lambda(codegen_state *, pic_value);

static void
codegen(codegen_state *state, pic_value obj)
{
  pic_state *pic = state->pic;
  codegen_context *cxt = state->cxt;
  pic_sym *sym;

  sym = pic_sym_ptr(pic_car(pic, obj));
  if (sym == pic->sGREF) {
    emit_i(state, OP_GREF, index_symbol(state, pic_sym_ptr(pic_list_ref(pic, obj, 1))));
    return;
  } else if (sym == pic->sCREF) {
    pic_sym *name;
    int depth;

    depth = pic_int(pic_list_ref(pic, obj, 1));
    name  = pic_sym_ptr(pic_list_ref(pic, obj, 2));
    emit_r(state, OP_CREF, depth, index_capture(state, name, depth));
    return;
  } else if (sym == pic->sLREF) {
    pic_sym *name;
    int i;

    name = pic_sym_ptr(pic_list_ref(pic, obj, 1));
    if ((i = index_capture(state, name, 0)) != -1) {
      emit_i(state, OP_LREF, i + (int)xv_size(cxt->args) + (int)xv_size(cxt->locals) + 1);
      return;
    }
    emit_i(state, OP_LREF, index_local(state, name));
    return;
  } else if (sym == pic->sSETBANG) {
    pic_value var, val;
    pic_sym *type;

    val = pic_list_ref(pic, obj, 2);
    codegen(state, val);

    var = pic_list_ref(pic, obj, 1);
    type = pic_sym_ptr(pic_list_ref(pic, var, 0));
    if (type == pic->sGREF) {
      emit_i(state, OP_GSET, index_symbol(state, pic_sym_ptr(pic_list_ref(pic, var, 1))));
      emit_n(state, OP_PUSHNONE);
      return;
    }
    else if (type == pic->sCREF) {
      pic_sym *name;
      int depth;

      depth = pic_int(pic_list_ref(pic, var, 1));
      name  = pic_sym_ptr(pic_list_ref(pic, var, 2));
      emit_r(state, OP_CSET, depth, index_capture(state, name, depth));
      emit_n(state, OP_PUSHNONE);
      return;
    }
    else if (type == pic->sLREF) {
      pic_sym *name;
      int i;

      name = pic_sym_ptr(pic_list_ref(pic, var, 1));
      if ((i = index_capture(state, name, 0)) != -1) {
        emit_i(state, OP_LSET, i + (int)xv_size(cxt->args) + (int)xv_size(cxt->locals) + 1);
        emit_n(state, OP_PUSHNONE);
        return;
      }
      emit_i(state, OP_LSET, index_local(state, name));
      emit_n(state, OP_PUSHNONE);
      return;
    }
  }
  else if (sym == pic->sLAMBDA) {
    int k;

    if (cxt->ilen >= cxt->icapa) {
      cxt->icapa *= 2;
      cxt->irep = pic_realloc(pic, cxt->irep, sizeof(struct pic_irep *) * cxt->icapa);
    }
    k = (int)cxt->ilen++;
    emit_i(state, OP_LAMBDA, k);

    cxt->irep[k] = codegen_lambda(state, obj);
    return;
  }
  else if (sym == pic->sIF) {
    int s, t;

    codegen(state, pic_list_ref(pic, obj, 1));

    s = (int)cxt->clen;

    emit_n(state, OP_JMPIF);

    /* if false branch */
    codegen(state, pic_list_ref(pic, obj, 3));

    t = (int)cxt->clen;

    emit_n(state, OP_JMP);

    cxt->code[s].u.i = (int)cxt->clen - s;

    /* if true branch */
    codegen(state, pic_list_ref(pic, obj, 2));
    cxt->code[t].u.i = (int)cxt->clen - t;
    return;
  }
  else if (sym == pic->sBEGIN) {
    pic_value elt, it;
    int i = 0;

    pic_for_each (elt, pic_cdr(pic, obj), it) {
      if (i++ != 0) {
        emit_n(state, OP_POP);
      }
      codegen(state, elt);
    }
    return;
  }
  else if (sym == pic->sQUOTE) {
    int pidx;

    obj = pic_list_ref(pic, obj, 1);
    switch (pic_type(obj)) {
    case PIC_TT_BOOL:
      emit_n(state, (pic_true_p(obj) ? OP_PUSHTRUE : OP_PUSHFALSE));
      return;
    case PIC_TT_INT:
      emit_i(state, OP_PUSHINT, pic_int(obj));
      return;
    case PIC_TT_NIL:
      emit_n(state, OP_PUSHNIL);
      return;
    case PIC_TT_CHAR:
      emit_c(state, OP_PUSHCHAR, pic_char(obj));
      return;
    default:
      if (cxt->plen >= cxt->pcapa) {
        cxt->pcapa *= 2;
        cxt->pool = pic_realloc(pic, cxt->pool, sizeof(pic_value) * cxt->pcapa);
      }
      pidx = (int)cxt->plen++;
      cxt->pool[pidx] = obj;
      emit_i(state, OP_PUSHCONST, pidx);
      return;
    }
  }
  else if (sym == pic->sCONS) {
    codegen(state, pic_list_ref(pic, obj, 1));
    codegen(state, pic_list_ref(pic, obj, 2));
    emit_n(state, OP_CONS);
    return;
  }
  else if (sym == pic->sCAR) {
    codegen(state, pic_list_ref(pic, obj, 1));
    emit_n(state, OP_CAR);
    return;
  }
  else if (sym == pic->sCDR) {
    codegen(state, pic_list_ref(pic, obj, 1));
    emit_n(state, OP_CDR);
    return;
  }
  else if (sym == pic->sNILP) {
    codegen(state, pic_list_ref(pic, obj, 1));
    emit_n(state, OP_NILP);
    return;
  }
  else if (sym == pic->sSYMBOLP) {
    codegen(state, pic_list_ref(pic, obj, 1));
    emit_n(state, OP_SYMBOLP);
    return;
  }
  else if (sym == pic->sPAIRP) {
    codegen(state, pic_list_ref(pic, obj, 1));
    emit_n(state, OP_PAIRP);
    return;
  }
  else if (sym == pic->sADD) {
    codegen(state, pic_list_ref(pic, obj, 1));
    codegen(state, pic_list_ref(pic, obj, 2));
    emit_n(state, OP_ADD);
    return;
  }
  else if (sym == pic->sSUB) {
    codegen(state, pic_list_ref(pic, obj, 1));
    codegen(state, pic_list_ref(pic, obj, 2));
    emit_n(state, OP_SUB);
    return;
  }
  else if (sym == pic->sMUL) {
    codegen(state, pic_list_ref(pic, obj, 1));
    codegen(state, pic_list_ref(pic, obj, 2));
    emit_n(state, OP_MUL);
    return;
  }
  else if (sym == pic->sDIV) {
    codegen(state, pic_list_ref(pic, obj, 1));
    codegen(state, pic_list_ref(pic, obj, 2));
    emit_n(state, OP_DIV);
    return;
  }
  else if (sym == pic->sMINUS) {
    codegen(state, pic_list_ref(pic, obj, 1));
    emit_n(state, OP_MINUS);
    return;
  }
  else if (sym == pic->sEQ) {
    codegen(state, pic_list_ref(pic, obj, 1));
    codegen(state, pic_list_ref(pic, obj, 2));
    emit_n(state, OP_EQ);
    return;
  }
  else if (sym == pic->sLT) {
    codegen(state, pic_list_ref(pic, obj, 1));
    codegen(state, pic_list_ref(pic, obj, 2));
    emit_n(state, OP_LT);
    return;
  }
  else if (sym == pic->sLE) {
    codegen(state, pic_list_ref(pic, obj, 1));
    codegen(state, pic_list_ref(pic, obj, 2));
    emit_n(state, OP_LE);
    return;
  }
  else if (sym == pic->sGT) {
    codegen(state, pic_list_ref(pic, obj, 2));
    codegen(state, pic_list_ref(pic, obj, 1));
    emit_n(state, OP_LT);
    return;
  }
  else if (sym == pic->sGE) {
    codegen(state, pic_list_ref(pic, obj, 2));
    codegen(state, pic_list_ref(pic, obj, 1));
    emit_n(state, OP_LE);
    return;
  }
  else if (sym == pic->sNOT) {
    codegen(state, pic_list_ref(pic, obj, 1));
    emit_n(state, OP_NOT);
    return;
  }
  else if (sym == pic->sCALL || sym == pic->sTAILCALL) {
    int len = (int)pic_length(pic, obj);
    pic_value elt, it;

    pic_for_each (elt, pic_cdr(pic, obj), it) {
      codegen(state, elt);
    }
    emit_i(state, (sym == pic->sCALL ? OP_CALL : OP_TAILCALL), len - 1);
    return;
  }
  else if (sym == pic->sCALL_WITH_VALUES || sym == pic->sTAILCALL_WITH_VALUES) {
    /* stack consumer at first */
    codegen(state, pic_list_ref(pic, obj, 2));
    codegen(state, pic_list_ref(pic, obj, 1));
    /* call producer */
    emit_i(state, OP_CALL, 1);
    /* call consumer */
    emit_i(state, (sym == pic->sCALL_WITH_VALUES ? OP_CALL : OP_TAILCALL), -1);
    return;
  }
  else if (sym == pic->sRETURN) {
    int len = (int)pic_length(pic, obj);
    pic_value elt, it;

    pic_for_each (elt, pic_cdr(pic, obj), it) {
      codegen(state, elt);
    }
    emit_i(state, OP_RET, len - 1);
    return;
  }
  pic_errorf(pic, "codegen: unknown AST type ~s", obj);
}

static struct pic_irep *
codegen_lambda(codegen_state *state, pic_value obj)
{
  pic_state *pic = state->pic;
  pic_value name, args, locals, closes, body;
  bool varg;

  name = pic_list_ref(pic, obj, 1);
  args = pic_list_ref(pic, obj, 2);
  locals = pic_list_ref(pic, obj, 3);
  varg = pic_true_p(pic_list_ref(pic, obj, 4));
  closes = pic_list_ref(pic, obj, 5);
  body = pic_list_ref(pic, obj, 6);

  /* inner environment */
  push_codegen_context(state, name, args, locals, varg, closes);
  {
    /* body */
    codegen(state, body);
  }
  return pop_codegen_context(state);
}

struct pic_irep *
pic_codegen(pic_state *pic, pic_value obj)
{
  codegen_state *state;

  state = new_codegen_state(pic);

  codegen(state, obj);

  return destroy_codegen_state(state);
}

struct pic_proc *
pic_compile(pic_state *pic, pic_value obj, struct pic_lib *lib)
{
  struct pic_irep *irep;
  size_t ai = pic_gc_arena_preserve(pic);

#if DEBUG
  fprintf(stdout, "ai = %zu\n", pic_gc_arena_preserve(pic));

  fprintf(stdout, "# input expression\n");
  pic_debug(pic, obj);
  fprintf(stdout, "\n");

  fprintf(stdout, "ai = %zu\n", pic_gc_arena_preserve(pic));
#endif

  /* macroexpand */
  obj = pic_macroexpand(pic, obj, lib);
#if DEBUG
  fprintf(stdout, "## macroexpand completed\n");
  pic_debug(pic, obj);
  fprintf(stdout, "\n");
  fprintf(stdout, "ai = %zu\n", pic_gc_arena_preserve(pic));
#endif

  /* analyze */
  obj = pic_analyze(pic, obj);
#if DEBUG
  fprintf(stdout, "## analyzer completed\n");
  pic_debug(pic, obj);
  fprintf(stdout, "\n");
  fprintf(stdout, "ai = %zu\n", pic_gc_arena_preserve(pic));
#endif

  /* codegen */
  irep = pic_codegen(pic, obj);
#if DEBUG
  fprintf(stdout, "## codegen completed\n");
  pic_dump_irep(irep);
#endif

#if DEBUG
  fprintf(stdout, "# compilation finished\n");
  puts("");
#endif

  pic_gc_arena_restore(pic, ai);
  pic_gc_protect(pic, pic_obj_value(irep));

  return pic_make_proc_irep(pic, irep, NULL);
}
