#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "common.h"

#include "ir.h"
#include "ir_builder.h"

#define ir_CONST_STR(str) ir_const_str(_ir_CTX, ir_str(_ir_CTX, str))

static inline decode_t decode_at_address(const Instr_t* prog, uint32_t addr) {
    assert(addr < PROGRAM_SIZE);
    decode_t result = {0};
    Instr_t raw_instr = prog[addr];
    result.opcode = raw_instr;
    switch (raw_instr) {
    case Instr_Nop:
    case Instr_Halt:
    case Instr_Print:
    case Instr_Swap:
    case Instr_Dup:
    case Instr_Inc:
    case Instr_Add:
    case Instr_Sub:
    case Instr_Mul:
    case Instr_Rand:
    case Instr_Dec:
    case Instr_Drop:
    case Instr_Over:
    case Instr_Mod:
    case Instr_And:
    case Instr_Or:
    case Instr_Xor:
    case Instr_SHL:
    case Instr_SHR:
    case Instr_Rot:
    case Instr_SQRT:
    case Instr_Pick:
        result.length = 1;
        break;
    case Instr_Push:
    case Instr_JNE:
    case Instr_JE:
    case Instr_Jump:
        result.length = 2;
        if (!(addr+1 < PROGRAM_SIZE)) {
            result.length = 1;
            result.opcode = Instr_Break;
            break;
        }
        result.immediate = (int32_t)prog[addr+1];
        break;
    case Instr_Break:
    default: /* Undefined instructions equal to Break */
        result.length = 1;
        result.opcode = Instr_Break;
        break;
    }
    return result;
}

/*** Service routines ***/
typedef struct _jit_label {
    ir_ref inputs; /* number of input edges */
    ir_ref merge;  /* reference of MERGE or "list" of forward inputs */
} jit_label;

typedef struct _jit_ctx {
    ir_ctx     ctx;
    ir_ref     cpu;
#ifdef JIT_RESOLVE_STACK
# if defined(JIT_USE_VARS) || defined(JIT_USE_SSA)
    int        stack_limit;
    ir_ref    *vars;
# endif
    int        sp;
    int       *bb_sp; /* SP value at start of basic-block */
#endif
    jit_label *labels;
    ir_ref     stack_overflow;
    ir_ref     stack_underflow;
    ir_ref     stack_bound;
} jit_ctx;

#undef  _ir_CTX
#define _ir_CTX    (&jit->ctx)

static void jit_push(jit_ctx *jit, ir_ref v) {
#ifdef JIT_RESOLVE_STACK
    int sp = ++jit->sp;
# if defined(JIT_USE_VARS) || defined(JIT_USE_SSA)
    assert(sp < jit->stack_limit);
    // JIT: pcpu->stack[++pcpu->sp] = v;
    ir_VSTORE(jit->vars[sp], v);
# else
    assert(jit->sp < STACK_CAPACITY);
    // JIT: pcpu->stack[++pcpu->sp] = v;
    ir_STORE(ir_ADD_OFFSET(jit->cpu, offsetof(cpu_t, stack) + sp * sizeof(uint32_t)), v);
# endif
#else
    // JIT: if (pcpu->sp >= STACK_CAPACITY-1) {
    ir_ref sp_addr = ir_ADD_OFFSET(jit->cpu, offsetof(cpu_t, sp));
    ir_ref sp = ir_LOAD_I32(sp_addr);
    ir_ref if_overflow = ir_IF(ir_GE(sp, ir_CONST_I32(STACK_CAPACITY-1)));

    ir_IF_TRUE_cold(if_overflow);
    ir_END_list(jit->stack_overflow);

    ir_IF_FALSE(if_overflow);

    // JIT: pcpu->stack[++pcpu->sp] = v;
    sp = ir_ADD_I32(sp, ir_CONST_I32(1));
    ir_STORE(sp_addr, sp);
    ir_STORE(ir_ADD_I32(ir_ADD_OFFSET(jit->cpu, offsetof(cpu_t, stack)),
            ir_MUL_I32(sp, ir_CONST_I32(sizeof(uint32_t)))), v);
#endif
}

