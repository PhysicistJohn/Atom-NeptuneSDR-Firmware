// SPDX-License-Identifier: MIT
module neptune_pack_samples (
  input  logic signed [15:0] sample_i,
  input  logic signed [15:0] sample_q,
  input  logic signed [7:0]  sample_i_s8,
  input  logic signed [7:0]  sample_q_s8,
  output logic [31:0]        s16,
  output logic [23:0]        s12p,
  output logic [15:0]        s8,
  output logic [1:0]         s12p_clipping
);
  function automatic logic signed [11:0] clamp_s12(
    input logic signed [15:0] value
  );
    begin
      if (value > 16'sd2047) clamp_s12 = 12'sh7ff;
      else if (value < -16'sd2048) clamp_s12 = 12'sh800;
      else clamp_s12 = value[11:0];
    end
  endfunction

  // Little-endian serializers send the low-order I container first.
  assign s16  = {sample_q, sample_i};
  // Native 12-bit ADC input is bit-exact. Corrected 16-bit products saturate
  // explicitly instead of wrapping across the signed 12-bit boundary.
  assign s12p = {clamp_s12(sample_q), clamp_s12(sample_i)};
  assign s8   = {sample_q_s8, sample_i_s8};
  assign s12p_clipping = {
    sample_q > 16'sd2047 || sample_q < -16'sd2048,
    sample_i > 16'sd2047 || sample_i < -16'sd2048
  };
endmodule
