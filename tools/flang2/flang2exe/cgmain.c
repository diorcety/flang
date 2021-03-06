/*
 * Copyright (c) 2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

/**
   \file
   \brief Main source module to translate into LLVM
 */

#include "gbldefs.h"
#include "error.h"
#include "global.h"
#include "symtab.h"
#include "ili.h"
#include "machreg.h"
#include "dinit.h"
#include "cg.h"
#include "x86.h"
#include "fih.h"
#include "pd.h"
#include "llutil.h"
#include "ll_structure.h"
#include "lldebug.h"
#include "go.h"
#include <stdlib.h>
#include <stdio.h>
#include "llassem.h"
#include "ll_write.h"
#include "expand.h"
#include "outliner.h"
#include "cgllvm.h"
#if defined(SOCPTRG)
#include "soc.h"
#endif
#include "llvm/Config/llvm-config.h"
#include "ccffinfo.h"

/* clang-format off */

const int max_operands[I_LAST + 1] = {
    1,  1,  -1, -1, /* I_NONE, I_RET, I_BR, I_SW, */
    -1, -1, -1,     /* I_INVOKE, I_UNWIND, I_UNREACH */
    2,  2,  2,  2,  /* I_ADD, I_FADD, I_SUB,  I_FSUB, */
    2,  2,  2,      /* I_MUL, I_FMUL, I_UDIV */
    2,  2,  2,  2,  /* I_SDIV, I_FDIV, I_UREM, I_SREM, */
    2,  2,  2,      /* I_FREM, I_SHL, I_LSHR */
    2,  2,  2,  2,  /* I_ASHR, I_AND, I_OR, I_XOR, */
    2,  3,  3,      /* I_EXTELE, I_INSELE, I_SHUFFVEC */
    -1, -1, -1, -1, /* I_EXTRACTVAL, I_INSERTVAL, I_MALLOC, I_FREE, */
    -1, 1,  2,      /* I_ALLOCA, I_LOAD, I_STORE */
    -1, 1,  1,  1,  /* I_GEP, I_TRUNC, I_ZEXT, I_SEXT, */
    1,  1,  1,      /* I_FPTRUNC, I_FPEXT, I_FPTOUI */
    1,  1,  1,  1,  /* I_FPTOSI, I_UITOFP, I_SITOFP, I_PTRTOINT, */
    1,  1,  3,      /* I_INTTOPTR, I_BITCAST, I_ICMP */
    3,  3,  3,  -1, /* I_FCMP, I_VICMP, I_VFCMP, I_PHI, */
    3,  -1, 1,      /* I_SELECT, I_CALL, I_VA_ARG */
    1,  2,  1,  1,  /* I_DECL, I_LANDINGPAD,  I_RESUME, I_CLEANUP, */
    1,  1,  1,      /* I_CATCH, I_BARRIER, I_ATOMICRMW */
    3,  -1, -1, -1, /* I_CMPXCHG, I_PICALL, I_INDBR, I_FILTER */
    -1              /* I_NONE */
};

static const char *const llvm_instr_names[I_LAST] = {
  "none", "ret", "br", "switch", "invoke", "unwind", "unreachable",
  "add nsw",
  "fadd",
  "sub nsw",
  "fsub",
  "mul nsw",
  "fmul", "udiv", "sdiv", "fdiv", "urem", "srem", "frem",
  "shl nsw",
  "lshr", "ashr", "and", "or", "xor", "extractelement", "insertelement",
  "shufflevector", "extractvalue", "insertvalue", "malloc", "free", "alloca",
  "load", "store", "getelementptr", "trunc", "zext", "sext", "fptrunc",
  "fpext", "fptoui", "fptosi", "uitofp", "sitofp", "ptrtoint", "inttoptr",
  "bitcast", "icmp", "fcmp", "vicmp", "vfcmp", "phi", "select", "call",
  "va_arg", "=", "landingpad", "resume", "cleanup", "catch", "fence",
  "atomicrmw", "cmpxchg", "fence", "call", "indirectbr", "filter"
};

static const char *const stmt_names[STMT_LAST] = {
    "STMT_NONE", "STMT_RET",  "STMT_EXPR",  "STMT_LABEL", "STMT_BR",
    "STMT_ST",   "STMT_CALL", "STMT_SMOVE", "STMT_SZERO", "STMT_DECL"
};

/* clang-format on */

const int MEM_EXTRA = 500;

static int fn_sig_len = MAXARGLEN;
static char *fn_sig_ptr = NULL;
static void insert_entry_label(int);
static void insert_jump_entry_instr(int);
static void store_return_value_for_entry(OPERAND *, int);

static int openacc_prefix_sptr = 0;
static unsigned addressElementSize;

#define ENTOCL_PREFIX "__pgocl_"

#if DEBUG
#include "mwd.h"
CGDATA cg;
#endif

#define HOMEFORDEBUG(sptr) (XBIT(183, 8) && SCG(sptr) == SC_DUMMY)

#define ENABLE_CSE_OPT ((flg.opt >= 1) && !XBIT(183, 0x20) && !killCSE)
#define ENABLE_BLK_OPT ((flg.opt >= 2) && XBIT(183, 0x400))
#define ENABLE_ENHANCED_CSE_OPT (flg.opt >= 2 && !XBIT(183, 0x200000))

#ifdef TARGET_LLVM_ARM
/* TO DO: to be revisited, for now we assume we always target NEON unit */
#define NEON_ENABLED TEST_FEATURE(FEATURE_NEON)
#endif

/* debug switches:
   -Mq,11,16 dump ili right before ILI -> LLVM translation
   -Mq,12,16 provides dinit info, ilt trace, and some basic preprocessing info
   -Mq,12,32 provides complete flow debug info through the LLVM routines
*/

#if defined(TARGET_LLVM_X8632) || defined(TARGET_LLVM_X8664)

#ifndef TEST_FEATURE
#define TEST_FEATURE(M) 0
#endif

#define HAS_AVX TEST_FEATURE(FEATURE_AVX)
#endif

#define DBGTRACEIN(str) DBGXTRACEIN(DBGBIT(12, 0x20), 1, str)
#define DBGTRACEIN1(str, p1) DBGXTRACEIN1(DBGBIT(12, 0x20), 1, str, p1)
#define DBGTRACEIN2(str, p1, p2) DBGXTRACEIN2(DBGBIT(12, 0x20), 1, str, p1, p2)
#define DBGTRACEIN3(str, p1, p2, p3) \
  DBGXTRACEIN3(DBGBIT(12, 0x20), 1, str, p1, p2, p3)
#define DBGTRACEIN4(str, p1, p2, p3, p4) \
  DBGXTRACEIN4(DBGBIT(12, 0x20), 1, str, p1, p2, p3, p4)
#define DBGTRACEIN7(str, p1, p2, p3, p4, p5, p6, p7) \
  DBGXTRACEIN7(DBGBIT(12, 0x20), 1, str, p1, p2, p3, p4, p5, p6, p7)

#define DBGTRACEOUT(str) DBGXTRACEOUT(DBGBIT(12, 0x20), 1, str)
#define DBGTRACEOUT1(str, p1) DBGXTRACEOUT1(DBGBIT(12, 0x20), 1, str, p1)
#define DBGTRACEOUT2(str, p1, p2) \
  DBGXTRACEOUT2(DBGBIT(12, 0x20), 1, str, p1, p2)
#define DBGTRACEOUT3(str, p1, p2, p3) \
  DBGXTRACEOUT3(DBGBIT(12, 0x20), 1, str, p1, p2, p3)
#define DBGTRACEOUT4(str, p1, p2, p3, p4) \
  DBGXTRACEOUT4(DBGBIT(12, 0x20), 1, str, p1, p2, p3, p4)

#define DBGDUMPLLTYPE(str, llt) DBGXDUMPLLTYPE(DBGBIT(12, 0x20), 1, str, llt)

#define DBGTRACE(str) DBGXTRACE(DBGBIT(12, 0x20), 1, str)
#define DBGTRACE1(str, p1) DBGXTRACE1(DBGBIT(12, 0x20), 1, str, p1)
#define DBGTRACE2(str, p1, p2) DBGXTRACE2(DBGBIT(12, 0x20), 1, str, p1, p2)
#define DBGTRACE3(str, p1, p2, p3) \
  DBGXTRACE3(DBGBIT(12, 0x20), 1, str, p1, p2, p3)
#define DBGTRACE4(str, p1, p2, p3, p4) \
  DBGXTRACE4(DBGBIT(12, 0x20), 1, str, p1, p2, p3, p4)
#define DBGTRACE5(str, p1, p2, p3, p4, p5) \
  DBGXTRACE5(DBGBIT(12, 0x20), 1, str, p1, p2, p3, p4, p5)

/* Exported variables */

char **sptr_array = NULL;

/* This should live in llvm_info, but we need to access this module from other
 * translation units temporarily */
LL_Module *cpu_llvm_module = NULL;

LL_Type **sptr_type_array = NULL;


/* File static variables */

static struct {
  unsigned _new_ebb : 1;
  unsigned _killCSE : 1;
  unsigned _init_once : 1;
  unsigned _cpp_init_once : 1;
  unsigned _ftn_init_once : 1;
  unsigned _float_jmp : 1;
  unsigned _fcmp_negate : 1;
  unsigned _last_stmt_is_branch : 1;
} CGMain;

#define new_ebb (CGMain._new_ebb)
#define killCSE (CGMain._killCSE)
#define init_once (CGMain._init_once)
#define cpp_init_once (CGMain._cpp_init_once)
#define ftn_init_once (CGMain._ftn_init_once)
#define float_jmp (CGMain._float_jmp)
#define fcmp_negate (CGMain._fcmp_negate)
#define last_stmt_is_branch (CGMain._last_stmt_is_branch)

static int fcount = 0;
static int fnegcc[17] = LLCCF_NEG;
static int expr_id;
static int entry_bih = 0;
static int routine_count;
static int addr_func_ptrs;
static STMT_Type curr_stmt_type;
static int *idxstack = NULL;

static struct ret_tag {
  /** If ILI uses a hidden pointer argument to return a struct, this is it. */
  int sret_sptr;
  LOGICAL emit_sret; /**< Should we emit an sret argument in LLVM IR? */
} ret_info;

static struct llvm_tag {
  GBL_LIST *last_global;
  INSTR_LIST *last_instr;
  INSTR_LIST *curr_instr;
  LL_ABI_Info *abi_info;

  /** The LLVM function currently being built. */
  LL_Function *curr_func;

  /** LLVM representation of the current function's return type.
      See comment before analyze_ret_info(). */
  LL_Type *return_ll_type;

  LOGICAL no_debug_info;
  int last_sym_avail;
  int last_dtype_avail;
  DTYPE curr_ret_dtype;
  char *buf;
  int buf_idx;
  int buf_sz;

  /** Map sptr -> OPERAND* for those formal function arguments that are saved
      to a local variable in the prolog by process_formal_arguments(). The
      OPERAND* can be used to access the actual LLVM function argument while
      the normal SNAME(sptr) refers to the local variable created by
      process_formal_arguments(). */
  hashmap_t homed_args;

  /** Map name -> func_type for intrinsics that have already been declared by
      get_intrinsic(). */
  hashmap_t declared_intrinsics;
} llvm_info;

typedef struct temp_buf {
  char *buffer;
  int size;
} TEMP_BUF;
static TEMP_BUF sbuf;

typedef struct char_len {
  int sptr;
  int base_sptr;
  struct char_len *next;
} sclen;
static sclen *c_len;

typedef struct temp_buf_list {
  TEMP_BUF buf;
  struct temp_buf_list *next;
} TEMP_BUF_LIST;

static GBL_LIST *Globals;
static GBL_LIST *recorded_Globals;
static INSTR_LIST *Instructions;
static CSED_ITEM *csedList;

typedef struct TmpsMap {
  unsigned size;
  TMPS **map;
} TmpsMap;
static TmpsMap tempsMap;

/* ---  static prototypes (exported prototypes belong in cgllvm.h) --- */

static void write_verbose_type(LL_Type *);
static void gen_store_instr(int, TMPS *, LL_Type *);
static void fma_rewrite(INSTR_LIST *isns);
static void undo_recip_div(INSTR_LIST *isns);
static char *set_local_sname(int sptr, const char *name);
static int is_special_return_symbol(int sptr);
static LOGICAL cgmain_init_call(int);
static OPERAND *gen_call_llvm_intrinsic(const char *, OPERAND *, LL_Type *,
                                        INSTR_LIST *, int);
static OPERAND *gen_llvm_atomicrmw_instruction(int, int, OPERAND *, DTYPE);
static void gen_llvm_fence_instruction(int ilix);
static const char *get_atomicrmw_opname(LL_InstrListFlags);
static const char *get_atomic_memory_order_name(int);
static void insert_llvm_memcpy(int, int, OPERAND *, OPERAND *, int, int, int);
static void insert_llvm_memset(int, int, OPERAND *, int, int, int, int);
static int get_call_sptr(int);
static LL_Type *make_function_type_from_args(LL_Type *return_type,
                                             OPERAND *first_arg_op,
                                             LOGICAL is_varargs);
static int match_prototypes(LL_Type *, LL_Type *);
static MATCH_Kind match_types(LL_Type *, LL_Type *);
static int decimal_value_from_oct(int, int, int);
static char *match_names(int);
static char *vect_llvm_intrinsic_name(int);
static char *vect_power_intrinsic_name(int);
static void build_unused_global_define_from_params(void);
static void print_function_signature(int func_sptr, const char *fn_name,
                                     LL_ABI_Info *abi, LOGICAL print_arg_names);
static void write_global_and_static_defines(void);
static void finish_routine(void);
static char *gen_constant(int, int, INT, INT, int);
static char *process_string(char *, int, int);
static void make_stmt(STMT_Type, int, LOGICAL, int, int ilt);
static INSTR_LIST *make_instr(LL_InstrName);
static INSTR_LIST *gen_instr(LL_InstrName, TMPS *, LL_Type *, OPERAND *);
static OPERAND *ad_csed_instr(LL_InstrName, int, LL_Type *, OPERAND *,
                              LL_InstrListFlags, bool);
static void ad_instr(int, INSTR_LIST *);
static OPERAND *gen_call_expr(int, int, INSTR_LIST *, int);
static INSTR_LIST *gen_switch(int ilix);
static OPERAND *gen_unary_expr(int, int);
static OPERAND *gen_binary_vexpr(int, int, int, int);
static OPERAND *gen_binary_expr(int, int);
static OPERAND *gen_va_start(int);
static OPERAND *gen_va_arg(int);
static OPERAND *gen_va_end(int);
static OPERAND *gen_gep_index(OPERAND *, LL_Type *, int);
static OPERAND *gen_insert_value(OPERAND *aggr, OPERAND *elem, unsigned index);
static char *gen_vconstant(const char *, int, int, int);
static LL_Type *make_vtype(int, int);
static LL_Type *make_type_from_msz(MSZ);
static LL_Type *make_type_from_opc(ILI_OP);
static int add_to_cselist(int ilix);
static void clear_csed_list(void);
static void remove_from_csed_list(int);
static void set_csed_operand(OPERAND **, OPERAND *);
static OPERAND **get_csed_operand(int ilix);
static void build_csed_list(int);
static OPERAND *gen_base_addr_operand(int, LL_Type *);
static OPERAND *gen_comp_operand(OPERAND *, ILI_OP, int, int, int, int, int);
static OPERAND *gen_optext_comp_operand(OPERAND *, ILI_OP, int, int, int, int,
                                        int, int, int);
static OPERAND *gen_sptr(int);
static OPERAND *gen_load(OPERAND *addr, LL_Type *type, unsigned flags);
static void make_store(OPERAND *, OPERAND *, unsigned);
static OPERAND *make_load(int, OPERAND *, LL_Type *, MSZ, unsigned flags);
static OPERAND *convert_sint_to_float(OPERAND *, LL_Type *);
static OPERAND *convert_uint_to_float(OPERAND *, LL_Type *);
static OPERAND *convert_float_to_sint(OPERAND *, LL_Type *);
static OPERAND *convert_float_to_uint(OPERAND *, LL_Type *);
static OPERAND *convert_float_size(OPERAND *, LL_Type *);
static int follow_sptr_hashlk(int);
static int follow_ptr_dtype(int);
static bool same_op(OPERAND *, OPERAND *);
static void remove_dead_instrs(void);
static void write_instructions(LL_Module *);
static int convert_to_llvm_cc(int, LOGICAL);
static OPERAND *get_intrinsic(const char *name, LL_Type *func_type);
static OPERAND *get_intrinsic_call_ops(const char *name, LL_Type *return_type,
                                       OPERAND *args);
static void add_global_define(GBL_LIST *);
static LOGICAL check_global_define(GBL_LIST *);
static void add_external_function_declaration(EXFUNC_LIST *);
static LOGICAL repeats_in_binary(union xx_u);
static bool zerojump(ILI_OP);
static bool exprjump(ILI_OP);
static OPERAND *gen_resized_vect(OPERAND *, int, int);
static bool is_blockaddr_store(int, int, int);
static int process_blockaddr_sptr(int, int);
static LOGICAL is_256_or_512_bit_math_intrinsic(int);
static OPERAND *make_bitcast(OPERAND *, LL_Type *);
static void update_llvm_sym_arrays(void);
static bool need_debug_info(SPTR sptr);
static OPERAND *convert_int_size(int, OPERAND *, LL_Type *);
static OPERAND *convert_int_to_ptr(OPERAND *, LL_Type *);
static OPERAND *gen_call_vminmax_intrinsic(int ilix, OPERAND *op1,
                                           OPERAND *op2);
#if defined(TARGET_LLVM_POWER)
static OPERAND *gen_call_vminmax_power_intrinsic(int ilix, OPERAND *op1,
                                                 OPERAND *op2);
#endif
#if defined(TARGET_LLVM_ARM)
static OPERAND *gen_call_vminmax_neon_intrinsic(int ilix, OPERAND *op1,
                                                OPERAND *op2);
#endif

static void
consTempMap(unsigned size)
{
  if (tempsMap.map) {
    free(tempsMap.map);
  }
  tempsMap.size = size;
  tempsMap.map = calloc(sizeof(struct TmpsMap), size);
}

static void
gcTempMap(void)
{
  free(tempsMap.map);
  tempsMap.size = 0;
  tempsMap.map = NULL;
}

static TMPS *
getTempMap(unsigned ilix)
{
  return (ilix < tempsMap.size) ? tempsMap.map[ilix] : NULL;
}

static void
setTempMap(unsigned ilix, OPERAND *op)
{
  if (ilix < tempsMap.size) {
    tempsMap.map[ilix] = op->tmps;
  }
}

void
set_llvm_sptr_name(OPERAND *operand)
{
  int sptr = operand->val.sptr;
  operand->string = SNAME(sptr);
}

char *
get_label_name(int sptr)
{
  char *nm = SNAME(sptr);
  if (*nm == '@')
    nm++;
  return nm;
}

char *
get_llvm_sname(int sptr)
{
  char *p = SNAME(sptr);
  if (p == NULL) {
    process_sptr(sptr);
    p = SNAME(sptr);
  }
  if (p == NULL) {
    p = SYMNAME(sptr);
    if (p == NULL)
      return "";
    SNAME(sptr) = (char *)getitem(LLVM_LONGTERM_AREA, strlen(p) + 1);
    p = strcpy(SNAME(sptr), p);
    return p;
  }
  if (*p == '@')
    p++;
  return p;
}

char *
get_llvm_mips_sname(int sptr)
{
  return get_llvm_sname(sptr);
}

int
cg_get_type(int n, int v1, int v2)
{
  int ret_dtype;
  ret_dtype = get_type(n, v1, v2);
  update_llvm_sym_arrays();
  return ret_dtype;
}

int
is_struct_union(int dtype)
{
  switch (DTY(dtype)) {
  case TY_STRUCT:
  case TY_UNION:
    return TRUE;
  }
  return FALSE;
}

INSTR_LIST *
llvm_info_last_instr(void)
{
  return llvm_info.last_instr;
}

/*
 * Return value handling.
 *
 * Functions that return a struct or other aggregrate that doesn't fit in
 * registers may require the caller to pass in a return value pointer as a
 * hidden first argument. The callee wil store the returned struct to the
 * pointer.
 *
 * In LLVM IR, this is represented by an sret attribute on the hidden pointer
 * argument:
 *
 *   %struct.S = type { [10 x i32] }
 *
 *   define void @f(%struct.S* noalias sret %agg.result) ...
 *
 * Some structs can be returned in registers, depending on ABI-specific rules.
 * For example, x86-64 can return a struct {long x, y; } struct in registers
 * %rax and %rdx:
 *
 *   define { i64, i64 } @f() ...
 *
 * When targeting LLVM, ILI for a function returning a struct looks like the
 * caller passed in an sret pointer, no matter how the ABI specifies the struct
 * should be returned. This simplifies the ILI, and we will translate here if
 * the struct can actually be returned in registers for the current ABI.
 */

/*
 * Analyze the return value of the current function and determine how it should
 * be translated to LLVM IR.
 *
 * If the LLVM IR representation uses an sret argument, set:
 *
 *   ret_info.emit_sret = TRUE.
 *   ret_info.sret_sptr = symbol table entry for sret argument.
 *   llvm_info.return_ll_type = void.
 *
 * If the ILI representation uses a hidden struct argument, but the LLVM IR
 * returns in registers, set:
 *
 *   ret_info.emit_sret = FALSE.
 *   ret_info.sret_sptr = symbol table entry for sret argument.
 *   llvm_info.return_ll_type = LLVM function return type.
 *
 * Otherwise when both ILI and LLVM IR return in a register, set:
 *
 *   ret_info.emit_sret = FALSE.
 *   ret_info.sret_sptr = 0.
 *   llvm_info.return_ll_type = LLVM function return type.
 */
static void
analyze_ret_info(int func_sptr)
{
  int return_dtype;

/* Get the symbol table entry for the function's return value. If ILI is
 * using a hidden sret argument, this will be it.
 *
 * Fortran complex return values are handled differently, and don't get an
 * 'sret' attribute.
 */
#if defined(ENTRYG)
  ret_info.sret_sptr = aux.entry_base[ENTRYG(func_sptr)].ret_var;
#endif

  if (gbl.arets) {
    return_dtype = DT_INT;
  } else {
    /* get return type from ag_table or ll_abi table */
    return_dtype = get_return_type(func_sptr);
    /*
     * do not set the sret_sptr for 'bind(c)' complex functions in the
     * presence of multiple entries
     */
    if (!has_multiple_entries(gbl.currsub) && DT_ISCMPLX(return_dtype) &&
        (CFUNCG(func_sptr) || CMPLXFUNC_C)) {
      ret_info.sret_sptr = FVALG(func_sptr);
    }
  }

  DBGTRACE2("sret_sptr=%d, return_dtype=%d", ret_info.sret_sptr, return_dtype);

  llvm_info.return_ll_type = make_lltype_from_dtype(return_dtype);

  ret_info.emit_sret = LL_ABI_HAS_SRET(llvm_info.abi_info);

  if (ret_info.emit_sret) {
    assert(ret_info.sret_sptr, "ILI should use a ret_var", func_sptr, 4);
    llvm_info.return_ll_type = make_void_lltype();
  } else if (llvm_info.return_ll_type != llvm_info.abi_info->arg[0].type) {
    /* Make sure the return type matches the ABI type. */
    llvm_info.return_ll_type =
        make_lltype_from_abi_arg(&llvm_info.abi_info->arg[0]);
  }

  /* Process sret_sptr *after* setting up ret_info. Some decisions in
   * process_auto_sptr() depends on ret_info. */
  if (ret_info.sret_sptr)
    process_sptr(ret_info.sret_sptr);
}

/**
   \brief Generate a return operand when ILI didn't provide a return value.

   LLVM requires a return instruction, even if it is only a "return undef".
   Also handle the case where we have a special return value symbol but want to
   return a value in registers.
 */
static OPERAND *
gen_return_operand(void)
{
  LL_Type *rtype = llvm_info.return_ll_type;

  if (rtype->data_type == LL_VOID) {
    OPERAND *op = make_operand();
    op->ll_type = rtype;
    return op;
  }

  /* ret_sptr is the return value symbol which we want to return in registers.
   *
   * Coerce it to the correct type by bitcasting the pointer and loading
   * the return value type from the stack slot.
   */
  if (ret_info.sret_sptr) {
    /* Bitcast sret_sptr to a pointer to the return type. */
    LL_Type *prtype = make_ptr_lltype(rtype);
    OPERAND *sret_as_prtype =
        make_bitcast(gen_sptr(ret_info.sret_sptr), prtype);
    /* Load sret_sptr as the return type and return that. */
    return gen_load(sret_as_prtype, rtype,
                    ldst_instr_flags_from_dtype(DTYPEG(ret_info.sret_sptr)));
  }

  /* No return value symbol available. We need to return something, so just
   * return undef.
   */
  return make_undef_op(rtype);
}

void
print_personality(void)
{
  print_token(
      " personality i8* bitcast (i32 (...)* @__gxx_personality_v0 to i8*)");
}

/**
   \brief Clear \c SNAME for \p sptr
   \param sptr  the symbol
   Used by auto parallel in C when the optimizer uses the same compiler
   generated variable across loops
 */
void
llvmResetSname(int sptr)
{
  SNAME(sptr) = NULL;
}

/* --------------------------------------------------------- */

static int
processOutlinedByConcur(int bih)
{
  int eili, bili, bilt, eilt, gtid;
  int bbih, ebih, bopc, eopc;
  int bconcur = 0;
  int display = 0;
  static int workingBih = 0;

  if (workingBih == 0)
    workingBih = BIH_NEXT(workingBih);

  /* does not support nested auto parallel */
  for (bbih = workingBih; bbih; bbih = BIH_NEXT(bbih)) {

    /* if IL_BCONCUR is always be the first - we can just check the first ilt */
    for (bilt = BIH_ILTFIRST(bbih); bilt; bilt = ILT_NEXT(bilt)) {
      bili = ILT_ILIP(bilt);
      bopc = ILI_OPC(bili);

      if (bopc == IL_BCONCUR) {
        ++bconcur;

        GBL_CURRFUNC = ILI_OPND(bili, 1);
        display = llvmAddConcurEntryBlk(bbih);

        /* if IL_ECONCUR is always be the first - we can just check the first
         * ilt */
        for (ebih = bbih; ebih; ebih = BIH_NEXT(ebih)) {
          for (eilt = BIH_ILTFIRST(ebih); eilt; eilt = ILT_NEXT(eilt)) {
            eili = ILT_ILIP(eilt);
            eopc = ILI_OPC(eili);
            if (eopc == IL_ECONCUR) {
              --bconcur;
              llvmAddConcurExitBlk(ebih);
              display = 0;
              workingBih = BIH_NEXT(ebih); /* bih after IL_ECONCUR block */
              BIH_NEXT(ebih) = 0;

              /* Reset SNAME field for gtid which needs to be done for C/C++.
               * gtid can be have SC_LOCAL and ENCLFUNC of the host rotine and
               * the code generator will not process if SNAME already exist.  We
               * want this variable declared in the Mconcur outlined routine.
               */
              gtid = ll_get_gtid();
              if (gtid)
                llvmResetSname(gtid);
              ll_save_gtid_val(0);

#if DEBUG
              if (DBGBIT(10, 4)) {
                dump_blocks(gbl.dbgfil, gbl.entbih,
                            "***** BIHs for Function \"%s\" *****", 0);
              }

#endif
              return ebih;
            } else if (eopc == IL_BCONCUR && bbih != ebih) {
              return 0; /* error happens */
            }
          }
        }
      }
    }
  }
  workingBih = 0; /* no more concur */
  return 0;
}

/*
 * Inspect all variables in the symbol table and change their storage
 * class from SC_LOCAL to SC_STATIC if appropriate.  The CG needs to
 * know the final storage class of variables before it begins code
 * generation.
 */
static void
assign_fortran_storage_classes(void)
{
  int sptr;

  for (sptr = stb.firstusym; sptr < stb.symavl; ++sptr) {
    switch (STYPEG(sptr)) {
    case ST_PLIST:
    case ST_VAR:
    case ST_ARRAY:
    case ST_STRUCT:
    case ST_UNION:
      if (REFG(sptr))
        break;

      if (SCG(sptr) != SC_LOCAL && SCG(sptr) != SC_NONE)
        break;

      if (DINITG(sptr) || SAVEG(sptr)) {
        SCP(sptr, SC_STATIC);
        if ((flg.smp || (XBIT(34, 0x200) || gbl.usekmpc)) && PARREFG(sptr))
          PARREFP(sptr, 0);
      } else if (STYPEG(sptr) != ST_VAR && !flg.recursive &&
                 (!CCSYMG(sptr) || INLNG(sptr))) {
        SCP(sptr, SC_STATIC);
        if ((flg.smp || (XBIT(34, 0x200) || gbl.usekmpc)) && PARREFG(sptr))
          PARREFP(sptr, 0);
      }
      break;
    default:
      break;
    }
  }
} /* end assign_fortran_storage_classes() */

/**
   \brief Perform code translation from ILI to LLVM for one routine
 */
void
schedule(void)
{
  LL_Type *func_type;
  int bihx, ilt, ilix, ilix2, nme;
  ILI_OP opc;
  int rhs_ili, lhs_ili, cc_val, opnd1_ili, opnd2_ili, sptr, sptr_init;
  int bih, bihprev, bihcurr, bihnext, li, i, concurBih;
  LOGICAL made_return;
  LOGICAL merge_next_block;
  int save_currfunc;
  bool processHostConcur = true;
  int func_sptr = gbl.currsub;
  int first = 1;
  concurBih = 0;

  assign_fortran_storage_classes();

restartConcur:
  FTN_HOST_REG() = 1;
  func_sptr = GBL_CURRFUNC;
  entry_bih = gbl.entbih;
  cg_llvm_init();

  consTempMap(ilib.stg_avail);

  store_llvm_localfptr();

  /* inititalize the definition lists per routine */
  csedList = NULL;
  memset(&ret_info, 0, sizeof(ret_info));
  llvm_info.curr_func = NULL;

#if DEBUG
  if (DBGBIT(11, 1))
    dumpblocks("just before LLVM translation");
  if (DBGBIT(11, 0x810) || DBGBIT(12, 0x30)) {
    fprintf(ll_dfile, "--- ROUTINE %s (sptr# %d) ---\n", SYMNAME(func_sptr),
            func_sptr);
  }
  if (DBGBIT(11, 0x10)) {
    bihx = gbl.entbih;
    for (;;) {
      dmpilt(bihx);
      if (BIH_LAST(bihx))
        break;
      bihx = BIH_NEXT(bihx);
    }
    dmpili();
  }

#endif

  /* Start the LLVM translation here */
  llvm_info.last_instr = NULL;
  llvm_info.curr_instr = NULL;
  Instructions = NULL;
  /* Update symbol table before we process any routine arguments, this must be
   * called before ll_abi_for_func_sptr()
   */
  stb_process_routine_parameters();

  hashmap_clear(llvm_info.homed_args);
  llvm_info.abi_info = ll_abi_for_func_sptr(cpu_llvm_module, func_sptr, 0);
  func_type = ll_abi_function_type(llvm_info.abi_info);
  process_sptr(func_sptr);
  llvm_info.curr_func =
      ll_create_function_from_type(func_type, SNAME(func_sptr));

  ad_instr(0, gen_instr(I_NONE, NULL, NULL, make_label_op(0)));

  ll_proto_add_sptr(func_sptr, llvm_info.abi_info);

  if (flg.debug || XBIT(120, 0x1000)) {
    if (!CCSYMG(func_sptr) || BIH_FINDEX(gbl.entbih)) {
      const int funcType = get_return_type(func_sptr);
      LL_Value *func_ptr = ll_create_pointer_value_from_type(cpu_llvm_module,
                                                func_type, SNAME(func_sptr), 0);
      lldbg_emit_subprogram(cpu_llvm_module->debug_info, func_sptr, funcType,
                            BIH_FINDEX(gbl.entbih), FALSE);
      lldbg_set_func_ptr(cpu_llvm_module->debug_info, func_ptr);
    }
  }

  /* set the return type of the function */
  analyze_ret_info(func_sptr);

/* Build up the additional items/dummys needed for the master sptr if there
 * are entries, and call process_formal_arguments on that information. */
  if (has_multiple_entries(gbl.currsub) && get_entries_argnum())
    process_formal_arguments(
        process_ll_abi_func_ftn_mod(cpu_llvm_module, get_master_sptr(), 1));
  else
    process_formal_arguments(llvm_info.abi_info);
  made_return = FALSE;

  get_local_overlap_size();
  expr_id = 0;
  last_stmt_is_branch = 0;

  bih = BIH_NEXT(0);
  if ((XBIT(34, 0x200) || gbl.usekmpc) && !processHostConcur)
    bih = gbl.entbih;
  /* construct the body of the function */
  for (; bih; bih = BIH_NEXT(bih))
    for (ilt = BIH_ILTFIRST(bih); ilt; ilt = ILT_NEXT(ilt))
      build_csed_list(ILT_ILIP(ilt));

  merge_next_block = FALSE;
  bih = BIH_NEXT(0);
  if ((XBIT(34, 0x200) || gbl.usekmpc) && !processHostConcur)
    bih = gbl.entbih;
  for (; bih; bih = BIH_NEXT(bih)) {

#if DEBUG
    if (DBGBIT(12, 0x10)) {
      fprintf(ll_dfile, "schedule(): at bih %d\n", bih);
    }
#endif
    DBGTRACE1("Processing bih %d", bih)
    bihcurr = bih;

    /* skip over an entry bih  */
    if (BIH_EN(bih)) {
      if (BIH_ILTFIRST(bih) != BIH_ILTLAST(bih))
        goto do_en_bih;
      else if (has_multiple_entries(gbl.currsub) && DPDSCG(gbl.currsub) == 0)
        goto do_en_bih;
      bihprev = bih;
      continue;
    }
    /* do we have a label that's the target of a branch? Either a
     * user label (via a goto) or a compiler created label for branching.
     */
    else if ((sptr = BIH_LABEL(bih)) && (DEFDG(sptr) || CCSYMG(sptr))) {
      assert(STYPEG(sptr) == ST_LABEL, "schedule(), not ST_LABEL", sptr, 4);
      clear_csed_list();
      make_stmt(STMT_LABEL, sptr, FALSE, 0, 0);
    }

  do_en_bih:

    /* in general, ilts will correspond to statements */
    for (ilt = BIH_ILTFIRST(bih); ilt; ilt = ILT_NEXT(ilt))
      build_csed_list(ILT_ILIP(ilt));

    bihnext = BIH_NEXT(bih);

    if (merge_next_block == FALSE)
      new_ebb = TRUE;

    if (((flg.opt == 1 && BIH_EN(bih)) || (flg.opt >= 2 && !BIH_TAIL(bih))) &&
        bihnext && (!BIH_LABEL(bihnext)) && BIH_PAR(bihnext) == BIH_PAR(bih) &&
        BIH_CS(bihnext) == BIH_CS(bih) && BIH_TASK(bihnext) == BIH_TASK(bih) &&
        !BIH_NOMERGE(bih) && !BIH_NOMERGE(bihnext)) {
      merge_next_block = TRUE;
    } else
      merge_next_block = FALSE;

    for (ilt = BIH_ILTFIRST(bih); ilt; ilt = ILT_NEXT(ilt)) {
      if (BIH_EN(bih) && ilt == BIH_ILTFIRST(bih)) {
        if (!has_multiple_entries(gbl.currsub))
          continue;
        if (first) {
          insert_jump_entry_instr(ilt);
          first = 0;
        }
        insert_entry_label(ilt);
        continue;
      }
#if DEBUG
      if (DBGBIT(12, 0x10)) {
        fprintf(ll_dfile, "\tat ilt %d\n", ilt);
      }
#endif
      if (flg.debug || XBIT(120, 0x1000)) {
        lldbg_emit_line(cpu_llvm_module->debug_info, ILT_LINENO(ilt));
      }
      ilix = ILT_ILIP(ilt);
      opc = ILI_OPC(ilix);

      if (processHostConcur && (XBIT(34, 0x200) || gbl.usekmpc)) {
        if (opc == IL_BCONCUR) {
          ++concurBih;
        } else if (opc == IL_ECONCUR) {
          --concurBih;
        }
        if (concurBih)
          continue;
      }

      if (ILT_BR(ilt)) /* branch */
      {
        int next_bih_label;

        if (!ILT_NEXT(ilt) && bihnext &&
            ((next_bih_label = BIH_LABEL(bihnext)) &&
             (DEFDG(next_bih_label) || CCSYMG(next_bih_label))))
          make_stmt(STMT_BR, ilix, FALSE, next_bih_label, ilt);
        else
          make_stmt(STMT_BR, ilix, FALSE, 0, ilt);
      } else if ((ILT_ST(ilt) || ILT_DELETE(ilt)) &&
                 IL_TYPE(opc) == ILTY_STORE) /* store */
      {
        rhs_ili = ILI_OPND(ilix, 1);
        lhs_ili = ILI_OPND(ilix, 2);
        nme = ILI_OPND(ilix, 3);
        /* can we ignore homing code? Try it here */
        if (is_rgdfili_opcode(ILI_OPC(rhs_ili)))
          continue;
        if (BIH_EN(bih)) { /* homing code */
          int sym = NME_SYM(nme);
          if (sym > 0) {
            if (SCG(sym) == SC_DUMMY)
              continue;
          }
        }
        make_stmt(STMT_ST, ilix, ENABLE_CSE_OPT && ILT_DELETE(ilt) &&
                                     IL_TYPE(opc) == ILTY_STORE,
                  0, ilt);
      } else if (opc == IL_JSR && cgmain_init_call(ILI_OPND(ilix, 1))) {
        make_stmt(STMT_SZERO, ILI_OPND(ilix, 2), FALSE, 0, ilt);
      } else if (opc == IL_SMOVE) {
        make_stmt(STMT_SMOVE, ilix, FALSE, 0, ilt);
      } else if (ILT_EX(ilt)) { /* call */
        if (opc == IL_LABEL)
          continue; /* gen_llvm_expr does not handle IL_LABEL */
        switch (opc) {
        case IL_DFRSP:
        case IL_DFRDP:
        case IL_DFRCS:
          ilix = ILI_OPND(ilix, 1);
          opc = ILI_OPC(ilix);
          break;
        default:;
        }
        if (is_mvili_opcode(opc)) /* call part of the return */
          goto return_with_call;
        else if (is_freeili_opcode(opc)) {
          remove_from_csed_list(ilix);
          make_stmt(STMT_DECL, ilix, FALSE, 0, ilt);
        } else if (opc == IL_JSR || opc == IL_QJSR || /* call not in a return */
                   opc == IL_JSRA
#ifdef SJSR
                   || opc == IL_SJSR || opc == IL_SJSRA
#endif
                   ) {
          make_stmt(STMT_CALL, ilix, FALSE, 0, ilt);
        } else {
          if ((opc != IL_DEALLOC) && (opc != IL_NOP))
            make_stmt(STMT_DECL, ilix, FALSE, 0, ilt);
        }
      } else if (opc == IL_FENCE) {
        gen_llvm_fence_instruction(ilix);
      } else {
        /* may be a return; otherwise mostly ignored */
        /* However, need to keep track of FREE* ili, to match them
         * with CSE uses, since simple dependences need to be checked
         */
      return_with_call:
        if (is_mvili_opcode(opc)) { /* routine return */
          if (ret_info.sret_sptr == 0) {
            ilix2 = ILI_OPND(ilix, 1);
            /* what type of return value */
            switch (IL_TYPE(ILI_OPC(ilix2))) {
            case ILTY_LOAD:
            /*
               ilix2 = ILI_OPND(ilix2,1);
               assert(IL_TYPE(ILI_OPC(ilix2))==ILTY_CONS,
               "schedule(): wrong load type",IL_TYPE(ILI_OPC(ilix2)),4); */
            case ILTY_CONS:
            case ILTY_ARTH:
            case ILTY_DEFINE:
            case ILTY_MOVE:
              make_stmt(STMT_RET, ilix2, FALSE, 0, ilt);
              break;
            case ILTY_OTHER:
              /* handle complex builtin */
              if (XBIT(70, 0x40000000)) {
                if (IL_RES(ILI_OPC(ilix2)) == ILIA_DP ||
                    IL_RES(ILI_OPC(ilix2)) == ILIA_SP) {
                  make_stmt(STMT_RET, ilix2, FALSE, 0, ilt);
                  break;
                }
              }
            default:
              switch (ILI_OPC(ilix2)) {
              case IL_ISELECT:
              case IL_KSELECT:
              case IL_ASELECT:
              case IL_FSELECT:
              case IL_DSELECT:
              case IL_ATOMICRMWI:
              case IL_ATOMICRMWKR:
              case IL_ATOMICRMWA:
              case IL_ATOMICRMWSP:
              case IL_ATOMICRMWDP:
                make_stmt(STMT_RET, ilix2, FALSE, 0, ilt);
                break;
              default:
                assert(0, "schedule(): incompatible return type",
                       IL_TYPE(ILI_OPC(ilix2)), 4);
              }
            }
            made_return = TRUE;
          }
        } else if (is_freeili_opcode(opc)) {
#if DEBUG
          if (DBGBIT(12, 0x10)) {
            fprintf(ll_dfile, "\tfound free ili: %d(%s)\n", ilix, IL_NAME(opc));
          }
#endif
          remove_from_csed_list(ilix);
          make_stmt(STMT_DECL, ilix, FALSE, 0, ilt);
        } else if (opc == IL_LABEL) {
          continue; /* ignore IL_LABEL */
        } else if (BIH_LAST(bih) && !made_return) {
          /* at end, make a NULL return statement if return not already made */
          make_stmt(STMT_RET, ilix, FALSE, 0, ilt);
        } else if (opc == IL_SMOVE) {
          /* moving/storing a block of memory */
          make_stmt(STMT_SMOVE, ilix, FALSE, 0, ilt);        
        }
      }
    }
    bihprev = bih;
  }

  build_unused_global_define_from_params();

/* header already printed; now print global and static defines */
  write_ftn_typedefs();
  write_global_and_static_defines();

/* perform setup for each routine */
  if (has_multiple_entries(func_sptr))
    write_master_entry_routine();
  else
    build_routine_and_parameter_entries(func_sptr, llvm_info.abi_info,
                                        cpu_llvm_module);

  /* write out local variable defines */
  ll_write_local_objects(llvm_file(), llvm_info.curr_func);
  /* Emit alloca for local equivalence, c.f. get_local_overlap_var(). */
  write_local_overlap();

  if (ENABLE_BLK_OPT)
    optimize_block(llvm_info.last_instr);

  /*
   * similar code in llvect.c, cgoptim1.c, and llvm's cgmain.c & llvect.c
   * 01/17/17 -- we are no longer attempting to transform the divide into
   *             a multiply by recip; we are simply depending on the user
   *             adding -Mfprelaxed[=div]
   * 02/10/17 -- enabled with -Mnouniform
   *
   */
  if (XBIT_NOUNIFORM && (!XBIT(183, 0x8000)) && XBIT(15, 4) && (!flg.ieee)) {
    undo_recip_div(Instructions);
  }
  /* try FMA rewrite */
  if (XBIT_GENERATE_SCALAR_FMA /* HAS_FMA and x-flag 164 */
      && (get_llvm_version() >= LL_Version_3_7)) {
    fma_rewrite(Instructions);
  }

  if (ENABLE_CSE_OPT) {
    remove_dead_instrs();
    Instructions->prev = NULL;
    if (XBIT(183, 0x40))
      sched_instructions(Instructions);
  }

  /* print out the instructions */
  write_instructions(cpu_llvm_module);

  finish_routine();
  hashmap_clear(llvm_info.homed_args); /* Don't home entry trampoline parms */
  if (processHostConcur)
    print_entry_subroutine(cpu_llvm_module);
  ll_destroy_function(llvm_info.curr_func);
  llvm_info.curr_func = NULL;

  assem_data();
  assem_end();
  /* we need to set init_once to zero here because for cuda fortran combine with
   * acc - the constructors can be created without one after the other and
   * cg_llvm_end will not get call between those.  If init_once is not reset,
   * cg_llvm_init will not go through.
   */
  init_once = FALSE;

  if (--routine_count > 0)
  {
    /* free CG_MEDTERM_AREA - done on a per-routine basis */
    freearea(CG_MEDTERM_AREA);
  }
  FTN_HOST_REG() = 1;

  if ((XBIT(34, 0x200) || gbl.usekmpc) &&
      (concurBih = processOutlinedByConcur(concurBih))) {
    processHostConcur = false;
    goto restartConcur;
  }
  ll_reset_gtid();
  if (flg.smp || (XBIT(34, 0x200) || gbl.usekmpc))
    ll_reset_gtid();

  if (gbl.outlined && ((flg.inliner && !XBIT(14, 0x10000)) || flg.autoinline)) {
      GBL_CURRFUNC = 0;
  }
  gcTempMap();
} /* schedule */

INLINE static bool
call_sym_is(SPTR sptr, const char *sym_name)
{
  return sptr && (strncmp(SYMNAME(sptr), sym_name, strlen(sym_name)) == 0);
}

static OPERAND *
gen_llvm_instr(int ilix, ILI_OP opc, LL_Type *return_type,
               LL_Type *param_lltype, int itype)
{
  OPERAND *operand;
  OPERAND *param_op;
  INSTR_LIST *Curr_Instr;
  int arg_ili = ILI_OPND(ilix, 2);

  operand = make_tmp_op(return_type, make_tmps());
  Curr_Instr = gen_instr(itype, operand->tmps, operand->ll_type, NULL);
  assert(ILI_OPC(arg_ili) == opc,
         "gen_llvm_instr(): unexpected opc for parameter ", ILI_OPC(arg_ili),
         4);
  param_op = gen_llvm_expr(ILI_OPND(arg_ili, 1), param_lltype);
  Curr_Instr->operands = param_op;
  arg_ili = ILI_OPND(arg_ili, 2);
  while ((arg_ili > 0) && (ILI_OPC(arg_ili) != IL_NULL)) {
    assert(ILI_OPC(arg_ili) == opc,
           "gen_llvm_instr(): unexpected opc for parameter ", ILI_OPC(arg_ili),
           4);
    param_op->next = gen_llvm_expr(ILI_OPND(arg_ili, 1), param_lltype);
    param_op = param_op->next;
    arg_ili = ILI_OPND(arg_ili, 2);
  }
  ad_instr(ilix, Curr_Instr);

  return operand;
}

static OPERAND *
gen_llvm_atomic_intrinsic_for_builtin(int pdnum, int sptr, int ilix,
                                      INSTR_LIST *Call_Instr)
{
  OPERAND *operand;
  int call_sptr = sptr;
  int param_dtype;
  LL_Type *op_type;
  char routine_name[MAXIDLEN];
  int base_dtype;
  int first_arg_ili;
  LOGICAL incdec = FALSE;
  int arg_ili = ILI_OPND(ilix, 2);
  DTYPE call_dtype = DTYPEG(call_sptr);
  DTYPE return_dtype = DTY(call_dtype + 1);
  int params = DTY(call_dtype + 2);

  switch (pdnum) {
  default:
    assert(0, "gen_llvm_atomic_intrinsic_for_builtin(): invalid pdnum value ",
           pdnum, 4);
  }
  base_dtype = return_dtype;
  first_arg_ili = ILI_OPND(arg_ili, 1);
  switch (DTY(base_dtype)) {
  case TY_BINT:
    strcat(routine_name, "i8.p0i8");
    break;
  case TY_USINT:
    strcat(routine_name, "i16.p0i16");
    break;
  case TY_SINT:
    strcat(routine_name, "i16.p0i16");
    break;
  case TY_UINT:
  case TY_INT:
    strcat(routine_name, "i32.p0i32");
    break;
  case TY_INT8:
  case TY_UINT8:
    strcat(routine_name, "i64.p0i64");
    break;
  case TY_REAL:
    return NULL;
  default:
    assert(0, "gen_llvm_atomic_intrinsic_for_builtin(): invalid base type for "
              "call to sptr",
           sptr, 4);
  }
  op_type = make_lltype_from_dtype(cg_get_type(2, TY_PTR, return_dtype));
  operand = gen_llvm_expr(first_arg_ili, op_type);
  op_type = make_lltype_from_dtype(return_dtype);
  if (incdec) {
    operand->next = gen_llvm_expr(ad_icon(1), op_type);
  } else {
    int next_arg_ili = ILI_OPND(arg_ili, 2);
    operand->next = gen_llvm_expr(ILI_OPND(next_arg_ili, 1), op_type);
    next_arg_ili = ILI_OPND(next_arg_ili, 2);
    if (ILI_OPC(next_arg_ili) != IL_NULL) {
      int next = ILI_OPND(next_arg_ili, 1);
      operand->next->next = gen_llvm_expr(next, op_type);
    }
  }

  return gen_llvm_atomicrmw_instruction(ilix, pdnum, operand, return_dtype);
}

static OPERAND *
gen_llvm_intrinsic_for_builtin(int sptr, int arg_ili, INSTR_LIST *Call_Instr)
{
  OPERAND *operand;
  int call_sptr = sptr;
  int call_dtype;
  int return_dtype;
  int params, param_dtype;
  int pd_sym;
  LL_Type *return_type;
  char routine_name[MAXIDLEN];
  int base_dtype;
  int first_arg_ili;
  return operand;
}

static OPERAND *
gen_call_vminmax_intrinsic(int ilix, OPERAND *op1, OPERAND *op2)
{
  int vect_dtype;
  int vect_size;
  int type_size;
  char sign = 'u';
  char type = 'i';
  char *mstr = "maxnum";
  static char buf[MAXIDLEN];

  if (ILI_OPC(ilix) == IL_VMIN)
    mstr = "minnum";
  else
    assert(ILI_OPC(ilix) == IL_VMAX, "gen_call_vminmax_intrinsic(): bad opc",
           ILI_OPC(ilix), 4);
  vect_dtype = ILI_OPND(ilix, 3);
  vect_size = DTY(vect_dtype + 2);
  switch (DTY(DTY(vect_dtype + 1))) {
  case TY_FLOAT:
  case TY_DBLE:
    type = 'f';
  case TY_INT:
    sign = 's';
  case TY_UINT:
    if (vect_size != 2 && vect_size != 4 && vect_size != 8)
      return NULL;
    break;
  case TY_SINT:
    sign = 's';
  case TY_USINT:
    if (vect_size != 4 && vect_size != 8)
      return NULL;
    break;
  case TY_BINT:
    sign = 's';
  default:
    return NULL;
  }
  op1->next = op2;
  type_size = zsize_of(DTY(vect_dtype + 1)) * 8;
  sprintf(buf, "@llvm.%s.v%d%c%d", mstr, vect_size, type, type_size);
  return gen_call_to_builtin(ilix, buf, op1, make_lltype_from_dtype(vect_dtype),
                             NULL, I_PICALL, FALSE);
}

#if defined(TARGET_LLVM_POWER)
static OPERAND *
gen_call_vminmax_power_intrinsic(int ilix, OPERAND *op1, OPERAND *op2)
{
  int vect_dtype;
  int vect_size; /* number of elements per vector */
  int type_size;
  char *type = "sp";
  char *mstr = "max";
  static char buf[MAXIDLEN];

  if (ILI_OPC(ilix) == IL_VMIN)
    mstr = "min";
  vect_dtype = ILI_OPND(ilix, 3);
  vect_size = DTY(vect_dtype + 2);
  if (vect_size != 2 && vect_size != 4)
    return NULL;

  if (vect_size == 2)
    type = "dp";

  switch (DTY(DTY(vect_dtype + 1))) {
  case TY_FLOAT:
  case TY_DBLE:
    break;
  default:
    return NULL;
  }
  op1->next = op2;
  type_size = zsize_of(DTY(vect_dtype + 1)) * 8;
  sprintf(buf, "@llvm.ppc.vsx.xv%s%s", mstr, type);
  return gen_call_to_builtin(ilix, buf, op1, make_lltype_from_dtype(vect_dtype),
                             NULL, I_PICALL, FALSE);
}
#endif

#if defined(TARGET_LLVM_ARM)
static OPERAND *
gen_call_vminmax_neon_intrinsic(int ilix, OPERAND *op1, OPERAND *op2)
{
  int vect_dtype;
  int vect_size;
  int type_size;
  char sign = 'u';
  char type = 'i';
  char *mstr = "vmax";
  static char buf[MAXIDLEN];

  if (!NEON_ENABLED)
    return NULL;
  if (ILI_OPC(ilix) == IL_VMIN)
    mstr = "vmin";
  vect_dtype = ILI_OPND(ilix, 3);
  vect_size = DTY(vect_dtype + 2);
  switch (DTY(DTY(vect_dtype + 1))) {
  case TY_FLOAT:
    type = 'f';
  case TY_INT:
    sign = 's';
  case TY_UINT:
    if (vect_size != 2 && vect_size != 4)
      return NULL;
    break;
  case TY_SINT:
    sign = 's';
  case TY_USINT:
    if (vect_size != 4 && vect_size != 8)
      return NULL;
    break;
  case TY_BINT:
    sign = 's';
  default:
    return NULL;
  }
  op1->next = op2;
  type_size = zsize_of(DTY(vect_dtype + 1)) * 8;
  sprintf(buf, "@llvm.arm.neon.%s%c.v%d%c%d", mstr, sign, vect_size, type,
          type_size);
  return gen_call_to_builtin(ilix, buf, op1, make_lltype_from_dtype(vect_dtype),
                             NULL, I_PICALL, FALSE);
}
#endif

OPERAND *
gen_call_as_llvm_instr(int sptr, int ilix)
{
  int pd_sym;
  if (call_sym_is(sptr, "__builtin_alloca")) {
    if (size_of(DT_CPTR) == 8)
      return gen_llvm_instr(ilix, IL_ARGKR, make_lltype_from_dtype(DT_CPTR),
                            make_lltype_from_dtype(DT_INT8), I_ALLOCA);
    else
      return gen_llvm_instr(ilix, IL_ARGIR, make_lltype_from_dtype(DT_CPTR),
                            make_lltype_from_dtype(DT_INT), I_ALLOCA);
  }
  return NULL;
}

static LOGICAL
cgmain_init_call(int sptr)
{
  if (sptr && (strncmp(SYMNAME(sptr), "__c_bzero", 9) == 0)) {
    return TRUE;
  }
  return FALSE;
} /* cgmain_init_call */

DTYPE
msz_dtype(MSZ msz)
{
  switch (msz) {
  case MSZ_SBYTE:
    return DT_BINT;
  case MSZ_SHWORD:
    return DT_SINT;
  case MSZ_SWORD:
    return DT_INT;
  case MSZ_SLWORD:
    return DT_INT;
  case MSZ_BYTE:
    return DT_BINT;
  case MSZ_UHWORD:
    return DT_USINT;
  case MSZ_UWORD:
    return DT_UINT;
  case MSZ_ULWORD:
    return DT_INT;
  case MSZ_FWORD:
    return DT_FLOAT;
  case MSZ_DFLWORD:
    return DT_DBLE;
  case MSZ_I8:
    return DT_INT8;
  case MSZ_PTR:
    return DT_CPTR;
  case MSZ_F16:
#ifdef TARGET_LLVM_X8664
    return DT_128;
#else
    return DT_QUAD;
#endif
  case MSZ_F32:
    return DT_256;
  default:
    assert(0, "msz_dtype, bad value", msz, 4);
  }
  return DT_NONE;
}

/* Begin define calling conventions */
#define CALLCONV \
  PRESENT(cc_default,      ""),               \
    PRESENT(arm_aapcscc,     "arm_aapcscc"),    \
    PRESENT(arm_aapcs_vfpcc, "arm_aapcs_vfpcc")

#define PRESENT(x, y) x
enum calling_conventions { CALLCONV };
#undef PRESENT

#define PRESENT(x, y) y
char *cc_as_str[] = {CALLCONV};
#undef PRESENT

#undef CALLCONV
/* End define calling conventions */

/**
   \brief Emit line info debug information.

   Output the string " !dbg !<i>n</i>", where <i>n</i> is a metadata ref.
 */
static void
print_dbg_line_no_comma(LL_MDRef md)
{
  char buf[32];
  snprintf(buf, 32, " !dbg !%u", LL_MDREF_value(md));
  print_token(buf);
}

static void
print_dbg_line(LL_MDRef md)
{
  print_token(",");
  print_dbg_line_no_comma(md);
}

/**
   \brief Create and append a !dbg info metadata from \p module
   \param module   The module from which to get \c debug_info
 */
static void
emit_dbg_from_module(LL_Module *module)
{
  const LL_MDRef linemd = lldbg_cons_line(module->debug_info);
  if (!LL_MDREF_IS_NULL(linemd)) {
    print_dbg_line(linemd);
  }
}

static LL_Type *
fixup_x86_abi_return(LL_Type *sig)
{
  LL_Type *rv;
  const unsigned numArgs = sig->sub_elements;
  const LOGICAL isVarArgs = (sig->flags & LL_TYPE_IS_VARARGS_FUNC) != 0;
  LL_Type **args = (LL_Type**) malloc(numArgs * sizeof(LL_Type*));
  memcpy(args, sig->sub_types, numArgs * sizeof(LL_Type*));
  args[0] = make_lltype_from_dtype(DT_INT);
  rv = ll_create_function_type(sig->module, args, numArgs-1, isVarArgs);
  free(args);
  return rv;
}

#if defined(TARGET_LLVM_X8664)
LL_Type *
maybe_fixup_x86_abi_return(LL_Type *sig)
{
  if (!XBIT(183, 0x400000) && (sig->data_type == LL_PTR)) {
    LL_Type *pt = sig->sub_types[0];
    if (pt->data_type == LL_FUNCTION) {
      LL_Type *rt = pt->sub_types[0];
      if (rt->data_type == LL_I16)
        return ll_get_pointer_type(fixup_x86_abi_return(pt));
    }
  }
  return sig;
}
#endif

/**
 * \brief write \c I_CALL instruction
 * \param curr_instr  pointer to current instruction instance
 * \param emit_func_signature_for_call
 * \return 1 if debug op was written, 0 otherwise
 */
static int
write_I_CALL(INSTR_LIST *curr_instr, LOGICAL emit_func_signature_for_call)
{
  /* Function invocation description as a list of OPERAND values */
  int i_name = curr_instr->i_name;
  /* get the return type of the call */
  LL_Type *return_type = curr_instr->ll_type;
  /* Get invocation description */
  OPERAND *call_op = curr_instr->operands;
  /* Debug has not been printed yet */
  LOGICAL dbg_line_op_written = FALSE;
  LOGICAL routine_label_written = FALSE;
  /* Start with default calling conventions */
  enum calling_conventions c_conv = cc_default;
  LOGICAL callRequiresTrunc = FALSE;
  LOGICAL simple_callee = TRUE;
  LL_Type *callee_type = call_op->ll_type;
  int sptr, call_dtype, param, param_dtype;
  char callRequiresTruncName[32];

  /* operand pattern:
   *   result (optional - only if return type of call not null)
   *   if var_args need to provide call signature
   *   call sptr (if null return type, this is the first operand)
   *   zero or more operands for the call arguments
   */
  print_token("\t");
#if defined(TARGET_LLVM_X8664)
  if (return_type->data_type == LL_I16) {
    callRequiresTrunc = !XBIT(183, 0x400000);
  }
#endif
  assert(return_type, "write_I_CALL(): missing return type for call "
         "instruction", 0, ERR_Fatal);
  assert(call_op, "write_I_CALL(): missing operand for call instruction", 0,
         ERR_Fatal);

  /* The callee is either a function pointer (before LLVM 3.7) or a
   * function (3.7).
   *
   * We don't have to print the entire callee type unless it is a varargs
   * function or a function returning a function pointer.  In the common case,
   * print the function return type instead of the whole function type. LLVM
   * will infer the rest from the arguments.
   *
   * FIXME: We still generate function calls with bad callee types that
   * are not function pointers:
   * - gen_call_to_builtin()
   * - gen_va_start()
   */

  /* This should really be an assertion, see above: */
  if (ll_type_is_pointer_to_function(callee_type)) {
    callee_type = callee_type->sub_types[0];
    
    /* Varargs callee => print whole function pointer type. */
    if (callee_type->flags & LL_TYPE_IS_VARARGS_FUNC)
      simple_callee = FALSE;
    /* Call returns pointer to function => print whole type. */
    if (ll_type_is_pointer_to_function(return_type))
      simple_callee = FALSE;
  }

  if (return_type->data_type != LL_VOID) {
    if (callRequiresTrunc) {
      snprintf(callRequiresTruncName, 32, "%%call.%d", expr_id);
      print_token(callRequiresTruncName);
    } else {
      print_tmp_name(curr_instr->tmps);
    }
    print_token(" = ");
  }
  print_token(llvm_instr_names[i_name]);
  print_space(1);

  if ((!flg.ieee || XBIT(216, 1)) && (curr_instr->flags & FAST_MATH_FLAG))
    print_token("fast ");

  /* Print calling conventions */
  if (curr_instr->flags & CALLCONV_MASK) {
    enum LL_CallConv cc = (curr_instr->flags & CALLCONV_MASK) >> CALLCONV_SHIFT;
    print_token(ll_get_calling_conv_str(cc));
    print_space(1);
  }

  sptr = call_op->val.sptr;
  /* write out call signature if var_args */
  if (curr_instr->flags & FAST_CALL) {
    print_token("fastcc ");
  }

  if (simple_callee) {
    LL_Type *retTy = return_type;
    /* In simple case it is sufficient to write just the return type */
    if (callRequiresTrunc)
      retTy = make_lltype_from_dtype(DT_INT);
    write_type(retTy);
  } else {
    LL_Type *sig = emit_func_signature_for_call ? callee_type :
      call_op->ll_type;
    if (callRequiresTrunc)
      sig = fixup_x86_abi_return(sig);
    /* Write out either function type or pointer type for the callee */
    write_type(sig);
  }
  print_space(1);

    if (!routine_label_written)
      write_operand(call_op, " (", FLG_OMIT_OP_TYPE);
    write_operands(call_op->next, 0);
    /* if no arguments, write out the parens */
    print_token(")");
  if (callRequiresTrunc) {
    print_dbg_line(curr_instr->dbg_line_op);
    print_token("\n\t");
    print_tmp_name(curr_instr->tmps);
    print_token(" = trunc i32 ");
    print_token(callRequiresTruncName);
    print_token(" to i16");
  }
  {
    const LOGICAL wrDbg = TRUE;
    if (wrDbg && cpu_llvm_module->debug_info &&
        ll_feature_subprogram_not_in_cu(&cpu_llvm_module->ir) &&
        LL_MDREF_IS_NULL(curr_instr->dbg_line_op)) {
      /* we must emit !dbg metadata in this case */
      emit_dbg_from_module(cpu_llvm_module);
      return TRUE;
    }
  }
  return dbg_line_op_written;
} /* write_I_CALL */

/**
   \brief Create the root and omnipotent pointer nodes of the TBAA tree

   These metadata nodes are unique per LLVM module and should be cached there.
 */
static LL_MDRef
get_omnipotent_pointer(LL_Module *module)
{
  LL_MDRef omni = LL_MDREF_INITIALIZER(0, 0);
  omni = module->omnipotentPtr;
  if (LL_MDREF_IS_NULL(omni)) {
    const char *baseName = "Flang TBAA";
    const char *const omniName = "unlimited ptr";
    const char *const unObjName = "unref ptr";
    LL_MDRef s0 = ll_get_md_string(module, baseName);
    LL_MDRef r0 = ll_get_md_node(module, LL_PlainMDNode, &s0, 1);
    LL_MDRef a[3];
    a[0] = ll_get_md_string(module, unObjName);
    a[1] = r0;
    a[2] = ll_get_md_i64(module, 0);
    module->unrefPtr = ll_get_md_node(module, LL_PlainMDNode, a, 3);
    a[0] = ll_get_md_string(module, omniName);
    a[1] = r0;
    a[2] = ll_get_md_i64(module, 0);
    omni = ll_get_md_node(module, LL_PlainMDNode, a, 3);
    module->omnipotentPtr = omni;
  }
  return omni;
}

static LOGICAL
assumeWillAlias(int nme)
{
  do {
    int sym = NME_SYM(nme);
    if (sym > 0) {
#if defined(VARIANTG)
      const int variant = VARIANTG(sym);
      if (variant > 0)
        sym = variant;
#endif
      if (NOCONFLICTG(sym) || CCSYMG(sym)) {
/* do nothing */
#if defined(PTRSAFEG)
      } else if (PTRSAFEG(sym)) {
/* do nothing */
#endif
      } else if (DTY(DTYPEG(sym)) == TY_PTR) {
        return TRUE;
#if defined(POINTERG)
      } else if (POINTERG(sym)) {
        return TRUE;
#endif
      }
    }
    switch (NME_TYPE(nme)) {
    default:
      return FALSE;
    case NT_MEM:
    case NT_IND:
    case NT_ARR:
    case NT_SAFE:
      nme = NME_NM(nme);
      break;
    }
  } while (nme != 0);
  return FALSE;
}

/**
   \brief Fortran location set to "TBAA" translation

   In Fortran, there isn't any TBAA. But we use the LLVM mechanism to hint to
   the backend what may alias.
 */
static LL_MDRef
locset_to_tbaa_info(LL_Module *module, LL_MDRef omniPtr, int ilix)
{
  char name[16];
  LL_MDRef a[3];
  int bsym;
  const ILI_OP opc = ILI_OPC(ilix);
  const ILTY_KIND ty = IL_TYPE(opc);
  const int nme = (ty == ILTY_LOAD) ? ILI_OPND(ilix, 2) : ILI_OPND(ilix, 3);
  const int base = basenme_of(nme);

  if (!base)
    return omniPtr;

  bsym = NME_SYM(base);
  switch (STYPEG(bsym)) {
  case ST_VAR:
  case ST_ARRAY:
  case ST_STRUCT:
    /* do nothing */
    break;
  default:
    return LL_MDREF_ctor(LL_MDREF_kind(module->unrefPtr),
                         LL_MDREF_value(module->unrefPtr));
  }

  if ((NME_SYM(nme) != bsym) && assumeWillAlias(nme))
    return omniPtr;

#if defined(REVMIDLNKG)
  if (REVMIDLNKG(bsym)) {
    const int ptr = REVMIDLNKG(bsym);
    if (!NOCONFLICTG(ptr) && !PTRSAFEG(ptr))
      return LL_MDREF_ctor(0, 0);
    bsym = ptr;
  }
#endif

  if (NOCONFLICTG(bsym) || CCSYMG(bsym)) {
/* do nothing */
#if defined(PTRSAFEG)
  } else if (PTRSAFEG(bsym)) {
/* do nothing */
#endif
  } else if (DTY(DTYPEG(bsym)) == TY_PTR) {
    return omniPtr;
#if defined(POINTERG)
  } else if (POINTERG(bsym)) {
    return omniPtr;
#endif
  }

#if defined(SOCPTRG)
  if (SOCPTRG(bsym)) {
    int ysoc = SOCPTRG(bsym);
    while (SOC_NEXT(ysoc))
      ysoc = SOC_NEXT(ysoc);
    snprintf(name, 16, "soc.%08x", ysoc);
    a[0] = ll_get_md_string(module, name);
    a[1] = omniPtr;
    a[2] = ll_get_md_i64(module, 0);
    return ll_get_md_node(module, LL_PlainMDNode, a, 3);
  }
#endif

  /* variable can't alias type-wise. It's Fortran! */
  snprintf(name, 16, "tnm.%08x", base);
  a[0] = ll_get_md_string(module, name);
  a[1] = omniPtr;
  a[2] = ll_get_md_i64(module, 0);
  return ll_get_md_node(module, LL_PlainMDNode, a, 3);
}

/**
   \brief Write TBAA metadata for the address \p opnd
   \param module  The module
   \param opnd  a pointer to a typed location
   \param isVol   Is this a volatile access?

   To do this correctly for C, we have use the effective type.
 */
static LL_MDRef
get_tbaa_metadata(LL_Module *module, int ilix, OPERAND *opnd, int isVol)
{
  LL_MDRef a[3];
  LL_MDRef myPtr, omniPtr;
  LL_Type *ty;

  ty = opnd->ll_type;
  assert(ty->data_type == LL_PTR, "must be a ptr", ty->data_type, ERR_Fatal);
  omniPtr = get_omnipotent_pointer(module);

  /* volatile memory access aliases all */
  if (isVol)
    return omniPtr;

  ty = ty->sub_types[0];
  assert(ty->data_type != LL_NOTYPE, "must be a type", 0, ERR_Fatal);

  myPtr = locset_to_tbaa_info(module, omniPtr, ilix);

cons_indirect:
  if (!myPtr)
    return LL_MDREF_ctor(0, 0);

  a[0] = a[1] = myPtr;
  a[2] = ll_get_md_i64(module, 0);
  return ll_get_md_node(module, LL_PlainMDNode, a, 3);
}

/**
   \brief Is TBAA disabled?
 */
INLINE static bool
tbaa_disabled(void)
{
  return (flg.opt < 2) || XBIT(183, 0x20000);
}

/**
   \brief Write out the TBAA metadata, if needed
 */
static void
write_tbaa_metadata(LL_Module *mod, int ilix, OPERAND *opnd, int isVol)
{
  LL_MDRef md;

  if (tbaa_disabled()) {
    /* TBAA is disabled */
    return;
  }

  md = get_tbaa_metadata(mod, ilix, opnd, isVol);
  if (!LL_MDREF_IS_NULL(md)) {
    print_token(", !tbaa ");
    write_mdref(gbl.asmfil, mod, md, 1);
  }
}

/**
   \brief Test for improperly constructed instruction streams
   \param insn   The instruction under the cursor
   \return TRUE  iff we don't need to emit a dummy label
 */
INLINE static int
dont_force_a_dummy_label(INSTR_LIST *insn)
{
  const int i_name = insn->i_name;
  if (i_name == I_NONE) {
    /* insn is a label, no need for a dummy */
    return TRUE;
  }
  if ((i_name == I_BR) && insn->next && (insn->next->i_name == I_NONE)) {
    /* odd case: two terminators appearing back-to-back followed by a
       label. write_instructions() will skip over this 'insn' and
       emit the next one. Don't emit two labels back-to-back. */
    return TRUE;
  }
  return FALSE;
}

/**
   \brief For the given instruction, write [singlethread] <memory ordering>
   to the LLVM IR output file.
 */
static void
write_memory_order(INSTR_LIST *instrs)
{
  if (instrs->flags & ATOMIC_SINGLETHREAD_FLAG) {
    print_token(" singlethread");
  }
  print_space(1);
  print_token(get_atomic_memory_order_name(instrs->flags));
}

/**
   \brief For the given instruction, write [singlethread] <memory ordering>,
   <alignment> to the LLVM IR output file.
 */
static void
write_memory_order_and_alignment(INSTR_LIST *instrs)
{
  int align;
  DEBUG_ASSERT(instrs->i_name == I_LOAD || instrs->i_name == I_STORE,
           "write_memory_order_and_alignment: not a load or store instruction");

  /* Print memory order if instruction is atomic. */
  if (instrs->flags & ATOMIC_MEM_ORD_FLAGS) {
    write_memory_order(instrs);
  } else {
    DEBUG_ASSERT(
        (instrs->flags & ATOMIC_SINGLETHREAD_FLAG) == 0,
        "write_memory_order_and_alignment: inappropriate singlethread");
  }

  /* Extract the alignment in bytes from the flags field. It's stored as
   * log2(bytes). */
  align = LDST_BYTEALIGN(instrs->flags);
  if (align) {
    char align_token[4];
    print_token(", align ");
    sprintf(align_token, "%d", align);
    print_token(align_token);
  }
}

/**
   \brief Write the instruction list to the LLVM IR output file
 */
static void
write_instructions(LL_Module *module)
{
  INSTR_LIST *instrs;
  OPERAND *p, *call_op, *p1;
  LL_Type *return_type;
  DTYPE call_dtype, param_dtype;
  LL_InstrName i_name;
  int align;
  SPTR sptr;
  int param;
  bool forceLabel = true;
  LOGICAL dbg_line_op_written;
  LOGICAL routine_label_written;

  DBGTRACEIN("")

  for (instrs = Instructions; instrs; instrs = instrs->next) {
    llvm_info.curr_instr = instrs;
    i_name = instrs->i_name;
    dbg_line_op_written = FALSE;

    DBGTRACE3("#instruction(%d) %s for ilix %d\n", i_name,
              llvm_instr_names[i_name], instrs->ilix);

    if (dont_force_a_dummy_label(instrs))
      forceLabel = false;
    if (forceLabel) {
      char buff[32];
      static unsigned counter = 0;
      snprintf(buff, 32, "L.dead%u:\n", counter++);
      print_token(buff);
      forceLabel = false;
    }
    if (instrs->flags & CANCEL_CALL_DBG_VALUE) {
      DBGTRACE("#instruction llvm.dbg.value canceled")
      continue;
    } else if (BINOP(i_name) || BITOP(i_name)) {
      print_token("\t");
      print_tmp_name(instrs->tmps);
      print_token(" = ");
      print_token(llvm_instr_names[i_name]);
      if (!flg.ieee || XBIT(216, 1))
        switch (i_name) {
        case I_FADD:
        case I_FSUB:
        case I_FMUL:
        case I_FDIV:
        case I_FREM:
          print_token(" fast");
          break;
        default:
          break;
        }
      p = instrs->operands;
      assert(p->ll_type, "write_instruction(): missing binary type", 0, 4);
      asrt(match_types(instrs->ll_type, p->ll_type) == MATCH_OK);
      print_space(1);
      /* write_type(p->ll_type); */
      write_type(instrs->ll_type);
      print_space(1);
      write_operand(p, ", ", FLG_OMIT_OP_TYPE);
      p = p->next;
      assert(p->ll_type, "write_instruction(): missing binary type", 0, 4);
      asrt(match_types(instrs->ll_type, p->ll_type) == MATCH_OK);
      write_operand(p, "", FLG_OMIT_OP_TYPE);
    } else if (CONVERT(i_name)) {
      p = instrs->operands;
      assert(p->next == NULL, "write_instructions(),bad next ptr", 0, 4);
      print_token("\t");
      print_tmp_name(instrs->tmps);
      print_token(" = ");
      print_token(llvm_instr_names[i_name]);
      print_space(1);
#if defined(PGFTN) && defined(TARGET_LLVM_X8664)
      write_operand(p, " to ", FLG_FIXUP_RETURN_TYPE);
      write_type(maybe_fixup_x86_abi_return(instrs->ll_type));
#else
      write_operand(p, " to ", 0);
      write_type(instrs->ll_type);
#endif
    } else {
      switch (i_name) {
      case I_NONE: /* should be a label */
        forceLabel = false;
        sptr = instrs->operands->val.sptr;
        if (instrs->prev == NULL && sptr == 0) {
          /* entry label we just ignore it*/
          break;
        }
        assert(sptr, "write_instructions(): missing symbol", 0, 4);
        if (sptr != instrs->operands->val.sptr)
          printf("sptr mixup sptr= %d, val = %d\n", sptr,
                 instrs->operands->val.sptr);
        /* every label must be immediately preceded by a branch */
        if (!llvm_info.curr_instr->prev ||
            !INSTR_IS_BRANCH(INSTR_PREV(llvm_info.curr_instr)))
        {
          print_token("\t");
          print_token(llvm_instr_names[I_BR]);
          print_token(" label %L");
          print_token(get_llvm_name(sptr));
          print_nl();
        }

        write_operand(instrs->operands, "", 0);
        /* if label is last instruction in the module we need
         * a return instruction as llvm requires a termination
         * instruction at the end of the block.
         */
        if (!instrs->next) {
          print_nl();
          print_token("\t");
          print_token(llvm_instr_names[I_RET]);
          print_space(1);
          if (has_multiple_entries(gbl.currsub)) {
            if (gbl.arets)
              llvm_info.return_ll_type = make_lltype_from_dtype(DT_INT);
            else
              llvm_info.return_ll_type = make_lltype_from_dtype(DT_NONE);
          }
          write_type(llvm_info.abi_info->extend_abi_return ?
                     make_lltype_from_dtype(DT_INT) :
                     llvm_info.return_ll_type);
          if (llvm_info.return_ll_type->data_type != LL_VOID) {
            switch (llvm_info.return_ll_type->data_type) {
            case LL_PTR:
              print_token(" null");
              break;
            case LL_I1:
            case LL_I8:
            case LL_I16:
            case LL_I24:
            case LL_I32:
            case LL_I40:
            case LL_I48:
            case LL_I56:
            case LL_I64:
            case LL_I128:
            case LL_I256:
              print_token(" 0");
              break;
            case LL_DOUBLE:
            case LL_FLOAT:
              print_token(" 0.0");
              break;
            case LL_PPC_FP128:
              print_token(" 0xM00000000000000000000000000000000");
              break;
            default:
              print_token(" zeroinitializer");
            }
          }
        }
        break;
      case I_EXTRACTVAL:
      case I_INSERTVAL: {
        /* extractvalue lhs, rhs, int
         * lhs = extractvalue rhs_type rhs, int
         * lhs = insertvalue rhs_type rhs, int
         */
        OPERAND *cc = instrs->operands;
        print_token("\t");
        print_tmp_name(instrs->tmps);
        print_token(" = ");
        print_token(llvm_instr_names[i_name]);
        print_space(1);
        write_verbose_type(instrs->ll_type);
        print_space(1);
        write_operand(cc, ", ", FLG_OMIT_OP_TYPE);
        cc = cc->next;
        if (i_name == I_INSERTVAL) {
          write_operand(cc, ", ", 0);
          cc = cc->next;
        }
        write_operand(cc, "", FLG_OMIT_OP_TYPE);
      } break;
      case I_RESUME: {
        /* resume { i8*, i32 } %33 */
        OPERAND *cc;
        /* forceLabel = TRUE; is not needed here? */
        cc = instrs->operands;
        print_token("\t");
        print_token(llvm_instr_names[I_RESUME]);
        write_verbose_type(cc->ll_type);
        write_operand(cc, " ", FLG_OMIT_OP_TYPE);
        break;
      }
      case I_CLEANUP:
        print_token("\t");
        print_token(llvm_instr_names[I_CLEANUP]);
        break;
      case I_LANDINGPAD:
        /* landingpad: typeinfo_var, catch_clause_sptr,
         * caught_object_sptr
         */
        /* LABEL */
        print_token("\t");
        print_tmp_name(instrs->tmps);
        print_token(" = ");
        print_token(llvm_instr_names[I_LANDINGPAD]);
        print_space(1);
        write_verbose_type(instrs->ll_type);
        if (ll_feature_eh_personality_on_landingpad(&module->ir))
          print_personality();
        dbg_line_op_written = TRUE;
        break;
      case I_CATCH: {
        OPERAND *cc;
        cc = instrs->operands;

        if (cc->ot_type == OT_CONSTVAL) {
          print_token("\tcatch i8* ");
          write_operand(cc, " ", FLG_OMIT_OP_TYPE);
        } else {
          print_token("\tcatch i8* bitcast ( ");
          write_type(cc->ll_type);
          print_token("* ");
          write_operand(cc, " ", FLG_OMIT_OP_TYPE);
          print_token(" to i8*)");
        }
        break;
      }
      case I_FILTER: {
        /* "filter <array-type> [ <array-of-typeinfo-vars> ]"
           Each operand is a typeinfo variable for a type in the exception
           specification. */
        if (instrs->operands == NULL) {
          /* A no-throw exception spec, "throw()" */
          /* LLVM documentation says that "filter [0xi8*] undef" is fine, but
             the LLVM compiler rejects it.  So we have to do it differently. */
          print_token("\t\tfilter [0 x i8*] zeroinitializer");
        } else {
          OPERAND *esti;       /* One typeinfo var for the exception spec. */
          int count = 0;       /* Number of types in the exception spec. */
          char buffer[19 + 9]; /* Format string + small integer */
          for (esti = instrs->operands; esti != NULL; esti = esti->next) {
            ++count;
          }
          snprintf(buffer, sizeof buffer, "\tfilter [%d x i8*] [", count);
          print_token(buffer);
          for (esti = instrs->operands; esti != NULL; esti = esti->next) {
            print_token("i8* bitcast ( ");
            write_type(esti->ll_type);
            print_token("* ");
            write_operand(esti, NULL, FLG_OMIT_OP_TYPE);
            print_token(" to i8*)");
            if (esti->next != NULL) {
              print_token(", ");
            }
          }
          print_token("]");
        }
        break;
      }
      case I_INVOKE:
      /* forceLabel = true; is not needed here, already handled */
      case I_PICALL:
      case I_CALL:
        dbg_line_op_written = write_I_CALL(instrs,
                          ll_feature_emit_func_signature_for_call(&module->ir));
        break;
      case I_SW:
        forceLabel = true;
        p = instrs->operands;
        print_token("\t");
        print_token(llvm_instr_names[i_name]);
        print_space(1);
        write_operand(p, ", ", 0);
        write_operand(p->next, "[\n\t\t", 0);
        p1 = p->next->next;
        while (p1) {
          write_operand(p1, ", ", 0);
          p1 = p1->next;
          if (p1) {
            write_operand(p1, "\n\t\t", 0);
            p1 = p1->next;
          }
        }
        print_token("]");
        break;
      case I_RET:
        forceLabel = true;
        p = instrs->operands;
        /* This is a way to return value for multiple entries with return type
         * pass as argument to the master/common routine */
        if (has_multiple_entries(gbl.currsub) && FVALG(gbl.currsub) &&
            SCG(FVALG(gbl.currsub)) != SC_DUMMY) {
          /* (1) bitcast result(second argument) from i8* to type of p->ll_type
           * (2) store result into (1)
           * (3) return void.
           */
          store_return_value_for_entry(p, i_name);
          break;
        }
        print_token("\t");
        print_token(llvm_instr_names[i_name]);
        print_space(1);
        write_type(llvm_info.abi_info->extend_abi_return ?
                   make_lltype_from_dtype(DT_INT) :
                   llvm_info.return_ll_type);
        if (p->ot_type != OT_NONE && p->ll_type->data_type != LL_VOID) {
          print_space(1);
          write_operand(p, "", FLG_OMIT_OP_TYPE);
          assert(p->next == NULL, "write_instructions(), bad next ptr", 0, 4);
        }
        break;
      case I_LOAD:
        p = instrs->operands;
        print_token("\t");
        print_tmp_name(instrs->tmps);
        print_token(" = ");
        print_token(llvm_instr_names[i_name]);
        print_space(1);
        if (instrs->flags & ATOMIC_MEM_ORD_FLAGS) {
          print_token("atomic ");
        }
        if (instrs->flags & VOLATILE_FLAG) {
          print_token("volatile ");
        }

        /* Print out the loaded type. */
        if (ll_feature_explicit_gep_load_type(&module->ir)) {
          LL_Type *t = p->ll_type;
          assert(t && t->data_type == LL_PTR, "load operand must be a pointer",
                 0, 4);
          t = t->sub_types[0];
          print_token(t->str);
          print_token(", ");
        }

        /* Print out the pointer operand. */
        write_operand(p, "", 0);

        write_memory_order_and_alignment(instrs);

        assert(p->next == NULL, "write_instructions(), bad next ptr", 0, 4);

        write_tbaa_metadata(module, instrs->ilix, instrs->operands,
                            instrs->flags & VOLATILE_FLAG);
        break;
      case I_STORE:
        p = instrs->operands;
        print_token("\t");
        print_token(llvm_instr_names[i_name]);
        print_space(1);
        if (instrs->flags & ATOMIC_MEM_ORD_FLAGS) {
          print_token("atomic ");
        }
        if (instrs->flags & VOLATILE_FLAG) {
          print_token("volatile ");
        }
        write_operand(p, ", ", 0);
        p = p->next;
        write_operand(p, "", 0);

        write_memory_order_and_alignment(instrs);

        write_tbaa_metadata(module, instrs->ilix, instrs->operands->next,
                            instrs->flags & VOLATILE_FLAG);
        break;
      case I_BR:
        if (!INSTR_PREV(instrs) || ((INSTR_PREV(instrs)->i_name != I_RET) &&
                                    (INSTR_PREV(instrs)->i_name != I_RESUME) &&
                                    (INSTR_PREV(instrs)->i_name != I_BR))) {
          forceLabel = true;
          print_token("\t");
          print_token(llvm_instr_names[i_name]);
          print_space(1);
          write_operands(instrs->operands, 0);
        }
        break;
      case I_INDBR:
        forceLabel = true;
        print_token("\t");
        print_token(llvm_instr_names[i_name]);
        print_space(1);
        write_operands(instrs->operands, 0);
        break;
      case I_GEP:
        p = instrs->operands;
        print_token("\t");
        print_tmp_name(instrs->tmps);
        print_token(" = ");
        print_token(llvm_instr_names[i_name]);
        print_space(1);

        /* Print out the indexed type. */
        if (ll_feature_explicit_gep_load_type(&module->ir)) {
          LL_Type *t = p->ll_type;
          assert(t && t->data_type == LL_PTR, "gep operand must be a pointer",
                 0, 4);
          t = t->sub_types[0];
          print_token(t->str);
          print_token(", ");
        }

        write_operands(p, FLG_AS_UNSIGNED);
        break;
      case I_VA_ARG:
        p = instrs->operands;
        print_token("\t");
        print_tmp_name(instrs->tmps);
        print_token(" = ");
        print_token(llvm_instr_names[i_name]);
        print_space(1);
        write_operand(p, "", 0);
        write_type(instrs->ll_type);
        break;
      case I_DECL:
        break;
      case I_FCMP:
      case I_ICMP:
        print_token("\t");
        print_tmp_name(instrs->tmps);
        print_token(" = ");
        print_token(llvm_instr_names[i_name]);
        if ((i_name == I_FCMP) && (!flg.ieee))
          print_token(" fast");
        print_space(1);
        p = instrs->operands;
        write_operand(p, " ", 0);
        /* use the type of the comparison operators */
        write_type(instrs->operands->next->ll_type);
        print_space(1);
        p = p->next;
        write_operand(p, ", ", FLG_OMIT_OP_TYPE);
        p = p->next;
        write_operand(p, "", FLG_OMIT_OP_TYPE);
        break;
      case I_ALLOCA:
        print_token("\t");
        print_tmp_name(instrs->tmps);
        print_token(" = ");
        print_token(llvm_instr_names[i_name]);
        print_space(1);
        write_type(instrs->ll_type->sub_types[0]);
        print_token(",");
        print_space(1);
        p = instrs->operands;
        write_operand(p, "", 0);
        break;
      case I_SELECT:
      case I_EXTELE:
      case I_INSELE:
      case I_SHUFFVEC:
        print_token("\t");
        print_tmp_name(instrs->tmps);
        print_token(" = ");
        print_token(llvm_instr_names[i_name]);
        p = instrs->operands;
        print_space(1);
        write_operands(p, 0);
        break;
      case I_BARRIER:
        print_token("\t");
        print_token(llvm_instr_names[i_name]);
        print_space(1);
        print_token("acq_rel");
        break;
      case I_ATOMICRMW:
        print_token("\t");
        print_tmp_name(instrs->tmps);
        print_token(" = ");
        print_token(llvm_instr_names[i_name]);
        print_space(1);
        print_token(get_atomicrmw_opname(instrs->flags));
        print_space(1);
        write_operands(instrs->operands, 0);
        write_memory_order(instrs);
        break;
      case I_CMPXCHG: 
        print_token("\t");
        print_tmp_name(instrs->tmps);
        print_token(" = ");
        print_token(llvm_instr_names[i_name]);
        print_space(1);
        write_operands(instrs->operands, 0);
        print_token(get_atomic_memory_order_name(instrs->flags));
        print_space(1);
        print_token(get_atomic_memory_order_name(FROM_CMPXCHG_MEMORDER_FAIL(instrs->flags)));
        break;
      case I_FENCE:
        print_token("\t");
        print_token(llvm_instr_names[i_name]);
        write_memory_order(instrs);
        break;
      case I_UNREACH:
        print_token("\t");
        print_token(llvm_instr_names[i_name]);
        break;
      default:
        DBGTRACE1("### write_instructions(): unknown instr name: %s",
                  llvm_instr_names[i_name])
        assert(0, "write_instructions(): unknown instr name", instrs->i_name,
               4);
      }
    }
    if (!LL_MDREF_IS_NULL(instrs->dbg_line_op) && !dbg_line_op_written) {
      print_dbg_line(instrs->dbg_line_op);
    }
    if (instrs->traceComment) {
      print_token("\t\t;) ");
      print_token(instrs->traceComment);
    }
#if DEBUG
    if (XBIT(183, 0x800)) {
      char buf[200];

      if (instrs->tmps)
        sprintf(buf, "\t\t\t; ilix %d, usect %d", instrs->ilix,
                instrs->tmps->use_count);
      else
        sprintf(buf, "\t\t\t; ilix %d", instrs->ilix);

      if (instrs->flags & DELETABLE)
        strcat(buf, " deletable");
      if (instrs->flags & STARTEBB)
        strcat(buf, " startebb");
      if (instrs->flags & ROOTDG)
        strcat(buf, " rootdg");
      print_token(buf);
      sprintf(buf, "\ti%d", instrs->rank);
      print_token(buf);
    }
#endif
    print_nl();
  }

  DBGTRACEOUT("")
} /* write_instructions */

/* write out the struct member types */
static void
write_verbose_type(LL_Type *ll_type)
{
  print_token(ll_type->str);
}

#if DEBUG
void
dump_type_for_debug(LL_Type *ll_type)
{
  if (ll_type == NULL) {
    fprintf(ll_dfile, "(UNKNOWN)\n");
  }
  fprintf(ll_dfile, "%s\n", ll_type->str);
}
#endif

/* create the instructions for:
 * 	%new_tmp =  extractvalue { struct mems } %tmp, index
 *  or
 * 	%new_tmp =  insertvalue { struct mems } %tmp, tmp2_type tmp2, index

 */
TMPS *
gen_extract_insert(int i_name, LL_Type *struct_type, TMPS *tmp,
                   LL_Type *tmp_type, TMPS *tmp2, LL_Type *tmp2_type, int index)
{
  OPERAND *cc;
  int num[2];
  TMPS *new_tmp;
  INSTR_LIST *Curr_Instr;

  /* %new_tmp =  extractvalue { struct mems } %tmp, index */
  if (tmp)
    cc = make_tmp_op(tmp_type, tmp);
  else
    cc = make_undef_op(tmp_type);
  new_tmp = make_tmps();
  Curr_Instr = gen_instr(i_name, new_tmp, struct_type, cc);

  /* insertval requires one more temp */
  if (tmp2) {
    cc->next = make_tmp_op(tmp2_type, tmp2);
    cc = cc->next;
  }
  cc->next = make_operand();
  cc = cc->next;

  cc->ot_type = OT_CONSTSPTR;
  cc->ll_type = make_lltype_from_dtype(DT_INT);
  switch (index) {
  case 0:
    cc->val.sptr = stb.i0;
    break;
  case 1:
    cc->val.sptr = stb.i1;
    break;
  default:
    num[0] = 0;
    num[1] = index;
    cc->val.sptr = getcon(num, DT_INT);
    break;
  }

  ad_instr(0, Curr_Instr);
  return new_tmp;
}

/* Generate an insertvalue instruction which inserts 'elem' into 'aggr' at
 * position 'index'. */
static OPERAND *
gen_insert_value(OPERAND *aggr, OPERAND *elem, unsigned index)
{
  aggr->next = elem;
  elem->next = make_constval32_op(index);
  return ad_csed_instr(I_INSERTVAL, 0, aggr->ll_type, aggr, 0, FALSE);
}

static void
gen_store_instr(int sptr_lhs, TMPS *tmp, LL_Type *tmp_type)
{
  OPERAND *cc;
  int num[2];
  INSTR_LIST *Curr_Instr;

  /* store new_tmp_type %new_tmp sptr_lhs_type %llvm_name_for_sptr_lhs */
  Curr_Instr = make_instr(I_STORE);
  Curr_Instr->operands = cc = make_tmp_op(tmp_type, tmp);

  cc->next = make_operand();
  cc = cc->next;

  cc->val.sptr = sptr_lhs;
  cc->ot_type = OT_VAR;
  cc->ll_type = make_ptr_lltype(make_lltype_from_dtype(DTYPEG(sptr_lhs)));
  set_llvm_sptr_name(cc);

  ad_instr(0, Curr_Instr);
}

/** \brief Construct an INSTR_LIST object.
   
    Initializes fields i_name (and dbg_line_op if appropriate).
    Zeros the other fields. */
static INSTR_LIST *
make_instr(LL_InstrName instr_name)
{
  INSTR_LIST *iptr;

  iptr = (INSTR_LIST *)getitem(LLVM_LONGTERM_AREA, sizeof(INSTR_LIST));
  memset(iptr, 0, sizeof(INSTR_LIST));
  iptr->i_name = instr_name;
  if (flg.debug || XBIT(120, 0x1000)) {
    switch (instr_name) {
    default:
      iptr->dbg_line_op = lldbg_get_line(cpu_llvm_module->debug_info);
      break;
    case I_NONE:
    case I_DECL:
    case I_CLEANUP:
    case I_CATCH:
      break;
    }
  }
  return iptr;
} /* make_instr */

/**
   \brief Like make_instr, but also sets tmps, ll_type, and operands.
 */
static INSTR_LIST *
gen_instr(LL_InstrName instr_name, TMPS *tmps, LL_Type *ll_type,
          OPERAND *operands)
{
  INSTR_LIST *iptr;

  iptr = make_instr(instr_name);
  iptr->tmps = tmps;
  if (tmps != NULL)
    tmps->info.idef = iptr;
  iptr->ll_type = ll_type;
  iptr->operands = operands;
  return iptr;
}

static OPERAND *
ad_csed_instr(LL_InstrName instr_name, int ilix, LL_Type *ll_type,
              OPERAND *operands, LL_InstrListFlags flags, bool do_cse)
{
  OPERAND *operand, *new_op;
  INSTR_LIST *instr;
  if (do_cse && ENABLE_CSE_OPT && !new_ebb) {
    instr = llvm_info.last_instr;
    while (instr) {
      if (instr->i_name == instr_name) {
        operand = instr->operands;
        new_op = operands;
        while (operand && new_op) {
          if (!same_op(operand, new_op))
            break;
          operand = operand->next;
          new_op = new_op->next;
        }
        if (operand == NULL && new_op == NULL)
          return make_tmp_op(instr->ll_type, instr->tmps);
      }
      switch (instr->i_name) {
      case I_SW:
      case I_INVOKE:
      case I_CALL:
        instr = NULL;
        break;
      case I_BR:
      case I_INDBR:
      case I_NONE:
        if (!ENABLE_ENHANCED_CSE_OPT) {
          instr = NULL;
          break;
        }
      default:
        if (instr->flags & STARTEBB)
          instr = NULL;
        else
          instr = instr->prev;
      }
    }
  }
  operand = make_tmp_op(ll_type, make_tmps());
  instr = gen_instr(instr_name, operand->tmps, ll_type, operands);
  instr->flags = flags;
  ad_instr(ilix, instr);
  return operand;
}

static void
ad_instr(int ilix, INSTR_LIST *instr)
{
  OPERAND *operand;

  if (instr == NULL)
    return;

  instr->ilix = ilix;

  for (operand = instr->operands; operand; operand = operand->next) {
    if (operand->ot_type == OT_TMP) {
      assert(operand->tmps, "ad_instr(): missing last instruction", 0, 4);
      operand->tmps->use_count++;
    }
  }
  if (Instructions) {
    assert(llvm_info.last_instr, "ad_instr(): missing last instruction", 0, 4);
    llvm_info.last_instr->next = instr;
    instr->prev = llvm_info.last_instr;
  } else {
    assert(!llvm_info.last_instr, "ad_instr(): last instruction not NULL", 0,
           4);
    Instructions = instr;
  }
  llvm_info.last_instr = instr;
  if (new_ebb) {
    instr->flags |= STARTEBB;
    new_ebb = FALSE;
  }
}

static LOGICAL
cancel_store(int ilix, int op_ili, int addr_ili)
{
  ILI_OP op_opc = ILI_OPC(op_ili);
  int csed = 0;

  if (is_cseili_opcode(op_opc)) {
    op_ili = ILI_OPND(op_ili, 1);
    op_opc = ILI_OPC(op_ili);
    csed = 1;
  }
  if (IL_TYPE(op_opc) == ILTY_LOAD) {
    LOGICAL ret_val = (ILI_OPND(op_ili, 1) == addr_ili);

    if (ret_val && csed) {
      DBGTRACE1("#store of CSE'd operand removed for ilix(%d)", ilix)
    }

    return ret_val;
  }
  return FALSE;
}

static LL_InstrListFlags
ll_instr_flags_from_memory_order(MEMORY_ORDER mo)
{
  switch (mo) {
  default:
    assert(false,
           "ll_instr_flags_from_memory_order: unimplemented memory order", mo,
           4);
  case MO_RELAXED:
    return ATOMIC_MONOTONIC_FLAG;
  case MO_CONSUME:
  /* LLVM does not support "consume", so round up to acquire. */
  case MO_ACQUIRE:
    return ATOMIC_ACQUIRE_FLAG;
  case MO_RELEASE:
    return ATOMIC_RELEASE_FLAG;
  case MO_ACQ_REL:
    return ATOMIC_ACQ_REL_FLAG;
  case MO_SEQ_CST:
    return ATOMIC_SEQ_CST_FLAG;
  }
}

/**
  \brief From an ILI atomic instruction with a fence,
         get instruction flags for [singlethread] <memory order>.
 */
static LL_InstrListFlags
ll_instr_flags_for_memory_order_and_scope(int ilix)
{
  LL_InstrListFlags flags =
      ll_instr_flags_from_memory_order(memory_order(ilix));
  ATOMIC_INFO info = atomic_info(ilix);
  if (info.scope == SS_SINGLETHREAD)
    flags |= ATOMIC_SINGLETHREAD_FLAG;
  return flags;
}

static void
make_stmt(STMT_Type stmt_type, int ilix, LOGICAL deletable, int next_bih_label,
          int ilt)
{
  int lhs_ili, rhs_ili, sc, nme, i, size1, size2;
  SPTR sptr, sptr_lab;
  int offset_to, offset_from, stype, ts, sym, pd_sym;
  DTYPE dtype;
  int to_ili, from_ili, length_ili, opnd, bytes, from_nme, cc;
  ILI_OP opc;
  char *name, *lname, *tmp_name, *retc;
  TMPS *tmps, *last_tmps, *new_tmps;
  LL_Type *llt, *op_type, *last_type, *ty1, *ty2, *load_type, *switch_type;
  INSTR_LIST *instr;
  OPERAND *ret_op, *store_op, *operand1, *operand2, *op_tmp, *op1, *op2;
  OPERAND *load_op, *dst_op, *src_op, *first_label, *second_label;
  int match, conversion_instr, d1, d2;
  LOGICAL mark_daddr, sta, has_entries;
  MSZ msz;
  LL_Type *llt_expected;
  int alignment;
  INSTR_LIST *Curr_Instr;

  DBGTRACEIN2(" type: %s ilix: %d", stmt_names[stmt_type], ilix)

  curr_stmt_type = stmt_type;
  if (last_stmt_is_branch && stmt_type != STMT_LABEL) {
    sptr_lab = getlab();
    update_llvm_sym_arrays();
    make_stmt(STMT_LABEL, sptr_lab, FALSE, 0, ilt);
  }
  last_stmt_is_branch = 0;
  switch (stmt_type) {
  case STMT_RET: {
    LL_Type *retTy = llvm_info.abi_info->extend_abi_return ?
      make_lltype_from_dtype(DT_INT) : llvm_info.return_ll_type;
    last_stmt_is_branch = 1;
    has_entries = has_multiple_entries(gbl.currsub);
    switch (ILI_OPC(ilix)) {
    case IL_AADD:
    case IL_ASUB:
    case IL_ACON:
    case IL_IAMV:
    case IL_KAMV:
    case IL_LDA:
      if (has_entries && !gbl.arets)
        ret_op = gen_base_addr_operand(ilix, NULL);
      else
        ret_op = gen_base_addr_operand(ilix, retTy);
      break;
    default:
      /* IL_EXIT */
      if (has_entries && !gbl.arets)
        ret_op = gen_llvm_expr(ilix, NULL);
      else
        ret_op = gen_llvm_expr(ilix, retTy);
    }
    Curr_Instr = gen_instr(I_RET, NULL, NULL, ret_op);
    ad_instr(ilix, Curr_Instr);
  } break;
  case STMT_DECL:
    Curr_Instr = gen_instr(I_DECL, NULL, NULL, gen_llvm_expr(ilix, NULL));
    ad_instr(ilix, Curr_Instr);
    break;
  case STMT_LABEL: {
    sptr = ilix;
    process_sptr(sptr);
    Curr_Instr = gen_instr(I_NONE, NULL, NULL, make_label_op(sptr));
    ad_instr(ilix, Curr_Instr);

    break;
  }
  case STMT_CALL:
    if (getTempMap(ilix))
      return;
    sym = pd_sym = get_call_sptr(ilix);

    if (sym != pd_sym && STYPEG(pd_sym) == ST_PD) {
      switch (PDNUMG(pd_sym)) {
      default:
        break;
      }
    }
    gen_call_expr(ilix, 0, NULL, 0);
    break;
  continue_call:
    /* Add instruction if it hasn't been added already by gen_call_expr(). */
    if (!Instructions || !Curr_Instr->prev)
      ad_instr(ilix, Curr_Instr);
    break;

  case STMT_BR:
    opc = ILI_OPC(ilix);
    if (opc == IL_JMP) /* unconditional jump */
    {
      last_stmt_is_branch = 1;
      sptr = ILI_OPND(ilix, 1);
      {
        /* also in gen_new_landingpad_jump */
        process_sptr(sptr);
        Curr_Instr = gen_instr(I_BR, NULL, NULL, make_target_op(sptr));
        ad_instr(ilix, Curr_Instr);
      }
    } else if (exprjump(opc) || zerojump(opc)) /* cond or zero jump */
    {
      if (exprjump(opc)) /* get sptr pointing to jump label */
      {
        sptr = ILI_OPND(ilix, 4);
        cc = ILI_OPND(ilix, 3);
      } else {
        sptr = ILI_OPND(ilix, 3);
        cc = ILI_OPND(ilix, 2);
      }
      process_sptr(sptr);
      Curr_Instr = make_instr(I_BR);
      tmps = make_tmps();
      Curr_Instr->operands = make_tmp_op(make_int_lltype(1), tmps);

      /* make the condition code */
      switch (opc) {
      case IL_FCJMP:
      case IL_FCJMPZ:
      case IL_DCJMP:
      case IL_DCJMPZ:
        ad_instr(ilix, gen_instr(I_FCMP, tmps, Curr_Instr->operands->ll_type,
                                 gen_llvm_expr(ilix, NULL)));
        break;
      default:
        ad_instr(ilix, gen_instr(I_ICMP, tmps, Curr_Instr->operands->ll_type,
                                 gen_llvm_expr(ilix, NULL)));
      }
      first_label = make_target_op(sptr);
      /* need to make a label for the false condition -- llvm conditional
       * branch requires this step.
       */
      if (next_bih_label) {
        sptr_lab = next_bih_label;
      } else {
        sptr_lab = getlab();
        update_llvm_sym_arrays();
      }
      second_label = make_target_op(sptr_lab);
      first_label->next = second_label;
      Curr_Instr->operands->next = first_label;
      ad_instr(ilix, Curr_Instr);
      /* now add the label instruction */
      if (!next_bih_label)
        make_stmt(STMT_LABEL, sptr_lab, FALSE, 0, ilt);
      DBGTRACE1("#goto statement: jump to label sptr %d", sptr);
    } else if (opc == IL_JMPM || opc == IL_JMPMK) {
      /* unconditional jump */
      Curr_Instr = gen_switch(ilix);
      last_stmt_is_branch = 1;
    } else if (opc == IL_JMPA) {
      int arg1 = ILI_OPND(ilix, 1);
      last_stmt_is_branch = 1;
      op1 = gen_llvm_expr(arg1, make_lltype_from_dtype(DT_CPTR));
      Curr_Instr = gen_instr(I_INDBR, NULL, NULL, op1);
      ad_instr(ilix, Curr_Instr);
    } else {
      /* unknown jump type */
      assert(0, "ilt branch: unexpected branch code", opc, 4);
    }
    break;
  case STMT_SMOVE:
    from_ili = ILI_OPND(ilix, 1);
    to_ili = ILI_OPND(ilix, 2);
    length_ili = ILI_OPND(ilix, 3);
    opnd = ILI_OPND(length_ili, 1);
    assert(DTYPEG(opnd) == DT_CPTR, "make_stmt(): expected DT_CPTR",
           DTYPEG(opnd), 4);
    bytes = CONVAL2G(opnd);
/* IL_SMOVE 3rd opnd has a 4-byte or 8-byte unit, the rest of the
   data are copied using other STORE ili.
   we use it as bytes.
*/
    bytes = bytes * 8;
    assert(bytes, "make_stmt(): expected smove byte size", 0, 4);
    from_nme = ILI_OPND(ilix, 4);
    ts = 8 * size_of(DT_CPTR);
    src_op = gen_llvm_expr(from_ili, make_lltype_from_dtype(DT_CPTR));
    dst_op = gen_llvm_expr(to_ili, make_lltype_from_dtype(DT_CPTR));
    dtype = dt_nme(from_nme);
    if (dtype)
      alignment = align_of(dtype);
    else
      alignment = 1;

    insert_llvm_memcpy(ilix, ts, dst_op, src_op, bytes, alignment, 0);
    break;
  case STMT_SZERO:
    assert(ILI_OPC(ilix) == IL_ARGIR || ILI_OPC(ilix) == IL_DAIR,
           "make_stmt(): expected ARGIR/DAIR for ilix ", ilix, 4);
    length_ili = ILI_OPND(ilix, 1);
    opnd = ILI_OPND(length_ili, 1);
    if (ILI_OPC(ilix) == IL_DAIR)
      to_ili = ILI_OPND(ilix, 3);
    else
      to_ili = ILI_OPND(ilix, 2);
    assert(ILI_OPC(to_ili) == IL_ARGAR || ILI_OPC(to_ili) == IL_DAAR,
           "make_stmt(): expected ARGAR/DAAR for ili ", to_ili, 4);
    to_ili = ILI_OPND(to_ili, 1);
    bytes = CONVAL2G(opnd);
    assert(bytes, "make_stmt(): expected szero byte size", 0, 4);
    ts = 8 * size_of(DT_CPTR);
    dst_op = gen_llvm_expr(to_ili, make_lltype_from_dtype(DT_CPTR));
    insert_llvm_memset(ilix, ts, dst_op, bytes, 0, 1, 0);
    break;
  case STMT_ST:
    /* STORE statement */
    llvm_info.curr_ret_dtype = DT_NONE;
    nme = ILI_OPND(ilix, 3);
    lhs_ili = ILI_OPND(ilix, 2);
    rhs_ili = ILI_OPND(ilix, 1);
    if (!cancel_store(ilix, rhs_ili, lhs_ili)) {
      DTYPE vect_dtype = 0;
      LL_InstrListFlags store_flags = 0;
      LL_Type *int_llt = NULL;
      LL_Type *v4_llt = NULL;
      msz = ILI_MSZ_OF_ST(ilix);
      vect_dtype = ili_get_vect_type(ilix);
#if defined(TARGET_LLVM_ARM)
      if (vect_dtype) {
        store_flags = ldst_instr_flags_from_dtype(vect_dtype);
        if ((DTY(vect_dtype + 2) == 3) && (ILI_OPC(ilix) == IL_VST)) {
          v4_llt = make_lltype_sz4v3_from_dtype(vect_dtype);
        } else {
          switch (zsize_of(vect_dtype)) {
          case 2:
            int_llt = make_lltype_from_dtype(DT_SINT);
            break;
          case 4:
            if (DTY(vect_dtype + 2) != 3)
              int_llt = make_lltype_from_dtype(DT_INT);
            break;
          }
        }
      }
#endif
      if (ILI_OPC(ilix) == IL_STA) {
        LL_Type *ptrTy = make_lltype_from_dtype(DT_CPTR);
        op1 = gen_base_addr_operand(rhs_ili, ptrTy);
        store_flags = ldst_instr_flags_from_dtype(DT_CPTR);
      } else {
        if (vect_dtype) {
          if (v4_llt) {
            op1 = gen_llvm_expr(rhs_ili, v4_llt);
          } else {
            LL_Type *ty = make_lltype_from_dtype(vect_dtype);
            op1 = gen_llvm_expr(rhs_ili, ty);
          }
          if (int_llt)
            op1 = make_bitcast(op1, int_llt);
          /* Clear alignment bits ==> alignment = 1 byte. */
          if (ILI_OPC(ilix) == IL_VSTU)
            store_flags &= ~LDST_LOGALIGN_MASK;
        } else if (is_blockaddr_store(ilix, rhs_ili, lhs_ili)) {
          return;
        } else if (ILI_OPC(ilix) == IL_STSCMPLX) {
          LL_Type *ty = make_lltype_from_dtype(DT_CMPLX);
          op1 = gen_llvm_expr(rhs_ili, ty);
          store_flags = ldst_instr_flags_from_dtype(DT_CMPLX);
        } else if (ILI_OPC(ilix) == IL_STDCMPLX) {
          LL_Type *ty = make_lltype_from_dtype(DT_DCMPLX);
          op1 = gen_llvm_expr(rhs_ili, ty);
          store_flags = ldst_instr_flags_from_dtype(DT_DCMPLX);
        } else {
          LL_Type *ty = make_type_from_msz(msz);
          op1 = gen_llvm_expr(rhs_ili, ty);
          store_flags = ldst_instr_flags_from_dtype(msz_dtype(msz));
        }
      }
      llt_expected = NULL;
      if ((ILI_OPC(ilix) == IL_STA) || (op1->ll_type->data_type == LL_STRUCT)) {
        llt_expected = make_ptr_lltype(op1->ll_type);
      }
      if (vect_dtype) {
        if (v4_llt)
          llt_expected = make_ptr_lltype(v4_llt);
        else if (int_llt)
          llt_expected = make_ptr_lltype(int_llt);
        else
          llt_expected = make_ptr_lltype(make_lltype_from_dtype(vect_dtype));
        store_op = gen_address_operand(lhs_ili, nme, false, llt_expected, -1);
      } else {
        store_op = gen_address_operand(lhs_ili, nme, false, llt_expected, msz);
      }
      if ((store_op->ll_type->data_type == LL_PTR) &&
          ll_type_int_bits(store_op->ll_type->sub_types[0]) &&
          ll_type_int_bits(op1->ll_type) &&
          (ll_type_bytes(store_op->ll_type->sub_types[0]) !=
           ll_type_bytes(op1->ll_type))) {
        /* Need to add a conversion here */
        op1 = convert_int_size(ilix, op1, store_op->ll_type->sub_types[0]);
      }

      Curr_Instr = make_instr(I_STORE);
      if (nme == NME_VOL)
        store_flags |= VOLATILE_FLAG;
      if (IL_HAS_FENCE(ILI_OPC(ilix)))
        store_flags |= ll_instr_flags_for_memory_order_and_scope(ilix);
      Curr_Instr->flags = store_flags;
      Curr_Instr->operands = op1;
      DBGTRACE2("#store_op %p, op1 %p\n", store_op, op1)
      if (deletable)
        Curr_Instr->flags |= DELETABLE;
      Curr_Instr->operands->next = store_op;
      ad_instr(ilix, Curr_Instr);
    }
    break;
  default:
    assert(0, "make_stmt(): unknown statment type", stmt_type, 4);
  }
end_make_stmt:;

  DBGTRACEOUT("")
} /* make_stmt */

OPERAND *
gen_va_start(int ilix)
{
  OPERAND *call_op, *arg_op;
  char *va_start_name, *gname;
  int arg;
  static LOGICAL va_start_defined = FALSE;
  EXFUNC_LIST *exfunc;
  LL_Type *expected_type;

  DBGTRACEIN1(" called with ilix %d\n", ilix)

  call_op = make_operand();
  call_op->ot_type = OT_CALL;
  call_op->ll_type = make_void_lltype();
  va_start_name = (char *)getitem(LLVM_LONGTERM_AREA, 17);
  sprintf(va_start_name, "@llvm.va_start");
  call_op->string = va_start_name;
  arg = ILI_OPND(ilix, 2);
  assert(arg && is_argili_opcode(ILI_OPC(arg)), "gen_va_start(): bad argument",
         arg, 4);
  expected_type = make_lltype_from_dtype(DT_CPTR);
  arg_op = gen_llvm_expr(ILI_OPND(arg, 1), expected_type);
  call_op->next = arg_op;
  /* add prototype if needed */
  if (!va_start_defined) {
    va_start_defined = TRUE;
    gname = (char *)getitem(LLVM_LONGTERM_AREA, strlen(va_start_name) + 35);
    sprintf(gname, "declare void %s(i8*)", va_start_name);
    exfunc = (EXFUNC_LIST *)getitem(LLVM_LONGTERM_AREA, sizeof(EXFUNC_LIST));
    memset(exfunc, 0, sizeof(EXFUNC_LIST));
    exfunc->func_def = gname;
    exfunc->flags |= EXF_INTRINSIC;
    add_external_function_declaration(exfunc);
  }

  DBGTRACEOUT1(" returns operand %p", call_op)

  return call_op;
} /* gen_va_start */

/* Create a variable of type 'dtype'
 * This is a convenience routine only used by gen_va_arg.
 * Returns an sptr to the newly created instance.
 *
 * align: Log of alignment (in bytes)
 */
static int
make_va_arg_tmp(int ilix, int dtype, int align)
{
  int tmp;
  char tmp_name[32];

  NEWSYM(tmp);
  snprintf(tmp_name, sizeof(tmp_name), ".vargtmp.%d", ilix);
  NMPTRP(tmp, putsname(tmp_name, strlen(tmp_name)));
  STYPEP(tmp, ST_STRUCT);
  SCP(tmp, SC_AUTO);
  DTYPEP(tmp, dtype);
  PDALNP(tmp, align);
  return tmp;
}

/*
 * Expand an IL_VA_ARG instruction: VA_ARG arlnk dtype
 *
 * The first argument is a pointer to the va_list, the second is the dtype of
 * the argument to be extracted. Produce a pointer where the argument can be
 * loaded.
 *
 * There are two versions of this function (one for x86-64 and one for
 * non-x86-64).
 *
 */
static OPERAND *
gen_va_arg(int ilix)
{
  /*
   * va_arg for other targets: va_list is an i8**, arguments are contiguous in
   * memory.
   *
   * %ap_cast = bitcast i8** %ap to uintptr_t*
   * %addr = load uintptr_t* %ap_cast
   * if (arg_align > reg_size)
   *   %addr = round-up-to-align(arg_align)
   *
   * %next = getelementptr argtype* %addr, 1
   * store argtype* %next, %ap_cast
   * return argtype %ptr
   */
  int tmp;
  OPERAND *addr_op, *result_op, *next_op;
  const int ap_ili = ILI_OPND(ilix, 1);
  const int arg_dtype = ILI_OPND(ilix, 2);
  const unsigned reg_size = size_of(DT_CPTR);
  unsigned arg_align = alignment(arg_dtype) + 1;
  unsigned arg_size = size_of(arg_dtype);
  LL_Type *uintptr_type = make_int_lltype(8 * reg_size);
  OPERAND *ap_cast = gen_llvm_expr(ap_ili, make_ptr_lltype(uintptr_type));
  const unsigned flags = ldst_instr_flags_from_dtype(DT_CPTR);

  addr_op = gen_load(ap_cast, uintptr_type, flags);

  switch (DTY(arg_dtype)) {
    /* These types are (needlessly) aligned to 16 bytes when laying out
     * structures, but treated as pairs or quadruplets of doubles in the
     * context of argument passing.
     */
    arg_align = 8;
    break;
  }

  if (arg_align > reg_size) {
    /* This argument has alignment greater than the pointer register size.
     * We need to dynamically align the address. */
    /* addr_op += arg_align-1 */
    addr_op->next = make_constval_op(uintptr_type, arg_align - 1, 0);
    addr_op =
        ad_csed_instr(I_ADD, 0, uintptr_type, addr_op, NOUNSIGNEDWRAP, FALSE);
    /* addr_op &= -arg_align */
    addr_op->next = make_constval_op(uintptr_type, -arg_align, -1);
    addr_op = ad_csed_instr(I_AND, 0, uintptr_type, addr_op, 0, FALSE);
  }
  result_op = convert_int_to_ptr(
      addr_op, make_ptr_lltype(make_lltype_from_dtype(arg_dtype)));

#ifdef TARGET_POWER
  /* POWER ABI: va_args are passed in the parameter save region of the stack.
   * The caller is responsible for setting up the stack space for this (LLVM
   * will do this for us).
   *
   * The special case here is for 'float complex' where each complex has
   * two components, treated as the same type and alignment as the first
   * component (the real component of the complex value).
   *
   * The reason for this special case is because we need to treat the
   * components of the complex as coming from two separate float arguments.
   * These are stored into a temp complex {float, float} and a pointer to that
   * temp is returned.
   */
  if (arg_dtype == DT_CMPLX) {
    LL_Type *llt_float = make_lltype_from_dtype(DT_FLOAT);
    LL_Type *llt_float_ptr = make_ptr_lltype(llt_float);
    LL_Type *llt_cptr = make_lltype_from_dtype(DT_CPTR);
    const unsigned flt_flags = ldst_instr_flags_from_dtype(DT_FLOAT);
    OPERAND *tmp_op, *cmplx_op, *val_op;

    /* Pointer to temp real */
    tmp = make_va_arg_tmp(ilix, arg_dtype, 0);
    cmplx_op = tmp_op = make_var_op(tmp); /* points to {float,float} */
    tmp_op = make_bitcast(tmp_op, llt_cptr);
    tmp_op = gen_gep_index(tmp_op, llt_cptr, 0);
    tmp_op = make_bitcast(tmp_op, llt_float_ptr);

    /* Pointer to actual real */
    result_op = make_bitcast(result_op, llt_cptr);
    result_op = gen_gep_index(result_op, llt_cptr, 0);
    result_op = make_bitcast(result_op, llt_float_ptr);
    val_op = gen_load(result_op, llt_float, flt_flags);
    make_store(val_op, tmp_op, flt_flags);

    /* Now for imaginary (must skip 2 * DT_FLOAT bytes) */
    tmp_op = make_bitcast(tmp_op, llt_cptr);
    tmp_op = gen_gep_index(tmp_op, llt_cptr, size_of(DT_FLOAT));
    tmp_op = make_bitcast(tmp_op, llt_float_ptr);
    result_op = make_bitcast(result_op, llt_cptr);
    result_op = gen_gep_index(result_op, llt_cptr, size_of(DT_FLOAT) * 2);
    result_op = make_bitcast(result_op, llt_float_ptr);
    val_op = gen_load(result_op, llt_float, flt_flags);
    make_store(val_op, tmp_op, flt_flags);

    result_op = gen_copy_op(cmplx_op);
    arg_size *= 2; /* Skip two floats instead of one float */
  }
#endif /* TARGET_POWER */

  /* Compute the address of the next argument.
   * Round up to a multiple of reg_size.
   */
  arg_size = (arg_size + reg_size - 1) & -reg_size;
  addr_op = gen_copy_op(addr_op);
  addr_op->next = make_constval_op(uintptr_type, arg_size, 0);
  next_op =
      ad_csed_instr(I_ADD, 0, uintptr_type, addr_op, NOUNSIGNEDWRAP, FALSE);
  make_store(next_op, gen_copy_op(ap_cast), flags);

  return result_op;
}

OPERAND *
gen_va_end(int ilix)
{
  OPERAND *call_op, *arg_op;
  char *va_end_name, *gname;
  int arg;
  static LOGICAL va_end_defined = FALSE;
  EXFUNC_LIST *exfunc;
  LL_Type *expected_type;

  DBGTRACEIN1(" called with ilix %d\n", ilix)

  call_op = make_operand();
  call_op->ot_type = OT_CALL;
  call_op->ll_type = make_void_lltype();
  va_end_name = (char *)getitem(LLVM_LONGTERM_AREA, 17);
  sprintf(va_end_name, "@llvm.va_end");
  call_op->string = va_end_name;
  arg = ILI_OPND(ilix, 2);
  assert(arg && is_argili_opcode(ILI_OPC(arg)), "gen_va_end(): bad argument",
         arg, 4);
  expected_type = make_lltype_from_dtype(DT_CPTR);
  arg_op = gen_llvm_expr(ILI_OPND(arg, 1), expected_type);
  call_op->next = arg_op;
  /* add prototype if needed */
  if (!va_end_defined) {
    va_end_defined = TRUE;
    gname = (char *)getitem(LLVM_LONGTERM_AREA, strlen(va_end_name) + 35);
    sprintf(gname, "declare void %s(i8*)", va_end_name);
    exfunc = (EXFUNC_LIST *)getitem(LLVM_LONGTERM_AREA, sizeof(EXFUNC_LIST));
    memset(exfunc, 0, sizeof(EXFUNC_LIST));
    exfunc->func_def = gname;
    exfunc->flags |= EXF_INTRINSIC;
    add_external_function_declaration(exfunc);
  }

  DBGTRACEOUT1(" returns operand %p", call_op)

  return call_op;
} /* gen_va_end */

OPERAND *
gen_call_to_builtin(int ilix, char *fname, OPERAND *params,
                    LL_Type *return_ll_type, INSTR_LIST *Call_Instr, int i_name,
                    LOGICAL prefix_declaration)
{
  OPERAND *call_op, *operand = NULL;
  char *intrinsic_name, *gname;
  static char buf[MAXIDLEN];
  INSTR_LIST *Curr_Instr;

  DBGTRACEIN1(" for ilix %d\n", ilix)

  intrinsic_name = (char *)getitem(LLVM_LONGTERM_AREA, strlen(fname) + 1);
  strcpy(intrinsic_name, fname);
  operand = make_tmp_op(return_ll_type, make_tmps());
  if (!Call_Instr)
    Curr_Instr = make_instr(i_name);
  else
    Curr_Instr = Call_Instr;
  Curr_Instr->flags |= CALL_INTRINSIC_FLAG;
  Curr_Instr->tmps = operand->tmps; /* result operand */
  Curr_Instr->tmps->info.idef = Curr_Instr;
  Curr_Instr->ll_type = return_ll_type;
  Curr_Instr->operands =
      get_intrinsic_call_ops(intrinsic_name, return_ll_type, params);
  if (!Call_Instr)
    ad_instr(ilix, Curr_Instr);

  DBGTRACEOUT("")

  return operand;
} /* gen_call_to_builtin */

static const char *
get_atomicrmw_opname(LL_InstrListFlags instr_flags)
{
  switch (instr_flags & ATOMIC_RMW_OP_FLAGS) {
  case ATOMIC_SUB_FLAG:
    return "sub";
  case ATOMIC_ADD_FLAG:
    return "add";
  case ATOMIC_XCHG_FLAG:
    return "xchg";
  case ATOMIC_UMIN_FLAG:
    return "umin";
  case ATOMIC_MIN_FLAG:
    return "min";
  case ATOMIC_UMAX_FLAG:
    return "umax";
  case ATOMIC_MAX_FLAG:
    return "max";
  case ATOMIC_AND_FLAG:
    return "and";
  case ATOMIC_OR_FLAG:
    return "or";
  case ATOMIC_XOR_FLAG:
    return "xor";
  default:
    interr("Unexpected atomic rmw flag: ", instr_flags & ATOMIC_RMW_OP_FLAGS,
           3);
    return "";
  }
}

static const char *
get_atomic_memory_order_name(int instr_flags)
{
  switch (instr_flags & ATOMIC_MEM_ORD_FLAGS) {
  case ATOMIC_MONOTONIC_FLAG:
    return "monotonic";
  case ATOMIC_ACQUIRE_FLAG:
    return "acquire";
  case ATOMIC_RELEASE_FLAG:
    return "release";
  case ATOMIC_ACQ_REL_FLAG:
    return "acq_rel";
  case ATOMIC_SEQ_CST_FLAG:
    return "seq_cst";
  default:
    interr("Unexpected atomic mem ord flag: ",
           instr_flags & ATOMIC_MEM_ORD_FLAGS, 3);
    return "";
  }
}

static OPERAND *
gen_llvm_atomicrmw_instruction(int ilix, int pdnum, OPERAND *params,
                               DTYPE return_dtype)
{
  return NULL;
}

static OPERAND *
gen_call_llvm_intrinsic(const char *fname, OPERAND *params,
                        LL_Type *return_ll_type, INSTR_LIST *Call_Instr,
                        int i_name)
{
  static char buf[MAXIDLEN];

  sprintf(buf, "@llvm.%s", fname);
  return gen_call_to_builtin(0, buf, params, return_ll_type, Call_Instr, i_name,
                             FALSE);
}

static OPERAND *
gen_call_pgocl_intrinsic(char *fname, OPERAND *params, LL_Type *return_ll_type,
                         INSTR_LIST *Call_Instr, int i_name)
{
  static char buf[MAXIDLEN];

  sprintf(buf, "@%s%s", ENTOCL_PREFIX, fname);
  return gen_call_to_builtin(0, buf, params, return_ll_type, Call_Instr, i_name,
                             TRUE);
}

void
insert_llvm_memset(int ilix, int size, OPERAND *dest_op, int len, int value,
                   int align, int is_volatile)
{
  EXFUNC_LIST *exfunc;
  OPERAND *call_op;
  static LOGICAL memset_defined = FALSE;
  char *memset_name, *gname;
  INSTR_LIST *Curr_Instr;

  DBGTRACEIN("")

  memset_name = (char *)getitem(LLVM_LONGTERM_AREA, 22);
  sprintf(memset_name, "@llvm.memset.p0i8.i%d", size);
  Curr_Instr = make_instr(I_CALL);
  Curr_Instr->flags |= CALL_INTRINSIC_FLAG;
  Curr_Instr->operands = call_op = make_operand();
  call_op->ot_type = OT_CALL;
  call_op->ll_type = make_void_lltype();
  Curr_Instr->ll_type = call_op->ll_type;
  call_op->string = memset_name;
  call_op->next = dest_op;

  dest_op->next = make_constval_op(make_int_lltype(8), value, 0);
  /* length in bytes of memset */
  dest_op->next->next = make_constval_op(make_int_lltype(size), len, 0);
  /* alignment */
  dest_op->next->next->next = make_constval32_op(align);
  dest_op->next->next->next->next =
      make_constval_op(make_int_lltype(1), is_volatile, 0);
  ad_instr(ilix, Curr_Instr);
  /* add global define of llvm.memset to external function list, if needed */
  if (!memset_defined) {
    memset_defined = TRUE;
    gname = (char *)getitem(LLVM_LONGTERM_AREA, strlen(memset_name) + 45);
    sprintf(gname, "declare void %s(i8*, i8, i%d, i32, i1)", memset_name, size);
    exfunc = (EXFUNC_LIST *)getitem(LLVM_LONGTERM_AREA, sizeof(EXFUNC_LIST));
    memset(exfunc, 0, sizeof(EXFUNC_LIST));
    exfunc->func_def = gname;
    exfunc->flags |= EXF_INTRINSIC;
    add_external_function_declaration(exfunc);
  }
  DBGTRACEOUT("")
} /* insert_llvm_memset */

void
insert_llvm_memcpy(int ilix, int size, OPERAND *dest_op, OPERAND *src_op,
                   int len, int align, int is_volatile)
{
  EXFUNC_LIST *exfunc;
  OPERAND *call_op;
  static LOGICAL memcpy_defined = FALSE;
  char *memcpy_name, *gname;
  INSTR_LIST *Curr_Instr;

  DBGTRACEIN("")

  memcpy_name = (char *)getitem(LLVM_LONGTERM_AREA, 27);
  sprintf(memcpy_name, "@llvm.memcpy.p0i8.p0i8.i%d", size);
  Curr_Instr = make_instr(I_CALL);
  Curr_Instr->flags |= CALL_INTRINSIC_FLAG;
  Curr_Instr->operands = call_op = make_operand();
  call_op->ot_type = OT_CALL;
  call_op->ll_type = make_void_lltype();
  Curr_Instr->ll_type = call_op->ll_type;
  call_op->string = memcpy_name;
  call_op->next = dest_op;
  dest_op->next = src_op;
  /* length in bytes of memcpy */
  src_op->next = make_constval_op(make_int_lltype(size), len, 0);
  src_op->next->next = make_constval32_op(align); /* alignment */
  src_op->next->next->next =
      make_constval_op(make_int_lltype(1), is_volatile, 0);
  ad_instr(ilix, Curr_Instr);
  /* add global define of llvm.memcpy to external function list, if needed */
  if (!memcpy_defined) {
    memcpy_defined = TRUE;
    gname = (char *)getitem(LLVM_LONGTERM_AREA, strlen(memcpy_name) + 49);
    sprintf(gname, "declare void %s(i8*, i8*, i%d, i32, i1)", memcpy_name,
            size);
    exfunc = (EXFUNC_LIST *)getitem(LLVM_LONGTERM_AREA, sizeof(EXFUNC_LIST));
    memset(exfunc, 0, sizeof(EXFUNC_LIST));
    exfunc->func_def = gname;
    exfunc->flags |= EXF_INTRINSIC;
    add_external_function_declaration(exfunc);
  }

  DBGTRACEOUT("")
} /* insert_llvm_memcpy */

/**
   \brief Insert <tt>@llvm.dbg.declare</tt> call for debug
   \param mdnode  metadata node
   \param sptr    symbol
   \param llTy    preferred type of \p sptr or \c NULL
 */
static void
insert_llvm_dbg_declare(LL_MDRef mdnode, int sptr, LL_Type *llTy,
                        OPERAND *exprMDOp)
{
  EXFUNC_LIST *exfunc;
  OPERAND *call_op;
  static bool dbg_declare_defined = false;
  char *gname;
  INSTR_LIST *Curr_Instr;

  Curr_Instr = make_instr(I_CALL);
  Curr_Instr->flags |= CALL_INTRINSIC_FLAG;
  Curr_Instr->operands = call_op = make_operand();
  Curr_Instr->dbg_line_op =
      lldbg_get_var_line(cpu_llvm_module->debug_info, sptr);
  call_op->ot_type = OT_CALL;
  call_op->ll_type = make_void_lltype();
  Curr_Instr->ll_type = call_op->ll_type;
  call_op->string = "@llvm.dbg.declare";

  call_op->next = make_metadata_wrapper_op(sptr, llTy);
  call_op->next->next = make_mdref_op(mdnode);
  if (ll_feature_dbg_declare_needs_expression_md(&cpu_llvm_module->ir)) {
    if (exprMDOp) {
      call_op->next->next->next = exprMDOp;
    } else {
      LL_DebugInfo *di = cpu_llvm_module->debug_info;
      LL_MDRef md = lldbg_emit_empty_expression_mdnode(di);
      call_op->next->next->next = make_mdref_op(md);
    }
  }

  ad_instr(0, Curr_Instr);
  /* add global define of llvm.dbg.declare to external function list, if needed
   */
  if (!dbg_declare_defined) {
    dbg_declare_defined = true;
    if (ll_feature_dbg_declare_needs_expression_md(&cpu_llvm_module->ir)) {
      gname = "declare void @llvm.dbg.declare(metadata, metadata, metadata)";
    } else {
      gname = "declare void @llvm.dbg.declare(metadata, metadata)";
    }
    exfunc = (EXFUNC_LIST *)getitem(LLVM_LONGTERM_AREA, sizeof(EXFUNC_LIST));
    memset(exfunc, 0, sizeof(EXFUNC_LIST));
    exfunc->func_def = gname;
    exfunc->flags |= EXF_INTRINSIC;
    add_external_function_declaration(exfunc);
  }
}

char *
match_names(MATCH_Kind match_val)
{
  char *tt;

  switch (match_val) {
  case MATCH_NO:
    return "MATCH_NO";
  case MATCH_OK:
    return "MATCH_OK";
  case MATCH_MEM:
    return "MATCH_MEM";
  default:
    asrt(match_val > 1);
    tt = (char *)getitem(LLVM_LONGTERM_AREA, 10);
    sprintf(tt, "MATCH_%d", match_val);
    return tt;
  }
} /* match_names */

static OPERAND *
gen_const_expr(int ilix, LL_Type *expected_type)
{
  OPERAND *operand;
  int sptr = ILI_OPND(ilix, 1);

  /* Generate null pointer operands when requested.
   * Accept both IL_ICON and IL_KCON nulls. */
  if (expected_type && (expected_type->data_type == LL_PTR) &&
      CONVAL2G(sptr) == 0 && CONVAL1G(sptr) == 0)
    return make_null_op(expected_type);

  operand = make_operand();
  operand->ot_type = OT_CONSTSPTR;

  switch (ILI_OPC(ilix)) {
  case IL_KCON:
    operand->ll_type = make_lltype_from_dtype(DT_INT8);
    operand->val.sptr = sptr;
    break;
  case IL_ICON:
    if (expected_type && ll_type_int_bits(expected_type) &&
        (ll_type_int_bits(expected_type) < 32)) {
      operand->ot_type = OT_CONSTVAL;
      operand->val.conval[0] = CONVAL2G(sptr);
      operand->val.conval[1] = 0;
      assert(ll_type_int_bits(expected_type), "expected int",
             expected_type->data_type, ERR_Fatal);
      operand->ll_type = expected_type;
    } else {
      operand->ll_type = make_lltype_from_dtype(DT_INT);
      operand->val.sptr = sptr;
    }
    break;
  case IL_FCON:
    operand->ll_type = make_lltype_from_dtype(DT_FLOAT);
    operand->val.sptr = sptr;
    break;
  case IL_DCON:
    operand->ll_type = make_lltype_from_dtype(DT_DBLE);
    operand->val.sptr = sptr;
    break;
  case IL_VCON:
    operand->ll_type = make_lltype_from_sptr(sptr);
    operand->val.sptr = sptr;
    break;
  case IL_SCMPLXCON:
  case IL_DCMPLXCON:
    operand->ll_type = make_lltype_from_dtype(DTYPEG(sptr));
    operand->val.sptr = sptr;
    break;
  default:
    interr("Unknown gen_const_expr opcode", ILI_OPC(ilix), 4);
  }
  return operand;
} /* gen_const_expr */

static OPERAND *
gen_unary_expr(int ilix, int itype)
{
  int op_ili;
  ILI_OP opc = ILI_OPC(ilix);
  OPERAND *operand;
  LL_Type *opc_type, *instr_type;
  TMPS *new_tmps;

  DBGTRACEIN2(" ilix: %d(%s) \n", ilix, IL_NAME(opc))

  instr_type = opc_type = make_type_from_opc(opc);
  assert(opc_type != NULL, "gen_unary_expr(): no type information", 0, 4);

  op_ili = ILI_OPND(ilix, 1);

  switch (opc) {
  case IL_DFIXUK:
  case IL_DFIXU:
  case IL_DFIX:
  case IL_DFIXK:
  case IL_SNGL:
    opc_type = make_lltype_from_dtype(DT_DBLE);
    break;
  case IL_DBLE:
  case IL_UFIX:
  case IL_FIX:
  case IL_FIXK:
  case IL_FIXUK:
    opc_type = make_lltype_from_dtype(DT_FLOAT);
    break;
  case IL_FLOAT:
  case IL_FLOATU:
  case IL_DFLOATU:
  case IL_DFLOAT:
  case IL_ALLOC:
    opc_type = make_lltype_from_dtype(DT_INT);
    break;
  case IL_FLOATK:
  case IL_FLOATUK:
  case IL_DFLOATUK:
  case IL_DFLOATK:
    opc_type = make_lltype_from_dtype(DT_INT8);
    break;
  default:;
  }

  DBGTRACE2("#generating unary operand, op_ili: %d(%s)", op_ili,
            IL_NAME(ILI_OPC(op_ili)))

  /* now make the new unary expression */
  operand = ad_csed_instr(itype, ilix, instr_type,
                          gen_llvm_expr(op_ili, opc_type), 0, TRUE);

  DBGTRACEOUT1(" return operand %p\n", operand)

  return operand;
} /* gen_unary_expr */

static OPERAND *
gen_abs_expr(int ilix)
{
  int lhs_ili;
  ILI_OP opc = ILI_OPC(ilix);
  OPERAND *operand, *cmp_op, *op1, *op2, *zero_op, *comp_operands;
  LL_Type *opc_type, *bool_type;
  int cc_itype, cc_val;
  INT tmp[2];
  union {
    double d;
    INT tmp[2];
  } dtmp;
  float f;
  double d;
  INSTR_LIST *Curr_Instr;

  DBGTRACEIN2(" ilix: %d(%s) \n", ilix, IL_NAME(opc))

  lhs_ili = ILI_OPND(ilix, 1);
  opc_type = make_type_from_opc(opc);
  assert(opc_type, "gen_abs_expr(): no type information", 0, 4);
  operand = make_tmp_op(opc_type, make_tmps());
  op1 = gen_llvm_expr(lhs_ili, operand->ll_type);
  /* now make the new binary expression */
  Curr_Instr = gen_instr(I_SELECT, operand->tmps, operand->ll_type, NULL);
  bool_type = make_int_lltype(1);
  switch (ILI_OPC(ilix)) {
  case IL_IABS:
    cc_itype = I_ICMP;
    cc_val = convert_to_llvm_cc(CC_LT, CMP_INT);
    op2 = gen_llvm_expr(ad1ili(IL_INEG, lhs_ili), operand->ll_type);
    zero_op = gen_llvm_expr(ad_icon(0), operand->ll_type);
    break;
  case IL_KABS:
    cc_itype = I_ICMP;
    cc_val = convert_to_llvm_cc(CC_LT, CMP_INT);
    op2 = gen_llvm_expr(ad1ili(IL_KNEG, lhs_ili), operand->ll_type);
    zero_op = gen_llvm_expr(ad_kconi(0), operand->ll_type);
    break;
  case IL_FABS:
    cc_itype = I_FCMP;
    cc_val = convert_to_llvm_cc(CC_LT, CMP_FLT);
    op2 = gen_llvm_expr(ad1ili(IL_FNEG, lhs_ili), operand->ll_type);
    tmp[0] = 0;
    f = 0.0;
    mftof(f, tmp[1]);
    zero_op =
        gen_llvm_expr(ad1ili(IL_FCON, getcon(tmp, DT_FLOAT)), operand->ll_type);
    break;
  case IL_DABS:
    cc_itype = I_FCMP;
    cc_val = convert_to_llvm_cc(CC_LT, CMP_FLT);
    op2 = gen_llvm_expr(ad1ili(IL_DNEG, lhs_ili), operand->ll_type);
    d = 0.0;
    xmdtod(d, dtmp.tmp);
    zero_op = gen_llvm_expr(ad1ili(IL_DCON, getcon(dtmp.tmp, DT_DBLE)),
                            operand->ll_type);
    break;
  }
  cmp_op = make_tmp_op(bool_type, make_tmps());

  Curr_Instr->operands = cmp_op;
  Curr_Instr->operands->next = op2;
  Curr_Instr->operands->next->next = op1;

  comp_operands = make_operand();
  comp_operands->ot_type = OT_CC;
  comp_operands->val.cc = cc_val;
  comp_operands->ll_type = bool_type;
  comp_operands->next = gen_copy_op(op1);
  comp_operands->next->next = gen_copy_op(zero_op);

  ad_instr(ilix,
           gen_instr(cc_itype, cmp_op->tmps, cmp_op->ll_type, comp_operands));

  ad_instr(ilix, Curr_Instr);

  DBGTRACEOUT1(" returns operand %p", operand)

  return operand;
}

static OPERAND *
gen_minmax_expr(int ilix, OPERAND *op1, OPERAND *op2)
{
  ILI_OP opc = ILI_OPC(ilix);
  OPERAND *operand, *cmp_op;
  LL_Type *llt, *bool_type;
  int cc_itype, cc_val, cc_ctype = CC_GT;
  int vect_dtype;
  INSTR_LIST *Curr_Instr;

  DBGTRACEIN2(" ilix: %d(%s)", ilix, IL_NAME(opc))

  operand = make_tmp_op(NULL, make_tmps());
  vect_dtype = ili_get_vect_type(ilix);
  if (vect_dtype) {
    llt = make_lltype_from_dtype(vect_dtype);
    operand->ll_type = llt->sub_types[0];
  } else
  {
    llt = make_type_from_opc(opc);
    operand->ll_type = llt;
  }

  /* now make the new binary expression */
  bool_type = make_int_lltype(1);
  switch (opc) {
  case IL_UIMIN:
  case IL_UKMIN:
    cc_itype = I_ICMP;
    cc_val = convert_to_llvm_cc(CC_LT, CMP_INT | CMP_USG);
    break;
  case IL_IMIN:
  case IL_KMIN:
    cc_itype = I_ICMP;
    cc_val = convert_to_llvm_cc(CC_LT, CMP_INT);
    break;
  case IL_FMIN:
  case IL_DMIN:
    cc_itype = I_FCMP;
    cc_val = convert_to_llvm_cc(CC_LT, CMP_FLT);
    break;
  case IL_UIMAX:
  case IL_UKMAX:
    cc_itype = I_ICMP;
    cc_val = convert_to_llvm_cc(CC_GT, CMP_INT | CMP_USG);
    break;
  case IL_IMAX:
  case IL_KMAX:
    cc_itype = I_ICMP;
    cc_val = convert_to_llvm_cc(CC_GT, CMP_INT);
    break;
  case IL_FMAX:
  case IL_DMAX:
    cc_itype = I_FCMP;
    cc_val = convert_to_llvm_cc(CC_GT, CMP_FLT);
    break;
  case IL_VMIN:
    cc_ctype = CC_LT;
  /* Fall through */
  case IL_VMAX:
    switch (DTY(DTY(vect_dtype + 1))) {
    case TY_FLOAT:
    case TY_DBLE:
      cc_itype = I_FCMP;
      cc_val = convert_to_llvm_cc(cc_ctype, CMP_FLT);
      break;
    default:
      cc_itype = I_ICMP;
      if (DT_ISUNSIGNED(DTY(vect_dtype + 1)))
        cc_val = convert_to_llvm_cc(cc_ctype, CMP_INT | CMP_USG);
      else
        cc_val = convert_to_llvm_cc(cc_ctype, CMP_INT);
      break;
    }
    break;
  default:; /*TODO: can this happen? */
  }
  cmp_op = make_tmp_op(bool_type, make_tmps());

  Curr_Instr = gen_instr(cc_itype, cmp_op->tmps, bool_type, NULL);
  Curr_Instr->operands = make_operand();
  Curr_Instr->operands->ot_type = OT_CC;
  Curr_Instr->operands->val.cc = cc_val;
  Curr_Instr->operands->ll_type = llt; /* opc type computed at top of routine */
  Curr_Instr->operands->next = op1;
  Curr_Instr->operands->next->next = op2;
  ad_instr(ilix, Curr_Instr);

  cmp_op->next = gen_copy_op(op1);
  cmp_op->next->next = gen_copy_op(op2);
  Curr_Instr = gen_instr(I_SELECT, operand->tmps, operand->ll_type, cmp_op);
  ad_instr(ilix, Curr_Instr);

  DBGTRACEOUT1(" returns operand %p", operand)

  return operand;
}

static OPERAND *
gen_select_expr(int ilix)
{
  int cmp_ili, lhs_ili, rhs_ili;
  ILI_OP opc = ILI_OPC(ilix);
  OPERAND *operand;
  LL_Type *opc_type, *bool_type;
  INSTR_LIST *Curr_Instr;

  DBGTRACEIN2(" ilix: %d(%s) \n", ilix, IL_NAME(opc))

  cmp_ili = ILI_OPND(ilix, 1);
  lhs_ili = ILI_OPND(ilix, 3);
  rhs_ili = ILI_OPND(ilix, 2);
  opc_type = make_type_from_opc(opc);
  assert(opc_type, "gen_select_expr(): no type information", 0, 4);
  operand = make_tmp_op(opc_type, make_tmps());

  /* now make the new binary expression */
  Curr_Instr = gen_instr(I_SELECT, operand->tmps, operand->ll_type, NULL);

  DBGTRACE2("#generating comparison operand, cmp_ili: %d(%s)", cmp_ili,
            IL_NAME(ILI_OPC(cmp_ili)))

  bool_type = make_int_lltype(1);
  if (IEEE_CMP)
    float_jmp = TRUE;
  Curr_Instr->operands = gen_llvm_expr(cmp_ili, bool_type);
  float_jmp = FALSE;

  DBGTRACE2("#generating second operand, lhs_ili: %d(%s)", lhs_ili,
            IL_NAME(ILI_OPC(lhs_ili)))

  Curr_Instr->operands->next = gen_llvm_expr(lhs_ili, operand->ll_type);

  DBGTRACE2("#generating third operand, rhs_ili: %d(%s)", rhs_ili,
            IL_NAME(ILI_OPC(rhs_ili)))

  Curr_Instr->operands->next->next = gen_llvm_expr(rhs_ili, operand->ll_type);
  ad_instr(ilix, Curr_Instr);

  DBGTRACEOUT1(" returns operand %p", operand)

  return operand;
}

static int
get_vconi(int dtype, INT value)
{
  INT v[TY_VECT_MAXLEN];
  int i;

  for (i = 0; i < DTY(dtype + 2); i++) {
    v[i] = value;
  }
  return get_vcon(v, dtype);
}

static int
get_vcon0_n(int dtype, int start, int N)
{
  INT v[TY_VECT_MAXLEN];
  int i;

  for (i = 0; i < N; i++) {
    v[i] = start + i;
  }
  return get_vcon(v, dtype);
}

static OPERAND *
gen_imask(int sptr)
{
  OPERAND *operand;
  int vdtype = DTYPEG(sptr);

  operand = make_operand();
  operand->ot_type = OT_CONSTSPTR;
  operand->ll_type = make_vtype(DT_INT, DTY(vdtype + 2));
  operand->val.sptr = sptr;

  return operand;
}

/*
 * Here we generate LLVM instruction to insert scalar operand <sop> at index
 * <idx>
 * into vector operand <vop>
 * So in LLVM it will tranlate into:
 * %0 = insertelement <<sz> x <ty>> <vop>, <sop>, i32 <idx>
 */
static OPERAND *
gen_insert_vector(OPERAND *vop, OPERAND *sop, int idx)
{
  OPERAND *operand;
  INSTR_LIST *Curr_Instr;

  operand = make_tmp_op(vop->ll_type, make_tmps());

  Curr_Instr = gen_instr(I_INSELE, operand->tmps, operand->ll_type, vop);
  vop->next = sop;
  vop->next->next = make_constval32_op(idx);
  ad_instr(0, Curr_Instr);

  return operand;
}

/*
 * Here we generate LLVM instruction to extract a scalar at a index <idx>
 * from vector operand <vop>
 * So in LLVM it will tranlate into:
 * %0 = extractelement <<sz> x <ty>> <vop>, i32 <idx>
 */
static OPERAND *
gen_extract_vector(OPERAND *vop, int idx)
{
  OPERAND *operand;
  INSTR_LIST *Curr_Instr;

  assert(vop->ll_type->data_type == LL_VECTOR,
         "gen_extract_vector(): vector type expected for operand\n",
         vop->ll_type->data_type, 4);
  operand = make_tmp_op(vop->ll_type->sub_types[0], make_tmps());

  Curr_Instr = gen_instr(I_EXTELE, operand->tmps, operand->ll_type, vop);
  vop->next = make_constval32_op(idx);
  ad_instr(0, Curr_Instr);

  return operand;
}

/**
   \brief Create a new vector

   The new vactor will have the same type as \p vop with size \p new_size and
   filed with values from \p vop.  This is useful for converting 3 element
   vectors to 4 element vectors and vice-versa.

   Let's assume \p vop is a vector of 4 floats and \p new_size is 3.  This will
   be expanded into following LLVM instruction:

   <pre>
   %0 = shufflevector <4 x float> %vop, <4 x float> undef,
             <3 x i32> <i32 0, i32 1, i32 2>
   </pre>

   This will build a vector of 3 floats with 3 first elements from \p vop.
 */
static OPERAND *
gen_resized_vect(OPERAND *vop, int new_size, int start)
{
  OPERAND *operand, *undefop;
  LL_Type *llt;
  INSTR_LIST *Curr_Instr;
  INT v[TY_VECT_MAXLEN];
  int i;

  assert(vop->ll_type->data_type == LL_VECTOR, "expecting vector type",
         vop->ll_type->data_type, ERR_Fatal);
  llt = ll_get_vector_type(vop->ll_type->sub_types[0], new_size);
  operand = make_tmp_op(llt, make_tmps());

  Curr_Instr = gen_instr(I_SHUFFVEC, operand->tmps, operand->ll_type, vop);

  vop->next = make_operand();
  vop->next->ot_type = OT_UNDEF;
  vop->next->ll_type = vop->ll_type;

  if ((ll_type_bytes(vop->ll_type) * 8) > new_size) {
    vop->next->next = gen_imask(
        get_vcon0_n(get_vector_type(DT_INT, new_size), start, new_size));
  } else {
    for (i = 0; i < ll_type_bytes(vop->ll_type) * 8; i++)
      v[i] = i + start;
    for (; i < new_size; i++)
      v[i] = ll_type_bytes(vop->ll_type) * 8 + start;
    vop->next->next = gen_imask(get_vcon(v, get_vector_type(DT_INT, new_size)));
  }

  ad_instr(0, Curr_Instr);

  return operand;
}

static OPERAND *
gen_scalar_to_vector_helper(int ilix, int from_ili, LL_Type *ll_vecttype)
{
  OPERAND *operand, *undefop, *arg;
  INSTR_LIST *Curr_Instr;

  operand = make_tmp_op(ll_vecttype, make_tmps());

  Curr_Instr = gen_instr(I_SHUFFVEC, operand->tmps, operand->ll_type, NULL);

  undefop = make_undef_op(ll_vecttype);
  arg = gen_llvm_expr(from_ili, ll_vecttype->sub_types[0]);
  Curr_Instr->operands = gen_insert_vector(undefop, arg, 0);

  Curr_Instr->operands->next = make_undef_op(ll_vecttype);

  Curr_Instr->operands->next->next =
      gen_imask(get_vcon0(get_vector_type(DT_INT, ll_vecttype->sub_elements)));
  ad_instr(ilix, Curr_Instr);

  return operand;
}

/*
 * Create a vector from a scalar value represented by 'ilix'
 * ilix -> <ilix, ilix, ilix, ilix>
 * Let's assume ilix needs to be promoted to a vector of 4 floats
 * This will be expanded into following LLVM instructions:
 * %0 = insertelement <4 x float> undef, <ilix>, i32 0
 * %1 = shufflevector <4 x float> %0, <4 x float> undef, <4 x i32> <i32 0, i32
 * 0, i32, 0 i32 0>
 */
INLINE static OPERAND *
gen_scalar_to_vector(int ilix, LL_Type *ll_vecttype)
{
  const int from_ili = ILI_OPND(ilix, 1);
  return gen_scalar_to_vector_helper(ilix, from_ili, ll_vecttype);
}

static OPERAND *
gen_scalar_to_vector_no_shuffle(int ilix, LL_Type *ll_vecttype)
{
  const int from_ili = ILI_OPND(ilix, 1);
  OPERAND *undefop = make_undef_op(ll_vecttype);
  OPERAND *arg = gen_llvm_expr(from_ili, ll_vecttype->sub_types[0]);
  OPERAND *operand = gen_insert_vector(undefop, arg, 0);
  return operand;
}

INLINE static OPERAND *
gen_temp_to_vector(int from_ili, LL_Type *ll_vecttype)
{
  const int ilix = 0;
  return gen_scalar_to_vector_helper(ilix, from_ili, ll_vecttype);
}

static OPERAND *
gen_gep_op(int ilix, OPERAND *base_op, LL_Type *llt, OPERAND *index_op)
{
  base_op->next = index_op;
  return ad_csed_instr(I_GEP, ilix, llt, base_op, 0, TRUE);
}

INLINE static OPERAND *
gen_gep_index(OPERAND *base_op, LL_Type *llt, int index)
{
  return gen_gep_op(0, base_op, llt, make_constval32_op(index));
}

static void
insertLLVMDbgValue(OPERAND *load, LL_MDRef mdnode, SPTR sptr, LL_Type *type)
{
  static bool defined = false;
  OPERAND *callOp;
  OPERAND *oper;
  LLVMModuleRef mod = cpu_llvm_module;
  LL_DebugInfo *di = mod->debug_info;
  INSTR_LIST *callInsn = make_instr(I_CALL);

  if (!defined) {
    EXFUNC_LIST *exfunc;
    char *gname =
      "declare void @llvm.dbg.value(metadata, i64, metadata, metadata)";
    exfunc = (EXFUNC_LIST *)getitem(LLVM_LONGTERM_AREA, sizeof(EXFUNC_LIST));
    memset(exfunc, 0, sizeof(EXFUNC_LIST));
    exfunc->func_def = gname;
    exfunc->flags |= EXF_INTRINSIC;
    add_external_function_declaration(exfunc);
    defined = true;
  }

  callInsn->flags |= CALL_INTRINSIC_FLAG;
  callInsn->operands = callOp = make_operand();
  callInsn->dbg_line_op = lldbg_get_var_line(di, sptr);
  callOp->ot_type = OT_CALL;
  callOp->ll_type = make_void_lltype();
  callInsn->ll_type = callOp->ll_type;
  callOp->string = "@llvm.dbg.value";

  callOp->next = oper = make_operand();
  oper->ot_type = OT_MDNODE;
  oper->tmps = load->tmps;
  oper->ll_type = type;
  oper->flags |= OPF_WRAPPED_MD;
  oper = make_constval_op(ll_create_int_type(mod, 64), 0, 0);
  callOp->next->next = oper;
  oper->next = make_mdref_op(mdnode);
  oper->next->next = make_mdref_op(lldbg_emit_empty_expression_mdnode(di));

  ad_instr(0, callInsn);
}

/**
   \brief Construct llvm.dbg.value calls on the load sites
   \param ld    The load
   \param addr  The address being loaded
   \param type  The type of the object being loaded
 */
static void
consLoadDebug(OPERAND *ld, OPERAND *addr, LL_Type *type)
{
  SPTR sptr = addr->val.sptr;
  if (sptr && need_debug_info(sptr)) {
    LL_DebugInfo *di = cpu_llvm_module->debug_info;
    int fin = BIH_FINDEX(gbl.entbih);
    LL_MDRef lcl = lldbg_emit_local_variable(di, sptr, fin, true);
    insertLLVMDbgValue(ld, lcl, sptr, type);
  }
}

/**
   \brief Insert an LLVM load instruction and return the loaded value.
 
   The address operand must be a pointer type that points to the load type.
 
   The flags provide the alignment and volatility, see
   ldst_instr_flags_from_dtype().
 */
static OPERAND *
gen_load(OPERAND *addr, LL_Type *type, unsigned flags)
{
  OPERAND *ld = ad_csed_instr(I_LOAD, 0, type, addr, flags, FALSE);
  consLoadDebug(ld, addr, type);
  return ld;
}

static void
make_store(OPERAND *sop, OPERAND *address_op, unsigned flags)
{
  INSTR_LIST *Curr_Instr;

  Curr_Instr = gen_instr(I_STORE, NULL, NULL, sop);
  Curr_Instr->flags |= flags;
  sop->next = address_op;
  ad_instr(0, Curr_Instr);
}

static OPERAND *
gen_convert_vector(int ilix)
{
  int itype;
  LL_Type *ll_src, *ll_dst;
  OPERAND *operand;
  int dtype_dst = ILI_OPND(ilix, 2);
  int dtype_src = ILI_OPND(ilix, 3);

  ll_dst = make_lltype_from_dtype(dtype_dst);
  ll_src = make_lltype_from_dtype(dtype_src);
  assert(ll_dst->data_type == LL_VECTOR, "gen_convert_vector(): vector type"
                                         " expected for dst",
         ll_dst->data_type, ERR_Fatal);
  assert(ll_src->data_type == LL_VECTOR, "gen_convert_vector(): vector type"
                                         " expected for src",
         ll_src->data_type, ERR_Fatal);
  operand = gen_llvm_expr(ILI_OPND(ilix, 1), ll_src);
  switch (ll_dst->sub_types[0]->data_type) {
  case LL_I1:
  case LL_I8:
  case LL_I16:
  case LL_I24:
  case LL_I32:
  case LL_I40:
  case LL_I48:
  case LL_I56:
  case LL_I64:
  case LL_I128:
  case LL_I256:
    switch (ll_src->sub_types[0]->data_type) {
    case LL_I1:
    case LL_I8:
    case LL_I16:
    case LL_I24:
    case LL_I32:
    case LL_I40:
    case LL_I48:
    case LL_I56:
    case LL_I64:
    case LL_I128:
    case LL_I256:
      if (DT_ISUNSIGNED(dtype_dst))
        operand->flags |= OPF_ZEXT;
      return convert_int_size(ilix, operand, ll_dst);
    case LL_FLOAT:
    case LL_DOUBLE:
      if (DT_ISUNSIGNED(dtype_dst))
        return convert_float_to_uint(operand, ll_dst);
      return convert_float_to_sint(operand, ll_dst);
    default:
      break;
    }
    break;
  case LL_FLOAT:
    switch (ll_src->sub_types[0]->data_type) {
    case LL_I1:
    case LL_I8:
    case LL_I16:
    case LL_I24:
    case LL_I32:
    case LL_I40:
    case LL_I48:
    case LL_I56:
    case LL_I64:
    case LL_I128:
    case LL_I256:
      if (DT_ISUNSIGNED(dtype_src))
        return convert_uint_to_float(operand, ll_dst);
      return convert_sint_to_float(operand, ll_dst);
    case LL_DOUBLE:
      return convert_float_size(operand, ll_dst);
    default:
      break;
    }
    break;
  case LL_DOUBLE:
    switch (ll_src->sub_types[0]->data_type) {
    case LL_I1:
    case LL_I8:
    case LL_I16:
    case LL_I24:
    case LL_I32:
    case LL_I40:
    case LL_I48:
    case LL_I56:
    case LL_I64:
    case LL_I128:
    case LL_I256:
      if (DT_ISUNSIGNED(dtype_src))
        return convert_uint_to_float(operand, ll_dst);
      return convert_sint_to_float(operand, ll_dst);
    case LL_FLOAT:
      return convert_float_size(operand, ll_dst);
    default:
      break;
    }
    break;
  default:
    assert(0, "gen_convert_vector(): unhandled vector type for dst",
           ll_dst->sub_types[0]->data_type, ERR_Fatal);
  }
  assert(0, "gen_convert_vector(): unhandled vector type for src",
         ll_src->sub_types[0]->data_type, ERR_Fatal);
  return NULL;
}

static OPERAND *
gen_binary_vexpr(int ilix, int itype_int, int itype_uint, int itype_float)
{
  DTYPE vect_dtype = ili_get_vect_type(ilix);
  assert(vect_dtype,
         "gen_binary_vexpr(): called with non vector type for ilix ", ilix, 4);
  switch (DTY(DTY(vect_dtype + 1))) {
  case TY_REAL:
  case TY_DBLE:
    return gen_binary_expr(ilix, itype_float);
  case TY_INT:
  case TY_SINT:
  case TY_BINT:
  case TY_INT8:
    return gen_binary_expr(ilix, itype_int);
  case TY_UINT:
  case TY_USINT:
  case TY_LOG:
  case TY_UINT8:
    return gen_binary_expr(ilix, itype_uint);
  default:
    assert(0, "gen_binary_vexpr(): vector type not yet handled for ilix ", ilix,
           4);
  }
  return NULL;
}

/**
   \brief Is \p ilix a candidate for translation to FMA?
   \param ilix   The index of the ILI
   \return ILI of the multiply operand or 0 if not a candidate

   This inspects the operation at \p ilix to determine if it matches any of the
   suitable forms for a fused multiply add instruction.
   <pre>
   ([-] (([-] A) * ([-] B))) + [-] C
   </pre>
   where <tt>[-]</tt> is an optional negation/subraction.
 */
static int
fused_multiply_add_candidate(int ilix)
{
  int l, r, lx, rx;

  switch (ILI_OPC(ilix)) {
#if defined(TARGET_LLVM_POWER) || defined(TARGET_LLVM_X8664)
  case IL_FSUB:
#endif
  case IL_FADD:
    lx = ILI_OPND(ilix, 1);
    l = ILI_OPC(lx);
    if (l == IL_FMUL)
      return lx;
    rx = ILI_OPND(ilix, 2);
    r = ILI_OPC(rx);
    if (r == IL_FMUL)
      return rx;
#if defined(TARGET_LLVM_POWER) || defined(TARGET_LLVM_X8664)
    if ((l == IL_FNEG) && (ILI_OPC(ILI_OPND(lx, 1)) == IL_FMUL))
      return lx;
    if ((r == IL_FNEG) && (ILI_OPC(ILI_OPND(rx, 1)) == IL_FMUL))
      return rx;
#endif
    break;
#if defined(TARGET_LLVM_POWER) || defined(TARGET_LLVM_X8664)
  case IL_DSUB:
#endif
  case IL_DADD:
    lx = ILI_OPND(ilix, 1);
    l = ILI_OPC(lx);
    if (l == IL_DMUL)
      return lx;
    rx = ILI_OPND(ilix, 2);
    r = ILI_OPC(rx);
    if (r == IL_DMUL)
      return rx;
#if defined(TARGET_LLVM_POWER) || defined(TARGET_LLVM_X8664)
    if ((l == IL_DNEG) && (ILI_OPC(ILI_OPND(lx, 1)) == IL_DMUL))
      return lx;
    if ((r == IL_DNEG) && (ILI_OPC(ILI_OPND(rx, 1)) == IL_DMUL))
      return rx;
#endif
    break;
  }
  return 0;
}

#if defined(TARGET_LLVM_POWER)
/**
   \brief Get the OpenPOWER intrinsic name of the MAC instruction
   \param swap  [output] TRUE if caller must swap arguments
   \param fneg  [output] TRUE if multiply result is negated
   \param ilix  The root of the MAC (must be an ADD or SUB)
   \param l     The (original) lhs ili
   \param r     The (original) rhs ili
   \return the name of the intrinsic (sans "llvm." prefix)
 */
static const char *
get_mac_name(int *swap, int *fneg, int ilix, int matches, int l, int r)
{
  ILI_OP opc;

  *swap = (matches == r);
  opc = ILI_OPC((*swap) ? r : l);
  *fneg = (opc == IL_FNEG) || (opc == IL_DNEG);
  switch (ILI_OPC(ilix)) {
  case IL_FADD:
    return (*fneg) ? "ppc.vsx.xsnmsubasp" : "ppc.vsx.xsmaddasp";
  case IL_DADD:
    return (*fneg) ? "ppc.vsx.xsnmsubadp" : "ppc.vsx.xsmaddadp";
  case IL_FSUB:
    if (*swap) {
      return (*fneg) ? "ppc.vsx.xsmaddasp" : "ppc.vsx.xsnmsubasp";
    }
    return (*fneg) ? "ppc.vsx.xsnmaddasp" : "ppc.vsx.xsmsubasp";
  case IL_DSUB:
    if (*swap) {
      return (*fneg) ? "ppc.vsx.xsmaddadp" : "ppc.vsx.xsnmsubadp";
    }
    return (*fneg) ? "ppc.vsx.xsnmaddadp" : "ppc.vsx.xsmsubadp";
  }
  assert(FALSE, "does not match MAC", opc, 4);
  return "";
}
#endif

#if defined(TARGET_LLVM_X8664)
/**
   \brief Get the x86 intrinsic name of the MAC instruction
   \param swap  [output] TRUE if caller must swap arguments
   \param fneg  [output] TRUE if multiply result is negated
   \param ilix  The root of the MAC (must be an ADD or SUB)
   \param l     The (original) lhs ili
   \param r     The (original) rhs ili
   \return the name of the intrinsic (sans "llvm." prefix)
 */
static const char *
get_mac_name(int *swap, int *fneg, int ilix, int matches, int l, int r)
{
  int opc;

  *swap = (matches == r);
  opc = ILI_OPC((*swap) ? r : l);
  *fneg = (opc == IL_FNEG) || (opc == IL_DNEG);
  switch (ILI_OPC(ilix)) {
  case IL_FADD:
    return (*fneg) ? "x86.fma.vfnmadd.ss" : "x86.fma.vfmadd.ss";
  case IL_DADD:
    return (*fneg) ? "x86.fma.vfnmadd.sd" : "x86.fma.vfmadd.sd";
  case IL_FSUB:
    if (*swap) {
      return (*fneg) ? "x86.fma.vfmadd.ss" : "x86.fma.vfnmadd.ss";
    }
    return (*fneg) ? "x86.fma.vfnmsub.ss" : "x86.fma.vfmsub.ss";
  case IL_DSUB:
    if (*swap) {
      return (*fneg) ? "x86.fma.vfmadd.sd" : "x86.fma.vfnmadd.sd";
    }
    return (*fneg) ? "x86.fma.vfnmsub.sd" : "x86.fma.vfmsub.sd";
  }
  assert(FALSE, "does not match MAC", opc, 4);
  return "";
}
#endif

/**
   \brief Put the candidate in proper canonical form

   The canonical form is lhs = (a * b), rhs = c, lhs + rhs.
 */
static void
fused_multiply_add_canonical_form(INSTR_LIST *addInsn, int matches, ILI_OP opc,
                                  OPERAND **l, OPERAND **r, int *lhs_ili,
                                  int *rhs_ili)
{
  ILI_OP lopc;
  ILI_OP negOpc;

  (*l)->next = (*r)->next = NULL;

  if (opc == IL_FSUB) {
    /* negate the rhs. t1 - t2 => t1 + (-t2) */
    negOpc = IL_FNEG;
  } else if (opc == IL_DSUB) {
    negOpc = IL_DNEG;
  } else {
    /* it's already an ADD */
    negOpc = 0;
  }

  if (matches == *rhs_ili) {
    /* multiply on right, exchange lhs and rhs. Don't rewrite anything yet. */
    int tmp = *rhs_ili;
    OPERAND *t = *r;
    *rhs_ili = *lhs_ili;
    *r = *l;
    *lhs_ili = tmp;
    *l = t;
  } else if (negOpc) {
    /* handle subtract form when multiply was already on the left */
    const int ropc = ILI_OPC(*rhs_ili);
    if ((ropc == IL_FNEG) || (ropc == IL_DNEG)) {
      /* double negative */
      (*r)->tmps->use_count--;
      *r = (*r)->tmps->info.idef->operands->next;
    } else {
      OPERAND *neg = gen_llvm_expr(ad1ili(negOpc, *rhs_ili), (*r)->ll_type);
      if (neg->tmps)
        neg->tmps->info.idef->operands->next = *r;
      *r = neg;
    }
    negOpc = 0;
  }

  /* negOpc implies a swap was made. Fixup any negations now. */
  lopc = ILI_OPC(*lhs_ili);
  if (negOpc && ((lopc == IL_FNEG) || (lopc == IL_DNEG))) {
    /* double negation. -(-(a * b)) => (a * b) */
    (*l)->tmps->use_count--;
    *l = (*l)->tmps->info.idef->operands->next;
  } else if (negOpc || (lopc == IL_FNEG) || (lopc == IL_DNEG)) {
    /* swap mult and negate.  -(a * b) => (-a) * b */
    OPERAND *n, *newMul;
    /* l has form: (a * b) or (0 - (a * b)) */
    OPERAND *mul = negOpc ? *l : (*l)->tmps->info.idef->operands->next;
    OPERAND *mul_l = mul->tmps->info.idef->operands; /* a term */
    OPERAND *mul_r = mul_l->next;                    /* b term */
    int mulili = negOpc ? *lhs_ili : ILI_OPND(*lhs_ili, 1);
    int muliliop = ILI_OPC(mulili);
    int mulili_l = ILI_OPND(mulili, 1);
    LL_Type *fTy = mul_l->ll_type;
    /* create n, where n ::= -a */
    if (muliliop == IL_FMUL) {
      n = gen_llvm_expr(ad1ili(IL_FNEG, mulili_l), fTy);
    } else {
      assert(ILI_OPC(mulili) == IL_DMUL, "unexpected expr", mulili, 4);
      n = gen_llvm_expr(ad1ili(IL_DNEG, mulili_l), fTy);
    }
    /* rebuild the multiply */
    if (n->tmps)
      n->tmps->info.idef->operands->next = mul_l;
    n->next = mul_r;
    newMul = make_tmp_op(mul->ll_type, make_tmps());
    ad_instr(mulili, gen_instr(I_FMUL, newMul->tmps, mul->ll_type, n));
    *l = newMul; /* l ::= (n * b) = (-a * b) */
  }
}

/**
   \brief Does this multiply op have more than one use?
   \param multop A multiply operation (unchecked)
 */
static LOGICAL
fma_mult_has_mult_uses(OPERAND *multop)
{
  return (!multop->tmps) || (multop->tmps->use_count > 1);
}

static void
overwrite_fma_add(INSTR_LIST *oldAdd, INSTR_LIST *newFma, INSTR_LIST *last)
{
  INSTR_LIST *prev = oldAdd->prev;
  INSTR_LIST *start = last->next;
  OPERAND *op;
  last->next = NULL;
  start->prev = NULL;
  if (start != newFma) {
    prev->next = start;
    start->prev = prev;
    newFma->prev->next = oldAdd;
    oldAdd->prev = newFma->prev;
  }
  /* updated in place since uses have pointers to oldAdd */
  oldAdd->rank = newFma->rank;
  oldAdd->i_name = newFma->i_name;
  oldAdd->ilix = newFma->ilix;
  oldAdd->flags = newFma->flags;
  oldAdd->ll_type = newFma->ll_type;
  oldAdd->operands = newFma->operands;
  /* force the DCE pass to keep the FMA arguments alive */
  for (op = oldAdd->operands; op; op = op->next) {
    if (op->tmps)
      op->tmps->use_count += 100;
  }
}

#if defined(TARGET_LLVM_X8664)
static OPERAND *
x86_promote_to_vector(LL_Type *eTy, LL_Type *vTy, OPERAND *op)
{
  OPERAND *op1, *undefop, *next;

  if (op == NULL)
    return NULL;

  undefop = make_undef_op(vTy);
  next = op->next;
  op1 = gen_insert_vector(undefop, gen_copy_op(op), 0);
  op1->next = x86_promote_to_vector(eTy, vTy, next);
  return op1;
}
#endif

/**
   \brief Replace add instruction with an FMA when all conditions are met
 */
static void
maybe_generate_fma(int ilix, INSTR_LIST *insn)
{
  int lhs_ili = ILI_OPND(ilix, 1);
  int rhs_ili = ILI_OPND(ilix, 2);
  int matches, opc, isSinglePrec;
  const char *intrinsicName;
  OPERAND *l_l, *l_r, *l, *r, *binops, *fmaop;
  LL_Type *llTy;
  INSTR_LIST *fma, *last;
#if defined(TARGET_LLVM_POWER) || defined(TARGET_LLVM_X8664)
  int swap, fneg;
  OPERAND *mul;
#endif
#if defined(TARGET_LLVM_X8664)
  LL_Type *vTy;
  INSTR_LIST *mulPrev, *mulNext;
#endif

  last = llvm_info.last_instr;
  binops = insn->operands;
  if (lhs_ili == rhs_ili)
    return;

  matches = fused_multiply_add_candidate(ilix);
  if (!matches)
    return;

  l = (matches == lhs_ili) ? binops : binops->next;
  if (fma_mult_has_mult_uses(l))
    return;

  opc = ILI_OPC(matches);
  if ((opc == IL_FNEG) || (opc == IL_DNEG))
    if (fma_mult_has_mult_uses(l->tmps->info.idef->operands->next))
      return;
  l = binops;
  r = binops->next;
  opc = ILI_OPC(ilix);
  /* put in canonical form: left:mult, right:_ */
  killCSE = TRUE;
  clear_csed_list();
  isSinglePrec = (opc == IL_FADD) || (opc == IL_FSUB);
  llTy = make_lltype_from_dtype(isSinglePrec ? DT_FLOAT : DT_DBLE);
#if defined(TARGET_LLVM_POWER) || defined(TARGET_LLVM_X8664)
  /* use intrinsics for specific target instructions */
  intrinsicName = get_mac_name(&swap, &fneg, ilix, matches, lhs_ili, rhs_ili);
  if (swap) {
    OPERAND *t = r;
    r = l;
    l = t;
  }
  mul = fneg ? l->tmps->info.idef->operands->next : l;
  l_l = mul->tmps->info.idef->operands;
  l_r = l_l->next;
  l_r->next = r;
  r->next = NULL;
  l->tmps->use_count--;
#if defined(TARGET_LLVM_X8664)
  vTy = ll_get_vector_type(llTy, (isSinglePrec ? 4 : 2));
  l_l = x86_promote_to_vector(llTy, vTy, l_l);
  llTy = vTy;
#endif
#else /* not Power/LLVM or X86-64/LLVM */
  /* use the documented LLVM intrinsic: '@llvm.fma.*' */
  fused_multiply_add_canonical_form(insn, matches, opc, &l, &r, &lhs_ili,
                                    &rhs_ili);
  /* llvm.fma ::= madd(l.l * l.r + r), assemble args in the LLVM order */
  l_l = l->tmps->info.idef->operands;
  l_r = l_l->next;
  l_r->next = r;
  l->tmps->use_count--;
  intrinsicName = isSinglePrec ? "fma.f32" : "fma.f64";
#endif
  fmaop = gen_call_llvm_intrinsic(intrinsicName, l_l, llTy, NULL, I_PICALL);
#if defined(TARGET_LLVM_X8664)
  fmaop->tmps->use_count++;
  fmaop = gen_extract_vector(fmaop, 0);
#endif
  fmaop->tmps->use_count++;
  fma = fmaop->tmps->info.idef;
  overwrite_fma_add(insn, fma, last);
  if (DBGBIT(12, 0x40000))
    printf("fma %s inserted at ili %d\n", intrinsicName, ilix);
  ccff_info(MSGOPT, "OPT051", gbl.findex, gbl.lineno,
            "FMA (fused multiply-add) instruction(s) generated", NULL);
  llvm_info.last_instr = last;
  killCSE = FALSE;
}

/**
   \brief Find and rewrite any multiply-add opportunities
   \param isns  The list of instructions
 */
static void
fma_rewrite(INSTR_LIST *isns)
{
  INSTR_LIST *p;

  for (p = isns; p; p = p->next) {
    int ilx = p->ilix;
    if (ilx && p->tmps && p->operands && (p->tmps->use_count > 0))
      maybe_generate_fma(ilx, p);
  }
}

/**
   \brief Undoes the multiply-by-reciprocal transformation
   \param isns  The list of instructions
 */
static void
undo_recip_div(INSTR_LIST *isns)
{
  INSTR_LIST *p;

  for (p = isns; p; p = p->next)
    if (p->ilix && p->tmps && p->operands && (p->tmps->use_count > 0) &&
        (p->i_name == I_FMUL))
      maybe_undo_recip_div(p);
}

static OPERAND *
gen_binary_expr(int ilix, int itype)
{
  int lhs_ili, rhs_ili, ret_match, size1, size2;
  int vect_type, vect_dtype = 0;
  int flags = 0;
  ILI_OP opc = ILI_OPC(ilix);
  OPERAND *operand, *binops, *load_op;
  LL_Type *opc_type, *instr_type, *ll_tmp, *load_type;
  TMPS *new_tmps;
  INT val[2];
  union {
    double d;
    INT tmp[2];
  } dtmp;
  float f;
  double d;

  DBGTRACEIN2(" ilix: %d(%s)", ilix, IL_NAME(opc))

  lhs_ili = ILI_OPND(ilix, 1);
  rhs_ili = ILI_OPND(ilix, 2);

  switch (opc) {
  case IL_VMUL:
  case IL_IMUL:
  case IL_KMUL:
  case IL_VSUB:
  case IL_ISUB:
  case IL_KSUB:
  case IL_VADD:
  case IL_IADD:
  case IL_KADD:
  case IL_VLSHIFTV:
  case IL_VLSHIFTS:
  case IL_LSHIFT:
  case IL_KLSHIFT:
  case IL_VNEG:
  case IL_INEG:
  case IL_KNEG:
    flags |= NOSIGNEDWRAP;
    break;
  default:;
  }
  /* account for the *NEG ili - LLVM treats all of these as subtractions
   * from zero.
   */
  if (!rhs_ili || !IL_ISLINK(opc, 2)) {
    rhs_ili = lhs_ili;
    switch (opc) {
    case IL_NOT:
    case IL_UNOT:
      lhs_ili = ad_icon(-1);
      break;
    case IL_KNOT:
    case IL_UKNOT:
      lhs_ili = ad_kconi(-1);
      break;
    case IL_VNOT:
      vect_dtype = ILI_OPND(ilix, 2);
      switch (DTY(DTY(vect_dtype + 1))) {
      case TY_INT8:
      case TY_UINT8:
      {
        INT num[2];

        ISZ_2_INT64(-1, num);
        lhs_ili =
            ad1ili(IL_VCON, get_vconi(ILI_OPND(ilix, 2), getcon(num, DT_INT8)));
      } break;
      case TY_REAL:
      case TY_DBLE:
        assert(0, "gen_binary_expr(): VNOT of float/double not handled yet", 0,
               4);
        break;
      default:
        lhs_ili = ad1ili(IL_VCON, get_vconi(ILI_OPND(ilix, 2), -1));
      }
      break;
    case IL_INEG:
    case IL_UINEG:
      lhs_ili = ad_icon(0);
      break;
    case IL_KNEG:
    case IL_UKNEG:
      lhs_ili = ad_kconi(0);
      break;
    case IL_FNEG:
      lhs_ili = ad1ili(IL_FCON, stb.fltm0);
      break;
    case IL_DNEG:
      lhs_ili = ad1ili(IL_DCON, stb.dblm0);
      break;
    case IL_VNEG:
      vect_dtype = ILI_OPND(ilix, 2);
      lhs_ili = ad1ili(IL_VCON, get_vconm0(vect_dtype));
      vect_type = (DTY(DTY(vect_dtype + 1)));
      break;
    default:
      DBGTRACE1("#opcode %s not handled as *NEG ili", IL_NAME(opc))
      assert(0, "gen_binary_expr(): confusion with opcode", opc, 4);
    }
  }
  vect_dtype = ili_get_vect_type(ilix);
  if (vect_dtype) {
    instr_type = make_lltype_from_dtype(vect_dtype);
  } else
      if ((instr_type = make_type_from_opc(opc)) == NULL) {
    assert(0, "gen_binary_expr(): no type information", 0, 4);
  }

  DBGTRACE2("#generating first binary operand, lhs_ili: %d(%s)", lhs_ili,
            IL_NAME(ILI_OPC(lhs_ili)))

  binops = gen_llvm_expr(lhs_ili, instr_type);

  DBGTRACE2("#generating second binary operand, rhs_ili: %d(%s)", rhs_ili,
            IL_NAME(ILI_OPC(rhs_ili)))

  switch (opc) {
  case IL_KLSHIFT:
  case IL_KURSHIFT:
  case IL_KARSHIFT:
    binops->next = gen_llvm_expr(rhs_ili, make_lltype_from_dtype(DT_UINT8));
    break;
  case IL_LSHIFT:
  case IL_ULSHIFT:
  case IL_URSHIFT:
  case IL_RSHIFT:
  case IL_ARSHIFT:
    binops->next = gen_llvm_expr(rhs_ili, make_lltype_from_dtype(DT_UINT));
    break;
  case IL_VLSHIFTS:
  case IL_VRSHIFTS:
  case IL_VURSHIFTS:
    binops->next = gen_temp_to_vector(
        rhs_ili, make_vtype(DTY(vect_dtype + 1), DTY(vect_dtype + 2)));
    break;
  default:
    binops->next = gen_llvm_expr(rhs_ili, instr_type);
  }

  /* now make the new binary expression */
  operand = ad_csed_instr(itype, ilix, instr_type, binops, flags, TRUE);

  DBGTRACEOUT1(" returns operand %p", operand)

  return operand;
} /* gen_binary_expr */

/* Compute the high result bits of a multiplication.
 *
 * ilix should be an IL_KMULH or IL_UKMULH instruction.
 */
static OPERAND *
gen_mulh_expr(int ilix)
{
  int ext_instr, shr_instr;
  int mul_flags;
  int bits = 64;
  OPERAND *lhs, *rhs, *result;
  LL_Type *op_llt, *big_llt;

  switch (ILI_OPC(ilix)) {
  case IL_KMULH:
    ext_instr = I_SEXT;
    mul_flags = NOSIGNEDWRAP;
    shr_instr = I_ASHR;
    break;
  case IL_UKMULH:
    ext_instr = I_ZEXT;
    mul_flags = NOUNSIGNEDWRAP;
    shr_instr = I_LSHR;
    break;
  default:
    interr("Unknown mulh opcode", ILI_OPC(ilix), 4);
  }

  /* Extend both sides to i128. */
  op_llt = make_int_lltype(bits);
  big_llt = make_int_lltype(2 * bits);
  lhs = gen_llvm_expr(ILI_OPND(ilix, 1), op_llt);
  rhs = gen_llvm_expr(ILI_OPND(ilix, 2), op_llt);
  lhs = ad_csed_instr(ext_instr, ilix, big_llt, lhs, 0, TRUE);
  rhs = ad_csed_instr(ext_instr, ilix, big_llt, rhs, 0, TRUE);

  /* Do the multiplication in 128 bits. */
  lhs->next = rhs;
  result = ad_csed_instr(I_MUL, ilix, big_llt, lhs, mul_flags, TRUE);

  /* Shift down to get the high bits. */
  result->next = make_constval_op(big_llt, bits, 0);
  result = ad_csed_instr(shr_instr, ilix, big_llt, result, 0, TRUE);

  /* Finally truncate down to 64 bits */
  return ad_csed_instr(I_TRUNC, ilix, op_llt, result, 0, TRUE);
}

/**
   return new operand of type OT_TMP as result of converting cast_op.  If
   cast_op has a next, make sure the next pointer is dealt with properly BEFORE
   the call to make_bitcast().
 */
static OPERAND *
make_bitcast(OPERAND *cast_op, LL_Type *rslt_type)
{
  OPERAND *operand;
  TMPS *new_tmps;
  INSTR_LIST *Curr_Instr, *instr;

  if (strict_match(cast_op->ll_type, rslt_type))
    return gen_copy_op(cast_op);

  assert(ll_type_bytes(cast_op->ll_type) == ll_type_bytes(rslt_type),
         "sizes do not match", 0, ERR_Fatal);

  if (cast_op->ot_type == OT_TMP) {
    instr = cast_op->tmps->info.idef;
    if (instr && (instr->i_name == I_BITCAST) &&
        strict_match(instr->operands->ll_type, rslt_type)) {
      return gen_copy_op(instr->operands);
    }
  }

  DBGTRACEIN1(" cast op: %p", cast_op)
  DBGDUMPLLTYPE("result type ", rslt_type)
  DBGDUMPLLTYPE("cast_op type ", cast_op->ll_type)

  if (ENABLE_CSE_OPT) {
    instr = llvm_info.last_instr;
    while (instr) {
      switch (instr->i_name) {
      case I_BR:
      case I_INDBR:
      case I_NONE:
        instr = NULL;
        break;
      case I_BITCAST:
        if (same_op(cast_op, instr->operands) &&
            strict_match(rslt_type, instr->ll_type)) {
          operand = make_tmp_op(rslt_type, instr->tmps);
          DBGTRACEOUT1(" returns CSE'd operand %p\n", operand)

          return operand;
        }
      default:
        if (instr->flags & STARTEBB)
          instr = NULL;
        else
          instr = instr->prev;
      }
    }
  }
  Curr_Instr = gen_instr(I_BITCAST, new_tmps = make_tmps(), rslt_type, cast_op);
  cast_op->next = NULL;
  ad_instr(0, Curr_Instr);
  /* now build the operand */
  operand = make_tmp_op(rslt_type, new_tmps);

  DBGTRACEOUT1(" returns operand %p\n", operand)

  return operand;
} /* make_bitcast */

/* return new operand of type OT_TMP as result of converting convert_op,
 * which is floating pt but needs coercion to the larger size within rslt_type.
 * If the passed operand convert_op has a next pointer, make sure it
 * is handled BEFORE this call!
 */
static OPERAND *
convert_float_size(OPERAND *convert_op, LL_Type *rslt_type)
{
  LL_Type *ty1, *ty2;
  int kind1, kind2;
  int conversion_instr;
  OPERAND *op_tmp;
  TMPS *new_tmps;
  INSTR_LIST *Curr_Instr;

  DBGTRACEIN1(" convert op %p", convert_op)
  DBGDUMPLLTYPE("result type ", rslt_type)

  ty1 = convert_op->ll_type;
  ty2 = rslt_type;
  kind1 = (ty1->data_type == LL_VECTOR) ? ty1->sub_types[0]->data_type
                                        : ty1->data_type;
  kind2 = (ty2->data_type == LL_VECTOR) ? ty2->sub_types[0]->data_type
                                        : ty2->data_type;
  if (kind1 > kind2)
    conversion_instr = I_FPTRUNC;
  else
    conversion_instr = I_FPEXT;
  new_tmps = make_tmps();
  op_tmp = make_tmp_op(ty2, new_tmps);
  Curr_Instr =
      gen_instr(conversion_instr, new_tmps, op_tmp->ll_type, convert_op);
  convert_op->next = NULL;
  ad_instr(0, Curr_Instr);

  DBGTRACEOUT1(" returns operand %p", op_tmp)
  return op_tmp;
} /* convert_float_size */

/** return new operand of type OT_TMP as result of converting convert_op, which
    is an int but needs coercion to the int size within rslt_type.  If the
    passed operand convert_op has a next pointer, make sure it is handled BEFORE
    this call! */
static OPERAND *
convert_int_size(int ilix, OPERAND *convert_op, LL_Type *rslt_type)
{
  LL_Type *ty1, *ty2, *ll_type;
  int size1, size2, kind1, kind2, flags1, flags2, conversion_instr;
  OPERAND *op_tmp;
  TMPS *new_tmps;
  INSTR_LIST *Curr_Instr;

  DBGTRACEIN1(" convert op %p", convert_op)
  DBGDUMPLLTYPE("result type ", rslt_type)

  ty1 = convert_op->ll_type;
  ty2 = rslt_type;
  if (ty1->data_type == LL_VECTOR) {
    kind1 = ty1->sub_types[0]->data_type;
    size1 = ll_type_int_bits(ty1->sub_types[0]);
    if (!size1)
      size1 = ll_type_bytes(ty1->sub_types[0]) * 8;
  } else {
    kind1 = ty1->data_type;
    size1 = ll_type_int_bits(ty1);
    if (!size1)
      size1 = ll_type_bytes(ty1) * 8;
  }
  flags1 = convert_op->flags;
  if (ty2->data_type == LL_VECTOR) {
    kind2 = ty2->sub_types[0]->data_type;
    size2 = ll_type_int_bits(ty2->sub_types[0]);
    if (!size2)
      size2 = ll_type_bytes(ty2->sub_types[0]) * 8;
  } else {
    kind2 = ty2->data_type;
    size2 = ll_type_int_bits(ty2);
    if (!size2)
      size2 = ll_type_bytes(ty2) * 8;
  }
  assert(ll_type_int_bits(ty1), "convert_int_size(): expected int type for"
                                " src",
         kind1, ERR_Fatal);
  assert(ll_type_int_bits(ty2), "convert_int_size(): expected int type for"
                                " dst",
         kind2, ERR_Fatal);
  /* need conversion, either extension or truncation */
  if (size1 < size2) {
    /* extension */
    conversion_instr = (flags1 & OPF_ZEXT) ? I_ZEXT : I_SEXT;
  } else if (size1 > size2) {
    /* size1 > size2, truncation */
    conversion_instr = I_TRUNC;
  } else {
    DBGTRACE("#conversion of same size, should be a conversion signed/unsigned")
    DBGTRACEOUT1(" returns operand %p", convert_op)
    return convert_op;
  }

  DBGTRACE2("#coercing ints to size %d with instruction %s", size2,
            llvm_instr_names[conversion_instr])

  new_tmps = make_tmps();
  ll_type = ty2;
  op_tmp = ad_csed_instr(conversion_instr, ilix, ll_type, convert_op, 0, TRUE);

  DBGTRACEOUT1(" returns operand %p", op_tmp)
  return op_tmp;
} /* convert_int_size */

static OPERAND *
convert_operand(OPERAND *convert_op, LL_Type *rslt_type,
                int convert_instruction)
{
  LL_Type *ty, *ll_type;
  int size;
  OPERAND *op_tmp;
  TMPS *new_tmps;
  INSTR_LIST *Curr_Instr;

  DBGTRACEIN1(" convert op %p", convert_op)
  DBGDUMPLLTYPE("result type ", rslt_type)

  ty = convert_op->ll_type;
  size = ll_type_bytes(ty) * 8;
  new_tmps = make_tmps();
  ll_type = rslt_type;
  op_tmp = make_tmp_op(ll_type, new_tmps);
  Curr_Instr = gen_instr(convert_instruction, new_tmps, ll_type, convert_op);
  ad_instr(0, Curr_Instr);
  DBGTRACEOUT1(" returns operand %p", op_tmp)
  return op_tmp;
}

static OPERAND *
convert_int_to_ptr(OPERAND *convert_op, LL_Type *rslt_type)
{
  const LL_Type *llt = convert_op->ll_type;
  assert(llt && (ll_type_int_bits(llt) == 8 * size_of(DT_CPTR)),
         "Unsafe type for inttoptr", ll_type_int_bits(llt), ERR_Fatal);
  return convert_operand(convert_op, rslt_type, I_INTTOPTR);
}

static OPERAND *
convert_ptr_to_int(OPERAND *convert_op, LL_Type *rslt_type)
{
  return convert_operand(convert_op, rslt_type, I_PTRTOINT);
}

static OPERAND *
sign_extend_int(OPERAND *op, unsigned result_bits)
{
  const LL_Type *llt = op->ll_type;
  assert(ll_type_int_bits(llt) && (ll_type_int_bits(llt) < result_bits),
         "sign_extend_int: bad type", ll_type_int_bits(llt), ERR_Fatal);
  return convert_operand(op, make_int_lltype(result_bits), I_SEXT);
}

static OPERAND *
zero_extend_int(OPERAND *op, unsigned result_bits)
{
  const LL_Type *llt = op->ll_type;
  assert(ll_type_int_bits(llt) && (ll_type_int_bits(llt) < result_bits),
         "zero_extend_int: bad type", ll_type_int_bits(llt), ERR_Fatal);
  return convert_operand(op, make_int_lltype(result_bits), I_ZEXT);
}

static OPERAND *
convert_sint_to_float(OPERAND *convert_op, LL_Type *rslt_type)
{
  return convert_operand(convert_op, rslt_type, I_SITOFP);
}

static OPERAND *
convert_float_to_sint(OPERAND *convert_op, LL_Type *rslt_type)
{
  return convert_operand(convert_op, rslt_type, I_FPTOSI);
}

static OPERAND *
convert_uint_to_float(OPERAND *convert_op, LL_Type *rslt_type)
{
  return convert_operand(convert_op, rslt_type, I_UITOFP);
}

static OPERAND *
convert_float_to_uint(OPERAND *convert_op, LL_Type *rslt_type)
{
  return convert_operand(convert_op, rslt_type, I_FPTOUI);
}

static INSTR_LIST *
remove_instr(INSTR_LIST *instr, LOGICAL update_usect_only)
{
  INSTR_LIST *prev, *next;
  OPERAND *operand;

  prev = instr->prev;
  next = instr->next;
  if (!update_usect_only) {
    if (next)
      next->prev = prev;
    else
      llvm_info.last_instr = prev;
    if (prev)
      prev->next = next;
    else
      Instructions = next;
  }
  for (operand = instr->operands; operand; operand = operand->next) {
    if (operand->ot_type == OT_TMP) {
      assert(operand->tmps, "remove_instr(): missing temp operand", 0, 4);
      operand->tmps->use_count--;
    }
  }

  return prev;
}

static void
remove_dead_instrs(void)
{
  INSTR_LIST *instr;
  instr = llvm_info.last_instr;
  while (instr) {
    if ((instr->i_name == I_STORE) && instr->flags & DELETABLE)
      instr = remove_instr(instr, FALSE);
    else if ((instr->i_name != I_CALL && instr->i_name != I_INVOKE &&
              instr->i_name != I_ATOMICRMW) &&
             (instr->tmps != NULL) && instr->tmps->use_count <= 0)
      instr = remove_instr(instr, FALSE);
    else
      instr = instr->prev;
  }
}

static bool
same_op(OPERAND *op1, OPERAND *op2)
{
  if (op1->ot_type != op2->ot_type)
    return false;
  switch (op1->ot_type) {
  case OT_TMP:
    return (op1->tmps == op2->tmps);
  case OT_VAR:
    return (op1->val.sptr == op2->val.sptr);
  case OT_CONSTVAL:
    return (op1->val.conval[0] == op2->val.conval[0]) &&
      (op1->val.conval[1] == op2->val.conval[1]);
  default:
    break;
  }
  return false;
}

/** Return true if a load can be moved upwards (backwards in time)
    over fencing specified by the given instruction. */
static bool
can_move_load_up_over_fence(INSTR_LIST *instr)
{
  switch (instr->flags & ATOMIC_MEM_ORD_FLAGS) {
  case ATOMIC_ACQUIRE_FLAG:
  case ATOMIC_ACQ_REL_FLAG:
  case ATOMIC_SEQ_CST_FLAG:
    return false;
  default:
    break;
  }
  return true;
}

static OPERAND *
find_load_cse(int ilix, OPERAND *load_op, LL_Type *llt)
{
  INSTR_LIST *instr, *del_store_instr, *last_instr;
  int del_store_flags;
  int ld_nme;
  int c;

  if (new_ebb || (!ilix) || (IL_TYPE(ILI_OPC(ilix)) != ILTY_LOAD))
    return NULL;

  ld_nme = ILI_OPND(ilix, 2);
  if (ld_nme == NME_VOL) /* don't optimize a VOLATILE load */
    return NULL;

  /* If there is a deletable store to 'ld_nme', 'del_store_li', set
   * its 'deletable' flag to FALSE.  We do this because 'ld_ili'
   * loads from that address, so we mustn't delete the preceding
   * store to it.  However, if the following LILI scan reaches
   * 'del_store_li', *and* we return the expression that is stored
   * by 'del_store_li', then we restore its 'deletable' flag, since
   * in that case the store *can* be deleted.
   * We track deletable store in EBB but perform CSE load opt only in
   * BB to avoid LLVM opt to fail, so we have to mark stores in EBB as
   * undeletable
   */
  del_store_instr = NULL;
  last_instr = NULL;
  for (instr = llvm_info.last_instr; instr; instr = instr->prev) {
    if (instr->i_name == I_STORE) {
      if (instr->ilix) {
        if (ld_nme == ILI_OPND(instr->ilix, 3)) {
          del_store_instr = instr;
          del_store_flags = del_store_instr->flags;
          del_store_instr->flags &= ~DELETABLE;
          break;
        }
      }
    }
    if (instr->flags & STARTEBB) {
      if (instr->i_name != I_NONE)
        last_instr = instr;
      else
        last_instr = instr->prev;
      break;
    }
  }

  for (instr = llvm_info.last_instr; instr != last_instr; instr = instr->prev) {
    if (instr->ilix == ilix) {
      if (!same_op(instr->operands, load_op))
        return NULL;
      return make_tmp_op(instr->ll_type, instr->tmps);
    }
    switch (instr->i_name) {
    case I_LOAD:
    case I_CMPXCHG:
    case I_ATOMICRMW:
    case I_FENCE:
      DEBUG_ASSERT(instr->ilix != 0 || instr->i_name == I_LOAD,
                   "missing ilix for I_CMPXCHG, I_ATOMICRMW, or I_FENCE");
      if (!can_move_load_up_over_fence(instr))
        return NULL;
      if (instr->i_name == I_LOAD)
        break;
      goto check_conflict;
    case I_STORE:
      if (instr->ilix == 0)
        return NULL;
      if (IL_TYPE(ILI_OPC(instr->ilix)) != ILTY_STORE)
        return NULL;
      if (ILI_OPND(ilix, 1) == ILI_OPND(instr->ilix, 2)) {
        /* Maybe revisited to add conversion op */
        if (match_types(instr->operands->ll_type, llt) != MATCH_OK)
          return NULL;
        if (!same_op(instr->operands->next, load_op))
          return NULL;
        if (instr == del_store_instr)
          instr->flags = del_store_flags;
        return gen_copy_op(instr->operands);
      }
    check_conflict:
      c = enhanced_conflict(ld_nme, ILI_OPND(instr->ilix, 3));
      if (c == SAME || (flg.depchk && c != NOCONFLICT))
        return NULL;
      break;
    case I_INVOKE:
    case I_CALL:
      if (!(instr->flags & FAST_CALL))
        return NULL;
      break;
    case I_NONE:
    case I_BR:
    case I_INDBR:
      if (!ENABLE_ENHANCED_CSE_OPT)
        return NULL;
    default:
      break;
    }
  }

  return NULL;
}

/**
   \brief return new operand of type OT_TMP as result of loading \p load_op
   
   If \p load_op has a next, make sure the next pointer is dealt with properly
   \e BEFORE the call to make_load().
 
   \p flags is the instruction flags to set. Should usually be
   ldst_instr_flags_from_dtype() for natural alignment.
 */
static OPERAND *
make_load(int ilix, OPERAND *load_op, LL_Type *rslt_type, MSZ msz,
          unsigned flags)
{
  OPERAND *operand, *fptrs_op, *cse_op;
  TMPS *new_tmps;
  LL_Type *load_type;
  int array_var, array_dtype, dtype;
  INSTR_LIST *Curr_Instr;

  assert(((int)msz) != -1, "make_load():adding a load because of a matchmem ?",
         0, ERR_Fatal);

  cse_op = NULL;
  if (ENABLE_CSE_OPT) {
    operand = find_load_cse(ilix, load_op, rslt_type);
    if (operand != NULL) {
      const int bits = ll_type_int_bits(operand->ll_type);
      if ((bits > 0) && (bits < 32)) {
        LL_Type *ll_tmp;

        switch (msz) {
        case MSZ_SBYTE:
        case MSZ_SHWORD:
          ll_tmp = operand->ll_type;
          operand->flags |= OPF_SEXT;
          operand->ll_type = ll_tmp;
          break;
        case MSZ_BYTE:
        case MSZ_UHWORD:
          ll_tmp = make_lltype_from_dtype(DT_UINT);
          operand->flags |= OPF_ZEXT;
          operand = convert_int_size(0, operand, ll_tmp);
        default:
          break;
        }
      }
      cse_op = operand;
      return cse_op;
    }
  }
  if (load_op->ot_type == OT_VAR && ll_type_is_pointer_to_function(rslt_type)) {
    load_type = LLTYPE(load_op->val.sptr);
  } else {
    load_type = load_op->ll_type;
  }

  DBGTRACEIN2(" ilix %d, load op: %p", ilix, load_op)
  DBGDUMPLLTYPE("result type ", rslt_type)

  assert(load_type->data_type == LL_PTR, "make_load(): op not ptr type",
         load_type->data_type, ERR_Fatal);
  assert(match_types(load_type->sub_types[0], rslt_type) == MATCH_OK,
         "make_load(): types don't match", 0, ERR_Fatal);
  new_tmps = make_tmps();
  Curr_Instr = gen_instr(I_LOAD, new_tmps, rslt_type, load_op);
  Curr_Instr->flags = flags;
  load_op->next = NULL;
  ad_instr(ilix, Curr_Instr);
  /* make the new operand to be the temp */
  operand = make_tmp_op(rslt_type, new_tmps);
  consLoadDebug(operand, load_op, rslt_type);
  /* Need to make sure the char type is unsigned */
  if (ll_type_int_bits(operand->ll_type) &&
      (ll_type_int_bits(operand->ll_type) < 16)) {
    switch (msz) {
    case MSZ_UBYTE:
    case MSZ_UHWORD:
      operand->flags |= OPF_ZEXT;
      break;
    default:
      break;
    }
  }
  if (ll_type_int_bits(operand->ll_type) &&
      (ll_type_int_bits(operand->ll_type) < 32)) {
    switch (msz) {
    case MSZ_BYTE:
    case MSZ_UHWORD: {
      LL_Type *ll_tmp = make_lltype_from_dtype(DT_UINT);
      operand->flags |= OPF_ZEXT;
      operand = convert_int_size(0, operand, ll_tmp);
    } break;
    default:
      break;
    }
  }

  DBGTRACEOUT1(" returns operand %p", operand);
  return cse_op ? cse_op : operand;
}

/**
   \brief Find the (virtual) function pointer in a JSRA call
   \param ilix  the first argument of the \c IL_JSRA
*/
int
find_pointer_to_function(int ilix)
{
  int addr, addr_acon_ptr;
  int sptr = 0;

  addr = ILI_OPND(ilix, 1);
  while (ILI_OPC(addr) == IL_LDA) {
    if (ILI_OPC(ILI_OPND(addr, 1)) == IL_ACON) {
      addr_acon_ptr = ILI_OPND(addr, 1);
      sptr = ILI_OPND(addr_acon_ptr, 1);
      if (CONVAL1G(sptr)) {
        sptr = CONVAL1G(sptr);
      }
    } else if (ILI_OPC(ILI_OPND(addr, 1)) == IL_AADD) {
      if (ILI_OPC(ILI_OPND(ILI_OPND(addr, 1), 1)) == IL_ACON) {
        addr_acon_ptr = ILI_OPND(ILI_OPND(addr, 1), 1);
        sptr = CONVAL1G(ILI_OPND(addr_acon_ptr, 1));
      }
      addr = ILI_OPND(addr, 1);
    }
    addr = ILI_OPND(addr, 1);
  }

  return sptr;
}

static int
get_call_sptr(int ilix)
{
  int sptr, addr, addr_acon_ptr;
  ILI_OP opc = ILI_OPC(ilix);

  DBGTRACEIN2(" called with ilix %d (opc=%s)", ilix, IL_NAME(opc))

  switch (opc) {
  case IL_JSR:
  case IL_QJSR:
    sptr = ILI_OPND(ilix, 1);
    break;
  case IL_JSRA:
    addr = ILI_OPND(ilix, 1);
    if (ILI_OPC(addr) == IL_LDA) {
      sptr = find_pointer_to_function(ilix);
    } else if (ILI_OPC(addr) == IL_ACON) {
      addr_acon_ptr = ILI_OPND(addr, 1);
      if (!CONVAL1G(addr_acon_ptr))
        sptr = addr_acon_ptr;
      else
        sptr = CONVAL1G(addr_acon_ptr);
    } else if (ILI_OPC(addr) == IL_DFRAR) {
      addr_acon_ptr = ILI_OPND(addr, 1);
      if (ILI_OPC(addr_acon_ptr) == IL_JSR)
        /* this sptr is the called function, but the DFRAR is
         * returning a function pointer from that sptr, and that
         * returned indirect function sptr is unknown.
         */
        /* sptr = ILI_OPND(addr_acon_ptr,1); */
        sptr = 0;
      else if (ILI_OPC(addr_acon_ptr) == IL_JSRA)
        return get_call_sptr(addr_acon_ptr);
      else
        assert(0, "get_call_sptr(): indirect call via DFRAR not JSR/JSRA",
               ILI_OPC(addr_acon_ptr), 4);
    } else {
      assert(false, "get_call_sptr(): indirect call not via LDA/ACON",
             ILI_OPC(addr), ERR_Fatal);
    }
    break;
  default:
    DBGTRACE2("###get_call_sptr unknown opc %d (%s)", opc, IL_NAME(opc))
    assert(0, "get_call_sptr(): unknown opc", opc, 4);
  }

  DBGTRACEOUT1(" returns %d", sptr)

  return sptr;
} /* get_call_sptr */

static void
update_return_type_for_ccfunc(int ilix, ILI_OP opc)
{
  int sptr = ILI_OPND(ilix, 1);
  DTYPE dtype = DTYPEG(sptr);
  DTYPE new_dtype;
  switch (opc) {
  case IL_DFRAR:
    new_dtype = cg_get_type(3, DTY(dtype), DT_CPTR);
    break;
#ifdef IL_DFRSPX87
  case IL_DFRSPX87:
#endif
  case IL_DFRSP:
    new_dtype = cg_get_type(3, DTY(dtype), DT_FLOAT);
    break;
#ifdef IL_DFRDPX87
  case IL_DFRDPX87:
#endif
  case IL_DFRDP:
    new_dtype = cg_get_type(3, DTY(dtype), DT_DBLE);
    break;
  case IL_DFRIR:
    new_dtype = cg_get_type(3, DTY(dtype), DT_INT);
    break;
  case IL_DFRKR:
    new_dtype = cg_get_type(3, DTY(dtype), DT_INT8);
    break;
  case IL_DFRCS:
    new_dtype = cg_get_type(3, DTY(dtype), DT_CMPLX);
    break;
  default:
    assert(0,
           "update_return_type_for_ccfunc():return type not handled for opc ",
           opc, 4);
  }
  DTY(new_dtype + 2) = DTY(dtype + 2);
  DTYPEP(sptr, new_dtype);
}

/* Create a function type from a return type and an argument list. */
static LL_Type *
make_function_type_from_args(LL_Type *return_type, OPERAND *first_arg_op,
                             LOGICAL is_varargs)
{
  unsigned nargs = 0;
  LL_Type **types;
  OPERAND *op;
  unsigned i = 0;
  LL_Type *func_type;

  /* Count the arguments. */
  for (op = first_arg_op; op; op = op->next)
    nargs++;

  /* [0] = return type, [1..] = args. */
  types = calloc(1 + nargs, sizeof(LL_Type *));
  types[i++] = return_type;
  for (op = first_arg_op; op; op = op->next)
    types[i++] = op->ll_type;

  func_type =
      ll_create_function_type(cpu_llvm_module, types, nargs, is_varargs);
  free(types);
  return func_type;
}

/*
 * Generate a single operand for a function call.
 *
 * abi_arg is the index into abi->args. 0 is the return value, 1 is the first
 * argument, ...
 * arg_ili is the ILI instruction setting up the argument, IL_ARG*, IL_GARG, or
 * IL_DA*
 */
static OPERAND *
gen_arg_operand(LL_ABI_Info *abi, unsigned abi_arg, int arg_ili)
{
  int val_res;
  const ILI_OP arg_opc = ILI_OPC(arg_ili);
  const int value_ili = ILI_OPND(arg_ili, 1);
  LL_ABI_ArgInfo arg_info, *arg;
  LL_Type *arg_type;
  int dtype = 0;
  /* Is the ILI value argument a pointer to the value? */
  LOGICAL indirect_ili_value = FALSE;
  LOGICAL need_load = FALSE;
  unsigned flags = 0;
  OPERAND *operand;
  LOGICAL missing = FALSE;

  /* Determine the dtype of the argument, or at least an approximation. Also
   * compute whether indirect_ili_value should be set. */
  switch (arg_opc) {
  case IL_GARGRET:
    assert(abi_arg == 0, "GARGRET out of place", arg_ili, 4);
    /* GARGRET value next-lnk dtype */
    dtype = ILI_OPND(arg_ili, 3);
    /* The GARGRET value is a pointer to where the return value should be
     * stored. */
    indirect_ili_value = TRUE;
    break;

  case IL_GARG:
    /* GARG value next-lnk dtype */
    dtype = ILI_OPND(arg_ili, 3);

    /* The ili argument may be a pointer to the value to be passed. This
     * happens when passing structs by value, for example.  Assume
     * that pointers are never passed indirectly.  This also considers
     * LDSCMPLX and LDDCMPLX (complex value loads).
     */
    val_res = IL_RES(ILI_OPC(value_ili));
    if ((DTY(dtype) != TY_PTR) && ILIA_ISAR(val_res))
      indirect_ili_value = TRUE;
    break;

  default:
    /* Without a GARG, we'll assume that any pointers (IL_ARGAR) are passed
     * by value. The indirect_ili_value stays false, and we don't support
     * passing structs by value. */
    dtype = get_dtype_from_arg_opc(arg_opc);
  }

  /* Make sure arg points to relevant lowering information, generate it if
   * required. */
  if (abi_arg <= abi->nargs) {
    /* This is one of the known arguments. */
    arg = &abi->arg[abi_arg];
  } else {
    missing = TRUE;
    /* This is a trailing argument to a varargs function, or we don't have a
     * prototype. */
    memset(&arg_info, 0, sizeof(arg_info));
    arg = &arg_info;
    assert(dtype, "Can't infer argument dtype from ILI", arg_ili, 4);
    if (abi->is_fortran && !abi->is_iso_c && indirect_ili_value) {
      arg->kind = LL_ARG_INDIRECT;
      ll_abi_classify_arg_dtype(abi, arg, DT_ADDR);
      ll_abi_complete_arg_info(abi, arg, DT_ADDR);
    } else
    {
      ll_abi_classify_arg_dtype(abi, arg, dtype);
      ll_abi_complete_arg_info(abi, arg, dtype);
    }
  }

/* For fortan we want to follow the ILI as close as possible.
 * The exception is if GARG ILI field '4' is set (the arg is byval).
 * Return early in the case of fortran.
 * TODO: Allow code to pass straight through this routine and not return
 * early. Just set 'need_load' properly.
 */
  arg_type = make_lltype_from_abi_arg(arg);
  if (arg->kind != LL_ARG_BYVAL && indirect_ili_value &&
      (ILI_OPND(arg_ili, 4) || arg->kind == LL_ARG_COERCE)) {
    operand = gen_llvm_expr(value_ili, make_ptr_lltype(arg_type));
    return gen_load(operand, arg_type, ldst_instr_flags_from_dtype(dtype));
  }
  operand = gen_llvm_expr(value_ili, arg_type);
  if (arg->kind == LL_ARG_BYVAL && !missing)
    operand->flags |= OPF_SRARG_TYPE;
  return operand;

  arg_type = make_lltype_from_abi_arg(arg);

  switch (arg->kind) {
  case LL_ARG_DIRECT:
    need_load = indirect_ili_value;
    break;

  case LL_ARG_ZEROEXT:
    need_load = indirect_ili_value;
    /* flags |= OP_ZEROEXT_FLAG */
    break;

  case LL_ARG_SIGNEXT:
    need_load = indirect_ili_value;
    /* flags |= OP_SIGNEXT_FLAG */
    break;

  case LL_ARG_COERCE:
    /* It is possible to coerce with a bitcast, but we only implement
     * coercion via memory for now.
     *
     * Complex values are treated as coercion types, due to different abi
     * representations.
     *
     * Note that bitcast coercion works differently on little-endian and
     * big-endian architectures. The coercion cast always works as if the
     * value was stored with the old type and loaded with the new type.
     */
    assert(indirect_ili_value, "Can only coerce indirect args", arg_ili, 4);
    need_load = TRUE;
    break;

  case LL_ARG_INDIRECT:
    assert(indirect_ili_value, "Indirect arg required", arg_ili, 4);
    /* Tag an 'sret' attribute on an indirect return value. */
    if (abi_arg == 0)
      flags |= OPF_SRET_TYPE;
    break;

  case LL_ARG_BYVAL:
    assert(indirect_ili_value, "Indirect arg required for byval", arg_ili, 4);
    flags |= OPF_SRARG_TYPE;
    break;

  default:
    interr("Unknown ABI argument kind", arg->kind, 4);
  }

  if (need_load) {
    LL_Type *ptr_type = make_ptr_lltype(arg_type);
    OPERAND *ptr = gen_llvm_expr(value_ili, ptr_type);
    operand = gen_load(ptr, arg_type, ldst_instr_flags_from_dtype(dtype));
  } else {
    operand = gen_llvm_expr(value_ili, arg_type);
  }

  /* Set sret, byval, sign/zeroext flags. */
  operand->flags |= flags;
  return operand;
}

/* Get the next argument ILI from an IL_ARG* or IL_DA* ILI. The list will be
 * terminated by ILI_NULL. */
static int
get_next_arg(int arg_ili)
{
  switch (ILI_OPC(arg_ili)) {
  case IL_ARGAR:
  case IL_ARGDP:
  case IL_ARGIR:
  case IL_ARGKR:
  case IL_ARGSP:
  case IL_GARG:
  case IL_GARGRET:
    return ILI_OPND(arg_ili, 2);

  case IL_DAAR:
  case IL_DADP:
  case IL_DAIR:
  case IL_DAKR:
  case IL_DASP:
    return ILI_OPND(arg_ili, 3);

  default:
    interr("Unknown IL_ARG opcode", arg_ili, 4);
    return IL_NULL;
  }
}

/*
 * Generate linked list of operands for a call.
 *
 * The returned operand list represent the function arguments. The operand
 * representing the callee is not included.
 */
static OPERAND *
gen_arg_operand_list(LL_ABI_Info *abi, int arg_ili)
{
  LOGICAL fastcall;
  unsigned abi_arg, max_abi_arg = ~0u;
  OPERAND *first_arg_op = NULL, *arg_op = NULL;

  if (LL_ABI_HAS_SRET(abi)) {
/* ABI requires a hidden argument to return a struct. We require ILI to
 * contain a GARGRET instruction in this case. */
    /* GARGRET value next-lnk dtype */
    first_arg_op = arg_op = gen_arg_operand(abi, 0, arg_ili);
    arg_ili = get_next_arg(arg_ili);
  } else if (ILI_OPC(arg_ili) == IL_GARGRET) {
    /* It is up to gen_call_expr() to save the function return value in
     * this case. We'll just ignore the GARGRET. */
    arg_ili = get_next_arg(arg_ili);
  }

  /* If we know the exact prototype of the callee and it isn't a varargs
   * function, don't create more arguments than the function accepts.
   * Old-style C allows functions to be called with extra arguments, but LLVM
   * does not. */
  if (!abi->missing_prototype && !abi->is_varargs)
    max_abi_arg = abi->nargs;

  /* Generate operands for all the provided call arguments. */
  for (abi_arg = 1; abi_arg <= max_abi_arg && ILI_OPC(arg_ili) != IL_NULL;
       arg_ili = get_next_arg(arg_ili), abi_arg++) {
    OPERAND *op = gen_arg_operand(abi, abi_arg, arg_ili);
    if (arg_op == NULL)
      first_arg_op = op;
    else
      arg_op->next = op;
    arg_op = op;
  }

  return first_arg_op;
}

/*
 * Generate LLVM instructions for a call.
 *
 * - ilix is the IL_JSR or IL_JSRA instruction representing the call.
 * - ret_dtype is the dtype for the return value, or 0 for unused return value.
 * - call_instr is either a newly allocated call instruction, or NULL.  If
 *   NULL, a new instruction will be allocated.
 * - call_sptr is the sptr of the called function or function pointer, if known
 *
 * Return an OPERAND representing the value returned by the call.
 */

static OPERAND *
gen_call_expr(int ilix, int ret_dtype, INSTR_LIST *call_instr, int call_sptr)
{
  int first_arg_ili;
  LL_ABI_Info *abi;
  LL_Type *func_type = NULL;
  LL_Type *return_type;
  OPERAND *first_arg_op;
  OPERAND *callee_op;
  OPERAND *result_op = NULL;
  int throw_label = ili_throw_label(ilix);

  if (call_instr == NULL) {
    if (throw_label > 0) {
      call_instr = make_instr(I_INVOKE);
    } else {
      call_instr = make_instr(I_CALL);
    }
  }

  /* Prefer IL_GJSR / IL_GJSRA when available. */
  if (ILI_ALT(ilix)) {
    ilix = ILI_ALT(ilix);
  }
  /* GJSR sym args, or GJSRA addr args flags. */
  first_arg_ili = ILI_OPND(ilix, 2);

  /* Get an ABI descriptor which at least knows about the function return type.
     We may have more arguments than the descriptor knows about if this is a
     varargs call, or if the prototype is missing. */
  abi = ll_abi_from_call_site(cpu_llvm_module, ilix, ret_dtype);
  first_arg_op = gen_arg_operand_list(abi, first_arg_ili);

  /* Now that we have the args we can make an informed decision to FTN calling
     convention. */

  /* Set the calling convention, read by write_I_CALL. */
  call_instr->flags |= abi->call_conv << CALLCONV_SHIFT;

  if (abi->fast_math)
    call_instr->flags |= FAST_MATH_FLAG;

  /* Functions without a prototype are represented in LLVM IR as f(...) varargs
     functions.  Do what clang does and bitcast to a function pointer which is
     varargs, but with all the actual argument types filled in. */
  if (abi->missing_prototype) {
#if defined(TARGET_LLVM_X8664)
    /* Fortran argument lists of dtype currently not precsise. So when
     * we make 256/512-bit math intrinsic calls, which are not really covered
     * by the ABI, LLVM can get confused with stack alignment. This
     * check is just a temporary workaround (which is to not generate
     * the bitcast into a varargs, but just use the known argument.)
     */
    int dsize = DTYPEG(call_sptr);
    dsize = (dsize == 0 ? 0 : zsize_of(dsize));
    if ((dsize == 32 || dsize == 64) &&
        is_256_or_512_bit_math_intrinsic(call_sptr) && !XBIT(183, 0x4000))
      func_type = make_function_type_from_args(ll_abi_return_type(abi),
                                               first_arg_op, 0);
    else
#endif
      func_type = make_function_type_from_args(
          ll_abi_return_type(abi), first_arg_op, abi->call_as_varargs);
  }

  /* Now figure out the callee itself. */
  switch (ILI_OPC(ilix)) {
  case IL_JSR:
  case IL_GJSR:
  case IL_QJSR: {
    /* Direct call: JSR sym arg-lnk */
    int callee_sptr = ILI_OPND(ilix, 1);
    callee_op = make_var_op(callee_sptr);
    /* Create an alternative function type from arguments for a function that
       is defined in the current source file and does not have an alternative
       function type defined yet.  Don't perform this conversion for varargs
       functions, because for those arguments the invocation doesn't match the
       definition. */
    if (!func_type && !abi->is_varargs) {
      func_type = make_function_type_from_args(
          ll_abi_return_type(abi), first_arg_op, abi->call_as_varargs);
    }
    /* Cast function points that are missing a prototype. */
    if (func_type && func_type != callee_op->ll_type->sub_types[0]) {
      callee_op = make_bitcast(callee_op, make_ptr_lltype(func_type));
    }
    break;
  }
  case IL_JSRA:
  case IL_GJSRA: {
    /* Indirect call: JSRA addr arg-lnk flags */
    int addr_ili = ILI_OPND(ilix, 1);
    if (!func_type) {
      func_type = ll_abi_function_type(abi);
    }
    /* Now that we know the desired type we can create the callee address
       expression. */
    callee_op = gen_llvm_expr(addr_ili, make_ptr_lltype(func_type));
    call_instr->flags |= CALL_FUNC_PTR_FLAG;
    break;
  }
  default:
    interr("Unhandled call instruction", ilix, 4);
    break;
  }

  callee_op->next = first_arg_op;
  call_instr->operands = callee_op;

  if (throw_label == -1) {
    /* The function might throw, but the exception should just propagate out to
       the calling function.  Nothing to do. */
  } else if (throw_label == 0) {
    if (callee_op->string && 
        (!strcmp("@__cxa_call_unexpected", callee_op->string))) {
      /* Ignore __cxa_call_unexpected as nounwind, due to bugs in PowerPC
         backend. */
    } else {
      /* The called function should never throw. */
      call_instr->flags |= NOUNWIND_CALL_FLAG;
    }
  } else {
    /* The function might throw, and if it does, control should jump to the
       given label. The normal return label and the exception label are added
       to the end of the operand list. */
    OPERAND *label_op;
    OPERAND *op = call_instr->operands;
    while (op->next) {
      op = op->next;
    }
    label_op = make_label_op(getlab());
    op->next = label_op;
    op = label_op;
    label_op = make_operand();
    label_op->ot_type = OT_LABEL;
    label_op->val.sptr = throw_label;
    op->next = label_op;
  }

  return_type = ll_abi_return_type(abi);
  if (return_type->data_type == LL_VOID) {
    call_instr->ll_type = make_void_lltype();
    /* This function may return a struct via a hidden argument.  See if ILI is
       expecting the function to return the hidden argument pointer, like the
       x86-64 ABI requires.  LLVM handles this in the code generator, so we
       have produced a void return type for the LLVM IR.  Just return the
       hidden argument directly for the ret_dtype to consume. */
    if (LL_ABI_HAS_SRET(abi) &&
        (DTY(ret_dtype) == TY_STRUCT || DTY(ret_dtype) == TY_UNION)) {
      result_op = gen_copy_op(first_arg_op);
      result_op->flags = 0;
    }
  } else {
    /* When we stop wrapping alt_type, this can simply be return_type. */
    call_instr->ll_type = make_lltype_from_abi_arg(&abi->arg[0]);
    call_instr->tmps = make_tmps();

    /* If ret_dtype is not set, no return value is expected. */
    if (ret_dtype) {
      result_op = make_tmp_op(call_instr->ll_type, call_instr->tmps);
    }
  }

  ad_instr(ilix, call_instr);

  /* Check if the expander created a GARGRET call but the ABI returns in
     registers.  In that case, coerce the returned value by storing it to the
     GARGRET value pointer. */
  if (ILI_OPC(first_arg_ili) == IL_GARGRET && !LL_ABI_HAS_SRET(abi)) {
    int addr_ili = ILI_OPND(first_arg_ili, 1);
    int return_dtype = ILI_OPND(first_arg_ili, 3);
    OPERAND *addr;
    assert(ILIA_ISAR(IL_RES(ILI_OPC(addr_ili))),
           "GARGRET must be indirect value", ilix, 4);
    addr = gen_llvm_expr(addr_ili, make_ptr_lltype(call_instr->ll_type));
    make_store(make_tmp_op(call_instr->ll_type, call_instr->tmps), addr,
               ldst_instr_flags_from_dtype(return_dtype));

    /* Does ILI expect the function call to return the hidden argument, like an
       x86-64 sret function call? */
    if (DTY(ret_dtype) == TY_STRUCT || DTY(ret_dtype) == TY_UNION) {
      result_op = gen_copy_op(addr);
    }
  }

  return result_op;
} /* gen_call_expr */

static LOGICAL
is_256_or_512_bit_math_intrinsic(int sptr)
{
  int new_num, new_type;
  const char *sptrName;
  LOGICAL is_g_name = FALSE; /* the first cut at generic names */
  LOGICAL is_newest_name = FALSE;

  if (sptr == 0 || !CCSYMG(sptr))
    return FALSE;
  sptrName = SYMNAME(sptr);
  if (sptrName == NULL)
    return FALSE;

  /* test for generic name that matches "__gv<s|d|c|z>_<math-func>_<2|4|8>" */
  if (!strncmp(sptrName, "__gv", 4)) {
    is_g_name = TRUE;
    new_type = sptrName[4];
  }

  /* test for newest name that matches
   * "__<frp><s|d|c|z>_<math-func>_<2|4|8|16>
   */
  else if (!strncmp(sptrName, "__f", 4) || !strncmp(sptrName, "__p", 4) ||
           !strncmp(sptrName, "__r", 4)) {
    new_type = sptrName[3];
    switch (new_type) {
    case 's':
    case 'd':
    case 'c':
    case 'z':
      is_newest_name = TRUE;
      break;
    default:
      break;
    }
  }

  /* names match: generic name or  "__<f|r>v<s|d>_<math-func>_<vex|fma4>_256" */
  if (is_newest_name || is_g_name || !strncmp(sptrName, "__fv", 4) ||
      !strncmp(sptrName, "__rv", 4))
    sptrName += 4;
  else
    return FALSE;

  if (is_newest_name)
    sptrName++;
  else if ((*sptrName) && ((*sptrName == 's') || (*sptrName == 'd') ||
                           (*sptrName == 'c') || (*sptrName == 'z')) &&
           (sptrName[1] == '_'))
    sptrName += 2;
  else
    return FALSE;

  if (!(*sptrName))
    return FALSE;
  if ((!strncmp(sptrName, "sin", 3)) || (!strncmp(sptrName, "cos", 3)) ||
      (!strncmp(sptrName, "tan", 3)) || (!strncmp(sptrName, "pow", 3)) ||
      (!strncmp(sptrName, "log", 3)) || (!strncmp(sptrName, "exp", 3)) ||
      (!strncmp(sptrName, "mod", 3)) || (!strncmp(sptrName, "div", 3)))
    sptrName += 3;
  else if ((!strncmp(sptrName, "sinh", 4)) || (!strncmp(sptrName, "cosh", 4)))
    sptrName += 4;
  else if (!strncmp(sptrName, "log10", 5))
    sptrName += 5;
  else if (!strncmp(sptrName, "sincos", 6))
    sptrName += 6;
  else
    return FALSE;

  if (is_newest_name) {
    sptrName++;
    new_num = atoi(sptrName);
    switch (new_type) {
    case 's':
      if (new_num == 8 || new_num == 16)
        return TRUE;
      break;
    case 'd':
      if (new_num == 4 || new_num == 8)
        return TRUE;
      break;
    case 'c':
      if (new_num == 4 || new_num == 8)
        return TRUE;
      break;
    case 'z':
      if (new_num == 2 || new_num == 4)
        return TRUE;
      break;
    default:
      return FALSE;
    }
    return FALSE;
  } else if (is_g_name) {
    new_num = atoi(sptrName);
    if (isdigit(sptrName[0]))
      switch (new_type) {
      case 's':
        if (new_num == 8)
          return TRUE;
        break;
      case 'd':
        if (new_num == 4)
          return TRUE;
        break;
      default:
        return FALSE;
      }
    return FALSE;
  } else if (*sptrName == '_')
    sptrName++;
  else
    return FALSE;

  if (!(*sptrName))
    return FALSE;
  if (!strncmp(sptrName, "vex_", 4))
    sptrName += 4;
  else if (!strncmp(sptrName, "fma4_", 5))
    sptrName += 5;
  else
    return FALSE;

  return (!strcmp(sptrName, "256")); /* strcmp: check for trailing garbage */
}

static INSTR_LIST *Void_Call_Instr = NULL;

/* LLVM extractvalue instruction:
 * Given an aggregate and index return the value at that index.
 *
 * <result> = extractvalue <aggregate type> <val>, <idx>{, <idx>}*
 */
static OPERAND *
gen_extract_value_ll(OPERAND *aggr, LL_Type *aggr_ty, LL_Type *elt_ty, int idx) {
  OPERAND *res = make_tmp_op(elt_ty, make_tmps());
  INSTR_LIST *Curr_Instr = gen_instr(I_EXTRACTVAL, res->tmps, aggr_ty, aggr);
  aggr->next = make_constval32_op(idx);
  ad_instr(0, Curr_Instr);
  return res;
}

/* Like gen_extract_value_ll, but takes DTYPE instead of LL_TYPE argumentss. */
static OPERAND *
gen_extract_value(OPERAND *aggr, DTYPE aggr_dtype, DTYPE elt_dtype, int idx)
{
  LL_Type *aggr_ty = make_lltype_from_dtype(aggr_dtype);
  LL_Type *elt_ty = make_lltype_from_dtype(elt_dtype);
  return gen_extract_value_ll(aggr, aggr_ty, elt_ty, idx);
}

static OPERAND *
gen_eval_cmplx_value(int ilix, int dtype)
{
  OPERAND *c1;
  INSTR_LIST *Curr_Instr;
  LL_Type *cmplx_type = make_lltype_from_dtype(dtype);

  c1 = gen_llvm_expr(ilix, cmplx_type);

  /* should move this to a %temp? */

  return c1;
}

static OPERAND *
gen_copy_operand(OPERAND *opnd)
{
  OPERAND *curr;
  OPERAND *head;

  /* copy operand opnd -> c1 */
  head = gen_copy_op(opnd);
  curr = head;
  while (opnd->next) {
    curr->next = gen_copy_op(opnd->next);
    opnd = opnd->next;
    curr = curr->next;
  }

  return head;
}

/* Math operations for complex values.
 * 'itype' should be the I_FADD, I_FSUB, I_xxxx etc.
 * 'dtype' should either be DT_CMPLX or DT_DCMPLX.
 */
static OPERAND *
gen_cmplx_math(int ilix, int dtype, int itype)
{
  OPERAND *r1, *r2, *i1, *i2, *rmath, *imath, *res, *c1, *c2, *cse1, *cse2;
  LL_Type *cmplx_type, *cmpnt_type;
  const int cmpnt = dtype == DT_CMPLX ? DT_FLOAT : DT_DBLE;

  assert(DT_ISCMPLX(dtype), "gen_cmplx_math: Expected DT_CMPLX or DT_DCMPLX",
         dtype, 4);

  cmplx_type = make_lltype_from_dtype(dtype);
  cmpnt_type = make_lltype_from_dtype(cmpnt);

  /* Obtain the components (real and imaginary) for both operands */

  c1 = gen_eval_cmplx_value(ILI_OPND(ilix, 1), dtype);
  c2 = gen_eval_cmplx_value(ILI_OPND(ilix, 2), dtype);
  cse1 = gen_copy_operand(c1);
  cse2 = gen_copy_operand(c2);

  r1 = gen_extract_value(c1, dtype, cmpnt, 0);
  i1 = gen_extract_value(cse1, dtype, cmpnt, 1);

  r2 = gen_extract_value(c2, dtype, cmpnt, 0);
  i2 = gen_extract_value(cse2, dtype, cmpnt, 1);

  r1->next = r2;
  i1->next = i2;

  rmath = ad_csed_instr(itype, 0, cmpnt_type, r1, 0, TRUE);
  imath = ad_csed_instr(itype, 0, cmpnt_type, i1, 0, TRUE);

  /* Build a temp complex in registers and store the mathed values in that */
  res = make_undef_op(cmplx_type);
  res = gen_insert_value(res, rmath, 0);
  return gen_insert_value(res, imath, 1);
}

/* Complex multiply:
 * (a + bi) * (c + di) ==  (a*c) + (a*di) + (bi*c) + (bi*di)
 */
static OPERAND *
gen_cmplx_mul(int ilix, int dtype)
{
  const int elt_dt = (dtype == DT_CMPLX) ? DT_FLOAT : DT_DBLE;
  LL_Type *cmpnt_type = make_lltype_from_dtype(elt_dt);
  OPERAND *a, *bi, *c, *di, *cse1, *cse2;
  OPERAND *r1, *r2, *r3, *r4, *imag, *real, *res, *c1, *c2;

  c1 = gen_eval_cmplx_value(ILI_OPND(ilix, 1), dtype);
  c2 = gen_eval_cmplx_value(ILI_OPND(ilix, 2), dtype);
  cse1 = gen_copy_operand(c1);
  cse2 = gen_copy_operand(c2);

  a = gen_extract_value(c1, dtype, elt_dt, 0);
  bi = gen_extract_value(cse1, dtype, elt_dt, 1);
  c = gen_extract_value(c2, dtype, elt_dt, 0);
  di = gen_extract_value(cse2, dtype, elt_dt, 1);

  /* r1 = (a * c) */
  a->next = c;
  r1 = ad_csed_instr(I_FMUL, 0, cmpnt_type, a, 0, TRUE);

  /* r2 = (a * di) */
  cse1 = gen_copy_operand(c1);
  a = gen_extract_value(cse1, dtype, elt_dt, 0);
  a->next = di;
  r2 = ad_csed_instr(I_FMUL, 0, cmpnt_type, a, 0, TRUE);

  /* r3 = (bi * c) */
  bi->next = c;
  r3 = ad_csed_instr(I_FMUL, 0, cmpnt_type, bi, 0, TRUE);

  /* r4 = (bi * di) */
  cse1 = gen_copy_operand(c1);
  bi = gen_extract_value(cse1, dtype, elt_dt, 1);
  bi->next = di;
  r4 = ad_csed_instr(I_FMUL, 0, cmpnt_type, bi, 0, TRUE);

  /* Real: r1 - r4 */
  r1->next = r4;
  real = ad_csed_instr(I_FSUB, 0, cmpnt_type, r1, 0, TRUE);

  /* Imag: r2 + r3 */
  r2->next = r3;
  imag = ad_csed_instr(I_FADD, 0, cmpnt_type, r2, 0, TRUE);

  res = make_undef_op(make_lltype_from_dtype(dtype));
  res = gen_insert_value(res, real, 0);
  return gen_insert_value(res, imag, 1);
}

static LL_InstrListFlags
ll_instr_flags_from_aop(ATOMIC_RMW_OP aop)
{
  switch (aop) {
  default:
    assert(false, "gen_llvm_atomicrmw_expr: unimplemented op", aop, 4);
  case AOP_XCHG:
    return ATOMIC_XCHG_FLAG;
  case AOP_ADD:
    return ATOMIC_ADD_FLAG;
  case AOP_SUB:
    return ATOMIC_SUB_FLAG;
  case AOP_AND:
    return ATOMIC_AND_FLAG;
  case AOP_OR:
    return ATOMIC_OR_FLAG;
  case AOP_XOR:
    return ATOMIC_XOR_FLAG;
  }
}

static OPERAND *
gen_llvm_atomicrmw_expr(int ilix)
{
  MEMORY_ORDER mo;
  OPERAND *result;
  ATOMIC_INFO info = atomic_info(ilix);
  LL_Type *instr_type = make_type_from_msz(info.msz);
  /* LLVM instruction atomicrmw has operands in opposite order of ILI
   * instruction. */
  OPERAND *op1 = gen_llvm_expr(ILI_OPND(ilix, 2), make_ptr_lltype(instr_type));
  OPERAND *op2 = gen_llvm_expr(ILI_OPND(ilix, 1), instr_type);
  LL_InstrListFlags flags;
  op1->next = op2;
  flags = ll_instr_flags_for_memory_order_and_scope(ilix);
  if (ILI_OPND(ilix, 3) == NME_VOL)
    flags |= VOLATILE_FLAG;
  flags |= ll_instr_flags_from_aop(info.op);
  /* Caller will deal with doing zero-extend/sign-extend if necessary. */
  result = ad_csed_instr(I_ATOMICRMW, ilix, instr_type, op1, flags, false);
  return result;
}

static void
gen_llvm_fence_instruction(int ilix)
{
  LL_InstrListFlags flags = ll_instr_flags_for_memory_order_and_scope(ilix);
  INSTR_LIST *fence;
  fence = gen_instr(I_FENCE, NULL, NULL, NULL);
  fence->flags |= flags;
  ad_instr(0, fence);
}

static OPERAND *
gen_llvm_cmpxchg(int ilix)
{
  LL_Type *aggr_type;
  LL_InstrListFlags flags;
  OPERAND *result;
  LL_Type *elements[2];
  TMPS *tmps;

  /* A cmpxchg instruction can be referenced by both a IL_CMPXCHG_SUCCESS
     and a IL_CMPXCHG_OLDx instruction, so check if we already have
     an LLVM operand for it. */
  tmps = getTempMap(ilix);
  if (tmps) {
    DEBUG_ASSERT(tmps->info.idef->i_name == I_CMPXCHG,
                 "gen_llvm_cmpxchg: TempMap problem");
    result = make_tmp_op(tmps->info.idef->ll_type, tmps);
    return result;
  }

  /* Construct aggregate type for result of cmpxchg. */
  {
    MSZ msz = atomic_info(ilix).msz;
    LL_Module *module = cpu_llvm_module;
    elements[0] = make_type_from_msz(msz);
    elements[1] = ll_create_basic_type(module, LL_I1, 0);
    aggr_type =
        ll_create_anon_struct_type(module, elements, 2, /*is_packed=*/false);
  }

  /* address of location */
  OPERAND *op1 = gen_llvm_expr(cmpxchg_loc(ilix), make_ptr_lltype(elements[0]));
  /* comparand */
  OPERAND *op2 = gen_llvm_expr(ILI_OPND(ilix, 5), elements[0]);
  /* new value */
  OPERAND *op3 = gen_llvm_expr(ILI_OPND(ilix, 1), elements[0]);
  op1->next = op2;
  op2->next = op3;

  /* Construct flags for memory order, volatile, and weak. */
  {
    CMPXCHG_MEMORY_ORDER order = cmpxchg_memory_order(ilix);
    flags = ll_instr_flags_for_memory_order_and_scope(ilix);
    flags |= TO_CMPXCHG_MEMORDER_FAIL(
        ll_instr_flags_from_memory_order(order.failure));
    if (ILI_OPND(ilix, 3) == NME_VOL)
      flags |= VOLATILE_FLAG;
    if (cmpxchg_is_weak(ilix))
      flags |= CMPXCHG_WEAK_FLAG;
  }
  result = ad_csed_instr(I_CMPXCHG, ilix, aggr_type, op1, flags, false);
  return result;
}

static OPERAND *
gen_llvm_cmpxchg_component(int ilix, int idx)
{
  DEBUG_ASSERT((unsigned)idx < 2u, "gen_llvm_cmpxchg_component: bad index");
  OPERAND *ll_cmpxchg;
  int ilix_cmpxchg = ILI_OPND(ilix, 1);
  OPERAND *result;

  /* Generate the cmpxchg */
  ll_cmpxchg = gen_llvm_expr(ilix_cmpxchg, NULL);
  ll_cmpxchg->next = make_constval32_op(idx);
  result = gen_extract_value_ll(ll_cmpxchg, ll_cmpxchg->ll_type,
                                ll_cmpxchg->ll_type->sub_types[idx], idx);
  return result;
}


OPERAND *
gen_llvm_expr(int ilix, LL_Type *expected_type)
{
  int nme_ili, ld_ili, flags;
  SPTR sptr;
  MSZ msz;
  int lhs_ili, rhs_ili, ili_cc, zero_ili = 0;
  int first_ili, second_ili;
  int ct, dt, cmpnt;
  DTYPE dtype;
  ILI_OP opc, cse_opc = (ILI_OP)0;
  DTYPE call_dtype = 0;
  SPTR call_sptr = 0;
  MATCH_Kind ret_match;
  LL_Type *comp_exp_type = NULL, *intrinsic_type;
  OPERAND *operand, *args, *call_op;
  OPERAND *cc_op1, *cc_op2, *c1, *cse1;
  INT tmp[2];
  char *intrinsic_name;
  union {
    double d;
    INT tmp[2];
  } dtmp;
  float f;
  double d;

  switch (ILI_OPC(ilix)) {
  case IL_JSR:
  case IL_JSRA:
    /*  ILI_ALT may be IL_GJSR/IL_GJSRA */
    break;
  default:
    if (ILI_ALT(ilix)) {
      ilix = ILI_ALT(ilix);
    }
  }
  opc = ILI_OPC(ilix);

  DBGTRACEIN2(" ilix: %d(%s)", ilix, IL_NAME(opc));
  DBGDUMPLLTYPE("#expected type: ", expected_type);

  assert(ilix, "gen_llvm_expr(): no incoming ili", 0, 4);
  operand = make_operand();

  switch (opc) {
  case IL_JSRA:
  case IL_GJSRA:
    call_dtype = ILI_OPND(ilix, 4);
    if (call_dtype) {
      call_dtype = DTY(DTYPEG(call_dtype)); /* iface symbol table value */
    } else
      call_dtype = llvm_info.curr_ret_dtype;
    goto call_processing;
    break;
  case IL_QJSR:
  case IL_JSR:
  case IL_GJSR:
    sptr = ILI_OPND(ilix, 1);
    call_op = gen_call_as_llvm_instr(sptr, ilix);
    if (call_op) {
      operand = call_op;
      break;
    }

/* check for return dtype */
    call_dtype = DTY(DTYPEG(sptr));

    DBGTRACE1("#CALL to %s", SYMNAME(sptr))

    call_sptr = sptr;

  call_processing:
    call_op = gen_call_expr(ilix, call_dtype, NULL, call_sptr);
    if (call_op) {
      operand = call_op;
      if (DT_ISUNSIGNED(call_dtype))
        operand->flags |= OPF_ZEXT;
      else if (DT_ISINT(call_dtype))
        operand->flags |= OPF_SEXT;
    } else {
      operand->ll_type = make_void_lltype();
    }
    break;
  case IL_EXIT:
    operand = gen_return_operand();
    break;
  case IL_VA_ARG:
    operand = gen_va_arg(ilix);
    break;
  case IL_ACON:
    operand = gen_base_addr_operand(ilix, expected_type);
    break;
  case IL_LDA:
    nme_ili = ILI_OPND(ilix, 2);
    ld_ili = ILI_OPND(ilix, 1);
    if (ILI_OPC(ld_ili) != IL_ACON && expected_type &&
        (expected_type->data_type == LL_PTR)) {
      LL_Type *pt_expected_type = make_ptr_lltype(expected_type);
      operand = gen_base_addr_operand(ld_ili, pt_expected_type);
    } else {
      operand =
          gen_address_operand(ld_ili, nme_ili, true, NULL, ILI_OPND(ilix, 3));
    }
    sptr = basesym_of(nme_ili);
    if ((operand->ll_type->data_type == LL_PTR) ||
        (operand->ll_type->data_type == LL_ARRAY)) {
      DTYPE dtype = DTYPEG(sptr);

      /* If no type found assume generic pointer */
      if (!dtype)
        dtype = DT_CPTR;

      if (operand->ll_type->sub_types[0]->data_type == LL_PTR ||
          ILI_OPC(ld_ili) != IL_ACON) {
        operand = make_load(ilix, operand, operand->ll_type->sub_types[0], -2,
                            ldst_instr_flags_from_dtype_nme(dtype, nme_ili));
      } else {
        if (ADDRTKNG(sptr) || (SCG(sptr) != SC_DUMMY))
        {
          LL_Type *llt = make_ptr_lltype(expected_type);
          operand = make_bitcast(operand, llt);
          /* ??? what is the magic constant -2? */
          operand = make_load(ilix, operand, operand->ll_type->sub_types[0], -2,
                              ldst_instr_flags_from_dtype_nme(dtype, nme_ili));
        }
      }
    }
    break;
  case IL_VLD: {
    LL_Type *llt, *vect_lltype, *int_llt = NULL;
    int vect_dtype = ILI_OPND(ilix, 3);

    nme_ili = ILI_OPND(ilix, 2);
    ld_ili = ILI_OPND(ilix, 1);
    vect_lltype = make_lltype_from_dtype(vect_dtype);
    llt = make_ptr_lltype(vect_lltype);
    if (expected_type && (expected_type->data_type == LL_VECTOR) &&
        (expected_type->sub_elements == 4 ||
         expected_type->sub_elements == 3) &&
        (llt->sub_types[0]->data_type == LL_VECTOR) &&
        (llt->sub_types[0]->sub_elements == 3)) {
      LL_Type *veleTy = llt->sub_types[0]->sub_types[0];
      LL_Type *vTy = ll_get_vector_type(veleTy, 4);
      llt = make_ptr_lltype(vTy);
    }
#ifdef TARGET_LLVM_ARM
    switch (zsize_of(vect_dtype)) {
    case 2:
      int_llt = make_ptr_lltype(make_lltype_from_dtype(DT_SINT));
      break;
    case 3:
    case 4:
      int_llt = make_ptr_lltype(make_lltype_from_dtype(DT_INT));
      break;
    default:
      break;
    }
#endif
    operand = gen_address_operand(ld_ili, nme_ili, false,
                                  (int_llt ? int_llt : llt), -1);
    if ((operand->ll_type->data_type == LL_PTR) ||
        (operand->ll_type->data_type == LL_ARRAY)) {
      operand = make_load(ilix, operand, operand->ll_type->sub_types[0], -2,
                          ldst_instr_flags_from_dtype(vect_dtype));
      if (int_llt != NULL) {
        if (expected_type == NULL ||
            !strict_match(operand->ll_type, int_llt->sub_types[0]))
          operand = make_bitcast(operand, llt->sub_types[0]);
      }
    } else if (int_llt) {
      operand = make_bitcast(operand, llt);
    }
    if (expected_type && (expected_type->data_type == LL_VECTOR) &&
        !strict_match(operand->ll_type, expected_type)) {
      if (expected_type->sub_elements == 3) {
        if (int_llt && (zsize_of(vect_dtype) == 4))
          operand = make_bitcast(operand, vect_lltype);
      } else {
        operand = make_bitcast(operand, expected_type);
      }
    }
  } break;
  case IL_VLDU: {
    LL_Type *llt, *vect_lltype, *int_llt = NULL;
    int vect_dtype = ILI_OPND(ilix, 3);

    nme_ili = ILI_OPND(ilix, 2);
    ld_ili = ILI_OPND(ilix, 1);
    vect_lltype = make_lltype_from_dtype(vect_dtype);
    llt = make_ptr_lltype(vect_lltype);
    if (expected_type && (expected_type->data_type == LL_VECTOR) &&
        (expected_type->sub_elements == 4) &&
        (llt->sub_types[0]->data_type == LL_VECTOR) &&
        (llt->sub_types[0]->sub_elements == 3)) {
      LL_Type *veleTy = llt->sub_types[0]->sub_types[0];
      LL_Type *vTy = ll_get_vector_type(veleTy, 4);
      llt = make_ptr_lltype(vTy);
    }
#ifdef TARGET_LLVM_ARM
    if (vect_lltype->sub_elements != 3) {
      if (vect_lltype->sub_elements != 3) {
        switch (zsize_of(vect_dtype)) {
        case 2:
          int_llt = make_ptr_lltype(make_lltype_from_dtype(DT_SINT));
          break;
        case 4:
          if (expected_type && (expected_type->data_type == LL_VECTOR) &&
              (expected_type->sub_elements != 3))
            int_llt = make_ptr_lltype(make_lltype_from_dtype(DT_INT));
          break;
        default:
          break;
        }
      } else if (expected_type && ll_type_int_bits(expected_type)) {
        int_llt = make_ptr_lltype(expected_type);
      }
    }
#endif
    operand = gen_address_operand(ld_ili, nme_ili, false,
                                  (int_llt ? int_llt : llt), -1);
    if (ll_type_is_mem_seq(operand->ll_type)) {
      operand = make_load(ilix, operand, operand->ll_type->sub_types[0], -2,
                          /* unaligned */ 0);
      if (int_llt != NULL) {
        if (expected_type == NULL ||
            !strict_match(operand->ll_type, int_llt->sub_types[0]))
          operand = make_bitcast(operand, llt->sub_types[0]);
      } else if (vect_lltype->sub_elements == 3 && expected_type &&
                 ll_type_int_bits(expected_type) &&
                 !strict_match(operand->ll_type, expected_type)) {
        operand = gen_resized_vect(operand, ll_type_bytes(expected_type), 0);
        operand = make_bitcast(operand, expected_type);
      }
    } else if (int_llt) {
      operand = make_bitcast(operand, llt);
    }
    if (expected_type && expected_type->data_type == LL_VECTOR &&
        !strict_match(operand->ll_type, expected_type)) {
      if (expected_type->sub_elements == 3) {
        if (int_llt && (zsize_of(vect_dtype) == 4))
          operand = make_bitcast(operand, vect_lltype);
      } else {
        operand = make_bitcast(operand, expected_type);
      }
    }
  } break;
  case IL_LDSCMPLX:
  case IL_LDDCMPLX: {
    unsigned flags;
    ld_ili = ILI_OPND(ilix, 1);
    nme_ili = ILI_OPND(ilix, 2);
    msz = ILI_OPND(ilix, 3);
    flags = opc == IL_LDSCMPLX ? DT_CMPLX : DT_DCMPLX;
    operand = gen_address_operand(ld_ili, nme_ili, false,
                                  make_ptr_lltype(expected_type), -1);
    assert(operand->ll_type->data_type == LL_PTR,
           "Invalid operand for cmplx load", ilix, 4);
    operand =
        make_load(ilix, operand, operand->ll_type->sub_types[0], msz, flags);
  } break;
  case IL_LD:
  case IL_LDSP:
  case IL_LDDP:
  case IL_LDKR:
#ifdef TARGET_LLVM_X8664
  case IL_LDQ:
  case IL_LD256:
#endif
    ld_ili = ILI_OPND(ilix, 1);
    nme_ili = ILI_OPND(ilix, 2);
    msz = ILI_OPND(ilix, 3);
    operand = gen_address_operand(ld_ili, nme_ili, false, NULL, msz);
    if ((operand->ll_type->data_type == LL_PTR) ||
        (operand->ll_type->data_type == LL_ARRAY)) {
      LL_InstrListFlags flags =
          ldst_instr_flags_from_dtype_nme(msz_dtype(msz), nme_ili);
      operand =
          make_load(ilix, operand, operand->ll_type->sub_types[0], msz, flags);
    }
    break;
  case IL_ATOMICLDI:
  case IL_ATOMICLDKR: {
    LL_InstrListFlags flags;
    ld_ili = ILI_OPND(ilix, 1);
    nme_ili = ILI_OPND(ilix, 2);
    msz = ILI_MSZ_OF_LD(ilix);
    flags = ll_instr_flags_for_memory_order_and_scope(ilix) |
            ldst_instr_flags_from_dtype_nme(msz_dtype(msz), nme_ili);
    operand = gen_address_operand(ld_ili, nme_ili, false, NULL, msz);
    operand =
        make_load(ilix, operand, operand->ll_type->sub_types[0], msz, flags);
  } break;
  case IL_VCON:
  case IL_KCON:
  case IL_ICON:
  case IL_FCON:
  case IL_DCON:
  case IL_SCMPLXCON:
  case IL_DCMPLXCON:
    operand = gen_const_expr(ilix, expected_type);
    break;
  case IL_FIX:
  case IL_DFIX:
    operand = gen_unary_expr(ilix, I_FPTOSI);
    break;
  case IL_FIXK:
  case IL_DFIXK:
    operand = gen_unary_expr(ilix, I_FPTOSI);
    break;
  case IL_FIXUK:
  case IL_DFIXUK:
  case IL_DFIXU:
  case IL_UFIX:
    operand = gen_unary_expr(ilix, I_FPTOUI);
    break;
  case IL_FLOATU:
  case IL_DFLOATU:
  case IL_FLOATUK:
  case IL_DFLOATUK:
    operand = gen_unary_expr(ilix, I_UITOFP);
    break;
  case IL_FLOAT:
  case IL_DFLOAT:
  case IL_DFLOATK:
  case IL_FLOATK:
    operand = gen_unary_expr(ilix, I_SITOFP);
    break;
  case IL_SNGL:
    operand = gen_unary_expr(ilix, I_FPTRUNC);
    break;
  case IL_DBLE:
    operand = gen_unary_expr(ilix, I_FPEXT);
    break;
  case IL_ALLOC:
    operand = gen_unary_expr(ilix, I_ALLOCA);
    break;
  case IL_DEALLOC:
    break;
  case IL_VADD:
    operand = gen_binary_vexpr(ilix, I_ADD, I_ADD, I_FADD);
    break;
  case IL_IADD:
  case IL_KADD:
  case IL_UKADD:
  case IL_UIADD:
    operand = gen_binary_expr(ilix, I_ADD);
    break;
  case IL_FADD:
  case IL_DADD:
    operand = gen_binary_expr(ilix, I_FADD);
    break;
  case IL_SCMPLXADD:
    operand = gen_cmplx_math(ilix, DT_CMPLX, I_FADD);
    break;
  case IL_DCMPLXADD:
    operand = gen_cmplx_math(ilix, DT_DCMPLX, I_FADD);
    break;
  case IL_VSUB:
    operand = gen_binary_vexpr(ilix, I_SUB, I_SUB, I_FSUB);
    break;
  case IL_ISUB:
  case IL_KSUB:
  case IL_UKSUB:
  case IL_UISUB:
    operand = gen_binary_expr(ilix, I_SUB);
    break;
  case IL_FSUB:
  case IL_DSUB:
    operand = gen_binary_expr(ilix, I_FSUB);
    break;
  case IL_SCMPLXSUB:
    operand = gen_cmplx_math(ilix, DT_CMPLX, I_FSUB);
    break;
  case IL_DCMPLXSUB:
    operand = gen_cmplx_math(ilix, DT_DCMPLX, I_FSUB);
    break;
  case IL_VMUL:
    operand = gen_binary_vexpr(ilix, I_MUL, I_MUL, I_FMUL);
    break;
  case IL_IMUL:
  case IL_KMUL:
  case IL_UKMUL:
  case IL_UIMUL:
    operand = gen_binary_expr(ilix, I_MUL);
    break;
  case IL_KMULH:
  case IL_UKMULH:
    operand = gen_mulh_expr(ilix);
    break;
  case IL_FMUL:
  case IL_DMUL:
    operand = gen_binary_expr(ilix, I_FMUL);
    break;
  case IL_SCMPLXMUL:
    operand = gen_cmplx_mul(ilix, DT_CMPLX);
    break;
  case IL_DCMPLXMUL:
    operand = gen_cmplx_mul(ilix, DT_DCMPLX);
    break;
  case IL_VDIV:
    operand = gen_binary_vexpr(ilix, I_SDIV, I_UDIV, I_FDIV);
    break;
  case IL_KDIV:
  case IL_IDIV:
    operand = gen_binary_expr(ilix, I_SDIV);
    break;
  case IL_UKDIV:
  case IL_UIDIV:
    operand = gen_binary_expr(ilix, I_UDIV);
    break;
  case IL_FDIV:
  case IL_DDIV:
    operand = gen_binary_expr(ilix, I_FDIV);
    break;
  case IL_VLSHIFTV:
  case IL_VLSHIFTS:
  case IL_LSHIFT:
  case IL_ULSHIFT:
  case IL_KLSHIFT:
    operand = gen_binary_expr(ilix, I_SHL);
    break;
  case IL_VRSHIFTV:
  case IL_VRSHIFTS:
    operand = gen_binary_vexpr(ilix, I_ASHR, I_LSHR, I_ASHR);
    break;
  case IL_VURSHIFTS:
    operand = gen_binary_vexpr(ilix, I_LSHR, I_LSHR, I_LSHR);
    break;
  case IL_URSHIFT:
  case IL_KURSHIFT:
    operand = gen_binary_expr(ilix, I_LSHR);
    break;
  case IL_RSHIFT:
  case IL_ARSHIFT:
  case IL_KARSHIFT:
    operand = gen_binary_expr(ilix, I_ASHR);
    break;
  case IL_VAND:
    /* need to check dtype - if floating type need special code to
     * cast to int, compare, then cast back to float. Similar to
     * what is done with the IL_FAND case, except with vectors.
     * Else just fall through.
     * NB: currently this method only works for float values, not
     * doubles (and when using -Mfprelaxed that is all our compiler
     * currently operates on anyway.)
     */
    dtype = ILI_OPND(ilix, 3); /* get the vector dtype */
    assert(TY_ISVECT(DTY(dtype)), "gen_llvm_expr(): expected vect type",
           DTY(dtype), 4);
    /* check the base type for float/real */
    if (DTY(DTY(dtype + 1)) == TY_FLOAT) {
      OPERAND *op1, *op2, *op3, *op4, *op5, *op6;
      INSTR_LIST *instr1, *instr2, *instr3;
      int vsize = DTY(dtype + 2);
      LL_Type *viTy = make_vtype(DT_INT, vsize);
      LL_Type *vfTy = make_vtype(DT_FLOAT, vsize);
      op1 = gen_llvm_expr(ILI_OPND(ilix, 1), NULL);
      op2 = make_tmp_op(viTy, make_tmps());
      instr1 = gen_instr(I_BITCAST, op2->tmps, viTy, op1);
      ad_instr(ilix, instr1);
      op3 = gen_llvm_expr(ILI_OPND(ilix, 2), NULL);
      op4 = make_tmp_op(viTy, make_tmps());
      instr2 = gen_instr(I_BITCAST, op4->tmps, viTy, op3);
      ad_instr(ilix, instr2);
      op6 = make_tmp_op(vfTy, make_tmps());
      op2->next = op4;
      op5 = ad_csed_instr(I_AND, 0, viTy, op2, 0, FALSE);
      instr3 = gen_instr(I_BITCAST, op6->tmps, vfTy, op5);
      ad_instr(ilix, instr3);
      operand = op6;
      break;
    }
  case IL_KAND:
  case IL_AND:
    operand = gen_binary_expr(ilix, I_AND);
    break;
  case IL_VOR:
  case IL_KOR:
  case IL_OR:
    operand = gen_binary_expr(ilix, I_OR);
    break;
  case IL_VXOR:
  case IL_KXOR:
  case IL_XOR:
    operand = gen_binary_expr(ilix, I_XOR);
    break;
  case IL_VMOD:
    operand = gen_binary_vexpr(ilix, I_SREM, I_UREM, I_FREM);
    break;
  case IL_SCMPLXXOR:
    operand = gen_cmplx_math(ilix, DT_CMPLX, I_XOR);
    break;
  case IL_DCMPLXXOR:
    operand = gen_cmplx_math(ilix, DT_DCMPLX, I_XOR);
    break;
  case IL_KMOD:
  case IL_MOD:
    operand = gen_binary_expr(ilix, I_SREM);
    break;
  case IL_KUMOD:
  case IL_UIMOD:
    operand = gen_binary_expr(ilix, I_UREM);
    break;
  case IL_ASUB:
  case IL_AADD: {
    LL_Type *t =
        expected_type ? expected_type : make_lltype_from_dtype(DT_CPTR);
    operand = gen_base_addr_operand(ilix, t);
  } break;
  /* jumps on zero with cc */
  case IL_FCJMPZ:
    tmp[0] = 0.0;
    f = 0.0;
    mftof(f, tmp[1]);
    zero_ili = ad1ili(IL_FCON, getcon(tmp, DT_FLOAT));
    comp_exp_type = make_lltype_from_dtype(DT_FLOAT);
  case IL_DCJMPZ:
    if (!zero_ili) {
      d = 0.0;
      xmdtod(d, dtmp.tmp);
      zero_ili = ad1ili(IL_DCON, getcon(dtmp.tmp, DT_DBLE));
      comp_exp_type = make_lltype_from_dtype(DT_DBLE);
    }
    operand->ot_type = OT_CC;
    first_ili = ILI_OPND(ilix, 1);
    second_ili = zero_ili;
    ili_cc = ILI_OPND(ilix, 2);
    if (IEEE_CMP)
      float_jmp = TRUE;
    operand->val.cc = convert_to_llvm_cc(ili_cc, CMP_FLT);
    float_jmp = FALSE;
    operand->ll_type = make_type_from_opc(opc);
    goto process_cc;
    break;
  case IL_UKCJMPZ:
    zero_ili = ad_kconi(0);
    operand->ot_type = OT_CC;
    operand->val.cc = convert_to_llvm_cc(ILI_OPND(ilix, 2), CMP_INT | CMP_USG);
    operand->ll_type = make_type_from_opc(opc);
    first_ili = ILI_OPND(ilix, 1);
    second_ili = zero_ili;
    comp_exp_type = make_lltype_from_dtype(DT_INT8);
    goto process_cc;
    break;
  case IL_UICJMPZ:
    zero_ili = ad_icon(0);
    operand->ot_type = OT_CC;
    operand->val.cc = convert_to_llvm_cc(ILI_OPND(ilix, 2), CMP_INT | CMP_USG);
    operand->ll_type = make_type_from_opc(opc);
    first_ili = ILI_OPND(ilix, 1);
    second_ili = zero_ili;
    comp_exp_type = make_lltype_from_dtype(DT_INT);
    goto process_cc;
    break;

  case IL_KCJMPZ:
    zero_ili = ad_kconi(0);
    operand->ot_type = OT_CC;
    operand->val.cc = convert_to_llvm_cc(ILI_OPND(ilix, 2), CMP_INT);
    operand->ll_type = make_type_from_opc(opc);
    first_ili = ILI_OPND(ilix, 1);
    second_ili = zero_ili;
    comp_exp_type = make_lltype_from_dtype(DT_INT8);
    goto process_cc;
    break;
  case IL_ICJMPZ:
    zero_ili = ad_icon(0);

    operand->ot_type = OT_CC;
    operand->val.cc = convert_to_llvm_cc(ILI_OPND(ilix, 2), CMP_INT);
    operand->ll_type = make_type_from_opc(opc);
    first_ili = ILI_OPND(ilix, 1);
    second_ili = zero_ili;
    comp_exp_type = make_lltype_from_dtype(DT_INT);
    goto process_cc;
    break;
  case IL_ACJMPZ:
    zero_ili = ad_icon(0);

    operand->ot_type = OT_CC;
    operand->val.cc = convert_to_llvm_cc(ILI_OPND(ilix, 2), CMP_INT | CMP_USG);
    comp_exp_type = operand->ll_type = make_type_from_opc(opc);
    first_ili = ILI_OPND(ilix, 1);
    second_ili = zero_ili;
    goto process_cc;
    break;
  /* jumps with cc and expression */
  case IL_FCJMP:
  case IL_DCJMP:
    operand->ot_type = OT_CC;
    first_ili = ILI_OPND(ilix, 1);
    second_ili = ILI_OPND(ilix, 2);
    ili_cc = ILI_OPND(ilix, 3);
    if (IEEE_CMP)
      float_jmp = TRUE;
    operand->val.cc = convert_to_llvm_cc(ili_cc, CMP_FLT);
    float_jmp = FALSE;
    comp_exp_type = operand->ll_type = make_type_from_opc(opc);
    goto process_cc;
    break;
  case IL_UKCJMP:
  case IL_UICJMP:
    operand->ot_type = OT_CC;
    operand->val.cc = convert_to_llvm_cc(ILI_OPND(ilix, 3), CMP_INT | CMP_USG);
    comp_exp_type = operand->ll_type = make_type_from_opc(opc);
    first_ili = ILI_OPND(ilix, 1);
    second_ili = ILI_OPND(ilix, 2);
    goto process_cc;
    break;
  case IL_KCJMP:
  case IL_ICJMP:
    operand->ot_type = OT_CC;
    operand->val.cc = convert_to_llvm_cc(ILI_OPND(ilix, 3), CMP_INT);
    comp_exp_type = operand->ll_type = make_type_from_opc(opc);
    first_ili = ILI_OPND(ilix, 1);
    second_ili = ILI_OPND(ilix, 2);
    goto process_cc;
    break;
  case IL_ACJMP:
    operand->ot_type = OT_CC;
    operand->val.cc = convert_to_llvm_cc(ILI_OPND(ilix, 3), CMP_INT | CMP_USG);
    comp_exp_type = operand->ll_type = make_type_from_opc(opc);
    first_ili = ILI_OPND(ilix, 1);
    second_ili = ILI_OPND(ilix, 2);
  process_cc:
    operand->next = cc_op1 = gen_llvm_expr(first_ili, operand->ll_type);
    operand->next->next = cc_op2 = gen_llvm_expr(second_ili, comp_exp_type);
    break;
  case IL_FCMP:
  case IL_DCMP:
    lhs_ili = ILI_OPND(ilix, 1);
    rhs_ili = ILI_OPND(ilix, 2);
    ili_cc = ILI_OPND(ilix, 3);
    if (IEEE_CMP)
      float_jmp = TRUE;
    operand = gen_comp_operand(operand, opc, lhs_ili, rhs_ili, ili_cc, CMP_FLT,
                               I_FCMP);
    break;
  case IL_CMPNEQSS: {
    OPERAND *op1;
    INSTR_LIST *instr1;
    unsigned bits = 8 * size_of(DT_FLOAT);
    LL_Type *iTy = make_int_lltype(bits);
    lhs_ili = ILI_OPND(ilix, 1);
    rhs_ili = ILI_OPND(ilix, 2);
    ili_cc = CC_NE;
    if (IEEE_CMP)
      float_jmp = TRUE;
    operand = gen_optext_comp_operand(operand, opc, lhs_ili, rhs_ili, ili_cc,
                                      CMP_FLT, I_FCMP, 0, 0);
    /* sext i1 to i32 */
    op1 = make_tmp_op(iTy, make_tmps());
    instr1 = gen_instr(I_SEXT, op1->tmps, iTy, operand);
    ad_instr(ilix, instr1);
    operand = op1;
  } break;
  case IL_VCMPNEQ: {
    OPERAND *op1;
    INSTR_LIST *instr1;
    int vsize;
    LL_Type *viTy;
    dtype = ILI_OPND(ilix, 3); /* get the vector dtype */
    assert(TY_ISVECT(DTY(dtype)), "gen_llvm_expr(): expected vect type",
           DTY(dtype), 4);
    vsize = DTY(dtype + 2);
    viTy = make_vtype(DT_INT, vsize);
    lhs_ili = ILI_OPND(ilix, 1);
    rhs_ili = ILI_OPND(ilix, 2);
    ili_cc = CC_NE;
    if (IEEE_CMP)
      float_jmp = TRUE;
    operand = gen_optext_comp_operand(operand, opc, lhs_ili, rhs_ili, ili_cc,
                                      CMP_FLT, I_FCMP, 0, ilix);
    /* sext i1 to i32 */
    op1 = make_tmp_op(viTy, make_tmps());
    instr1 = gen_instr(I_SEXT, op1->tmps, viTy, operand);
    ad_instr(ilix, instr1);
    operand = op1;
  } break;
  case IL_KCMPZ:
    lhs_ili = ILI_OPND(ilix, 1);
    rhs_ili = ad_kconi(0);
    operand = gen_comp_operand(operand, opc, lhs_ili, rhs_ili,
                               ILI_OPND(ilix, 2), CMP_INT, I_ICMP);
    break;
  case IL_UKCMPZ:
    lhs_ili = ILI_OPND(ilix, 1);
    rhs_ili = ad_kconi(0);
    operand = gen_comp_operand(operand, opc, lhs_ili, rhs_ili,
                               ILI_OPND(ilix, 2), CMP_INT | CMP_USG, I_ICMP);
    break;
  case IL_ICMPZ:
    /* what are we testing for here? We may have an ICMPZ pointing to
     * an FCMP, which is negating the sense of the FCMP. To account for
     * NaNs (hence the IEEE_CMP test) we need to correctly negate
     * the floating comparison operator, taking into account both
     * ordered and unordered cases. That is why we set fcmp_negate
     * for use in convert_to_llvm_cc().
     */
    if (IEEE_CMP && ILI_OPC(ILI_OPND(ilix, 1)) == IL_FCMP) {
      int fcmp_ili = ILI_OPND(ilix, 1);

      lhs_ili = ILI_OPND(fcmp_ili, 1);
      rhs_ili = ILI_OPND(fcmp_ili, 2);
      fcmp_negate = TRUE;
      operand = gen_comp_operand(operand, IL_FCMP, lhs_ili, rhs_ili,
                                 ILI_OPND(fcmp_ili, 3), CMP_FLT, I_FCMP);
      fcmp_negate = FALSE;
      break;
    }
    lhs_ili = ILI_OPND(ilix, 1);
    rhs_ili = ad_icon(0);
    operand = gen_comp_operand(operand, opc, lhs_ili, rhs_ili,
                               ILI_OPND(ilix, 2), CMP_INT, I_ICMP);
    break;
  case IL_ACMPZ:
    lhs_ili = ILI_OPND(ilix, 1);
    rhs_ili = ad_icon(0);
    operand = gen_comp_operand(operand, opc, lhs_ili, rhs_ili,
                               ILI_OPND(ilix, 2), CMP_INT | CMP_USG, I_ICMP);
    break;
  case IL_UICMPZ:
    lhs_ili = ILI_OPND(ilix, 1);
    rhs_ili = ad_icon(0);
    operand = gen_comp_operand(operand, opc, lhs_ili, rhs_ili,
                               ILI_OPND(ilix, 2), CMP_INT | CMP_USG, I_ICMP);
    break;
  case IL_UKCMP:
  case IL_UICMP:
    lhs_ili = ILI_OPND(ilix, 1);
    rhs_ili = ILI_OPND(ilix, 2);
    operand = gen_comp_operand(operand, opc, lhs_ili, rhs_ili,
                               ILI_OPND(ilix, 3), CMP_INT | CMP_USG, I_ICMP);
    break;
  case IL_ACMP:
    lhs_ili = ILI_OPND(ilix, 1);
    rhs_ili = ILI_OPND(ilix, 2);
    operand = gen_comp_operand(operand, opc, lhs_ili, rhs_ili,
                               ILI_OPND(ilix, 3), CMP_INT | CMP_USG, I_ICMP);
    break;
  case IL_KCMP:
  case IL_ICMP:
    lhs_ili = ILI_OPND(ilix, 1);
    rhs_ili = ILI_OPND(ilix, 2);
    operand = gen_comp_operand(operand, opc, lhs_ili, rhs_ili,
                               ILI_OPND(ilix, 3), CMP_INT, I_ICMP);
    break;
  case IL_AIMV:
    operand = gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_CPTR));
    if (expected_type == NULL)
      expected_type = make_lltype_from_dtype(DT_INT);
    break;
  case IL_AKMV:
    operand = gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_CPTR));
    if (expected_type == NULL)
      expected_type = make_lltype_from_dtype(DT_INT8);
    break;
  case IL_KIMV:
    operand = gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_INT));
    break;
  case IL_IKMV:
    operand = gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_INT));
    operand = sign_extend_int(operand, 64);
    break;
  case IL_UIKMV:
    operand = gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_UINT));
    operand = zero_extend_int(operand, 64);
    break;
  case IL_IAMV:
    operand = gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_INT));
    /* This ILI is sometimes generated on 64-bit targets. Make sure it is
     * sign-extended, the LLVM inttoptr instruction zero-extends. */
    if (size_of(DT_CPTR) == 8)
      operand = sign_extend_int(operand, 64);
    break;

  case IL_KAMV:
    operand = gen_llvm_expr(ILI_OPND(ilix, 1), make_int_lltype(64));
#if TARGET_PTRSIZE < 8
    /* Explicitly truncate to a 32-bit int - convert_int_to_ptr() won't work
     * because it can't truncate. */
    operand =
        convert_int_size(ilix, operand, make_int_lltype(8 * TARGET_PTRSIZE));
#endif
    break;

    operand = gen_llvm_expr(ILI_OPND(ilix, 1), expected_type);
    break;
#ifdef IL_DFRSPX87
  case IL_FREESPX87:
    cse_opc = 1;
  case IL_DFRSPX87:
    if (expected_type == NULL)
      expected_type = make_lltype_from_dtype(DT_FLOAT);
    goto _process_define_ili;
  case IL_FREEDPX87:
    cse_opc = 1;
  case IL_DFRDPX87:
    if (expected_type == NULL)
      expected_type = make_lltype_from_dtype(DT_DBLE);
    goto _process_define_ili;
#endif
  case IL_FREEKR:
    cse_opc = 1;
  case IL_DFRKR:
    if (expected_type == NULL) {
      expected_type = make_lltype_from_dtype(DT_INT8);
    }
    goto _process_define_ili;
  case IL_FREEIR:
    cse_opc = 1;
  case IL_DFRIR:
    if (expected_type == NULL) {
      expected_type = make_lltype_from_dtype(DT_INT);
    }
    goto _process_define_ili;
  case IL_FREESP:
    cse_opc = 1;
  case IL_DFRSP:
    if (expected_type == NULL)
      expected_type = make_lltype_from_dtype(DT_FLOAT);
    goto _process_define_ili;
  case IL_FREEDP:
    cse_opc = 1;
  case IL_DFRDP:
    if (expected_type == NULL)
      expected_type = make_lltype_from_dtype(DT_DBLE);
    goto _process_define_ili;
  case IL_DFR128:
    if (expected_type == NULL)
      expected_type = make_lltype_from_dtype(DT_128);
    goto _process_define_ili;
  case IL_DFR256:
    if (expected_type == NULL)
      expected_type = make_lltype_from_dtype(DT_256);
    goto _process_define_ili;
  case IL_FREEAR:
    cse_opc = 1;
  case IL_DFRAR:
    if (expected_type == NULL)
      expected_type = make_lltype_from_dtype(DT_CPTR);
    goto _process_define_ili;
  case IL_FREECS:
    cse_opc = 1;
  case IL_DFRCS:
    if (expected_type == NULL)
      expected_type = make_lltype_from_dtype(DT_CMPLX);

  _process_define_ili:
    /* llvm_info.curr_ret_ili = ilix; */
    llvm_info.curr_ret_dtype = (cse_opc ? 0 : dtype_from_return_type(opc));
    switch (ILI_OPC(ILI_OPND(ilix, 1))) {

#ifdef PGPLUS
    case IL_JSRA:
#endif
    case IL_QJSR:
    case IL_JSR:
    case IL_GJSR:
      /*
       * For compiler-created functions, its DTYPE record is
       * believable if its dtype value is not a 'predeclared,
       * e.g., DT_IFUNC.
       */
      if (CCSYMG(ILI_OPND(ILI_OPND(ilix, 1), 1)) &&
          DTYPEG(ILI_OPND(ILI_OPND(ilix, 1), 1)) < DT_MAX) {
        update_return_type_for_ccfunc(ILI_OPND(ilix, 1), opc);
      }
    }

    /* Identical calls in the same block must be csed for correctness,
     * identical calls that are supposed to be repeated are given different
     * ILI numbers.
     *
     * Don't cse QJSR/GJSR calls. They are hashed as other instructions, so
     * cse'ing could inadvertently move loads across stores. See
     * pgc_correctll/gf40.c on an architecture that calls __mth_i_floatk
     * with QJSR.
     */
    if (ILI_OPC(ILI_OPND(ilix, 1)) == IL_QJSR ||
        ILI_OPC(ILI_OPND(ilix, 1)) == IL_GJSR) {
      operand = gen_llvm_expr(ILI_OPND(ilix, 1), expected_type);
    } else {
      OPERAND **csed_operand = get_csed_operand(ILI_OPND(ilix, 1));

      if (csed_operand == NULL) {
        operand = gen_llvm_expr(ILI_OPND(ilix, 1), expected_type);
        add_to_cselist(ILI_OPND(ilix, 1));
        csed_operand = get_csed_operand(ILI_OPND(ilix, 1));
        set_csed_operand(csed_operand, operand);
      } else if (!ILI_COUNT(ILI_OPND(ilix, 1))) {
        operand = gen_llvm_expr(ILI_OPND(ilix, 1), expected_type);
      } else if (*csed_operand == NULL) {
        operand = gen_llvm_expr(ILI_OPND(ilix, 1), expected_type);
        set_csed_operand(csed_operand, operand);
      } else {
        operand = gen_copy_op(*csed_operand);
      }
      assert(operand, "null operand in cse list for ilix ", ILI_OPND(ilix, 1),
             4);
    }
    break;
  case IL_FREE:
    if (expected_type == NULL)
      expected_type = make_lltype_from_dtype(ILI_OPND(ilix, 2));
    goto _process_define_ili;
  case IL_VCVTV:
    operand = gen_convert_vector(ilix);
    break;

  case IL_VCVTS:
    operand =
        gen_scalar_to_vector(ilix, make_lltype_from_dtype(ILI_OPND(ilix, 2)));
    break;

  case IL_VNOT:
  case IL_NOT:
  case IL_UNOT:
  case IL_KNOT:
  case IL_UKNOT:
    operand = gen_binary_expr(ilix, I_XOR);
    break;
  case IL_VNEG:
    operand = gen_binary_vexpr(ilix, I_SUB, I_SUB, I_FSUB);
    break;
  case IL_INEG:
  case IL_UINEG:
  case IL_KNEG:
  case IL_UKNEG:
    operand = gen_binary_expr(ilix, I_SUB);
    break;
  case IL_DNEG:
  case IL_FNEG:
    operand = gen_binary_expr(ilix, I_FSUB);
    break;
  case IL_SCMPLXNEG:
  case IL_DCMPLXNEG: {
    OPERAND *res, *op_rneg, *op_ineg, *c1, *cse1;
    LL_Type *cmplx_ty, *cmpnt_ty;
    const DTYPE dt = opc == IL_SCMPLXNEG ? DT_CMPLX : DT_DCMPLX;
    const DTYPE et = opc == IL_SCMPLXNEG ? DT_FLOAT : DT_DBLE;

    cmpnt_ty = make_lltype_from_dtype(dt == DT_CMPLX ? DT_FLOAT : DT_DBLE);

    c1 = gen_eval_cmplx_value(ILI_OPND(ilix, 1), dt);
    cse1 = gen_copy_operand(c1);

    /* real = 0 - real */
    op_rneg = make_constval_op(cmpnt_ty, 0, 0);
    op_rneg->next = gen_extract_value(c1, dt, et, 0);
    op_rneg = ad_csed_instr(I_FSUB, 0, cmpnt_ty, op_rneg, 0, TRUE);

    /* imag = 0 - imag */
    op_ineg = make_constval_op(cmpnt_ty, 0, 0);
    op_ineg->next = gen_extract_value(cse1, dt, et, 1);
    op_ineg = ad_csed_instr(I_FSUB, 0, cmpnt_ty, op_ineg, 0, TRUE);

    /* {real, imag} */
    res = make_undef_op(make_lltype_from_dtype(dt));
    res = gen_insert_value(res, op_rneg, 0);
    operand = gen_insert_value(res, op_ineg, 1);
  } break;
  case IL_CSE:
  case IL_CSEKR:
  case IL_CSEIR:
  case IL_CSESP:
  case IL_CSEDP:
  case IL_CSEAR:
  case IL_CSECS:
  case IL_CSECD:
  {
    int csed_ilix;
    OPERAND **csed_operand;

    csed_ilix = ILI_OPND(ilix, 1);
    if (ILI_ALT(csed_ilix))
      csed_ilix = ILI_ALT(csed_ilix);
    csed_operand = get_csed_operand(csed_ilix);

    assert(csed_operand, "missing cse operand list for ilix ", csed_ilix, 4);
    if (!ILI_COUNT(csed_ilix)) {
      operand = gen_llvm_expr(csed_ilix, expected_type);
    } else {
      operand = gen_copy_op(*csed_operand);
    }
    assert(operand, "null operand in cse list for ilix ", csed_ilix, 4);
  } break;
  case IL_IR2SP:
    operand = make_bitcast(gen_llvm_expr(ILI_OPND(ilix, 1), 0),
                           make_lltype_from_dtype(DT_REAL));
    break;
  case IL_KR2DP:
    operand = make_bitcast(gen_llvm_expr(ILI_OPND(ilix, 1), 0),
                           make_lltype_from_dtype(DT_DBLE));
    break;
  /* these next ILI are currently generated by idiom recognition within
   * induc, and as arguments to our __c_mset* routines we want them treated
   * as integer bits without conversion.
   */
  case IL_SP2IR:
    operand = make_bitcast(gen_llvm_expr(ILI_OPND(ilix, 1), 0),
                           make_lltype_from_dtype(DT_INT));
    break;
  case IL_DP2KR:
    operand = make_bitcast(gen_llvm_expr(ILI_OPND(ilix, 1), 0),
                           make_lltype_from_dtype(DT_INT8));
    break;
  case IL_CS2KR:
    comp_exp_type = make_lltype_from_dtype(DT_CMPLX);
    cc_op1 = gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_CMPLX));

    /* ILI_OPND(ilix, 1) can be expression */
    if (ILTY_CONS == IL_TYPE(ILI_OPC(ILI_OPND(ilix, 1))))
      cc_op2 = make_var_op(ILI_OPND(ILI_OPND(ilix, 1), 1));
    else {
      assert(0, "gen_llvm_expr(): unsupport operand for CS2KR ", opc, 4);
      /* it is not worth it to do it */
    }

    operand =
        make_bitcast(cc_op2, make_ptr_lltype(make_lltype_from_dtype(DT_INT8)));
    operand = make_load(ilix, operand, operand->ll_type->sub_types[0], -2,
                        ldst_instr_flags_from_dtype(DT_INT8));
    break;
  case IL_SCMPLX2REAL:
    dt = DT_CMPLX;
    cmpnt = 0;
    goto component;
  case IL_DCMPLX2REAL:
    dt = DT_DCMPLX;
    cmpnt = 0;
    goto component;
  case IL_SCMPLX2IMAG:
    dt = DT_CMPLX;
    cmpnt = 1;
    goto component;
  case IL_DCMPLX2IMAG:
    dt = DT_DCMPLX;
    cmpnt = 1;
    goto component;
  component:
    c1 = gen_eval_cmplx_value(ILI_OPND(ilix, 1), dt);
    operand =
        gen_extract_value(c1, dt, dt == DT_CMPLX ? DT_FLOAT : DT_DBLE, cmpnt);
    break;
  case IL_SPSP2SCMPLX:
  case IL_DPDP2DCMPLX: {
    LL_Type *dt, *et;
    if (opc == IL_SPSP2SCMPLX) {
      dt = make_lltype_from_dtype(DT_CMPLX);
      et = make_lltype_from_dtype(DT_FLOAT);
    } else {
      dt = make_lltype_from_dtype(DT_DCMPLX);
      et = make_lltype_from_dtype(DT_DBLE);
    }
    cc_op1 = gen_llvm_expr(ILI_OPND(ilix, 1), et);
    cc_op2 = gen_llvm_expr(ILI_OPND(ilix, 2), et);
    operand = make_undef_op(dt);
    operand = gen_insert_value(operand, cc_op1, 0);
    operand = gen_insert_value(operand, cc_op2, 1);
  } break;
  case IL_SPSP2SCMPLXI0:
    dt = DT_CMPLX;
    cmpnt = DT_FLOAT;
    goto component_zero;
  case IL_DPDP2DCMPLXI0:
    dt = DT_DCMPLX;
    cmpnt = DT_DBLE;
    goto component_zero;
  component_zero: /* Set imaginary value to 0 */
    cc_op1 = gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(cmpnt));
    cc_op2 = make_constval_op(make_lltype_from_dtype(cmpnt), 0, 0);
    operand = make_undef_op(make_lltype_from_dtype(dt));
    operand = gen_insert_value(operand, cc_op1, 0);
    operand = gen_insert_value(operand, cc_op2, 1);
    break;
  case IL_SCMPLXCONJG:
    dt = DT_CMPLX;
    cmpnt = DT_FLOAT;
    goto cmplx_conj;
  case IL_DCMPLXCONJG:
    dt = DT_DCMPLX;
    cmpnt = DT_DBLE;
    goto cmplx_conj;
  cmplx_conj:
    /* result = {real , 0 - imag} */
    c1 = gen_eval_cmplx_value(ILI_OPND(ilix, 1), dt);
    cse1 = gen_copy_operand(c1);
    cc_op1 = gen_extract_value(c1, dt, cmpnt, 1);
    cc_op2 = make_constval_op(make_lltype_from_dtype(cmpnt), 0, 0);
    cc_op2->next = cc_op1;
    cc_op2 = ad_csed_instr(I_FSUB, 0, make_lltype_from_dtype(cmpnt), cc_op2, 0,
                           TRUE);
    cc_op1 = gen_extract_value(cse1, dt, cmpnt, 0);
    operand = make_undef_op(make_lltype_from_dtype(dt));
    operand = gen_insert_value(operand, cc_op1, 0);
    operand = gen_insert_value(operand, cc_op2, 1);
    break;
  case IL_FABS:
    operand = gen_call_llvm_intrinsic(
        "fabs.f32",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_FLOAT)),
        make_lltype_from_dtype(DT_FLOAT), NULL, I_PICALL);
    break;
  case IL_VABS:
    intrinsic_name = vect_llvm_intrinsic_name(ilix);
    operand = gen_call_llvm_intrinsic(
        intrinsic_name,
        gen_llvm_expr(ILI_OPND(ilix, 1),
                      make_lltype_from_dtype(ILI_OPND(ilix, 2))),
        make_lltype_from_dtype(ILI_OPND(ilix, 2)), NULL, I_PICALL);
    break;
  case IL_VSQRT:
    intrinsic_name = vect_llvm_intrinsic_name(ilix);
    operand = gen_call_llvm_intrinsic(
        intrinsic_name,
        gen_llvm_expr(ILI_OPND(ilix, 1),
                      make_lltype_from_dtype(ILI_OPND(ilix, 2))),
        make_lltype_from_dtype(ILI_OPND(ilix, 2)), NULL, I_PICALL);
    break;
  case IL_VRSQRT:
#if defined(TARGET_LLVM_POWER)
    intrinsic_name = "ppc.vsx.xvrsqrtesp";
#elif defined(TARGET_LLVM_X8632) || defined(TARGET_LLVM_X8664)
  {
    int vsize;
    dtype = ILI_OPND(ilix, 2); /* get the vector dtype */
    assert(TY_ISVECT(DTY(dtype)), "gen_llvm_expr(): expected vect type",
           DTY(dtype), 4);
    vsize = DTY(dtype + 2);
    if (vsize == 4) {
      intrinsic_name = "x86.sse.rsqrt.ps";
    } else if (vsize == 8) {
      intrinsic_name = "x86.avx.rsqrt.ps.256";
    } else
      assert(0, "gen_llvm_expr(): unexpected vector size", vsize, 4);
  }
#else
    assert(0, "gen_llvm_expr(): unknown target", 0, 4);
#endif
    intrinsic_type = make_lltype_from_dtype(ILI_OPND(ilix, 2));
    operand = gen_call_llvm_intrinsic(
        intrinsic_name, gen_llvm_expr(ILI_OPND(ilix, 1), intrinsic_type),
        intrinsic_type, NULL, I_PICALL);
    break;
  case IL_VRCP:
#if defined(TARGET_LLVM_POWER)
    intrinsic_name = "ppc.vsx.xvresp";
#elif defined(TARGET_LLVM_X8632) || defined(TARGET_LLVM_X8664)
  {
    int vsize;
    dtype = ILI_OPND(ilix, 2); /* get the vector dtype */
    assert(TY_ISVECT(DTY(dtype)), "gen_llvm_expr(): expected vect type",
           DTY(dtype), 4);
    vsize = DTY(dtype + 2);
    if (vsize == 4)
      intrinsic_name = "x86.sse.rcp.ps";
    else if (vsize == 8)
      intrinsic_name = "x86.avx.rcp.ps.256";
    else
      assert(0, "gen_llvm_expr(): unexpected vector size", vsize, 4);
  }
#else
    assert(0, "gen_llvm_expr(): unknown target", 0, 4);
#endif
    intrinsic_type = make_lltype_from_dtype(ILI_OPND(ilix, 2));
    operand = gen_call_llvm_intrinsic(
        intrinsic_name, gen_llvm_expr(ILI_OPND(ilix, 1), intrinsic_type),
        intrinsic_type, NULL, I_PICALL);
    break;
  case IL_VFMA1:
  case IL_VFMA2:
  case IL_VFMA3:
  case IL_VFMA4:
#if defined(TARGET_LLVM_POWER)
    intrinsic_name = vect_power_intrinsic_name(ilix);
#else
    intrinsic_name = vect_llvm_intrinsic_name(ilix);
#endif
    intrinsic_type = make_lltype_from_dtype(ILI_OPND(ilix, 4));
    args = gen_llvm_expr(ILI_OPND(ilix, 1), intrinsic_type);
    args->next = gen_llvm_expr(ILI_OPND(ilix, 2), intrinsic_type);
    args->next->next = gen_llvm_expr(ILI_OPND(ilix, 3), intrinsic_type);
    operand = gen_call_llvm_intrinsic(intrinsic_name, args, intrinsic_type,
                                      NULL, I_PICALL);
    break;
  case IL_VSIN: /* VSIN really only here for testing purposes */
    intrinsic_name = vect_llvm_intrinsic_name(ilix);
    operand = gen_call_llvm_intrinsic(
        intrinsic_name,
        gen_llvm_expr(ILI_OPND(ilix, 1),
                      make_lltype_from_dtype(ILI_OPND(ilix, 2))),
        make_lltype_from_dtype(ILI_OPND(ilix, 2)), NULL, I_PICALL);
    break;
  case IL_DABS:
    operand = gen_call_llvm_intrinsic(
        "fabs.f64",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_DBLE)),
        make_lltype_from_dtype(DT_DBLE), NULL, I_PICALL);
    break;
  case IL_IABS:
  case IL_KABS:
    operand = gen_abs_expr(ilix);
    break;
  case IL_IMIN:
  case IL_UIMIN:
  case IL_KMIN:
  case IL_UKMIN:
  case IL_FMIN:
  case IL_DMIN:
  case IL_IMAX:
  case IL_UIMAX:
  case IL_KMAX:
  case IL_UKMAX:
  case IL_FMAX:
  case IL_DMAX: {
    LL_Type *llTy;
    lhs_ili = ILI_OPND(ilix, 2);
    rhs_ili = ILI_OPND(ilix, 1);
    llTy = make_type_from_opc(opc);
    operand = gen_minmax_expr(ilix, gen_llvm_expr(lhs_ili, llTy),
                              gen_llvm_expr(rhs_ili, llTy));
  } break;
  case IL_VMIN:
  case IL_VMAX: {
    int vect_dtype = ILI_OPND(ilix, 3);
    OPERAND *op1, *op2;
    LL_Type *llTy;
    lhs_ili = ILI_OPND(ilix, 2);
    rhs_ili = ILI_OPND(ilix, 1);
    llTy = make_lltype_from_dtype(vect_dtype);
    op1 = gen_llvm_expr(lhs_ili, llTy);
    op2 = gen_llvm_expr(rhs_ili, llTy);
#if defined(TARGET_LLVM_POWER)
    if ((operand = gen_call_vminmax_power_intrinsic(ilix, op1, op2)) == NULL) {
      operand = gen_minmax_expr(ilix, op1, op2);
    }
#elif defined(TARGET_LLVM_ARM)
    if ((operand = gen_call_vminmax_neon_intrinsic(ilix, op1, op2)) == NULL) {
      operand = gen_minmax_expr(ilix, op1, op2);
    }
#else
    if ((operand = gen_call_vminmax_intrinsic(ilix, op1, op2)) == NULL) {
      operand = gen_minmax_expr(ilix, op1, op2);
    }
#endif
  } break;
  case IL_ISELECT:
  case IL_KSELECT:
  case IL_ASELECT:
  case IL_FSELECT:
  case IL_DSELECT:
  case IL_CSSELECT:
  case IL_CDSELECT:
    operand = gen_select_expr(ilix);
    break;
  case IL_FSQRT:
    operand = gen_call_llvm_intrinsic(
        "sqrt.f32",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_FLOAT)),
        make_lltype_from_dtype(DT_FLOAT), NULL, I_PICALL);
    break;
  case IL_DSQRT:
    operand = gen_call_llvm_intrinsic(
        "sqrt.f64",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_DBLE)),
        make_lltype_from_dtype(DT_DBLE), NULL, I_PICALL);
    break;
  case IL_FLOG:
    operand = gen_call_llvm_intrinsic(
        "log.f32",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_FLOAT)),
        make_lltype_from_dtype(DT_FLOAT), NULL, I_PICALL);
    break;
  case IL_DLOG:
    operand = gen_call_llvm_intrinsic(
        "log.f64",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_DBLE)),
        make_lltype_from_dtype(DT_DBLE), NULL, I_PICALL);
    break;
  case IL_FLOG10:
    operand = gen_call_llvm_intrinsic(
        "log10.f32",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_FLOAT)),
        make_lltype_from_dtype(DT_FLOAT), NULL, I_PICALL);
    break;
  case IL_DLOG10:
    operand = gen_call_llvm_intrinsic(
        "log10.f64",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_DBLE)),
        make_lltype_from_dtype(DT_DBLE), NULL, I_PICALL);
    break;
  case IL_FSIN:
    operand = gen_call_llvm_intrinsic(
        "sin.f32",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_FLOAT)),
        make_lltype_from_dtype(DT_FLOAT), NULL, I_PICALL);
    break;
  case IL_DSIN:
    operand = gen_call_llvm_intrinsic(
        "sin.f64",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_DBLE)),
        make_lltype_from_dtype(DT_DBLE), NULL, I_PICALL);
    break;
  case IL_FTAN:
    operand = gen_call_pgocl_intrinsic(
        "tan_f",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_FLOAT)),
        make_lltype_from_dtype(DT_FLOAT), NULL, I_CALL);
    break;
  case IL_DTAN:
    operand = gen_call_pgocl_intrinsic(
        "tan_d",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_DBLE)),
        make_lltype_from_dtype(DT_DBLE), NULL, I_CALL);
    break;
  case IL_FPOWF:
    operand =
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_FLOAT));
    operand->next =
        gen_llvm_expr(ILI_OPND(ilix, 2), make_lltype_from_dtype(DT_FLOAT));
    operand = gen_call_pgocl_intrinsic(
        "pow_f", operand, make_lltype_from_dtype(DT_FLOAT), NULL, I_CALL);
    break;
  case IL_DPOWD:
    operand = gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_DBLE));
    operand->next =
        gen_llvm_expr(ILI_OPND(ilix, 2), make_lltype_from_dtype(DT_DBLE));
    operand = gen_call_pgocl_intrinsic(
        "pow_d", operand, make_lltype_from_dtype(DT_DBLE), NULL, I_CALL);
    break;
  case IL_DPOWI:
    // TODO: won't work because our builtins expect args in registers (xxm0 in
    // ths case) and
    // the call generated here (with llc) puts the args on the stack
    assert(ILI_ALT(ilix),
           "gen_llvm_expr(): missing ILI_ALT field for DPOWI ili ", ilix, 4);
    operand = gen_llvm_expr(ilix, make_lltype_from_dtype(DT_DBLE));
    break;
  case IL_FCOS:
    operand = gen_call_llvm_intrinsic(
        "cos.f32",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_FLOAT)),
        make_lltype_from_dtype(DT_FLOAT), NULL, I_PICALL);
    break;
  case IL_DCOS:
    operand = gen_call_llvm_intrinsic(
        "cos.f64",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_DBLE)),
        make_lltype_from_dtype(DT_DBLE), NULL, I_PICALL);
    break;
  case IL_FEXP:
    operand = gen_call_llvm_intrinsic(
        "exp.f32",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_FLOAT)),
        make_lltype_from_dtype(DT_FLOAT), NULL, I_PICALL);
    break;
  case IL_DEXP:
    operand = gen_call_llvm_intrinsic(
        "exp.f64",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_DBLE)),
        make_lltype_from_dtype(DT_DBLE), NULL, I_PICALL);
    break;
  case IL_FAND:
    /* bitwise logical AND op. operand has floating-point type
       %copnd1 = bitcast float %opnd1 to iX
       %copnd2 = bitcast float %opnd1 to iX
       %crslt = and iX %copnd1, %copnd2
       %result = bitcast iX %crslt to float
     */
    {
      OPERAND *op3, *op4, *op5, *op6;
      INSTR_LIST *instr2, *instr3;
      unsigned bits = 8 * size_of(DT_FLOAT);
      LL_Type *iTy = make_int_lltype(bits);
      LL_Type *fltTy = make_lltype_from_dtype(DT_FLOAT);
      OPERAND *op1 = gen_llvm_expr(ILI_OPND(ilix, 1), NULL);
      OPERAND *op2 = make_tmp_op(iTy, make_tmps());
      INSTR_LIST *instr1 = gen_instr(I_BITCAST, op2->tmps, iTy, op1);
      ad_instr(ilix, instr1);
      op3 = gen_llvm_expr(ILI_OPND(ilix, 2), NULL);
      op4 = make_tmp_op(iTy, make_tmps());
      instr2 = gen_instr(I_BITCAST, op4->tmps, iTy, op3);
      ad_instr(ilix, instr2);
      op6 = make_tmp_op(fltTy, make_tmps());
      op2->next = op4;
      op5 = ad_csed_instr(I_AND, 0, iTy, op2, 0, FALSE);
      instr3 = gen_instr(I_BITCAST, op6->tmps, fltTy, op5);
      ad_instr(ilix, instr3);
      operand = op6;
    }
    break;
  case IL_RSQRTSS:
#if defined(TARGET_LLVM_POWER)
    operand = gen_call_llvm_intrinsic(
        "ppc.vsx.xsrsqrtesp",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_FLOAT)),
        make_lltype_from_dtype(DT_FLOAT), NULL, I_PICALL);
#endif
#if defined(TARGET_LLVM_X8632) || defined(TARGET_LLVM_X8664)
    {
      /* intrinsic has type <4 x float> -> <4 x float>, so need to build
         and extract from vectors */
      const char *nm = "x86.sse.rsqrt.ss";
      LL_Type *vTy = make_vtype(DT_FLOAT, 4);
      OPERAND *op1 = gen_scalar_to_vector_no_shuffle(ilix, vTy);
      OPERAND *op2 = gen_call_llvm_intrinsic(nm, op1, vTy, NULL, I_PICALL);
      operand = gen_extract_vector(op2, 0);
    }
#endif
    break;
  case IL_RCPSS:
#if defined(TARGET_LLVM_POWER)
    operand = gen_call_llvm_intrinsic(
        "ppc.vsx.xsresp",
        gen_llvm_expr(ILI_OPND(ilix, 1), make_lltype_from_dtype(DT_FLOAT)),
        make_lltype_from_dtype(DT_FLOAT), NULL, I_PICALL);
#endif
#if defined(TARGET_LLVM_X8632) || defined(TARGET_LLVM_X8664)
    {
      /* intrinsic has type <4 x float> -> <4 x float>, so need to build
         and extract from vectors */
      const char *nm = "x86.sse.rcp.ss";
      LL_Type *vTy = make_vtype(DT_FLOAT, 4);
      OPERAND *op1 = gen_scalar_to_vector(ilix, vTy);
      OPERAND *op2 = gen_call_llvm_intrinsic(nm, op1, vTy, NULL, I_PICALL);
      operand = gen_extract_vector(op2, 0);
    }
#endif
    break;
  case IL_VPERMUTE: {
    OPERAND *op1;
    LL_Type *vect_lltype;
    DTYPE vect_dtype = ili_get_vect_type(ilix);
    int mask_ili;

    /* LLVM shufflevector instruction has a mask whose selector takes
     * the concatenation of two vectors and numbers the elements as
     * 0,1,2,3,... from left to right.
     */

    vect_lltype = make_lltype_from_dtype(vect_dtype);
    lhs_ili = ILI_OPND(ilix, 1);
    rhs_ili = ILI_OPND(ilix, 2);
    mask_ili = ILI_OPND(ilix, 3);
    op1 = gen_llvm_expr(lhs_ili, vect_lltype);
    op1->next = gen_llvm_expr(rhs_ili, vect_lltype);
    op1->next->next = gen_llvm_expr(mask_ili, 0);
    operand = ad_csed_instr(I_SHUFFVEC, ilix, vect_lltype, op1, 0, TRUE);
  } break;
  case IL_ATOMICRMWI:
  case IL_ATOMICRMWA:
  case IL_ATOMICRMWKR:
    operand = gen_llvm_atomicrmw_expr(ilix);
    break;
  case IL_CMPXCHG_OLDI:
  case IL_CMPXCHG_OLDKR:
    operand = gen_llvm_cmpxchg_component(ilix, 0);
    break;
  case IL_CMPXCHG_SUCCESS:
    operand = gen_llvm_cmpxchg_component(ilix, 1);
    /* Any widening should do zero-extend, not sign-extend. */
    operand->flags |= OPF_ZEXT;
    break;
  case IL_CMPXCHGI:
  case IL_CMPXCHGKR:
    operand = gen_llvm_cmpxchg(ilix);
    break;
  default:
    DBGTRACE3("### gen_llvm_expr; ilix %d, unknown opcode: %d(%s)\n", ilix, opc,
              IL_NAME(opc))
    assert(0, "gen_llvm_expr(): unknown opcode", opc, 4);
    break;
  } /* End of switch(opc) */

  assert(operand, "gen_llvm_expr(): missing operand", ilix, 4);
  if (!operand->ll_type) {
    DBGTRACE2("# missing type for operand %p (ilix %d)", operand, ilix)
    assert(operand->ll_type, "gen_llvm_expr(): missing type", ilix, 4);
  }
  {
    OPERAND **csed_operand = get_csed_operand(ilix);

    if (csed_operand != NULL)
      set_csed_operand(csed_operand, operand);
  }
  ILI_COUNT(ilix)++;
  if (expected_type) {
    LL_Type *tty1, *tty2;
    ret_match = match_types(expected_type, operand->ll_type);
    switch (ret_match) {
    case MATCH_MEM:
      if ((operand->ll_type->data_type == LL_PTR) &&
          ll_type_int_bits(expected_type)) {
        operand = convert_ptr_to_int(operand, expected_type);
      } else {
        operand = make_bitcast(operand, expected_type);
      }
      break;
    case MATCH_OK:
      if ((operand->ll_type->data_type == LL_VECTOR) &&
          (expected_type->data_type == LL_VECTOR) &&
          (operand->ll_type->sub_types[0] == expected_type->sub_types[0]) &&
          (ll_type_bytes(operand->ll_type) != ll_type_bytes(expected_type))) {
        operand = gen_resized_vect(operand, expected_type->sub_elements, 0);
        break;
      }
      tty1 = expected_type;
      tty2 = operand->ll_type;
      ct = 0;
      while (tty1->data_type == tty2->data_type) {
        if ((tty1->data_type == LL_PTR) || (tty1->data_type == LL_ARRAY)) {
          tty1 = tty1->sub_types[0];
          tty2 = tty2->sub_types[0];
          ct++;
        } else {
          break;
        }
      }
      if (tty1 != tty2) {
        operand = make_bitcast(operand, expected_type);
      }
      break;
    case MATCH_NO:
      /* binop1 points to int of different size than instr_type */
      if (ll_type_int_bits(operand->ll_type) &&
          ll_type_int_bits(expected_type)) {
        operand = convert_int_size(ilix, operand, expected_type);
        break;
      } else if (expected_type->data_type == LL_PTR &&
                 operand->ll_type->data_type == LL_PTR) {
        DBGDUMPLLTYPE("#adding bitcast to match expected type:", expected_type)
        operand = make_bitcast(operand, expected_type);
        break;
      } else if (ll_type_is_fp(expected_type) &&
                 ll_type_is_fp(operand->ll_type)) {
        operand = convert_float_size(operand, expected_type);
        break;
      } else if (ll_type_is_fp(operand->ll_type) &&
                 ll_type_int_bits(expected_type)) {
        operand = convert_float_to_sint(operand, expected_type);
        break;
      } else if (ll_type_int_bits(operand->ll_type) &&
                 (expected_type->data_type == LL_PTR)) {
        operand = convert_int_to_ptr(operand, expected_type);
        break;
      } else if ((operand->ll_type->data_type == LL_PTR) &&
                 ll_type_int_bits(expected_type)) {
        operand = convert_ptr_to_int(operand, expected_type);
        break;
      } else if (ll_type_int_bits(operand->ll_type) &&
                 ll_type_is_fp(expected_type)) {
        assert(ll_type_bytes(operand->ll_type) == ll_type_bytes(expected_type),
               "bitcast with differing sizes",
               ll_type_bytes(operand->ll_type) - ll_type_bytes(expected_type),
               ERR_Fatal);
        operand = make_bitcast(operand, expected_type);
        break;
      }
    default:
      assert(0, "gen_llvm_expr(): bad match type for operand", ret_match, 4);
    }
  }

  DBGDUMPLLTYPE("#returned type: ", operand->ll_type);
  DBGTRACEOUT2(" returns operand %p, count %d", operand, ILI_COUNT(ilix));
  setTempMap(ilix, operand);
  return operand;
} /* gen_llvm_expr */

static char *
vect_power_intrinsic_name(int ilix)
{
  int vnum, dtype;
  dtype = ILI_OPND(ilix, 4); /* get the vector dtype */
  assert(TY_ISVECT(DTY(dtype)),
         "vect_power_intrinsic_name(): expected vect type", DTY(dtype), 4);
  vnum = DTY(dtype + 2); /* the number of vector elements */
  switch (ILI_OPC(ilix)) {
  case IL_VFMA1:
    if (vnum == 4)
      return "ppc.vsx.xvmaddasp";
    else if (vnum == 2)
      return "ppc.vsx.xvmaddadp";
    else
      assert(0, "vect_power_intrinsic_name(): bad size", vnum, 4);
    break;
  case IL_VFMA2:
    if (vnum == 4)
      return "ppc.vsx.xvmsubasp";
    else if (vnum == 2)
      return "ppc.vsx.xvmsubadp";
    else
      assert(0, "vect_power_intrinsic_name(): bad size", vnum, 4);
    break;
  case IL_VFMA3:
    if (vnum == 4)
      return "ppc.vsx.xvnmsubasp";
    else if (vnum == 2)
      return "ppc.vsx.xvnmsubadp";
    else
      assert(0, "vect_power_intrinsic_name(): bad size", vnum, 4);
    break;
  case IL_VFMA4:
    if (vnum == 4)
      return "ppc.vsx.xvnmaddasp";
    else if (vnum == 2)
      return "ppc.vsx.xvnmaddadp";
    else
      assert(0, "vect_power_intrinsic_name(): bad size", vnum, 4);
    break;
  default:
    assert(0, "vect_power_intrinsic_name(): bad fma opc", ILI_OPC(ilix), 4);
  }
  return NULL;
}

static char *
vect_llvm_intrinsic_name(int ilix)
{
  int type, n, fsize, dtype;
  ILI_OP opc = ILI_OPC(ilix);
  char *basename, *retc;
  assert(IL_VECT(opc), "vect_llvm_intrinsic_name(): not vect ili", ilix, 4);
  if (opc == IL_VFMA1)
    dtype = ILI_OPND(ilix, 4);
  else
    dtype = ILI_OPND(ilix, 2);

  assert(DTY(dtype) == TY_VECT, "vect_llvm_intrinsic_name(): not vect dtype",
         DTY(dtype), 4);
  type = DTY(dtype + 1);
  retc = (char *)getitem(LLVM_LONGTERM_AREA, 20);
  n = DTY(dtype + 2);
  switch (opc) {
  case IL_VSQRT:
    basename = "sqrt";
    break;
  case IL_VABS:
    basename = "fabs";
    break;
  case IL_VFMA1:
    basename = "fma";
    break;
  case IL_VSIN: /* VSIN here for testing purposes */
    basename = "sin";
    break;
  default:
    assert(0, "vect_llvm_intrinsic_name(): unhandled opc", opc, 4);
  }
  switch (type) {
  case DT_FLOAT:
    fsize = 32;
    break;
  case DT_DBLE:
    fsize = 64;
    break;
  default:
    assert(0, "vect_llvm_intrinsic_name(): unhandled type", type, 4);
  }

  sprintf(retc, "%s.v%df%d", basename, n, fsize);

  return retc;
} /* vect_llvm_intrinsic_name */

static OPERAND *
gen_comp_operand(OPERAND *operand, ILI_OP opc, int lhs_ili, int rhs_ili,
                 int cc_ili, int cc_type, int itype)
{
  return gen_optext_comp_operand(operand, opc, lhs_ili, rhs_ili, cc_ili,
                                 cc_type, itype, 1, 0);
}

/**
   \brief Generate comparison operand. Optionally extending the result.
   \param optext  if this is false, do not extend the result to 32 bits.
 */
static OPERAND *
gen_optext_comp_operand(OPERAND *operand, ILI_OP opc, int lhs_ili, int rhs_ili,
                        int cc_ili, int cc_type, int itype, int optext,
                        int ilix)
{
  LL_Type *expected_type, *op_type;
  INSTR_LIST *Curr_Instr;
  int dtype, vsize;

  operand->ot_type = OT_TMP;
  operand->tmps = make_tmps();

  operand->ll_type = make_int_lltype(1);
  if (opc == IL_VCMPNEQ) {
    assert(ilix, "gen_optext_comp_operand(): missing ilix", 0, 4);
    dtype = ILI_OPND(ilix, 3);
    vsize = DTY(dtype + 2);
    op_type = operand->ll_type;
    operand->ll_type = make_vector_lltype(vsize, op_type);
  }

  /* now make the new binary expression */
  Curr_Instr =
      gen_instr(itype, operand->tmps, operand->ll_type, make_operand());
  Curr_Instr->operands->ot_type = OT_CC;
  Curr_Instr->operands->val.cc = convert_to_llvm_cc(cc_ili, cc_type);
  if (opc == IL_VCMPNEQ)
    Curr_Instr->operands->ll_type = expected_type =
        make_lltype_from_dtype(dtype);
  else
    Curr_Instr->operands->ll_type = expected_type = make_type_from_opc(opc);
  Curr_Instr->operands->next = gen_llvm_expr(lhs_ili, expected_type);
  if (opc == IL_ACMPZ || opc == IL_ACMP) {
    LL_Type *ty0 = Curr_Instr->operands->next->ll_type;
    OPERAND *opTo = Curr_Instr->operands->next;
    opTo->next = gen_base_addr_operand(rhs_ili, ty0);
  } else {
    Curr_Instr->operands->next->next = gen_llvm_expr(rhs_ili, expected_type);
  }
  if (opc == IL_ACMPZ)
    Curr_Instr->operands->next->next->flags |= OPF_NULL_TYPE;

  ad_instr(0, Curr_Instr);
  if (!optext)
    return operand;
  /* Result type is LOGICAL which is signed, -1 for true, 0 for false. */
  return sign_extend_int(operand, 32);
}

/*
 * Given an ilix that is either an IL_JMPM or an IL_JMPMK, generate and insert
 * the corresponding switch instruction.
 *
 * Return the switch instruction.
 */
static INSTR_LIST *
gen_switch(int ilix)
{
  int is_64bit = FALSE;
  LL_Type *switch_type;
  INSTR_LIST *instr;
  OPERAND *last_op;
  int switch_sptr;
  int sw_elt;

  switch (ILI_OPC(ilix)) {
  case IL_JMPM:
    is_64bit = FALSE;
    break;
  case IL_JMPMK:
    is_64bit = TRUE;
    break;
  default:
    interr("gen_switch(): Unexpected jump ili", ilix, 4);
  }

  instr = make_instr(I_SW);
  switch_type = make_int_lltype(is_64bit ? 64 : 32);

  /*
   * JMPM  irlnk1 irlnk2 sym1 sym2
   * JMPMK krlnk1 irlnk2 sym1 sym2
   *
   * irlnk1 / krlnk1 is the value being switched on.
   * irlnk2 is the table size.
   * sym1 is the label for the memory table.
   * sym2 is the default label.
   *
   * Produce: switch <expr>, <default> [value, label]+
   */
  instr->operands = last_op = gen_llvm_expr(ILI_OPND(ilix, 1), switch_type);

  switch_sptr = ILI_OPND(ilix, 3);

  /* Add the default case. */
  last_op->next = make_target_op(DEFLABG(switch_sptr));
  last_op = last_op->next;

  /* Get all the switch elements out of the switch_base table. */
  for (sw_elt = SWELG(switch_sptr); sw_elt; sw_elt = switch_base[sw_elt].next) {
    OPERAND *label = make_target_op(switch_base[sw_elt].clabel);
    OPERAND *value;
    if (is_64bit)
      value = make_constsptr_op(switch_base[sw_elt].val);
    else
      value = make_constval32_op(switch_base[sw_elt].val);
    /* Remaining switch operands are (value, target) pairs. */
    last_op->next = value;
    value->next = label;
    last_op = label;
  }

  ad_instr(ilix, instr);
  return instr;
}

static int
add_to_cselist(int ilix)
{
  CSED_ITEM *csed;

  if (ILI_ALT(ilix))
    ilix = ILI_ALT(ilix);

  DBGTRACE1("#adding to cse list ilix %d", ilix)

  for (csed = csedList; csed; csed = csed->next) {
    if (ilix == csed->ilix) {
      DBGTRACE2("#ilix %d already in cse list, count %d", ilix, ILI_COUNT(ilix))
      return 1;
    }
  }
  csed = (CSED_ITEM *)getitem(LLVM_LONGTERM_AREA, sizeof(CSED_ITEM));
  memset(csed, 0, sizeof(CSED_ITEM));
  csed->ilix = ilix;
  csed->next = csedList;
  csedList = csed;
  build_csed_list(ilix);

  return 0;
}

static void
set_csed_operand(OPERAND **csed_operand, OPERAND *operand)
{
  if (operand->tmps) {
    OPERAND *new_op;

    DBGTRACE("#set_csed_operand using tmps created for operand")
    if (*csed_operand && (*csed_operand)->tmps &&
        (*csed_operand)->tmps != operand->tmps) {
      DBGTRACE("#tmps are different")
    }
    new_op = make_tmp_op(operand->ll_type, operand->tmps);
    *csed_operand = new_op;
  } else {
    DBGTRACE("#set_csed_operand replace csed operand")
    *csed_operand = operand;
  }
}

static void
clear_csed_list(void)
{
  CSED_ITEM *csed;

  for (csed = csedList; csed; csed = csed->next) {
    ILI_COUNT(csed->ilix) = 0;
    csed->operand = NULL;
  }
}

static void
remove_from_csed_list(int ili)
{
  int i, noprs;
  ILI_OP opc;
  CSED_ITEM *csed;

  opc = ILI_OPC(ili);
  for (csed = csedList; csed; csed = csed->next) {
    if (is_cseili_opcode(ILI_OPC(ili)))
      return;
    if (ili == csed->ilix) {
      DBGTRACE1("#remove_from_csed_list ilix(%d)", ili)
      ILI_COUNT(ili) = 0;
      csed->operand = NULL;
    }
  }

  noprs = ilis[opc].oprs;
  for (i = 1; i <= noprs; ++i) {
    if (IL_ISLINK(opc, i))
      remove_from_csed_list(ILI_OPND(ili, i));
  }
}

static OPERAND **
get_csed_operand(int ilix)
{
  CSED_ITEM *csed;

  if (ILI_ALT(ilix))
    ilix = ILI_ALT(ilix);
  for (csed = csedList; csed; csed = csed->next) {
    if (ilix == csed->ilix) {
      OPERAND *p = csed->operand;

      if (p != NULL) {
        int sptr = p->val.sptr;
        DBGTRACE3(
            "#get_csed_operand for ilix %d, operand found %p, with type (%s)",
            ilix, p, OTNAMEG(p))
        DBGDUMPLLTYPE("cse'd operand type ", p->ll_type)
      } else {
        DBGTRACE1("#get_csed_operand for ilix %d, operand found is null", ilix);
      }
      return &csed->operand;
    }
  }

  DBGTRACE1("#get_csed_operand for ilix %d not found", ilix)

  return NULL;
}

static void
build_csed_list(int ilix)
{
  int i, noprs;
  ILI_OP opc;

  opc = ILI_OPC(ilix);

  if (is_cseili_opcode(opc)) {
    int csed_ilix = ILI_OPND(ilix, 1);
    if (ILI_ALT(csed_ilix))
      csed_ilix = ILI_ALT(csed_ilix);
    if (add_to_cselist(csed_ilix))
      return;
  }

  noprs = ilis[opc].oprs;
  for (i = 1; i <= noprs; ++i) {
    if (IL_ISLINK(opc, i))
      build_csed_list(ILI_OPND(ilix, i));
  }
}

static int
convert_to_llvm_cc(int cc, int cc_type)
{
  int ret_code;

  switch (cc) {
  case CC_EQ:
  case CC_NOTNE:
    if (cc_type & CMP_INT)
      ret_code = LLCC_EQ;
    else
      ret_code = LLCCF_OEQ;
    break;
  case CC_NE:
  case CC_NOTEQ:
    if (cc_type & CMP_INT)
      ret_code = LLCC_NE;
    else {
      if (IEEE_CMP)
        ret_code = LLCCF_UNE;
      else
        ret_code = LLCCF_ONE;
    }
    break;
  case CC_LT:
  case CC_NOTGE:
    if (cc_type & CMP_INT)
      ret_code = (cc_type & CMP_USG) ? LLCC_ULT : LLCC_SLT;
    else {
      if (IEEE_CMP && cc == CC_NOTGE && float_jmp)
        ret_code = LLCCF_ULT;
      else
        ret_code = LLCCF_OLT;
    }
    break;
  case CC_GE:
  case CC_NOTLT:
    if (cc_type & CMP_INT)
      ret_code = (cc_type & CMP_USG) ? LLCC_UGE : LLCC_SGE;
    else {
      if (IEEE_CMP && cc == CC_NOTLT && float_jmp)
        ret_code = LLCCF_UGE;
      else
        ret_code = LLCCF_OGE;
    }
    break;
  case CC_LE:
  case CC_NOTGT:
    if (cc_type & CMP_INT)
      ret_code = (cc_type & CMP_USG) ? LLCC_ULE : LLCC_SLE;
    else {
      if (IEEE_CMP && cc == CC_NOTGT && float_jmp)
        ret_code = LLCCF_ULE;
      else
        ret_code = LLCCF_OLE;
    }
    break;
  case CC_GT:
  case CC_NOTLE:
    if (cc_type & CMP_INT)
      ret_code = (cc_type & CMP_USG) ? LLCC_UGT : LLCC_SGT;
    else {
      if (IEEE_CMP && cc == CC_NOTLE && float_jmp)
        ret_code = LLCCF_UGT;
      else
        ret_code = LLCCF_OGT;
    }
    break;
  default:
    assert(0, "convert_to_llvm_cc, unknown condition code", cc, 4);
  }

  if (IEEE_CMP && fcmp_negate)
    ret_code = fnegcc[ret_code];
  return ret_code;
} /* convert_to_llvm_cc */

static void
add_global_define(GBL_LIST *gitem)
{
  GBL_LIST *gl;

  DBGTRACEIN2(": '%s', (sptr %d)", gitem->global_def, gitem->sptr);

  /* make sure the global def for this sptr has not already been added;
   * can occur with -Mipa=inline that multiple versions exist.
   */
  if (!check_global_define(gitem)) {
    if (Globals) {
      llvm_info.last_global->next = gitem;
    } else {
      Globals = gitem;
    }
    llvm_info.last_global = gitem;
    if (flg.debug) {
      if (gitem->sptr && ST_ISVAR(STYPEG(gitem->sptr)) &&
          !CCSYMG(gitem->sptr)) {
        LL_Type *type = make_lltype_from_sptr(gitem->sptr);
        LL_Value *value = ll_create_value_from_type(cpu_llvm_module, type,
                                                    SNAME(gitem->sptr));
        lldbg_emit_global_variable(cpu_llvm_module->debug_info, gitem->sptr, 0,
                                   1, value);
      }
    }
  }

  DBGTRACEOUT("");
} /* add_global_define */

static LOGICAL
check_global_define(GBL_LIST *cgl)
{
  GBL_LIST *gl, *gitem;

  for (gl = recorded_Globals; gl; gl = gl->next) {
    if (gl->sptr > 0 && gl->sptr == cgl->sptr) {
      DBGTRACE1("#sptr %d already in Global list; exiting", gl->sptr)
      return TRUE;
    }
  }
  gitem = (GBL_LIST *)getitem(LLVM_LONGTERM_AREA, sizeof(GBL_LIST));
  memset(gitem, 0, sizeof(GBL_LIST));
  gitem->sptr = cgl->sptr;
  gitem->global_def = cgl->global_def;
  gitem->next = recorded_Globals;
  recorded_Globals = gitem;
  return FALSE;
} /* check_global_define */

void
update_external_function_declarations(char *decl, unsigned int flags)
{
  EXFUNC_LIST *efl;
  char *gname;

  gname = (char *)getitem(LLVM_LONGTERM_AREA, strlen(decl) + 1);
  strcpy(gname, decl);
  efl = (EXFUNC_LIST *)getitem(LLVM_LONGTERM_AREA, sizeof(EXFUNC_LIST));
  memset(efl, 0, sizeof(EXFUNC_LIST));
  efl->func_def = gname;
  efl->flags |= flags;
  add_external_function_declaration(efl);
}

static void
add_external_function_declaration(EXFUNC_LIST *exfunc)
{
  const int sptr = exfunc->sptr;

  if (sptr) {
    LL_ABI_Info *abi =
        ll_abi_for_func_sptr(cpu_llvm_module, sptr, DTYPEG(sptr));
    ll_proto_add_sptr(sptr, abi);
    if (exfunc->flags & EXF_INTRINSIC)
      ll_proto_set_intrinsic(ll_proto_key(sptr), exfunc->func_def);
#ifdef WEAKG
    if (WEAKG(sptr))
      ll_proto_set_weak(ll_proto_key(sptr), TRUE);
#endif
  } else {
    assert(exfunc->func_def && (exfunc->flags & EXF_INTRINSIC),
           "Invalid external function descriptor", 0, 4);
    ll_proto_add(exfunc->func_def, NULL);
    ll_proto_set_intrinsic(exfunc->func_def, exfunc->func_def);
// get_intrin_ag(exfunc->func_def);
  }
} /* add_external_function_declaration */

/* Get an operand representing an intrinsic function with the given name and
 * function type.
 *
 * Create an external function declaration if necessary, or verify that the
 * requested function type matches previous uses.
 *
 * Use this to generate calls to LLVM intrinsics or runtime library functions.
 *
 * The function name should include the leading '@'. The pointer will not be
 * copied.
 */
static OPERAND *
get_intrinsic(const char *name, LL_Type *func_type)
{
  hash_data_t old_type = NULL;
  OPERAND *op;

  if (hashmap_lookup(llvm_info.declared_intrinsics, name, &old_type)) {
    assert(old_type == func_type,
           "Intrinsic already declared with different signature", 0, 4);
  } else {
    /* First time we see this intrinsic. */
    int i;
    char *decl = (char *)getitem(LLVM_LONGTERM_AREA,
                                 strlen(name) + strlen(func_type->str) + 50);
    if (!strncmp(name, "asm ", 4)) {
      /* do nothing - CALL asm() */
    } else {
      sprintf(decl, "declare %s %s(", func_type->sub_types[0]->str, name);
      for (i = 1; i < func_type->sub_elements; i++) {
        if (i > 1)
          strcat(decl, ", ");
        strcat(decl, func_type->sub_types[i]->str);
      }
      strcat(decl, ")");
      update_external_function_declarations(decl, EXF_INTRINSIC);
      hashmap_insert(llvm_info.declared_intrinsics, name, func_type);
    }
  }

  op = make_operand();
  op->ot_type = OT_CALL;
  op->ll_type = make_ptr_lltype(func_type);
  op->string = (char *)name;
  return op;
}

/**
   \brief Prepend the callee to a list of operands for an intrinsic call

   When preparing a call to <code>float @llvm.foo(i32 %a, i32 %b)</code>, pass
   the \c %a, \c %b operands to
   <code>get_intrinsic_call_ops("@llvm.foo", float, a_b_ops);</code>
 */
static OPERAND *
get_intrinsic_call_ops(const char *name, LL_Type *return_type, OPERAND *args)
{
  LL_Type *func_type = make_function_type_from_args(return_type, args, FALSE);
  OPERAND *op = get_intrinsic(name, func_type);
  op->next = args;
  return op;
}

#define OCTVAL(v) ((v >= 48) && (v <= 55))

static int
decimal_value_from_oct(int c, int b, int a)
{
  int val, vc, vb, va;

  vc = c - 48;
  vb = b - 48;
  va = a - 48;
  val = vc * 64 + vb * 8 + va;
  return val;
} /* decimal value */

/**
   \brief Format a string for LLVM output

   LLVM uses hex ASCII characters in strings in place of escape sequences. So
   process the string here making all needed replacements.
 */
static char *
process_string(char *name, int pad, int string_length)
{
  int i, value, remain, count = 0;
  char *new_name;
  int len = strlen(name);

  new_name =
      (char *)getitem(LLVM_LONGTERM_AREA, 3 * len * sizeof(char) + 2 + 3 * pad);
  DBGTRACEIN4(" arg name: %s, pad: %d, len: %d, string_length %d", name, pad,
              len, string_length)

  for (i = 0; i <= len; i++) {
    if (name[i] == 92 && i < len) /* backslash that might be an escape */
    {
      switch (name[i + 1]) {
      case 39: /* \' in string => ' */
        new_name[count++] = name[i + 1];
        i++;
        break;
      case 48: /* look for octal values */
      case 49:
      case 50:
      case 51:
      case 52:
      case 53:
      case 54:
      case 55:
        if (i <= len - 2 && OCTVAL(name[i + 2]) && OCTVAL(name[i + 3])) {
          value = decimal_value_from_oct(name[i + 1], name[i + 2], name[i + 3]);
          remain = value % 16;
          value = value / 16;
          new_name[count++] = name[i]; /* copy the \ character */
          if (value < 10)
            new_name[count++] = 48 + value;
          else
            new_name[count++] = 55 + value;
          if (remain < 10)
            new_name[count++] = 48 + remain;
          else
            new_name[count++] = 55 + remain;
          i += 3;
        } else
          new_name[count++] = name[i]; /* copy the \ character */
        break;
      case 97: /* bell character (bel) - \a in string => \07 */
        new_name[count++] = name[i]; /* copy the \ character */
        new_name[count++] = 48;
        new_name[count++] = 55;
        i++;
        break;
      case 98:                       /* backspace (bs) - \b in string => \08 */
        new_name[count++] = name[i]; /* copy the \ character */
        new_name[count++] = 48;
        new_name[count++] = 56;
        i++;
        break;
      case 116: /* horizontal tab (ht) - \t in string => \09 */
        new_name[count++] = name[i]; /* copy the \ character */
        new_name[count++] = 48;
        new_name[count++] = 57;
        i++;
        break;
      case 110:                      /* newline (nl) - \n in string => \0a */
        new_name[count++] = name[i]; /* copy the \ character */
        new_name[count++] = 48;
        new_name[count++] = 97;
        i++;
        break;
      case 102:                      /* form feed (np) - \f in string => \0c */
        new_name[count++] = name[i]; /* copy the \ character */
        new_name[count++] = 48;
        new_name[count++] = 99;
        i++;
        break;
      case 114: /* carriage return (cr) - \r in string => \0d */
        new_name[count++] = name[i]; /* copy the \ character */
        new_name[count++] = 48;
        new_name[count++] = 100;
        i++;
        break;
      case 34:                       /* quote character - \" in string => \22 */
        new_name[count++] = name[i]; /* copy the \ character */
        new_name[count++] = 50;
        new_name[count++] = 50;
        i++;
        break;
      case 92: /* backslash  character - \\ in string => \5C */
        new_name[count++] = name[i]; /* copy the \ character */
        new_name[count++] = 53;
        new_name[count++] = 67;
        i++;
        break;
      default:                       /* don't do anything */
        new_name[count++] = name[i]; /* copy the \ character */
        break;
      }
    } else {
      switch (name[i]) {
      case 10:
        new_name[count++] = 92; /* copy the \ character */
        new_name[count++] = 48;
        new_name[count++] = 97;
        break;
      case 34:
        if (i && i != (len - 1)) {
          new_name[count++] = 92; /* copy the \ character */
          new_name[count++] = '2';
          new_name[count++] = '2';
          break;
        }
      default:
        new_name[count++] = name[i];
      }
    }
  }
  len = strlen(new_name);
  /* add any needed padding */
  for (i = 0; i < pad; i++) {
    new_name[len + (i * 3 - 1)] = 92; /* \ */
    new_name[len + (i * 3)] = 48;     /* 0 */
    new_name[len + (i * 3 + 1)] = 48; /* 0 */
  }

  if (pad) /* if needed, fix up the end of the padding */
  {
    new_name[len + (3 * pad - 1)] = 34; /* " */
    new_name[len + (3 * pad)] = 0;      /* '\0' */
  }

  len = strlen(new_name);
  /* need to have the string end with \00" unless tight
   * character array initialization.
   */
  if (!string_length || len - 2 != string_length) {
    new_name[len - 1] = 92; /* \ */
    new_name[len] = 48;     /* 0 */
    new_name[len + 1] = 48; /* 0 */
    new_name[len + 2] = 34; /* " */
    new_name[len + 3] = 0;  /* '\0' */
  }

  DBGTRACEOUT1(" returns '%s'", new_name)

  return new_name;
} /* process_string */

/** 
    \brief Get string name for a struct type
    \param dtype  dtype index
    \return string containing dtype name
 */
char *
dtype_struct_name(int dtype)
{
  char *dtype_str = process_dtype_struct(dtype);
  return dtype_str;
}
static int count = 0;

/* Set the LLVM name of a global sptr to '@' + name.
 *
 * This is appropriate for external identifiers and internal identifiers with a
 * module-unique name.
 */
static char *
set_global_sname(int sptr, const char *name)
{
  SNAME(sptr) = (char *)getitem(LLVM_LONGTERM_AREA, strlen(name) + 2);
  sprintf(SNAME(sptr), "@%s", name);
  return SNAME(sptr);
}

/* Set the LLVM name of a global sptr to '@' + name + '.' + sptr.
 *
 * This is appropriate for internal globals that don't have a unique name
 * because they belong to some scope. The sptr suffix makes the name unique.
 */
static char *
set_numbered_global_sname(int sptr, const char *name)
{
  SNAME(sptr) = (char *)getitem(LLVM_LONGTERM_AREA, strlen(name) + 12);
  sprintf(SNAME(sptr), "@%s.%d", name, sptr);
  return SNAME(sptr);
}

/* Set the LLVM name of a local sptr to '%' + name.
 *
 * This is appropriate for function-local identifiers.
 */
static char *
set_local_sname(int sptr, const char *name)
{
  SNAME(sptr) = (char *)getitem(LLVM_LONGTERM_AREA, strlen(name) + 2);
  sprintf(SNAME(sptr), "%%%s", name);
  return SNAME(sptr);
}

/* Create an LLVM initializer for a global define and record it as
 * gitem->global_def.
 *
 * This will either use the dinit_string() or an appropriate zero-initializer.
 *
 * The flag_str modifies the global variable linkage, visibility, and other
 * flags.
 *
 * The type_str is the name of of global type as returned from char_type().
 *
 */
static void
create_global_initializer(GBL_LIST *gitem, const char *flag_str,
                          const char *type_str)
{
  int dty, stype;
  int sptr = gitem->sptr;
  const char *initializer;
  char *gname;

  assert(sptr, "gitem must be initialized", 0, 4);
  assert(gitem->global_def == NULL, "gitem already has an initializer", sptr,
         4);
  assert(SNAME(sptr), "sptr must have an LLVM name", sptr, 4);

  /* Create an initializer string. */
  if (DINITG(sptr))
    return;

  dty = DTY(DTYPEG(sptr));
  stype = STYPEG(sptr);

  if (
      (stype == ST_VAR && dty == TY_PTR))
    initializer = "null";
  else if (AGGREGATE_STYPE(stype) || COMPLEX_DTYPE(DTYPEG(sptr)) ||
           VECTOR_DTYPE(DTYPEG(sptr)))
    initializer = "zeroinitializer";
  else if (stype == ST_VAR && TY_ISREAL(dty))
    initializer = "0.0";
  else
    initializer = "0";
  gname = (char *)getitem(LLVM_LONGTERM_AREA,
                          strlen(SNAME(sptr)) + strlen(flag_str) +
                          strlen(type_str) + strlen(initializer) + 8);
  sprintf(gname, "%s = %s %s %s", SNAME(sptr), flag_str, type_str, initializer);
  gitem->global_def = gname;
}

/**
   \brief Get the alignment in bytes of a symbol representing a variable
 */
static unsigned
align_of_var(int sptr)
{
  if (!PDALN_IS_DEFAULT(sptr))
    return 1u << PDALNG(sptr);
  if (DTYPEG(sptr))
    return align_of(DTYPEG(sptr));
  if (STYPEG(sptr) == ST_PROC) /* No DTYPE */
    return align_of(DT_ADDR);
  return 0;
}

/**
   \brief Separate symbols that should NOT have debug information
   \param sptr  a symbol
   \return false iff \p sptr ought NOT to have debug info
 */
INLINE static bool
needDebugInfoFilt(SPTR sptr)
{
  if (!sptr)
    return true;
  /* Fortran case needs to be revisited when we start to support debug, for now
   * just the obvious case */
  return DCLDG(sptr);
}

/**
   \brief Determine if debug information is needed for a particular symbol
   \param sptr  The symbol

   Checks debug flags and symbol properties.
 */
INLINE static bool
need_debug_info(SPTR sptr)
{
  /* Start with checking debug flags */
  return flg.debug && cpu_llvm_module->debug_info && needDebugInfoFilt(sptr);
}

static void
addDebugForGlobalVar(SPTR sptr, ISZ_T off)
{
  if (need_debug_info(sptr)) {
    LL_Module *mod = cpu_llvm_module;
    /* TODO: defeat unwanted side-effects. make_lltype_from_sptr() will update
       the LLTYPE() type (sptr_type_array) along some paths. This may be
       undesirable at this point, because the array gets updated with an
       unexpected/incorrect type. Work around this buggy behavior by caching and
       restoring the type value.  Figure out why this works the way it does. */
    LL_Type *cache = LLTYPE(sptr);
    LL_Type *type = make_lltype_from_sptr(sptr);
    LL_Value *value = ll_create_value_from_type(mod, type, SNAME(sptr));
    lldbg_emit_global_variable(mod->debug_info, sptr, off, 1, value);
    LLTYPE(sptr) = cache;
  }
}

/**
   \brief process \c SC_STATIC \p sptr representing a file-local variable
   \param sptr  A symbol
 */
static void
process_static_sptr(SPTR sptr, ISZ_T off)
{
  GBL_LIST *gitem;
  const char *retc;
  char name[MAXIDLEN];
  int stype = STYPEG(sptr);
  const char *type_str = IS_TLS(sptr) ?
    "internal thread_local global" : "internal global";

  assert(SCG(sptr) == SC_STATIC, "Expected static variable sptr", sptr, 4);
  assert(SNAME(sptr) == NULL, "Already precessed sptr", sptr, 4);

  set_global_sname(sptr, get_llvm_name(sptr));
  sym_is_refd(sptr);

  if ((STYPEG(sptr) != ST_ENTRY) && (STYPEG(sptr) != ST_PROC)) {
    if ((STYPEG(sptr) != ST_CONST) && (STYPEG(sptr) != ST_PARAM))
      addDebugForGlobalVar(sptr, off);
  }

}

static bool
is_blockaddr_store(int ilix, int rhs, int lhs)
{
  if (ILI_OPC(rhs) == IL_AIMV || ILI_OPC(rhs) == IL_AKMV)
    rhs = ILI_OPND(rhs, 1);

  if (ILI_OPC(rhs) == IL_ACEXT) {
    int gl_sptr, ili, newnme;
    int nme = ILI_OPND(ilix, 3);
    int sptr = basesym_of(nme);
    int label = CONVAL1G(ILI_OPND(rhs, 1));
    process_sptr(label);
    gl_sptr = process_blockaddr_sptr(sptr, label);

    /* MSZ could be 64 if it is 64-bit */
    ili = ad_acon(gl_sptr, 0);
    STYPEP(gl_sptr, ST_VAR);
    newnme = addnme(NT_VAR, gl_sptr, 0, (INT)0);
    ili = ad3ili(IL_LD, ili, newnme, MSZ_WORD);

    ili = ad4ili(IL_ST, ili, lhs, nme, MSZ_WORD);
    make_stmt(STMT_ST, ili, 0, 0, 0);

    return true;
  }
  return false;
}

/**
   \brief Process block address symbol

   We want to generate something similar to:
   \verbatim
     @MAIN_iab = internal global  i8*  blockaddress(@MAIN_, %L.LB1_351)

   MAIN_:
      %0 = load i8** @iab2
      store i8* %0, i8** %iab
      ; next instruction is use when branching
      ; indirectbr i8** %iab, [label %the_label]
   \endverbatim
  */
static int
process_blockaddr_sptr(int sptr, int label)
{
  int gl_sptr;
  char *curfnm = getsname(gbl.currsub);
  char *sptrnm = SYMNAME(sptr);
  int size = strlen(curfnm) + strlen(sptrnm);

  DEBUG_ASSERT(size <= MXIDLN, "strcat exceeds available space");
  gl_sptr = getsymbol(strcat(curfnm, sptrnm));
  DTYPEP(gl_sptr, DT_CPTR);
  STYPEP(gl_sptr, ST_VAR);
  SCP(gl_sptr, SC_EXTERN);
  ADDRESSP(gl_sptr, 0);
  CCSYMP(gl_sptr, 1);

  if (SNAME(gl_sptr) == NULL) {
    LL_Type* ttype;
    char *sname, *gname, *retc, *labelName;
    GBL_LIST *gitem = (GBL_LIST *)getitem(LLVM_LONGTERM_AREA, sizeof(GBL_LIST));
    memset(gitem, 0, sizeof(GBL_LIST));
    gitem->sptr = gl_sptr;

    sname = (char *)getitem(LLVM_LONGTERM_AREA, strlen(SYMNAME(gl_sptr)));
    sprintf(sname, "@%s", SYMNAME(gl_sptr));
    SNAME(gl_sptr) = sname;
    ttype = make_lltype_sz4v3_from_sptr(gl_sptr);
    LLTYPE(gl_sptr) = ttype;

    size = size + 80;

    retc = (char *)char_type(DTYPEG(gl_sptr), gl_sptr);
    // FIXME: should use snprintf or check. How do we know +80 is big enough?
    gname = (char *)getitem(LLVM_LONGTERM_AREA, size);
    labelName = get_label_name(label);
    sprintf(gname, "@%s = internal global %s blockaddress(@%s, %%L%s)",
            SYMNAME(gl_sptr), retc, getsname(gbl.currsub), labelName);
    gitem->global_def = gname;
    add_global_define(gitem);
  }

  return gl_sptr;
}

/**
   \brief Process \p sptr and initialize \c SNAME(sptr)
   \param sptr  an external function
 */
static void
process_extern_function_sptr(int sptr)
{
  int dtype = DTYPEG(sptr);
  int return_dtype;
  EXFUNC_LIST *exfunc;
  char *name, *gname, *extend_prefix;
  LL_Type* ll_ttype;

  assert(SCG(sptr) == SC_EXTERN, "Expected extern sptr", sptr, 4);
  assert(SNAME(sptr) == NULL, "Already precessed sptr", sptr, 4);
  assert(STYPEG(sptr) == ST_PROC || STYPEG(sptr) == ST_ENTRY,
         "Can only process extern procedures", sptr, 4);

  name = set_global_sname(sptr, get_llvm_name(sptr));

  sym_is_refd(sptr);
  if (CFUNCG(sptr) && STYPEG(sptr) == ST_PROC) {
    int ttype = DDTG(dtype);
    if (DTY(ttype) == TY_CHAR) {
      ll_ttype = make_ptr_lltype(make_lltype_from_dtype(DT_BINT));
      LLTYPE(sptr) = ll_ttype;
    }
  }
  return_dtype = dtype;

  if (DEFDG(sptr))
    return; /* defined in the file, so no need to declare separately */
  if (INMODULEG(sptr) && INMODULEG(sptr) == INMODULEG(gbl.currsub))
    return; /* module subroutine call its module subroutine*/

#if defined ALIASG && defined WEAKG
  /* Don't emit an external reference if the name needs to be defined
   * as a weak alias in write_aliases().
   */
  if (ALIASG(sptr) > NOSYM && WEAKG(sptr))
    return;
#endif

  /* In the case of a function, we want the return type, not the type of
   * the sptr, which we know is "function"
   */
  exfunc = (EXFUNC_LIST *)getitem(LLVM_LONGTERM_AREA, sizeof(EXFUNC_LIST));
  memset(exfunc, 0, sizeof(EXFUNC_LIST));
  exfunc->sptr = sptr;
  if (cgmain_init_call(sptr)) {
    gname = (char *)getitem(LLVM_LONGTERM_AREA, 34);
    sprintf(gname, "declare void @__c_bzero(i32, i8*)");
    exfunc->flags |= EXF_INTRINSIC;
  } else {
    const int dTy =
        get_return_dtype(return_dtype, &(exfunc->flags), EXF_STRUCT_RETURN);
    const char *retc = char_type(dTy, 0);
    const int size = strlen(retc) + strlen(name) + 50;
    gname = (char *)getitem(LLVM_LONGTERM_AREA, size);
#ifdef VARARGG
    if (VARARGG(sptr))
      exfunc->flags |= EXF_VARARG;
#endif
    /* do we return a char? If so, must add
     * attribute "zeroext" or "signext"
     */
    switch (DTY(dTy)) {
    default:
      extend_prefix = "";
      break;
    case TY_SINT:
#if defined(TARGET_LLVM_X8664)
      /* Workaround: LLVM on x86 does not sign extend i16 types */
      retc = char_type(DT_INT, 0);
#endif
    case TY_BINT:
      extend_prefix = "signext";
      break;
    case TY_USINT:
#if defined(TARGET_LLVM_X8664)
      /* Workaround: LLVM on x86 does not sign extend i16 types */
      retc = char_type(DT_INT, 0);
#endif
      extend_prefix = "zeroext";
      break;
    }
    sprintf(gname, "%s %s %s", extend_prefix, retc, name);
  }
  exfunc->func_def = gname;
  exfunc->use_dtype = DTYPEG(sptr);
  add_external_function_declaration(exfunc);
}

/**
   \brief Process extern variable \p sptr and initialize <tt>SNAME(sptr)</tt>
   \param sptr  a symbol
 */
static void
process_extern_variable_sptr(int sptr, ISZ_T off)
{
  int ch, ipai, dtype, stype = STYPEG(sptr);
  char *name, *ipag;
  const char *retc;
  GBL_LIST *gitem;
  LL_Type* ttype;

  assert(SCG(sptr) == SC_EXTERN, "Expected extern sptr", sptr, 4);
  assert(SNAME(sptr) == NULL, "Already processed sptr", sptr, 4);

  name = set_global_sname(sptr, get_llvm_name(sptr));
  retc = char_type(DTYPEG(sptr), sptr);

/* if this is an IPA-globalized variable, deal with it here */
#ifdef IPANAMEG
  /* I am unhappy with this following hack. Orginally came from
   * correctness test lx00 and spec test crafty.
   */
  int found_ipags = FALSE;
  if (strncmp(name, "@.gst.", 6) == 0) {
    int len = strlen(name);
    /* name[len] should be ternminating '\0' */
    for (ch = name[--len]; isdigit(ch); ch = name[--len])
      ;
    assert(ch == 46,
           "process_extern_variable_sptr(): expected period character", ch, 4);
    ipag = &name[len + 1];
    ipai = atoi(ipag);
    if (ipai < stb.symavl)
      ipag = get_llvm_name(ipai);
    if (ipai < stb.symavl && !strcmp(name, ipag) && SNAME(ipai) == NULL &&
        IPANAMEG(ipai) && SCG(ipai) == SC_EXTERN && DEFDG(ipai) /* &&
                               DINITG(ipai) */) {
      /* what we have is an IPA-globalized static, and we
       * need to process the original symbol. We also need
       * to set the SNAME of ipai so that it will not be
       * picked up again by compute_unrefed_defines().
       * NB: SNAME(ipai) == SNAME(sptr) [see get_llvm_name()]
       */
      SNAME(ipai) = name;
      ttype = make_lltype_sz4v3_from_sptr(ipai);
      LLTYPE(ipai) = ttype;
      LLTYPE(sptr) = LLTYPE(ipai); /* make the types match */
      sptr = ipai;
      found_ipags = TRUE;
    }
    /* check for original symbol already processed and put
     * on global define list. Make the types match as above,
     * then exit. Don't want symbol redefinition.
     */
    else if (ipai < stb.symavl && !strcmp(name + 1, ipag) && sptr != ipai &&
             SNAME(ipai) && IPANAMEG(ipai)) {
      LLTYPE(sptr) = LLTYPE(ipai);
      return;
    }
  }

  /* ipa may create another extern symbol with the same name;
   * We need to disambiguate the cases since LLVM requires all
   * references to be declared. And multiple declarations just
   * won't work.
   */
  if (!found_ipags && (SCOPEG(sptr) == 0 || SCOPEG(sptr) == 1) &&
      (ipai = follow_sptr_hashlk(sptr)) && (ipag = get_llvm_name(ipai)) &&
      !strcmp(ipag, name + 1)) {
    if (!SNAME(ipai)) {
      SNAME(ipai) = name;
      ttype = make_lltype_sz4v3_from_sptr(ipai);
      LLTYPE(ipai) = ttype;
      /* if sptr & ipai are structs or unions, we need to sync up their
       * structure names. Use original name if possible.  NOTE: might we
       * need to recurse; that is, if the dtype is a ptr to a struct in
       * each case could we have the same situation? Yes, we do and that
       * is why the routine follow_ptr_dtype() has been added.
       */
      int sptr_dtype = follow_ptr_dtype(DTYPEG(sptr));
      int ipai_dtype = follow_ptr_dtype(DTYPEG(ipai));
      if ((DTY(sptr_dtype) == TY_STRUCT && DTY(ipai_dtype) == TY_STRUCT) ||
          (DTY(sptr_dtype) == TY_UNION && DTY(ipai_dtype) == TY_UNION)) {
        retc = char_type(DTYPEG(ipai), ipai);
      }
      /* also need to use original symbol if it is defined;
       * typically ipa/inlining creates global versions
       * which, if used, will lead to undefined references
       * when base symbol (ipai) has a defined value.
       */
      if (DEFDG(ipai))
        sptr = ipai;
    } else { /* ipai already processed, don't want redefinition */
      /* however, need to coordinate dtype names as above */
      int sptr_dtype = follow_ptr_dtype(DTYPEG(sptr));
      int ipai_dtype = follow_ptr_dtype(DTYPEG(ipai));
      if ((DTY(sptr_dtype) == TY_STRUCT && DTY(ipai_dtype) == TY_STRUCT) ||
          (DTY(sptr_dtype) == TY_UNION && DTY(ipai_dtype) == TY_UNION)) {
        retc = char_type(DTYPEG(ipai), ipai);
      }
      return;
    }
  }
#endif

  gitem = (GBL_LIST *)getitem(LLVM_LONGTERM_AREA, sizeof(GBL_LIST));
  memset(gitem, 0, sizeof(GBL_LIST));
  gitem->sptr = sptr;
  gitem->alignment = align_of_var(sptr);

  if (
      !DEFDG(sptr)
          ) {
    return;
    char *gname =
        (char *)getitem(LLVM_LONGTERM_AREA, strlen(retc) + strlen(name) + 52);
    sprintf(gname, "%s = external %s global %s", name,
            IS_TLS(sptr) ? "thread_local" : "", retc);
    gitem->global_def = gname;
  } else {
    const char *flag_str = IS_TLS(sptr) ? "thread_local global" : "global";

    /* Defined as a global, not initialized. */
    if (!DINITG(sptr) && IS_TLS(sptr))
      flag_str = "common thread_local global";
    else if (!DINITG(sptr))
      flag_str = "common global";
    create_global_initializer(gitem, flag_str, retc);
  }
  add_global_define(gitem);
} /* process_extern_variable_sptr */

/**
   \brief add debug information for variable \p sptr
   \param sptr  the symbol to be added
   \param type  the type to be used for \p sptr
 */
INLINE static void
addDebugForLocalVar(int sptr, LL_Type *type)
{
  if (need_debug_info(sptr)) {
    /* Dummy sptrs are treated as local (see above) */
    LL_MDRef param_md = lldbg_emit_local_variable(
        cpu_llvm_module->debug_info, sptr, BIH_FINDEX(gbl.entbih), TRUE);
    insert_llvm_dbg_declare(param_md, sptr, type, NULL);
  }
}

/**
   \brief process an \c SC_LOCAL \p sptr and initialize \c SNAME(sptr)
   \param sptr  a symbol
 */
static void
process_local_sptr(SPTR sptr)
{
  LL_Type *type = NULL;
  assert(SCG(sptr) == SC_LOCAL, "Expected local sptr", sptr, ERR_Fatal);
  assert(SNAME(sptr) == NULL, "Already precessed sptr", sptr, ERR_Fatal);

  sym_is_refd(sptr);

  if (REFG(sptr) && DINITG(sptr)) {
    char *name = get_llvm_name(sptr);
    if (SCOPEG(sptr) == 0) {
      name = set_global_sname(sptr, name);
    } else {
      name = set_numbered_global_sname(sptr, name);
    }
    DBGTRACE2("#variable #%d(%s) is data initialized", sptr, SYMNAME(sptr))
  } else if (DINITG(sptr) || SAVEG(sptr)) {
    char *name = get_llvm_name(sptr);
    GBL_LIST *gitem = (GBL_LIST *)getitem(LLVM_LONGTERM_AREA, sizeof(GBL_LIST));
    memset(gitem, 0, sizeof(GBL_LIST));
    gitem->sptr = sptr;

    if (SCOPEG(sptr) == 0) {
      name = set_global_sname(sptr, name);
    } else {
      name = set_numbered_global_sname(sptr, name);
    }

    DBGTRACE2("#variable #%d(%s) is data initialized", sptr, SYMNAME(sptr));
    create_global_initializer(gitem, "internal global",
                              char_type(DTYPEG(sptr), sptr));
    add_global_define(gitem);
  } else if (SOCPTRG(sptr)) {
    SNAME(sptr) = get_local_overlap_var();
  } else {
    /* This is an actual local variable. Create an alloca. */
    LL_Object *local;
    type = LLTYPE(sptr);

    /* make_lltype_from_sptr() should have added a pointer to the type of
     * this local variable. Remove it */
    CHECK(type->data_type == LL_PTR);
    type = type->sub_types[0];

    /* Now create the alloca for this variable.
     * FIXME: Apparently, the AG table is keeping track of local symbols by
     * name, but we have no guarantee that locval names are unique. This
     * will end in tears. */
    local = ll_create_local_object(llvm_info.curr_func, type,
                                   align_of_var(sptr), "%s", get_llvm_name(sptr));
    SNAME(sptr) = (char *)local->address.data;
  }

  addDebugForLocalVar(sptr, type);
}

/* May need to be revisited */
static void
process_private_sptr(int sptr)
{
  if (!gbl.outlined && !TASKG(sptr))
    return;

  assert(SCG(sptr) == SC_PRIVATE, "Expected local sptr", sptr, 4);
  assert(SNAME(sptr) == NULL, "Already precessed sptr", sptr, 4);

  /* TODO: Check enclfuncg's scope and if its is not the same as the
   * scope level for -g, then return early, this is not a private sptr
   */
  sym_is_refd(sptr);

  /* This is an actual local variable. Create an alloca. */
  LL_Type *type = LLTYPE(sptr);
  LL_Object *local;

  /* make_lltype_from_sptr() should have added a pointer to the type of
   * this local variable. Remove it */
  CHECK(type->data_type == LL_PTR);
  type = type->sub_types[0];

  /* Now create the alloca for this variable.
   * FIXME: Apparently, the AG table is keeping track of local symbols by
   * name, but we have no guarantee that locval names are unique. This
   * will end in tears.
   */
  local = ll_create_local_object(llvm_info.curr_func, type, align_of_var(sptr),
                                 "%s", get_llvm_name(sptr));
  SNAME(sptr) = (char *)local->address.data;
}

/**
   \brief Does this arg's pointer type really need to be dereferenced?
   \param sptr   The argument
   \return true iff this argument should NOT use the pointer's base type
 */
INLINE static bool
processAutoSptr_skip(SPTR sptr)
{
  if ((CUDAG(gbl.currsub) & (CUDA_GLOBAL | CUDA_DEVICE)) &&
      DEVICEG(sptr) && !PASSBYVALG(sptr)) {
    return (SCG(sptr) == SC_DUMMY) ||
      ((SCG(sptr) == SC_BASED) && (SCG(MIDNUMG(sptr)) == SC_DUMMY));
  }
  return false;
}

INLINE static LL_Type *
fixup_argument_type(SPTR sptr, LL_Type *type)
{
  if (processAutoSptr_skip(sptr))
    return type;
  /* type = pointer base type */
  return type->sub_types[0];
}

/**
   \brief Process an \c SC_AUTO or \c AC_REGISTER \p sptr
   \param sptr  A symbol
   Also initialize <tt>SNAME(sptr)</tt>.
 */
static void
process_auto_sptr(int sptr)
{
  LL_Type *type = LLTYPE(sptr);
  LL_Object *local;

  /* Accept SC_DUMMY sptrs if they are arguments that have been given local
   * variable storage. */
  if (SCG(sptr) == SC_DUMMY) {
    assert(hashmap_lookup(llvm_info.homed_args, INT2HKEY(sptr), NULL),
           "Expected coerced dummy sptr", sptr, 4);
  } else {
  }
  assert(SNAME(sptr) == NULL, "Already precessed sptr", sptr, 4);

  /* The hidden return argument is created as an SC_AUTO sptr containing the
   * pointer, but it does not need a local entry if we're actually going to
   * emit an LLVM IR sret argument which is just a constant pointer.
   */
  if (ret_info.emit_sret && is_special_return_symbol(sptr)) {
    SNAME(sptr) = (char *)ll_create_local_name(llvm_info.curr_func, "sretaddr");
    return;
  }

  /* make_lltype_from_sptr() should have added a pointer to the type of this
   * local variable. Remove it */
  CHECK(type->data_type == LL_PTR);
  type = fixup_argument_type(sptr, type);

  /* Now create the alloca for this variable. Since the alloca produces the
   * address of the local, name it "%foo.addr". */
  local = ll_create_local_object(llvm_info.curr_func, type, align_of_var(sptr),
                                 "%s.addr", SYMNAME(sptr));
  SNAME(sptr) = (char *)local->address.data;

  addDebugForLocalVar(sptr, type);
}

static void
process_label_sptr_c(SPTR sptr)
{
  const char *name = get_llvm_name(sptr);
  SNAME(sptr) = (char *)getitem(LLVM_LONGTERM_AREA, strlen(name) + 1);
  strcpy(SNAME(sptr), name);
}

/**
   \brief Process an <tt>SC_NONE</tt> \p sptr
   \param sptr represents a label
   Also initialize <tt>SNAME(sptr)</tt>.
 */
static void
process_label_sptr(SPTR sptr)
{
  assert(SCG(sptr) == SC_NONE, "Expected label sptr", sptr, 4);
  assert(SNAME(sptr) == NULL, "Already precessed sptr", sptr, 4);

  switch (STYPEG(sptr)) {
  case ST_CONST:
    /* TODO: Move this sooner, into the bridge */
    sym_is_refd(sptr);
    return;
  case ST_MEMBER:
    return;
  default:
    break;
  }
  process_label_sptr_c(sptr);
}

static void
process_sptr_offset(SPTR sptr, ISZ_T off)
{
  SC_KIND sc;
  DTYPE dtype;
  int midnum;
  LL_Type* ttype;

  sym_is_refd(sptr);
  update_llvm_sym_arrays();
  sc = SCG(sptr);

  if (SNAME(sptr))
    return;

  DBGTRACEIN7(" sptr %d = '%s' (%s) SNAME(%d)=%p, sc %d, ADDRTKNG(%d)", sptr,
              getprint(sptr), stb.scnames[sc], sptr, SNAME(sptr), sc,
              ADDRTKNG(sptr));

  ttype = make_lltype_sz4v3_from_sptr(sptr);
  LLTYPE(sptr) = ttype;

  switch (sc) {
  case SC_CMBLK:
    set_global_sname(sptr, get_llvm_name(sptr));
    break;
  case SC_STATIC:
    process_static_sptr(sptr, off);
    break;

  case SC_EXTERN:
    if (
        STYPEG(sptr) == ST_PROC || STYPEG(sptr) == ST_ENTRY
        ) {
      process_extern_function_sptr(sptr);
    } else {
      process_extern_variable_sptr(sptr, off);
    }
    break;

  case SC_DUMMY:
    midnum = MIDNUMG(sptr);
    if (DTYPEG(sptr) == DT_ADDR && midnum &&
        hashmap_lookup(llvm_info.homed_args, INT2HKEY(midnum), NULL)) {
      LLTYPE(sptr) = LLTYPE(midnum);
      SNAME(sptr) = SNAME(midnum);
      return;
    }
    if (hashmap_lookup(llvm_info.homed_args, INT2HKEY(sptr), NULL)) {
      process_auto_sptr(sptr);
    } else {
      set_local_sname(sptr, get_llvm_name(sptr));
    }
    if ((flg.smp || (XBIT(34, 0x200) || gbl.usekmpc)) && gbl.outlined) {
      if (sptr == ll_get_shared_arg(gbl.currsub)) {
        LLTYPE(sptr) = make_ptr_lltype(make_lltype_from_dtype(DT_INT8));
      }
    }
    DBGTRACE1("#dummy argument: %s", SNAME(sptr));
    break;

  case SC_LOCAL:
    process_local_sptr(sptr);
    break;

  case SC_BASED:
    if (DEVICEG(sptr) && (CUDAG(gbl.currsub) & (CUDA_GLOBAL | CUDA_DEVICE))) {
      if (hashmap_lookup(llvm_info.homed_args, INT2HKEY(MIDNUMG(sptr)), NULL)) {
        process_auto_sptr(sptr);
        LLTYPE(MIDNUMG(sptr)) = LLTYPE(sptr);
        SNAME(MIDNUMG(sptr)) = SNAME(sptr);
      } else {
        set_local_sname(sptr, get_llvm_name(sptr));
      }
    } else
      set_local_sname(sptr, get_llvm_name(sptr));
    DBGTRACE1("#dummy argument: %s", SNAME(sptr));
    break;

  case SC_NONE: /* should be a label */
    process_label_sptr(sptr);
    break;

#ifdef SC_PRIVATE
  case SC_PRIVATE: /* OpenMP */
    process_private_sptr(sptr);
    break;
#endif
  default:
    assert(0, "process_sptr(): unexpected storage type", sc, 4);
  }

  DBGTRACEOUT("")
} /* process_sptr_offset */

void
process_sptr(SPTR sptr)
{
  process_sptr_offset(sptr, 0);
}

static int
follow_sptr_hashlk(int sptr)
{
  /* ipa sometimes makes additional symbol entries for external
   * variables (I have noticed this mainly on globally-defined
   * anonymous structures). However, since LLVM requires all
   * references to be declared within the file that they are
   * referenced, this may result in multiple declararions of the
   * same symbol. Does not work with the opt and llc tools of LLVM.
   * Thus we try to track down, if possible, the original symbol
   * of storage class extern. Follow the links as far as possible.
   */

  char *hash_name, *name = get_llvm_name(sptr);
  int ret_val = 0;
  int hashlk = HASHLKG(sptr);
  while (hashlk > 0) {
    hash_name = get_llvm_name(hashlk);
    if (SCG(hashlk) == SC_EXTERN && !strcmp(name, hash_name))
      ret_val = hashlk;
    ret_val = hashlk;
    hashlk = HASHLKG(hashlk);
  }
  return ret_val;
} /* follow_sptr_hashlk */

static int
follow_ptr_dtype(int dtype)
{
  int dty = dtype;
  while (DTY(dty) == TY_PTR)
    dty = DTY(dty + 1);
  return dty;

} /* follow_ptr_dtype */

LOGICAL
strict_match(LL_Type *ty1, LL_Type *ty2)
{
  return ty1 == ty2;
}

/*
 * both ty1 & ty2 are of kind LLT_FUNCTION
 * check that protoypes are matchings
 */
INLINE static LOGICAL
match_prototypes(LL_Type *ty1, LL_Type *ty2)
{
  return strict_match(ty1, ty2);
}

/**
   \brief Does \c ty2 "match" \c ty1 or can \c ty2 be converted to \c ty1?
   \param ty1  the result type
   \param ty2  a type

   If \c ty1 is a ptr and \c ty2 is not, we have an error.  In general, if the
   nesting level of \c ty1 is greater than that of \c ty2, then we have an
   error.

   NB: The original algorithm did \e NOT enforce the latter condition above. The
   old algorithm would peel off all outer levels of array types blindly and
   until a non-array element type is found. This implied that this function
   would return \c MATCH_OK when the input types are \c i32 and <code>[A x [B x
   [C x [D x i32]]]]</code>.
 */
static MATCH_Kind
match_types(LL_Type *ty1, LL_Type *ty2)
{
  int ret_type;
  int base_ty1, base_ty2, ct1, ct2;
  LL_Type *llt1, *llt2;

  assert(ty1 && ty2, "match_types(): missing argument", 0, 4);

  DBGTRACEIN2("match_types: ty1=%s, ty2=%s\n", ty1->str, ty2->str);
  if (ty1 == ty2)
    return MATCH_OK;

  if (ty1->data_type == LL_ARRAY) {
    LL_Type *ele1 = ll_type_array_elety(ty1);
    LL_Type *ele2 = ll_type_array_elety(ty2);
    return ele2 ? match_types(ele1, ele2) : MATCH_NO;
  }

  if ((ty1->data_type == LL_PTR) || (ty2->data_type == LL_PTR)) {
    /* at least one pointer type */
    if (ty2->data_type != LL_PTR) {
      /* reject as only ty1 is a ptr */
      ret_type = MATCH_NO;
    } else {
      /* get the depth of each pointer type */
      ct1 = 0;
      llt1 = ty1;
      while (llt1->data_type == LL_PTR) {
        asrt(llt1);
        ct1++;
        llt1 = llt1->sub_types[0];
      }
      ct2 = 0;
      llt2 = ty2;
      while (llt2->data_type == LL_PTR) {
        asrt(llt2);
        ct2++;
        llt2 = llt2->sub_types[0];
      }
      if (ct1 > ct2) {
        ret_type = MATCH_NO;
      } else if (match_types(llt1, llt2) == MATCH_OK) {
        if (ct1 == ct2)
          ret_type = MATCH_OK; // ptrs have same level of indirection only
        else if (ct1 + 1 == ct2)
          ret_type = MATCH_MEM;
        else
          ret_type = MATCH_NO;
      } else if ((llt1->data_type == LL_VOID) || (llt2->data_type == LL_VOID)) {
        // one or the other is ptr-to-void; implies, void* == T*****
        ret_type = MATCH_OK;
      } else {
        ret_type = MATCH_NO;
      }
    }
  } else if (ty1->data_type == ty2->data_type) {
    if (ty1->data_type == LL_STRUCT) {
      ret_type = ((ty1 == ty2) ? MATCH_OK : MATCH_NO);
    } else if (ty1->data_type == LL_FUNCTION) {
      /* ??? used to check if both FUNC types were "old-style" or not.
         "Old-style" meant that (DTY(dtype) == TY_FUNC). Why would it matter?
         This doesn't otherwise check the signature. */
      ret_type = MATCH_OK;
    } else {
      ret_type = MATCH_OK;
    }
  } else {
    ret_type = MATCH_NO;
  }

  if (ll_type_int_bits(ty1)) {
    DBGTRACEOUT4(" returns %d(%s) ty1 = %s%d", ret_type, match_names(ret_type),
                 ty1->str, (int)(ll_type_bytes(ty1) * 8))
  } else {
    DBGTRACEOUT3(" returns %d(%s) ty1 = %s", ret_type, match_names(ret_type),
                 ty1->str)
  }
  return ret_type;
} /* match_types */

int
match_llvm_types(LL_Type *ty1, LL_Type *ty2)
{
  return match_types(ty1, ty2);
}

static LL_Type *
make_type_from_opc(ILI_OP opc)
{
  LL_Type *llt;

  DBGTRACEIN1(" (%s)", IL_NAME(opc))
  /* these opcodes will come from conversion operations and expression
   * evaluation without a store, such as:
   *           if( j << 2 )
   *           if (j - (float)3.0)
   * the other possibility is jump ILI with expressions, or cast due
   * to array manipulations. These are mostly
   * of integer type, as the the evaluation of a condition is inherently
   * integral. However, notice first two cases, which are of type LLT_PTR.
   */
  switch (opc) {
  case IL_ACMP:
  case IL_ACMPZ:
  case IL_ACJMP:
  case IL_ACJMPZ:
  case IL_ASELECT:
    llt = make_lltype_from_dtype(DT_CPTR);
    break;
  case IL_ICJMP:
  case IL_UICJMP:
  case IL_FIX:
  case IL_AND:
  case IL_OR:
  case IL_XOR:
  case IL_NOT:
  case IL_MOD:
  case IL_MODZ:
  case IL_LSHIFT:
  case IL_RSHIFT:
  case IL_ARSHIFT:
  case IL_ICON:
  case IL_ICJMPZ:
  case IL_UICJMPZ:
  case IL_UICMPZ:
  case IL_IADD:
  case IL_ISUB:
  case IL_IMUL:
  case IL_IDIVZ:
  case IL_IDIV:
  case IL_UIADD:
  case IL_UISUB:
  case IL_UIMUL:
  case IL_UIDIV:
  case IL_INEG:
  case IL_UINEG:
  case IL_DFIX:
  case IL_DFIXU:
  case IL_ICMP:
  case IL_ICMPZ:
  case IL_ISELECT:
  case IL_IMIN:
  case IL_IMAX:
  case IL_UIMIN:
  case IL_UIMAX:
  case IL_IABS:
  case IL_CMPXCHG_OLDI:
    llt = make_lltype_from_dtype(DT_INT);
    break;
  case IL_KAND:
  case IL_KLSHIFT:
  case IL_KCJMP:
  case IL_KCON:
  case IL_KADD:
  case IL_KSUB:
  case IL_KMUL:
  case IL_KDIV:
  case IL_KDIVZ:
  case IL_KNOT:
  case IL_KCJMPZ:
  case IL_KOR:
  case IL_FIXK:
  case IL_KXOR:
  case IL_KMOD:
  case IL_KARSHIFT:
  case IL_KNEG:
  case IL_KCMP:
  case IL_KCMPZ:
  case IL_UKMIN:
  case IL_UKMAX:
  case IL_KMIN:
  case IL_KMAX:
  case IL_KSELECT:
  case IL_KABS:
  case IL_CMPXCHG_OLDKR:
    llt = make_lltype_from_dtype(DT_INT8);
    break;
  case IL_KUMOD:
  case IL_KUMODZ:
  case IL_UKDIV:
  case IL_UKDIVZ:
  case IL_FIXUK:
  case IL_KURSHIFT:
  case IL_UKCMP:
  case IL_DFIXK:
  case IL_DFIXUK:
  case IL_UKCJMP:
  case IL_UKADD:
  case IL_UKSUB:
  case IL_UKMUL:
  case IL_UKCJMPZ:
  case IL_UKCMPZ:
  case IL_UKNEG:
  case IL_UKNOT:
    llt = make_lltype_from_dtype(DT_UINT8);
    break;
  case IL_UNOT:
  case IL_UFIX:
  case IL_UIMOD:
  case IL_UIMODZ:
  case IL_UIDIVZ:
  case IL_ULSHIFT:
  case IL_URSHIFT:
  case IL_UICMP:
    llt = make_lltype_from_dtype(DT_UINT);
    break;
  case IL_FLOAT:
  case IL_FLOATU:
  case IL_FLOATK:
  case IL_FLOATUK:
  case IL_FMOD:
  case IL_SNGL:
  case IL_FSUB:
  case IL_FMUL:
  case IL_FDIV:
  case IL_FADD:
  case IL_FCON:
  case IL_FNEG:
  case IL_FCJMP:
  case IL_FCJMPZ:
  case IL_FCMP:
  case IL_CMPNEQSS:
  case IL_FMIN:
  case IL_FMAX:
  case IL_FABS:
  case IL_FSELECT:
    llt = make_lltype_from_dtype(DT_FLOAT);
    break;
  case IL_DCJMP:
  case IL_DCJMPZ:
  case IL_DFLOAT:
  case IL_DFLOATU:
  case IL_DFLOATK:
  case IL_DFLOATUK:
  case IL_DMOD:
  case IL_DBLE:
  case IL_DADD:
  case IL_DSUB:
  case IL_DNEG:
  case IL_DMAX:
  case IL_DMIN:
  case IL_DMUL:
  case IL_DDIV:
  case IL_DCON:
  case IL_DCMP:
  case IL_DSELECT:
  case IL_DABS:
    llt = make_lltype_from_dtype(DT_DBLE);
    break;
  case IL_CSSELECT:
  case IL_SCMPLXADD:
    llt = make_lltype_from_dtype(DT_CMPLX);
    break;
  case IL_CDSELECT:
  case IL_DCMPLXADD:
    llt = make_lltype_from_dtype(DT_DCMPLX);
    break;
  case IL_ALLOC:
    llt = make_lltype_from_dtype(DT_CPTR);
    break;
  default:
    DBGTRACE2("###make_type_from_opc(): unknown opc %d(%s)", opc, IL_NAME(opc))
    assert(0, "make_type_from_opc: unknown opc", opc, 4);
    llt = NULL;
  }

  DBGTRACEOUT1(" returns %p", llt)
  return llt;
} /* make_type_from_opc */

static LL_Type *
make_type_from_msz(MSZ msz)
{
  return make_lltype_from_dtype(msz_dtype(msz));
} /* make_type_from_msz */

static LL_Type *
make_vtype(int dtype, int sz)
{
  LL_Type *llt;
  int vect_dtype;
  vect_dtype = get_vector_type(dtype, sz);
  return make_lltype_from_dtype(vect_dtype);
} /* make_vtype */

static int
is_special_return_symbol(int sptr)
{
  return ret_info.sret_sptr == sptr;
}

int
need_ptr(int sptr, int sc, int sdtype)
{
  if (is_special_return_symbol(sptr))
    return DTY(sdtype) != TY_PTR;

  switch (sc) {
  case SC_EXTERN:
    return TRUE;

  case SC_STATIC:
    return !DINITG(sptr) || !SAVEG(sptr);

#ifdef SC_PRIVATE
  case SC_PRIVATE:
    return TRUE;
#endif
  case SC_LOCAL:
  case SC_CMBLK:
    return TRUE;

#ifdef SC_REGISTER
  case SC_REGISTER:
    return TRUE;
#endif

  case SC_DUMMY:
    /* process_formal_arguments() homes all dummies. */
    return TRUE;
  }

  if (sptr)
    switch (STYPEG(sptr)) {
    case ST_ARRAY:
      return TRUE;
    case ST_MEMBER:
      if (DTY(sdtype) == TY_ARRAY)
        return TRUE;
      break;
    default:
      break;
    }

  return FALSE;
}

static OPERAND *
gen_sptr(SPTR sptr)
{
  SC_KIND sc;
  DTYPE dtype;
  OPERAND *sptr_operand, *operand2;

  DBGTRACEIN2(" sptr %d (%s)", sptr, SYMNAME(sptr))

  sptr_operand = make_operand();
  sc = SCG(sptr);
  process_sptr(sptr);
  sptr_operand->ll_type = LLTYPE(sptr);
  switch (sc) {
  case SC_CMBLK:
  case SC_DUMMY:
#ifdef SC_PRIVATE
  case SC_PRIVATE:
#endif
  case SC_STATIC:
  case SC_EXTERN:
  case SC_AUTO:
#ifdef SC_REGISTER
  case SC_REGISTER:
#endif
    DBGTRACE2("#using this name for %s; %s", SYMNAME(sptr), SNAME(sptr))

    sptr_operand->ot_type = OT_VAR;
    sptr_operand->val.sptr = sptr;
    sptr_operand->string = SNAME(sptr);
    break;
  case SC_NONE:
    /* For some constants, we need its address to pass to our runtime.
       They also need to be initialized.
     */
    if (STYPEG(sptr) == ST_CONST) {
      sptr_operand->ot_type = OT_VAR;
      sptr_operand->val.sptr = sptr;
      sptr_operand->string = SNAME(sptr);
      break;
    }
  /* TBD */
  case SC_BASED:
  default:
    assert(0, "gen_sptr(): unexpected storage type", sc, 4);
  }

  DBGTRACEOUT1(" returns operand %p", sptr_operand)
  return sptr_operand;
} /* gen_sptr */

/**
   \brief Generate an address expression as an operand
   \param addr_op	ILI of address expression
   \param nme		NME value
   \param lda		is this an IL_LDA?
   \param llt_expected  expected LL_Type
   \param msz		memory access size
 */
OPERAND *
gen_address_operand(int addr_op, int nme, bool lda, LL_Type *llt_expected,
                    MSZ msz)
{
  OPERAND *operand;
  OPERAND **csed_operand;
  LL_Type *llt = llt_expected;
  SPTR sptr = basesym_of(nme);
  unsigned savedAddressSize = addressElementSize;

  DBGTRACEIN2(" for ilix: %d(%s)", addr_op, IL_NAME(ILI_OPC(addr_op)))
  DBGDUMPLLTYPE("expected type ", llt_expected)

  if (!llt && !lda && (((int)msz) >= 0)) {
    llt = make_ptr_lltype(make_type_from_msz(msz));
  }
  sptr = basesym_of(nme);

  if (llt) {
    /* do nothing */
  } else {
    if (sptr <= 0) {
      int sub_add = addr_op, sub_opc;
      llt = make_lltype_from_dtype(DT_CPTR);
      while (sub_add) {
        sub_opc = ILI_OPC(sub_add);
        switch (sub_opc) {
        case IL_AADD:
        case IL_ASUB:
          sub_add = ILI_OPND(sub_add, 1);
          continue;
        case IL_DFRAR:
        case IL_ACON:
          llt = make_ptr_lltype(llt);
          sub_add = 0;
          break;
        case IL_LDA:
          llt = make_ptr_lltype(llt);
          sub_add = ILI_OPND(sub_add, 1);
          break;
        default:
          sub_add = 0;
        }
      }
    } else {
      llt = make_lltype_from_sptr(sptr);
      DBGTRACE4("#lda of sptr '%s' for type %d %d %d", getprint(sptr),
                STYPEG(sptr), SCG(sptr), llt->data_type);

      if ((SCG(sptr) == SC_DUMMY) || (SCG(sptr) == SC_AUTO)) {
        const int midnum = MIDNUMG(sptr);
        if (midnum && STYPEG(midnum) == ST_PROC) {
          assert(LLTYPE(midnum), "process_sptr() never called for sptr", sptr,
                 ERR_Fatal);
          llt = LLTYPE(midnum);
        } else if ((flg.smp || XBIT(34, 0x200) || gbl.usekmpc) &&
                   gbl.outlined && (sptr == ll_get_shared_arg(gbl.currsub))) {
          llt = LLTYPE(sptr);
        } else
#ifdef TARGET_LLVM_ARM
            if ((llt->data_type == LL_STRUCT) && (NME_SYM(nme) != sptr)) {
          llt = make_ptr_lltype(make_lltype_from_dtype(DT_CPTR));
        }
#endif
        if ((llt->data_type == LL_PTR) &&
            (llt->sub_types[0]->data_type != LL_PTR) && NME_SYM(nme) != sptr) {
          llt = make_ptr_lltype(make_lltype_from_dtype(DT_CPTR));
        }
        if ((STYPEG(sptr) != ST_VAR) && (ASSNG(sptr) || ADDRTKNG(sptr))) {
          if ((llt->data_type == LL_PTR) &&
              (llt->sub_types[0]->data_type != LL_PTR)) {
            llt = make_ptr_lltype(make_lltype_from_dtype(DT_CPTR));
          }
        }
      } else if ((STYPEG(sptr) != ST_VAR) &&
                 ((llt->data_type != LL_PTR) ||
                  (llt->sub_types[0]->data_type != LL_PTR))) {
        llt = make_ptr_lltype(make_lltype_from_dtype(DT_CPTR));
      }
    }
  }
  addressElementSize = (llt->data_type == LL_PTR) ?
    ll_type_bytes_unchecked(llt->sub_types[0]) : 0;
  operand = gen_base_addr_operand(addr_op, llt);

  DBGTRACEOUT("")

  csed_operand = get_csed_operand(addr_op);
  if (csed_operand != NULL) {
    set_csed_operand(csed_operand, operand);
  }

  ILI_COUNT(addr_op)++;
  addressElementSize = savedAddressSize;
  return operand;
}

/**
   \brief Computes byte offset into aggregate structure of \p sptr
   \param sptr  the symbol
   \param idx   additional addend to offset
   \return an offset into a memory object or 0

   NB: sym_is_refd() must be called prior to this function in order to return
   the correct result.
 */
static ISZ_T
variable_offset_in_aggregate(SPTR sptr, ISZ_T idx)
{
  if (ADDRESSG(sptr) && (SCG(sptr) != SC_DUMMY) && (SCG(sptr) != SC_LOCAL)) {
    /* expect:
          int2                           int
          sptr:301  dtype:6  nmptr:2848  sc:4=CMBLK  stype:6=variable
          symlk:1=NOSYM
          address:8  enclfunc:295=mymod
          midnum:302=_mymod$0

       This can be found in a common block.  Don't add address on stack for
       local/dummy arguments */
    idx += ADDRESSG(sptr);
  } else if ((SCG(sptr) == SC_LOCAL) && SOCPTRG(sptr)) {
    idx += get_socptr_offset(sptr);
  }
  return idx;
}

/**
   \brief Generate an OPERAND representing the value of the ACON ilix.
   \param ilix           An IL_ACON ilix
   \param expected_type  The expected type of the result
   \return an OPERAND
 */
static OPERAND *
gen_acon_expr(int ilix, LL_Type *expected_type)
{
  int sptr, dtype;
  ISZ_T idx;
  LL_Type *ty1;
  OPERAND *base_op, *index_op;
  OPERAND *operand = NULL;
  const SPTR opnd = ILI_OPND(ilix, 1);
  const int ptrbits = 8 * size_of(DT_CPTR);
  INT val[2];
  ISZ_T num;

  assert(ILI_OPC(ilix) == IL_ACON, "gen_acon_expr: acon expected", ilix, 4);
  assert(STYPEG(opnd) == ST_CONST, "gen_acon_expr: ST_CONST argument expected",
         ilix, 4);

  /* Handle integer constants, converting to a pointer-sized integer */
  dtype = DTYPEG(opnd);
  if (DT_ISINT(dtype)) {
    INT hi = CONVAL1G(opnd);
    INT lo = CONVAL2G(opnd);

    /* Sign-extend DT_INT to 64 bits */
    if (dtype == DT_INT && ptrbits == 64)
      hi = lo < 0 ? -1 : 0;
    return make_constval_op(make_int_lltype(ptrbits), lo, hi);
  }

  /* With integers handled above, there should only be DT_CPTR constants left.
   * Apparently we sometimes generate DT_IPTR constants too (for wide string
   * constants) */
  assert(DTY(dtype) == TY_PTR,
         "gen_acon_expr: Expected pointer or integer constant", ilix, 4);

  /* Handle pointer constants with no base symbol table pointer.
   * This also becomes a pointer-sized integer */
  sptr = CONVAL1G(opnd);
  if (!sptr) {
    num = ACONOFFG(opnd);
    ISZ_2_INT64(num, val);
    return make_constval_op(make_int_lltype(ptrbits), val[1], val[0]);
  }
  sym_is_refd(sptr);
  process_sptr_offset(sptr, variable_offset_in_aggregate(sptr, ACONOFFG(opnd)));
  idx = ACONOFFG(opnd); /* byte offset */

  ty1 = make_lltype_from_dtype(DT_ADDR);
  idx = variable_offset_in_aggregate(sptr, idx);
  if (idx) {
    base_op = gen_sptr(sptr);
    index_op = NULL;
    base_op = make_bitcast(base_op, ty1);
    ISZ_2_INT64(idx, val);    /* make a index operand */
    index_op = make_constval_op(make_int_lltype(ptrbits), val[1], val[0]);
    operand = gen_gep_op(ilix, base_op, ty1, index_op);
  } else {
    operand = gen_sptr(sptr);
    /* SC_DUMMY - address constant .cxxxx */
    if (SCG(sptr) == SC_DUMMY &&
        (CUDAG(gbl.currsub) & (CUDA_GLOBAL | CUDA_DEVICE)) &&
        DTYPEG(sptr) == DT_ADDR) {
      /* scalar argument */
      int midnum = MIDNUMG(sptr);
      if (midnum && DEVICEG(midnum) && !PASSBYVALG(midnum))
        operand->ll_type = make_ptr_lltype(operand->ll_type);
      else if (DTY(DTYPEG(midnum)) == TY_PTR ||
               DTY(DTYPEG(midnum)) == TY_ARRAY) /* pointer */
        operand->ll_type = make_ptr_lltype(operand->ll_type);
    }
  }

  if (operand->ll_type && VOLG(sptr))
    operand->flags |= OPF_VOLATILE;
  return operand;
}

/**
   \brief Pattern match the ILI tree and fold when there is a match
   \param addr  The ILI to pattern match
   \param size  The expected type size
 */
static OPERAND *
attempt_gep_folding(int addr, BIGINT64 size)
{
  int kmul, kcon;
  BIGINT64 val;
  OPERAND *op;

  if (ILI_OPC(addr) != IL_KAMV)
    return NULL;
  kmul = ILI_OPND(addr, 1);
  if (ILI_OPC(kmul) != IL_KMUL)
    return NULL;
  kcon = ILI_OPND(kmul, 2);
  if (ILI_OPC(kcon) != IL_KCON)
    return NULL;
  val = ((BIGINT64)CONVAL1G(ILI_OPND(kcon, 1))) << 32;
  val |= CONVAL2G(ILI_OPND(kcon, 1)) & (0xFFFFFFFF);
  if (val != size)
    return NULL;
  /* at this point we are going to drop the explicit multiply */
  op = gen_llvm_expr(ILI_OPND(kmul, 1), make_int_lltype(64));
  return op;
}

/**
   \brief Attempt to convert explicit pointer scaling into GEP
   \param aadd	An IL_AADD
   \param idxOp	The index expression to be checked

   Do \e not assume \p idxOp is the same as <tt>ILI_OPND(aadd, 2)</tt>.
 */
static OPERAND *
maybe_do_gep_folding(int aadd, int idxOp, LL_Type *ty)
{
  int baseOp;
  OPERAND *rv;
  LL_Type *i8ptr;
  unsigned savedAddressElementSize;

  if (addressElementSize == 0)
    return NULL;

  baseOp = ILI_OPND(aadd, 1);
  i8ptr = make_lltype_from_dtype(DT_CPTR);
  if (ty == i8ptr) {
    if (addressElementSize != TARGET_PTRSIZE)
      return NULL;
    ty = ll_get_pointer_type(ty);
  }

  savedAddressElementSize = addressElementSize;
  addressElementSize = 0;

  /* 1. check if idxOp is a scaled expression */
  rv = attempt_gep_folding(idxOp, savedAddressElementSize);
  if (rv) {
    OPERAND *base = gen_base_addr_operand(baseOp, ty);
    rv = gen_gep_op(aadd, base, ty, rv);
    if (rv->tmps && rv->tmps->info.idef)
      rv->tmps->info.idef->traceComment = "mul optim";
    return rv;
  }

  /* 2. check if baseOp is a scaled expression */
  rv = attempt_gep_folding(baseOp, savedAddressElementSize);
  if (rv) {
    OPERAND *index = gen_base_addr_operand(idxOp, ty);
    rv = gen_gep_op(aadd, index, ty, rv);
    if (rv->tmps && rv->tmps->info.idef)
      rv->tmps->info.idef->traceComment = "mul optim'";
    return rv;
  }

  addressElementSize = savedAddressElementSize;
  return NULL;
}

static OPERAND *
gen_base_addr_operand(int ilix, LL_Type *expected_type)
{
  OPERAND *operand = NULL, *base_op, *index_op, *cast_op;
  OPERAND **csed_operand;
  LL_Type *ty1, *ty2;
  int opnd = 0;
  int nme;

  DBGTRACEIN2(" for ilix: %d(%s), expected_type ", ilix, IL_NAME(ILI_OPC(ilix)))
  DBGDUMPLLTYPE("expected type ", expected_type)

  switch (ILI_OPC(ilix)) {
  case IL_ASUB:
    if (!ll_type_int_bits(expected_type)) {
      switch (ILI_OPC(ILI_OPND(ilix, 2))) {
      case IL_IAMV:
        opnd = ad2ili(IL_ISUB, ad_icon(0), ILI_OPND(ilix, 2));
        break;
      case IL_KAMV:
        opnd = ad2ili(IL_KSUB, ad_kconi(0), ILI_OPND(ilix, 2));
        break;
      default:
        if (size_of(DT_CPTR) == 8) {
          opnd = ad1ili(IL_AKMV, ILI_OPND(ilix, 2));
          opnd = ad2ili(IL_KSUB, ad_kconi(0), opnd);
        } else {
          opnd = ad1ili(IL_AIMV, ILI_OPND(ilix, 2));
          opnd = ad2ili(IL_ISUB, ad_icon(0), opnd);
        }
      }
    } else {
      if (size_of(DT_CPTR) == 8) {
        opnd = ad1ili(IL_AKMV, ILI_OPND(ilix, 2));
        opnd = ad2ili(IL_KSUB, ad_kconi(0), opnd);
        opnd = ad1ili(IL_KAMV, opnd);
      } else {
        opnd = ad1ili(IL_AIMV, ILI_OPND(ilix, 2));
        opnd = ad2ili(IL_ISUB, ad_icon(0), opnd);
        opnd = ad1ili(IL_IAMV, opnd);
      }
    }
  case IL_AADD:
    opnd = opnd ? opnd : ILI_OPND(ilix, 2);
    operand = (XBIT(183, 0x40000)) ? NULL : maybe_do_gep_folding(ilix, opnd,
                                                                 expected_type);
    if (!operand) {
      ty1 = make_lltype_from_dtype(DT_CPTR);
      base_op = gen_base_addr_operand(ILI_OPND(ilix, 1), ty1);
      ty2 = make_int_lltype(8 * size_of(DT_CPTR));
      index_op = gen_base_addr_operand(opnd, ty2);
      operand = gen_gep_op(ilix, base_op, ty1, index_op);
    }
    break;
  case IL_ACON:
    operand = gen_acon_expr(ilix, expected_type);
    break;
  default:
    /* third arg must be 0 since we're not generating a GEP in this case */
    operand = gen_llvm_expr(ilix, expected_type);
  }
  if (expected_type)
    ty1 = expected_type;
  else
    goto _exit_gen_base_addr_operand;
  ty2 = operand->ll_type;

  DBGDUMPLLTYPE("#operand type ", ty2);
  DBGDUMPLLTYPE("#expected type ", ty1);

  if (ll_type_int_bits(ty1) && ll_type_int_bits(ty2)) {
    if (ll_type_int_bits(ty1) != ll_type_int_bits(ty2)) {
      operand = convert_int_size(ilix, operand, ty1);
    }
    goto _exit_gen_base_addr_operand;
  } else if ((ty1->data_type == LL_PTR) && (ty2->data_type == LL_PTR)) {
    /* both pointers, but pointing to different types */
    LL_Type *tty1 = NULL, *tty2 = NULL;

    DBGTRACE("#both are pointers, checking if they are pointing to same type")

    if (ty2->sub_types[0]->data_type == LL_ARRAY) {
      tty1 = ty1;
      tty2 = ty2;
    }
    if (tty1 || tty2) {

      while (tty1->data_type == tty2->data_type) {
        if ((tty1->data_type == LL_PTR) || (tty1->data_type == LL_ARRAY)) {
          tty1 = tty1->sub_types[0];
          tty2 = tty2->sub_types[0];
        } else {
          break;
        }
      }
      if (ll_type_int_bits(tty1) && ll_type_int_bits(tty2) &&
          (ll_type_int_bits(tty1) != ll_type_int_bits(tty2))) {
        const int flags = operand->flags & (OPF_SEXT | OPF_ZEXT | OPF_VOLATILE);
        operand = make_bitcast(operand, ty1);
        operand->flags |= flags;
      } else if (tty1->data_type != LL_NOTYPE) {
        operand = make_bitcast(operand, ty1);
      }
    } else if (!strict_match(ty1->sub_types[0], ty2->sub_types[0])) {
      DBGTRACE("#no strict match between pointers")

      operand = make_bitcast(operand, ty1);
    } else {
      LL_Type *ety1 = ty1->sub_types[0];
      LL_Type *ety2 = ty2->sub_types[0];
      DBGTRACE("#strict match between pointers,"
               " checking signed/unsigned conflicts");
      while (ety1->data_type == ety2->data_type) {
        if ((ety1->data_type == LL_PTR) || (ety1->data_type == LL_ARRAY)) {
          ety1 = ety1->sub_types[0];
          ety2 = ety2->sub_types[0];
        } else {
          break;
        }
      }
      if (ll_type_int_bits(ety1) && ll_type_int_bits(ety2) &&
          (ll_type_int_bits(ety1) != ll_type_int_bits(ety2))) {
        const int flags = operand->flags & (OPF_SEXT | OPF_ZEXT | OPF_VOLATILE);
        operand = make_bitcast(operand, ty1);
        operand->flags |= flags;
      }
    }
  } else if ((ty1->data_type == LL_PTR) && ll_type_int_bits(ty2)) {
    if ((operand->ot_type == OT_CONSTVAL) && (!operand->val.conval[0]) &&
        (!operand->val.conval[1])) {
      // rewrite: cast(iN 0) to T*  ==>  (T* null)
      operand = make_constval_op(ty1, 0, 0);
      operand->flags |= OPF_NULL_TYPE;
    } else if ((operand->ot_type != OT_VAR) ||
               (!ll_type_int_bits(ty1->sub_types[0]))) {
      operand = convert_int_to_ptr(operand, ty1);
    }
  } else if (ty1->data_type == LL_PTR && ty2->data_type == LL_STRUCT) {
    operand->ll_type = make_ptr_lltype(ty2);
    operand = make_bitcast(operand, ty1);
  } else if (ty1->data_type == LL_PTR && ty2->data_type == LL_VECTOR &&
             !strict_match(ty1->sub_types[0], ty2)) {
    operand->ll_type = make_ptr_lltype(ty2);
    operand = make_bitcast(operand, ty1);
  } else if (ll_type_int_bits(ty1) && (ty2->data_type == LL_PTR)) {
    operand = convert_ptr_to_int(operand, ty1);
  } else if (ty1->data_type == LL_PTR && ty2->data_type == LL_ARRAY) {
    operand = make_bitcast(operand, ty1);
  } else if (ty1->data_type != ty2->data_type) {
    if (ty1->data_type == LL_PTR && operand->ot_type == OT_VAR) {
      ty1 = ty1->sub_types[0];
      while (ty1->data_type == ty2->data_type) {
        if (ty1->data_type == LL_PTR || ty1->data_type == LL_ARRAY) {
          ty1 = ty1->sub_types[0];
          ty2 = ty2->sub_types[0];
        } else {
          break;
        }
      }
      if (ty1->data_type == ty2->data_type || ty1->data_type == LL_VOID)
        goto _exit_gen_base_addr_operand;
      if (ll_type_int_bits(ty1) && (ty2->data_type == LL_FLOAT) &&
          ll_type_bytes(ty1) == 4) {
        operand = make_bitcast(operand, ty1);
        goto _exit_gen_base_addr_operand;
      }
    }
    assert(0, "gen_base_addr_operand(): unexpected conversion", 0, 0);
  }
_exit_gen_base_addr_operand:
  csed_operand = get_csed_operand(ilix);
  if (csed_operand != NULL)
    set_csed_operand(csed_operand, operand);
  ILI_COUNT(ilix)++;

  DBGTRACEOUT4(" returns operand %p, tmps %p, count %d for ilix %d", operand,
               operand->tmps, ILI_COUNT(ilix), ilix)
  setTempMap(ilix, operand);
  return operand;
}

void
print_tmp_name(TMPS *t)
{
  char tmp[10];
  int idx = 0;

  if (!t) {
    idx = ++expr_id;
    sprintf(tmp, "%%%d", idx - 1);
    print_token(tmp);
    return;
  }

  if (!t->id)
    t->id = ++expr_id;
  sprintf(tmp, "%%%d", t->id - 1);
  print_token(tmp);
}

static LOGICAL
repeats_in_binary(union xx_u xx)
{
  LOGICAL ret_val;
  double dd = (double)xx.ff;

  if (!llvm_info.no_debug_info) {
    DBGTRACEIN1(" input value: %g \n", dd)
  }

  ret_val = TRUE;
  if (!llvm_info.no_debug_info) {
    DBGTRACEOUT1(" returns %s", ret_val ? "TRUE" : "FALSE")
  }
  return ret_val;
} /* repeats_in_binary */

static char *
gen_vconstant(const char *ctype, int sptr, int tdtype, int flags)
{
  int vdtype;
  int vsize;
  int i;
  int edtype;
  static char tmp_vcon_buf[2000];
  char *vctype, *constant;

  vdtype = DTY(tdtype + 1);
  vsize = DTY(tdtype + 2);
  edtype = CONVAL1G(sptr);

  if (flags & FLG_OMIT_OP_TYPE) {
    tmp_vcon_buf[0] = '<';
    tmp_vcon_buf[1] = '\0';
  } else
    sprintf(tmp_vcon_buf, "%s <", ctype);

  for (i = 0; i < vsize; i++) {
    if (i)
      strcat(tmp_vcon_buf, ", ");
    switch (DTY(vdtype)) {
    case TY_REAL:
      strcat(tmp_vcon_buf, gen_constant(0, vdtype, VCON_CONVAL(edtype + i), 0,
                                        flags & ~FLG_OMIT_OP_TYPE));
      break;
    case TY_INT8:
    case TY_DBLE:
      strcat(tmp_vcon_buf, gen_constant(VCON_CONVAL(edtype + i), 0, 0, 0,
                                        flags & ~FLG_OMIT_OP_TYPE));
      break;
    default:
      strcat(tmp_vcon_buf, gen_constant(0, vdtype, VCON_CONVAL(edtype + i), 0,
                                        flags & ~FLG_OMIT_OP_TYPE));
    }
  }
  strcat(tmp_vcon_buf, ">");
  constant = (char *)getitem(LLVM_LONGTERM_AREA,
                             strlen(tmp_vcon_buf) + 1); /* room for \0 */
  strcpy(constant, tmp_vcon_buf);
  return constant;
}

char *
gen_llvm_vconstant(const char *ctype, int sptr, int tdtype, int flags)
{
  return gen_vconstant(ctype, sptr, tdtype, flags);
}

static char *
gen_constant(int sptr, int tdtype, INT conval0, INT conval1, int flags)
{
  int dtype;
  INT num[2] = {0, 0};
  union xx_u xx;
  union {
    double d;
    INT tmp[2];
  } dtmp, dtmp2;
  char *constant, *constant1, *constant2;
  char *ctype = "";
  int size = 0;

  static char d[MAXIDLEN];
  static char *b = NULL;

  if (b == NULL) {
    NEW(b, char, 100);
  }

  assert((sptr || tdtype), "gen_constant(): missing arguments", 0, 4);
  if (sptr)
    dtype = DTYPEG(sptr);
  else
    dtype = tdtype;

  if (!llvm_info.no_debug_info) {
    DBGTRACEIN3(" sptr %d, dtype:%d(%s)", sptr, dtype, stb.tynames[DTY(dtype)])
  }

  if (!(flags & FLG_OMIT_OP_TYPE)) {
    ctype = llvm_fc_type(dtype);
    size += strlen(ctype) + 1; /* include room for space after the type */
  }
  /* Use an enum's underlying type. */

  if (dtype && DTY(dtype) == TY_VECT)
    return gen_vconstant(ctype, sptr, dtype, flags);

  switch (dtype) {
  case DT_INT:
  case DT_SINT:
  case DT_BINT:
  case DT_USINT:
  case DT_UINT:
  case DT_LOG:
  case DT_SLOG:
  case DT_BLOG:
#if !LONG_IS_64
#endif

    if (sptr)
      sprintf(b, "%ld", (long)CONVAL2G(sptr));
    else
      sprintf(b, "%ld", (long)conval0); /* from dinit info */
    size += strlen(b);

    if (!llvm_info.no_debug_info) {
      DBGTRACE2("#generating integer value: %s %s\n",
                char_type(dtype, sptr), b)
    }

    constant = (char *)getitem(LLVM_LONGTERM_AREA, size + 1); /* room for \0 */
    if (flags & FLG_OMIT_OP_TYPE)
      sprintf(constant, "%s", b);
    else
      sprintf(constant, "%s %s", ctype, b);
    break;
#if LONG_IS_64
#endif
  case DT_INT8:
  case DT_UINT8:
  case DT_LOG8:
    if (sptr) {
      num[1] = CONVAL2G(sptr);
      num[0] = CONVAL1G(sptr);
    } else {
      num[1] = conval0;
      num[0] = conval1;
    }
    ui64toax(num, b, 22, 0, 10);
    size += strlen(b);

    if (!llvm_info.no_debug_info) {
      DBGTRACE2("#generating integer value: %s %s\n",
                char_type(dtype, sptr), b)
    }

    constant = (char *)getitem(LLVM_LONGTERM_AREA, size + 1); /* room for \0 */
    if (flags & FLG_OMIT_OP_TYPE)
      sprintf(constant, "%s", b);
    else
      sprintf(constant, "%s %s", ctype, b);
    break;

  case DT_DBLE:
  case DT_QUAD:

    if (sptr) {
      num[0] = CONVAL1G(sptr);
      num[1] = CONVAL2G(sptr);
    } else {
      num[0] = conval0;
      num[1] = conval1;
    }

    cprintf(d, "%.17le", num);
    /* Check for  `+/-Infinity` and 'NaN' based on the IEEE bit patterns */
    if ((num[0] & 0x7ff00000) == 0x7ff00000) /* exponent == 2047 */
      sprintf(d, "0x%08x00000000", num[0]);
    /* also check for -0 */
    else if (num[0] == 0x80000000 && num[1] == 0x00000000)
      sprintf(d, "-0.00000000e+00");
    /* remember to make room for /0 */
    constant =
        (char *)getitem(LLVM_LONGTERM_AREA, strlen(d) + strlen(ctype) + 1);
    if (flags & FLG_OMIT_OP_TYPE)
      sprintf(constant, "%s", d);
    else
      sprintf(constant, "%s %s", ctype, d);
    if (!llvm_info.no_debug_info) {
      DBGTRACE1("#set double exponent value to %s", d)
    }
    break;
  case DT_REAL:
    /* our internal representation of floats is in 8 digit hex form;
     * internal LLVM representation of floats in hex form is 16 digits;
     * thus we must make the conversion. Also need to decide when to
     * represent final float form in exponential or hexadecimal form.
     */
    if (sptr)
      xx.ww = CONVAL2G(sptr);
    else
      xx.ww = conval0;
    xdble(xx.ww, dtmp2.tmp);
    xdtomd(dtmp2.tmp, &dtmp.d);
    snprintf(d, 200, "%.8e", dtmp.d);
    size += 19;
    constant = (char *)getitem(
        LLVM_LONGTERM_AREA,
        size + 2); /* room for \0  and potentially a '-' sign for neg_zero */
    constant1 = (char *)getitem(LLVM_LONGTERM_AREA, 9);
    constant2 = (char *)getitem(LLVM_LONGTERM_AREA, 9);
    if (repeats_in_binary(xx)) {
      /* put in hexadecimal form unless neg 0 */
      if (dtmp.tmp[0] == -1) /* pick up the quiet nan */
        sprintf(constant1, "7FF80000");
      else if (!dtmp.tmp[1])
        sprintf(constant1, "00000000");
      else
        sprintf(constant1, "%X", dtmp.tmp[1]);
      if (!dtmp.tmp[0] || dtmp.tmp[0] == -1)
        sprintf(constant2, "00000000");
      else
        sprintf(constant2, "%X", dtmp.tmp[0]);
      if (flags & FLG_OMIT_OP_TYPE)
        sprintf(constant, "0x%s%s", constant1, constant2);
      else
        sprintf(constant, "%s 0x%s%s", ctype, constant1, constant2);

      /* check for negative zero */
      if (dtmp.tmp[1] == 0x80000000 && !dtmp.tmp[0]) {
        if (flags & FLG_OMIT_OP_TYPE)
          sprintf(constant, "-0.000000e+00");
        else
          sprintf(constant, "%s -0.000000e+00", ctype);
      }
    } else {
      /*  put in exponential form */
      if (flags & FLG_OMIT_OP_TYPE)
        sprintf(constant, "%s", d);
      else
        sprintf(constant, "%s %s", ctype, d);
    }

    if (!llvm_info.no_debug_info) {
      DBGTRACE1("#set float exp value to %s", d)
      DBGTRACE2("#set float hex value to 0x%X%x", dtmp.tmp[1], dtmp.tmp[0])
    }

    break;

  default:
    if (!llvm_info.no_debug_info) {
      DBGTRACE3("### gen_constant; sptr %d, unknown dtype: %d(%s)", sptr, dtype,
                stb.tynames[DTY(dtype)])
    }
    assert(0, "gen_constant(): unexpected constant dtype", dtype, 4);
  }

  if (!llvm_info.no_debug_info) {
    DBGTRACEOUT1(" returns %s", constant)
  }
  return constant;
} /* gen_constant */

static char *
add_tmp_buf_list_item(TEMP_BUF_LIST **tempbuflist_ptr, int sz)
{
  int i;
  TEMP_BUF_LIST *last;

  for (last = *tempbuflist_ptr; last && last->next; last = last->next)
    ;

  if (*tempbuflist_ptr) {
    last->next =
        (TEMP_BUF_LIST *)getitem(CG_MEDTERM_AREA, sizeof(TEMP_BUF_LIST));
    last = last->next;
  } else {
    *tempbuflist_ptr = last =
        (TEMP_BUF_LIST *)getitem(CG_MEDTERM_AREA, sizeof(TEMP_BUF_LIST));
  }

  last->next = NULL;
  last->buf.buffer = (char *)getitem(CG_MEDTERM_AREA, sz);
  *last->buf.buffer = '\0';
  return last->buf.buffer;
}

static void
write_extern_fndecl(struct LL_FnProto_ *proto)
{
  /* Only print decls if we have not seen a body (must be external) */
  if (!proto->has_defined_body) {
    if (proto->intrinsic_decl_str)
      print_line(proto->intrinsic_decl_str);
    else {
      print_token("declare");
      if (proto->is_weak)
        print_token(" extern_weak");
      print_function_signature(0, proto->fn_name, proto->abi, FALSE);
      if (proto->abi->is_pure)
        print_token(" nounwind");
      if (proto->abi->is_pure)
        print_token(" readnone");
      if ((!flg.ieee || XBIT(216, 1)) && proto->abi->fast_math)
        print_token(" \"no-infs-fp-math\"=\"true\" "
                    "\"no-nans-fp-math\"=\"true\" "	
                    "\"unsafe-fp-math\"=\"true\" \"use-soft-float\"=\"false\""	
                    " \"no-signed-zeros-fp-math\"=\"true\"");
      print_nl();
    }
  }
}

void
write_external_function_declarations(int first_time)
{
  DBGTRACEIN("");

  if (first_time)
    print_nl();
  ll_proto_iterate(write_extern_fndecl);
  DBGTRACEOUT("");
} /* write_external_function_declarations */

/**
   \brief Emit function attributes in debugging mode output

   The <code>"no-frame-pointer-elim-non-leaf"</code> flag is included to
   generate better coverage of the function in the \c .eh_frame section. This is
   done primarily to help the profiler unwind the stack.
 */
INLINE static void
write_function_attributes(void)
{
  if (!need_debug_info(0))
    return;

  if (XBIT(183, 0x10)) {
    print_token("attributes #0 = "
                "{ \"no-frame-pointer-elim-non-leaf\" }\n");
    return;
  }
  print_token("attributes #0 = "
              "{ noinline \"no-frame-pointer-elim-non-leaf\" }\n");
}

static void
write_global_and_static_defines(void)
{
  GBL_LIST *gl;
  for (gl = Globals; gl; gl = gl->next) {
    if ((STYPEG(gl->sptr) == ST_CONST)
        || ((SCG(gl->sptr) == SC_LOCAL) && DINITG(gl->sptr) && !REFG(gl->sptr))
        || ((SCG(gl->sptr) == SC_EXTERN) && (STYPEG(gl->sptr) == ST_VAR) &&
            (DTYPEG(gl->sptr) == DT_ADDR))) {
      print_line(gl->global_def);
    }
  }
  Globals = NULL;
}

static void
build_unused_global_define_from_params(void)
{
  return;
}

/**
   \brief Helper function: In Fortran, test if \c MIDNUM is not \c SC_DUMMY
   \param sptr   a symbol
 */
INLINE static bool
formalsMidnumNotDummy(SPTR sptr)
{
  return SCG(MIDNUMG(sptr)) != SC_DUMMY;
}

/**
   \brief Helper function: Get \c DTYPE of \p s
   \param s  a symbol
   Fortran requires special handling for ST_PROC.
 */
INLINE static DTYPE
formalsGetDtype(SPTR s)
{
  return ((STYPEG(s) == ST_PROC) && (!DTYPEG(s))) ? DT_ADDR : DTYPEG(s);
}

INLINE static OPERAND *
cons_expression_metadata_operand(LL_Type *llTy)
{
  // FIXME: we don't need to always do this, do we? do a type check here
  LL_DebugInfo *di = cpu_llvm_module->debug_info;
  unsigned v = lldbg_encode_expression_arg(LL_DW_OP_deref, 0);
  LL_MDRef exprMD = lldbg_emit_expression_mdnode(di, 1, v);
  return make_mdref_op(exprMD);
}

/**
   \brief Helper function: add debug information for formal argument
   \param sptr  a symbol
   \param i     parameter position
 */
INLINE static void
formalsAddDebug(SPTR sptr, unsigned i, LL_Type *llType)
{
  if (need_debug_info(sptr)) {
    LL_MDRef param_md = lldbg_emit_param_variable(cpu_llvm_module->debug_info,
                                               sptr, BIH_FINDEX(gbl.entbih), i);
    LL_Type *llTy = fixup_argument_type(sptr, llType);
    OPERAND *exprMDOp = cons_expression_metadata_operand(llTy);
    insert_llvm_dbg_declare(param_md, sptr, llTy, exprMDOp);
  }
}

/**
   \brief Process the formal arguments to the current function

   Generate therequired prolog code to home all arguments that need it.
 
   \c llvm_info.abi_info must be initialized before calling this function.
 
   Populates \c llvm_info.homed_args, which must be allocated and empty before
   the call.
 */
void
process_formal_arguments(LL_ABI_Info *abi)
{
  /* Entries already have been processed */
  unsigned i;

  for (i = 1; i <= abi->nargs; i++) {
    OPERAND *arg_op;
    OPERAND *store_addr;
    LL_Type *llTy;
    unsigned flags;
    int key;
    LL_ABI_ArgInfo *arg = &abi->arg[i];
    const char *suffix = ".arg";
    bool ftn_byval = false;

    assert(arg->sptr, "Unnamed function argument", i, ERR_Fatal);
    assert(SNAME(arg->sptr) == NULL, "Argument sptr already processed",
           arg->sptr, ERR_Fatal);

    if ((SCG(arg->sptr) != SC_DUMMY) && formalsMidnumNotDummy(arg->sptr)) {
      process_sptr(arg->sptr);
      continue;
    }

    switch (arg->kind) {
    case LL_ARG_BYVAL:
      if (abi->is_fortran && !abi->is_iso_c) {
        ftn_byval = true;
        break;
      }
      /* falls thru */
    case LL_ARG_INDIRECT:
      /* For device pointer, we need to home it because we will need to pass it
       * as &&arg(pointer to pointer), make_var_op will call process_sptr later.
       */
      if (DEVICEG(arg->sptr) &&
          (CUDAG(gbl.currsub) & (CUDA_GLOBAL | CUDA_DEVICE)))
        break;
      /* These arguments already appear as pointers. Should we make a copy of
       * an indirect arg? The caller doesn't expect us to modify the memory.
       */
      process_sptr(arg->sptr);
      key = ((SCG(arg->sptr) == SC_BASED) && MIDNUMG(arg->sptr)) ?
        MIDNUMG(arg->sptr) : arg->sptr;
      llTy = llis_dummied_arg(key) ? make_generic_dummy_lltype() : LLTYPE(key);
      formalsAddDebug(key, i, llTy);
      continue;

    case LL_ARG_COERCE:
      /* This argument is passed by value as arg->type which is not the real
       * type of the argument. Generate code to save the LLVM argument into a
       * local variable of the right type. */
      suffix = ".coerce";
      break;

    default:
      /* Other by-value kinds. */
      break;
    }

    /* This op represents the real LLVM argument, not the local variable. */
    arg_op = make_operand();
    arg_op->ot_type = OT_VAR;
    arg_op->ll_type = make_lltype_from_abi_arg(arg);
    arg_op->val.sptr = arg->sptr;

    key = arg->sptr;
    /* if it is a pointer, should use midnum as hash key because most of
     * the time, the ILI is referencing to is MIDNUMG(x$p).
     * If there will ever be a reference to this SC_BASED directly,
     * we should always use its MIDNUMG for hashing.
     */
    if (SCG(arg->sptr) == SC_BASED && MIDNUMG(arg->sptr))
      key = MIDNUMG(arg->sptr);
    hashmap_insert(llvm_info.homed_args, INT2HKEY(key), arg_op);

    /* Process the argument sptr *after* updating homed_args.
     * process_sptr() will look at this map to treat the argument as an
     * auto instead of a dummy. */
    store_addr = make_var_op(arg->sptr);

    /* make sure it is pointer to pointer */
    if (DEVICEG(arg->sptr) &&
        (CUDAG(gbl.currsub) & (CUDA_GLOBAL | CUDA_DEVICE)) &&
        !(ftn_byval || PASSBYVALG(arg->sptr)))
      store_addr->ll_type = make_ptr_lltype(store_addr->ll_type);

    /* Make a name for the real LLVM IR argument. This will also be used by
     * build_routine_and_parameter_entries(). */
    arg_op->string = (char *)ll_create_local_name(llvm_info.curr_func, "%s%s",
                                                  get_llvm_name(arg->sptr), suffix);

    /* Emit code in the entry block that saves the argument into the local
     * variable. The pointer bitcast takes care of the coercion.
     *
     * FIXME: What if the coerced type is larger than the local variable?
     * We'll be writing outside its alloca. */
    if (store_addr->ll_type->sub_types[0] != arg->type)
      store_addr = make_bitcast(store_addr, make_ptr_lltype(arg_op->ll_type));

    flags = ldst_instr_flags_from_dtype(formalsGetDtype(arg->sptr));
    if (ftn_byval) {
      arg_op = make_load(0, arg_op, arg_op->ll_type->sub_types[0],
                         mem_size(TY_INT), 0);
      store_addr = make_var_op(arg->sptr);
    }
    make_store(arg_op, store_addr, flags);

    /* Emit an @llvm.dbg.declare right after the store. */
    formalsAddDebug(arg->sptr, i, LLTYPE(arg->sptr));
  }
}

/**
   \brief Write out attributes for a function argument or return value.
   \param arg  an argument's info record
 */
static void
print_arg_attributes(LL_ABI_ArgInfo *arg)
{
  switch (arg->kind) {
  case LL_ARG_DIRECT:
  case LL_ARG_COERCE:
  case LL_ARG_INDIRECT:
    break;
  case LL_ARG_ZEROEXT:
    print_token(" zeroext");
    break;
  case LL_ARG_SIGNEXT:
    print_token(" signext");
    break;
  case LL_ARG_BYVAL:
    print_token(" byval");
    break;
  default:
    interr("Unknown argument kind", arg->kind, ERR_Fatal);
  }
  if (arg->inreg)
    print_token(" inreg");
}

/**
 * \brief Print the signature of func_sptr, omitting the leading define/declare,
 * ending after the function attributes.
 *
 * When print_arg_names is set, also print the names of arguments. (From
 * abi->arg[n].sptr).
 *
 * fn_name is passed separately from the sptr, since Fortran also calls this
 * routine.  In the Fortran case, the sptr will not always be valid, but the
 * LL_FnProto contains a valid fn_name string.
 */
static void
print_function_signature(int func_sptr, const char *fn_name, LL_ABI_Info *abi,
                         LOGICAL print_arg_names)
{
  unsigned i;
  const char *param;
  LOGICAL need_comma = FALSE;

  /* Fortran treats functions with unknown prototypes as varargs,
   * we cannot decorate them with fastcc.
   */
  if (abi->call_conv &&
      !(abi->call_conv == LL_CallConv_Fast && abi->call_as_varargs)) {
    print_space(1);
    print_token(ll_get_calling_conv_str(abi->call_conv));
  }
#ifdef WEAKG
  if (func_sptr > NOSYM && WEAKG(func_sptr))
    print_token(" weak");
#endif

  /* Print function return type with attributes. */
  if (LL_ABI_HAS_SRET(abi)) {
    print_token(" void");
  } else {
    print_arg_attributes(&abi->arg[0]);
    print_space(1);
    print_token(abi->extend_abi_return ? make_lltype_from_dtype(DT_INT)->str :
                abi->arg[0].type->str);
  }

  print_token(" @");
  print_token(fn_name);
  print_token("(");

  /* Hidden sret argument for struct returns. */
  if (LL_ABI_HAS_SRET(abi)) {
    print_token(abi->arg[0].type->str);
    print_token(" sret");
    if (print_arg_names) {
      print_space(1);
      print_token(SNAME(ret_info.sret_sptr));
    }
    need_comma = TRUE;
  }

  /* Iterate over function arguments. */
  for (i = 1; i <= abi->nargs; i++) {
    LL_ABI_ArgInfo *arg = &abi->arg[i];

    if (need_comma)
      print_token(", ");

    print_token(arg->type->str);
    print_arg_attributes(arg);

    if (print_arg_names && arg->sptr) {
      int key;
      OPERAND *coerce_op = NULL;
      print_space(1);
      key = arg->sptr;
      if (SCG(arg->sptr) == SC_BASED && MIDNUMG(arg->sptr))
        key = MIDNUMG(arg->sptr);

      if (hashmap_lookup(llvm_info.homed_args, INT2HKEY(key),
                         (hash_data_t *)&coerce_op)) {
        print_token(coerce_op->string);
      } else {
        assert(SNAME(arg->sptr), "print_function_signature: "
                                 "No SNAME for sptr",
               arg->sptr, ERR_Fatal);
        print_token(SNAME(arg->sptr));
      }
    }
    need_comma = TRUE;
  }

  /* Finally, append ... for varargs functions. */
  if (ll_abi_use_llvm_varargs(abi)) {
    if (need_comma)
      print_token(", ");
    print_token("...");
  }

  print_token(")");

  /* Function attributes. */
  if (need_debug_info(0)) {
    /* 'attributes #0 = { ... }' to be emitted later */
    print_token(" #0");
  } else if (!XBIT(183, 0x10)) {
    /* Nobody sets -x 183 0x10, besides Flang. We're disabling LLVM inlining for
     * proprietary compilers. */
    print_token(" noinline");
  }

  if (func_sptr > NOSYM) {
/* print_function_signature() can be called with func_sptr=0 */
  }

}

/**
   \brief write out the header of the function definition

   Writes text from \c define to the label of the entry block.
 */
void
build_routine_and_parameter_entries(SPTR func_sptr, LL_ABI_Info *abi,
                                    LL_Module *module)
{
  const char *linkage = NULL;

  /* Start printing the defining line to the output file. */
  print_token("define");

  /* Function linkage. */
  if (SCG(func_sptr) == SC_STATIC)
    linkage = " internal";

  if (linkage)
    print_token(linkage);
  if (SCG(func_sptr) != SC_STATIC)
    llvm_set_unique_sym(func_sptr);
#ifdef WEAKG
  if (WEAKG(func_sptr))
    ll_proto_set_weak(ll_proto_key(func_sptr), TRUE);
#endif

  print_function_signature(func_sptr, get_llvm_name(func_sptr), abi, TRUE);

  /* As of LLVM 3.8 the DISubprogram metadata nodes no longer bear
   * 'function' members that address the code for the subprogram.
   * Now, the references are reverse, the function definition carries
   * a !dbg metadata reference to the subprogram.
   */
  if (module && module->debug_info &&
      ll_feature_debug_info_ver38(&module->ir)) {
    LL_MDRef subprogram = lldbg_subprogram(module->debug_info);
    if (!LL_MDREF_IS_NULL(subprogram)) {
      print_dbg_line_no_comma(subprogram);
    }
  }

  print_line(" {"); /* } so vi matches */
  print_line("L.entry:");

  ll_proto_set_defined_body(ll_proto_key(func_sptr), TRUE);
}

static bool
exprjump(ILI_OP opc)
{
  switch (opc) {
  case IL_UKCJMP:
  case IL_KCJMP:
  case IL_ICJMP:
  case IL_FCJMP:
  case IL_DCJMP:
  case IL_ACJMP:
  case IL_UICJMP:
    return true;
  default:
    break;
  }
  return false;
}

static bool
zerojump(ILI_OP opc)
{
  switch (opc) {
  case IL_KCJMPZ:
  case IL_UKCJMPZ:
  case IL_ICJMPZ:
  case IL_FCJMPZ:
  case IL_DCJMPZ:
  case IL_ACJMPZ:
  case IL_UICJMPZ:
    return true;
  default:
    break;
  }
  return false;
}

/**
   \brief Get the string representation of the type of \p sptr or \p dtype
   \param sptr  symbol to use for type consing or 0
   \param dtype used when \p sptr not provided
 */
const char *
char_type(int dtype, int sptr)
{
  LL_Type *ty;

  if (sptr && (DTYPEG(sptr) == dtype)) {
    ty = make_lltype_from_sptr(sptr);
    if (need_ptr(sptr, SCG(sptr), dtype))
      ty = ty->sub_types[0];
  } else {
    ty = make_lltype_from_dtype(dtype);
  }
  return ty->str;
}

static void
finish_routine(void)
{
  const int currFn = gbl.currsub;
  /***** "{" so vi matches *****/
  print_line("}");
  llassem_end_func(cpu_llvm_module->debug_info, currFn);
  if (flg.smp) {
    ll_reset_outlined_func();
  }
}

/**
   \brief Update the shadow symbol arrays

   When adding new symbols or starting a new routine, make sure the shadow
   symbol arrays and dtype debug array are updated.
 */
static void
update_llvm_sym_arrays(void)
{
  const int new_size = stb.symavl + MEM_EXTRA;
  int old_last_sym_avail = llvm_info.last_sym_avail; // NEEDB assigns
  NEEDB(stb.symavl, sptr_array, char *, llvm_info.last_sym_avail, new_size);
  NEEDB(stb.symavl, sptr_type_array, LL_Type *, old_last_sym_avail, new_size);
  if ((flg.debug || XBIT(120, 0x1000)) && cpu_llvm_module) {
    lldbg_update_arrays(cpu_llvm_module->debug_info, llvm_info.last_dtype_avail,
                        stb.dt_avail + MEM_EXTRA);
  }
}

void
cg_llvm_init(void)
{
  int i, dtype, return_dtype;
  const char *triple = "";
  enum LL_IRVersion ir_version;

  if (init_once) {
    update_llvm_sym_arrays();
    return;
  }
  ll_proto_init();
  routine_count = 0;

  CHECK(TARGET_PTRSIZE == size_of(DT_CPTR));

  triple = LLVM_DEFAULT_TARGET_TRIPLE;

  ir_version = get_llvm_version();

  if (!cpu_llvm_module)
    cpu_llvm_module = ll_create_module(gbl.file_name, triple, ir_version);
  llvm_info.declared_intrinsics = hashmap_alloc(hash_functions_strings);

  llvm_info.homed_args = hashmap_alloc(hash_functions_direct);

#if DEBUG
  ll_dfile = gbl.dbgfil ? gbl.dbgfil : stderr;
#endif

  llvm_info.last_dtype_avail = stb.dt_avail + 2000;
  /* set up sptr array - some extra for symbols that may need to be added */
  /* last_sym_avail is used for all the arrays below */
  llvm_info.last_sym_avail = stb.symavl + MEM_EXTRA;

  NEW(sptr_array, char *, stb.symavl + MEM_EXTRA);
  BZERO(sptr_array, char *, stb.symavl + MEM_EXTRA);
  /* set up the type array shadowing the symbol table */
  NEW(sptr_type_array, LL_Type *, stb.symavl + MEM_EXTRA);
  BZERO(sptr_type_array, LL_Type *, stb.symavl + MEM_EXTRA);

  Globals = NULL;
  recorded_Globals = NULL;

  /* get a count of the number of routines in this file */
  for (i = gbl.entries; i > NOSYM; i = SYMLKG(i)) {
    routine_count++;
  }

  entry_bih = gbl.entbih;
#if DEBUG
  if (DBGBIT(12, 0x10)) {
    indent(0);
    if (routine_count)
      fprintf(ll_dfile, "# %d routines in file %s\n", routine_count,
              entry_bih ? FIH_FILENAME(BIH_FINDEX(entry_bih))
                        : "FILENAME(gbl.entbih) NOT SET");
    else
      fprintf(ll_dfile, "# no routine in file\n");
  }
#endif

  if (flg.debug || XBIT(120, 0x1000)) {
    lldbg_init(cpu_llvm_module);
  }

  init_once = TRUE;
  assem_init();
  if (!ftn_init_once && FTN_HAS_INIT() == 0)
    init_output_file();

  ftn_init_once = TRUE;

  write_ftn_typedefs();
} /* cg_llvm_init */

/**
   \brief Process the end of the file (Fortran)

   Dumps the metadata for the Module.
 */
void
cg_llvm_end(void)
{
  write_function_attributes();
  ll_write_metadata(llvm_file(), cpu_llvm_module);
}

/**
   \brief Process the end of the SUBROUTINE (Fortran)

   In Fortran, we carry over data from the LONGTERM_AREA to the next subroutine
   to be processed.
 */
void
cg_llvm_fnend(void)
{
  if (!init_once) {
    cg_llvm_init();
  }
  write_global_and_static_defines();
  write_ftn_typedefs();
  Globals = NULL;

  /* Note that this function is called for every routine.  */
  assem_end();
  init_once = FALSE;
  llutil_struct_def_reset();
  ll_reset_module_types(cpu_llvm_module);

  recorded_Globals = NULL;

  freearea(CG_MEDTERM_AREA);
}

LOGICAL
is_cg_llvm_init(void)
{
  return init_once;
}

/**
   \brief Insert the jump entry instruction

   Insert compare and jump instruction to correct "entry" based on the first
   argument of the routine.
 */
static void
insert_jump_entry_instr(int ilt)
{
  int sptr, lab, sym;
  int *dpdscp;
  INT val = 0;

  if (!has_multiple_entries(gbl.currsub))
    return;

  dpdscp = (int *)(aux.dpdsc_base + DPDSCG(master_sptr));
  sym = *dpdscp;
  assert(hashmap_lookup(llvm_info.homed_args, INT2HKEY(sym), NULL),
         "Expected homed master-entry-choice sptr", sym, 4);

  for (sptr = gbl.entries; sptr > NOSYM; sptr = SYMLKG(sptr)) {
    /* The first arg (choice) is homed via process_formal_arguments() */
    INSTR_LIST *Curr_Instr;
    OPERAND *choice_op = make_var_op(sym);
    OPERAND *load_op = gen_load(choice_op, make_lltype_from_dtype(DT_INT),
                                ldst_instr_flags_from_dtype(DT_INT));

    OPERAND *operand = make_tmp_op(make_int_lltype(1), make_tmps());
    operand->tmps->use_count++;
    Curr_Instr =
        gen_instr(I_ICMP, operand->tmps, operand->ll_type, make_operand());
    Curr_Instr->operands->ot_type = OT_CC;
    Curr_Instr->operands->val.cc = convert_to_llvm_cc(CC_EQ, CMP_INT);
    Curr_Instr->operands->ll_type = make_type_from_opc(IL_ICMP);
    Curr_Instr->operands->next = load_op;
    Curr_Instr->operands->next->next =
        gen_llvm_expr(ad_icon(val), make_lltype_from_dtype(DT_INT));
    ad_instr(0, Curr_Instr);
    val++;

    lab = getlab();
    Curr_Instr = make_instr(I_BR);
    Curr_Instr->operands = make_tmp_op(make_int_lltype(1), operand->tmps);

    Curr_Instr->operands->next = make_target_op(sptr);
    Curr_Instr->operands->next->next = make_target_op(lab);
    ad_instr(0, Curr_Instr);

    /* label lab: */
    Curr_Instr = gen_instr(I_NONE, NULL, NULL, make_label_op(lab));
    ad_instr(0, Curr_Instr);
  }
}

static void
insert_entry_label(int ilt)
{
  int ilix = ILT_ILIP(ilt);
  SPTR sptr = ILI_OPND(ilix, 1);
  INSTR_LIST *Curr_Instr = gen_instr(I_NONE, NULL, NULL, make_label_op(sptr));
  ad_instr(0, Curr_Instr);
  if (gbl.arets)
    llvm_info.return_ll_type = make_lltype_from_dtype(DT_INT);
  else {
    llvm_info.return_ll_type =
        make_lltype_from_dtype(get_return_type(DTYPEG(sptr)));
  }
}

void
reset_expr_id(void)
{
  expr_id = 0;
}

static void
store_return_value_for_entry(OPERAND *p, int i_name)
{
  TMPS *tmp = make_tmps();

  if (p->ot_type != OT_VAR || !DT_ISCMPLX(DTYPEG(p->val.sptr))) {
    print_token("\t");
    print_tmp_name(tmp);
    print_token(" = bitcast ");
    write_type(make_generic_dummy_lltype());
    print_token(" %");
    print_token(get_entret_arg_name());
    print_token(" to ");
    write_type(make_ptr_lltype(p->ll_type));
    print_nl();

    print_token("\tstore ");
    write_type(p->ll_type);
    print_space(1);
    write_operand(p, "", FLG_OMIT_OP_TYPE);
    print_token(", ");

    write_type(make_ptr_lltype(p->ll_type));
    print_space(1);
    print_tmp_name(tmp);
    print_token(", align 4 ");
    print_nl();
  } else {
    TMPS *loadtmp;
    /*  %10 = bitcast i64* %__master_entry_rslt323 to <{float, float}>*  */
    print_token("\t");
    print_tmp_name(tmp);
    print_token(" = bitcast ");
    write_type(make_generic_dummy_lltype());
    print_token(" %");
    print_token(get_entret_arg_name());
    print_token(" to ");
    write_type(p->ll_type);
    print_nl();

    /*  %11 = load <{float, float}>, <{float, float}>* %cp1_300, align 4  */
    loadtmp = make_tmps();
    print_token("\t");
    print_tmp_name(loadtmp);
    print_token(" = load");
    write_type(p->ll_type->sub_types[0]);
    print_token(", ");
    write_type(p->ll_type);
    print_space(1);
    write_operand(p, "", FLG_OMIT_OP_TYPE);
    print_token(", align 4 ");
    print_nl();

    /*  store <{float, float}> %11, <{float, float}>* %10, align 4  */
    print_token("\tstore ");
    write_type(p->ll_type->sub_types[0]);
    print_space(1);
    print_tmp_name(loadtmp);
    print_token(", ");

    write_type(p->ll_type);
    print_space(1);
    print_tmp_name(tmp);
    print_token(", align 4 ");
    print_nl();
  }

  print_token("\t");
  print_token(llvm_instr_names[i_name]);
  print_space(1);
  print_token("void");
}

/*
 * Global initialization and finalization routines
 */

#define LLVM_DEFAULT_PRIORITY 65535

struct init_node {
  const char *name;
  int priority;
  struct init_node *next;
};

typedef struct {
  struct init_node *head;
  struct init_node *tail;
  int size;
} init_list_t;

static init_list_t llvm_ctor_list = {NULL, NULL, 0};
static init_list_t llvm_dtor_list = {NULL, NULL, 0};

static void
llvm_add_to_init_list(const char *name, int priority, init_list_t *list)
{
  struct init_node *node = malloc(sizeof(struct init_node));
  node->name = llutil_strdup(name);
  if (priority < 0 || priority > LLVM_DEFAULT_PRIORITY) {
    priority = LLVM_DEFAULT_PRIORITY;
  }
  node->priority = priority;
  node->next = NULL;

  if (list->head == NULL) {
    list->head = node;
    list->tail = node;
  } else {
    list->tail->next = node;
    list->tail = node;
  }
  ++(list->size);
}

void
llvm_ctor_add(const char *name)
{
  llvm_add_to_init_list(name, LLVM_DEFAULT_PRIORITY, &llvm_ctor_list);
}

void
llvm_ctor_add_with_priority(const char *name, int priority)
{
  llvm_add_to_init_list(name, priority, &llvm_ctor_list);
}

void
llvm_dtor_add(const char *name)
{
  llvm_add_to_init_list(name, LLVM_DEFAULT_PRIORITY, &llvm_dtor_list);
}

void
llvm_dtor_add_with_priority(const char *name, int priority)
{
  llvm_add_to_init_list(name, priority, &llvm_dtor_list);
}

static void
llvm_write_ctor_dtor_list(init_list_t *list, const char *global_name)
{
  struct init_node *node;
  char int_str_buffer[20];

  if (list->size == 0) {
    return;
  }

  print_token("@");
  print_token(global_name);
  print_token(" = appending global [");
  sprintf(int_str_buffer, "%d", list->size);
  print_token(int_str_buffer);
  print_token(" x { i32, void ()* }][");
  for (node = list->head; node != NULL; node = node->next) {
    print_token("{ i32, void ()* } { i32 ");
    sprintf(int_str_buffer, "%d", node->priority);
    print_token(int_str_buffer);
    print_token(", void ()* @");
    print_token(node->name);
    print_token(" }");
    if (node->next != NULL) {
      print_token(", ");
    }
  }
  print_token("]");
  print_nl();
}

void
llvm_write_ctors()
{
  llvm_write_ctor_dtor_list(&llvm_ctor_list, "llvm.global_ctors");
  llvm_write_ctor_dtor_list(&llvm_dtor_list, "llvm.global_dtors");
}
