PUMA X-HEEP RTL step 1

Files:
- puma_accel.sv        : X-HEEP reg_iface/MMIO wrapper
- puma_like_core.sv    : core with START, IMEM programming, VREG readback, BUSY/DONE
- mvm_unit.sv          : previously validated helper module
- memory_unit.sv       : previously validated helper module

Keep the generated files already in hw/ip_examples/puma_accel/rtl:
- puma_accel_reg_pkg.sv
- puma_accel_reg_top.sv
