`timescale 1ns/1ps
module tb_neptune_trigger_controller;
  logic clk = 0;
  logic rst_n = 0;
  logic arm = 0;
  logic disarm = 0;
  logic sample_written = 0;
  logic [63:0] sample_timestamp = 0;
  logic trigger = 0;
  logic [7:0] trigger_source = 8'h22;
  logic [31:0] ring_samples = 8;
  logic [31:0] pretrigger_samples = 2;
  logic [31:0] posttrigger_samples = 2;
  logic armed;
  logic capture_active;
  logic capture_done;
  logic [31:0] write_index;
  logic [31:0] capture_start_index;
  logic [31:0] capture_sample_count;
  logic [63:0] trigger_timestamp;
  logic [7:0] latched_trigger_source;
  logic configuration_error;

  always #5 clk = ~clk;
  neptune_trigger_controller dut (.*);

  task write_sample(input integer stamp, input logic fire);
    begin
      @(negedge clk); sample_timestamp = stamp; trigger = fire; sample_written = 1;
      @(posedge clk); #1;
      @(negedge clk); sample_written = 0; trigger = 0;
    end
  endtask

  initial begin
    repeat (2) @(posedge clk);
    @(negedge clk); rst_n = 1; arm = 1;
    @(posedge clk); #1;
    @(negedge clk); arm = 0;
    if (!armed || configuration_error) $fatal(1, "capture did not arm");
    write_sample(100, 0);
    write_sample(101, 0);
    write_sample(102, 1);
    if (!capture_active || trigger_timestamp !== 102 ||
        capture_start_index !== 0 || capture_sample_count !== 5 ||
        latched_trigger_source !== 8'h22)
      $fatal(1, "trigger metadata mismatch");
    write_sample(103, 0);
    if (capture_done) $fatal(1, "capture ended one sample early");
    write_sample(104, 0);
    if (!capture_done || capture_active) $fatal(1, "capture did not finish");
    $display("tb_neptune_trigger_controller PASS");
    $finish;
  end
endmodule
