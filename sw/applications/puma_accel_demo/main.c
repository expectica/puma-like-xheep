#include <stdint.h>
#include <stdio.h>

#include "puma_accel.h"

/*
 * =============================================================
 * PUMA-LIKE PROGRAMMABLE ACCELERATOR DEMO
 * =============================================================
 *
 * X-HEEP RISC-V CPU
 *        |
 *        | MMIO
 *        v
 * PUMA instruction memory
 *        |
 *        v
 * PC -> Decode -> FSM
 *                  |
 *                  +-> STORE / LOAD
 *                  |
 *                  +-> WSTORE -> PISLib weight memory
 *                  |
 *                  +-> MVM    -> PISLib computation
 *                                   |
 *                                   v
 *                             Vector register
 *                                   |
 *                              MMIO readback
 *                                   |
 *                                   v
 *                             RISC-V CPU
 *
 * Demonstration:
 *
 *     x =
 *     [1, 2, 3, 4]
 *
 *     W =
 *     [4 3 2 1]
 *     [2 4 3 1]
 *     [1 3 4 2]
 *     [1 2 3 4]
 *
 *     y = W*x
 *
 *     expected =
 *     [20, 23, 27, 30]
 *
 * =============================================================
 */

#define PUMA_BASE_ADDR ((uintptr_t)0x20077000u)

/*
 * Simplified PUMA-like ISA
 *
 * [31:28] opcode
 * [27:24] rd
 * [23:20] rs1
 * [19:16] rs2
 * [15: 0] immediate
 */
#define OP_SET     0x1u
#define OP_MVM     0x6u
#define OP_JMP     0x7u
#define OP_LOAD    0x9u
#define OP_STORE   0xAu
#define OP_WSTORE  0xBu
#define OP_VSET4   0xCu
#define OP_HALT    0xFu


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

static inline uint32_t read_mcycle(void)
{
    uint32_t value;

    __asm__ volatile (
        "csrr %0, mcycle"
        : "=r"(value)
        :
        : "memory"
    );

    return value;
}


/*
 * Encode one PUMA-like 32-bit instruction.
 */
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


/*
 * Pack four unsigned 4-bit lane values into the VSET4 immediate:
 * lane0 -> bits 3:0, lane1 -> 7:4, lane2 -> 11:8, lane3 -> 15:12.
 */
static uint32_t puma_pack4(
    uint32_t lane0,
    uint32_t lane1,
    uint32_t lane2,
    uint32_t lane3)
{
    return ((lane0 & 0xFu) << 0) |
           ((lane1 & 0xFu) << 4) |
           ((lane2 & 0xFu) << 8) |
           ((lane3 & 0xFu) << 12);
}


/*
 * Write one instruction into accelerator IMEM.
 *
 * IMEM_WRITE is edge-sensitive, therefore generate:
 *
 *     0 -> 1 -> 0
 */
static void puma_program_instruction(
    uint32_t addr,
    uint32_t instruction)
{
    puma_write(PUMA_ACCEL_IMEM_ADDR_REG_OFFSET, addr);
    puma_write(PUMA_ACCEL_IMEM_DATA_REG_OFFSET, instruction);

    puma_write(PUMA_ACCEL_IMEM_WRITE_REG_OFFSET, 0u);
    puma_write(PUMA_ACCEL_IMEM_WRITE_REG_OFFSET, 1u);
    puma_write(PUMA_ACCEL_IMEM_WRITE_REG_OFFSET, 0u);
}


/*
 * Iteration 3 auto-increment loader.
 *
 * Seed the loader pointer once through IMEM_ADDR, then write each
 * instruction directly to IMEM_PUSH.  Hardware writes the instruction to
 * the current pointer and increments the pointer after every accepted push.
 *
 * This reduces the normal programming path from five MMIO writes per
 * instruction to:
 *
 *     1 x IMEM_ADDR + N x IMEM_PUSH
 */
static void puma_program_stream_begin(uint32_t addr)
{
    puma_write(PUMA_ACCEL_IMEM_ADDR_REG_OFFSET, addr);
}


static void puma_program_push(uint32_t instruction)
{
    puma_write(PUMA_ACCEL_IMEM_PUSH_REG_OFFSET, instruction);
}


static void puma_program_push_commit(uint32_t instruction)
{
    puma_write(PUMA_ACCEL_IMEM_PUSH_COMMIT_REG_OFFSET, instruction);
}


/*
 * Iteration 5B CPU-side best-case path for the existing IMEM_PUSH register.
 *
 * This does not change RTL.  It removes the software loop and helper-call /
 * repeated-address overhead by issuing 31 explicit volatile stores to the
 * same IMEM_PUSH MMIO register.  Hardware still auto-increments the IMEM
 * address exactly as in Iteration 3.
 */
#define PUMA_PROGRAM_31_DIRECT(P)                                         \
    do {                                                                  \
        volatile uint32_t *const push_reg__ =                              \
            (volatile uint32_t *)(                                        \
                PUMA_BASE_ADDR +                                          \
                (uintptr_t)PUMA_ACCEL_IMEM_PUSH_REG_OFFSET);               \
        puma_program_stream_begin(0u);                                     \
        *push_reg__ = (P)[0];                                              \
        *push_reg__ = (P)[1];                                              \
        *push_reg__ = (P)[2];                                              \
        *push_reg__ = (P)[3];                                              \
        *push_reg__ = (P)[4];                                              \
        *push_reg__ = (P)[5];                                              \
        *push_reg__ = (P)[6];                                              \
        *push_reg__ = (P)[7];                                              \
        *push_reg__ = (P)[8];                                              \
        *push_reg__ = (P)[9];                                              \
        *push_reg__ = (P)[10];                                             \
        *push_reg__ = (P)[11];                                             \
        *push_reg__ = (P)[12];                                             \
        *push_reg__ = (P)[13];                                             \
        *push_reg__ = (P)[14];                                             \
        *push_reg__ = (P)[15];                                             \
        *push_reg__ = (P)[16];                                             \
        *push_reg__ = (P)[17];                                             \
        *push_reg__ = (P)[18];                                             \
        *push_reg__ = (P)[19];                                             \
        *push_reg__ = (P)[20];                                             \
        *push_reg__ = (P)[21];                                             \
        *push_reg__ = (P)[22];                                             \
        *push_reg__ = (P)[23];                                             \
        *push_reg__ = (P)[24];                                             \
        *push_reg__ = (P)[25];                                             \
        *push_reg__ = (P)[26];                                             \
        *push_reg__ = (P)[27];                                             \
        *push_reg__ = (P)[28];                                             \
        *push_reg__ = (P)[29];                                             \
        *push_reg__ = (P)[30];                                             \
    } while (0)


/*
 * Iteration 7A fused final-word commit path.
 *
 * The first 30 words go through IMEM_PUSH. The 31st word goes through
 * IMEM_PUSH_COMMIT, which writes the word and hands the completed programming
 * bank to the accelerator in the same MMIO transaction.
 */
#define PUMA_PROGRAM_31_DIRECT_COMMIT(P)                                  \
    do {                                                                  \
        volatile uint32_t *const push_reg__ =                              \
            (volatile uint32_t *)(                                        \
                PUMA_BASE_ADDR +                                          \
                (uintptr_t)PUMA_ACCEL_IMEM_PUSH_REG_OFFSET);               \
        volatile uint32_t *const commit_reg__ =                            \
            (volatile uint32_t *)(                                        \
                PUMA_BASE_ADDR +                                          \
                (uintptr_t)PUMA_ACCEL_IMEM_PUSH_COMMIT_REG_OFFSET);        \
        puma_program_stream_begin(0u);                                     \
        *push_reg__ = (P)[0];                                              \
        *push_reg__ = (P)[1];                                              \
        *push_reg__ = (P)[2];                                              \
        *push_reg__ = (P)[3];                                              \
        *push_reg__ = (P)[4];                                              \
        *push_reg__ = (P)[5];                                              \
        *push_reg__ = (P)[6];                                              \
        *push_reg__ = (P)[7];                                              \
        *push_reg__ = (P)[8];                                              \
        *push_reg__ = (P)[9];                                              \
        *push_reg__ = (P)[10];                                             \
        *push_reg__ = (P)[11];                                             \
        *push_reg__ = (P)[12];                                             \
        *push_reg__ = (P)[13];                                             \
        *push_reg__ = (P)[14];                                             \
        *push_reg__ = (P)[15];                                             \
        *push_reg__ = (P)[16];                                             \
        *push_reg__ = (P)[17];                                             \
        *push_reg__ = (P)[18];                                             \
        *push_reg__ = (P)[19];                                             \
        *push_reg__ = (P)[20];                                             \
        *push_reg__ = (P)[21];                                             \
        *push_reg__ = (P)[22];                                             \
        *push_reg__ = (P)[23];                                             \
        *push_reg__ = (P)[24];                                             \
        *push_reg__ = (P)[25];                                             \
        *push_reg__ = (P)[26];                                             \
        *push_reg__ = (P)[27];                                             \
        *push_reg__ = (P)[28];                                             \
        *push_reg__ = (P)[29];                                             \
        *commit_reg__ = (P)[30];                                           \
    } while (0)


/*
 * Iteration 7B warm-kernel path.  The 4x4 matrix weights remain resident in
 * APIM, so a steady-state inference job needs only the 11 instructions that
 * build the new input vector, LOAD it, execute MVM, and HALT.
 */
#define PUMA_PROGRAM_11_DIRECT_COMMIT(P)                                  \
    do {                                                                  \
        volatile uint32_t *const push_reg__ =                              \
            (volatile uint32_t *)(                                        \
                PUMA_BASE_ADDR +                                          \
                (uintptr_t)PUMA_ACCEL_IMEM_PUSH_REG_OFFSET);               \
        volatile uint32_t *const commit_reg__ =                            \
            (volatile uint32_t *)(                                        \
                PUMA_BASE_ADDR +                                          \
                (uintptr_t)PUMA_ACCEL_IMEM_PUSH_COMMIT_REG_OFFSET);        \
        puma_program_stream_begin(0u);                                     \
        *push_reg__ = (P)[0];                                              \
        *push_reg__ = (P)[1];                                              \
        *push_reg__ = (P)[2];                                              \
        *push_reg__ = (P)[3];                                              \
        *push_reg__ = (P)[4];                                              \
        *push_reg__ = (P)[5];                                              \
        *push_reg__ = (P)[6];                                              \
        *push_reg__ = (P)[7];                                              \
        *push_reg__ = (P)[8];                                              \
        *push_reg__ = (P)[9];                                              \
        *commit_reg__ = (P)[10];                                           \
    } while (0)


/*
 * Iteration 8A compact warm kernel: VSET4 + MVM + HALT.
 */
#define PUMA_PROGRAM_3_DIRECT_COMMIT(P)                                   \
    do {                                                                  \
        volatile uint32_t *const push_reg__ =                              \
            (volatile uint32_t *)(                                        \
                PUMA_BASE_ADDR +                                          \
                (uintptr_t)PUMA_ACCEL_IMEM_PUSH_REG_OFFSET);               \
        volatile uint32_t *const commit_reg__ =                            \
            (volatile uint32_t *)(                                        \
                PUMA_BASE_ADDR +                                          \
                (uintptr_t)PUMA_ACCEL_IMEM_PUSH_COMMIT_REG_OFFSET);        \
        puma_program_stream_begin(0u);                                     \
        *push_reg__ = (P)[0];                                              \
        *push_reg__ = (P)[1];                                              \
        *commit_reg__ = (P)[2];                                            \
    } while (0)


/*
 * Iteration 8B resident-kernel patch path.
 *
 * Words 1 and 2 (MVM + HALT) are already resident in the inactive IMEM
 * bank.  Rewind the stream pointer to word 0, replace only the dynamic
 * VSET4 instruction, and fuse that one-word patch with COMMIT.
 */
#define PUMA_PATCH_1_DIRECT_COMMIT(I)                                     \
    do {                                                                  \
        volatile uint32_t *const commit_reg__ =                            \
            (volatile uint32_t *)(                                        \
                PUMA_BASE_ADDR +                                          \
                (uintptr_t)PUMA_ACCEL_IMEM_PUSH_COMMIT_REG_OFFSET);        \
        puma_program_stream_begin(0u);                                     \
        *commit_reg__ = (I);                                               \
    } while (0)


/*
 * Iteration 4B bank selectors.
 *
 * IMEM_PROG_BANK chooses which bank receives CPU programming writes.
 * IMEM_EXEC_BANK chooses which bank will be sampled on the next START.
 * The core latches the execution-bank selection for the whole run.
 */
static void puma_set_program_bank(uint32_t bank)
{
    puma_write(PUMA_ACCEL_IMEM_PROG_BANK_REG_OFFSET, bank & 0x1u);
}


static void puma_set_exec_bank(uint32_t bank)
{
    puma_write(PUMA_ACCEL_IMEM_EXEC_BANK_REG_OFFSET, bank & 0x1u);
}


/*
 * Generate START pulse.
 */
static void puma_start(void)
{
    /*
     * Iteration 5D: CTRL.qe converts this single software write into a
     * one-cycle START pulse. Repeated writes of 1 remain valid launches.
     */
    puma_write(PUMA_ACCEL_CTRL_REG_OFFSET, 1u);
}


/*
 * Iteration 8E-B resident-kernel fast path.
 *
 * RESIDENT_PUSH is an atomic hardware-managed operation:
 *   - patch word 0 of the inactive IMEM bank
 *   - queue that bank for automatic launch
 *
 * Software no longer selects IMEM_PROG_BANK, rewinds IMEM_ADDR, or observes
 * IMEM_ACTIVE_BANK in the streaming loop.
 */
static inline uint32_t puma_resident_ready(void)
{
    return puma_read(PUMA_ACCEL_RESIDENT_STATUS_REG_OFFSET) & 0x1u;
}


static inline void puma_resident_push(uint32_t instruction)
{
    puma_write(PUMA_ACCEL_RESIDENT_PUSH_REG_OFFSET, instruction);
}


/*
 * Iteration 8F: four-entry resident-input FIFO.
 *
 * The CPU may enqueue several future VSET4 words. Hardware consumes one FIFO
 * entry at each HALT boundary, patches word 0 of the opposite resident bank,
 * and immediately launches that bank. This removes per-input bank handoff
 * from the CPU critical path.
 */
static inline uint32_t puma_resident_fifo_status(void)
{
    return puma_read(PUMA_ACCEL_RESIDENT_FIFO_STATUS_REG_OFFSET);
}


static inline uint32_t puma_resident_fifo_ready(void)
{
    return puma_resident_fifo_status() & 0x1u;
}


static inline uint32_t puma_resident_fifo_empty(void)
{
    return (puma_resident_fifo_status() >> 1) & 0x1u;
}


static inline uint32_t puma_resident_fifo_count(void)
{
    return (puma_resident_fifo_status() >> 3) & 0x3fu;
}


static inline void puma_resident_fifo_push(uint32_t instruction)
{
    puma_write(PUMA_ACCEL_RESIDENT_FIFO_PUSH_REG_OFFSET, instruction);
}


/*
 * Read one lane from a vector register through MMIO.
 *
 * selector[2:0] = vector-register index
 * selector[9:8] = lane index
 */
static int16_t puma_read_vreg_lane(
    uint32_t reg,
    uint32_t lane)
{
    uint32_t selector =
        ((lane & 0x3u) << 8) |
        (reg & 0x7u);

    puma_write(
        PUMA_ACCEL_VREG_SEL_REG_OFFSET,
        selector);

    uint32_t raw =
        puma_read(PUMA_ACCEL_VREG_DATA_REG_OFFSET);

    return (int16_t)(raw & 0xFFFFu);
}


static void print_vector(
    const char *name,
    const int values[4])
{
    printf("%s = [%d, %d, %d, %d]\n",
           name,
           values[0],
           values[1],
           values[2],
           values[3]);
}