static ir_ref jit_pop(jit_ctx *jit) {
#ifdef JIT_RESOLVE_STACK
    int sp = jit->sp--;
    assert(sp >= 0);
# if defined(JIT_USE_VARS) || defined(JIT_USE_SSA)
    //JIT: pcpu->stack[pcpu->sp--];
    return ir_VLOAD_U32(jit->vars[sp]);
# else
    //JIT: pcpu->stack[pcpu->sp--];
    return ir_LOAD_U32(ir_ADD_OFFSET(jit->cpu, offsetof(cpu_t, stack) + sp * sizeof(uint32_t)));
# endif
#else
    // JIT: if (pcpu->sp < 0) {
    ir_ref sp_addr = ir_ADD_OFFSET(jit->cpu, offsetof(cpu_t, sp));
    ir_ref sp = ir_LOAD_I32(sp_addr);
    ir_ref if_underflow = ir_IF(ir_LT(sp, ir_CONST_I32(0)));

    ir_IF_TRUE_cold(if_underflow);
    ir_END_list(jit->stack_underflow);

    ir_IF_FALSE(if_underflow);

    //JIT: pcpu->stack[pcpu->sp--];
    ir_ref ret = ir_LOAD_U32(ir_ADD_I32(ir_ADD_OFFSET(jit->cpu, offsetof(cpu_t, stack)),
            ir_MUL_I32(sp, ir_CONST_I32(sizeof(uint32_t)))));
    sp = ir_SUB_I32(sp, ir_CONST_I32(1));
    ir_STORE(sp_addr, sp);

    return ret;
#endif
}

static ir_ref jit_pick(jit_ctx *jit, ir_ref pos) {
#ifdef JIT_RESOLVE_STACK
# if defined(JIT_USE_VARS) || defined(JIT_USE_SSA)
    assert(IR_IS_CONST_REF(pos));
    int sp = jit->ctx.ir_base[pos].val.i32;
    assert(sp >= 0 && sp < jit->stack_limit);
    return ir_VLOAD_U32(jit->vars[sp]);
# else
    // JIT: if (pcpu->sp - 1 < pos) {
    ir_ref if_out = ir_IF(ir_LT(ir_CONST_U32(jit->sp - 1), pos));

    ir_IF_TRUE_cold(if_out);
    ir_END_list(jit->stack_bound);

    ir_IF_FALSE(if_out);
    // JIT: pcpu->stack[pcpu->sp - pos];
    return ir_LOAD_U32(ir_ADD_I32(ir_ADD_OFFSET(jit->cpu, offsetof(cpu_t, stack)),
            ir_MUL_I32(ir_SUB_I32(ir_CONST_U32(jit->sp), pos), ir_CONST_I32(sizeof(uint32_t)))));
# endif
#else
    // JIT: if (pcpu->sp - 1 < pos) {
    ir_ref sp = ir_LOAD_I32(ir_ADD_OFFSET(jit->cpu, offsetof(cpu_t, sp)));
    ir_ref if_out = ir_IF(ir_LT(ir_SUB_U32(sp, ir_CONST_U32(1)), pos));

    ir_IF_TRUE_cold(if_out);
    ir_END_list(jit->stack_bound);

    ir_IF_FALSE(if_out);
    // JIT: pcpu->stack[pcpu->sp - pos];
    return ir_LOAD_U32(ir_ADD_I32(ir_ADD_OFFSET(jit->cpu, offsetof(cpu_t, stack)),
            ir_MUL_I32(ir_SUB_I32(sp, pos), ir_CONST_I32(sizeof(uint32_t)))));
#endif
}

static void jit_goto_backward(jit_ctx *jit, uint32_t target) {
    ir_set_op(_ir_CTX, jit->labels[target].merge, ++jit->labels[target].inputs, ir_END());
#ifdef JIT_RESOLVE_STACK
    assert(jit->bb_sp[target] == -1 || jit->bb_sp[target] == jit->sp);
    jit->bb_sp[target] = jit->sp;
#endif
}

static void jit_goto_forward(jit_ctx *jit, uint32_t target) {
    ir_END_list(jit->labels[target].merge);
#ifdef JIT_RESOLVE_STACK
    assert(jit->bb_sp[target] == -1 || jit->bb_sp[target] == jit->sp);
    jit->bb_sp[target] = jit->sp;
#endif
}

