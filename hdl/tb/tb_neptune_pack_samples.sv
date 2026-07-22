`timescale 1ns/1ps
module tb_neptune_pack_samples;
  logic signed [15:0] sample_i;
  logic signed [15:0] sample_q;
  logic signed [7:0] sample_i_s8;
  logic signed [7:0] sample_q_s8;
  logic [31:0] s16;
  logic [23:0] s12p;
  logic [15:0] s8;
  logic [1:0] s12p_clipping;

  neptune_pack_samples dut (.*);

  initial begin
    sample_i = -16'sd2048; sample_q = 16'sd2047;
    sample_i_s8 = -8'sd128; sample_q_s8 = 8'sd127; #1;
    if (s12p !== 24'h7ff800 || s12p_clipping !== 2'b00)
      $fatal(1, "native S12 boundary was not bit exact");
    if (s16 !== {16'sd2047, -16'sd2048} || s8 !== 16'h7f80)
      $fatal(1, "S16/S8 packing mismatch");
    sample_i = -16'sd2049; sample_q = 16'sd2048; #1;
    if (s12p !== 24'h7ff800 || s12p_clipping !== 2'b11)
      $fatal(1, "corrected S12 overflow did not saturate");
    sample_i = 16'sd4095; sample_q = -16'sd4096; #1;
    if (s12p !== 24'h8007ff || s12p_clipping !== 2'b11)
      $fatal(1, "S12 wraparound regression");
    $display("tb_neptune_pack_samples PASS");
    $finish;
  end
endmodule
