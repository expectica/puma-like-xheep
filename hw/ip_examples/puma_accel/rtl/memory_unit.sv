module memory_unit #(
    parameter DATA_W = 16,
    parameter DEPTH  = 256
) (
    input logic clk_i,
    input logic rst_ni,

    input logic read_en_i,
    input logic write_en_i,

    input logic [7:0] addr_i,

    input logic signed [DATA_W-1:0] write_data_i,

    output logic signed [DATA_W-1:0] read_data_o
);


  logic signed [DATA_W-1:0] mem[0:DEPTH-1];


  integer i;


  always_ff @(posedge clk_i or negedge rst_ni) begin

    if (!rst_ni) begin

      read_data_o <= '0;

      for (i = 0; i < DEPTH; i = i + 1) mem[i] <= '0;

    end else begin


      if (write_en_i) mem[addr_i] <= write_data_i;


      if (read_en_i) read_data_o <= mem[addr_i];


    end

  end


endmodule
