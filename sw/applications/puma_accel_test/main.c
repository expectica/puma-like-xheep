#include <stdint.h>
#include <stdio.h>

#include "puma_accel.h"

/*
 * X-HEEP + PUMA-like accelerator + PISLib
 *
 * PHASE 1 : directed ISA regression
 * PHASE 2 : directed PISLib WSTORE/MVM regression
 * PHASE 3A: deterministic random ALU/dataflow regression vs software golden
 * PHASE 3B: deterministic random memory/control regression vs software golden
 * PHASE 3C: deterministic random PISLib MVM regression vs software golden
 *
 * The RTL is not modified by this test.
 */

#define PUMA_BASE_ADDR ((uintptr_t)0x20077000u)

#define PUMA_IMEM_DEPTH 32u
#define PUMA_REG_COUNT   8u
#define PUMA_LANES       4u
#define PUMA_MEM_DEPTH 256u
#define PUMA_APIM_DEPTH 1024u

#define OP_NOP     0x0u
#define OP_SET     0x1u
#define OP_VADD    0x2u
#define OP_VMUL    0x3u
#define OP_RELU    0x4u
#define OP_COPY    0x5u
#define OP_MVM     0x6u
#define OP_JMP     0x7u
#define OP_BRNZ    0x8u
#define OP_LOAD    0x9u
#define OP_STORE   0xAu
#define OP_WSTORE  0xBu
#define OP_HALT    0xFu

/*
 * First run: keep this at 0.
 * After the smoke regression passes, change only this line to 1
 * for the longer final regression.
 */
#define LONG_REGRESSION 1

#if LONG_REGRESSION
#define NUM_RANDOM_ALU_CASES      100u
#define NUM_RANDOM_MEMCTRL_CASES  100u
#define NUM_RANDOM_MVM_CASES       20u
#else
#define NUM_RANDOM_ALU_CASES       10u
#define NUM_RANDOM_MEMCTRL_CASES   10u
#define NUM_RANDOM_MVM_CASES        5u
#endif

#define SEED_ALU      0x5A17C0DEu
#define SEED_MEMCTRL  0xC001D00Du
#define SEED_MVM      0x13579BDFu

#define RUN_TIMEOUT 4000000u
#define GOLDEN_MAX_STEPS 256u

typedef struct {
    uint16_t vreg[PUMA_REG_COUNT][PUMA_LANES];
    uint16_t mem[PUMA_MEM_DEPTH];
    uint8_t  apim[PUMA_APIM_DEPTH];
} golden_t;

static golden_t golden;
static uint32_t rng_state;


/* -------------------------------------------------------------------------- */
/* MMIO helpers                                                                */
/* -------------------------------------------------------------------------- */

static inline void puma_write(uint32_t offset, uint32_t value)
{
    volatile uint32_t *addr =
        (volatile uint32_t *)(PUMA_BASE_ADDR + (uintptr_t)offset);
    *addr = value;
}

static inline uint32_t puma_read(uint32_t offset)
{
    volatile uint32_t *addr =
        (volatile uint32_t *)(PUMA_BASE_ADDR + (uintptr_t)offset);
    return *addr;
}

static uint32_t puma_enc(
    uint32_t opcode,
    uint32_t rd,
    uint32_t rs1,
    uint32_t rs2,
    uint32_t imm)
{
    return ((opcode & 0xFu) << 28) |
           ((rd     & 0xFu) << 24) |
           ((rs1    & 0xFu) << 20) |
           ((rs2    & 0xFu) << 16) |
           ( imm     & 0xFFFFu);
}

static void puma_program_instruction(uint32_t addr, uint32_t instruction)
{
    puma_write(PUMA_ACCEL_IMEM_ADDR_REG_OFFSET, addr);
    puma_write(PUMA_ACCEL_IMEM_DATA_REG_OFFSET, instruction);

    /* puma_accel.sv detects the rising edge of WE. */
    puma_write(PUMA_ACCEL_IMEM_WRITE_REG_OFFSET, 0u);
    puma_write(PUMA_ACCEL_IMEM_WRITE_REG_OFFSET, 1u);
    puma_write(PUMA_ACCEL_IMEM_WRITE_REG_OFFSET, 0u);
}

static void puma_start(void)
{
    puma_write(PUMA_ACCEL_CTRL_REG_OFFSET, 0u);
    puma_write(PUMA_ACCEL_CTRL_REG_OFFSET, 1u);
    puma_write(PUMA_ACCEL_CTRL_REG_OFFSET, 0u);
}

static uint16_t puma_read_vreg_raw(uint32_t reg, uint32_t lane)
{
    uint32_t selector =
        ((lane & 0x3u) << 8) |
        (reg & 0x7u);

    puma_write(PUMA_ACCEL_VREG_SEL_REG_OFFSET, selector);

    return (uint16_t)
        (puma_read(PUMA_ACCEL_VREG_DATA_REG_OFFSET) & 0xFFFFu);
}

