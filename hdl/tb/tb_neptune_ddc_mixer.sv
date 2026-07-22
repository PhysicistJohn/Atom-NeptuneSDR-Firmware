`timescale 1ns/1ps
module tb_neptune_ddc_mixer;
  logic bypass;
  logic signed [15:0] sample_i;
  logic signed [15:0] sample_q;
  logic signed [15:0] cosine;
  logic signed [15:0] sine;
  logic signed [15:0] mixed_i;
  logic signed [15:0] mixed_q;
  logic clipped;

  neptune_ddc_mixer dut (.*);

  initial begin
    bypass = 0; sample_i = 1000; sample_q = -500; cosine = 32767; sine = 0; #1;
    if ($signed(mixed_i) !== 1000 || $signed(mixed_q) !== -500 || clipped)
      $fatal(1, "zero-phase DDC mismatch");
    cosine = 0; sine = 32767; #1;
    if ($signed(mixed_i) !== -500 || $signed(mixed_q) !== -1000 || clipped)
      $fatal(1, "quarter-cycle DDC mismatch");
    bypass = 1; cosine = 123; sine = 456; #1;
    if ($signed(mixed_i) !== 1000 || $signed(mixed_q) !== -500)
      $fatal(1, "DDC bypass mismatch");
    $display("tb_neptune_ddc_mixer PASS");
    $finish;
  end
endmodule
