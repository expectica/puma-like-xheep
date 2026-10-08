# PUMA-like Programmable CIM Accelerator on X-HEEP

> **Research prototype — Iteration 8H baseline**  
> 基于 [X-HEEP](https://github.com/x-heep/x-heep) 的可编程存内计算加速器实验工程。**本项目不是原版 PUMA 的逐位兼容复现，也不是 X-HEEP 官方仓库。**

**English summary.** This repository extends the X-HEEP RISC-V SoC with a programmable, MMIO-controlled PUMA-like accelerator. Its current baseline supports a four-input/four-output PISLib/APIM MVM path, software-programmable weights, a resident kernel, dual-bank instruction memory, and a four-entry hardware-backpressured operand FIFO. The next milestone is tiled multi-PIM workload scalability.

## 1. 架构概览 / Architecture

```text
                        X-HEEP RISC-V CPU
                                |
                       MMIO register bus
                                v
                       +----------------+
                       |  puma_accel    |  register/MMIO wrapper
                       +-------+--------+
                               |
                       +-------v--------+
                       | puma_like_core |
                       | ISA decoder    |
                       | FSM, PC, IMEM  |  dual-bank IMEM
                       | vector RF      |
                       | resident FIFO  |  depth = 4; HW backpressure
                       | local memory   |
                       +-------+--------+
                               |
                         MVM start/done
                               v
                          mvm_unit
                               |
                          mvm_backend
                               |
                        pis_mvm_backend
                               |
                        Basic_GeMM_CIM
                         (one APIM/PIM)
                               |
                         4 output lanes
```

当前物理计算路径是 **一个 `Basic_GeMM_CIM` 实例**，逻辑 MVM 路径为 **4×4**。控制器、RISC-V 主机侧接口和单 PIM adapter 已集成。尚未实现多 PIM 并行归约或任意尺寸矩阵自动分块。

## 2. 当前主要功能 / Features

- **可编程执行**：32-bit 自定义指令、程序计数器、译码、FSM、多周期计算/存储操作。
- **数据路径**：vector register file、local memory、vector arithmetic、LOAD/STORE。
- **动态权重**：通过 `WSTORE` 指令编程 PISLib/APIM 内部权重；`MVM` 使用已写入的权重。
- **主机控制**：RISC-V CPU 经 MMIO 加载程序、启动、轮询状态并读回结果。
- **双 bank IMEM 与 resident 模式**：支持驻留小程序及后续输入流。
- **4-entry resident input FIFO**：硬件管理 input queue 和 backpressure，避免软件逐次手动交接成为唯一瓶颈。
- **性能计数器**：用于记录指令、MVM 和其他操作对应的执行计数。

常用指令包括 `WSTORE`、`MVM`、`VSET4`、`HALT`，以及 vector arithmetic、memory/control 类指令。**精确 opcode、字段宽度及寄存器位定义以 RTL 和驱动为准**，不要仅根据本文推断二进制编码。

## 3. 工作模式 / Operation Modes

**General programmable mode.** 主机通过 MMIO 写入指令/输入数据；accelerator 逐条执行程序，完成计算后提供状态与结果。可用于包含动态 `WSTORE` 的完整功能演示。

**Resident warm-streaming mode.** 权重已经写入 APIM，短程序驻留在 IMEM；后续多组输入通过 FIFO 与 hardware backpressure 提交。历史 8H 实验采用 `VSET4 → MVM → HALT` compact kernel。

> **历史性能记录（非本次全新克隆实测）**：8H 阶段记录了 14 cycles/input 的 accelerator kernel execution latency，以及 16→32 input 区间约 14.625 cycles/input 的 steady-state incremental throughput。请在下面的 clean-clone 环境重新测量后再用于论文或性能对比。

## 4. 源代码位置 / File Map

| 路径 | 功能 |
| --- | --- |
| `hw/ip_examples/puma_accel/rtl/puma_accel.sv` | accelerator 与 X-HEEP 总线/MMIO 的顶层连接 |
| `hw/ip_examples/puma_accel/rtl/puma_accel_reg_pkg.sv`、`puma_accel_reg_top.sv` | MMIO 寄存器定义及接口 |
| `hw/ip_examples/puma_accel/rtl/puma_like_core.sv` | 指令控制、FSM、寄存器、resident/streaming 逻辑 |
| `hw/ip_examples/puma_accel/rtl/memory_unit.sv` | 局部数据存储/LOAD-STORE |
| `hw/ip_examples/puma_accel/rtl/mvm_unit.sv` | MVM 操作时序/握手 |
| `hw/ip_examples/puma_accel/rtl/mvm_backend.sv` | MVM backend 抽象层 |
| `hw/ip_examples/puma_accel/rtl/pis_mvm_backend.sv` | 单 PIM adapter，包括权重写入及计算握手 |
| `hw/ip_examples/puma_accel/rtl/pislib/APIM.v` | PISLib/APIM 计算模型 |
| `hw/ip_examples/puma_accel/data/puma_accel.hjson` | 寄存器配置的源定义 |
| `hw/ip_examples/puma_accel/puma_accel.core` | FuseSoC 文件列表/依赖 |
| `sw/device/lib/drivers/puma_accel/puma_accel.h` | 主机软件寄存器/driver 接口 |
| `sw/device/lib/drivers/puma_accel/puma_accel.md` | 寄存器/driver 说明 |
| `sw/applications/puma_accel_demo/main.c` | 系统演示与测量 |
| `sw/applications/puma_accel_test/main.c` | 功能回归测试 |
| `tb/testharness.sv.tpl`、`tb/testharness_pkg.sv.tpl`、`tb/x-heep-tb-utils.core` | X-HEEP 仿真集成 |

**MMIO 地址及位域请以 `puma_accel.hjson`、`puma_accel.h` 和 RTL 为准**；此 README 暂不重复维护一套可能失效的地址表。

## 5. 环境与首次运行 / Reproduction

建议使用与此提交兼容的 **X-HEEP Docker/toolchain 环境**，内含 RISC-V 交叉编译器、FuseSoC、Verilator 等。参见 [X-HEEP 官方 Setup](https://x-heep.readthedocs.io/en/latest/GettingStarted/Setup.html)。Linux/WSL 文件系统可保留所需 symlink；不建议在不支持 symlink 的普通 Windows 拷贝目录里直接编译。

```bash
git clone https://github.com/expectica/puma-like-xheep.git
cd puma-like-xheep
git rev-parse HEAD
```

在已激活 **兼容的 X-HEEP 构建环境**后，从仓库根目录运行：

```bash
make mcu-gen
make verilator-build
make app PROJECT=puma_accel_demo
make verilator-run
```

如果通过 `make -C util/docker docker-run` 启动 Docker，应首先确认它把**当前 clone**挂载到容器内的 `/workspace/x-heep`，而不是错误地挂载另一个旧目录。不要把宿主机上的 build 产物当作可复现证明。

可从完整仿真输出查找：

```text
DEMO PASS
ITERATION 8H 4-ENTRY BACKPRESSURED FIFO PASS
Program Finished with value 0
```

以上是**历史实验中的预期成功标志**；具体打印以 `main.c` 和该提交实际运行结果为准。必须检查进程退出码以及是否出现 `FAIL`。最新仓库**尚需完成独立 clean-clone 验证**，不能将这里的步骤直接视为已验证的 CI。

旧的基线 4×4 回归可单独编译：

```bash
make app PROJECT=puma_accel_test
make verilator-run
```

注意两次 `make app` 会替换 `sw/build/` 下的固件。需要运行哪个程序，就先编译哪个。

## 6. 当前限制与下一阶段 / Limitations & Roadmap

**Iteration 8H 当前限制**：一个物理 APIM；4×4 计算路径；resident FIFO 的 operand packing 针对四输入 compact kernel；更大矩阵尚没有完整 tile address map、partial-sum aggregation 和多输出 tile 的软件协议。

**Iteration 9A（计划中）**：两个 4×4 PIM tile 构成一个 4×8 MVM，新增 `pis_mvm_array` 统一包装、路由和加和；旧 4×4 `MVM` 必须继续通过回归。

**Iteration 9B / 后续（计划中）**：把双 input tile 的 resident streaming 与 FIFO packet 协议补齐；随后扩到四个 4×4 PIM、逻辑 8×8，处理输出分块和规模化验证。**计划不代表当前 RTL 已实现。**

## 7. 上游与许可证 / Attribution & Licensing

- **X-HEEP**: https://github.com/x-heep/x-heep ，请保留本仓库现有 `LICENSE` 及适用的上游版权声明。
- **Original X-HEEP README**: https://github.com/x-heep/x-heep#readme
- **PUMA-inspired architecture**：本仓库是独立设计的 PUMA-like functional prototype；不宣称实现完整原始 PUMA ISA、精确电路特性或官方兼容性。
- **PISLib/APIM**：`rtl/pislib/APIM.v` 的来源及独立再分发许可需要确认并正确标注。导师允许研究开源不自动替代第三方代码授权。

本仓库沿用 X-HEEP 的 Git 历史；新增 PUMA 相关文件及 X-HEEP 集成修改可以通过 Git 提交差异识别。