static int puma_program_and_run(
    const char *name,
    const uint32_t *program,
    uint32_t words,
    int verbose_program)
{
    uint32_t i;
    uint32_t status;
    uint32_t timeout = RUN_TIMEOUT;

    if (words == 0u || words > PUMA_IMEM_DEPTH) {
        printf("FAIL: %s has %lu words (IMEM depth = %lu)\n",
               name,
               (unsigned long)words,
               (unsigned long)PUMA_IMEM_DEPTH);
        return 0;
    }

    printf("\nProgramming %s: %lu instructions\n",
           name, (unsigned long)words);

    for (i = 0u; i < words; ++i) {
        puma_program_instruction(i, program[i]);

        if (verbose_program) {
            printf("  IMEM[%2lu] = 0x%08lx\n",
                   (unsigned long)i,
                   (unsigned long)program[i]);
        }
    }

    printf("Starting %s...\n", name);
    puma_start();

    /*
     * A few MMIO reads give the core time to leave the previous HALT state.
     * We intentionally do not require observing DONE=0, because a very short
     * program could theoretically finish before software observes that edge.
     */
    for (i = 0u; i < 4u; ++i) {
        status = puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);
        (void)status;
    }

    do {
        status = puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        if ((status & 0x1u) != 0u) {
            printf("%s DONE.\n", name);
            return 1;
        }

        --timeout;
    } while (timeout != 0u);

    printf("FAIL: timeout waiting for %s DONE\n", name);
    printf("  STATUS = 0x%08lx\n",
           (unsigned long)puma_read(PUMA_ACCEL_STATUS_REG_OFFSET));
    printf("  PC     = 0x%08lx\n",
           (unsigned long)puma_read(PUMA_ACCEL_PC_REG_OFFSET));
    printf("  IR     = 0x%08lx\n",
           (unsigned long)puma_read(PUMA_ACCEL_IR_REG_OFFSET));

    return 0;
}


/* -------------------------------------------------------------------------- */
/* Small display/check helpers                                                 */
/* -------------------------------------------------------------------------- */

static int signed16_for_print(uint16_t x)
{
    if ((x & 0x8000u) != 0u) {
        return (int)((int32_t)x - 65536);
    }
    return (int)x;
}

static uint16_t raw16_from_i32(int32_t x)
{
    return (uint16_t)((uint32_t)x & 0xFFFFu);
}

static int check_vec(
    const char *label,
    uint32_t reg,
    const uint16_t expected[PUMA_LANES],
    int print_pass)
{
    uint16_t got[PUMA_LANES];
    uint32_t lane;
    int pass = 1;

    for (lane = 0u; lane < PUMA_LANES; ++lane) {
        got[lane] = puma_read_vreg_raw(reg, lane);
        if (got[lane] != expected[lane]) {
            pass = 0;
        }
    }

    if (print_pass || !pass) {
        printf("%s: v%lu = [%d, %d, %d, %d], "
               "expected [%d, %d, %d, %d] -> %s\n",
               label,
               (unsigned long)reg,
               signed16_for_print(got[0]),
               signed16_for_print(got[1]),
               signed16_for_print(got[2]),
               signed16_for_print(got[3]),
               signed16_for_print(expected[0]),
               signed16_for_print(expected[1]),
               signed16_for_print(expected[2]),
               signed16_for_print(expected[3]),
               pass ? "PASS" : "FAIL");
    }

    return pass;
}

static void dump_program(const uint32_t *program, uint32_t words)
{
    uint32_t i;

    printf("PROGRAM DUMP (%lu words):\n", (unsigned long)words);
    for (i = 0u; i < words; ++i) {
        printf("  %02lu: 0x%08lx\n",
               (unsigned long)i,
               (unsigned long)program[i]);
    }
}

static void dump_reg_mismatch(
    uint32_t reg,
    const golden_t *g)
{
    uint32_t lane;

    printf("  v%lu RTL    = [", (unsigned long)reg);
    for (lane = 0u; lane < PUMA_LANES; ++lane) {
        uint16_t got = puma_read_vreg_raw(reg, lane);
        printf("%d%s",
               signed16_for_print(got),
               (lane + 1u == PUMA_LANES) ? "" : ", ");
    }
    printf("]\n");

    printf("  v%lu GOLDEN = [", (unsigned long)reg);
    for (lane = 0u; lane < PUMA_LANES; ++lane) {
        printf("%d%s",
               signed16_for_print(g->vreg[reg][lane]),
               (lane + 1u == PUMA_LANES) ? "" : ", ");
    }
    printf("]\n");
}

static int compare_regs_to_golden(
    const golden_t *g,
    uint32_t reg_mask,
    uint32_t *bad_reg,
    uint32_t *bad_lane)
{
    uint32_t reg;
    uint32_t lane;

    for (reg = 0u; reg < PUMA_REG_COUNT; ++reg) {
        if ((reg_mask & (1u << reg)) == 0u) {
            continue;
        }

        for (lane = 0u; lane < PUMA_LANES; ++lane) {
            uint16_t got = puma_read_vreg_raw(reg, lane);

            if (got != g->vreg[reg][lane]) {
                if (bad_reg != (uint32_t *)0) {
                    *bad_reg = reg;
                }
                if (bad_lane != (uint32_t *)0) {
                    *bad_lane = lane;
                }
                return 0;
            }
        }
    }

    return 1;
}


/* -------------------------------------------------------------------------- */
/* Deterministic RNG                                                           */
/* -------------------------------------------------------------------------- */

