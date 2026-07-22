// SPDX-License-Identifier: MIT
// y = a*x + b*conj(x), coefficients are signed Q1.15.
module neptune_widely_linear_correct (
  input  logic               bypass,
  input  logic signed [15:0] sample_i,
  input  logic signed [15:0] sample_q,
  input  logic signed [15:0] a_i,
  input  logic signed [15:0] a_q,
  input  logic signed [15:0] b_i,
  input  logic signed [15:0] b_q,
  output logic signed [15:0] corrected_i,
  output logic signed [15:0] corrected_q,
  output logic [1:0]         clipping,
  output logic               clipped
);
  logic signed [39:0] acc_i;
  logic signed [39:0] acc_q;
  logic signed [39:0] rounded_i;
  logic signed [39:0] rounded_q;
  logic signed [39:0] scaled_i;
  logic signed [39:0] scaled_q;
  logic signed [31:0] p_ai_i;
  logic signed [31:0] p_aq_q;
  logic signed [31:0] p_bi_i;
  logic signed [31:0] p_bq_q;
  logic signed [31:0] p_ai_q;
  logic signed [31:0] p_aq_i;
  logic signed [31:0] p_bq_i;
  logic signed [31:0] p_bi_q;

  function automatic logic signed [39:0] round_q15(input logic signed [39:0] value);
    logic signed [39:0] magnitude;
    begin
      if (value < 0) begin
        magnitude = -value;
        round_q15 = -((magnitude + 40'sd16384) >>> 15);
      end else begin
        round_q15 = (value + 40'sd16384) >>> 15;
      end
    end
  endfunction

  always_comb begin
    p_ai_i = $signed(a_i) * $signed(sample_i);
    p_aq_q = $signed(a_q) * $signed(sample_q);
    p_bi_i = $signed(b_i) * $signed(sample_i);
    p_bq_q = $signed(b_q) * $signed(sample_q);
    p_ai_q = $signed(a_i) * $signed(sample_q);
    p_aq_i = $signed(a_q) * $signed(sample_i);
    p_bq_i = $signed(b_q) * $signed(sample_i);
    p_bi_q = $signed(b_i) * $signed(sample_q);
    acc_i = {{8{p_ai_i[31]}}, p_ai_i}
          - {{8{p_aq_q[31]}}, p_aq_q}
          + {{8{p_bi_i[31]}}, p_bi_i}
          + {{8{p_bq_q[31]}}, p_bq_q};
    acc_q = {{8{p_ai_q[31]}}, p_ai_q}
          + {{8{p_aq_i[31]}}, p_aq_i}
          + {{8{p_bq_i[31]}}, p_bq_i}
          - {{8{p_bi_q[31]}}, p_bi_q};
    rounded_i = round_q15(acc_i);
    rounded_q = round_q15(acc_q);
    scaled_i = rounded_i;
    scaled_q = rounded_q;
    clipping = 2'b00;
    if (bypass) begin
      corrected_i = sample_i;
      corrected_q = sample_q;
    end else begin
      if (scaled_i > 40'sd32767) begin corrected_i = 16'sh7fff; clipping[0] = 1'b1; end
      else if (scaled_i < -40'sd32768) begin corrected_i = 16'sh8000; clipping[0] = 1'b1; end
      else corrected_i = scaled_i[15:0];
      if (scaled_q > 40'sd32767) begin corrected_q = 16'sh7fff; clipping[1] = 1'b1; end
      else if (scaled_q < -40'sd32768) begin corrected_q = 16'sh8000; clipping[1] = 1'b1; end
      else corrected_q = scaled_q[15:0];
    end
  end
  assign clipped = |clipping;
endmodule
