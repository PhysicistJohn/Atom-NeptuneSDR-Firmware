`timescale 1ns/1ps
module tb_neptune_coefficient_banks;
  logic clk = 0;
  logic rst_n = 0;
  logic write_enable = 0;
  logic [1:0] write_address = 0;
  logic signed [17:0] write_i = 0;
  logic signed [17:0] write_q = 0;
  logic write_error;
  logic commit_request = 0;
  logic commit_ready;
  logic block_boundary = 0;
  logic [63:0] sample_timestamp = 100;
  logic [1:0] read_address = 0;
  logic signed [17:0] read_i;
  logic signed [17:0] read_q;
  logic active_bank;
  logic [31:0] active_revision;
  logic activation_event;
  logic [63:0] activation_timestamp;

  always #5 clk = ~clk;
  neptune_coefficient_banks #(.COEFFICIENT_COUNT(4)) dut (.*);

  initial begin
    repeat (2) @(posedge clk);
    @(negedge clk); rst_n = 1; write_enable = 1; write_address = 2; write_i = 123; write_q = -45;
    @(posedge clk); #1;
    @(negedge clk); write_enable = 0; commit_request = 1;
    @(posedge clk); #1;
    @(negedge clk); commit_request = 0;
    if (active_bank || active_revision != 0) $fatal(1, "bank swapped before boundary");
    @(negedge clk); block_boundary = 1;
    @(posedge clk); #1;
    @(negedge clk); block_boundary = 0; read_address = 2;
    #1;
    if (!active_bank || active_revision != 1 || !activation_event ||
        activation_timestamp != 100 || $signed(read_i) != 123 || $signed(read_q) != -45)
      $fatal(1, "atomic coefficient bank activation failed");
    $display("tb_neptune_coefficient_banks PASS");
    $finish;
  end
endmodule
