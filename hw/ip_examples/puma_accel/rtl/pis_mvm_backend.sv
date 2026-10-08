/* verilator lint_off UNUSEDSIGNAL */
module pis_mvm_backend #(
    parameter int DATA_W = 16,
    parameter int LANES  = 4
) (
    input logic clk_i,
    input logic rst_ni,

    input logic start_i,

    // APIM weight-memory programming interface.
    input logic       weight_we_i,
    input logic [9:0] weight_addr_i,
    input logic [7:0] weight_data_i,

    input  logic [LANES*DATA_W-1:0] vec_i_flat,
    output logic [LANES*DATA_W-1:0] vec_o_flat,

    output logic busy_o,
    output logic done_o
);

  // This first wrapper targets the four-input/four-output portion of
  // Basic_GeMM_CIM used by the previous verified standalone integration.
  initial begin
    if (LANES != 4) begin
      $error("pis_mvm_backend currently requires LANES=4");
    end
    if (DATA_W < 14) begin
      $error("pis_mvm_backend currently requires DATA_W>=14");
    end
  end

  // --------------------------- PISLib APIM interface -----------------------
  logic [ 9:0] apim_addr;
  logic        apim_cs;
  logic        apim_web;
  logic        apim_cimeb;
  logic [ 7:0] apim_d;
  wire  [ 7:0] apim_q;

  logic [ 3:0] cim_in0;
  logic [ 3:0] cim_in1;
  logic [ 3:0] cim_in2;
  logic [ 3:0] cim_in3;

  wire  [13:0] cim_out0;
  wire  [13:0] cim_out1;
  wire  [13:0] cim_out2;
  wire  [13:0] cim_out3;
  wire  [13:0] cim_out4;
  wire  [13:0] cim_out5;
  wire  [13:0] cim_out6;
  wire  [13:0] cim_out7;

  // Keep otherwise-unused APIM outputs consumed for lint cleanliness.
  logic        unused_apim_outputs;
  assign unused_apim_outputs = ^apim_q ^ ^cim_out4 ^ ^cim_out5 ^ ^cim_out6 ^ ^cim_out7;

  typedef enum logic [2:0] {
    S_IDLE,
    S_WEIGHT_WRITE,
    S_WEIGHT_DONE,
    S_CIM_REQUEST,
    S_CIM_WAIT,
    S_DONE
  } state_t;

  state_t state_q;

  logic [LANES*DATA_W-1:0] vec_latched_q;
  logic [9:0] weight_addr_q;
  logic [7:0] weight_data_q;

  // Override ADC_PRECISION to 14 so the full 4x(4-bit x 8-bit) accumulated
  // result is visible rather than the default 6-bit truncated value.
  Basic_GeMM_CIM #(
      .DATA_WIDTH            (8),
      .ADDR_WIDTH            (10),
      .ADC_PRECISION         (14),
      .CIM_INPUT_PRECISION   (4),
      .CIM_INPUT_PARALLELISM (4),
      .CIM_OUTPUT_PARALLELISM(8)
  ) u_apim (
      .q       (apim_q),
      .cim_out0(cim_out0),
      .cim_out1(cim_out1),
      .cim_out2(cim_out2),
      .cim_out3(cim_out3),
      .cim_out4(cim_out4),
      .cim_out5(cim_out5),
      .cim_out6(cim_out6),
      .cim_out7(cim_out7),

      .clk  (clk_i),
      .a    (apim_addr),
      .cs   (apim_cs),
      .web  (apim_web),
      .cimeb(apim_cimeb),
      .d    (apim_d),

      .cim_in0(cim_in0),
      .cim_in1(cim_in1),
      .cim_in2(cim_in2),
      .cim_in3(cim_in3)
  );

  // Drive APIM according to the wrapper FSM.
  always_comb begin
    apim_cs    = 1'b0;
    apim_web   = 1'b1;
    apim_cimeb = 1'b1;
    apim_addr  = 10'd0;
    apim_d     = 8'd0;

    // PISLib APIM accepts four 4-bit CIM inputs. The current accelerator lanes
    // are 16-bit, so this first integration intentionally uses their low 4 bits.
    cim_in0    = vec_latched_q[0*DATA_W+:4];
    cim_in1    = vec_latched_q[1*DATA_W+:4];
    cim_in2    = vec_latched_q[2*DATA_W+:4];
    cim_in3    = vec_latched_q[3*DATA_W+:4];

    case (state_q)
      S_WEIGHT_WRITE: begin
        apim_cs    = 1'b1;
        apim_web   = 1'b0;  // APIM write is active-low.
        apim_cimeb = 1'b1;  // Memory mode.
        apim_addr  = weight_addr_q;
        apim_d     = weight_data_q;
      end

      S_CIM_REQUEST: begin
        apim_cs    = 1'b1;
        apim_web   = 1'b1;
        apim_cimeb = 1'b0;  // CIM mode.
        apim_addr  = 10'd0;
      end

      default: begin
        // Keep APIM idle.
      end
    endcase
  end

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      state_q       <= S_IDLE;
      vec_latched_q <= '0;
      vec_o_flat    <= '0;
      weight_addr_q <= '0;
      weight_data_q <= '0;
    end else begin
      case (state_q)
        S_IDLE: begin
          // Give weight programming priority if both requests were ever
          // presented together.
          if (weight_we_i) begin
            weight_addr_q <= weight_addr_i;
            weight_data_q <= weight_data_i;
            state_q       <= S_WEIGHT_WRITE;
          end else if (start_i) begin
            vec_latched_q <= vec_i_flat;
            state_q       <= S_CIM_REQUEST;
          end
        end

        S_WEIGHT_WRITE: begin
          // The APIM write occurs on this clock edge.
          state_q <= S_WEIGHT_DONE;
        end

        S_WEIGHT_DONE: begin
          state_q <= S_IDLE;
        end

        S_CIM_REQUEST: begin
          // APIM performs the CIM operation on this clock edge.
          state_q <= S_CIM_WAIT;
        end

        S_CIM_WAIT: begin
          // Capture the first four APIM outputs one cycle after the request.
          vec_o_flat[0*DATA_W+:DATA_W] <= {{(DATA_W - 14) {1'b0}}, cim_out0};
          vec_o_flat[1*DATA_W+:DATA_W] <= {{(DATA_W - 14) {1'b0}}, cim_out1};
          vec_o_flat[2*DATA_W+:DATA_W] <= {{(DATA_W - 14) {1'b0}}, cim_out2};
          vec_o_flat[3*DATA_W+:DATA_W] <= {{(DATA_W - 14) {1'b0}}, cim_out3};
          state_q <= S_DONE;
        end

        S_DONE: begin
          state_q <= S_IDLE;
        end

        default: begin
          state_q <= S_IDLE;
        end
      endcase
    end
  end

  always_comb begin
    busy_o = (state_q != S_IDLE) && (state_q != S_DONE);
    done_o = (state_q == S_DONE);
  end

`ifndef SYNTHESIS
`ifdef PUMA_DEBUG

  always @(posedge clk_i) begin
    if (rst_ni) begin

      // One line should appear for every accepted WSTORE.
      if (state_q == S_WEIGHT_WRITE) begin
        $display("PISDBG WRITE addr=%0d data=%0d cs=%0b web=%0b cimeb=%0b time=%0t", weight_addr_q,
                 weight_data_q, apim_cs, apim_web, apim_cimeb, $time);
      end

      // At MVM request time, inspect both inputs and the exact
      // 16 APIM locations that our test program should have written.
      if (state_q == S_CIM_REQUEST) begin
        $display("PISDBG CIM_IN = {%0d,%0d,%0d,%0d}", cim_in0, cim_in1, cim_in2, cim_in3);

        $display("PISDBG MEM_OUT0 = {%0d,%0d,%0d,%0d}", u_apim.mem[0], u_apim.mem[256],
                 u_apim.mem[512], u_apim.mem[768]);

        $display("PISDBG MEM_OUT1 = {%0d,%0d,%0d,%0d}", u_apim.mem[4], u_apim.mem[260],
                 u_apim.mem[516], u_apim.mem[772]);

        $display("PISDBG MEM_OUT2 = {%0d,%0d,%0d,%0d}", u_apim.mem[8], u_apim.mem[264],
                 u_apim.mem[520], u_apim.mem[776]);

        $display("PISDBG MEM_OUT3 = {%0d,%0d,%0d,%0d}", u_apim.mem[12], u_apim.mem[268],
                 u_apim.mem[524], u_apim.mem[780]);
      end

      // These are the values that leave Basic_GeMM_CIM.
      if (state_q == S_CIM_WAIT) begin
        $display("PISDBG CIM_OUT = {%0d,%0d,%0d,%0d}", cim_out0, cim_out1, cim_out2, cim_out3);
      end

    end
  end

`endif  // PUMA_DEBUG
`endif  // SYNTHESIS

endmodule

/* verilator lint_on UNUSEDSIGNAL */
