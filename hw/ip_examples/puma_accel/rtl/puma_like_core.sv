/* verilator lint_off WIDTHEXPAND */
module puma_like_core #(
    parameter int DATA_W     = 16,
    parameter int LANES      = 4,
    parameter int REG_COUNT  = 8,
    parameter int IMEM_DEPTH = 32
) (
    input logic clk_i,
    input logic rst_ni,

    // X-HEEP/MMIO control
    input logic start_i,

    // Instruction-memory programming port
    input logic imem_we_i,
    input logic [$clog2(IMEM_DEPTH)-1:0] imem_addr_i,
    input logic [31:0] imem_wdata_i,
    input logic imem_prog_bank_i,
    input logic imem_commit_i,
    input logic exec_bank_i,

    // Iteration 8E-B resident-kernel fast path.  A resident push atomically
    // patches IMEM word 0 of the inactive bank and queues/launches that bank.
    input logic resident_push_i,
    input logic [31:0] resident_wdata_i,
    output logic resident_ready_o,
    output logic resident_queued_o,

    // Iteration 8G: 32-entry resident-input burst FIFO. Software may enqueue
    // an entire N<=32 batch without a per-input status read. The FIFO is
    // consumed automatically at HALT boundaries.
    input logic resident_fifo_push_i,
    input logic [31:0] resident_fifo_wdata_i,
    output logic resident_fifo_ready_o,
    output logic resident_fifo_empty_o,
    output logic resident_fifo_full_o,
    output logic [5:0] resident_fifo_count_o,

    output logic imem_prog_ready_o,
    output logic active_bank_o,

    // Vector-register readback selector:
    //   [2:0] = vector register
    //   [9:8] = lane
    input  logic [ 9:0] vreg_sel_i,
    output logic [15:0] vreg_data_o,

    output logic busy_o,
    output logic done_o,
    output logic [$clog2(IMEM_DEPTH)-1:0] pc_o,
    output logic [31:0] ir_o,

    // Performance counters. These are observation-only signals and do not
    // change the functional execution path.
    output logic [31:0] perf_total_cycles_o,
    output logic [31:0] perf_instr_o,
    output logic [31:0] perf_load_cycles_o,
    output logic [31:0] perf_store_cycles_o,
    output logic [31:0] perf_wstore_cycles_o,
    output logic [31:0] perf_mvm_cycles_o
);

  // ------------------------------------------------------------------------
  // Simplified PUMA-like internal ISA (32-bit starter encoding):
  //
  // [31:28] opcode
  // [27:24] rd
  // [23:20] rs1
  // [19:16] rs2
  // [15: 0] immediate / branch target / matrix id
  // ------------------------------------------------------------------------
  localparam logic [3:0] OP_NOP = 4'h0;
  localparam logic [3:0] OP_SET = 4'h1;
  localparam logic [3:0] OP_VADD = 4'h2;
  localparam logic [3:0] OP_VMUL = 4'h3;
  localparam logic [3:0] OP_RELU = 4'h4;
  localparam logic [3:0] OP_COPY = 4'h5;
  localparam logic [3:0] OP_MVM = 4'h6;
  localparam logic [3:0] OP_JMP = 4'h7;
  localparam logic [3:0] OP_BRNZ = 4'h8;
  localparam logic [3:0] OP_LOAD = 4'h9;
  localparam logic [3:0] OP_STORE = 4'hA;
  localparam logic [3:0] OP_WSTORE = 4'hB;
  // Iteration 8A: compact four-lane input setup. The current PIS backend
  // consumes the low 4 bits of each lane, so four lane values fit exactly
  // in the 16-bit immediate: lane0=imm[3:0] ... lane3=imm[15:12].
  localparam logic [3:0] OP_VSET4 = 4'hC;
  localparam logic [3:0] OP_HALT = 4'hF;


  // Actual vector-register array index width. The ISA still keeps
  // 4-bit rd/rs fields; validity is checked before indexing.
  localparam int unsigned REG_IDX_W = (REG_COUNT <= 1) ? 1 : $clog2(REG_COUNT);
  typedef enum logic [3:0] {
    S_RESET,
    S_IDLE,
    S_FETCH,
    S_DECODE,
    S_EXEC,
    S_LOAD_REQ,
    S_LOAD_WAIT,
    S_LOAD_CAPTURE,
    S_STORE_REQ,
    S_STORE_WAIT,
    S_MVM_START,
    S_MVM_WAIT,
    S_HALT,
    S_WSTORE_REQ
  } state_t;

  state_t        state_q;

  // Iteration 4B: two independent CPU-programmable instruction banks.
  // Programming selects a bank through imem_prog_bank_i. The execution bank
  // is sampled only on START and then remains stable for the complete run.
  // While the core is BUSY, software may program only the inactive bank.
  logic   [31:0] imem                [0:1][0:IMEM_DEPTH-1];
  logic          exec_bank_q;

  // Iteration 7A queued-commit state. A final PUSH_COMMIT received while the
  // current bank is BUSY records the inactive bank as the next job. When the
  // current program reaches HALT, that bank launches automatically.
  logic          queued_bank_valid_q;
  logic          queued_bank_q;

  // Iteration 8H: return the resident-input FIFO to four entries. 8G used a
  // 32-entry FIFO as a characterization vehicle so N<=32 could be issued
  // without producer-side capacity checks. In 8H the small FIFO is retained,
  // while MMIO backpressure (implemented in puma_accel.sv) stalls a PUSH only
  // when this FIFO is full.
  localparam int unsigned RESIDENT_FIFO_DEPTH = 4;
  localparam int unsigned RESIDENT_FIFO_PTR_W = $clog2(RESIDENT_FIFO_DEPTH);
  logic [31:0] resident_fifo_mem[0:RESIDENT_FIFO_DEPTH-1];
  logic [RESIDENT_FIFO_PTR_W-1:0] resident_fifo_rd_ptr_q;
  logic [RESIDENT_FIFO_PTR_W-1:0] resident_fifo_wr_ptr_q;
  logic [5:0] resident_fifo_count_q;
  logic resident_fifo_push_accept;
  logic resident_fifo_pop_accept;

  // Vector register file.
  logic signed [DATA_W-1:0] vreg[0:REG_COUNT-1][0:LANES-1];

  logic [$clog2(IMEM_DEPTH)-1:0] pc_q;
  logic [31:0] ir_q;

  // ------------------------------------------------------------------------
  // Performance counters
  // ------------------------------------------------------------------------
  // total_cycles: every active PUMA FSM cycle from FETCH through the HALT
  // instruction's EXEC cycle.
  // instr:        number of instructions that reach S_EXEC.
  // *_cycles:     cycles spent in the instruction-specific multi-cycle states
  //               in addition to the common FETCH/DECODE/EXEC path.
  logic [31:0] perf_total_cycles_q;
  logic [31:0] perf_instr_q;
  logic [31:0] perf_load_cycles_q;
  logic [31:0] perf_store_cycles_q;
  logic [31:0] perf_wstore_cycles_q;
  logic [31:0] perf_mvm_cycles_q;

  logic [3:0] opcode;
  logic [3:0] rd;
  logic [3:0] rs1;
  logic [3:0] rs2;
  logic [15:0] imm;

  assign opcode = ir_q[31:28];
  assign rd = ir_q[27:24];
  assign rs1 = ir_q[23:20];
  assign rs2 = ir_q[19:16];
  assign imm = ir_q[15:0];

  assign pc_o = pc_q;
  assign ir_o = ir_q;
  assign done_o = (state_q == S_HALT);
  assign busy_o = (state_q != S_IDLE) && (state_q != S_HALT) && (state_q != S_RESET);
  assign active_bank_o = exec_bank_q;

  // Iteration 8E-B fast-path acceptance rule.
  //
  // While a job is running, one resident input may occupy the existing
  // one-entry queued-bank slot.  Once that queued bank launches, READY rises
  // again and software may immediately supply the following input.  HALT with
  // no queued job is also ready: a resident push there launches immediately.
  assign resident_ready_o = (busy_o && !queued_bank_valid_q) || (state_q == S_HALT);

  assign resident_queued_o = queued_bank_valid_q;

  assign resident_fifo_ready_o = (resident_fifo_count_q < RESIDENT_FIFO_DEPTH);
  assign resident_fifo_empty_o = (resident_fifo_count_q == 0);
  assign resident_fifo_full_o = (resident_fifo_count_q == RESIDENT_FIFO_DEPTH);
  assign resident_fifo_count_o = resident_fifo_count_q;

  // Accept a FIFO push whenever capacity exists. A FIFO entry is consumed at
  // HALT only when no older handoff mechanism has priority. This preserves all
  // 8E-B/legacy behavior while making 8F an independent fast path.
  assign resident_fifo_push_accept = resident_fifo_push_i && resident_fifo_ready_o;

  assign resident_fifo_pop_accept =
      (state_q == S_HALT) &&
      !(resident_push_i && resident_ready_o) &&
      !imem_commit_i &&
      !queued_bank_valid_q &&
      (resident_fifo_count_q != 0);

  // Iteration 4B programming ownership rule:
  //
  //   IDLE/HALT : either bank may be programmed.
  //   BUSY      : only the inactive bank may be programmed.
  //
  // Export the exact acceptance condition so the MMIO wrapper can keep
  // its auto-increment loader pointer synchronized with successful writes.
  assign imem_prog_ready_o =
      (state_q == S_IDLE) ||
      (state_q == S_HALT) ||
      (busy_o && (imem_prog_bank_i != exec_bank_q));

  assign perf_total_cycles_o = perf_total_cycles_q;
  assign perf_instr_o = perf_instr_q;
  assign perf_load_cycles_o = perf_load_cycles_q;
  assign perf_store_cycles_o = perf_store_cycles_q;
  assign perf_wstore_cycles_o = perf_wstore_cycles_q;
  assign perf_mvm_cycles_o = perf_mvm_cycles_q;

  // Software-visible selected vector register lane.
  always_comb begin
    vreg_data_o = '0;
    if (({29'b0, vreg_sel_i[2:0]} < REG_COUNT) &&
        ({30'b0, vreg_sel_i[9:8]} < LANES) &&
        (vreg_sel_i[7:3] == 5'b0)) begin
      vreg_data_o = vreg[vreg_sel_i[2:0]][vreg_sel_i[9:8]];
    end
  end

  // ---------------------------- MVMU interface -----------------------------
  logic                           mvm_start_q;
  logic                           mvm_busy;
  logic                           mvm_done;
  logic        [             1:0] mvm_matrix_id_q;
  logic        [             3:0] mvm_dest_q;
  logic        [LANES*DATA_W-1:0] mvm_vec_i_flat;
  logic        [LANES*DATA_W-1:0] mvm_vec_o_flat;

  // PISLib/APIM weight-programming interface used by OP_WSTORE.
  logic                           mvm_weight_we_q;
  logic        [             9:0] mvm_weight_addr_q;
  logic        [             7:0] mvm_weight_data_q;

  // --------------------------- Memory Unit interface -----------------------
  logic                           mem_read_en;
  logic                           mem_write_en;
  logic        [             7:0] mem_addr;
  logic signed [      DATA_W-1:0] mem_wdata;
  logic signed [      DATA_W-1:0] mem_rdata;

  localparam int LANE_IDX_W = (LANES <= 1) ? 1 : $clog2(LANES);

  logic        [ REG_IDX_W-1:0] load_dest_q;
  logic        [LANE_IDX_W-1:0] load_lane_q;
  logic        [           7:0] load_base_q;

  logic        [           7:0] store_addr_q;
  logic signed [    DATA_W-1:0] store_data_q;



  mvm_unit #(
      .DATA_W(DATA_W),
      .LANES (LANES)
  ) u_mvm (
      .clk_i      (clk_i),
      .rst_ni     (rst_ni),
      .start_i    (mvm_start_q),
      .matrix_id_i(mvm_matrix_id_q),

      .weight_we_i  (mvm_weight_we_q),
      .weight_addr_i(mvm_weight_addr_q),
      .weight_data_i(mvm_weight_data_q),

      .vec_i_flat(mvm_vec_i_flat),
      .vec_o_flat(mvm_vec_o_flat),
      .busy_o    (mvm_busy),
      .done_o    (mvm_done)
  );

  memory_unit u_memory (
      .clk_i       (clk_i),
      .rst_ni      (rst_ni),
      .read_en_i   (mem_read_en),
      .write_en_i  (mem_write_en),
      .addr_i      (mem_addr),
      .write_data_i(mem_wdata),
      .read_data_o (mem_rdata)
  );

  integer r;
  integer lane;
  integer imem_bank_idx;
  integer imem_idx;
  integer resident_fifo_idx;

  // ------------------------------- Control FSM -----------------------------
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      state_q                <= S_RESET;
      pc_q                   <= '0;
      ir_q                   <= '0;
      exec_bank_q            <= 1'b0;
      queued_bank_valid_q    <= 1'b0;
      queued_bank_q          <= 1'b0;
      resident_fifo_rd_ptr_q <= '0;
      resident_fifo_wr_ptr_q <= '0;
      resident_fifo_count_q  <= 6'd0;
      mvm_start_q            <= 1'b0;
      mvm_matrix_id_q        <= '0;
      mvm_dest_q             <= '0;
      mvm_weight_we_q        <= 1'b0;
      mvm_weight_addr_q      <= '0;
      mvm_weight_data_q      <= '0;

      mem_read_en            <= 1'b0;
      mem_write_en           <= 1'b0;
      mem_addr               <= '0;
      mem_wdata              <= '0;
      mvm_vec_i_flat         <= '0;
      load_dest_q            <= '0;
      load_lane_q            <= '0;
      load_base_q            <= '0;
      store_addr_q           <= '0;
      store_data_q           <= '0;


      for (r = 0; r < REG_COUNT; r = r + 1) begin
        for (lane = 0; lane < LANES; lane = lane + 1) begin
          vreg[r][lane] <= '0;
        end
      end

      for (imem_bank_idx = 0; imem_bank_idx < 2; imem_bank_idx = imem_bank_idx + 1) begin
        for (imem_idx = 0; imem_idx < IMEM_DEPTH; imem_idx = imem_idx + 1) begin
          imem[imem_bank_idx][imem_idx] <= '0;
        end
      end

      for (
          resident_fifo_idx = 0;
          resident_fifo_idx < RESIDENT_FIFO_DEPTH;
          resident_fifo_idx = resident_fifo_idx + 1
      ) begin
        resident_fifo_mem[resident_fifo_idx] <= '0;
      end
    end else begin
      // External CPU programming port.
      //
      // Iteration 4B permits CPU programming of the inactive IMEM bank
      // while execution is active.  imem_prog_ready_o remains false when
      // software targets the active execution bank, preserving the Iteration 2
      // protection against self-modifying the running program.
      if (imem_we_i && imem_prog_ready_o) begin
        imem[imem_prog_bank_i][imem_addr_i] <= imem_wdata_i;
      end

      // Iteration 8E-B: resident fast path.  Software no longer selects an
      // IMEM bank or rewinds IMEM_ADDR.  Hardware always patches word 0 of
      // the bank opposite the currently executing bank.
      if (resident_push_i && resident_ready_o) begin
        imem[~exec_bank_q][0] <= resident_wdata_i;
      end

      // Iteration 8F FIFO producer/consumer bookkeeping.
      if (resident_fifo_push_accept) begin
        resident_fifo_mem[resident_fifo_wr_ptr_q] <= resident_fifo_wdata_i;
        resident_fifo_wr_ptr_q <= resident_fifo_wr_ptr_q + 1'b1;
      end

      if (resident_fifo_pop_accept) begin
        resident_fifo_rd_ptr_q <= resident_fifo_rd_ptr_q + 1'b1;
      end

      case ({
        resident_fifo_push_accept, resident_fifo_pop_accept
      })
        2'b10:   resident_fifo_count_q <= resident_fifo_count_q + 6'd1;
        2'b01:   resident_fifo_count_q <= resident_fifo_count_q - 6'd1;
        default: resident_fifo_count_q <= resident_fifo_count_q;
      endcase

      // A commit received while execution is active becomes a one-entry
      // queued next-bank launch. If the core is already IDLE/HALT, the state
      // machine below consumes imem_commit_i directly and launches the newly
      // completed bank without first creating a persistent queue entry.
      if (resident_push_i && resident_ready_o && busy_o) begin
        // Atomically queue the inactive bank selected by hardware.
        queued_bank_valid_q <= 1'b1;
        queued_bank_q       <= ~exec_bank_q;
      end else if (imem_commit_i && busy_o) begin
        queued_bank_valid_q <= 1'b1;
        queued_bank_q       <= imem_prog_bank_i;
      end

      // One-cycle internal request defaults.
      mvm_start_q     <= 1'b0;
      mvm_weight_we_q <= 1'b0;
      mem_read_en     <= 1'b0;
      mem_write_en    <= 1'b0;

      case (state_q)
        S_RESET: begin
          pc_q    <= '0;
          ir_q    <= '0;
          state_q <= S_IDLE;
        end

        S_IDLE: begin
          pc_q <= '0;

          // PUSH_COMMIT can also launch a freshly completed program from IDLE.
          // The final IMEM word is written on this clock edge; FETCH occurs on
          // the following edge and therefore observes the committed data.
          if (imem_commit_i) begin
            ir_q                <= '0;
            exec_bank_q         <= imem_prog_bank_i;
            queued_bank_valid_q <= 1'b0;
            state_q             <= S_FETCH;
          end else if (start_i) begin
            ir_q                <= '0;
            exec_bank_q         <= exec_bank_i;
            queued_bank_valid_q <= 1'b0;
            state_q             <= S_FETCH;
          end
        end

        S_FETCH: begin
          ir_q    <= imem[exec_bank_q][pc_q];
          pc_q    <= pc_q + 1'b1;
          state_q <= S_DECODE;
        end

        S_DECODE: begin
          state_q <= S_EXEC;
        end

        S_EXEC: begin
          case (opcode)
            OP_NOP: begin
              state_q <= S_FETCH;
            end

            OP_SET: begin
              if ({28'b0, rd} < REG_COUNT) begin
                for (lane = 0; lane < LANES; lane = lane + 1) begin
                  vreg[rd[REG_IDX_W-1:0]][lane] <= $signed(imm);
                end
              end
              state_q <= S_FETCH;
            end

            OP_VADD: begin
              if (({28'b0, rd} < REG_COUNT) && ({28'b0, rs1} < REG_COUNT) && ({28'b0, rs2} < REG_COUNT)) begin
                for (lane = 0; lane < LANES; lane = lane + 1) begin
                  vreg[rd[REG_IDX_W-1:0]][lane] <= $signed(vreg[rs1[REG_IDX_W-1:0]][lane]) +
                      $signed(vreg[rs2[REG_IDX_W-1:0]][lane]);
                end
              end
              state_q <= S_FETCH;
            end

            OP_VMUL: begin
              if (({28'b0, rd} < REG_COUNT) && ({28'b0, rs1} < REG_COUNT) && ({28'b0, rs2} < REG_COUNT)) begin
                for (lane = 0; lane < LANES; lane = lane + 1) begin
                  vreg[rd[REG_IDX_W-1:0]][lane] <= $signed(vreg[rs1[REG_IDX_W-1:0]][lane]) *
                      $signed(vreg[rs2[REG_IDX_W-1:0]][lane]);
                end
              end
              state_q <= S_FETCH;
            end

            OP_RELU: begin
              if (({28'b0, rd} < REG_COUNT) && ({28'b0, rs1} < REG_COUNT)) begin
                for (lane = 0; lane < LANES; lane = lane + 1) begin
                  if ($signed(vreg[rs1[REG_IDX_W-1:0]][lane]) < 0)
                    vreg[rd[REG_IDX_W-1:0]][lane] <= '0;
                  else vreg[rd[REG_IDX_W-1:0]][lane] <= vreg[rs1[REG_IDX_W-1:0]][lane];
                end
              end
              state_q <= S_FETCH;
            end

            OP_COPY: begin
              if (({28'b0, rd} < REG_COUNT) && ({28'b0, rs1} < REG_COUNT)) begin
                for (lane = 0; lane < LANES; lane = lane + 1) begin
                  vreg[rd[REG_IDX_W-1:0]][lane] <= vreg[rs1[REG_IDX_W-1:0]][lane];
                end
              end
              state_q <= S_FETCH;
            end

            OP_LOAD: begin
              if (rd < REG_COUNT) begin
                load_dest_q <= rd[REG_IDX_W-1:0];
                load_lane_q <= '0;
                load_base_q <= imm[7:0];

                state_q <= S_LOAD_REQ;
              end else begin
                state_q <= S_FETCH;
              end
            end

            OP_STORE: begin
              if ({28'b0, rs1} < REG_COUNT) begin
                store_addr_q <= imm[7:0];
                store_data_q <= vreg[rs1[REG_IDX_W-1:0]][0];
                state_q      <= S_STORE_REQ;
              end else begin
                state_q <= S_FETCH;
              end
            end

            OP_WSTORE: begin
              if ({28'b0, rs1} < REG_COUNT) begin
                // imm[9:0] selects one APIM weight-memory location.
                // The programmed weight is lane 0, low 8 bits, of rs1.
                mvm_weight_addr_q <= imm[9:0];
                mvm_weight_data_q <= vreg[rs1[REG_IDX_W-1:0]][0][7:0];
                state_q           <= S_WSTORE_REQ;
              end else begin
                state_q <= S_FETCH;
              end
            end

            OP_MVM: begin
              if (({28'b0, rd} < REG_COUNT) && ({28'b0, rs1} < REG_COUNT)) begin
                for (lane = 0; lane < LANES; lane = lane + 1) begin
                  mvm_vec_i_flat[lane*DATA_W+:DATA_W] <= vreg[rs1[REG_IDX_W-1:0]][lane];
                end
                mvm_dest_q      <= rd;
                mvm_matrix_id_q <= imm[1:0];
                state_q         <= S_MVM_START;
              end else begin
                state_q <= S_FETCH;
              end
            end

            OP_VSET4: begin
              // Pack one 4-bit value per lane into the instruction immediate.
              // Zero-extension matches the unsigned 4-bit CIM input path used
              // by the current PISLib/APIM integration.
              if ({28'b0, rd} < REG_COUNT) begin
                for (lane = 0; lane < LANES; lane = lane + 1) begin
                  vreg[rd[REG_IDX_W-1:0]][lane] <= {{(DATA_W - 4) {1'b0}}, imm[lane*4+:4]};
                end
              end
              state_q <= S_FETCH;
            end

            OP_JMP: begin
              pc_q    <= imm[$clog2(IMEM_DEPTH)-1:0];
              state_q <= S_FETCH;
            end

            OP_BRNZ: begin
              if (({28'b0, rs1} < REG_COUNT) && (vreg[rs1[REG_IDX_W-1:0]][0] != '0))
                pc_q <= imm[$clog2(IMEM_DEPTH)-1:0];
              state_q <= S_FETCH;
            end

            OP_HALT: begin
              state_q <= S_HALT;
            end

            default: begin
              state_q <= S_FETCH;
            end
          endcase
        end

        // Multi-cycle synchronous LOAD path.
        S_LOAD_REQ: begin

          // Send a read request to memory_unit.
          mem_read_en <= 1'b1;

          mem_addr <= load_base_q + load_lane_q;

          state_q <= S_LOAD_WAIT;
        end


        S_LOAD_WAIT: begin

          // memory_unit performs a synchronous read.
          //
          // The request generated in S_LOAD_REQ reaches
          // memory_unit on the following clock edge.
          // Give read_data_o time to become valid before
          // capturing it into the vector register.

          state_q <= S_LOAD_CAPTURE;
        end


        S_LOAD_CAPTURE: begin

          // Capture the returned memory data.
          vreg[load_dest_q][load_lane_q] <= mem_rdata;

          if (load_lane_q == LANES - 1) begin

            // All vector lanes have been loaded.
            state_q <= S_FETCH;

          end else begin

            // Read the next memory location.
            load_lane_q <= load_lane_q + 1'b1;

            state_q <= S_LOAD_REQ;
          end
        end


        S_STORE_REQ: begin
          // Present the write request to memory_unit.
          mem_write_en <= 1'b1;
          mem_addr     <= store_addr_q;
          mem_wdata    <= store_data_q;

          state_q      <= S_STORE_WAIT;
        end

        S_STORE_WAIT: begin
          // memory_unit observes the registered write request at this
          // clock edge and performs the synchronous write. The default
          // assignments above deassert mem_write_en after this request.
          state_q <= S_FETCH;
        end

        S_WSTORE_REQ: begin
          // Hold the request until the PIS backend has accepted it and entered
          // its active weight-write phase.  Once mvm_busy is observed, the
          // backend is already in S_WEIGHT_WRITE; APIM commits the write on
          // this same clock edge.  The core can therefore release WSTORE and
          // begin fetching the next instruction immediately instead of
          // waiting for the backend's S_WEIGHT_DONE -> S_IDLE cleanup.
          //
          // Backend cleanup now overlaps the next instruction's FETCH/DECODE.
          mvm_weight_we_q <= 1'b1;

          if (mvm_busy) begin
            mvm_weight_we_q <= 1'b0;
            state_q         <= S_FETCH;
          end
        end

        S_MVM_START: begin
          mvm_start_q <= 1'b1;
          state_q     <= S_MVM_WAIT;
        end

        S_MVM_WAIT: begin
          if (mvm_done && !mvm_busy) begin
            if ({28'b0, mvm_dest_q} < REG_COUNT) begin
              for (lane = 0; lane < LANES; lane = lane + 1) begin
                vreg[mvm_dest_q[REG_IDX_W-1:0]][lane] <=
                    $signed(mvm_vec_o_flat[lane*DATA_W+:DATA_W]);
              end
            end
            state_q <= S_FETCH;
          end
        end

        // Without a queued job, DONE remains asserted until software starts
        // another run. A queued commit has priority and turns HALT into a
        // one-cycle handoff point between the two instruction banks.
        S_HALT: begin
          if (resident_push_i && resident_ready_o) begin
            // Iteration 8E-B: if software arrives after the previous job has
            // already halted, patch the opposite bank and launch it directly.
            // The IMEM write occurs on this same edge; FETCH reads it on the
            // following edge.
            pc_q                <= '0;
            ir_q                <= '0;
            exec_bank_q         <= ~exec_bank_q;
            queued_bank_valid_q <= 1'b0;
            state_q             <= S_FETCH;
          end else if (imem_commit_i) begin
            // Current job already finished before the final word arrived:
            // launch the just-committed programming bank immediately.
            pc_q                <= '0;
            ir_q                <= '0;
            exec_bank_q         <= imem_prog_bank_i;
            queued_bank_valid_q <= 1'b0;
            state_q             <= S_FETCH;
          end else if (queued_bank_valid_q) begin
            // Final word arrived while the previous job was BUSY.
            pc_q                <= '0;
            ir_q                <= '0;
            exec_bank_q         <= queued_bank_q;
            queued_bank_valid_q <= 1'b0;
            state_q             <= S_FETCH;
          end else if (resident_fifo_count_q != 0) begin
            // Iteration 8F: consume one queued input, patch word 0 of the
            // opposite resident bank, and launch it immediately. The write
            // occurs on this edge; S_FETCH observes it on the next edge.
            imem[~exec_bank_q][0] <= resident_fifo_mem[resident_fifo_rd_ptr_q];
            pc_q                  <= '0;
            ir_q                  <= '0;
            exec_bank_q           <= ~exec_bank_q;
            queued_bank_valid_q   <= 1'b0;
            state_q               <= S_FETCH;
          end else if (start_i) begin
            pc_q                <= '0;
            ir_q                <= '0;
            exec_bank_q         <= exec_bank_i;
            queued_bank_valid_q <= 1'b0;
            state_q             <= S_FETCH;
          end
        end

        default: begin
          state_q <= S_RESET;
        end
      endcase
    end
  end

  // -------------------------- Performance counters -------------------------
  // Reset the counters at the beginning of every new accelerator run, then
  // keep the final values stable while the core remains in S_HALT.
  //
  // This block deliberately does not drive any architectural state (PC, IR,
  // vector registers, memories, or MVM control), so adding the counters does
  // not alter the functional behavior of the accelerator.
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      perf_total_cycles_q  <= 32'd0;
      perf_instr_q         <= 32'd0;
      perf_load_cycles_q   <= 32'd0;
      perf_store_cycles_q  <= 32'd0;
      perf_wstore_cycles_q <= 32'd0;
      perf_mvm_cycles_q    <= 32'd0;
    end else if (
        (((state_q == S_IDLE) || (state_q == S_HALT)) && start_i) ||
        (((state_q == S_IDLE) || (state_q == S_HALT)) && imem_commit_i) ||
        ((state_q == S_HALT) && resident_push_i && resident_ready_o) ||
        ((state_q == S_HALT) && queued_bank_valid_q) ||
        ((state_q == S_HALT) && resident_fifo_count_q != 0)
    ) begin
      // Reset on manual START and on both forms of Iteration 7A automatic
      // launch so every chained job still gets independent performance data.
      perf_total_cycles_q  <= 32'd0;
      perf_instr_q         <= 32'd0;
      perf_load_cycles_q   <= 32'd0;
      perf_store_cycles_q  <= 32'd0;
      perf_wstore_cycles_q <= 32'd0;
      perf_mvm_cycles_q    <= 32'd0;
    end else begin
      // Count every active accelerator cycle. S_IDLE, S_RESET and S_HALT are
      // excluded. The S_EXEC cycle of OP_HALT is included.
      if ((state_q != S_IDLE) && (state_q != S_HALT) && (state_q != S_RESET)) begin
        perf_total_cycles_q <= perf_total_cycles_q + 32'd1;
      end

      // Every instruction in the current non-pipelined core reaches S_EXEC
      // exactly once, so S_EXEC is a convenient baseline retirement point.
      if (state_q == S_EXEC) begin
        perf_instr_q <= perf_instr_q + 32'd1;
      end

      // Instruction-specific extra states beyond FETCH/DECODE/EXEC.
      if ((state_q == S_LOAD_REQ) || (state_q == S_LOAD_WAIT) || (state_q == S_LOAD_CAPTURE)) begin
        perf_load_cycles_q <= perf_load_cycles_q + 32'd1;
      end

      if ((state_q == S_STORE_REQ) || (state_q == S_STORE_WAIT)) begin
        perf_store_cycles_q <= perf_store_cycles_q + 32'd1;
      end

      if (state_q == S_WSTORE_REQ) begin
        perf_wstore_cycles_q <= perf_wstore_cycles_q + 32'd1;
      end

      if ((state_q == S_MVM_START) || (state_q == S_MVM_WAIT)) begin
        perf_mvm_cycles_q <= perf_mvm_cycles_q + 32'd1;
      end
    end
  end

`ifndef SYNTHESIS
`ifdef PUMA_DEBUG

  always @(posedge clk_i) begin
    if (rst_ni) begin

      if (state_q == S_WSTORE_REQ && mvm_weight_we_q) begin
        $display("COREDBG WSTORE addr=%0d data=%0d busy=%0b time=%0t", mvm_weight_addr_q,
                 mvm_weight_data_q, mvm_busy, $time);
      end

      if (state_q == S_MVM_START) begin
        $display(
            "COREDBG MVM_START dest=v%0d in={%0d,%0d,%0d,%0d}", mvm_dest_q,
            $signed(mvm_vec_i_flat[0*DATA_W+:DATA_W]), $signed(mvm_vec_i_flat[1*DATA_W+:DATA_W]),
            $signed(mvm_vec_i_flat[2*DATA_W+:DATA_W]), $signed(mvm_vec_i_flat[3*DATA_W+:DATA_W]));
      end

      if ((state_q == S_MVM_WAIT) && mvm_done && !mvm_busy) begin
        $display(
            "COREDBG MVM_WRITEBACK dest=v%0d out={%0d,%0d,%0d,%0d}", mvm_dest_q,
            $signed(mvm_vec_o_flat[0*DATA_W+:DATA_W]), $signed(mvm_vec_o_flat[1*DATA_W+:DATA_W]),
            $signed(mvm_vec_o_flat[2*DATA_W+:DATA_W]), $signed(mvm_vec_o_flat[3*DATA_W+:DATA_W]));
      end

    end
  end

`endif  // PUMA_DEBUG
`endif  // SYNTHESIS

endmodule

/* verilator lint_on WIDTHEXPAND */