static void jit_program(jit_ctx *jit, const Instr_t *prog, int len) {
    assert(prog);
    decode_t decoded;
    ir_ref tmp1, tmp2, tmp3;

    ir_START();
    jit->cpu = ir_PARAM(IR_ADDR, "cpu", 1);

    jit->labels = calloc(len, sizeof(jit_label));
    jit->stack_overflow = IR_UNUSED;
    jit->stack_underflow = IR_UNUSED;
    jit->stack_bound = IR_UNUSED;

    /* mark goto targets */
    for (int i=0; i < len;) {
        decoded = decode_at_address(prog, i);
        i += decoded.length;
        switch(decoded.opcode) {
        case Instr_JE:
        case Instr_JNE:
            jit->labels[i + decoded.immediate].inputs++;
            jit->labels[i].inputs++;
            break;
        case Instr_Jump:
            jit->labels[i + decoded.immediate].inputs++;
            break;
        case Instr_Break:
            i = len;
            break;
        }
    }

#ifdef JIT_RESOLVE_STACK
    jit->sp = -1;
    jit->bb_sp = malloc(len * sizeof(int));
    memset(jit->bb_sp, -1, len * sizeof(int));

# if defined(JIT_USE_VARS) || defined(JIT_USE_SSA)
    /* calculate stack_limit */
    jit->stack_limit = 0;
    decoded.opcode = Instr_Nop;

    for (int i=0; i < len;) {
        if (jit->labels[i].inputs > 0) {
            if (decoded.opcode != Instr_Jump) {
                assert(jit->bb_sp[i] == -1 || jit->bb_sp[i] == jit->sp);
                jit->bb_sp[i] = jit->sp;
            }
            assert(jit->bb_sp[i] != -1);
            jit->sp == jit->bb_sp[i];
        }

        decoded = decode_at_address(prog, i);
        i += decoded.length;

        switch(decoded.opcode) {
        case Instr_Nop:
        case Instr_Halt:
        case Instr_Swap:
        case Instr_Inc:
        case Instr_Dec:
        case Instr_Rot:
        case Instr_SQRT:
        case Instr_Pick:
            /* Do nothing */
            break;
        case Instr_Push:
        case Instr_Dup:
        case Instr_Over:
        case Instr_Rand:
            jit->sp++;
            if (jit->sp >= jit->stack_limit) {
                jit->stack_limit = jit->sp + 1;
            }
            break;
        case Instr_Print:
        case Instr_Add:
        case Instr_Sub:
        case Instr_Mod:
        case Instr_Mul:
        case Instr_Drop:
        case Instr_And:
        case Instr_Or:
        case Instr_Xor:
        case Instr_SHL:
        case Instr_SHR:
            jit->sp--;
            break;
        case Instr_JE:
        case Instr_JNE:
            jit->sp--;
            assert(jit->bb_sp[i + decoded.immediate] == -1 || jit->bb_sp[i + decoded.immediate] == jit->sp);
            jit->bb_sp[i + decoded.immediate] = jit->sp;
            break;
        case Instr_Jump:
            assert(jit->bb_sp[i + decoded.immediate] == -1 || jit->bb_sp[i + decoded.immediate] == jit->sp);
            jit->bb_sp[i + decoded.immediate] = jit->sp;
            break;
        case Instr_Break:
            i = len;
            break;
        default:
            assert(0 && "Unsupported instruction");
            break;
        }
    }

    jit->sp = -1;
    jit->vars = malloc(jit->stack_limit * sizeof(ir_ref));
    for (int i = 0; i < jit->stack_limit; i++) {
        char s[16];
        sprintf(s, "t%d", i);
        jit->vars[i] = ir_var(_ir_CTX, IR_U32, 1, s);
    }
# endif
#endif

    ir_ref printf_func =
        ir_const_func(_ir_CTX, ir_str(_ir_CTX, "printf"), ir_proto_1(_ir_CTX, IR_I32, IR_VARARG_FUNC, IR_ADDR));
    ir_ref rand_func =
        ir_const_func(_ir_CTX, ir_str(_ir_CTX, "rand"), ir_proto_0(_ir_CTX, IR_I32, 0));
    ir_ref sqrt_func =
        ir_const_func(_ir_CTX, ir_str(_ir_CTX, "sqrt"), ir_proto_1(_ir_CTX, IR_DOUBLE, IR_BUILTIN_FUNC, IR_DOUBLE));

    decoded.opcode = Instr_Nop;

    for (int i=0; i < len;) {
        if (jit->labels[i].inputs > 0) {
            if (decoded.opcode != Instr_Jump) {
                if (decoded.opcode != Instr_JE
                 && decoded.opcode != Instr_JNE) {
                    jit->labels[i].inputs++;
                }
                jit_goto_forward(jit, i);
            }
            assert(!jit->ctx.control);
            if (jit->labels[i].inputs == 1) {
                tmp3 = jit->labels[i].merge;
                assert(tmp3);
                ir_insn *insn = &jit->ctx.ir_base[tmp3];
                assert(insn->op == IR_END && !insn->op2);
                insn->op2 = IR_UNUSED;
                ir_BEGIN(tmp3);
                jit->labels[i].merge = IR_UNUSED;
            } else {
                tmp1 = ir_emit_N(_ir_CTX, IR_MERGE, jit->labels[i].inputs);
                tmp2 = 0;
                tmp3 = jit->labels[i].merge;
                jit->labels[i].merge = jit->ctx.control = tmp1;

                while (tmp3) {
                    /* Store forward GOTOs into MERGE */
                    tmp2++;
                    assert(tmp2 <= jit->labels[i].inputs);
                    ir_set_op(_ir_CTX, tmp1, tmp2, tmp3);
                    ir_insn *insn = &jit->ctx.ir_base[tmp3];
                    assert(insn->op == IR_END);
                    tmp3 = insn->op2;
                    insn->op2 = IR_UNUSED;
                }
                jit->labels[i].inputs = tmp2;
            }

#ifdef JIT_RESOLVE_STACK
            assert(jit->bb_sp[i] != -1);
            jit->sp == jit->bb_sp[i];
#endif
        }

        decoded = decode_at_address(prog, i);
        i += decoded.length;

        switch(decoded.opcode) {
        case Instr_Nop:
            /* Do nothing */
            break;
        case Instr_Halt:
            // JIT: cpu.state = Cpu_Halted;
            ir_STORE(ir_ADD_OFFSET(jit->cpu, offsetof(cpu_t, state)), ir_CONST_I32(Cpu_Halted));
            ir_RETURN(IR_VOID);
            break;
        case Instr_Push:
            jit_push(jit, ir_CONST_U32(decoded.immediate));
            break;
        case Instr_Print:
            tmp1 = jit_pop(jit);
            ir_CALL_2(IR_VOID, printf_func, ir_CONST_STR("[%d]\n"), tmp1);
            break;
        case Instr_Swap:
            tmp1 = jit_pop(jit);
            tmp2 = jit_pop(jit);
            jit_push(jit, tmp1);
            jit_push(jit, tmp2);
            break;
        case Instr_Dup:
            tmp1 = jit_pop(jit);
            jit_push(jit, tmp1);
            jit_push(jit, tmp1);
            break;
        case Instr_Over:
            tmp1 = jit_pop(jit);
            tmp2 = jit_pop(jit);
            jit_push(jit, tmp2);
            jit_push(jit, tmp1);
            jit_push(jit, tmp2);
            break;
        case Instr_Inc:
            tmp1 = jit_pop(jit);
            jit_push(jit, ir_ADD_U32(tmp1, ir_CONST_U32(1)));
            break;
        case Instr_Add:
            tmp1 = jit_pop(jit);
            tmp2 = jit_pop(jit);
            jit_push(jit, ir_ADD_U32(tmp1, tmp2));
            break;
        case Instr_Sub:
            tmp1 = jit_pop(jit);
            tmp2 = jit_pop(jit);
            jit_push(jit, ir_SUB_U32(tmp1, tmp2));
            break;
        case Instr_Mod:
            tmp1 = jit_pop(jit);
            tmp2 = jit_pop(jit);
            // JIT if (tmp2 == 0)
            tmp3 = ir_IF(ir_EQ(tmp2, ir_CONST_U32(0)));
            ir_IF_TRUE_cold(tmp3);
            // JIT: printf("Division by zero\n");
            ir_CALL_1(IR_VOID, printf_func, ir_CONST_STR("Division by zero\n"));
            // JIT: pcpu->state = Cpu_Break;
            ir_STORE(ir_ADD_OFFSET(jit->cpu, offsetof(cpu_t, state)), ir_CONST_I32(Cpu_Break));
            ir_RETURN(IR_VOID);

            ir_IF_FALSE(tmp3);
            jit_push(jit, ir_MOD_U32(tmp1, tmp2));
            break;
        case Instr_Mul:
            tmp1 = jit_pop(jit);
            tmp2 = jit_pop(jit);
            jit_push(jit, ir_MUL_U32(tmp1, tmp2));
            break;
        case Instr_Rand:
            tmp1 = ir_CALL(IR_I32, rand_func);
            jit_push(jit, tmp1);
            break;
        case Instr_Dec:
            tmp1 = jit_pop(jit);
            jit_push(jit, ir_SUB_U32(tmp1, ir_CONST_U32(1)));
            break;
        case Instr_Drop:
            (void)jit_pop(jit);
            break;
        case Instr_JE:
            tmp1 = jit_pop(jit);
            // JIT: if (tmp1 == 0)
            tmp3 = ir_IF(ir_EQ(tmp1, ir_CONST_U32(0)));
            ir_IF_TRUE(tmp3);
            if (decoded.immediate >= 0) {
                jit_goto_forward(jit, i + decoded.immediate);
            } else {
                jit_goto_backward(jit, i + decoded.immediate);
            }
            ir_IF_FALSE(tmp3);
            break;
        case Instr_JNE:
            tmp1 = jit_pop(jit);
            // JIT: if (tmp1 == 0)
            tmp3 = ir_IF(ir_NE(tmp1, ir_CONST_U32(0)));
            ir_IF_TRUE(tmp3);
            if (decoded.immediate >= 0) {
                jit_goto_forward(jit, i + decoded.immediate);
            } else {
                jit_goto_backward(jit, i + decoded.immediate);
            }
            ir_IF_FALSE(tmp3);
            break;
        case Instr_Jump:
            if (decoded.immediate >= 0) {
                jit_goto_forward(jit, i + decoded.immediate);
            } else {
                jit_goto_backward(jit, i + decoded.immediate);
            }
            break;
        case Instr_And:
            tmp1 = jit_pop(jit);
            tmp2 = jit_pop(jit);
            jit_push(jit, ir_AND_U32(tmp1, tmp2));
            break;
        case Instr_Or:
            tmp1 = jit_pop(jit);
            tmp2 = jit_pop(jit);
            jit_push(jit, ir_OR_U32(tmp1, tmp2));
            break;
        case Instr_Xor:
            tmp1 = jit_pop(jit);
            tmp2 = jit_pop(jit);
            jit_push(jit, ir_XOR_U32(tmp1, tmp2));
            break;
        case Instr_SHL:
            tmp1 = jit_pop(jit);
            tmp2 = jit_pop(jit);
            jit_push(jit, ir_SHL_U32(tmp1, tmp2));
            break;
        case Instr_SHR:
            tmp1 = jit_pop(jit);
            tmp2 = jit_pop(jit);
            jit_push(jit, ir_SHR_U32(tmp1, tmp2));
            break;
        case Instr_Rot:
            tmp1 = jit_pop(jit);
            tmp2 = jit_pop(jit);
            tmp3 = jit_pop(jit);
            jit_push(jit, tmp1);
            jit_push(jit, tmp3);
            jit_push(jit, tmp2);
            break;
        case Instr_SQRT:
            tmp1 = jit_pop(jit);
            tmp1 = ir_FP2U32(ir_CALL_1(IR_DOUBLE, sqrt_func, ir_INT2D(tmp1)));
            jit_push(jit, tmp1);
            break;
        case Instr_Pick:
            tmp1 = jit_pop(jit);
            tmp1 = jit_pick(jit, tmp1);
            jit_push(jit, tmp1);
            break;
        case Instr_Break:
            if (jit->ctx.control) {
                // JIT: pcpu->state = Cpu_Break;
                ir_STORE(ir_ADD_OFFSET(jit->cpu, offsetof(cpu_t, state)), ir_CONST_I32(Cpu_Break));
                ir_RETURN(IR_VOID);
            }
            i = len;
            break;
        default:
            assert(0 && "Unsupported instruction");
            break;
        }
    }

    if (jit->stack_overflow) {
        ir_MERGE_list(jit->stack_overflow);
        // JIT: printf("Stack overflow\n");
        ir_CALL_1(IR_VOID, printf_func, ir_CONST_STR("Stack overflow\n"));
        // JIT: pcpu->state = Cpu_Break;
        ir_STORE(ir_ADD_OFFSET(jit->cpu, offsetof(cpu_t, state)), ir_CONST_I32(Cpu_Break));
        ir_RETURN(IR_VOID);
    }
    if (jit->stack_underflow) {
        ir_MERGE_list(jit->stack_underflow);
        // JIT: printf("Stack overflow\n");
        ir_CALL_1(IR_VOID, printf_func, ir_CONST_STR("Stack underflow\n"));
        // JIT: pcpu->state = Cpu_Break;
        ir_STORE(ir_ADD_OFFSET(jit->cpu, offsetof(cpu_t, state)), ir_CONST_I32(Cpu_Break));
        ir_RETURN(IR_VOID);
    }
    if (jit->stack_bound) {
        ir_MERGE_list(jit->stack_bound);
        // JIT: printf("Out of bound picking\n");
        ir_CALL_1(IR_VOID, printf_func, ir_CONST_STR("Stack underflow\n"));
        // JIT: pcpu->state = Cpu_Break;
        ir_STORE(ir_ADD_OFFSET(jit->cpu, offsetof(cpu_t, state)), ir_CONST_I32(Cpu_Break));
        ir_RETURN(IR_VOID);
    }

#ifdef JIT_RESOLVE_STACK
# if defined(JIT_USE_VARS) || defined(JIT_USE_SSA)
    free(jit->vars);
# endif
    free(jit->bb_sp);
#endif
    free(jit->labels);
}

