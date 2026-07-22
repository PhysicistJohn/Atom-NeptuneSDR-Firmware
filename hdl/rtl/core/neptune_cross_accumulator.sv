// SPDX-License-Identifier: MIT
// Block powers and x0*conj(x1), used for cross-spectrum/relative phase products.
module neptune_cross_accumulator (
  input  logic               clk,
  input  logic               rst_n,
  input  logic               sample_valid,
  input  logic               block_end,
  input  logic signed [15:0] ch0_i,
  input  logic signed [15:0] ch0_q,
  input  logic signed [15:0] ch1_i,
  input  logic signed [15:0] ch1_q,
  input  logic [63:0]        sample_timestamp,
  output logic               result_valid,
  output logic [63:0]        first_sample_timestamp,
  output logic [31:0]        sample_count,
  output logic [63:0]        ch0_power,
  output logic [63:0]        ch1_power,
  output logic signed [63:0] correlation_i,
  output logic signed [63:0] correlation_q
);
  logic [31:0] count;
  logic [63:0] first_timestamp;
  logic [63:0] power0_acc;
  logic [63:0] power1_acc;
  logic signed [63:0] corr_i_acc;
  logic signed [63:0] corr_q_acc;
  logic signed [31:0] p_00;
  logic signed [31:0] p_01;
  logic signed [31:0] p_10;
  logic signed [31:0] p_11;
  logic [63:0] power0_next;
  logic [63:0] power1_next;
  logic signed [63:0] corr_i_next;
  logic signed [63:0] corr_q_next;

  always_comb begin
    p_00 = $signed(ch0_i) * $signed(ch1_i);
    p_01 = $signed(ch0_q) * $signed(ch1_q);
    p_10 = $signed(ch0_q) * $signed(ch1_i);
    p_11 = $signed(ch0_i) * $signed(ch1_q);
    power0_next = power0_acc + ($signed(ch0_i) * $signed(ch0_i)) +
                  ($signed(ch0_q) * $signed(ch0_q));
    power1_next = power1_acc + ($signed(ch1_i) * $signed(ch1_i)) +
                  ($signed(ch1_q) * $signed(ch1_q));
    corr_i_next = corr_i_acc + {{32{p_00[31]}}, p_00} + {{32{p_01[31]}}, p_01};
    corr_q_next = corr_q_acc + {{32{p_10[31]}}, p_10} - {{32{p_11[31]}}, p_11};
  end

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      count                  <= 32'd0;
      first_timestamp        <= 64'd0;
      power0_acc             <= 64'd0;
      power1_acc             <= 64'd0;
      corr_i_acc             <= 64'sd0;
      corr_q_acc             <= 64'sd0;
      result_valid           <= 1'b0;
      first_sample_timestamp <= 64'd0;
      sample_count           <= 32'd0;
      ch0_power              <= 64'd0;
      ch1_power              <= 64'd0;
      correlation_i          <= 64'sd0;
      correlation_q          <= 64'sd0;
    end else begin
      result_valid <= 1'b0;
      if (sample_valid) begin
        if (count == 0)
          first_timestamp <= sample_timestamp;
        count      <= count + 32'd1;
        power0_acc <= power0_next;
        power1_acc <= power1_next;
        corr_i_acc <= corr_i_next;
        corr_q_acc <= corr_q_next;
        if (block_end) begin
          first_sample_timestamp <= count == 0 ? sample_timestamp : first_timestamp;
          sample_count           <= count + 32'd1;
          ch0_power              <= power0_next;
          ch1_power              <= power1_next;
          correlation_i          <= corr_i_next;
          correlation_q          <= corr_q_next;
          result_valid           <= 1'b1;
          count                  <= 32'd0;
          power0_acc             <= 64'd0;
          power1_acc             <= 64'd0;
          corr_i_acc             <= 64'sd0;
          corr_q_acc             <= 64'sd0;
        end
      end
    end
  end
endmodule
