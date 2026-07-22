// SPDX-License-Identifier: MIT
module neptune_peak_detector (
  input  logic               clk,
  input  logic               rst_n,
  input  logic               sample_valid,
  input  logic signed [15:0] sample_i,
  input  logic signed [15:0] sample_q,
  input  logic [63:0]        sample_timestamp,
  input  logic [31:0]        threshold_magnitude_squared,
  input  logic [31:0]        holdoff_samples,
  output logic               event_valid,
  output logic [63:0]        event_timestamp,
  output logic [32:0]        event_magnitude_squared
);
  logic [31:0] holdoff;
  logic [31:0] square_i;
  logic [31:0] square_q;
  logic [32:0] magnitude_squared;

  always_comb begin
    square_i = $signed(sample_i) * $signed(sample_i);
    square_q = $signed(sample_q) * $signed(sample_q);
    magnitude_squared = {1'b0, square_i} + {1'b0, square_q};
  end

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      holdoff                <= 32'd0;
      event_valid            <= 1'b0;
      event_timestamp        <= 64'd0;
      event_magnitude_squared <= 33'd0;
    end else begin
      event_valid <= 1'b0;
      if (sample_valid) begin
        if (holdoff != 0)
          holdoff <= holdoff - 32'd1;
        else if (magnitude_squared >= {1'b0, threshold_magnitude_squared}) begin
          event_valid             <= 1'b1;
          event_timestamp         <= sample_timestamp;
          event_magnitude_squared <= magnitude_squared;
          holdoff                 <= holdoff_samples;
        end
      end
    end
  end
endmodule
