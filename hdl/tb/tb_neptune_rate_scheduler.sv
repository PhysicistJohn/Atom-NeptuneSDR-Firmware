`timescale 1ns/1ps
module tb_neptune_rate_scheduler;
  logic clk = 0;
  logic rst_n = 0;
  logic input_tick = 0;
  logic emit_output;
  logic [10:0] phase;
  logic [63:0] input_count;
  logic [63:0] output_count;
  integer observed_outputs = 0;

  always #5 clk = ~clk;
  neptune_rate_scheduler dut (.*);

  initial begin
    repeat (2) @(posedge clk);
    @(negedge clk); rst_n = 1; input_tick = 1;
    repeat (1536) begin
      @(posedge clk); #1;
      if (emit_output) observed_outputs = observed_outputs + 1;
    end
    @(negedge clk); input_tick = 0;
    if (input_count !== 64'd1536 || output_count !== 64'd1375 ||
        observed_outputs !== 1375)
      $fatal(1, "rate ratio mismatch in=%0d out=%0d observed=%0d",
             input_count, output_count, observed_outputs);
    $display("tb_neptune_rate_scheduler PASS");
    $finish;
  end
endmodule
