`timescale 1ns/1ps
module tb_neptune_quantize_s8;
  logic signed [15:0] sample_i;
  logic signed [15:0] sample_q;
  logic [3:0] right_shift;
  logic signed [7:0] quantized_i;
  logic signed [7:0] quantized_q;
  logic [1:0] clipping;

  neptune_quantize_s8 dut (.*);

  task check(
    input integer i,
    input integer q,
    input integer shift,
    input integer expected_i,
    input integer expected_q,
    input logic [1:0] expected_clip
  );
    begin
      sample_i = i;
      sample_q = q;
      right_shift = shift;
      #1;
      if ($signed(quantized_i) !== expected_i ||
          $signed(quantized_q) !== expected_q || clipping !== expected_clip)
        $fatal(1, "quantize mismatch i=%0d q=%0d shift=%0d got=(%0d,%0d,%b)",
               i, q, shift, $signed(quantized_i), $signed(quantized_q), clipping);
    end
  endtask

  initial begin
    check(-127, 127, 8, 0, 0, 2'b00);
    check(-128, 128, 8, 0, 0, 2'b00);
    check(-384, 384, 8, -2, 2, 2'b00);
    check(-32768, 32767, 8, -128, 127, 2'b10);
    check(-129, 129, 8, -1, 1, 2'b00);
    check(-200, 200, 0, -128, 127, 2'b11);
    $display("tb_neptune_quantize_s8 PASS");
    $finish;
  end
endmodule
