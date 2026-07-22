// SPDX-License-Identifier: MIT
// Deterministic round-to-nearest-even followed by saturation.
module neptune_quantize_s8 (
  input  logic signed [15:0] sample_i,
  input  logic signed [15:0] sample_q,
  input  logic [3:0]         right_shift,
  output logic signed [7:0]  quantized_i,
  output logic signed [7:0]  quantized_q,
  output logic [1:0]         clipping
);
  function automatic logic signed [16:0] rounded_shift(
    input logic signed [15:0] value,
    input logic [3:0] shift
  );
    logic signed [16:0] extended;
    logic [16:0] magnitude;
    logic [16:0] quotient;
    logic [16:0] remainder;
    logic [16:0] mask;
    logic [16:0] half;
    begin
      extended = {value[15], value};
      if (shift == 0)
        rounded_shift = extended;
      else begin
        magnitude = extended < 0 ? $unsigned(-extended) : $unsigned(extended);
        quotient = magnitude >> shift;
        mask = (17'd1 << shift) - 17'd1;
        remainder = magnitude & mask;
        half = 17'd1 << (shift - 1'b1);
        if (remainder > half || (remainder == half && quotient[0]))
          quotient = quotient + 17'd1;
        rounded_shift = extended < 0 ? -$signed(quotient) : $signed(quotient);
      end
    end
  endfunction

  logic signed [16:0] shifted_i;
  logic signed [16:0] shifted_q;

  always_comb begin
    shifted_i = rounded_shift(sample_i, right_shift);
    shifted_q = rounded_shift(sample_q, right_shift);
    clipping = 2'b00;
    if (shifted_i > 17'sd127) begin quantized_i = 8'sh7f; clipping[0] = 1'b1; end
    else if (shifted_i < -17'sd128) begin quantized_i = 8'sh80; clipping[0] = 1'b1; end
    else quantized_i = shifted_i[7:0];
    if (shifted_q > 17'sd127) begin quantized_q = 8'sh7f; clipping[1] = 1'b1; end
    else if (shifted_q < -17'sd128) begin quantized_q = 8'sh80; clipping[1] = 1'b1; end
    else quantized_q = shifted_q[7:0];
  end
endmodule
