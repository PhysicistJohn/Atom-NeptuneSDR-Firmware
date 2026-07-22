// SPDX-License-Identifier: MIT
module neptune_nco (
  input  logic        clk,
  input  logic        rst_n,
  input  logic        sample_tick,
  input  logic        phase_reset,
  input  logic [31:0] phase_offset,
  input  logic [31:0] phase_increment,
  output logic [31:0] phase
);
  always_ff @(posedge clk) begin
    if (!rst_n || phase_reset)
      phase <= phase_offset;
    else if (sample_tick)
      phase <= phase + phase_increment;
  end
endmodule
