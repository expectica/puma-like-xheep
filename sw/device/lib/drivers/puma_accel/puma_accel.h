// Generated register defines for puma_accel

#ifndef _PUMA_ACCEL_REG_DEFS_
#define _PUMA_ACCEL_REG_DEFS_

#ifdef __cplusplus
extern "C" {
#endif
// Register width
#define PUMA_ACCEL_PARAM_REG_WIDTH 32

// PUMA accelerator control
#define PUMA_ACCEL_CTRL_REG_OFFSET 0x0
#define PUMA_ACCEL_CTRL_START_BIT 0

// PUMA accelerator status
#define PUMA_ACCEL_STATUS_REG_OFFSET 0x4
#define PUMA_ACCEL_STATUS_FLAGS_MASK 0x3
#define PUMA_ACCEL_STATUS_FLAGS_OFFSET 0
#define PUMA_ACCEL_STATUS_FLAGS_FIELD \
  ((bitfield_field32_t) { .mask = PUMA_ACCEL_STATUS_FLAGS_MASK, .index = PUMA_ACCEL_STATUS_FLAGS_OFFSET })

// Instruction memory programming address and auto-increment loader base
#define PUMA_ACCEL_IMEM_ADDR_REG_OFFSET 0x8
#define PUMA_ACCEL_IMEM_ADDR_ADDR_MASK 0xff
#define PUMA_ACCEL_IMEM_ADDR_ADDR_OFFSET 0
#define PUMA_ACCEL_IMEM_ADDR_ADDR_FIELD \
  ((bitfield_field32_t) { .mask = PUMA_ACCEL_IMEM_ADDR_ADDR_MASK, .index = PUMA_ACCEL_IMEM_ADDR_ADDR_OFFSET })

// Instruction memory programming data
#define PUMA_ACCEL_IMEM_DATA_REG_OFFSET 0xc

// Instruction memory write control
#define PUMA_ACCEL_IMEM_WRITE_REG_OFFSET 0x10
#define PUMA_ACCEL_IMEM_WRITE_WE_BIT 0

// Auto-increment instruction memory programming data
#define PUMA_ACCEL_IMEM_PUSH_REG_OFFSET 0x14

// Vector register and lane selection for software readback
#define PUMA_ACCEL_VREG_SEL_REG_OFFSET 0x18
#define PUMA_ACCEL_VREG_SEL_SEL_MASK 0x3ff
#define PUMA_ACCEL_VREG_SEL_SEL_OFFSET 0
#define PUMA_ACCEL_VREG_SEL_SEL_FIELD \
  ((bitfield_field32_t) { .mask = PUMA_ACCEL_VREG_SEL_SEL_MASK, .index = PUMA_ACCEL_VREG_SEL_SEL_OFFSET })

// Selected vector register lane value
#define PUMA_ACCEL_VREG_DATA_REG_OFFSET 0x1c
#define PUMA_ACCEL_VREG_DATA_DATA_MASK 0xffff
#define PUMA_ACCEL_VREG_DATA_DATA_OFFSET 0
#define PUMA_ACCEL_VREG_DATA_DATA_FIELD \
  ((bitfield_field32_t) { .mask = PUMA_ACCEL_VREG_DATA_DATA_MASK, .index = PUMA_ACCEL_VREG_DATA_DATA_OFFSET })

// Current PUMA program counter for debug
#define PUMA_ACCEL_PC_REG_OFFSET 0x20
#define PUMA_ACCEL_PC_VALUE_MASK 0xff
#define PUMA_ACCEL_PC_VALUE_OFFSET 0
#define PUMA_ACCEL_PC_VALUE_FIELD \
  ((bitfield_field32_t) { .mask = PUMA_ACCEL_PC_VALUE_MASK, .index = PUMA_ACCEL_PC_VALUE_OFFSET })

// Current PUMA instruction register for debug
#define PUMA_ACCEL_IR_REG_OFFSET 0x24

// Total active PUMA execution cycles
#define PUMA_ACCEL_PERF_TOTAL_CYCLES_REG_OFFSET 0x28

// Number of executed PUMA instructions
#define PUMA_ACCEL_PERF_INSTR_REG_OFFSET 0x2c

// Cycles spent in LOAD-specific states
#define PUMA_ACCEL_PERF_LOAD_CYCLES_REG_OFFSET 0x30

// Cycles spent in STORE-specific states
#define PUMA_ACCEL_PERF_STORE_CYCLES_REG_OFFSET 0x34

// Cycles spent in PISLib weight-programming states
#define PUMA_ACCEL_PERF_WSTORE_CYCLES_REG_OFFSET 0x38

// Cycles spent in MVM-specific states
#define PUMA_ACCEL_PERF_MVM_CYCLES_REG_OFFSET 0x3c

// Instruction-memory bank targeted by CPU programming writes
#define PUMA_ACCEL_IMEM_PROG_BANK_REG_OFFSET 0x40
#define PUMA_ACCEL_IMEM_PROG_BANK_BANK_BIT 0

// Instruction-memory bank to latch for the next accelerator START
#define PUMA_ACCEL_IMEM_EXEC_BANK_REG_OFFSET 0x44
#define PUMA_ACCEL_IMEM_EXEC_BANK_BANK_BIT 0

// Instruction-memory bank currently latched for execution
#define PUMA_ACCEL_IMEM_ACTIVE_BANK_REG_OFFSET 0x48
#define PUMA_ACCEL_IMEM_ACTIVE_BANK_BANK_BIT 0

// Write the final instruction at the auto-increment pointer and queue that
// programming bank for automatic execution
#define PUMA_ACCEL_IMEM_PUSH_COMMIT_REG_OFFSET 0x4c

// Fast-path resident-kernel input push. Hardware writes IMEM word 0 of the
// inactive bank and queues that bank for automatic execution.
#define PUMA_ACCEL_RESIDENT_PUSH_REG_OFFSET 0x50

// Resident-kernel fast-path status
#define PUMA_ACCEL_RESIDENT_STATUS_REG_OFFSET 0x54
#define PUMA_ACCEL_RESIDENT_STATUS_FLAGS_MASK 0x3
#define PUMA_ACCEL_RESIDENT_STATUS_FLAGS_OFFSET 0
#define PUMA_ACCEL_RESIDENT_STATUS_FLAGS_FIELD \
  ((bitfield_field32_t) { .mask = PUMA_ACCEL_RESIDENT_STATUS_FLAGS_MASK, .index = PUMA_ACCEL_RESIDENT_STATUS_FLAGS_OFFSET })

// Iteration 8G enqueue port for the 32-entry resident-input burst FIFO
#define PUMA_ACCEL_RESIDENT_FIFO_PUSH_REG_OFFSET 0x58

// Iteration 8G resident-input burst FIFO status
#define PUMA_ACCEL_RESIDENT_FIFO_STATUS_REG_OFFSET 0x5c
#define PUMA_ACCEL_RESIDENT_FIFO_STATUS_FLAGS_MASK 0x1ff
#define PUMA_ACCEL_RESIDENT_FIFO_STATUS_FLAGS_OFFSET 0
#define PUMA_ACCEL_RESIDENT_FIFO_STATUS_FLAGS_FIELD \
  ((bitfield_field32_t) { .mask = PUMA_ACCEL_RESIDENT_FIFO_STATUS_FLAGS_MASK, .index = PUMA_ACCEL_RESIDENT_FIFO_STATUS_FLAGS_OFFSET })

#ifdef __cplusplus
}  // extern "C"
#endif
#endif  // _PUMA_ACCEL_REG_DEFS_
// End generated register defines for puma_accel