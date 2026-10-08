/* verilator lint_off UNUSEDPARAM */
module mvm_backend #(
    parameter int DATA_W = 16,
    parameter int LANES  = 4,
    parameter int ACC_W  = 32
) (
    input logic clk_i,
    input logic rst_ni,

    input logic start_i,
    input logic [1:0] matrix_id_i,

    input logic       weight_we_i,
    input logic [9:0] weight_addr_i,
    input logic [7:0] weight_data_i,

    input  logic [LANES*DATA_W-1:0] vec_i_flat,
    output logic [LANES*DATA_W-1:0] vec_o_flat,

    output logic busy_o,
    output logic done_o
);

  // The first PISLib integration uses one APIM weight bank, so matrix_id_i is
  // not yet used for bank selection. Keep it at this boundary for future work.
  logic unused_matrix_id;
  assign unused_matrix_id = ^matrix_id_i;

  pis_mvm_backend #(
      .DATA_W(DATA_W),
      .LANES (LANES)
  ) u_pis_backend (
      .clk_i (clk_i),
      .rst_ni(rst_ni),

      .start_i(start_i),

      .weight_we_i  (weight_we_i),
      .weight_addr_i(weight_addr_i),
      .weight_data_i(weight_data_i),

      .vec_i_flat(vec_i_flat),
      .vec_o_flat(vec_o_flat),

      .busy_o(busy_o),
      .done_o(done_o)
  );

endmodule

/* verilator lint_on UNUSEDPARAM */
