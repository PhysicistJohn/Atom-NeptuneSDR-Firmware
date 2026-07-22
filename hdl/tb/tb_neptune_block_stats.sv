`timescale 1ns/1ps
module tb_neptune_block_stats;
  logic clk = 0;
  logic rst_n = 0;
  logic sample_valid = 0;
  logic block_end = 0;
  logic signed [15:0] sample_i = 0;
  logic signed [15:0] sample_q = 0;
  logic [1:0] sample_clipping = 0;
  logic stats_valid;
  logic [31:0] block_sample_count;
  logic [15:0] block_peak;
  logic [63:0] block_sum_squares;
  logic [31:0] block_clipping_count;

  always #5 clk = ~clk;
  neptune_block_stats dut (.*);

  initial begin
    repeat (2) @(posedge clk);
    @(negedge clk); rst_n = 1;
    @(negedge clk);
    sample_valid = 1; sample_i = 3; sample_q = 4; sample_clipping = 2'b11;
    @(posedge clk); #1;
    @(negedge clk);
    sample_i = -5; sample_q = 0; sample_clipping = 2'b01; block_end = 1;
    @(posedge clk); #1;
    if (!stats_valid || block_sample_count !== 2 || block_peak !== 5 ||
        block_sum_squares !== 50 || block_clipping_count !== 3)
      $fatal(1, "scalar clipping/statistics mismatch");
    @(negedge clk); sample_valid = 0; block_end = 0;
    @(posedge clk); #1;
    if (stats_valid) $fatal(1, "stats_valid was not a pulse");
    $display("tb_neptune_block_stats PASS");
    $finish;
  end
endmodule