static uint32_t rng_next(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static uint32_t rng_range(uint32_t n)
{
    return (n == 0u) ? 0u : (rng_next() % n);
}

static uint16_t rng_small_signed_raw(uint32_t magnitude)
{
    uint32_t span = 2u * magnitude + 1u;
    int32_t value =
        (int32_t)rng_range(span) - (int32_t)magnitude;

    return raw16_from_i32(value);
}


/* -------------------------------------------------------------------------- */
/* Software golden interpreter                                                */
/* -------------------------------------------------------------------------- */

static void golden_reset(golden_t *g)
{
    uint32_t r;
    uint32_t lane;
    uint32_t i;

    for (r = 0u; r < PUMA_REG_COUNT; ++r) {
        for (lane = 0u; lane < PUMA_LANES; ++lane) {
            g->vreg[r][lane] = 0u;
        }
    }

    for (i = 0u; i < PUMA_MEM_DEPTH; ++i) {
        g->mem[i] = 0u;
    }

    for (i = 0u; i < PUMA_APIM_DEPTH; ++i) {
        g->apim[i] = 0u;
    }
}

static int golden_execute(
    const uint32_t *program,
    uint32_t words,
    golden_t *g)
{
    uint32_t pc = 0u;
    uint32_t steps = 0u;

    while (steps++ < GOLDEN_MAX_STEPS) {
        uint32_t ir;
        uint32_t op;
        uint32_t rd;
        uint32_t rs1;
        uint32_t rs2;
        uint32_t imm;
        uint32_t lane;

        if (pc >= words) {
            return -1;
        }

        ir = program[pc++];
        op  = (ir >> 28) & 0xFu;
        rd  = (ir >> 24) & 0xFu;
        rs1 = (ir >> 20) & 0xFu;
        rs2 = (ir >> 16) & 0xFu;
        imm = ir & 0xFFFFu;

        switch (op) {
        case OP_NOP:
            break;

        case OP_SET:
            if (rd < PUMA_REG_COUNT) {
                for (lane = 0u; lane < PUMA_LANES; ++lane) {
                    g->vreg[rd][lane] = (uint16_t)imm;
                }
            }
            break;

        case OP_VADD:
            if (rd < PUMA_REG_COUNT &&
                rs1 < PUMA_REG_COUNT &&
                rs2 < PUMA_REG_COUNT) {
                for (lane = 0u; lane < PUMA_LANES; ++lane) {
                    uint32_t sum =
                        (uint32_t)g->vreg[rs1][lane] +
                        (uint32_t)g->vreg[rs2][lane];
                    g->vreg[rd][lane] = (uint16_t)(sum & 0xFFFFu);
                }
            }
            break;

        case OP_VMUL:
            if (rd < PUMA_REG_COUNT &&
                rs1 < PUMA_REG_COUNT &&
                rs2 < PUMA_REG_COUNT) {
                for (lane = 0u; lane < PUMA_LANES; ++lane) {
                    uint32_t product =
                        (uint32_t)g->vreg[rs1][lane] *
                        (uint32_t)g->vreg[rs2][lane];
                    g->vreg[rd][lane] =
                        (uint16_t)(product & 0xFFFFu);
                }
            }
            break;

        case OP_RELU:
            if (rd < PUMA_REG_COUNT && rs1 < PUMA_REG_COUNT) {
                for (lane = 0u; lane < PUMA_LANES; ++lane) {
                    uint16_t x = g->vreg[rs1][lane];
                    g->vreg[rd][lane] =
                        ((x & 0x8000u) != 0u) ? 0u : x;
                }
            }
            break;

        case OP_COPY:
            if (rd < PUMA_REG_COUNT && rs1 < PUMA_REG_COUNT) {
                for (lane = 0u; lane < PUMA_LANES; ++lane) {
                    g->vreg[rd][lane] = g->vreg[rs1][lane];
                }
            }
            break;

        case OP_LOAD:
            if (rd < PUMA_REG_COUNT) {
                for (lane = 0u; lane < PUMA_LANES; ++lane) {
                    uint32_t addr = (imm + lane) & 0xFFu;
                    g->vreg[rd][lane] = g->mem[addr];
                }
            }
            break;

        case OP_STORE:
            if (rs1 < PUMA_REG_COUNT) {
                g->mem[imm & 0xFFu] = g->vreg[rs1][0];
            }
            break;

        case OP_JMP:
            pc = imm & (PUMA_IMEM_DEPTH - 1u);
            break;

        case OP_BRNZ:
            if (rs1 < PUMA_REG_COUNT &&
                g->vreg[rs1][0] != 0u) {
                pc = imm & (PUMA_IMEM_DEPTH - 1u);
            }
            break;

        case OP_WSTORE:
            if (rs1 < PUMA_REG_COUNT) {
                g->apim[imm & 0x3FFu] =
                    (uint8_t)(g->vreg[rs1][0] & 0xFFu);
            }
            break;

        case OP_MVM:
            if (rd < PUMA_REG_COUNT && rs1 < PUMA_REG_COUNT) {
                uint32_t out;

                for (out = 0u; out < PUMA_LANES; ++out) {
                    uint32_t in;
                    uint32_t sum = 0u;

                    for (in = 0u; in < PUMA_LANES; ++in) {
                        uint32_t addr = 256u * in + 4u * out;
                        uint32_t x =
                            (uint32_t)g->vreg[rs1][in] & 0xFu;
                        uint32_t w = (uint32_t)g->apim[addr];

                        sum += x * w;
                    }

                    g->vreg[rd][out] =
                        (uint16_t)(sum & 0x3FFFu);
                }
            }
            break;

        case OP_HALT:
            return 0;

        default:
            /* Unknown instruction behaves like NOP. */
            break;
        }
    }

    return -2;
}


/* -------------------------------------------------------------------------- */
/* PHASE 1: directed ISA regression                                            */
/* -------------------------------------------------------------------------- */

static int phase1_directed_isa(void)
{
    uint32_t program[] = {
        puma_enc(OP_SET,   0u, 0u, 0u,    2u),
        puma_enc(OP_SET,   1u, 0u, 0u,    3u),
        puma_enc(OP_VADD,  2u, 0u, 1u,    0u),
        puma_enc(OP_VMUL,  3u, 0u, 1u,    0u),
        puma_enc(OP_SET,   4u, 0u, 0u, 0xFFFFu),
        puma_enc(OP_RELU,  4u, 4u, 0u,    0u),
        puma_enc(OP_COPY,  5u, 2u, 0u,    0u),

        puma_enc(OP_STORE, 0u, 3u, 0u,   40u),
        puma_enc(OP_STORE, 0u, 2u, 0u,   41u),
        puma_enc(OP_STORE, 0u, 0u, 0u,   42u),
        puma_enc(OP_STORE, 0u, 1u, 0u,   43u),
        puma_enc(OP_LOAD,  6u, 0u, 0u,   40u),

        /* v4 == 0, so this BRNZ must NOT be taken. */
        puma_enc(OP_BRNZ,  0u, 4u, 0u,   14u),
        puma_enc(OP_SET,   7u, 0u, 0u,   11u),

        /* v6[0] == 6, so this BRNZ must be taken. */
        puma_enc(OP_BRNZ,  0u, 6u, 0u,   16u),
        puma_enc(OP_SET,   7u, 0u, 0u,   99u),

        /* JMP must skip the following SET. */
        puma_enc(OP_JMP,   0u, 0u, 0u,   18u),
        puma_enc(OP_SET,   7u, 0u, 0u,   88u),

        puma_enc(OP_NOP,   0u, 0u, 0u,    0u),
        puma_enc(OP_HALT,  0u, 0u, 0u,    0u)
    };

    const uint16_t e0[4] = {2u, 2u, 2u, 2u};
    const uint16_t e1[4] = {3u, 3u, 3u, 3u};
    const uint16_t e2[4] = {5u, 5u, 5u, 5u};
    const uint16_t e3[4] = {6u, 6u, 6u, 6u};
    const uint16_t e4[4] = {0u, 0u, 0u, 0u};
    const uint16_t e5[4] = {5u, 5u, 5u, 5u};
    const uint16_t e6[4] = {6u, 5u, 2u, 3u};
    const uint16_t e7[4] = {11u, 11u, 11u, 11u};

    uint32_t words =
        (uint32_t)(sizeof(program) / sizeof(program[0]));
    int pass = 1;

    printf("\n");
    printf("=====================================================\n");
    printf(" PHASE 1: DIRECTED ISA REGRESSION\n");
    printf("=====================================================\n");

    if (!puma_program_and_run("PHASE 1", program, words, 1)) {
        return 0;
    }

    printf("  STATUS = 0x%08lx\n",
           (unsigned long)puma_read(PUMA_ACCEL_STATUS_REG_OFFSET));
    printf("  PC     = 0x%08lx\n",
           (unsigned long)puma_read(PUMA_ACCEL_PC_REG_OFFSET));
    printf("  IR     = 0x%08lx\n",
           (unsigned long)puma_read(PUMA_ACCEL_IR_REG_OFFSET));

    printf("\n---------------- PHASE 1 CHECKS ----------------\n");

    pass &= check_vec("SET v0",       0u, e0, 1);
    pass &= check_vec("SET v1",       1u, e1, 1);
    pass &= check_vec("VADD",         2u, e2, 1);
    pass &= check_vec("VMUL",         3u, e3, 1);
    pass &= check_vec("RELU",         4u, e4, 1);
    pass &= check_vec("COPY",         5u, e5, 1);
    pass &= check_vec("STORE + LOAD", 6u, e6, 1);
    pass &= check_vec("BRNZ + JMP",   7u, e7, 1);

    if ((puma_read(PUMA_ACCEL_STATUS_REG_OFFSET) & 0x1u) == 0u) {
        pass = 0;
        printf("PHASE 1 HALT/status -> FAIL\n");
    } else {
        printf("PHASE 1 HALT/status -> PASS\n");
    }

    printf("\nPHASE 1 instruction summary:\n");
    printf("  SET       %s\n", pass ? "PASS" : "CHECK ABOVE");
    printf("  VADD      %s\n", pass ? "PASS" : "CHECK ABOVE");
    printf("  VMUL      %s\n", pass ? "PASS" : "CHECK ABOVE");
    printf("  RELU      %s\n", pass ? "PASS" : "CHECK ABOVE");
    printf("  COPY      %s\n", pass ? "PASS" : "CHECK ABOVE");
    printf("  STORE     %s\n", pass ? "PASS" : "CHECK ABOVE");
    printf("  LOAD      %s\n", pass ? "PASS" : "CHECK ABOVE");
    printf("  BRNZ      %s\n", pass ? "PASS" : "CHECK ABOVE");
    printf("  JMP       %s\n", pass ? "PASS" : "CHECK ABOVE");
    printf("  HALT      %s\n", pass ? "PASS" : "CHECK ABOVE");

    return pass;
}


/* -------------------------------------------------------------------------- */
/* Common MVM program builder                                                  */
/* -------------------------------------------------------------------------- */

static uint32_t build_mvm_program(
    uint32_t program[PUMA_IMEM_DEPTH],
    const uint8_t weight[4][4],
    const uint8_t input[4])
{
    uint32_t n = 0u;
    uint32_t w;
    uint32_t out;
    uint32_t in;

    /*
     * Group WSTOREs by weight value. Random PHASE 3C uses weights 1..4,
     * so this always costs exactly 4 SET + 16 WSTORE instructions.
     */
    for (w = 1u; w <= 4u; ++w) {
        program[n++] = puma_enc(OP_SET, 0u, 0u, 0u, w);

        for (out = 0u; out < 4u; ++out) {
            for (in = 0u; in < 4u; ++in) {
                if ((uint32_t)weight[out][in] == w) {
                    uint32_t addr = 256u * in + 4u * out;
                    program[n++] =
                        puma_enc(OP_WSTORE, 0u, 0u, 0u, addr);
                }
            }
        }
    }

    /* Build the non-uniform input vector through STORE + LOAD. */
    for (in = 0u; in < 4u; ++in) {
        program[n++] =
            puma_enc(OP_SET, 0u, 0u, 0u, (uint32_t)input[in]);
        program[n++] =
            puma_enc(OP_STORE, 0u, 0u, 0u, 32u + in);
    }

    program[n++] = puma_enc(OP_LOAD, 1u, 0u, 0u, 32u);
    program[n++] = puma_enc(OP_MVM,  2u, 1u, 0u,  0u);
    program[n++] = puma_enc(OP_HALT, 0u, 0u, 0u,  0u);

    return n;
}


/* -------------------------------------------------------------------------- */
/* PHASE 2: directed PISLib WSTORE/MVM regression                              */
/* -------------------------------------------------------------------------- */

static int phase2_directed_pislib(void)
{
    static const uint8_t weight[4][4] = {
        {4u, 3u, 2u, 1u},
        {2u, 4u, 3u, 1u},
        {1u, 3u, 4u, 2u},
        {1u, 2u, 3u, 4u}
    };

    static const uint8_t input[4] = {1u, 2u, 3u, 4u};

    const uint16_t expected_input[4] = {1u, 2u, 3u, 4u};
    const uint16_t expected_mvm[4]   = {20u, 23u, 27u, 30u};

    uint32_t program[PUMA_IMEM_DEPTH];
    uint32_t words = build_mvm_program(program, weight, input);
    int pass = 1;

    printf("\n");
    printf("=====================================================\n");
    printf(" PHASE 2: PISLib WSTORE/MVM REGRESSION\n");
    printf("=====================================================\n");

    if (!puma_program_and_run("PHASE 2", program, words, 1)) {
        return 0;
    }

    printf("  STATUS = 0x%08lx\n",
           (unsigned long)puma_read(PUMA_ACCEL_STATUS_REG_OFFSET));
    printf("  PC     = 0x%08lx\n",
           (unsigned long)puma_read(PUMA_ACCEL_PC_REG_OFFSET));
    printf("  IR     = 0x%08lx\n",
           (unsigned long)puma_read(PUMA_ACCEL_IR_REG_OFFSET));

    printf("\n---------------- PHASE 2 CHECKS ----------------\n");

    pass &= check_vec("PISLib input / LOAD", 1u, expected_input, 1);
    pass &= check_vec("PISLib MVM output",   2u, expected_mvm,   1);

    if ((puma_read(PUMA_ACCEL_STATUS_REG_OFFSET) & 0x1u) == 0u) {
        pass = 0;
        printf("PHASE 2 HALT/status -> FAIL\n");
    } else {
        printf("PHASE 2 HALT/status -> PASS\n");
    }

    printf("\nPHASE 2 instruction summary:\n");
    printf("  WSTORE    %s\n", pass ? "PASS" : "CHECK ABOVE");
    printf("  MVM       %s\n", pass ? "PASS" : "CHECK ABOVE");
    printf("  STORE     %s\n", pass ? "PASS" : "CHECK ABOVE");
    printf("  LOAD      %s\n", pass ? "PASS" : "CHECK ABOVE");
    printf("  HALT      %s\n", pass ? "PASS" : "CHECK ABOVE");

    return pass;
}


/* -------------------------------------------------------------------------- */
/* PHASE 3A: random ALU/dataflow vs golden                                     */
/* -------------------------------------------------------------------------- */

static uint32_t build_random_alu_program(
    uint32_t program[PUMA_IMEM_DEPTH])
{
    uint32_t n = 0u;
    uint32_t r;
    uint32_t k;

    /* Initialize all 8 vector registers every case. */
    for (r = 0u; r < PUMA_REG_COUNT; ++r) {
        uint16_t imm = rng_small_signed_raw(7u);
        program[n++] = puma_enc(OP_SET, r, 0u, 0u, imm);
    }

    /* 20 random ALU/dataflow operations. */
    for (k = 0u; k < 20u; ++k) {
        uint32_t choice = rng_range(6u);
        uint32_t rd  = rng_range(PUMA_REG_COUNT);
        uint32_t rs1 = rng_range(PUMA_REG_COUNT);
        uint32_t rs2 = rng_range(PUMA_REG_COUNT);

        switch (choice) {
        case 0u:
            program[n++] =
                puma_enc(OP_SET, rd, 0u, 0u, rng_small_signed_raw(20u));
            break;
        case 1u:
            program[n++] = puma_enc(OP_VADD, rd, rs1, rs2, 0u);
            break;
        case 2u:
            program[n++] = puma_enc(OP_VMUL, rd, rs1, rs2, 0u);
            break;
        case 3u:
            program[n++] = puma_enc(OP_RELU, rd, rs1, 0u, 0u);
            break;
        case 4u:
            program[n++] = puma_enc(OP_COPY, rd, rs1, 0u, 0u);
            break;
        default:
            program[n++] = puma_enc(OP_NOP, 0u, 0u, 0u, 0u);
            break;
        }
    }

    program[n++] = puma_enc(OP_HALT, 0u, 0u, 0u, 0u);
    return n;
}

static int phase3a_random_alu(void)
{
    uint32_t case_idx;
    uint32_t program[PUMA_IMEM_DEPTH];

    rng_state = SEED_ALU;

    printf("\n");
    printf("=====================================================\n");
    printf(" PHASE 3A: GOLDEN RANDOM ALU/DATAFLOW REGRESSION\n");
    printf("=====================================================\n");
    printf("seed = 0x%08lx, cases = %lu\n",
           (unsigned long)SEED_ALU,
           (unsigned long)NUM_RANDOM_ALU_CASES);

    for (case_idx = 0u; case_idx < NUM_RANDOM_ALU_CASES; ++case_idx) {
        uint32_t case_seed = rng_state;
        uint32_t words;
        uint32_t bad_reg = 0u;
        uint32_t bad_lane = 0u;

        words = build_random_alu_program(program);

        golden_reset(&golden);
        if (golden_execute(program, words, &golden) != 0) {
            printf("FAIL: PHASE 3A golden interpreter error at case %lu\n",
                   (unsigned long)case_idx);
            dump_program(program, words);
            return 0;
        }

        if (!puma_program_and_run("PHASE 3A case", program, words, 0)) {
            printf("FAIL case = %lu, case_seed = 0x%08lx\n",
                   (unsigned long)case_idx,
                   (unsigned long)case_seed);
            dump_program(program, words);
            return 0;
        }

        if (!compare_regs_to_golden(
                &golden, 0xFFu, &bad_reg, &bad_lane)) {
            printf("FAIL: PHASE 3A case %lu\n",
                   (unsigned long)case_idx);
            printf("  phase seed = 0x%08lx\n",
                   (unsigned long)SEED_ALU);
            printf("  case seed  = 0x%08lx\n",
                   (unsigned long)case_seed);
            printf("  mismatch   = v%lu lane%lu\n",
                   (unsigned long)bad_reg,
                   (unsigned long)bad_lane);
            dump_reg_mismatch(bad_reg, &golden);
            dump_program(program, words);
            return 0;
        }

        if (((case_idx + 1u) % 10u) == 0u ||
            (case_idx + 1u) == NUM_RANDOM_ALU_CASES) {
            printf("  PHASE 3A: %lu/%lu cases PASS\n",
                   (unsigned long)(case_idx + 1u),
                   (unsigned long)NUM_RANDOM_ALU_CASES);
        }
    }

    printf("PHASE 3A PASS\n");
    return 1;
}


/* -------------------------------------------------------------------------- */
/* PHASE 3B: random memory/control vs golden                                   */
/* -------------------------------------------------------------------------- */

static uint32_t build_random_memctrl_program(
    uint32_t program[PUMA_IMEM_DEPTH])
{
    uint32_t n = 0u;
    uint32_t r;
    uint32_t base = 64u + rng_range(160u); /* base <= 223 */
    uint16_t a = rng_small_signed_raw(20u);
    uint16_t b = rng_small_signed_raw(20u);
    uint16_t c = rng_small_signed_raw(20u);
    uint16_t d = rng_small_signed_raw(20u);
    uint16_t value_a = rng_small_signed_raw(20u);
    uint16_t value_b = rng_small_signed_raw(20u);
    uint32_t cond = rng_range(2u);

    /* Define all registers so full architectural comparison is meaningful. */
    for (r = 0u; r < PUMA_REG_COUNT; ++r) {
        program[n++] = puma_enc(OP_SET, r, 0u, 0u, 0u);
    }

    /* Four independent memory words -> one non-uniform vector. */
    program[n++] = puma_enc(OP_SET,   0u, 0u, 0u, a);
    program[n++] = puma_enc(OP_STORE, 0u, 0u, 0u, base + 0u);
    program[n++] = puma_enc(OP_SET,   0u, 0u, 0u, b);
    program[n++] = puma_enc(OP_STORE, 0u, 0u, 0u, base + 1u);
    program[n++] = puma_enc(OP_SET,   0u, 0u, 0u, c);
    program[n++] = puma_enc(OP_STORE, 0u, 0u, 0u, base + 2u);
    program[n++] = puma_enc(OP_SET,   0u, 0u, 0u, d);
    program[n++] = puma_enc(OP_STORE, 0u, 0u, 0u, base + 3u);
    program[n++] = puma_enc(OP_LOAD,  1u, 0u, 0u, base);

    /*
     * Safe forward-only control-flow template.
     *
     * cond == 0: BRNZ not taken -> v3 = value_a
     * cond == 1: BRNZ taken     -> v3 = value_b
     */
    program[n++] = puma_enc(OP_SET,  2u, 0u, 0u, cond);
    program[n++] = puma_enc(OP_BRNZ, 0u, 2u, 0u, 21u);
    program[n++] = puma_enc(OP_SET,  3u, 0u, 0u, value_a);
    program[n++] = puma_enc(OP_JMP,  0u, 0u, 0u, 22u);
    program[n++] = puma_enc(OP_SET,  3u, 0u, 0u, value_b);

    /* Always-taken BRNZ + JMP signatures; no loop is possible. */
    program[n++] = puma_enc(OP_SET,  4u, 0u, 0u, 1u);
    program[n++] = puma_enc(OP_BRNZ, 0u, 4u, 0u, 25u);
    program[n++] = puma_enc(OP_SET,  5u, 0u, 0u, 0x1111u);
    program[n++] = puma_enc(OP_JMP,  0u, 0u, 0u, 27u);
    program[n++] = puma_enc(OP_SET,  5u, 0u, 0u, 0x2222u);

    program[n++] = puma_enc(OP_COPY, 6u, 1u, 0u, 0u);
    program[n++] = puma_enc(OP_VADD, 7u, 1u, 3u, 0u);
    program[n++] = puma_enc(OP_HALT, 0u, 0u, 0u, 0u);

    return n;
}

static int phase3b_random_memctrl(void)
{
    uint32_t case_idx;
    uint32_t program[PUMA_IMEM_DEPTH];

    rng_state = SEED_MEMCTRL;

    printf("\n");
    printf("=====================================================\n");
    printf(" PHASE 3B: GOLDEN RANDOM MEMORY/CONTROL REGRESSION\n");
    printf("=====================================================\n");
    printf("seed = 0x%08lx, cases = %lu\n",
           (unsigned long)SEED_MEMCTRL,
           (unsigned long)NUM_RANDOM_MEMCTRL_CASES);

    for (case_idx = 0u;
         case_idx < NUM_RANDOM_MEMCTRL_CASES;
         ++case_idx) {
        uint32_t case_seed = rng_state;
        uint32_t words;
        uint32_t bad_reg = 0u;
        uint32_t bad_lane = 0u;

        words = build_random_memctrl_program(program);

        golden_reset(&golden);
        if (golden_execute(program, words, &golden) != 0) {
            printf("FAIL: PHASE 3B golden interpreter error at case %lu\n",
                   (unsigned long)case_idx);
            dump_program(program, words);
            return 0;
        }

        if (!puma_program_and_run("PHASE 3B case", program, words, 0)) {
            printf("FAIL case = %lu, case_seed = 0x%08lx\n",
                   (unsigned long)case_idx,
                   (unsigned long)case_seed);
            dump_program(program, words);
            return 0;
        }

        if (!compare_regs_to_golden(
                &golden, 0xFFu, &bad_reg, &bad_lane)) {
            printf("FAIL: PHASE 3B case %lu\n",
                   (unsigned long)case_idx);
            printf("  phase seed = 0x%08lx\n",
                   (unsigned long)SEED_MEMCTRL);
            printf("  case seed  = 0x%08lx\n",
                   (unsigned long)case_seed);
            printf("  mismatch   = v%lu lane%lu\n",
                   (unsigned long)bad_reg,
                   (unsigned long)bad_lane);
            dump_reg_mismatch(bad_reg, &golden);
            dump_program(program, words);
            return 0;
        }

        if (((case_idx + 1u) % 10u) == 0u ||
            (case_idx + 1u) == NUM_RANDOM_MEMCTRL_CASES) {
            printf("  PHASE 3B: %lu/%lu cases PASS\n",
                   (unsigned long)(case_idx + 1u),
                   (unsigned long)NUM_RANDOM_MEMCTRL_CASES);
        }
    }

    printf("PHASE 3B PASS\n");
    return 1;
}


/* -------------------------------------------------------------------------- */
/* PHASE 3C: random PISLib MVM vs golden                                       */
/* -------------------------------------------------------------------------- */

static int phase3c_random_mvm(void)
{
    uint32_t case_idx;
    uint32_t program[PUMA_IMEM_DEPTH];

    rng_state = SEED_MVM;

    printf("\n");
    printf("=====================================================\n");
    printf(" PHASE 3C: GOLDEN RANDOM PISLib MVM REGRESSION\n");
    printf("=====================================================\n");
    printf("seed = 0x%08lx, cases = %lu\n",
           (unsigned long)SEED_MVM,
           (unsigned long)NUM_RANDOM_MVM_CASES);

    for (case_idx = 0u; case_idx < NUM_RANDOM_MVM_CASES; ++case_idx) {
        uint8_t weight[4][4];
        uint8_t input[4];
        uint32_t case_seed = rng_state;
        uint32_t words;
        uint32_t out;
        uint32_t in;
        uint32_t bad_reg = 0u;
        uint32_t bad_lane = 0u;

        for (out = 0u; out < 4u; ++out) {
            for (in = 0u; in < 4u; ++in) {
                weight[out][in] =
                    (uint8_t)(1u + rng_range(4u));
            }
        }

        for (in = 0u; in < 4u; ++in) {
            input[in] = (uint8_t)rng_range(16u);
        }

        words = build_mvm_program(program, weight, input);

        if (words != 31u) {
            printf("FAIL: PHASE 3C builder produced %lu words, expected 31\n",
                   (unsigned long)words);
            return 0;
        }

        golden_reset(&golden);
        if (golden_execute(program, words, &golden) != 0) {
            printf("FAIL: PHASE 3C golden interpreter error at case %lu\n",
                   (unsigned long)case_idx);
            dump_program(program, words);
            return 0;
        }

        if (!puma_program_and_run("PHASE 3C case", program, words, 0)) {
            printf("FAIL case = %lu, case_seed = 0x%08lx\n",
                   (unsigned long)case_idx,
                   (unsigned long)case_seed);
            dump_program(program, words);
            return 0;
        }

        /*
         * v1 is fully defined by LOAD, v2 by MVM.
         * Other registers intentionally persist across accelerator STARTs,
         * so PHASE 3C compares only the architecturally defined registers.
         */
        if (!compare_regs_to_golden(
                &golden,
                (1u << 1) | (1u << 2),
                &bad_reg,
                &bad_lane)) {
            printf("FAIL: PHASE 3C case %lu\n",
                   (unsigned long)case_idx);
            printf("  phase seed = 0x%08lx\n",
                   (unsigned long)SEED_MVM);
            printf("  case seed  = 0x%08lx\n",
                   (unsigned long)case_seed);
            printf("  mismatch   = v%lu lane%lu\n",
                   (unsigned long)bad_reg,
                   (unsigned long)bad_lane);
            dump_reg_mismatch(bad_reg, &golden);

            printf("  input = [%u, %u, %u, %u]\n",
                   (unsigned)input[0],
                   (unsigned)input[1],
                   (unsigned)input[2],
                   (unsigned)input[3]);

            printf("  weight matrix:\n");
            for (out = 0u; out < 4u; ++out) {
                printf("    [%u, %u, %u, %u]\n",
                       (unsigned)weight[out][0],
                       (unsigned)weight[out][1],
                       (unsigned)weight[out][2],
                       (unsigned)weight[out][3]);
            }

            dump_program(program, words);
            return 0;
        }

        if (((case_idx + 1u) % 5u) == 0u ||
            (case_idx + 1u) == NUM_RANDOM_MVM_CASES) {
            printf("  PHASE 3C: %lu/%lu cases PASS\n",
                   (unsigned long)(case_idx + 1u),
                   (unsigned long)NUM_RANDOM_MVM_CASES);
        }
    }

    printf("PHASE 3C PASS\n");
    return 1;
}


/* -------------------------------------------------------------------------- */
/* main                                                                        */
/* -------------------------------------------------------------------------- */

int main(void)
{
    int p1;
    int p2;
    int p3a;
    int p3b;
    int p3c;

    printf("\n");
    printf("=====================================================\n");
    printf(" X-HEEP + PUMA + PISLib GOLDEN RANDOM REGRESSION\n");
    printf("=====================================================\n");
    printf("PUMA base       = 0x%08lx\n",
           (unsigned long)PUMA_BASE_ADDR);
    printf("LONG_REGRESSION = %d\n", LONG_REGRESSION);

    p1 = phase1_directed_isa();
    if (!p1) {
        printf("\nFAIL: PHASE 1 directed ISA regression\n");
        return 1;
    }

    p2 = phase2_directed_pislib();
    if (!p2) {
        printf("\nFAIL: PHASE 2 PISLib regression\n");
        return 1;
    }

    p3a = phase3a_random_alu();
    if (!p3a) {
        printf("\nFAIL: PHASE 3A golden random ALU/dataflow regression\n");
        return 1;
    }

    p3b = phase3b_random_memctrl();
    if (!p3b) {
        printf("\nFAIL: PHASE 3B golden random memory/control regression\n");
        return 1;
    }

    p3c = phase3c_random_mvm();
    if (!p3c) {
        printf("\nFAIL: PHASE 3C golden random PISLib MVM regression\n");
        return 1;
    }

    printf("\n");
    printf("=====================================================\n");
    printf("FULL PUMA / X-HEEP / PISLib\n");
    printf("GOLDEN RANDOM REGRESSION PASS\n");
    printf("=====================================================\n");
    printf("  PHASE 1 directed ISA              PASS\n");
    printf("  PHASE 2 directed PISLib MVM       PASS\n");
    printf("  PHASE 3A random ALU/dataflow      PASS (%lu cases)\n",
           (unsigned long)NUM_RANDOM_ALU_CASES);
    printf("  PHASE 3B random memory/control    PASS (%lu cases)\n",
           (unsigned long)NUM_RANDOM_MEMCTRL_CASES);
    printf("  PHASE 3C random PISLib MVM        PASS (%lu cases)\n",
           (unsigned long)NUM_RANDOM_MVM_CASES);
    printf("=====================================================\n");

    return 0;
}