int main(int argc, char **argv) {
    uint64_t steplimit = parse_args(argc, argv);
    cpu_t cpu = init_cpu();
    jit_ctx jit;
    typedef void (*entry_t)(cpu_t*);
    entry_t entry;
    size_t size;
    uint32_t flags = IR_FUNCTION | IR_OPT_FOLDING | IR_OPT_CFG | IR_OPT_CODEGEN;

#if defined(JIT_RESOLVE_STACK) &&  defined(JIT_USE_SSA)
	flags |= IR_OPT_MEM2SSA;
#endif
    ir_init(&jit.ctx, flags, 256, 1024);

    jit_program(&jit, cpu.pmem, PROGRAM_SIZE);

    if (debug) {
        ir_save(&jit.ctx, IR_SAVE_CFG | IR_SAVE_RULES | IR_SAVE_REGS, stderr);
    }

    entry = (entry_t)ir_jit_compile(&jit.ctx, 2, &size);
    if (!entry) {
        printf("Compilation failure\n");
        exit(-1);
    }

    if (debug) {
        ir_save(&jit.ctx, IR_SAVE_CFG | IR_SAVE_RULES | IR_SAVE_REGS, stderr);
        ir_disasm("prog", entry, size, 0, &jit.ctx, stderr);
    }

    entry(&cpu);

    ir_free(&jit.ctx);

    assert(cpu.state != Cpu_Running || cpu.steps == steplimit);

    /* Print CPU state */
    printf("CPU executed %ld steps. End state \"%s\".\n",
            cpu.steps, cpu.state == Cpu_Halted? "Halted":
                       cpu.state == Cpu_Running? "Running": "Break");
    printf("PC = %#x, SP = %d\n", cpu.pc, cpu.sp);
    printf("Stack: ");
    for (int32_t i=cpu.sp; i >= 0 ; i--) {
        printf("%#10x ", cpu.stack[i]);
    }
    printf("%s\n", cpu.sp == -1? "(empty)": "");

    free(LoadedProgram);

    return cpu.state == Cpu_Halted ||
           (cpu.state == Cpu_Running &&
            cpu.steps == steplimit)?0:1;
}
