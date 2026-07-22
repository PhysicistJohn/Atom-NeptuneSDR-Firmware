`timescale 1ns/1ps
module tb_neptune_cross_accumulator;
  logic clk = 0;
  logic rst_n = 0;
  logic sample_valid = 0;
  logic block_end = 0;
  logic signed [15:0] ch0_i = 1;
  logic signed [15:0] ch0_q = 2;
  logic signed [15:0] ch1_i = 3;
  logic signed [15:0] ch1_q = 4;
  logic [63:0] sample_timestamp = 50;
  logic result_valid;
  logic [63:0] first_sample_timestamp;
  logic [31:0] sample_count;
  logic [63:0] ch0_power;
  logic [63:0] ch1_power;
  logic signed [63:0] correlation_i;
  logic signed [63:0] correlation_q;

  always #5 clk = ~clk;
  neptune_cross_accumulator dut (.*);

  initial begin
    repeat (2) @(posedge clk);
    @(negedge clk); rst_n = 1; sample_valid = 1;
    @(posedge clk); #1;
    @(negedge clk); sample_timestamp = 51; block_end = 1;
    @(posedge clk); #1;
    if (!result_valid || sample_count !== 2 || first_sample_timestamp !== 50 ||
        ch0_power !== 10 || ch1_power !== 50 ||
        $signed(correlation_i) !== 22 || $signed(correlation_q) !== 4)
      $fatal(1, "cross accumulator mismatch");
    $display("tb_neptune_cross_accumulator PASS");
    $finish;
  end
endmodule
