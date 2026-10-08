## Summary

| Name                                                       | Offset   |   Length | Description                                                                                                                          |
|:-----------------------------------------------------------|:---------|---------:|:-------------------------------------------------------------------------------------------------------------------------------------|
| puma_accel.[`CTRL`](#ctrl)                                 | 0x0      |        4 | PUMA accelerator control                                                                                                             |
| puma_accel.[`STATUS`](#status)                             | 0x4      |        4 | PUMA accelerator status                                                                                                              |
| puma_accel.[`IMEM_ADDR`](#imem_addr)                       | 0x8      |        4 | Instruction memory programming address and auto-increment loader base                                                                |
| puma_accel.[`IMEM_DATA`](#imem_data)                       | 0xc      |        4 | Instruction memory programming data                                                                                                  |
| puma_accel.[`IMEM_WRITE`](#imem_write)                     | 0x10     |        4 | Instruction memory write control                                                                                                     |
| puma_accel.[`IMEM_PUSH`](#imem_push)                       | 0x14     |        4 | Auto-increment instruction memory programming data                                                                                   |
| puma_accel.[`VREG_SEL`](#vreg_sel)                         | 0x18     |        4 | Vector register and lane selection for software readback                                                                             |
| puma_accel.[`VREG_DATA`](#vreg_data)                       | 0x1c     |        4 | Selected vector register lane value                                                                                                  |
| puma_accel.[`PC`](#pc)                                     | 0x20     |        4 | Current PUMA program counter for debug                                                                                               |
| puma_accel.[`IR`](#ir)                                     | 0x24     |        4 | Current PUMA instruction register for debug                                                                                          |
| puma_accel.[`PERF_TOTAL_CYCLES`](#perf_total_cycles)       | 0x28     |        4 | Total active PUMA execution cycles                                                                                                   |
| puma_accel.[`PERF_INSTR`](#perf_instr)                     | 0x2c     |        4 | Number of executed PUMA instructions                                                                                                 |
| puma_accel.[`PERF_LOAD_CYCLES`](#perf_load_cycles)         | 0x30     |        4 | Cycles spent in LOAD-specific states                                                                                                 |
| puma_accel.[`PERF_STORE_CYCLES`](#perf_store_cycles)       | 0x34     |        4 | Cycles spent in STORE-specific states                                                                                                |
| puma_accel.[`PERF_WSTORE_CYCLES`](#perf_wstore_cycles)     | 0x38     |        4 | Cycles spent in PISLib weight-programming states                                                                                     |
| puma_accel.[`PERF_MVM_CYCLES`](#perf_mvm_cycles)           | 0x3c     |        4 | Cycles spent in MVM-specific states                                                                                                  |
| puma_accel.[`IMEM_PROG_BANK`](#imem_prog_bank)             | 0x40     |        4 | Instruction-memory bank targeted by CPU programming writes                                                                           |
| puma_accel.[`IMEM_EXEC_BANK`](#imem_exec_bank)             | 0x44     |        4 | Instruction-memory bank to latch for the next accelerator START                                                                      |
| puma_accel.[`IMEM_ACTIVE_BANK`](#imem_active_bank)         | 0x48     |        4 | Instruction-memory bank currently latched for execution                                                                              |
| puma_accel.[`IMEM_PUSH_COMMIT`](#imem_push_commit)         | 0x4c     |        4 | Write the final instruction at the auto-increment pointer and queue that programming bank for automatic execution                    |
| puma_accel.[`RESIDENT_PUSH`](#resident_push)               | 0x50     |        4 | Fast-path resident-kernel input push. Hardware writes IMEM word 0 of the inactive bank and queues that bank for automatic execution. |
| puma_accel.[`RESIDENT_STATUS`](#resident_status)           | 0x54     |        4 | Resident-kernel fast-path status                                                                                                     |
| puma_accel.[`RESIDENT_FIFO_PUSH`](#resident_fifo_push)     | 0x58     |        4 | Iteration 8G enqueue port for the 32-entry resident-input burst FIFO                                                                 |
| puma_accel.[`RESIDENT_FIFO_STATUS`](#resident_fifo_status) | 0x5c     |        4 | Iteration 8G resident-input burst FIFO status                                                                                        |

## CTRL
PUMA accelerator control
- Offset: `0x0`
- Reset default: `0x0`
- Reset mask: `0x1`

### Fields

```wavejson
{"reg": [{"name": "START", "bits": 1, "attr": ["rw"], "rotate": -90}, {"bits": 31}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                 |
|:------:|:------:|:-------:|:-------|:----------------------------|
|  31:1  |        |         |        | Reserved                    |
|   0    |   rw   |    x    | START  | Start accelerator execution |

## STATUS
PUMA accelerator status
- Offset: `0x4`
- Reset default: `0x0`
- Reset mask: `0x3`

### Fields

```wavejson
{"reg": [{"name": "FLAGS", "bits": 2, "attr": ["ro"], "rotate": -90}, {"bits": 30}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description              |
|:------:|:------:|:-------:|:-------|:-------------------------|
|  31:2  |        |         |        | Reserved                 |
|  1:0   |   ro   |    x    | FLAGS  | bit 0: DONE, bit 1: BUSY |

## IMEM_ADDR
Instruction memory programming address and auto-increment loader base
- Offset: `0x8`
- Reset default: `0x0`
- Reset mask: `0xff`

### Fields

```wavejson
{"reg": [{"name": "ADDR", "bits": 8, "attr": ["rw"], "rotate": 0}, {"bits": 24}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                     |
|:------:|:------:|:-------:|:-------|:--------------------------------|
|  31:8  |        |         |        | Reserved                        |
|  7:0   |   rw   |    x    | ADDR   | Instruction memory word address |

## IMEM_DATA
Instruction memory programming data
- Offset: `0xc`
- Reset default: `0x0`
- Reset mask: `0xffffffff`

### Fields

```wavejson
{"reg": [{"name": "DATA", "bits": 32, "attr": ["rw"], "rotate": 0}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                  |
|:------:|:------:|:-------:|:-------|:-----------------------------|
|  31:0  |   rw   |    x    | DATA   | 32-bit PUMA-like instruction |

## IMEM_WRITE
Instruction memory write control
- Offset: `0x10`
- Reset default: `0x0`
- Reset mask: `0x1`

### Fields

```wavejson
{"reg": [{"name": "WE", "bits": 1, "attr": ["rw"], "rotate": -90}, {"bits": 31}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                                |
|:------:|:------:|:-------:|:-------|:-------------------------------------------|
|  31:1  |        |         |        | Reserved                                   |
|   0    |   rw   |    x    | WE     | Write IMEM_DATA to IMEM_ADDR when asserted |

## IMEM_PUSH
Auto-increment instruction memory programming data
- Offset: `0x14`
- Reset default: `0x0`
- Reset mask: `0xffffffff`

### Fields

```wavejson
{"reg": [{"name": "DATA", "bits": 32, "attr": ["rw"], "rotate": 0}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                                                |
|:------:|:------:|:-------:|:-------|:-----------------------------------------------------------|
|  31:0  |   rw   |    x    | DATA   | Write one instruction at the auto-increment loader pointer |

## VREG_SEL
Vector register and lane selection for software readback
- Offset: `0x18`
- Reset default: `0x0`
- Reset mask: `0x3ff`

### Fields

```wavejson
{"reg": [{"name": "SEL", "bits": 10, "attr": ["rw"], "rotate": 0}, {"bits": 22}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                                           |
|:------:|:------:|:-------:|:-------|:------------------------------------------------------|
| 31:10  |        |         |        | Reserved                                              |
|  9:0   |   rw   |    x    | SEL    | bits 2:0 select vector register, bits 9:8 select lane |

## VREG_DATA
Selected vector register lane value
- Offset: `0x1c`
- Reset default: `0x0`
- Reset mask: `0xffff`

### Fields

```wavejson
{"reg": [{"name": "DATA", "bits": 16, "attr": ["ro"], "rotate": 0}, {"bits": 16}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                          |
|:------:|:------:|:-------:|:-------|:-------------------------------------|
| 31:16  |        |         |        | Reserved                             |
|  15:0  |   ro   |    x    | DATA   | Selected 16-bit vector register lane |

## PC
Current PUMA program counter for debug
- Offset: `0x20`
- Reset default: `0x0`
- Reset mask: `0xff`

### Fields

```wavejson
{"reg": [{"name": "VALUE", "bits": 8, "attr": ["ro"], "rotate": 0}, {"bits": 24}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description             |
|:------:|:------:|:-------:|:-------|:------------------------|
|  31:8  |        |         |        | Reserved                |
|  7:0   |   ro   |    x    | VALUE  | Current program counter |

## IR
Current PUMA instruction register for debug
- Offset: `0x24`
- Reset default: `0x0`
- Reset mask: `0xffffffff`

### Fields

```wavejson
{"reg": [{"name": "VALUE", "bits": 32, "attr": ["ro"], "rotate": 0}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                |
|:------:|:------:|:-------:|:-------|:---------------------------|
|  31:0  |   ro   |    x    | VALUE  | Current 32-bit instruction |

## PERF_TOTAL_CYCLES
Total active PUMA execution cycles
- Offset: `0x28`
- Reset default: `0x0`
- Reset mask: `0xffffffff`

### Fields

```wavejson
{"reg": [{"name": "VALUE", "bits": 32, "attr": ["ro"], "rotate": 0}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                                                     |
|:------:|:------:|:-------:|:-------|:----------------------------------------------------------------|
|  31:0  |   ro   |    x    | VALUE  | Cycles from first FETCH through the HALT instruction EXEC cycle |

## PERF_INSTR
Number of executed PUMA instructions
- Offset: `0x2c`
- Reset default: `0x0`
- Reset mask: `0xffffffff`

### Fields

```wavejson
{"reg": [{"name": "VALUE", "bits": 32, "attr": ["ro"], "rotate": 0}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                            |
|:------:|:------:|:-------:|:-------|:---------------------------------------|
|  31:0  |   ro   |    x    | VALUE  | Number of instructions reaching S_EXEC |

## PERF_LOAD_CYCLES
Cycles spent in LOAD-specific states
- Offset: `0x30`
- Reset default: `0x0`
- Reset mask: `0xffffffff`

### Fields

```wavejson
{"reg": [{"name": "VALUE", "bits": 32, "attr": ["ro"], "rotate": 0}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                                     |
|:------:|:------:|:-------:|:-------|:------------------------------------------------|
|  31:0  |   ro   |    x    | VALUE  | Cycles in S_LOAD_REQ/S_LOAD_WAIT/S_LOAD_CAPTURE |

## PERF_STORE_CYCLES
Cycles spent in STORE-specific states
- Offset: `0x34`
- Reset default: `0x0`
- Reset mask: `0xffffffff`

### Fields

```wavejson
{"reg": [{"name": "VALUE", "bits": 32, "attr": ["ro"], "rotate": 0}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                        |
|:------:|:------:|:-------:|:-------|:-----------------------------------|
|  31:0  |   ro   |    x    | VALUE  | Cycles in S_STORE_REQ/S_STORE_WAIT |

## PERF_WSTORE_CYCLES
Cycles spent in PISLib weight-programming states
- Offset: `0x38`
- Reset default: `0x0`
- Reset mask: `0xffffffff`

### Fields

```wavejson
{"reg": [{"name": "VALUE", "bits": 32, "attr": ["ro"], "rotate": 0}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description            |
|:------:|:------:|:-------:|:-------|:-----------------------|
|  31:0  |   ro   |    x    | VALUE  | Cycles in S_WSTORE_REQ |

## PERF_MVM_CYCLES
Cycles spent in MVM-specific states
- Offset: `0x3c`
- Reset default: `0x0`
- Reset mask: `0xffffffff`

### Fields

```wavejson
{"reg": [{"name": "VALUE", "bits": 32, "attr": ["ro"], "rotate": 0}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                      |
|:------:|:------:|:-------:|:-------|:---------------------------------|
|  31:0  |   ro   |    x    | VALUE  | Cycles in S_MVM_START/S_MVM_WAIT |

## IMEM_PROG_BANK
Instruction-memory bank targeted by CPU programming writes
- Offset: `0x40`
- Reset default: `0x0`
- Reset mask: `0x1`

### Fields

```wavejson
{"reg": [{"name": "BANK", "bits": 1, "attr": ["rw"], "rotate": -90}, {"bits": 31}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                        |
|:------:|:------:|:-------:|:-------|:-----------------------------------|
|  31:1  |        |         |        | Reserved                           |
|   0    |   rw   |    x    | BANK   | 0 selects bank 0, 1 selects bank 1 |

## IMEM_EXEC_BANK
Instruction-memory bank to latch for the next accelerator START
- Offset: `0x44`
- Reset default: `0x0`
- Reset mask: `0x1`

### Fields

```wavejson
{"reg": [{"name": "BANK", "bits": 1, "attr": ["rw"], "rotate": -90}, {"bits": 31}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                        |
|:------:|:------:|:-------:|:-------|:-----------------------------------|
|  31:1  |        |         |        | Reserved                           |
|   0    |   rw   |    x    | BANK   | 0 selects bank 0, 1 selects bank 1 |

## IMEM_ACTIVE_BANK
Instruction-memory bank currently latched for execution
- Offset: `0x48`
- Reset default: `0x0`
- Reset mask: `0x1`

### Fields

```wavejson
{"reg": [{"name": "BANK", "bits": 1, "attr": ["ro"], "rotate": -90}, {"bits": 31}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                                |
|:------:|:------:|:-------:|:-------|:-------------------------------------------|
|  31:1  |        |         |        | Reserved                                   |
|   0    |   ro   |    x    | BANK   | Bank currently used by the PUMA fetch path |

## IMEM_PUSH_COMMIT
Write the final instruction at the auto-increment pointer and queue that programming bank for automatic execution
- Offset: `0x4c`
- Reset default: `0x0`
- Reset mask: `0xffffffff`

### Fields

```wavejson
{"reg": [{"name": "DATA", "bits": 32, "attr": ["rw"], "rotate": 0}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                                                                       |
|:------:|:------:|:-------:|:-------|:----------------------------------------------------------------------------------|
|  31:0  |   rw   |    x    | DATA   | Final instruction word; accepted write also commits the selected programming bank |

## RESIDENT_PUSH
Fast-path resident-kernel input push. Hardware writes IMEM word 0 of the inactive bank and queues that bank for automatic execution.
- Offset: `0x50`
- Reset default: `0x0`
- Reset mask: `0xffffffff`

### Fields

```wavejson
{"reg": [{"name": "DATA", "bits": 32, "attr": ["rw"], "rotate": 0}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                                                      |
|:------:|:------:|:-------:|:-------|:-----------------------------------------------------------------|
|  31:0  |   rw   |    x    | DATA   | Replacement resident-kernel word 0, normally a VSET4 instruction |

## RESIDENT_STATUS
Resident-kernel fast-path status
- Offset: `0x54`
- Reset default: `0x0`
- Reset mask: `0x3`

### Fields

```wavejson
{"reg": [{"name": "FLAGS", "bits": 2, "attr": ["ro"], "rotate": -90}, {"bits": 30}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                                                        |
|:------:|:------:|:-------:|:-------|:-------------------------------------------------------------------|
|  31:2  |        |         |        | Reserved                                                           |
|  1:0   |   ro   |    x    | FLAGS  | bit 0: READY for RESIDENT_PUSH, bit 1: one next-bank job is queued |

## RESIDENT_FIFO_PUSH
Iteration 8G enqueue port for the 32-entry resident-input burst FIFO
- Offset: `0x58`
- Reset default: `0x0`
- Reset mask: `0xffffffff`

### Fields

```wavejson
{"reg": [{"name": "DATA", "bits": 32, "attr": ["rw"], "rotate": 0}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                                                     |
|:------:|:------:|:-------:|:-------|:----------------------------------------------------------------|
|  31:0  |   rw   |    x    | DATA   | Resident-kernel word 0 to enqueue, normally a VSET4 instruction |

## RESIDENT_FIFO_STATUS
Iteration 8G resident-input burst FIFO status
- Offset: `0x5c`
- Reset default: `0x0`
- Reset mask: `0x1ff`

### Fields

```wavejson
{"reg": [{"name": "FLAGS", "bits": 9, "attr": ["ro"], "rotate": 0}, {"bits": 23}], "config": {"lanes": 1, "fontsize": 10, "vspace": 80}}
```

|  Bits  |  Type  |  Reset  | Name   | Description                                                       |
|:------:|:------:|:-------:|:-------|:------------------------------------------------------------------|
|  31:9  |        |         |        | Reserved                                                          |
|  8:0   |   ro   |    x    | FLAGS  | bit 0 READY, bit 1 EMPTY, bit 2 FULL, bits 8:3 FIFO COUNT (0..32) |
