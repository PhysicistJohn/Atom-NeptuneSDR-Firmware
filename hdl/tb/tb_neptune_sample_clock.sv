`timescale 1ns/1ps
module tb_neptune_sample_clock;
  logic clk = 0;
  logic rst_n = 0;
  logic sample_tick = 0;
  logic counter_set_valid = 0;
  logic [63:0] counter_set_value = 0;
  logic discontinuity_event = 0;
  logic [63:0] current_timestamp;
  logic [63:0] sample_timestamp;
  logic sample_discontinuity;
  logic [31:0] epoch;
  logic observed_discontinuity;

  always #5 clk = ~clk;
  always_ff @(posedge clk)
    if (sample_tick)
      observed_discontinuity <= sample_discontinuity;

  neptune_sample_clock dut (.*);

  task tick;
    begin
      @(negedge clk); sample_tick = 1;
      @(posedge clk); #1;
      @(negedge clk); sample_tick = 0;
    end
  endtask

  initial begin
    repeat (2) @(posedge clk);
    @(negedge clk); rst_n = 1;
    tick();
    if (sample_timestamp !== 64'd1 || observed_discontinuity !== 1'b1)
      $fatal(1, "first sample must be timestamp zero and marked discontinuous");
    tick();
    if (sample_timestamp !== 64'd2 || observed_discontinuity !== 1'b0)
      $fatal(1, "continuous sample clock failed");

    @(negedge clk);
    counter_set_value = 64'd1000;
    counter_set_valid = 1;
    @(posedge clk); #1;
    @(negedge clk); counter_set_valid = 0;
    if (sample_timestamp !== 64'd1000 || epoch !== 32'd1)
      $fatal(1, "explicit counter set failed");
    tick();
    if (sample_timestamp !== 64'd1001 || observed_discontinuity !== 1'b1)
      $fatal(1, "counter set was not marked on next sample");
    $display("tb_neptune_sample_clock PASS");
    $finish;
  end
endmodule
