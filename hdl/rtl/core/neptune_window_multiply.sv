// SPDX-License-Identifier: MIT
// Real Q1.17 window coefficient applied to complex S16 input.
module neptune_window_multiply (
  input  logic               bypass,
  input  logic signed [15:0] sample_i,
  input  logic signed [15:0] sample_q,
  input  logic signed [17:0] coefficient,
  output logic signed [15:0] windowed_i,
  output logic signed [15:0] windowed_q,
  output logic               clipped
);
  logic signed [33:0] product_i;
  logic signed [33:0] product_q;
  logic signed [33:0] scaled_i;
  logic signed [33:0] scaled_q;

  function automatic logic signed [33:0] round_q17(input logic signed [33:0] value);
    logic [33:0] magnitude;
    logic [33:0] quotient;
    logic [16:0] remainder;
    begin
      magnitude = value < 0 ? $unsigned(-value) : $unsigned(value);
      quotient = magnitude >> 17;
      remainder = magnitude[16:0];
      if (remainder > 17'h10000 || (remainder == 17'h10000 && quotient[0]))
        quotient = quotient + 34'd1;
      round_q17 = value < 0 ? -$signed(quotient) : $signed(quotient);
    end
  endfunction

  always_comb begin
    product_i = $signed(sample_i) * $signed(coefficient);
    product_q = $signed(sample_q) * $signed(coefficient);
    scaled_i = round_q17(product_i);
    scaled_q = round_q17(product_q);
    clipped = 1'b0;
    if (bypass) begin
      windowed_i = sample_i;
      windowed_q = sample_q;
    end else begin
      if (scaled_i > 34'sd32767) begin windowed_i = 16'sh7fff; clipped = 1'b1; end
      else if (scaled_i < -34'sd32768) begin windowed_i = 16'sh8000; clipped = 1'b1; end
      else windowed_i = scaled_i[15:0];
      if (scaled_q > 34'sd32767) begin windowed_q = 16'sh7fff; clipped = 1'b1; end
      else if (scaled_q < -34'sd32768) begin windowed_q = 16'sh8000; clipped = 1'b1; end
      else windowed_q = scaled_q[15:0];
    end
  end
endmodule
