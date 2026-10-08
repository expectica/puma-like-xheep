module puma_accel #(
    parameter int  DATA_W     = 16,
    parameter int  LANES      = 4,
    parameter int  REG_COUNT  = 8,
    parameter int  IMEM_DEPTH = 32,
    parameter type reg_req_t  = xheep_reg_pkg::xheep_reg_req_t,
    parameter type reg_rsp_t  = xheep_reg_pkg::xheep_reg_rsp_t
) (
    input logic clk_i,
    input logic rst_ni,

    input  reg_req_t reg_req_i,
    output reg_rsp_t reg_rsp_o
);

  localparam int IMEM_AW = $clog2(IMEM_DEPTH);

  puma_accel_reg_pkg::puma_accel_reg2hw_t               reg2hw;
  puma_accel_reg_pkg::puma_accel_hw2reg_t               hw2reg;

  // Iteration 8H: keep the generated register block untouched, but interpose
  // one response signal so RESIDENT_FIFO_PUSH can apply normal MMIO
  // backpressure when the four-entry FIFO is full.
  reg_rsp_t                                              reg_rsp_int;
  logic                                                  resident_fifo_push_access;

  logic                                                 imem_write_d;
  logic                                                 start_pulse;
  logic                                                 imem_write_pulse;
  logic                                                 imem_push_write;
  logic                                                 imem_push_commit_write;
  logic                                                 imem_stream_write;
  logic                                                 core_imem_commit;
  logic                                                 resident_push_write;
  logic                                                 core_resident_ready;
  logic                                                 core_resident_queued;
  logic                                                 resident_fifo_push_write;
  logic                                                 core_resident_fifo_ready;
  logic                                                 core_resident_fifo_empty;
  logic                                                 core_resident_fifo_full;
  logic                                   [        5:0] core_resident_fifo_count;

  logic                                   [IMEM_AW-1:0] auto_addr_q;
  logic                                                 core_imem_we;
  logic                                   [IMEM_AW-1:0] core_imem_addr;
  logic                                   [       31:0] core_imem_wdata;
  logic                                                 core_imem_prog_ready;
  logic                                                 core_imem_prog_bank;
  logic                                                 core_exec_bank_sel;
  logic                                                 core_active_bank;

  logic                                                 core_busy;
  logic                                                 core_done;
  logic                                   [IMEM_AW-1:0] core_pc;
  logic                                   [       31:0] core_ir;
  logic                                   [       15:0] core_vreg_data;

  // Performance-counter signals from the PUMA core.
  logic                                   [       31:0] core_perf_total_cycles;
  logic                                   [       31:0] core_perf_instr;
  logic                                   [       31:0] core_perf_load_cycles;
  logic                                   [       31:0] core_perf_store_cycles;
  logic                                   [       31:0] core_perf_wstore_cycles;
  logic                                   [       31:0] core_perf_mvm_cycles;

  // Iteration 5D: CTRL now exposes reggen's software-write-enable pulse.
  // A software write of START=1 therefore creates exactly one hardware START
  // pulse even when CTRL.q was already 1 from an earlier launch. This removes
  // the old 0 -> 1 -> 0 three-write launch sequence.
  //
  // Keep the legacy IMEM_WRITE edge detector unchanged for debug/fault tests.
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      imem_write_d <= 1'b0;
    end else begin
      imem_write_d <= reg2hw.imem_write.q;
    end
  end

  assign start_pulse = reg2hw.ctrl.qe & reg2hw.ctrl.q;
  assign imem_write_pulse = reg2hw.imem_write.q & ~imem_write_d;

  // IMEM_PUSH and IMEM_PUSH_COMMIT both use reggen's software-write-enable
  // pulse.  PUSH_COMMIT writes the final word exactly like IMEM_PUSH and also
  // queues the selected programming bank for automatic execution.
  assign imem_push_write = reg2hw.imem_push.qe;
  assign imem_push_commit_write = reg2hw.imem_push_commit.qe;
  assign imem_stream_write = imem_push_write | imem_push_commit_write;

  // Iteration 8E-B: one MMIO write carries the complete dynamic resident
  // input word.  The core chooses the inactive bank and handles queueing.
  assign resident_push_write = reg2hw.resident_push.qe;

  // Iteration 8H: software may issue FIFO PUSH writes without a preceding
  // STATUS read. The MMIO response below is held off while the FIFO is full;
  // once space exists, the held write completes and this qe pulse is accepted
  // by the core. The core still keeps its own ready check as a safety guard.
  assign resident_fifo_push_write = reg2hw.resident_fifo_push.qe;

  assign resident_fifo_push_access =
      reg_req_i.valid &&
      reg_req_i.write &&
      (reg_req_i.addr[puma_accel_reg_pkg::BlockAw-1:0] ==
       puma_accel_reg_pkg::PUMA_ACCEL_RESIDENT_FIFO_PUSH_OFFSET);

  // Keep the legacy address/data/write interface for debug and fault
  // injection. Streaming writes select the auto-increment pointer.
  assign core_imem_we = imem_write_pulse | imem_stream_write;
  assign core_imem_addr = imem_stream_write ? auto_addr_q : reg2hw.imem_addr.q[IMEM_AW-1:0];

  assign core_imem_wdata =
      imem_push_commit_write ? reg2hw.imem_push_commit.q :
      imem_push_write        ? reg2hw.imem_push.q :
                               reg2hw.imem_data.q;

  assign core_imem_prog_bank = reg2hw.imem_prog_bank.q;
  assign core_exec_bank_sel = reg2hw.imem_exec_bank.q;

  // A commit is architecturally valid only when the corresponding final IMEM
  // word is accepted.  Rejected active-bank BUSY writes therefore cannot arm
  // an automatic launch.
  assign core_imem_commit = imem_push_commit_write & core_imem_prog_ready;

  // Writing IMEM_ADDR while the selected target bank is programmable seeds
  // the streaming-loader pointer. Each accepted IMEM_PUSH advances it by one.
  // During BUSY, Iteration 4B accepts writes to the inactive bank but rejects
  // writes to the active bank; rejected writes do not advance the pointer.
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      auto_addr_q <= '0;
    end else if (core_imem_prog_ready) begin
      if (reg2hw.imem_addr.qe) begin
        auto_addr_q <= reg2hw.imem_addr.q[IMEM_AW-1:0];
      end else if (imem_stream_write) begin
        auto_addr_q <= auto_addr_q + 1'b1;
      end
    end
  end

  // Generated register block: X-HEEP reg_iface <-> reg2hw/hw2reg.
  puma_accel_reg_top #(
      .reg_req_t(reg_req_t),
      .reg_rsp_t(reg_rsp_t)
  ) puma_accel_reg_top_i (
      .clk_i,
      .rst_ni,
      .reg_req_i,
      .reg_rsp_o(reg_rsp_int),
      .reg2hw,
      .hw2reg,
      .devmode_i(1'b0)
  );

  // Iteration 8H MMIO backpressure.
  //
  // The generated register block normally answers every access immediately.
  // For RESIDENT_FIFO_PUSH only, suppress READY while the small FIFO is full.
  // A standards-compliant reg_iface master keeps the request valid until
  // READY returns. reg2hw.resident_fifo_push.qe may therefore remain asserted
  // during the wait, but puma_like_core accepts it only when FIFO READY is 1.
  // On the first cycle with free space, the write is accepted and the bus
  // transaction completes on that same request.
  always_comb begin
    reg_rsp_o = reg_rsp_int;

    if (resident_fifo_push_access && !core_resident_fifo_ready) begin
      reg_rsp_o.ready = 1'b0;
    end
  end

  puma_like_core #(
      .DATA_W(DATA_W),
      .LANES(LANES),
      .REG_COUNT(REG_COUNT),
      .IMEM_DEPTH(IMEM_DEPTH)
  ) puma_like_core_i (
      .clk_i,
      .rst_ni,

      .start_i(start_pulse),

      .imem_we_i       (core_imem_we),
      .imem_addr_i     (core_imem_addr),
      .imem_wdata_i    (core_imem_wdata),
      .imem_prog_bank_i(core_imem_prog_bank),
      .imem_commit_i   (core_imem_commit),
      .exec_bank_i     (core_exec_bank_sel),

      .resident_push_i  (resident_push_write),
      .resident_wdata_i (reg2hw.resident_push.q),
      .resident_ready_o (core_resident_ready),
      .resident_queued_o(core_resident_queued),

      .resident_fifo_push_i (resident_fifo_push_write),
      .resident_fifo_wdata_i(reg2hw.resident_fifo_push.q),
      .resident_fifo_ready_o(core_resident_fifo_ready),
      .resident_fifo_empty_o(core_resident_fifo_empty),
      .resident_fifo_full_o (core_resident_fifo_full),
      .resident_fifo_count_o(core_resident_fifo_count),

      .imem_prog_ready_o(core_imem_prog_ready),
      .active_bank_o    (core_active_bank),

      .vreg_sel_i (reg2hw.vreg_sel.q),
      .vreg_data_o(core_vreg_data),

      .busy_o(core_busy),
      .done_o(core_done),
      .pc_o  (core_pc),
      .ir_o  (core_ir),

      .perf_total_cycles_o (core_perf_total_cycles),
      .perf_instr_o        (core_perf_instr),
      .perf_load_cycles_o  (core_perf_load_cycles),
      .perf_store_cycles_o (core_perf_store_cycles),
      .perf_wstore_cycles_o(core_perf_wstore_cycles),
      .perf_mvm_cycles_o   (core_perf_mvm_cycles)
  );

  // Continuously mirror accelerator state into the generated RO registers.
  always_comb begin
    hw2reg = '0;

    // STATUS[0] = DONE, STATUS[1] = BUSY
    hw2reg.status.d = {core_busy, core_done};
    hw2reg.status.de = 1'b1;

    hw2reg.vreg_data.d = core_vreg_data;
    hw2reg.vreg_data.de = 1'b1;

    hw2reg.pc.d = '0;
    hw2reg.pc.d[IMEM_AW-1:0] = core_pc;
    hw2reg.pc.de = 1'b1;

    hw2reg.ir.d = core_ir;
    hw2reg.ir.de = 1'b1;

    // The active execution bank is latched by the core on START.
    hw2reg.imem_active_bank.d = core_active_bank;
    hw2reg.imem_active_bank.de = 1'b1;

    // RESIDENT_STATUS[0] = READY, [1] = QUEUED.
    hw2reg.resident_status.d = {core_resident_queued, core_resident_ready};
    hw2reg.resident_status.de = 1'b1;

    // RESIDENT_FIFO_STATUS:
    //   bit 0    READY (not full)
    //   bit 1    EMPTY
    //   bit 2    FULL
    //   bits 8:3 COUNT (0..4)
    hw2reg.resident_fifo_status.d = {
      core_resident_fifo_count,
      core_resident_fifo_full,
      core_resident_fifo_empty,
      core_resident_fifo_ready
    };
    hw2reg.resident_fifo_status.de = 1'b1;

    // Performance counters are read-only from software's point of view.
    hw2reg.perf_total_cycles.d = core_perf_total_cycles;
    hw2reg.perf_total_cycles.de = 1'b1;

    hw2reg.perf_instr.d = core_perf_instr;
    hw2reg.perf_instr.de = 1'b1;

    hw2reg.perf_load_cycles.d = core_perf_load_cycles;
    hw2reg.perf_load_cycles.de = 1'b1;

    hw2reg.perf_store_cycles.d = core_perf_store_cycles;
    hw2reg.perf_store_cycles.de = 1'b1;

    hw2reg.perf_wstore_cycles.d = core_perf_wstore_cycles;
    hw2reg.perf_wstore_cycles.de = 1'b1;

    hw2reg.perf_mvm_cycles.d = core_perf_mvm_cycles;
    hw2reg.perf_mvm_cycles.de = 1'b1;
  end

endmodule
