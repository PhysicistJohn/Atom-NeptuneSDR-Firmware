`timescale 1ns/1ps
module tb_neptune_cic_decimator;
  logic clk = 0;
  logic rst_n = 0;
  logic flush = 0;
  logic sample_valid = 0;
  logic signed [15:0] sample_i = 1;
  logic signed [15:0] sample_q = -1;
  logic [15:0] rate = 2;
  logic [5:0] output_right_shift = 0;
  logic output_valid;
  logic signed [15:0] output_i;
  logic signed [15:0] output_q;
  logic clipped;
  logic configuration_error;
  integer outputs = 0;

  always #5 clk = ~clk;
  neptune_cic_decimator #(.STAGES(1), .ACCUMULATOR_BITS(32), .MAX_RATE(8)) dut (.*);

  always @(posedge clk) begin
    #1;
    if (output_valid) begin
      if ($signed(output_i) !== 2 || $signed(output_q) !== -2 || clipped)
        $fatal(1, "CIC output mismatch");
      outputs = outputs + 1;
    end
  end

  initial begin
    repeat (2) @(posedge clk);
    @(negedge clk); rst_n = 1; sample_valid = 1;
    repeat (4) @(posedge clk);
    #1;
    if (outputs != 2 || configuration_error)
      $fatal(1, "CIC rate mismatch outputs=%0d", outputs);
    $display("tb_neptune_cic_decimator PASS");
    $finish;
  end
endmodule
