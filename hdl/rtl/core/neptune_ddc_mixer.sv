// SPDX-License-Identifier: MIT
// Complex downconverter using signed Q1.15 sine/cosine from a DDS/CORDIC.
module neptune_ddc_mixer (
  input  logic               bypass,
  input  logic signed [15:0] sample_i,
  input  logic signed [15:0] sample_q,
  input  logic signed [15:0] cosine,
  input  logic signed [15:0] sine,
  output logic signed [15:0] mixed_i,
  output logic signed [15:0] mixed_q,
  output logic               clipped
);
  logic signed [31:0] p_i_cos;
  logic signed [31:0] p_q_sin;
  logic signed [31:0] p_q_cos;
  logic signed [31:0] p_i_sin;
  logic signed [33:0] acc_i;
  logic signed [33:0] acc_q;
  logic signed [33:0] scaled_i;
  logic signed [33:0] scaled_q;

  function automatic logic signed [33:0] q15(input logic signed [33:0] value);
    logic signed [33:0] magnitude;
    begin
      if (value < 0) begin
        magnitude = -value;
        q15 = -((magnitude + 34'sd16384) >>> 15);
      end else begin
        q15 = (value + 34'sd16384) >>> 15;
      end
    end
  endfunction

  always_comb begin
    p_i_cos = $signed(sample_i) * $signed(cosine);
    p_q_sin = $signed(sample_q) * $signed(sine);
    p_q_cos = $signed(sample_q) * $signed(cosine);
    p_i_sin = $signed(sample_i) * $signed(sine);
    acc_i = {{2{p_i_cos[31]}}, p_i_cos} + {{2{p_q_sin[31]}}, p_q_sin};
    acc_q = {{2{p_q_cos[31]}}, p_q_cos} - {{2{p_i_sin[31]}}, p_i_sin};
    scaled_i = q15(acc_i);
    scaled_q = q15(acc_q);
    clipped = 1'b0;
    if (bypass) begin
      mixed_i = sample_i;
      mixed_q = sample_q;
    end else begin
      if (scaled_i > 34'sd32767) begin mixed_i = 16'sh7fff; clipped = 1'b1; end
      else if (scaled_i < -34'sd32768) begin mixed_i = 16'sh8000; clipped = 1'b1; end
      else mixed_i = scaled_i[15:0];
      if (scaled_q > 34'sd32767) begin mixed_q = 16'sh7fff; clipped = 1'b1; end
      else if (scaled_q < -34'sd32768) begin mixed_q = 16'sh8000; clipped = 1'b1; end
      else mixed_q = scaled_q[15:0];
    end
  end
endmodule