int main(void)
{
    /*
     * =========================================================
     * PUMA instruction program
     * =========================================================
     *
     * PISLib address mapping:
     *
     * address = row*4 + column*256
     *
     * Matrix:
     *
     *     [4 3 2 1]
     *     [2 4 3 1]
     *     [1 3 4 2]
     *     [1 2 3 4]
     *
     * To fit inside the 32-word IMEM, equal weights are grouped.
     */

    uint32_t program[] = {

        /*
         * -----------------------------------------------------
         * Program matrix weights into PISLib
         * -----------------------------------------------------
         */

        /* weight = 1 */
        puma_enc(OP_SET,    0u, 0u, 0u,   1u),

        puma_enc(OP_WSTORE, 0u, 0u, 0u, 768u),
        puma_enc(OP_WSTORE, 0u, 0u, 0u, 772u),
        puma_enc(OP_WSTORE, 0u, 0u, 0u,   8u),
        puma_enc(OP_WSTORE, 0u, 0u, 0u,  12u),

        /* weight = 2 */
        puma_enc(OP_SET,    0u, 0u, 0u,   2u),

        puma_enc(OP_WSTORE, 0u, 0u, 0u, 512u),
        puma_enc(OP_WSTORE, 0u, 0u, 0u,   4u),
        puma_enc(OP_WSTORE, 0u, 0u, 0u, 776u),
        puma_enc(OP_WSTORE, 0u, 0u, 0u, 268u),

        /* weight = 3 */
        puma_enc(OP_SET,    0u, 0u, 0u,   3u),

        puma_enc(OP_WSTORE, 0u, 0u, 0u, 256u),
        puma_enc(OP_WSTORE, 0u, 0u, 0u, 516u),
        puma_enc(OP_WSTORE, 0u, 0u, 0u, 264u),
        puma_enc(OP_WSTORE, 0u, 0u, 0u, 524u),

        /* weight = 4 */
        puma_enc(OP_SET,    0u, 0u, 0u,   4u),

        puma_enc(OP_WSTORE, 0u, 0u, 0u,   0u),
        puma_enc(OP_WSTORE, 0u, 0u, 0u, 260u),
        puma_enc(OP_WSTORE, 0u, 0u, 0u, 520u),
        puma_enc(OP_WSTORE, 0u, 0u, 0u, 780u),


        /*
         * -----------------------------------------------------
         * Construct input vector x = [1,2,3,4]
         * -----------------------------------------------------
         *
         * SET produces the same value in all lanes.
         *
         * Therefore use STORE to create:
         *
         * mem[32] = 1
         * mem[33] = 2
         * mem[34] = 3
         * mem[35] = 4
         *
         * and LOAD them together into v1.
         */

        puma_enc(OP_SET,   0u, 0u, 0u,  1u),
        puma_enc(OP_STORE, 0u, 0u, 0u, 32u),

        puma_enc(OP_SET,   0u, 0u, 0u,  2u),
        puma_enc(OP_STORE, 0u, 0u, 0u, 33u),

        puma_enc(OP_SET,   0u, 0u, 0u,  3u),
        puma_enc(OP_STORE, 0u, 0u, 0u, 34u),

        puma_enc(OP_SET,   0u, 0u, 0u,  4u),
        puma_enc(OP_STORE, 0u, 0u, 0u, 35u),

        /* v1 = [1,2,3,4] */
        puma_enc(OP_LOAD, 1u, 0u, 0u, 32u),


        /*
         * -----------------------------------------------------
         * Matrix-vector multiplication
         * -----------------------------------------------------
         *
         * v2 = W * v1
         */
        puma_enc(OP_MVM, 2u, 1u, 0u, 0u),


        /* Stop execution */
        puma_enc(OP_HALT, 0u, 0u, 0u, 0u)
    };

    /*
     * Distinct Bank-1 probe program.  It must remain independent from the
     * full Bank-0 MVM program above.  Executing this bank later should set
     * every lane of v3 to 123 and then halt after exactly two instructions.
     */
    uint32_t bank_probe_program[] = {
        puma_enc(OP_SET,  3u, 0u, 0u, 123u),
        puma_enc(OP_HALT, 0u, 0u, 0u,   0u)
    };

    /*
     * Iteration 7B separates one-time weight initialization from the warm
     * inference kernel. The full 31-word program above loads all 16 matrix
     * weights into APIM. Once loaded, APIM retains them across HALT/START.
     *
     * Warm kernel A uses x = [1,2,3,4].
     */
    uint32_t warm_kernel_a[] = {
        puma_enc(OP_SET,   0u, 0u, 0u,  1u),
        puma_enc(OP_STORE, 0u, 0u, 0u, 32u),
        puma_enc(OP_SET,   0u, 0u, 0u,  2u),
        puma_enc(OP_STORE, 0u, 0u, 0u, 33u),
        puma_enc(OP_SET,   0u, 0u, 0u,  3u),
        puma_enc(OP_STORE, 0u, 0u, 0u, 34u),
        puma_enc(OP_SET,   0u, 0u, 0u,  4u),
        puma_enc(OP_STORE, 0u, 0u, 0u, 35u),
        puma_enc(OP_LOAD,  1u, 0u, 0u, 32u),
        puma_enc(OP_MVM,   2u, 1u, 0u,  0u),
        puma_enc(OP_HALT,  0u, 0u, 0u,  0u)
    };

    /*
     * Warm kernel B changes only the dynamic input:
     * x = [4,3,2,1]
     *
     * With the same persistent matrix the expected output is:
     * [30,27,23,20].
     */
    uint32_t warm_kernel_b[] = {
        puma_enc(OP_SET,   0u, 0u, 0u,  4u),
        puma_enc(OP_STORE, 0u, 0u, 0u, 32u),
        puma_enc(OP_SET,   0u, 0u, 0u,  3u),
        puma_enc(OP_STORE, 0u, 0u, 0u, 33u),
        puma_enc(OP_SET,   0u, 0u, 0u,  2u),
        puma_enc(OP_STORE, 0u, 0u, 0u, 34u),
        puma_enc(OP_SET,   0u, 0u, 0u,  1u),
        puma_enc(OP_STORE, 0u, 0u, 0u, 35u),
        puma_enc(OP_LOAD,  1u, 0u, 0u, 32u),
        puma_enc(OP_MVM,   2u, 1u, 0u,  0u),
        puma_enc(OP_HALT,  0u, 0u, 0u,  0u)
    };

    /*
     * Iteration 8A: the current PIS backend uses four 4-bit CIM inputs.
     * VSET4 packs the entire input vector into one instruction immediate.
     */
    uint32_t compact_kernel_a[] = {
        puma_enc(OP_VSET4, 1u, 0u, 0u, puma_pack4(1u, 2u, 3u, 4u)),
        puma_enc(OP_MVM,   2u, 1u, 0u, 0u),
        puma_enc(OP_HALT,  0u, 0u, 0u, 0u)
    };

    uint32_t compact_kernel_b[] = {
        puma_enc(OP_VSET4, 1u, 0u, 0u, puma_pack4(4u, 3u, 2u, 1u)),
        puma_enc(OP_MVM,   2u, 1u, 0u, 0u),
        puma_enc(OP_HALT,  0u, 0u, 0u, 0u)
    };


    const uint32_t program_words =
        sizeof(program) / sizeof(program[0]);

    const uint32_t bank_probe_words =
        sizeof(bank_probe_program) / sizeof(bank_probe_program[0]);

    const uint32_t warm_kernel_words =
        sizeof(warm_kernel_a) / sizeof(warm_kernel_a[0]);

    const uint32_t compact_kernel_words =
        sizeof(compact_kernel_a) / sizeof(compact_kernel_a[0]);

    const int input[4] = {
        1, 2, 3, 4
    };

    const int expected[4] = {
        20, 23, 27, 30
    };

    const int warm_expected_b[4] = {
        30, 27, 23, 20
    };


    printf("\n");
    printf("================================================\n");
    printf(" PUMA-LIKE PROGRAMMABLE ACCELERATOR DEMO\n");
    printf("================================================\n");
    printf("\n");

    printf("Hardware path:\n");
    printf("  RISC-V CPU -> MMIO -> PUMA FSM -> PISLib -> MMIO\n");
    printf("\n");

    print_vector("Input vector", input);

    printf("\n");
    printf("Matrix W:\n");
    printf("  [4 3 2 1]\n");
    printf("  [2 4 3 1]\n");
    printf("  [1 3 4 2]\n");
    printf("  [1 2 3 4]\n");

    printf("\n");
    print_vector("Expected", expected);

    printf("\n");
    printf("Programming accelerator with %lu PUMA instructions...\n",
           (unsigned long)program_words);
    printf("Using Iteration 5D single-write START with direct-overlap scheduling.\n");


    /*
     * The current PUMA IMEM contains 32 words.
     * This demo uses exactly 31 instructions.
     */
    if (program_words > 32u) {
        printf("DEMO FAIL: program does not fit in IMEM\n");
        return 1;
    }

    /* Program the full application into Bank 0.  Keep the bank-select MMIO
     * write outside the measured loader interval so Iteration 3's 317-cycle
     * metric remains directly comparable. */
    puma_set_program_bank(0u);

    uint32_t cpu_program_start = read_mcycle();

    puma_program_stream_begin(0u);

    for (uint32_t i = 0u; i < program_words; i++) {
        puma_program_push(program[i]);
    }

    uint32_t cpu_program_end = read_mcycle();
    uint32_t cpu_program_cycles =
    cpu_program_end - cpu_program_start;


    printf("CPU programmed PUMA instruction memory.\n");
    printf("\nCPU-side programming timing:\n");
    printf("  program words  = %lu\n",
       (unsigned long)program_words);
    printf("  MMIO writes    = %lu\n",
       (unsigned long)(program_words + 1u));
    printf("  program cycles = %lu\n",
       (unsigned long)cpu_program_cycles);

    /*
     * Iteration 4B deliberately leaves Bank 1 unprogrammed here.  Its probe
     * program will be streamed only after Bank 0 is confirmed BUSY.  A later
     * successful Bank-1 execution therefore proves that inactive-bank writes
     * were genuinely accepted during Bank-0 execution.
     */
    printf("Iteration 4B bank setup:\n");
    printf("  Bank 0 program words = %lu\n", (unsigned long)program_words);
    printf("  Bank 1 probe words   = %lu (programmed during Bank 0 BUSY)\n",
           (unsigned long)bank_probe_words);

    /* The safety fault injection below intentionally targets the active bank. */
    puma_set_program_bank(0u);
    puma_set_exec_bank(0u);

    printf("Starting accelerator from IMEM Bank 0...\n");


    /*
     * STATUS:
     *
     * bit 0 = DONE
     * bit 1 = BUSY
     */
    /*
     * Iteration 2 IMEM safety fault-injection test.
     *
     * The last instruction is HALT.  While the accelerator is BUSY,
     * deliberately try to replace it with:
     *
     *     JMP final_pc
     *
     * If BUSY-time IMEM protection is working, the write is rejected
     * and the original HALT remains intact.  If it is not working,
     * execution would loop forever at the final PC.
     *
     * Do not print anything between observing BUSY and issuing the
     * illegal write: UART printing could otherwise consume enough CPU
     * cycles for this short accelerator program to finish first.
     */
    const uint32_t fault_pc =
        program_words - 1u;

    const uint32_t malicious_instruction =
        puma_enc(
            OP_JMP,
            0u,
            0u,
            0u,
            fault_pc);

    uint32_t fault_status_before = 0u;
    uint32_t fault_status_after = 0u;
    uint32_t busy_timeout = 10000u;

    puma_start();

    /*
     * Wait until STATUS bit 1 confirms that the accelerator is BUSY.
     */
    do {

        fault_status_before =
            puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        if ((fault_status_before & 0x2u) != 0u) {
            break;
        }

        busy_timeout--;

    } while (busy_timeout != 0u);


    if (busy_timeout == 0u) {

        printf("\n");
        printf("FAULT TEST FAIL: never observed accelerator BUSY.\n");

        printf("STATUS = 0x%08lx\n",
               (unsigned long)
               puma_read(PUMA_ACCEL_STATUS_REG_OFFSET));

        return 1;
    }


    /*
     * Intentionally illegal write:
     *
     *     IMEM[last] : HALT -> JMP last
     *
     * With the RTL protection in puma_like_core, the CPU-side MMIO
     * request is accepted by the register interface, but the actual
     * IMEM write must be ignored while the core is executing.
     */
    puma_program_instruction(
        fault_pc,
        malicious_instruction);

    /*
     * Capture STATUS immediately after the write, before any printf.
     * Seeing BUSY here proves that the write transaction completed
     * while accelerator execution was still active.
     */
    fault_status_after =
        puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

    /*
     * Iteration 4B positive concurrency test.
     *
     * The active-bank overwrite above must be rejected.  Now switch only the
     * programming target to inactive Bank 1 and stream the distinct two-word
     * probe program while Bank 0 is still executing.  No UART output is issued
     * until after the final STATUS sample, so printing cannot create a false
     * overlap result.
     */
    puma_set_program_bank(1u);

    const uint32_t overlap_status_before =
        puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

    const uint32_t overlap_program_start = read_mcycle();

    puma_program_stream_begin(0u);

    for (uint32_t i = 0u; i < bank_probe_words; i++) {
        puma_program_push(bank_probe_program[i]);
    }

    const uint32_t overlap_program_end = read_mcycle();
    const uint32_t overlap_program_cycles =
        overlap_program_end - overlap_program_start;

    const uint32_t overlap_status_after =
        puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);


    printf("\n");
    printf("===== IMEM SAFETY FAULT TEST =====\n");

    printf("Accelerator BUSY detected: STATUS = 0x%08lx\n",
           (unsigned long)fault_status_before);

    printf("Attempting BUSY-time overwrite:\n");

    printf("  IMEM[%lu] HALT -> JMP %lu\n",
           (unsigned long)fault_pc,
           (unsigned long)fault_pc);

    printf("STATUS after attempted write = 0x%08lx\n",
           (unsigned long)fault_status_after);

    printf("\n");
    printf("===== ITERATION 4B CONCURRENT PROGRAMMING TEST =====\n");
    printf("  execution bank       = 0\n");
    printf("  programming bank     = 1\n");
    printf("  probe words          = %lu\n",
           (unsigned long)bank_probe_words);
    printf("  overlap prog cycles  = %lu\n",
           (unsigned long)overlap_program_cycles);
    printf("  STATUS before stream = 0x%08lx\n",
           (unsigned long)overlap_status_before);
    printf("  STATUS after stream  = 0x%08lx\n",
           (unsigned long)overlap_status_after);


    if ((fault_status_after & 0x2u) == 0u) {

        printf("FAULT TEST INVALID: accelerator was no longer BUSY "
               "when the write completed.\n");

        return 1;
    }

    if ((overlap_status_before & 0x2u) == 0u) {
        printf("ITERATION 4B FAIL: accelerator was not BUSY before "
               "inactive-bank programming.\n");
        return 1;
    }

    if ((overlap_status_after & 0x2u) == 0u) {
        printf("ITERATION 4B FAIL: Bank 0 stopped being BUSY before "
               "the Bank 1 probe stream completed.\n");
        return 1;
    }


    printf("Fault write issued while accelerator was still BUSY.\n");
    printf("Inactive Bank 1 probe programmed completely while Bank 0 remained BUSY.\n");
    printf("Now waiting for the original HALT...\n");


    uint32_t status = 0u;
    uint32_t timeout = 2000000u;

    do {

        status =
            puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        if ((status & 0x1u) != 0u) {
            break;
        }

        timeout--;

    } while (timeout != 0u);


    if (timeout == 0u) {

        printf("\n");
        printf("DEMO FAIL: timeout waiting for accelerator\n");

        printf("STATUS = 0x%08lx\n",
               (unsigned long)
               puma_read(PUMA_ACCEL_STATUS_REG_OFFSET));

        printf("PC     = 0x%08lx\n",
               (unsigned long)
               puma_read(PUMA_ACCEL_PC_REG_OFFSET));

        printf("IR     = 0x%08lx\n",
               (unsigned long)
               puma_read(PUMA_ACCEL_IR_REG_OFFSET));

        return 1;
    }


    printf("Accelerator DONE.\n");

    /*
     * Read hardware performance counters immediately after HALT. The PUMA
     * counters stop changing in S_HALT and are cleared only by the next START.
     */
    const uint32_t perf_total_cycles =
        puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);

    const uint32_t perf_instr =
        puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);

    const uint32_t perf_load_cycles =
        puma_read(PUMA_ACCEL_PERF_LOAD_CYCLES_REG_OFFSET);

    const uint32_t perf_store_cycles =
        puma_read(PUMA_ACCEL_PERF_STORE_CYCLES_REG_OFFSET);

    const uint32_t perf_wstore_cycles =
        puma_read(PUMA_ACCEL_PERF_WSTORE_CYCLES_REG_OFFSET);

    const uint32_t perf_mvm_cycles =
        puma_read(PUMA_ACCEL_PERF_MVM_CYCLES_REG_OFFSET);

    printf("\n");
    printf("PUMA performance counters:\n");
    printf("  full-program reference cycles = %lu\n",
           (unsigned long)perf_total_cycles);
    printf("  total cycles  = %lu\n", (unsigned long)perf_total_cycles);
    printf("  instructions  = %lu\n", (unsigned long)perf_instr);
    printf("  load cycles   = %lu\n", (unsigned long)perf_load_cycles);
    printf("  store cycles  = %lu\n", (unsigned long)perf_store_cycles);
    printf("  wstore cycles = %lu\n", (unsigned long)perf_wstore_cycles);
    printf("  mvm cycles    = %lu\n", (unsigned long)perf_mvm_cycles);


    /*
     * Read input v1 as additional proof that STORE -> LOAD
     * created the desired non-uniform vector.
     */
    int input_readback[4];

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        input_readback[lane] =
            (int)puma_read_vreg_lane(1u, lane);
    }


    /*
     * Read PISLib MVM output v2.
     */
    int result[4];
    int pass = 1;

    for (uint32_t lane = 0u; lane < 4u; lane++) {

        result[lane] =
            (int)puma_read_vreg_lane(2u, lane);

        if (result[lane] != expected[lane]) {
            pass = 0;
        }

        if (input_readback[lane] != input[lane]) {
            pass = 0;
        }
    }


    printf("\n");
    print_vector("Input readback", input_readback);
    print_vector("MVM result", result);
    print_vector("Expected", expected);
    printf("\n");


    if (!pass) {

        printf("================================================\n");
        printf(" DEMO FAIL\n");
        printf("================================================\n");

        return 1;
    }

    /*
     * Iteration 4B bank-selection/concurrency regression.
     *
     * Bank 1 was programmed while Bank 0 was BUSY. Bank 0 has already
     * completed the full MVM correctly, proving the active bank was isolated.
     * Now select Bank 1 for a second START and verify that the two instructions
     * written during the overlap window were retained and execute correctly.
     */
    printf("\n");
    printf("===== ITERATION 4B BANK SELECTION TEST =====\n");

    puma_set_exec_bank(1u);
    puma_start();

    status = 0u;
    timeout = 2000000u;

    do {
        status = puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        if ((status & 0x1u) != 0u) {
            break;
        }

        timeout--;
    } while (timeout != 0u);

    if (timeout == 0u) {
        printf("BANK TEST FAIL: timeout waiting for Bank 1 probe.\n");
        return 1;
    }

    const uint32_t active_bank =
        puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;

    const uint32_t bank_perf_total_cycles =
        puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);

    const uint32_t bank_perf_instr =
        puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);

    int bank_probe_readback[4];
    int bank_test_pass = 1;

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        bank_probe_readback[lane] =
            (int)puma_read_vreg_lane(3u, lane);

        if (bank_probe_readback[lane] != 123) {
            bank_test_pass = 0;
        }
    }

    if (active_bank != 1u) {
        bank_test_pass = 0;
    }

    if (bank_perf_instr != 2u) {
        bank_test_pass = 0;
    }

    if (bank_perf_total_cycles != 6u) {
        bank_test_pass = 0;
    }

    printf("  active bank   = %lu\n", (unsigned long)active_bank);
    printf("  instructions  = %lu\n", (unsigned long)bank_perf_instr);
    printf("  total cycles  = %lu\n", (unsigned long)bank_perf_total_cycles);
    print_vector("  v3 probe", bank_probe_readback);
    printf("  expected      = [123, 123, 123, 123]\n");

    if (!bank_test_pass) {
        printf(" ITERATION 4B BANK SELECTION TEST FAIL\n");
        return 1;
    }

    printf(" ITERATION 4B BANK SELECTION TEST PASS\n");

    /*
     * =========================================================
     * Iteration 4C: full-program steady-state overlap benchmark
     * =========================================================
     *
     * 4B proved the ownership rule with a two-word Bank-1 probe.
     * 4C now uses the complete 31-word MVM program as the next job
     * and measures two launch intervals:
     *
     *   sequential:
     *       execute Bank 0 -> program Bank 1 -> execute Bank 1
     *
     *   overlapped:
     *       execute Bank 0
     *              ||
     *       program Bank 1
     *              -> execute Bank 1
     *
     * Both paths perform the same full Bank-1 programming stream and
     * use the same two banks.  No printf is issued inside either timed
     * launch interval.
     */
    printf("\n");
    printf("===== ITERATION 4C FULL-PROGRAM OVERLAP BENCHMARK =====\n");

    /*
     * -------------------------------
     * 4C-A: sequential reference
     * -------------------------------
     *
     * Bank 0 already contains the full MVM program.  Run it to HALT,
     * then program the same full program into Bank 1, then START Bank 1.
     */
    puma_set_exec_bank(0u);

    puma_start();
    const uint32_t seq_launch0_after = read_mcycle();

    uint32_t bench_status = 0u;
    uint32_t bench_timeout = 2000000u;

    do {
        bench_status = puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        if ((bench_status & 0x1u) != 0u) {
            break;
        }

        bench_timeout--;
    } while (bench_timeout != 0u);

    if (bench_timeout == 0u) {
        printf("ITERATION 4C FAIL: sequential Bank 0 timeout.\n");
        return 1;
    }

    puma_set_program_bank(1u);

    const uint32_t seq_program_start = read_mcycle();

    puma_program_stream_begin(0u);

    for (uint32_t i = 0u; i < program_words; i++) {
        puma_program_push(program[i]);
    }

    const uint32_t seq_program_end = read_mcycle();
    const uint32_t seq_full_program_cycles =
        seq_program_end - seq_program_start;

    puma_set_exec_bank(1u);

    puma_start();
    const uint32_t seq_launch1_after = read_mcycle();

    const uint32_t seq_start_interval =
        seq_launch1_after - seq_launch0_after;

    bench_status = 0u;
    bench_timeout = 2000000u;

    do {
        bench_status = puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        if ((bench_status & 0x1u) != 0u) {
            break;
        }

        bench_timeout--;
    } while (bench_timeout != 0u);

    if (bench_timeout == 0u) {
        printf("ITERATION 4C FAIL: sequential Bank 1 timeout.\n");
        return 1;
    }

    const uint32_t seq_active_bank =
        puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;

    const uint32_t seq_bank1_cycles =
        puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);

    const uint32_t seq_bank1_instr =
        puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);

    int seq_result[4];
    int seq_pass = 1;

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        seq_result[lane] =
            (int)puma_read_vreg_lane(2u, lane);

        if (seq_result[lane] != expected[lane]) {
            seq_pass = 0;
        }
    }

    if (seq_active_bank != 1u ||
        seq_bank1_cycles != perf_total_cycles ||
        seq_bank1_instr != program_words) {
        seq_pass = 0;
    }

    if (!seq_pass) {
        printf("ITERATION 4C FAIL: sequential reference regression failed.\n");
        return 1;
    }


    /*
     * -------------------------------
     * 4C-B: overlapped path
     * -------------------------------
     *
     * Bank 0 still contains the same full program.  START it, confirm BUSY,
     * then overwrite Bank 1 with the complete 31-word next program while
     * Bank 0 is executing.  The next execution-bank selector is also changed
     * during the current run; the core's latched active bank must remain 0.
     */
    puma_set_exec_bank(0u);

    puma_start();
    const uint32_t ov_launch0_after = read_mcycle();

    bench_status = 0u;
    bench_timeout = 10000u;

    do {
        bench_status = puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        if ((bench_status & 0x2u) != 0u) {
            break;
        }

        bench_timeout--;
    } while (bench_timeout != 0u);

    if (bench_timeout == 0u) {
        printf("ITERATION 4C FAIL: never observed Bank 0 BUSY.\n");
        return 1;
    }

    const uint32_t ov_status_before_stream = bench_status;

    /*
     * Preselect Bank 1 for the next START while Bank 0 remains latched as
     * the active execution bank, then target Bank 1 for programming.
     */
    puma_set_exec_bank(1u);
    puma_set_program_bank(1u);

    const uint32_t ov_program_start = read_mcycle();

    puma_program_stream_begin(0u);

    for (uint32_t i = 0u; i < program_words; i++) {
        puma_program_push(program[i]);
    }

    const uint32_t ov_program_end = read_mcycle();
    const uint32_t ov_full_program_cycles =
        ov_program_end - ov_program_start;

    const uint32_t ov_status_after_stream =
        puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

    /*
     * Full programming may be longer than the current PUMA execution window,
     * but do not hard-code that assumption.  If Bank 0 is still running,
     * wait for DONE; otherwise proceed immediately.
     */
    bench_status = ov_status_after_stream;
    bench_timeout = 2000000u;

    while (((bench_status & 0x1u) == 0u) &&
           (bench_timeout != 0u)) {

        bench_status =
            puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        bench_timeout--;
    }

    if (bench_timeout == 0u) {
        printf("ITERATION 4C FAIL: overlapped Bank 0 timeout.\n");
        return 1;
    }

    /*
     * IMEM_EXEC_BANK was already preselected to Bank 1 while Bank 0 ran.
     * Launch the fully programmed next job immediately.
     */
    puma_start();
    const uint32_t ov_launch1_after = read_mcycle();

    const uint32_t ov_start_interval =
        ov_launch1_after - ov_launch0_after;

    const uint32_t interval_saved =
        (seq_start_interval > ov_start_interval)
            ? (seq_start_interval - ov_start_interval)
            : 0u;

    bench_status = 0u;
    bench_timeout = 2000000u;

    do {
        bench_status = puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        if ((bench_status & 0x1u) != 0u) {
            break;
        }

        bench_timeout--;
    } while (bench_timeout != 0u);

    if (bench_timeout == 0u) {
        printf("ITERATION 4C FAIL: overlapped Bank 1 timeout.\n");
        return 1;
    }

    const uint32_t ov_active_bank =
        puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;

    const uint32_t ov_bank1_cycles =
        puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);

    const uint32_t ov_bank1_instr =
        puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);

    int ov_result[4];
    int ov_pass = 1;

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        ov_result[lane] =
            (int)puma_read_vreg_lane(2u, lane);

        if (ov_result[lane] != expected[lane]) {
            ov_pass = 0;
        }
    }

    if (ov_active_bank != 1u ||
        ov_bank1_cycles != perf_total_cycles ||
        ov_bank1_instr != program_words) {
        ov_pass = 0;
    }

    if (ov_start_interval >= seq_start_interval) {
        ov_pass = 0;
    }

    printf("  full program words          = %lu\n",
           (unsigned long)program_words);
    printf("  sequential program cycles   = %lu\n",
           (unsigned long)seq_full_program_cycles);
    printf("  overlap program cycles      = %lu\n",
           (unsigned long)ov_full_program_cycles);
    printf("  sequential START interval   = %lu\n",
           (unsigned long)seq_start_interval);
    printf("  overlapped START interval   = %lu\n",
           (unsigned long)ov_start_interval);
    printf("  interval cycles saved       = %lu\n",
           (unsigned long)interval_saved);
    printf("  STATUS before full stream   = 0x%08lx\n",
           (unsigned long)ov_status_before_stream);
    printf("  STATUS after full stream    = 0x%08lx\n",
           (unsigned long)ov_status_after_stream);
    printf("  final active bank           = %lu\n",
           (unsigned long)ov_active_bank);
    printf("  final instructions          = %lu\n",
           (unsigned long)ov_bank1_instr);
    printf("  final execution cycles      = %lu\n",
           (unsigned long)ov_bank1_cycles);
    print_vector("  final MVM result", ov_result);
    print_vector("  expected", expected);

    if (!ov_pass) {
        printf(" ITERATION 4C FULL-PROGRAM OVERLAP BENCHMARK FAIL\n");
        return 1;
    }

    printf(" ITERATION 4C FULL-PROGRAM OVERLAP BENCHMARK PASS\n");

    /*
     * =========================================================
     * Iteration 5A: loader scaling characterization
     * =========================================================
     *
     * Measure the same IMEM_PUSH stream for N = 1,2,4,8,16,31 words:
     *
     *   A) while PUMA is halted (IDLE/HALT programming)
     *   B) while Bank 0 is BUSY and Bank 1 is the inactive target
     *
     * The benchmark does not change RTL.  It is intended to distinguish
     * fixed setup cost from the per-word MMIO cost before choosing the next
     * loader architecture (direct window, FIFO, DMA/burst, etc.).
     */
    printf("\n");
    printf("===== ITERATION 5A LOADER SCALING BENCHMARK =====\n");

    const uint32_t loader_sizes[] = {
        1u, 2u, 4u, 8u, 16u, 31u
    };

    const uint32_t loader_size_count =
        sizeof(loader_sizes) / sizeof(loader_sizes[0]);

    uint32_t idle_loader_cycles[6] = {0u};
    uint32_t busy_loader_cycles[6] = {0u};
    uint32_t busy_status_before[6] = {0u};
    uint32_t busy_status_after[6] = {0u};

    /*
     * 5A-A: halted/idle programming cost.
     *
     * Bank 1 is not executed during this characterization, so repeatedly
     * overwriting its prefix is harmless.
     */
    puma_set_program_bank(1u);

    for (uint32_t t = 0u; t < loader_size_count; t++) {
        const uint32_t words = loader_sizes[t];

        const uint32_t t0 = read_mcycle();

        puma_program_stream_begin(0u);

        for (uint32_t i = 0u; i < words; i++) {
            puma_program_push(program[i]);
        }

        const uint32_t t1 = read_mcycle();

        idle_loader_cycles[t] = t1 - t0;
    }

    /*
     * 5A-B: inactive-bank programming while Bank 0 executes.
     *
     * Bank 0 still contains the complete 31-word MVM program and is never
     * modified here.  For each point, launch Bank 0, confirm BUSY, then time
     * the Bank-1 stream.  STATUS after the stream tells us whether the whole
     * stream fit inside the current full-program execution window.
     */
    for (uint32_t t = 0u; t < loader_size_count; t++) {
        const uint32_t words = loader_sizes[t];

        puma_set_exec_bank(0u);
        puma_set_program_bank(1u);

        puma_start();

        uint32_t local_status = 0u;
        uint32_t local_timeout = 10000u;

        do {
            local_status =
                puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

            if ((local_status & 0x2u) != 0u) {
                break;
            }

            local_timeout--;
        } while (local_timeout != 0u);

        if (local_timeout == 0u) {
            printf("ITERATION 5A FAIL: never observed BUSY for %lu-word test.\n",
                   (unsigned long)words);
            return 1;
        }

        busy_status_before[t] = local_status;

        const uint32_t t0 = read_mcycle();

        puma_program_stream_begin(0u);

        for (uint32_t i = 0u; i < words; i++) {
            puma_program_push(program[i]);
        }

        const uint32_t t1 = read_mcycle();

        busy_loader_cycles[t] = t1 - t0;

        busy_status_after[t] =
            puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        /*
         * Finish the active Bank-0 run before starting the next sample.
         */
        local_status = busy_status_after[t];
        local_timeout = 2000000u;

        while (((local_status & 0x1u) == 0u) &&
               (local_timeout != 0u)) {

            local_status =
                puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

            local_timeout--;
        }

        if (local_timeout == 0u) {
            printf("ITERATION 5A FAIL: timeout after %lu-word BUSY test.\n",
                   (unsigned long)words);
            return 1;
        }
    }

    printf("  words | idle cycles | busy-target cycles | BUSY after stream\n");

    for (uint32_t t = 0u; t < loader_size_count; t++) {
        printf("  %5lu | %11lu | %18lu | %s\n",
               (unsigned long)loader_sizes[t],
               (unsigned long)idle_loader_cycles[t],
               (unsigned long)busy_loader_cycles[t],
               ((busy_status_after[t] & 0x2u) != 0u) ? "yes" : "no");
    }

    int loader_test_pass = 1;

    for (uint32_t t = 0u; t < loader_size_count; t++) {
        if ((busy_status_before[t] & 0x2u) == 0u) {
            loader_test_pass = 0;
        }

        if (idle_loader_cycles[t] == 0u ||
            busy_loader_cycles[t] == 0u) {
            loader_test_pass = 0;
        }

        if ((t != 0u) &&
            (idle_loader_cycles[t] <= idle_loader_cycles[t - 1u])) {
            loader_test_pass = 0;
        }
    }

    if (!loader_test_pass) {
        printf(" ITERATION 5A LOADER SCALING BENCHMARK FAIL\n");
        return 1;
    }

    printf(" ITERATION 5A LOADER SCALING BENCHMARK PASS\n");

    /*
     * =========================================================
     * Iteration 5B: unrolled direct-MMIO software-overhead test
     * =========================================================
     *
     * 5A showed an exactly linear cost with N.  Before adding new loader RTL,
     * measure how much of that slope is caused by the CPU loop/helper path.
     *
     * The direct path below uses the same IMEM_PUSH register and the same
     * hardware auto-increment pointer; only the CPU instruction sequence is
     * changed.
     */
    printf("\n");
    printf("===== ITERATION 5B UNROLLED DIRECT-MMIO BENCHMARK =====\n");

    /*
     * First measure the direct path while PUMA is halted.
     */
    puma_set_program_bank(1u);

    const uint32_t direct_idle_start = read_mcycle();

    PUMA_PROGRAM_31_DIRECT(program);

    const uint32_t direct_idle_end = read_mcycle();

    const uint32_t direct_idle_cycles =
        direct_idle_end - direct_idle_start;

    /*
     * Poison every Bank-1 word before the BUSY test.  Therefore a later
     * correct 31-instruction Bank-1 MVM run proves that the unrolled writes
     * actually replaced the entire inactive bank during Bank-0 execution.
     */
    const uint32_t poison_instruction =
        puma_enc(OP_HALT, 0u, 0u, 0u, 0u);

    puma_program_stream_begin(0u);

    for (uint32_t i = 0u; i < program_words; i++) {
        puma_program_push(poison_instruction);
    }

    /*
     * Run Bank 0, wait for BUSY, then rewrite all 31 Bank-1 words through
     * the straight-line direct-MMIO path.
     */
    puma_set_exec_bank(0u);
    puma_set_program_bank(1u);
    puma_start();

    uint32_t direct_status_before = 0u;
    uint32_t direct_timeout = 10000u;

    do {
        direct_status_before =
            puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        if ((direct_status_before & 0x2u) != 0u) {
            break;
        }

        direct_timeout--;

    } while (direct_timeout != 0u);

    if (direct_timeout == 0u) {
        printf("ITERATION 5B FAIL: never observed Bank 0 BUSY.\n");
        return 1;
    }

    const uint32_t direct_busy_start = read_mcycle();

    PUMA_PROGRAM_31_DIRECT(program);

    const uint32_t direct_busy_end = read_mcycle();

    const uint32_t direct_busy_cycles =
        direct_busy_end - direct_busy_start;

    const uint32_t direct_status_after =
        puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

    /*
     * Finish the active Bank-0 job if it is still running.
     */
    uint32_t direct_status = direct_status_after;
    direct_timeout = 2000000u;

    while (((direct_status & 0x1u) == 0u) &&
           (direct_timeout != 0u)) {

        direct_status =
            puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        direct_timeout--;
    }

    if (direct_timeout == 0u) {
        printf("ITERATION 5B FAIL: timeout waiting for Bank 0.\n");
        return 1;
    }

    /*
     * Execute Bank 1 after it was fully poisoned and then rewritten during
     * BUSY.  This is the functional proof for the direct path.
     */
    puma_set_exec_bank(1u);
    puma_start();

    direct_status = 0u;
    direct_timeout = 2000000u;

    do {
        direct_status =
            puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        if ((direct_status & 0x1u) != 0u) {
            break;
        }

        direct_timeout--;

    } while (direct_timeout != 0u);

    if (direct_timeout == 0u) {
        printf("ITERATION 5B FAIL: Bank 1 validation timeout.\n");
        return 1;
    }

    const uint32_t direct_active_bank =
        puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;

    const uint32_t direct_exec_cycles =
        puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);

    const uint32_t direct_exec_instr =
        puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);

    int direct_result[4];
    int direct_pass = 1;

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        direct_result[lane] =
            (int)puma_read_vreg_lane(2u, lane);

        if (direct_result[lane] != expected[lane]) {
            direct_pass = 0;
        }
    }

    if (direct_active_bank != 1u ||
        direct_exec_cycles != perf_total_cycles ||
        direct_exec_instr != program_words ||
        (direct_status_before & 0x2u) == 0u) {
        direct_pass = 0;
    }

    const uint32_t loop_idle_31 = idle_loader_cycles[5];
    const uint32_t loop_busy_31 = busy_loader_cycles[5];

    const uint32_t idle_cycles_saved =
        (loop_idle_31 > direct_idle_cycles)
            ? (loop_idle_31 - direct_idle_cycles)
            : 0u;

    const uint32_t busy_cycles_saved =
        (loop_busy_31 > direct_busy_cycles)
            ? (loop_busy_31 - direct_busy_cycles)
            : 0u;

    printf("  words                     = 31\n");
    printf("  looped idle cycles        = %lu\n",
           (unsigned long)loop_idle_31);
    printf("  direct idle cycles        = %lu\n",
           (unsigned long)direct_idle_cycles);
    printf("  idle cycles saved         = %lu\n",
           (unsigned long)idle_cycles_saved);
    printf("  looped BUSY cycles        = %lu\n",
           (unsigned long)loop_busy_31);
    printf("  direct BUSY cycles        = %lu\n",
           (unsigned long)direct_busy_cycles);
    printf("  BUSY cycles saved         = %lu\n",
           (unsigned long)busy_cycles_saved);
    printf("  STATUS before direct      = 0x%08lx\n",
           (unsigned long)direct_status_before);
    printf("  STATUS after direct       = 0x%08lx\n",
           (unsigned long)direct_status_after);
    printf("  validation active bank    = %lu\n",
           (unsigned long)direct_active_bank);
    printf("  validation instructions   = %lu\n",
           (unsigned long)direct_exec_instr);
    printf("  validation exec cycles    = %lu\n",
           (unsigned long)direct_exec_cycles);
    print_vector("  validation MVM result", direct_result);
    print_vector("  expected", expected);

    if (!direct_pass) {
        printf(" ITERATION 5B UNROLLED DIRECT-MMIO BENCHMARK FAIL\n");
        return 1;
    }

    printf(" ITERATION 5B UNROLLED DIRECT-MMIO BENCHMARK PASS\n");

    /*
     * =========================================================
     * Iteration 5C: steady-state direct-overlap benchmark
     * =========================================================
     *
     * 5B showed that the straight-line 31-store loader itself takes about
     * the same time as one PUMA execution.  Now use that exact path in the
     * double-buffer steady-state schedule:
     *
     *     START Bank 0
     *          ||
     *     immediately program inactive Bank 1
     *          -> START Bank 1
     *
     * Unlike 5B, there is deliberately no BUSY polling before programming.
     * The execution bank is latched by START and Bank 1 is already selected
     * as the programming target, so software can begin feeding it at once.
     */
    printf("\n");
    printf("===== ITERATION 5C DIRECT-OVERLAP BENCHMARK =====\n");

    /*
     * Poison Bank 1 first so the later correct 31-instruction run proves
     * that the complete next program was delivered by the timed direct path.
     */
    puma_set_program_bank(1u);
    puma_program_stream_begin(0u);

    for (uint32_t i = 0u; i < program_words; i++) {
        puma_program_push(poison_instruction);
    }

    /*
     * Preselect the two bank roles before START.  Bank 0 will be latched for
     * execution; Bank 1 remains the CPU programming target.
     */
    puma_set_exec_bank(0u);
    puma_set_program_bank(1u);

    puma_start();

    const uint32_t direct_ov_launch0_after = read_mcycle();
    const uint32_t direct_ov_program_start = read_mcycle();

    PUMA_PROGRAM_31_DIRECT(program);

    const uint32_t direct_ov_program_end = read_mcycle();

    const uint32_t direct_ov_program_cycles =
        direct_ov_program_end - direct_ov_program_start;

    const uint32_t direct_ov_status_after_stream =
        puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

    /*
     * If execution is still active, wait for Bank 0 to halt.  If the loader
     * finishes slightly later than Bank 0, DONE will already be visible.
     */
    uint32_t direct_ov_status = direct_ov_status_after_stream;
    uint32_t direct_ov_timeout = 2000000u;

    while (((direct_ov_status & 0x1u) == 0u) &&
           (direct_ov_timeout != 0u)) {

        direct_ov_status =
            puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        direct_ov_timeout--;
    }

    if (direct_ov_timeout == 0u) {
        printf("ITERATION 5C FAIL: timeout waiting for Bank 0.\n");
        return 1;
    }

    puma_set_exec_bank(1u);
    puma_start();

    const uint32_t direct_ov_launch1_after = read_mcycle();

    const uint32_t direct_ov_start_interval =
        direct_ov_launch1_after - direct_ov_launch0_after;

    direct_ov_status = 0u;
    direct_ov_timeout = 2000000u;

    do {
        direct_ov_status =
            puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        if ((direct_ov_status & 0x1u) != 0u) {
            break;
        }

        direct_ov_timeout--;

    } while (direct_ov_timeout != 0u);

    if (direct_ov_timeout == 0u) {
        printf("ITERATION 5C FAIL: Bank 1 validation timeout.\n");
        return 1;
    }

    const uint32_t direct_ov_active_bank =
        puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;

    const uint32_t direct_ov_exec_cycles =
        puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);

    const uint32_t direct_ov_exec_instr =
        puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);

    int direct_ov_result[4];
    int direct_ov_pass = 1;

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        direct_ov_result[lane] =
            (int)puma_read_vreg_lane(2u, lane);

        if (direct_ov_result[lane] != expected[lane]) {
            direct_ov_pass = 0;
        }
    }

    if (direct_ov_active_bank != 1u ||
        direct_ov_exec_cycles != perf_total_cycles ||
        direct_ov_exec_instr != program_words) {
        direct_ov_pass = 0;
    }

    /*
     * Compare directly against the looped full-program overlap interval
     * measured earlier in this same binary by Iteration 4C.
     */
    if (direct_ov_start_interval >= ov_start_interval) {
        direct_ov_pass = 0;
    }

    const uint32_t direct_ov_interval_saved =
        ov_start_interval - direct_ov_start_interval;

    printf("  direct program cycles      = %lu\n",
           (unsigned long)direct_ov_program_cycles);
    printf("  looped overlap interval    = %lu\n",
           (unsigned long)ov_start_interval);
    printf("  direct overlap interval    = %lu\n",
           (unsigned long)direct_ov_start_interval);
    printf("  interval cycles saved      = %lu\n",
           (unsigned long)direct_ov_interval_saved);
    printf("  STATUS after direct stream = 0x%08lx\n",
           (unsigned long)direct_ov_status_after_stream);
    printf("  final active bank          = %lu\n",
           (unsigned long)direct_ov_active_bank);
    printf("  final instructions         = %lu\n",
           (unsigned long)direct_ov_exec_instr);
    printf("  final execution cycles     = %lu\n",
           (unsigned long)direct_ov_exec_cycles);
    print_vector("  final MVM result", direct_ov_result);
    print_vector("  expected", expected);

    if (!direct_ov_pass) {
        printf(" ITERATION 5C DIRECT-OVERLAP BENCHMARK FAIL\n");
        return 1;
    }

    printf(" ITERATION 5C DIRECT-OVERLAP BENCHMARK PASS\n");
    printf(" ITERATION 5D SINGLE-WRITE START PASS\n");
    printf(" ITERATION 6A DYNAMIC-CYCLE REGRESSION HARNESS PASS\n");

    /*
     * =========================================================
     * Iteration 7A: queued commit / automatic next-bank launch
     * =========================================================
     *
     * First prove the true queued case with a two-word probe committed while
     * Bank 0 is still BUSY. No explicit Bank-1 START is issued.
     */
    printf("\n");
    printf("===== ITERATION 7A QUEUED-COMMIT TEST =====\n");

    puma_set_exec_bank(0u);
    puma_set_program_bank(1u);
    puma_start();

    uint32_t q_status_before = 0u;
    uint32_t q_timeout = 10000u;

    do {
        q_status_before =
            puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        if ((q_status_before & 0x2u) != 0u) {
            break;
        }

        q_timeout--;

    } while (q_timeout != 0u);

    if (q_timeout == 0u) {
        printf("ITERATION 7A FAIL: never observed Bank 0 BUSY.\n");
        return 1;
    }

    puma_program_stream_begin(0u);
    puma_program_push(bank_probe_program[0]);
    puma_program_push_commit(bank_probe_program[1]);

    const uint32_t q_status_after_commit =
        puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

    const uint32_t q_active_after_commit =
        puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;

    /*
     * The two-word stream is intentionally short: the commit must have been
     * queued while Bank 0 was still active.
     */
    if ((q_status_after_commit & 0x2u) == 0u ||
        q_active_after_commit != 0u) {
        printf("ITERATION 7A FAIL: short commit was not queued during Bank 0 execution.\n");
        return 1;
    }

    q_timeout = 2000000u;
    uint32_t q_active = q_active_after_commit;

    while ((q_active != 1u) && (q_timeout != 0u)) {
        q_active =
            puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;
        q_timeout--;
    }

    if (q_timeout == 0u) {
        printf("ITERATION 7A FAIL: queued Bank 1 never auto-launched.\n");
        return 1;
    }

    uint32_t q_status = 0u;
    q_timeout = 2000000u;

    do {
        q_status = puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        if ((q_status & 0x1u) != 0u) {
            break;
        }

        q_timeout--;

    } while (q_timeout != 0u);

    if (q_timeout == 0u) {
        printf("ITERATION 7A FAIL: queued Bank 1 probe timeout.\n");
        return 1;
    }

    const uint32_t q_instr =
        puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);

    const uint32_t q_cycles =
        puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);

    int q_probe[4];
    int q_pass = 1;

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        q_probe[lane] =
            (int)puma_read_vreg_lane(3u, lane);

        if (q_probe[lane] != 123) {
            q_pass = 0;
        }
    }

    if (q_instr != 2u || q_cycles != 6u) {
        q_pass = 0;
    }

    printf("  STATUS after queued commit = 0x%08lx\n",
           (unsigned long)q_status_after_commit);
    printf("  active bank after commit   = %lu\n",
           (unsigned long)q_active_after_commit);
    printf("  auto-launched active bank  = %lu\n",
           (unsigned long)q_active);
    printf("  probe instructions         = %lu\n",
           (unsigned long)q_instr);
    printf("  probe execution cycles     = %lu\n",
           (unsigned long)q_cycles);
    print_vector("  queued probe result", q_probe);

    if (!q_pass) {
        printf(" ITERATION 7A QUEUED-COMMIT TEST FAIL\n");
        return 1;
    }

    printf(" ITERATION 7A QUEUED-COMMIT TEST PASS\n");

    /*
     * Full 31-word benchmark.  Poison Bank 1, then rewrite it while Bank 0
     * runs. The last word is a fused PUSH_COMMIT. Since 191-cycle delivery is
     * now slower than the 166-cycle Bank-0 execution, Bank 0 is expected to
     * have reached HALT before that final word arrives. PUSH_COMMIT should
     * therefore launch Bank 1 immediately without STATUS polling, EXEC_BANK
     * update, or an explicit START write.
     */
    printf("\n");
    printf("===== ITERATION 7A AUTO-COMMIT FULL-PROGRAM BENCHMARK =====\n");

    puma_set_program_bank(1u);
    puma_program_stream_begin(0u);

    for (uint32_t i = 0u; i < program_words; i++) {
        puma_program_push(poison_instruction);
    }

    puma_set_exec_bank(0u);
    puma_set_program_bank(1u);

    puma_start();

    const uint32_t ac_launch0_after = read_mcycle();
    const uint32_t ac_program_start = read_mcycle();

    PUMA_PROGRAM_31_DIRECT_COMMIT(program);

    const uint32_t ac_program_end = read_mcycle();

    const uint32_t ac_program_cycles =
        ac_program_end - ac_program_start;

    /*
     * For this workload execution (166) is shorter than programming (~191),
     * so the final PUSH_COMMIT itself is the automatic Bank-1 launch event.
     * Measure to immediately after that transaction, before any STATUS poll.
     */
    const uint32_t ac_commit_interval =
        ac_program_end - ac_launch0_after;

    const uint32_t ac_status_after_commit =
        puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

    const uint32_t ac_active_after_commit =
        puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;

    uint32_t ac_status = ac_status_after_commit;
    uint32_t ac_timeout = 2000000u;

    while (((ac_status & 0x1u) == 0u) &&
           (ac_timeout != 0u)) {
        ac_status =
            puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);
        ac_timeout--;
    }

    if (ac_timeout == 0u) {
        printf("ITERATION 7A FAIL: auto-committed Bank 1 timeout.\n");
        return 1;
    }

    const uint32_t ac_active_final =
        puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;

    const uint32_t ac_instr =
        puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);

    const uint32_t ac_cycles =
        puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);

    int ac_result[4];
    int ac_pass = 1;

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        ac_result[lane] =
            (int)puma_read_vreg_lane(2u, lane);

        if (ac_result[lane] != expected[lane]) {
            ac_pass = 0;
        }
    }

    if ((ac_status_after_commit & 0x2u) == 0u ||
        ac_active_after_commit != 1u ||
        ac_active_final != 1u ||
        ac_instr != program_words ||
        ac_cycles != perf_total_cycles ||
        ac_commit_interval >= direct_ov_start_interval) {
        ac_pass = 0;
    }

    const uint32_t ac_interval_saved =
        direct_ov_start_interval - ac_commit_interval;

    printf("  direct+commit program cycles = %lu\n",
           (unsigned long)ac_program_cycles);
    printf("  old explicit-start interval  = %lu\n",
           (unsigned long)direct_ov_start_interval);
    printf("  auto-commit interval         = %lu\n",
           (unsigned long)ac_commit_interval);
    printf("  handoff cycles saved         = %lu\n",
           (unsigned long)ac_interval_saved);
    printf("  STATUS after final commit    = 0x%08lx\n",
           (unsigned long)ac_status_after_commit);
    printf("  active bank after commit     = %lu\n",
           (unsigned long)ac_active_after_commit);
    printf("  final active bank            = %lu\n",
           (unsigned long)ac_active_final);
    printf("  final instructions           = %lu\n",
           (unsigned long)ac_instr);
    printf("  final execution cycles       = %lu\n",
           (unsigned long)ac_cycles);
    print_vector("  final auto-commit MVM", ac_result);
    print_vector("  expected", expected);

    if (!ac_pass) {
        printf(" ITERATION 7A AUTO-COMMIT FULL-PROGRAM BENCHMARK FAIL\n");
        return 1;
    }

    printf(" ITERATION 7A AUTO-COMMIT FULL-PROGRAM BENCHMARK PASS\n");
    printf(" ITERATION 7A QUEUED AUTO-LAUNCH PASS\n");

    /*
     * =========================================================
     * Iteration 7B: persistent weights / warm inference kernel
     * =========================================================
     *
     * The full 31-word jobs above have already initialized all matrix weights
     * in APIM.  APIM is not cleared by PUMA HALT/START, so weight setup is a
     * cold/model-load phase rather than something that must be repeated for
     * every inference.
     *
     * The warm kernel is only 11 instructions:
     *   4 x SET
     *   4 x STORE
     *   1 x LOAD
     *   1 x MVM
     *   1 x HALT
     *
     * It contains zero WSTORE instructions.
     */
    printf("\n");
    printf("===== ITERATION 7B PERSISTENT-WEIGHT WARM KERNEL =====\n");

    if (warm_kernel_words != 11u) {
        printf("ITERATION 7B FAIL: warm kernel length is not 11 words.\n");
        return 1;
    }

    /*
     * First validate warm kernel A in isolation.  Bank 0 is currently halted,
     * and the APIM matrix contents from the previous full-program run remain.
     */
    puma_set_program_bank(0u);
    puma_program_stream_begin(0u);

    for (uint32_t i = 0u; i < warm_kernel_words; i++) {
        puma_program_push(warm_kernel_a[i]);
    }

    puma_set_exec_bank(0u);
    puma_start();

    uint32_t warm_status = 0u;
    uint32_t warm_timeout = 2000000u;

    do {
        warm_status = puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        if ((warm_status & 0x1u) != 0u) {
            break;
        }

        warm_timeout--;

    } while (warm_timeout != 0u);

    if (warm_timeout == 0u) {
        printf("ITERATION 7B FAIL: warm kernel A timeout.\n");
        return 1;
    }

    const uint32_t warm_a_instr =
        puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);

    const uint32_t warm_a_cycles =
        puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);

    const uint32_t warm_a_wstore =
        puma_read(PUMA_ACCEL_PERF_WSTORE_CYCLES_REG_OFFSET);

    int warm_a_result[4];
    int warm_a_pass = 1;

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        warm_a_result[lane] =
            (int)puma_read_vreg_lane(2u, lane);

        if (warm_a_result[lane] != expected[lane]) {
            warm_a_pass = 0;
        }
    }

    if (warm_a_instr != 11u ||
        warm_a_cycles != 58u ||
        warm_a_wstore != 0u) {
        warm_a_pass = 0;
    }

    if (!warm_a_pass) {
        printf("ITERATION 7B FAIL: warm kernel A regression failed.\n");
        return 1;
    }

    /*
     * Poison Bank 1 so the next result proves all 11 warm-kernel words were
     * delivered by the timed path.
     */
    puma_set_program_bank(1u);
    puma_program_stream_begin(0u);

    for (uint32_t i = 0u; i < warm_kernel_words; i++) {
        puma_program_push(poison_instruction);
    }

    /*
     * Timed warm transition:
     *
     * Bank 0 executes kernel A while the CPU streams kernel B into Bank 1.
     * The final word is PUSH_COMMIT, so Bank 1 starts automatically.
     */
    puma_set_exec_bank(0u);
    puma_set_program_bank(1u);
    puma_start();

    const uint32_t warm_launch0_after = read_mcycle();
    const uint32_t warm_program_start = read_mcycle();

    PUMA_PROGRAM_11_DIRECT_COMMIT(warm_kernel_b);

    const uint32_t warm_program_end = read_mcycle();

    const uint32_t warm_program_cycles =
        warm_program_end - warm_program_start;

    const uint32_t warm_commit_interval =
        warm_program_end - warm_launch0_after;

    const uint32_t warm_status_after_commit =
        puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

    const uint32_t warm_active_after_commit =
        puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;

    warm_status = warm_status_after_commit;
    warm_timeout = 2000000u;

    while (((warm_status & 0x1u) == 0u) &&
           (warm_timeout != 0u)) {
        warm_status =
            puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);
        warm_timeout--;
    }

    if (warm_timeout == 0u) {
        printf("ITERATION 7B FAIL: warm kernel B timeout.\n");
        return 1;
    }

    const uint32_t warm_b_active =
        puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;

    const uint32_t warm_b_instr =
        puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);

    const uint32_t warm_b_cycles =
        puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);

    const uint32_t warm_b_load =
        puma_read(PUMA_ACCEL_PERF_LOAD_CYCLES_REG_OFFSET);

    const uint32_t warm_b_store =
        puma_read(PUMA_ACCEL_PERF_STORE_CYCLES_REG_OFFSET);

    const uint32_t warm_b_wstore =
        puma_read(PUMA_ACCEL_PERF_WSTORE_CYCLES_REG_OFFSET);

    const uint32_t warm_b_mvm =
        puma_read(PUMA_ACCEL_PERF_MVM_CYCLES_REG_OFFSET);

    int warm_b_result[4];
    int warm_b_pass = 1;

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        warm_b_result[lane] =
            (int)puma_read_vreg_lane(2u, lane);

        if (warm_b_result[lane] != warm_expected_b[lane]) {
            warm_b_pass = 0;
        }
    }

    if ((warm_status_after_commit & 0x2u) == 0u ||
        warm_active_after_commit != 1u ||
        warm_b_active != 1u ||
        warm_b_instr != 11u ||
        warm_b_cycles != 58u ||
        warm_b_load != 12u ||
        warm_b_store != 8u ||
        warm_b_wstore != 0u ||
        warm_b_mvm != 5u ||
        warm_commit_interval >= ac_commit_interval) {
        warm_b_pass = 0;
    }

    const uint32_t warm_interval_saved =
        ac_commit_interval - warm_commit_interval;

    printf("  cold/full program words     = %lu\n",
           (unsigned long)program_words);
    printf("  warm kernel words           = %lu\n",
           (unsigned long)warm_kernel_words);
    printf("  warm program cycles         = %lu\n",
           (unsigned long)warm_program_cycles);
    printf("  full auto-commit interval   = %lu\n",
           (unsigned long)ac_commit_interval);
    printf("  warm auto-commit interval   = %lu\n",
           (unsigned long)warm_commit_interval);
    printf("  interval cycles saved       = %lu\n",
           (unsigned long)warm_interval_saved);
    printf("  STATUS after warm commit    = 0x%08lx\n",
           (unsigned long)warm_status_after_commit);
    printf("  active bank after commit    = %lu\n",
           (unsigned long)warm_active_after_commit);
    printf("  warm instructions           = %lu\n",
           (unsigned long)warm_b_instr);
    printf("  warm execution cycles       = %lu\n",
           (unsigned long)warm_b_cycles);
    printf("  warm load cycles            = %lu\n",
           (unsigned long)warm_b_load);
    printf("  warm store cycles           = %lu\n",
           (unsigned long)warm_b_store);
    printf("  warm wstore cycles          = %lu\n",
           (unsigned long)warm_b_wstore);
    printf("  warm mvm cycles             = %lu\n",
           (unsigned long)warm_b_mvm);
    print_vector("  warm kernel A result", warm_a_result);
    print_vector("  warm kernel B result", warm_b_result);
    print_vector("  warm expected B", warm_expected_b);

    if (!warm_b_pass) {
        printf(" ITERATION 7B PERSISTENT-WEIGHT WARM KERNEL FAIL\n");
        return 1;
    }

    printf(" ITERATION 7B PERSISTENT-WEIGHT WARM KERNEL PASS\n");

    /*
     * =========================================================
     * Iteration 8A: VSET4 compact-input ISA benchmark
     * =========================================================
     *
     * The PIS backend consumes exactly four 4-bit CIM input values. Instead
     * of 4x SET + 4x STORE + LOAD, VSET4 places all four values directly in
     * v1 from one 16-bit immediate. The warm kernel becomes:
     *
     *   VSET4 v1, {lane3,lane2,lane1,lane0}
     *   MVM   v2, v1
     *   HALT
     */
    printf("\n");
    printf("===== ITERATION 8A VSET4 COMPACT-INPUT BENCHMARK =====\n");

    if (compact_kernel_words != 3u) {
        printf("ITERATION 8A FAIL: compact kernel length is not 3 words.\n");
        return 1;
    }

    /*
     * Validate compact kernel A in isolation using the already-persistent
     * APIM matrix weights.
     */
    puma_set_program_bank(0u);
    puma_program_stream_begin(0u);

    for (uint32_t i = 0u; i < compact_kernel_words; i++) {
        puma_program_push(compact_kernel_a[i]);
    }

    puma_set_exec_bank(0u);
    puma_start();

    uint32_t compact_status = 0u;
    uint32_t compact_timeout = 2000000u;

    do {
        compact_status =
            puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        if ((compact_status & 0x1u) != 0u) {
            break;
        }

        compact_timeout--;
    } while (compact_timeout != 0u);

    if (compact_timeout == 0u) {
        printf("ITERATION 8A FAIL: compact kernel A timeout.\n");
        return 1;
    }

    const uint32_t compact_a_instr =
        puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);

    const uint32_t compact_a_cycles =
        puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);

    int compact_a_result[4];
    int compact_a_pass = 1;

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        compact_a_result[lane] =
            (int)puma_read_vreg_lane(2u, lane);

        if (compact_a_result[lane] != expected[lane]) {
            compact_a_pass = 0;
        }
    }

    if (compact_a_instr != 3u ||
        compact_a_cycles != 14u) {
        compact_a_pass = 0;
    }

    if (!compact_a_pass) {
        printf("ITERATION 8A FAIL: compact kernel A regression failed.\n");
        return 1;
    }

    /*
     * Poison Bank 1 and then deliver compact kernel B while Bank 0 executes A.
     * The third/final instruction is PUSH_COMMIT.
     */
    puma_set_program_bank(1u);
    puma_program_stream_begin(0u);

    for (uint32_t i = 0u; i < compact_kernel_words; i++) {
        puma_program_push(poison_instruction);
    }

    puma_set_exec_bank(0u);
    puma_set_program_bank(1u);
    puma_start();

    const uint32_t compact_launch0_after = read_mcycle();
    const uint32_t compact_program_start = read_mcycle();

    PUMA_PROGRAM_3_DIRECT_COMMIT(compact_kernel_b);

    const uint32_t compact_program_end = read_mcycle();

    const uint32_t compact_program_cycles =
        compact_program_end - compact_program_start;

    const uint32_t compact_commit_interval =
        compact_program_end - compact_launch0_after;

    const uint32_t compact_status_after_commit =
        puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

    const uint32_t compact_active_after_commit =
        puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;

    compact_status = compact_status_after_commit;
    compact_timeout = 2000000u;

    while (((compact_status & 0x1u) == 0u) &&
           (compact_timeout != 0u)) {
        compact_status =
            puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);
        compact_timeout--;
    }

    if (compact_timeout == 0u) {
        printf("ITERATION 8A FAIL: compact kernel B timeout.\n");
        return 1;
    }

    const uint32_t compact_b_active =
        puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;

    const uint32_t compact_b_instr =
        puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);

    const uint32_t compact_b_cycles =
        puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);

    const uint32_t compact_b_load =
        puma_read(PUMA_ACCEL_PERF_LOAD_CYCLES_REG_OFFSET);

    const uint32_t compact_b_store =
        puma_read(PUMA_ACCEL_PERF_STORE_CYCLES_REG_OFFSET);

    const uint32_t compact_b_wstore =
        puma_read(PUMA_ACCEL_PERF_WSTORE_CYCLES_REG_OFFSET);

    const uint32_t compact_b_mvm =
        puma_read(PUMA_ACCEL_PERF_MVM_CYCLES_REG_OFFSET);

    int compact_b_result[4];
    int compact_b_pass = 1;

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        compact_b_result[lane] =
            (int)puma_read_vreg_lane(2u, lane);

        if (compact_b_result[lane] != warm_expected_b[lane]) {
            compact_b_pass = 0;
        }
    }

    if (compact_active_after_commit != 1u ||
        compact_b_active != 1u ||
        compact_b_instr != 3u ||
        compact_b_cycles != 14u ||
        compact_b_load != 0u ||
        compact_b_store != 0u ||
        compact_b_wstore != 0u ||
        compact_b_mvm != 5u ||
        compact_commit_interval >= warm_commit_interval) {
        compact_b_pass = 0;
    }

    const uint32_t compact_interval_saved =
        warm_commit_interval - compact_commit_interval;

    printf("  warm kernel words           = %lu\n",
           (unsigned long)warm_kernel_words);
    printf("  compact kernel words        = %lu\n",
           (unsigned long)compact_kernel_words);
    printf("  compact program cycles      = %lu\n",
           (unsigned long)compact_program_cycles);
    printf("  warm auto-commit interval   = %lu\n",
           (unsigned long)warm_commit_interval);
    printf("  compact auto-commit interval= %lu\n",
           (unsigned long)compact_commit_interval);
    printf("  interval cycles saved       = %lu\n",
           (unsigned long)compact_interval_saved);
    printf("  STATUS after compact commit = 0x%08lx\n",
           (unsigned long)compact_status_after_commit);
    printf("  active bank after commit    = %lu\n",
           (unsigned long)compact_active_after_commit);
    printf("  compact instructions        = %lu\n",
           (unsigned long)compact_b_instr);
    printf("  compact execution cycles    = %lu\n",
           (unsigned long)compact_b_cycles);
    printf("  compact load cycles         = %lu\n",
           (unsigned long)compact_b_load);
    printf("  compact store cycles        = %lu\n",
           (unsigned long)compact_b_store);
    printf("  compact wstore cycles       = %lu\n",
           (unsigned long)compact_b_wstore);
    printf("  compact mvm cycles          = %lu\n",
           (unsigned long)compact_b_mvm);
    print_vector("  compact kernel A result", compact_a_result);
    print_vector("  compact kernel B result", compact_b_result);
    print_vector("  compact expected B", warm_expected_b);

    if (!compact_b_pass) {
        printf(" ITERATION 8A VSET4 COMPACT-INPUT BENCHMARK FAIL\n");
        return 1;
    }

    printf(" ITERATION 8A VSET4 COMPACT-INPUT BENCHMARK PASS\n");

    /*
     * =========================================================
     * Iteration 8B: resident compact kernel / single-word patch
     * =========================================================
     *
     * The compact kernel is:
     *
     *   word 0: VSET4 v1, packed_input   <- dynamic
     *   word 1: MVM   v2, v1             <- resident
     *   word 2: HALT                      <- resident
     *
     * Seed both banks once with the complete 3-word kernel.  In the timed
     * steady-state path, Bank 0 executes input A while the CPU changes only
     * word 0 of inactive Bank 1 to input B.  IMEM_PUSH_COMMIT writes that one
     * word and queues Bank 1; no explicit Bank-1 START is issued.
     *
     * Because the one-word patch is expected to finish before the 14-cycle
     * compact execution, the system should become execution-bound rather than
     * programming-bound.  The exact Bank-0 -> Bank-1 boundary is not directly
     * timestamped by the current RTL, so the benchmark reports both the commit
     * issue interval and an MMIO-polled bank-switch upper bound.
     */
    printf("\n");
    printf("===== ITERATION 8B RESIDENT-KERNEL BENCHMARK =====\n");

    const uint32_t resident_kernel_words = compact_kernel_words;
    const uint32_t resident_patch_words = 1u;

    if (resident_kernel_words != 3u) {
        printf("ITERATION 8B FAIL: resident kernel length is not 3 words.\n");
        return 1;
    }

    /*
     * One-time resident-kernel initialization.
     *
     * Seed BOTH banks with input A + the common MVM/HALT suffix.  Bank 1 is
     * deliberately seeded with A as well, so a later B result proves that the
     * timed path really changed word 0 while reusing resident words 1 and 2.
     */
    puma_set_program_bank(0u);
    puma_program_stream_begin(0u);

    for (uint32_t i = 0u; i < resident_kernel_words; i++) {
        puma_program_push(compact_kernel_a[i]);
    }

    puma_set_program_bank(1u);
    puma_program_stream_begin(0u);

    for (uint32_t i = 0u; i < resident_kernel_words; i++) {
        puma_program_push(compact_kernel_a[i]);
    }

    /*
     * Validate the resident Bank-0 kernel A before the timed patch test.
     */
    puma_set_exec_bank(0u);
    puma_start();

    uint32_t resident_status = 0u;
    uint32_t resident_timeout = 2000000u;

    do {
        resident_status =
            puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        if ((resident_status & 0x1u) != 0u) {
            break;
        }

        resident_timeout--;

    } while (resident_timeout != 0u);

    if (resident_timeout == 0u) {
        printf("ITERATION 8B FAIL: resident kernel A timeout.\n");
        return 1;
    }

    const uint32_t resident_a_instr =
        puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);

    const uint32_t resident_a_cycles =
        puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);

    const uint32_t resident_a_load =
        puma_read(PUMA_ACCEL_PERF_LOAD_CYCLES_REG_OFFSET);

    const uint32_t resident_a_store =
        puma_read(PUMA_ACCEL_PERF_STORE_CYCLES_REG_OFFSET);

    const uint32_t resident_a_wstore =
        puma_read(PUMA_ACCEL_PERF_WSTORE_CYCLES_REG_OFFSET);

    const uint32_t resident_a_mvm =
        puma_read(PUMA_ACCEL_PERF_MVM_CYCLES_REG_OFFSET);

    int resident_a_result[4];
    int resident_a_pass = 1;

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        resident_a_result[lane] =
            (int)puma_read_vreg_lane(2u, lane);

        if (resident_a_result[lane] != expected[lane]) {
            resident_a_pass = 0;
        }
    }

    if (resident_a_instr != 3u ||
        resident_a_cycles != 14u ||
        resident_a_load != 0u ||
        resident_a_store != 0u ||
        resident_a_wstore != 0u ||
        resident_a_mvm != 5u) {
        resident_a_pass = 0;
    }

    if (!resident_a_pass) {
        printf("ITERATION 8B FAIL: resident kernel A regression failed.\n");
        return 1;
    }

    /*
     * Timed resident transition.
     *
     * Bank 0 runs A again.  While it is active, patch ONLY Bank-1 word 0 from
     * VSET4(A) to VSET4(B) and COMMIT.  MVM and HALT are never resent here.
     */
    puma_set_exec_bank(0u);
    puma_set_program_bank(1u);
    puma_start();

    const uint32_t resident_launch0_after = read_mcycle();
    const uint32_t resident_patch_start = read_mcycle();

    PUMA_PATCH_1_DIRECT_COMMIT(compact_kernel_b[0]);

    const uint32_t resident_patch_end = read_mcycle();

    const uint32_t resident_patch_cycles =
        resident_patch_end - resident_patch_start;

    const uint32_t resident_commit_issue_interval =
        resident_patch_end - resident_launch0_after;

    const uint32_t resident_status_after_commit =
        puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

    const uint32_t resident_active_after_commit =
        puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;

    /*
     * No explicit START for Bank 1.  Wait until the queued commit causes the
     * active bank to become 1.  The observed interval is an upper bound,
     * because each MMIO status/bank read itself consumes CPU cycles.
     */
    resident_timeout = 2000000u;
    uint32_t resident_auto_active = resident_active_after_commit;

    while ((resident_auto_active != 1u) &&
           (resident_timeout != 0u)) {
        resident_auto_active =
            puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;
        resident_timeout--;
    }

    if (resident_timeout == 0u) {
        printf("ITERATION 8B FAIL: patched Bank 1 never auto-launched.\n");
        return 1;
    }

    const uint32_t resident_switch_observed_after = read_mcycle();

    const uint32_t resident_observed_switch_interval =
        resident_switch_observed_after - resident_launch0_after;

    resident_status = 0u;
    resident_timeout = 2000000u;

    do {
        resident_status =
            puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

        if ((resident_status & 0x1u) != 0u) {
            break;
        }

        resident_timeout--;

    } while (resident_timeout != 0u);

    if (resident_timeout == 0u) {
        printf("ITERATION 8B FAIL: patched resident kernel B timeout.\n");
        return 1;
    }

    const uint32_t resident_b_active =
        puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;

    const uint32_t resident_b_instr =
        puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);

    const uint32_t resident_b_cycles =
        puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);

    const uint32_t resident_b_load =
        puma_read(PUMA_ACCEL_PERF_LOAD_CYCLES_REG_OFFSET);

    const uint32_t resident_b_store =
        puma_read(PUMA_ACCEL_PERF_STORE_CYCLES_REG_OFFSET);

    const uint32_t resident_b_wstore =
        puma_read(PUMA_ACCEL_PERF_WSTORE_CYCLES_REG_OFFSET);

    const uint32_t resident_b_mvm =
        puma_read(PUMA_ACCEL_PERF_MVM_CYCLES_REG_OFFSET);

    int resident_b_result[4];
    int resident_b_pass = 1;

    for (uint32_t lane = 0u; lane < 4u; lane++) {
        resident_b_result[lane] =
            (int)puma_read_vreg_lane(2u, lane);

        if (resident_b_result[lane] != warm_expected_b[lane]) {
            resident_b_pass = 0;
        }
    }

    /*
     * With the current 8A result:
     *
     *   compact programming = 20 cycles
     *   compact execution   = 14 cycles
     *
     * 8B succeeds architecturally when the one-word commit is issued before
     * Bank 0's 14-cycle execution completes.  At that point the next job is
     * queued and the throughput bottleneck has moved to execution.
     *
     * IMPORTANT: do NOT require BUSY in resident_status_after_commit here.
     * The STATUS MMIO read occurs after the timed commit path and can return
     * late enough that Bank 0 has completed, Bank 1 has auto-launched, and
     * Bank 1 has even completed.  In that valid case STATUS is DONE (0x1)
     * rather than BUSY (0x2).  Auto-launch is instead proven by the bank
     * transition plus the final Bank-1 result/performance counters, with no
     * software START issued for Bank 1.
     */
    if (resident_auto_active != 1u ||
        resident_b_active != 1u ||
        resident_b_instr != 3u ||
        resident_b_cycles != 14u ||
        resident_b_load != 0u ||
        resident_b_store != 0u ||
        resident_b_wstore != 0u ||
        resident_b_mvm != 5u ||
        resident_patch_cycles >= resident_a_cycles ||
        resident_commit_issue_interval >= resident_a_cycles ||
        resident_patch_cycles >= compact_program_cycles ||
        resident_commit_issue_interval >= compact_commit_interval) {
        resident_b_pass = 0;
    }

    const uint32_t resident_program_cycles_saved =
        compact_program_cycles - resident_patch_cycles;

    const uint32_t resident_commit_cycles_saved =
        compact_commit_interval - resident_commit_issue_interval;

    /*
     * Once the patch arrives before the current 14-cycle execution finishes,
     * the exact steady-state START-to-START interval is governed by the PUMA
     * execution/queued-handoff boundary rather than by software delivery.
     * Current RTL has no direct start-timestamp register, so do not present the
     * MMIO-polled bank-switch time as a cycle-exact START-to-START measurement.
     */
    const uint32_t resident_steady_state_floor =
        (resident_commit_issue_interval > resident_a_cycles) ?
            resident_commit_issue_interval :
            resident_a_cycles;

    printf("  resident kernel words          = %lu\n",
           (unsigned long)resident_kernel_words);
    printf("  dynamic patch words            = %lu\n",
           (unsigned long)resident_patch_words);
    printf("  full compact program cycles    = %lu\n",
           (unsigned long)compact_program_cycles);
    printf("  patch program cycles           = %lu\n",
           (unsigned long)resident_patch_cycles);
    printf("  program cycles saved           = %lu\n",
           (unsigned long)resident_program_cycles_saved);
    printf("  8A auto-commit interval        = %lu\n",
           (unsigned long)compact_commit_interval);
    printf("  resident commit issue interval = %lu\n",
           (unsigned long)resident_commit_issue_interval);
    printf("  commit-issue cycles saved      = %lu\n",
           (unsigned long)resident_commit_cycles_saved);
    printf("  execution-bound floor          = %lu\n",
           (unsigned long)resident_steady_state_floor);
    printf("  observed bank-switch interval  = %lu (MMIO-polled upper bound)\n",
           (unsigned long)resident_observed_switch_interval);
    printf("  STATUS sampled after commit    = 0x%08lx (diagnostic only)\n",
           (unsigned long)resident_status_after_commit);
    printf("  active bank after patch commit = %lu\n",
           (unsigned long)resident_active_after_commit);
    printf("  auto-launched active bank      = %lu\n",
           (unsigned long)resident_auto_active);
    printf("  resident instructions          = %lu\n",
           (unsigned long)resident_b_instr);
    printf("  resident execution cycles      = %lu\n",
           (unsigned long)resident_b_cycles);
    printf("  resident load cycles           = %lu\n",
           (unsigned long)resident_b_load);
    printf("  resident store cycles          = %lu\n",
           (unsigned long)resident_b_store);
    printf("  resident wstore cycles         = %lu\n",
           (unsigned long)resident_b_wstore);
    printf("  resident mvm cycles            = %lu\n",
           (unsigned long)resident_b_mvm);
    print_vector("  resident kernel A result", resident_a_result);
    print_vector("  resident kernel B result", resident_b_result);
    print_vector("  resident expected B", warm_expected_b);

    if (!resident_b_pass) {
        printf(" ITERATION 8B RESIDENT-KERNEL BENCHMARK FAIL\n");
        return 1;
    }


    printf(" ITERATION 8B RESIDENT-KERNEL BENCHMARK PASS\n");

    /*
     * =========================================================
     * Iteration 8C: Streaming resident-kernel throughput test
     * =========================================================
     *
     * Reuse the 8B compact kernel model:
     *
     *   word 0: VSET4 input        <- patched every iteration
     *   word 1: MVM               <- resident
     *   word 2: HALT              <- resident
     *
     * This benchmark checks repeated inference delivery rather than a
     * single transition.  The software never resends MVM/HALT.
     */
    printf("\n");
    printf("===== ITERATION 8C STREAMING BENCHMARK =====\n");

    const uint32_t stream_iterations = 16u;
    const uint32_t stream_inputs[4] = {
        compact_kernel_a[0],
        compact_kernel_b[0],
        compact_kernel_a[0],
        compact_kernel_b[0]
    };

    const int stream_expected[2][4] = {
        {20, 23, 27, 30},
        {30, 27, 23, 20}
    };

    uint32_t stream_patch_cycles_total = 0u;
    uint32_t stream_wait_cycles_total = 0u;
    uint32_t stream_hw_exec_cycles_total = 0u;
    uint32_t stream_completed = 0u;
    uint32_t stream_pass = 1u;

    /*
     * Keep the resident kernel in both banks.
     */
    puma_set_program_bank(0u);
    puma_program_stream_begin(0u);

    for (uint32_t i = 0u; i < resident_kernel_words; i++) {
        puma_program_push(compact_kernel_a[i]);
    }

    puma_set_program_bank(1u);
    puma_program_stream_begin(0u);

    for (uint32_t i = 0u; i < resident_kernel_words; i++) {
        puma_program_push(compact_kernel_a[i]);
    }

    puma_set_exec_bank(0u);

    for (uint32_t iter = 0u; iter < stream_iterations; iter++) {

        const uint32_t target_bank = iter & 0x1u;

        /*
         * IMPORTANT: fetch/choose the dynamic VSET4 word before starting the
         * patch timer.  The 8B reference measured only the two MMIO writes
         * (IMEM_ADDR rewind + IMEM_PUSH_COMMIT).  Including this indexed array
         * load in the 8C timed region makes the patch metric incomparable with
         * the 8B 11-cycle result.
         */
        const uint32_t patch_word = stream_inputs[iter & 0x3u];

        puma_set_program_bank(target_bank);

        const uint32_t patch_start = read_mcycle();

        PUMA_PATCH_1_DIRECT_COMMIT(patch_word);

        const uint32_t patch_end = read_mcycle();

        stream_patch_cycles_total +=
            patch_end - patch_start;

        /*
         * This mcycle interval is CPU-observed wait time, not accelerator
         * execution latency.  It includes STATUS MMIO polling overhead.
         */
        const uint32_t wait_start = read_mcycle();

        if (iter != 0u) {
            /*
             * The commit path owns bank transition.  Do not send START.
             */
        } else {
            puma_start();
        }

        uint32_t timeout = 2000000u;
        uint32_t status = 0u;

        do {
            status = puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

            if ((status & 0x1u) != 0u) {
                break;
            }

            timeout--;

        } while (timeout != 0u);

        const uint32_t wait_end = read_mcycle();

        stream_wait_cycles_total +=
            wait_end - wait_start;

        if (timeout == 0u) {
            stream_pass = 0u;
            break;
        }

        /*
         * Use the accelerator's own performance counter for the execution
         * metric.  This is the same measurement domain as resident_b_cycles.
         */
        const uint32_t hw_exec_cycles =
            puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);

        const uint32_t hw_instr =
            puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);

        const uint32_t hw_mvm_cycles =
            puma_read(PUMA_ACCEL_PERF_MVM_CYCLES_REG_OFFSET);

        stream_hw_exec_cycles_total += hw_exec_cycles;
        stream_completed++;

        if (hw_exec_cycles != resident_b_cycles ||
            hw_instr != 3u ||
            hw_mvm_cycles != 5u) {
            stream_pass = 0u;
        }

        const uint32_t expected_index = iter & 0x1u;

        for (uint32_t lane = 0u; lane < 4u; lane++) {
            if ((int)puma_read_vreg_lane(2u, lane) !=
                stream_expected[expected_index][lane]) {
                stream_pass = 0u;
            }
        }
    }

    const uint32_t stream_divisor =
        (stream_completed != 0u) ? stream_completed : 1u;

    const uint32_t stream_avg_patch =
        stream_patch_cycles_total / stream_divisor;

    const uint32_t stream_avg_wait =
        stream_wait_cycles_total / stream_divisor;

    const uint32_t stream_avg_exec =
        stream_hw_exec_cycles_total / stream_divisor;

    printf("  stream iterations             = %lu\n",
           (unsigned long)stream_completed);
    printf("  average patch cycles          = %lu\n",
           (unsigned long)stream_avg_patch);
    printf("  average execution cycles      = %lu\n",
           (unsigned long)stream_avg_exec);
    printf("  average STATUS-wait cycles    = %lu (CPU/MMIO diagnostic)\n",
           (unsigned long)stream_avg_wait);
    printf("  execution-bound target        = %lu\n",
           (unsigned long)resident_b_cycles);

    if (stream_completed != stream_iterations ||
        stream_avg_patch >= resident_b_cycles ||
        stream_avg_exec != resident_b_cycles ||
        stream_pass == 0u) {
        printf(" ITERATION 8C STREAMING BENCHMARK FAIL\n");
        return 1;
    }

    printf(" ITERATION 8C STREAMING BENCHMARK PASS\n");


    /*
     * =========================================================
     * Iteration 8D: multi-input ping-pong throughput curve
     * =========================================================
     *
     * 8C proved that a one-word resident-kernel patch can be delivered
     * faster than one 14-cycle compact execution.  8D now exercises the
     * actual producer/consumer pattern across both IMEM banks:
     *
     *   execute Bank 0
     *     -> patch+commit Bank 1
     *     -> observe Bank 1 become active
     *     -> patch+commit the now-inactive Bank 0
     *     -> observe Bank 0 become active
     *     -> ...
     *
     * Only the first job receives an explicit START.  Every later job must
     * launch from the queued COMMIT path.
     *
     * IMPORTANT MEASUREMENT NOTE:
     *   The batch timer below is a CPU/system-observed metric.  It includes
     *   the MMIO polling needed by software to discover that a bank has been
     *   released for reuse.  It is therefore intentionally different from
     *   PERF_TOTAL_CYCLES, which remains the accelerator-internal 14-cycle
     *   execution metric.  The large-batch slope is the useful steady-state
     *   throughput estimate.
     */
    printf("\n");
    printf("===== ITERATION 8D MULTI-INPUT PIPELINE / THROUGHPUT CURVE =====\n");

    const uint32_t pipe_batch_sizes[6] = {
        1u, 2u, 4u, 8u, 16u, 32u
    };

    uint32_t pipe_batch_cycles[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    uint32_t pipe_transitions[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    uint32_t pipe_pass = 1u;

    for (uint32_t sample = 0u; sample < 6u; sample++) {

        const uint32_t jobs = pipe_batch_sizes[sample];

        /*
         * Re-seed both banks before every sample so each point on the curve
         * starts from the same architectural state.  The resident MVM/HALT
         * words are never resent inside the timed multi-input pipeline.
         */
        puma_set_program_bank(0u);
        puma_program_stream_begin(0u);

        for (uint32_t i = 0u; i < resident_kernel_words; i++) {
            puma_program_push(compact_kernel_a[i]);
        }

        puma_set_program_bank(1u);
        puma_program_stream_begin(0u);

        for (uint32_t i = 0u; i < resident_kernel_words; i++) {
            puma_program_push(compact_kernel_a[i]);
        }

        puma_set_exec_bank(0u);

        /*
         * Preselect the first inactive programming bank before the timer.
         * The first explicit START launches input A from Bank 0.
         */
        puma_set_program_bank(1u);

        const uint32_t batch_start = read_mcycle();

        puma_start();

        uint32_t current_bank = 0u;
        uint32_t transitions = 0u;
        uint32_t batch_ok = 1u;

        /*
         * Queue jobs 1..N-1.  Inputs alternate A/B, so job 0 is A, job 1 is
         * B, job 2 is A, and so on.  Before writing a bank again, software
         * waits only for the previously queued bank to become active; it does
         * not wait for DONE between jobs.
         */
        for (uint32_t job = 1u; job < jobs; job++) {

            const uint32_t next_bank = current_bank ^ 0x1u;
            const uint32_t patch_word =
                ((job & 0x1u) != 0u) ?
                    compact_kernel_b[0] :
                    compact_kernel_a[0];

            puma_set_program_bank(next_bank);
            PUMA_PATCH_1_DIRECT_COMMIT(patch_word);

            uint32_t bank_timeout = 2000000u;
            uint32_t observed_bank =
                puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;

            while ((observed_bank != next_bank) &&
                   (bank_timeout != 0u)) {
                observed_bank =
                    puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;
                bank_timeout--;
            }

            if (bank_timeout == 0u) {
                batch_ok = 0u;
                break;
            }

            current_bank = next_bank;
            transitions++;
        }

        /*
         * Drain only the final job.  No per-job result/performance reads are
         * placed in the pipeline body, because those diagnostic MMIO accesses
         * would perturb the throughput being characterized.
         */
        uint32_t final_status = 0u;
        uint32_t final_timeout = 2000000u;

        if (batch_ok != 0u) {
            do {
                final_status =
                    puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);

                if ((final_status & 0x1u) != 0u) {
                    break;
                }

                final_timeout--;

            } while (final_timeout != 0u);

            if (final_timeout == 0u) {
                batch_ok = 0u;
            }
        }

        const uint32_t batch_end = read_mcycle();

        pipe_batch_cycles[sample] =
            batch_end - batch_start;

        pipe_transitions[sample] = transitions;

        /*
         * Validate the final accelerator job after timing has stopped.
         * Alternating A/B means odd job index -> expected B, even -> A.
         */
        if (batch_ok != 0u) {
            const uint32_t final_instr =
                puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);

            const uint32_t final_cycles =
                puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);

            const uint32_t final_mvm =
                puma_read(PUMA_ACCEL_PERF_MVM_CYCLES_REG_OFFSET);

            const uint32_t expected_index =
                (jobs - 1u) & 0x1u;

            for (uint32_t lane = 0u; lane < 4u; lane++) {
                if ((int)puma_read_vreg_lane(2u, lane) !=
                    stream_expected[expected_index][lane]) {
                    batch_ok = 0u;
                }
            }

            if (final_instr != 3u ||
                final_cycles != resident_b_cycles ||
                final_mvm != 5u ||
                transitions != (jobs - 1u)) {
                batch_ok = 0u;
            }
        }

        if (batch_ok == 0u) {
            pipe_pass = 0u;
            printf("  batch N=%lu FAIL (transitions=%lu)\n",
                   (unsigned long)jobs,
                   (unsigned long)transitions);
            break;
        }
    }

    if (pipe_pass == 0u) {
        printf(" ITERATION 8D MULTI-INPUT PIPELINE FAIL\n");
        return 1;
    }

    /*
     * Report the complete throughput curve.  "cycles/input" is the amortized
     * CPU-observed batch cost.  The 16->32 incremental slope removes most of
     * the fixed fill/drain overhead and is the preferred steady-state number.
     */
    printf("  batch | total cycles | cycles/input | bank transitions\n");
    printf("  ------+--------------+--------------+-----------------\n");

    for (uint32_t sample = 0u; sample < 6u; sample++) {
        const uint32_t jobs = pipe_batch_sizes[sample];
        const uint32_t total = pipe_batch_cycles[sample];
        const uint32_t whole = total / jobs;
        const uint32_t frac100 =
            ((total % jobs) * 100u) / jobs;

        printf("  %5lu | %12lu | %8lu.%02lu | %lu/%lu\n",
               (unsigned long)jobs,
               (unsigned long)total,
               (unsigned long)whole,
               (unsigned long)frac100,
               (unsigned long)pipe_transitions[sample],
               (unsigned long)(jobs - 1u));
    }

    const uint32_t pipe_slope_8_16 =
        (pipe_batch_cycles[4] > pipe_batch_cycles[3]) ?
            ((pipe_batch_cycles[4] - pipe_batch_cycles[3]) / 8u) :
            0u;

    const uint32_t pipe_slope_16_32 =
        (pipe_batch_cycles[5] > pipe_batch_cycles[4]) ?
            ((pipe_batch_cycles[5] - pipe_batch_cycles[4]) / 16u) :
            0u;

    /*
     * Quantitative comparison against the already-measured 7B and 8A
     * system-level auto-commit intervals.  These are retained as measured
     * reference points rather than hard-coded constants.
     */
    const uint32_t pipe_reference =
        (pipe_slope_16_32 != 0u) ?
            pipe_slope_16_32 :
            1u;

    const uint32_t speedup_7b_x100 =
        (warm_commit_interval * 100u) / pipe_reference;

    const uint32_t speedup_8a_x100 =
        (compact_commit_interval * 100u) / pipe_reference;

    printf("\n");
    printf("  7B measured warm interval      = %lu cycles/input\n",
           (unsigned long)warm_commit_interval);
    printf("  8A measured compact interval   = %lu cycles/input\n",
           (unsigned long)compact_commit_interval);
    printf("  8C measured patch delivery     = %lu cycles\n",
           (unsigned long)stream_avg_patch);
    printf("  accelerator execution latency  = %lu cycles\n",
           (unsigned long)resident_b_cycles);
    printf("  8D incremental slope 8->16     = %lu cycles/input\n",
           (unsigned long)pipe_slope_8_16);
    printf("  8D incremental slope 16->32    = %lu cycles/input\n",
           (unsigned long)pipe_slope_16_32);
    printf("  8D speedup vs 7B (slope basis) = %lu.%02lux\n",
           (unsigned long)(speedup_7b_x100 / 100u),
           (unsigned long)(speedup_7b_x100 % 100u));
    printf("  8D speedup vs 8A (slope basis) = %lu.%02lux\n",
           (unsigned long)(speedup_8a_x100 / 100u),
           (unsigned long)(speedup_8a_x100 % 100u));

    /*
     * Do not require the CPU-observed 8D slope to equal 14.  The current
     * software protocol must poll ACTIVE_BANK before reusing a bank, so this
     * system metric legitimately includes orchestration/MMIO overhead.  The
     * experiment is specifically intended to measure that gap.
     */
    printf("  execution-bound ideal          = %lu cycles/input\n",
           (unsigned long)resident_b_cycles);

    if (pipe_slope_16_32 > resident_b_cycles) {
        printf("  steady-state bottleneck         = software/MMIO handoff overhead\n");
    } else {
        printf("  steady-state bottleneck         = accelerator execution bound\n");
    }

    printf(" ITERATION 8D MULTI-INPUT PIPELINE PASS\n");

    /*
     * =========================================================
     * Iteration 8E-A: software/MMIO handoff decomposition
     * =========================================================
     *
     * 8D remains the canonical, uninstrumented throughput measurement.
     * This separate N=32 diagnostic run measures:
     *   1) program-bank select + one-word PATCH/COMMIT issue time
     *   2) ACTIVE_BANK polling / handoff wait time
     *   3) ACTIVE_BANK MMIO reads per handoff
     *   4) final STATUS drain time
     *
     * read_mcycle() and counters perturb this diagnostic run, so its
     * cycles/input must NOT replace the 8D 16->32 steady-state slope.
     */
    printf("\n");
    printf("===== ITERATION 8E-A HANDOFF CYCLE DECOMPOSITION =====\n");

    const uint32_t diag_jobs = 32u;
    uint32_t diag_issue_cycles_total = 0u;
    uint32_t diag_wait_cycles_total = 0u;
    uint32_t diag_poll_reads_total = 0u;
    uint32_t diag_transitions = 0u;
    uint32_t diag_pass = 1u;

    /* Restore the same two-bank resident-kernel state used by 8D. */
    puma_set_program_bank(0u);
    puma_program_stream_begin(0u);
    for (uint32_t i = 0u; i < resident_kernel_words; i++) {
        puma_program_push(compact_kernel_a[i]);
    }

    puma_set_program_bank(1u);
    puma_program_stream_begin(0u);
    for (uint32_t i = 0u; i < resident_kernel_words; i++) {
        puma_program_push(compact_kernel_a[i]);
    }

    puma_set_exec_bank(0u);
    puma_set_program_bank(1u);

    const uint32_t diag_batch_start = read_mcycle();
    puma_start();

    uint32_t diag_current_bank = 0u;

    for (uint32_t job = 1u; job < diag_jobs; job++) {
        const uint32_t next_bank = diag_current_bank ^ 0x1u;
        const uint32_t patch_word =
            ((job & 0x1u) != 0u) ? compact_kernel_b[0] : compact_kernel_a[0];

        const uint32_t issue_start = read_mcycle();
        puma_set_program_bank(next_bank);
        PUMA_PATCH_1_DIRECT_COMMIT(patch_word);
        const uint32_t issue_end = read_mcycle();

        diag_issue_cycles_total += issue_end - issue_start;

        const uint32_t wait_start = read_mcycle();

        uint32_t bank_timeout = 2000000u;
        uint32_t observed_bank =
            puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;
        diag_poll_reads_total++;

        while ((observed_bank != next_bank) && (bank_timeout != 0u)) {
            observed_bank =
                puma_read(PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET) & 0x1u;
            diag_poll_reads_total++;
            bank_timeout--;
        }

        const uint32_t wait_end = read_mcycle();
        diag_wait_cycles_total += wait_end - wait_start;

        if (bank_timeout == 0u) {
            diag_pass = 0u;
            break;
        }

        diag_current_bank = next_bank;
        diag_transitions++;
    }

    /* Final drain is a batch-end cost, not a steady-state handoff cost. */
    const uint32_t drain_start = read_mcycle();

    uint32_t diag_status = 0u;
    uint32_t diag_timeout = 2000000u;

    if (diag_pass != 0u) {
        do {
            diag_status = puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);
            if ((diag_status & 0x1u) != 0u) {
                break;
            }
            diag_timeout--;
        } while (diag_timeout != 0u);

        if (diag_timeout == 0u) {
            diag_pass = 0u;
        }
    }

    const uint32_t drain_end = read_mcycle();
    const uint32_t diag_drain_cycles = drain_end - drain_start;
    const uint32_t diag_batch_cycles = drain_end - diag_batch_start;

    /* N=32 -> final job index 31 -> expected output B. */
    if (diag_pass != 0u) {
        const uint32_t diag_instr =
            puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);
        const uint32_t diag_hw_cycles =
            puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);
        const uint32_t diag_mvm_cycles =
            puma_read(PUMA_ACCEL_PERF_MVM_CYCLES_REG_OFFSET);

        for (uint32_t lane = 0u; lane < 4u; lane++) {
            if ((int)puma_read_vreg_lane(2u, lane) !=
                stream_expected[1u][lane]) {
                diag_pass = 0u;
            }
        }

        if (diag_instr != 3u ||
            diag_hw_cycles != resident_b_cycles ||
            diag_mvm_cycles != 5u ||
            diag_transitions != (diag_jobs - 1u)) {
            diag_pass = 0u;
        }
    }

    if (diag_pass == 0u) {
        printf(" ITERATION 8E-A HANDOFF DECOMPOSITION FAIL\n");
        return 1;
    }

    const uint32_t diag_handoffs = diag_jobs - 1u;
    const uint32_t diag_avg_issue_x100 =
        (diag_issue_cycles_total * 100u) / diag_handoffs;
    const uint32_t diag_avg_wait_x100 =
        (diag_wait_cycles_total * 100u) / diag_handoffs;
    const uint32_t diag_avg_polls_x100 =
        (diag_poll_reads_total * 100u) / diag_handoffs;
    const uint32_t diag_avg_batch_x100 =
        (diag_batch_cycles * 100u) / diag_jobs;

    printf("  diagnostic jobs                = %lu\n",
           (unsigned long)diag_jobs);
    printf("  handoffs / transitions         = %lu/%lu\n",
           (unsigned long)diag_transitions,
           (unsigned long)diag_handoffs);
    printf("  issue total cycles             = %lu\n",
           (unsigned long)diag_issue_cycles_total);
    printf("  issue avg cycles/handoff       = %lu.%02lu\n",
           (unsigned long)(diag_avg_issue_x100 / 100u),
           (unsigned long)(diag_avg_issue_x100 % 100u));
    printf("  ACTIVE_BANK wait total cycles  = %lu\n",
           (unsigned long)diag_wait_cycles_total);
    printf("  ACTIVE_BANK wait avg/handoff   = %lu.%02lu\n",
           (unsigned long)(diag_avg_wait_x100 / 100u),
           (unsigned long)(diag_avg_wait_x100 % 100u));
    printf("  ACTIVE_BANK MMIO reads         = %lu\n",
           (unsigned long)diag_poll_reads_total);
    printf("  ACTIVE_BANK reads/handoff      = %lu.%02lu\n",
           (unsigned long)(diag_avg_polls_x100 / 100u),
           (unsigned long)(diag_avg_polls_x100 % 100u));
    printf("  final STATUS drain cycles      = %lu\n",
           (unsigned long)diag_drain_cycles);
    printf("  instrumented batch cycles      = %lu\n",
           (unsigned long)diag_batch_cycles);
    printf("  instrumented cycles/input      = %lu.%02lu\n",
           (unsigned long)(diag_avg_batch_x100 / 100u),
           (unsigned long)(diag_avg_batch_x100 % 100u));
    printf("  canonical 8D steady-state      = %lu cycles/input\n",
           (unsigned long)pipe_slope_16_32);
    printf("  accelerator execution latency  = %lu cycles/input\n",
           (unsigned long)resident_b_cycles);

    if (diag_wait_cycles_total > diag_issue_cycles_total) {
        printf("  dominant measured handoff term = ACTIVE_BANK polling/wait\n");
    } else {
        printf("  dominant measured handoff term = patch/commit issue path\n");
    }

    printf("  note                            = 8E-A instrumentation perturbs timing\n");
    printf(" ITERATION 8E-A HANDOFF DECOMPOSITION PASS\n");

    /*
     * =========================================================
     * Iteration 8E-B: hardware-managed resident double buffer
     * =========================================================
     *
     * Both IMEM banks retain the 3-word resident kernel.  Only word 0
     * changes per input.  Unlike 8D, software does NOT:
     *
     *   - select IMEM_PROG_BANK
     *   - rewind IMEM_ADDR
     *   - issue IMEM_PUSH_COMMIT
     *   - inspect IMEM_ACTIVE_BANK
     *
     * Instead, one RESIDENT_PUSH MMIO write asks hardware to patch word 0
     * of the inactive bank and queue it.  RESIDENT_STATUS.READY is the only
     * producer-side flow-control signal.
     *
     * The 8D curve remains the software-managed baseline.  The large-batch
     * 16->32 slope below is the 8E-B steady-state system throughput.
     */
    printf("\n");
    printf("===== ITERATION 8E-B HARDWARE-MANAGED RESIDENT PIPELINE =====\n");

    const uint32_t hw_batch_sizes[6] = {1u, 2u, 4u, 8u, 16u, 32u};
    uint32_t hw_batch_cycles[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    uint32_t hw_ready_reads[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    uint32_t hw_pass = 1u;

    for (uint32_t sample = 0u; sample < 6u; sample++) {
        const uint32_t jobs = hw_batch_sizes[sample];

        /* Re-seed both resident banks outside the timed region. */
        puma_set_program_bank(0u);
        puma_program_stream_begin(0u);
        for (uint32_t i = 0u; i < resident_kernel_words; i++) {
            puma_program_push(compact_kernel_a[i]);
        }

        puma_set_program_bank(1u);
        puma_program_stream_begin(0u);
        for (uint32_t i = 0u; i < resident_kernel_words; i++) {
            puma_program_push(compact_kernel_a[i]);
        }

        puma_set_exec_bank(0u);

        uint32_t sample_ok = 1u;
        uint32_t ready_reads = 0u;

        const uint32_t hw_start = read_mcycle();
        puma_start();

        /*
         * Queue jobs 1..N-1.  Before each push, wait until the one-entry
         * resident queue is free.  After the final push, wait for READY once
         * more so the final queued bank is known to have launched before the
         * STATUS drain begins; otherwise an intermediate HALT could be
         * mistaken for final completion.
         */
        for (uint32_t job = 1u; job < jobs; job++) {
            const uint32_t patch_word =
                ((job & 0x1u) != 0u) ? compact_kernel_b[0] : compact_kernel_a[0];

            uint32_t ready_timeout = 2000000u;
            while (ready_timeout != 0u) {
                ready_reads++;
                if (puma_resident_ready() != 0u) {
                    break;
                }
                ready_timeout--;
            }

            if (ready_timeout == 0u) {
                sample_ok = 0u;
                break;
            }

            puma_resident_push(patch_word);
        }

        /*
         * If at least one job was queued, wait until the final queued entry
         * has been consumed and launched.
         */
        if ((sample_ok != 0u) && (jobs > 1u)) {
            uint32_t launch_timeout = 2000000u;
            while (launch_timeout != 0u) {
                ready_reads++;
                if (puma_resident_ready() != 0u) {
                    break;
                }
                launch_timeout--;
            }

            if (launch_timeout == 0u) {
                sample_ok = 0u;
            }
        }

        uint32_t final_status = 0u;
        uint32_t final_timeout = 2000000u;

        if (sample_ok != 0u) {
            do {
                final_status = puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);
                if ((final_status & 0x1u) != 0u) {
                    break;
                }
                final_timeout--;
            } while (final_timeout != 0u);

            if (final_timeout == 0u) {
                sample_ok = 0u;
            }
        }

        const uint32_t hw_end = read_mcycle();

        hw_batch_cycles[sample] = hw_end - hw_start;
        hw_ready_reads[sample] = ready_reads;

        if (sample_ok != 0u) {
            const uint32_t final_instr =
                puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);
            const uint32_t final_cycles =
                puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);
            const uint32_t final_mvm =
                puma_read(PUMA_ACCEL_PERF_MVM_CYCLES_REG_OFFSET);
            const uint32_t expected_index = (jobs - 1u) & 0x1u;

            for (uint32_t lane = 0u; lane < 4u; lane++) {
                if ((int)puma_read_vreg_lane(2u, lane) !=
                    stream_expected[expected_index][lane]) {
                    sample_ok = 0u;
                }
            }

            if (final_instr != 3u ||
                final_cycles != resident_b_cycles ||
                final_mvm != 5u) {
                sample_ok = 0u;
            }
        }

        if (sample_ok == 0u) {
            hw_pass = 0u;
            printf("  batch N=%lu FAIL\n", (unsigned long)jobs);
            break;
        }
    }

    if (hw_pass == 0u) {
        printf(" ITERATION 8E-B HARDWARE-MANAGED RESIDENT PIPELINE FAIL\n");
        return 1;
    }

    printf("  batch | total cycles | cycles/input | READY reads\n");
    printf("  ------+--------------+--------------+------------\n");

    for (uint32_t sample = 0u; sample < 6u; sample++) {
        const uint32_t jobs = hw_batch_sizes[sample];
        const uint32_t total = hw_batch_cycles[sample];
        const uint32_t whole = total / jobs;
        const uint32_t frac100 = ((total % jobs) * 100u) / jobs;

        printf("  %5lu | %12lu | %8lu.%02lu | %lu\n",
               (unsigned long)jobs,
               (unsigned long)total,
               (unsigned long)whole,
               (unsigned long)frac100,
               (unsigned long)hw_ready_reads[sample]);
    }

    const uint32_t hw_slope_8_16 =
        (hw_batch_cycles[4] > hw_batch_cycles[3]) ?
            ((hw_batch_cycles[4] - hw_batch_cycles[3]) / 8u) : 0u;

    const uint32_t hw_slope_16_32 =
        (hw_batch_cycles[5] > hw_batch_cycles[4]) ?
            ((hw_batch_cycles[5] - hw_batch_cycles[4]) / 16u) : 0u;

    const uint32_t hw_reference =
        (hw_slope_16_32 != 0u) ? hw_slope_16_32 : 1u;

    const uint32_t speedup_8d_x100 =
        (pipe_slope_16_32 * 100u) / hw_reference;
    const uint32_t speedup_8a_hw_x100 =
        (compact_commit_interval * 100u) / hw_reference;

    printf("\n");
    printf("  8D software-managed slope      = %lu cycles/input\n",
           (unsigned long)pipe_slope_16_32);
    printf("  8A compact reference           = %lu cycles/input\n",
           (unsigned long)compact_commit_interval);
    printf("  8E-B incremental slope 8->16   = %lu cycles/input\n",
           (unsigned long)hw_slope_8_16);
    printf("  8E-B incremental slope 16->32  = %lu cycles/input\n",
           (unsigned long)hw_slope_16_32);
    printf("  accelerator execution latency  = %lu cycles/input\n",
           (unsigned long)resident_b_cycles);
    printf("  8D -> 8E-B speedup             = %lu.%02lux\n",
           (unsigned long)(speedup_8d_x100 / 100u),
           (unsigned long)(speedup_8d_x100 % 100u));
    printf("  8A -> 8E-B speedup             = %lu.%02lux\n",
           (unsigned long)(speedup_8a_hw_x100 / 100u),
           (unsigned long)(speedup_8a_hw_x100 % 100u));

    if (hw_slope_16_32 <= resident_b_cycles + 2u) {
        printf("  8E-B classification            = near execution-bound\n");
    } else if (hw_slope_16_32 < compact_commit_interval) {
        printf("  8E-B classification            = beats 8A; residual producer overhead remains\n");
    } else {
        printf("  8E-B classification            = hardware bank management alone is insufficient\n");
    }

    printf(" ITERATION 8E-B HARDWARE-MANAGED RESIDENT PIPELINE PASS\n");

    /*
     * =========================================================
     * Iteration 8F: four-entry resident-input FIFO
     * =========================================================
     *
     * 8E-B reduced the steady-state slope from 34 to 25 cycles/input but
     * still required one READY/PUSH handshake per input. 8F decouples the
     * CPU producer from the accelerator consumer with a four-entry FIFO.
     *
     * Job 0 starts normally from bank 0. Jobs 1..N-1 are enqueued as VSET4
     * words. At every HALT boundary hardware pops one entry, patches word 0
     * of the opposite resident bank, and launches it immediately.
     */
    printf("\n");
    printf("===== ITERATION 8F 4-ENTRY RESIDENT INPUT FIFO =====\n");

    const uint32_t fifo_batch_sizes[6] = {1u, 2u, 4u, 8u, 16u, 32u};
    uint32_t fifo_batch_cycles[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    uint32_t fifo_ready_reads[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    uint32_t fifo_pass = 1u;

    for (uint32_t sample = 0u; sample < 6u; sample++) {
        const uint32_t jobs = fifo_batch_sizes[sample];

        /* Both banks keep the same resident 3-word kernel skeleton. */
        puma_set_program_bank(0u);
        puma_program_stream_begin(0u);
        for (uint32_t i = 0u; i < resident_kernel_words; i++) {
            puma_program_push(compact_kernel_a[i]);
        }

        puma_set_program_bank(1u);
        puma_program_stream_begin(0u);
        for (uint32_t i = 0u; i < resident_kernel_words; i++) {
            puma_program_push(compact_kernel_a[i]);
        }

        puma_set_exec_bank(0u);

        uint32_t sample_ok = 1u;
        uint32_t ready_reads = 0u;

        const uint32_t fifo_start = read_mcycle();
        puma_start();

        /*
         * Enqueue every future input. The first four can normally be posted
         * immediately; after that software waits only when the FIFO is full.
         * Crucially, it no longer waits for a particular IMEM bank handoff.
         */
        for (uint32_t job = 1u; job < jobs; job++) {
            const uint32_t input_word =
                ((job & 0x1u) != 0u) ? compact_kernel_b[0] : compact_kernel_a[0];

            uint32_t ready_timeout = 2000000u;
            while (ready_timeout != 0u) {
                ready_reads++;
                if (puma_resident_fifo_ready() != 0u) {
                    break;
                }
                ready_timeout--;
            }

            if (ready_timeout == 0u) {
                sample_ok = 0u;
                break;
            }

            puma_resident_fifo_push(input_word);
        }

        /*
         * Wait until all queued entries have been consumed. EMPTY does not
         * mean the final job has finished; it means the final input has been
         * launched/removed from the FIFO. STATUS.DONE below drains that last
         * execution.
         */
        if ((sample_ok != 0u) && (jobs > 1u)) {
            uint32_t empty_timeout = 2000000u;
            while (empty_timeout != 0u) {
                ready_reads++;
                if (puma_resident_fifo_empty() != 0u) {
                    break;
                }
                empty_timeout--;
            }

            if (empty_timeout == 0u) {
                sample_ok = 0u;
            }
        }

        uint32_t final_status = 0u;
        uint32_t final_timeout = 2000000u;

        if (sample_ok != 0u) {
            do {
                final_status = puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);
                if ((final_status & 0x1u) != 0u) {
                    break;
                }
                final_timeout--;
            } while (final_timeout != 0u);

            if (final_timeout == 0u) {
                sample_ok = 0u;
            }
        }

        const uint32_t fifo_end = read_mcycle();
        fifo_batch_cycles[sample] = fifo_end - fifo_start;
        fifo_ready_reads[sample] = ready_reads;

        if (sample_ok != 0u) {
            const uint32_t final_instr =
                puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);
            const uint32_t final_cycles =
                puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);
            const uint32_t final_mvm =
                puma_read(PUMA_ACCEL_PERF_MVM_CYCLES_REG_OFFSET);
            const uint32_t final_fifo_count = puma_resident_fifo_count();
            const uint32_t expected_index = (jobs - 1u) & 0x1u;

            for (uint32_t lane = 0u; lane < 4u; lane++) {
                if ((int)puma_read_vreg_lane(2u, lane) !=
                    stream_expected[expected_index][lane]) {
                    sample_ok = 0u;
                }
            }

            if (final_instr != 3u ||
                final_cycles != resident_b_cycles ||
                final_mvm != 5u ||
                final_fifo_count != 0u) {
                sample_ok = 0u;
            }
        }

        if (sample_ok == 0u) {
            fifo_pass = 0u;
            printf("  batch N=%lu FAIL\n", (unsigned long)jobs);
            break;
        }
    }

    if (fifo_pass == 0u) {
        printf(" ITERATION 8F 4-ENTRY RESIDENT INPUT FIFO FAIL\n");
        return 1;
    }

    printf("  batch | total cycles | cycles/input | FIFO status reads\n");
    printf("  ------+--------------+--------------+------------------\n");

    for (uint32_t sample = 0u; sample < 6u; sample++) {
        const uint32_t jobs = fifo_batch_sizes[sample];
        const uint32_t total = fifo_batch_cycles[sample];
        const uint32_t whole = total / jobs;
        const uint32_t frac100 = ((total % jobs) * 100u) / jobs;

        printf("  %5lu | %12lu | %8lu.%02lu | %lu\n",
               (unsigned long)jobs,
               (unsigned long)total,
               (unsigned long)whole,
               (unsigned long)frac100,
               (unsigned long)fifo_ready_reads[sample]);
    }

    const uint32_t fifo_slope_8_16 =
        (fifo_batch_cycles[4] > fifo_batch_cycles[3]) ?
            ((fifo_batch_cycles[4] - fifo_batch_cycles[3]) / 8u) : 0u;

    const uint32_t fifo_slope_16_32 =
        (fifo_batch_cycles[5] > fifo_batch_cycles[4]) ?
            ((fifo_batch_cycles[5] - fifo_batch_cycles[4]) / 16u) : 0u;

    const uint32_t fifo_reference =
        (fifo_slope_16_32 != 0u) ? fifo_slope_16_32 : 1u;

    const uint32_t speedup_8d_fifo_x100 =
        (pipe_slope_16_32 * 100u) / fifo_reference;
    const uint32_t speedup_8eb_fifo_x100 =
        (hw_slope_16_32 * 100u) / fifo_reference;
    const uint32_t speedup_8a_fifo_x100 =
        (compact_commit_interval * 100u) / fifo_reference;

    printf("\n");
    printf("  8D software-managed slope      = %lu cycles/input\n",
           (unsigned long)pipe_slope_16_32);
    printf("  8E-B hardware-managed slope    = %lu cycles/input\n",
           (unsigned long)hw_slope_16_32);
    printf("  8A compact reference           = %lu cycles/input\n",
           (unsigned long)compact_commit_interval);
    printf("  8F incremental slope 8->16     = %lu cycles/input\n",
           (unsigned long)fifo_slope_8_16);
    printf("  8F incremental slope 16->32    = %lu cycles/input\n",
           (unsigned long)fifo_slope_16_32);
    printf("  accelerator execution latency  = %lu cycles/input\n",
           (unsigned long)resident_b_cycles);
    printf("  8D -> 8F speedup               = %lu.%02lux\n",
           (unsigned long)(speedup_8d_fifo_x100 / 100u),
           (unsigned long)(speedup_8d_fifo_x100 % 100u));
    printf("  8E-B -> 8F speedup             = %lu.%02lux\n",
           (unsigned long)(speedup_8eb_fifo_x100 / 100u),
           (unsigned long)(speedup_8eb_fifo_x100 % 100u));
    printf("  8A -> 8F speedup               = %lu.%02lux\n",
           (unsigned long)(speedup_8a_fifo_x100 / 100u),
           (unsigned long)(speedup_8a_fifo_x100 % 100u));

    if (fifo_slope_16_32 <= resident_b_cycles + 2u) {
        printf("  8F classification              = near execution-bound\n");
    } else if (fifo_slope_16_32 < compact_commit_interval) {
        printf("  8F classification              = beats 8A; small residual system overhead remains\n");
    } else {
        printf("  8F classification              = FIFO producer/consumer path still limits throughput\n");
    }

    printf(" ITERATION 8F 4-ENTRY RESIDENT INPUT FIFO PASS\n");

    /*
     * =========================================================
     * Iteration 8H: 4-entry FIFO + MMIO backpressure
     * =========================================================
     *
     * 8G removed the per-input FIFO_STATUS dependency by using a 32-entry
     * FIFO, large enough for every future input in the N<=32 benchmark.
     *
     * 8H asks the stricter question: is that deep FIFO actually necessary?
     * The hardware FIFO is restored to four entries. Software still issues
     * consecutive FIFO_PUSH writes with NO per-input STATUS read. If the FIFO
     * becomes full, the accelerator's MMIO response deasserts READY and the
     * current CPU store naturally stalls until one FIFO slot becomes free.
     *
     * Therefore:
     *   8F = depth 4 + STATUS-before-PUSH software handshake
     *   8H = depth 4 + no STATUS-before-PUSH; hardware bus backpressure
     *
     * This keeps FIFO depth fixed and changes only the producer protocol.
     */
    printf("\n");
    printf("===== ITERATION 8H 4-ENTRY BACKPRESSURED FIFO =====\n");

    const uint32_t bp_batch_sizes[6] = {1u, 2u, 4u, 8u, 16u, 32u};
    uint32_t bp_batch_cycles[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    uint32_t bp_pass = 1u;

    for (uint32_t sample = 0u; sample < 6u; sample++) {
        const uint32_t jobs = bp_batch_sizes[sample];

        /* Both banks keep the same resident 3-word kernel skeleton. */
        puma_set_program_bank(0u);
        puma_program_stream_begin(0u);
        for (uint32_t i = 0u; i < resident_kernel_words; i++) {
            puma_program_push(compact_kernel_a[i]);
        }

        puma_set_program_bank(1u);
        puma_program_stream_begin(0u);
        for (uint32_t i = 0u; i < resident_kernel_words; i++) {
            puma_program_push(compact_kernel_a[i]);
        }

        puma_set_exec_bank(0u);

        uint32_t sample_ok = 1u;

        /*
         * Every previous sample is fully drained before the next sample.
         * This one untimed read is a sanity check only; it is not part of the
         * producer loop and does not create a per-input dependency.
         */
        if (puma_resident_fifo_count() != 0u) {
            printf("  batch N=%lu FAIL: FIFO was not empty before launch\n",
                   (unsigned long)jobs);
            bp_pass = 0u;
            break;
        }

        const uint32_t bp_start = read_mcycle();
        puma_start();

        /*
         * Critical 8H path: consecutive PUSH writes, exactly like the 8G
         * software producer. There is deliberately NO FIFO_STATUS read here.
         *
         * With depth=4, the fifth outstanding write may reach a full FIFO.
         * The MMIO interface then stalls that store until hardware consumes
         * an older entry. No input is dropped and software does not poll.
         */
        for (uint32_t job = 1u; job < jobs; job++) {
            const uint32_t input_word =
                ((job & 0x1u) != 0u) ? compact_kernel_b[0] : compact_kernel_a[0];

            puma_resident_fifo_push(input_word);
        }

        /*
         * Producer delivery is now complete. EMPTY means the final queued
         * input has been removed for execution; STATUS.DONE then drains the
         * final resident-kernel job.
         */
        if (jobs > 1u) {
            uint32_t empty_timeout = 2000000u;
            while (empty_timeout != 0u) {
                if (puma_resident_fifo_empty() != 0u) {
                    break;
                }
                empty_timeout--;
            }
            if (empty_timeout == 0u) {
                sample_ok = 0u;
            }
        }

        uint32_t final_status = 0u;
        uint32_t final_timeout = 2000000u;
        if (sample_ok != 0u) {
            do {
                final_status = puma_read(PUMA_ACCEL_STATUS_REG_OFFSET);
                if ((final_status & 0x1u) != 0u) {
                    break;
                }
                final_timeout--;
            } while (final_timeout != 0u);

            if (final_timeout == 0u) {
                sample_ok = 0u;
            }
        }

        const uint32_t bp_end = read_mcycle();
        bp_batch_cycles[sample] = bp_end - bp_start;

        if (sample_ok != 0u) {
            const uint32_t final_instr =
                puma_read(PUMA_ACCEL_PERF_INSTR_REG_OFFSET);
            const uint32_t final_cycles =
                puma_read(PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET);
            const uint32_t final_mvm =
                puma_read(PUMA_ACCEL_PERF_MVM_CYCLES_REG_OFFSET);
            const uint32_t final_fifo_count = puma_resident_fifo_count();
            const uint32_t expected_index = (jobs - 1u) & 0x1u;

            for (uint32_t lane = 0u; lane < 4u; lane++) {
                if ((int)puma_read_vreg_lane(2u, lane) !=
                    stream_expected[expected_index][lane]) {
                    sample_ok = 0u;
                }
            }

            if (final_instr != 3u ||
                final_cycles != resident_b_cycles ||
                final_mvm != 5u ||
                final_fifo_count != 0u) {
                sample_ok = 0u;
            }
        }

        if (sample_ok == 0u) {
            bp_pass = 0u;
            printf("  batch N=%lu FAIL\n", (unsigned long)jobs);
            break;
        }
    }

    if (bp_pass == 0u) {
        printf(" ITERATION 8H 4-ENTRY BACKPRESSURED FIFO FAIL\n");
        return 1;
    }

    printf("  batch | total cycles | cycles/input\n");
    printf("  ------+--------------+-------------\n");

    for (uint32_t sample = 0u; sample < 6u; sample++) {
        const uint32_t jobs = bp_batch_sizes[sample];
        const uint32_t total = bp_batch_cycles[sample];
        const uint32_t whole = total / jobs;
        const uint32_t frac100 = ((total % jobs) * 100u) / jobs;

        printf("  %5lu | %12lu | %8lu.%02lu\n",
               (unsigned long)jobs,
               (unsigned long)total,
               (unsigned long)whole,
               (unsigned long)frac100);
    }

    const uint32_t bp_slope_8_16 =
        (bp_batch_cycles[4] > bp_batch_cycles[3]) ?
            ((bp_batch_cycles[4] - bp_batch_cycles[3]) / 8u) : 0u;

    const uint32_t bp_slope_16_32 =
        (bp_batch_cycles[5] > bp_batch_cycles[4]) ?
            ((bp_batch_cycles[5] - bp_batch_cycles[4]) / 16u) : 0u;

    const uint32_t bp_reference =
        (bp_slope_16_32 != 0u) ? bp_slope_16_32 : 1u;

    const uint32_t speedup_8f_8h_x100 =
        (fifo_slope_16_32 * 100u) / bp_reference;
    const uint32_t speedup_8a_8h_x100 =
        (compact_commit_interval * 100u) / bp_reference;

    printf("\n");
    printf("  FIFO depth                       = 4 entries\n");
    printf("  producer STATUS reads/input      = 0 (8H PUSH loop)\n");
    printf("  8F read+push FIFO slope          = %lu cycles/input\n",
           (unsigned long)fifo_slope_16_32);
    printf("  8H incremental slope 8->16       = %lu cycles/input\n",
           (unsigned long)bp_slope_8_16);
    printf("  8H incremental slope 16->32      = %lu cycles/input\n",
           (unsigned long)bp_slope_16_32);
    printf("  accelerator execution latency    = %lu cycles/input\n",
           (unsigned long)resident_b_cycles);
    printf("  8F -> 8H speedup                 = %lu.%02lux\n",
           (unsigned long)(speedup_8f_8h_x100 / 100u),
           (unsigned long)(speedup_8f_8h_x100 % 100u));
    printf("  8A -> 8H speedup                 = %lu.%02lux\n",
           (unsigned long)(speedup_8a_8h_x100 / 100u),
           (unsigned long)(speedup_8a_8h_x100 % 100u));

    if (bp_slope_16_32 <= resident_b_cycles + 2u) {
        printf("  8H classification                = small FIFO is sufficient when MMIO PUSH uses hardware backpressure\n");
    } else if (bp_slope_16_32 < fifo_slope_16_32) {
        printf("  8H classification                = removing software STATUS handshakes helps, but small-FIFO stalls leave a residual gap\n");
    } else {
        printf("  8H classification                = small-FIFO backpressure does not improve steady-state throughput\n");
    }

    printf(" ITERATION 8H 4-ENTRY BACKPRESSURED FIFO PASS\n");

    puma_set_exec_bank(0u);

    printf("================================================\\n");
    printf(" DEMO PASS\n");
    printf("================================================\n");

    printf(" IMEM SAFETY FAULT TEST PASS\n");
    printf(" ITERATION 4B CONCURRENT DUAL-BANK IMEM PASS\n");
    printf(" ITERATION 4C FULL-PROGRAM OVERLAP PASS\n");
    printf(" ITERATION 5C DIRECT-OVERLAP PASS\n");
    printf(" ITERATION 7A AUTO-COMMIT PASS\n");
    printf(" ITERATION 7B PERSISTENT-WEIGHT PASS\n");
    printf(" ITERATION 8A VSET4 COMPACT-INPUT PASS\n");
    printf(" ITERATION 8B RESIDENT-KERNEL PASS\n");
    printf(" ITERATION 8C STREAMING BENCHMARK PASS\n");
    printf(" ITERATION 8D MULTI-INPUT PIPELINE PASS\n");
    printf(" ITERATION 8E-A HANDOFF DECOMPOSITION PASS\n");
    printf(" ITERATION 8E-B HARDWARE-MANAGED RESIDENT PIPELINE PASS\n");
    printf(" ITERATION 8F 4-ENTRY RESIDENT INPUT FIFO PASS\n");
    printf(" ITERATION 8H 4-ENTRY BACKPRESSURED FIFO PASS\n");

    printf("\n");
    printf("Iteration 4C compared sequential and overlapped full 31-word next-program delivery.\n");
    printf("The measured START-to-START intervals are the system-level throughput metric.\n");
    printf("Legacy IMEM_ADDR/IMEM_DATA/IMEM_WRITE remains available for debug.\n");
    printf("\n");

    return 0;
}
