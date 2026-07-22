// SPDX-License-Identifier: MIT
// Exact peak, sum-of-squares, and clipping statistics for packet metadata.
// RMS conversion is performed by the packet-boundary integer-sqrt stage.
module neptune_block_stats (
  input  logic               clk,
  input  logic               rst_n,
  input  logic               sample_valid,
  input  logic               block_end,
  input  logic signed [15:0] sample_i,
  input  logic signed [15:0] sample_q,
  input  logic [1:0]         sample_clipping,
  output logic               stats_valid,
  output logic [31:0]        block_sample_count,
  output logic [15:0]        block_peak,
  output logic [63:0]        block_sum_squares,
  output logic [31:0]        block_clipping_count
);
  logic [31:0] count;
  logic [15:0] peak;
  logic [63:0] sum_squares;
  logic [31:0] clipping_count;
  logic [15:0] abs_i;
  logic [15:0] abs_q;
  logic [15:0] next_peak;
  logic [31:0] square_i;
  logic [31:0] square_q;
  logic [1:0] clip_increment;

  always_comb begin
    abs_i = sample_i == -16'sd32768 ? 16'h8000 :
            (sample_i < 0 ? $unsigned(-sample_i) : $unsigned(sample_i));
    abs_q = sample_q == -16'sd32768 ? 16'h8000 :
            (sample_q < 0 ? $unsigned(-sample_q) : $unsigned(sample_q));
    next_peak = peak;
    if (abs_i > next_peak) next_peak = abs_i;
    if (abs_q > next_peak) next_peak = abs_q;
    square_i = $signed(sample_i) * $signed(sample_i);
    square_q = $signed(sample_q) * $signed(sample_q);
    clip_increment = {1'b0, sample_clipping[0]} +
                     {1'b0, sample_clipping[1]};
  end

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      count                 <= 32'd0;
      peak                  <= 16'd0;
      sum_squares           <= 64'd0;
      clipping_count        <= 32'd0;
      stats_valid           <= 1'b0;
      block_sample_count    <= 32'd0;
      block_peak            <= 16'd0;
      block_sum_squares     <= 64'd0;
      block_clipping_count  <= 32'd0;
    end else begin
      stats_valid <= 1'b0;
      if (sample_valid) begin
        count          <= count + 32'd1;
        peak           <= next_peak;
        sum_squares    <= sum_squares + square_i + square_q;
        clipping_count <= clipping_count + clip_increment;

        if (block_end) begin
          block_sample_count   <= count + 32'd1;
          block_peak           <= next_peak;
          block_sum_squares    <= sum_squares + square_i + square_q;
          block_clipping_count <= clipping_count + clip_increment;
          stats_valid          <= 1'b1;
          count                <= 32'd0;
          peak                 <= 16'd0;
          sum_squares          <= 64'd0;
          clipping_count       <= 32'd0;
        end
      end
    end
  end
endmodule
