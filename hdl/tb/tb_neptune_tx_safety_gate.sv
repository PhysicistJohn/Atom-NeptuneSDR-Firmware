`timescale 1ns/1ps
module tb_neptune_tx_safety_gate;
  logic clk = 0;
  logic rst_n = 0;
  logic persistent_inhibit = 0;
  logic hard_disable = 0;
  logic thermal_fault = 0;
  logic watchdog_fault = 0;
  logic rf_pll_locked = 1;
  logic authenticated_arm_pulse = 0;
  logic authenticated_disarm_pulse = 0;
  logic sample_valid = 1;
  logic signed [15:0] sample_i = 123;
  logic signed [15:0] sample_q = -456;
  logic tx_armed;
  logic tx_sample_valid;
  logic signed [15:0] tx_sample_i;
  logic signed [15:0] tx_sample_q;
  logic [31:0] disarm_revision;
  logic [7:0] last_disarm_reason;

  always #5 clk = ~clk;
  neptune_tx_safety_gate dut (.*);

  initial begin
    repeat (2) @(posedge clk);
    if (tx_armed || tx_sample_valid || tx_sample_i !== 0 || tx_sample_q !== 0)
      $fatal(1, "TX was not safe during reset");
    @(negedge clk); rst_n = 1; authenticated_arm_pulse = 1;
    @(posedge clk); #1;
    @(negedge clk); authenticated_arm_pulse = 0;
    if (!tx_armed || !tx_sample_valid || tx_sample_i !== 123 || $signed(tx_sample_q) !== -456)
      $fatal(1, "authenticated TX arm failed");
    @(negedge clk); thermal_fault = 1;
    #1;
    if (tx_sample_valid || tx_sample_i !== 0 || tx_sample_q !== 0)
      $fatal(1, "asynchronous effective TX gate did not zero output");
    @(posedge clk); #1;
    if (tx_armed || last_disarm_reason !== 8'h05 || disarm_revision !== 2)
      $fatal(1, "thermal fault did not latch disarm");
    @(negedge clk); thermal_fault = 0; persistent_inhibit = 1; authenticated_arm_pulse = 1;
    @(posedge clk); #1;
    if (tx_armed) $fatal(1, "persistent inhibit allowed arm");
    $display("tb_neptune_tx_safety_gate PASS");
    $finish;
  end
endmodule
