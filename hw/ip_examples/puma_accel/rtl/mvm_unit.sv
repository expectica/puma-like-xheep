module mvm_unit #(
    parameter int DATA_W = 16,
    parameter int LANES  = 4,
    parameter int ACC_W  = 32
) (
    input logic clk_i,
    input logic rst_ni,

    input logic start_i,
    input logic [1:0] matrix_id_i,

    // PISLib/APIM weight-programming interface.
    input logic       weight_we_i,
    input logic [9:0] weight_addr_i,
    input logic [7:0] weight_data_i,

    input  logic [LANES*DATA_W-1:0] vec_i_flat,
    output logic [LANES*DATA_W-1:0] vec_o_flat,

    output logic busy_o,
    output logic done_o
);

  // Keep the core-facing MVMU interface stable. The actual computation is
  // delegated to a backend so the implementation can be changed independently.
  mvm_backend #(
      .DATA_W(DATA_W),
      .LANES (LANES),
      .ACC_W (ACC_W)
  ) u_backend (
      .clk_i      (clk_i),
      .rst_ni     (rst_ni),
      .start_i    (start_i),
      .matrix_id_i(matrix_id_i),

      .weight_we_i  (weight_we_i),
      .weight_addr_i(weight_addr_i),
      .weight_data_i(weight_data_i),

      .vec_i_flat(vec_i_flat),
      .vec_o_flat(vec_o_flat),
      .busy_o    (busy_o),
      .done_o    (done_o)
  );

endmodule
