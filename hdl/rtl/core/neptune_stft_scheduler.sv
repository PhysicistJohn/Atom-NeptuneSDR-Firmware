// SPDX-License-Identifier: MIT
// Announces frames for a dual-port overlap buffer. frame_timestamp is tied to
// the first sample, as required by the v1 product contract.
module neptune_stft_scheduler #(
  parameter int MAX_FFT_LOG2 = 12
) (
  input  logic        clk,
  input  logic        rst_n,
  input  logic        flush,
  input  logic        sample_valid,
  input  logic [63:0] sample_timestamp,
  input  logic [MAX_FFT_LOG2:0] fft_size,
  input  logic [MAX_FFT_LOG2:0] hop_size,
  output logic        frame_valid,
  output logic [63:0] frame_timestamp,
  output logic [63:0] frame_index,
  output logic        configuration_error
);
  logic [MAX_FFT_LOG2:0] fill_count;
  logic [MAX_FFT_LOG2:0] hop_count;

  always_ff @(posedge clk) begin
    if (!rst_n || flush) begin
      fill_count          <= '0;
      hop_count           <= '0;
      frame_valid         <= 1'b0;
      frame_timestamp     <= 64'd0;
      frame_index         <= 64'd0;
      configuration_error <= 1'b0;
    end else begin
      frame_valid <= 1'b0;
      configuration_error <= fft_size < 2 || hop_size == 0 || hop_size > fft_size;
      if (sample_valid && fft_size >= 2 && hop_size != 0 && hop_size <= fft_size) begin
        if (fill_count < fft_size - 1'b1) begin
          fill_count <= fill_count + 1'b1;
        end else if (hop_count == 0) begin
          frame_valid     <= 1'b1;
          frame_timestamp <= sample_timestamp - fft_size + 64'd1;
          frame_index     <= frame_index + 64'd1;
          hop_count       <= hop_size - 1'b1;
        end else begin
          hop_count <= hop_count - 1'b1;
        end
      end
    end
  end
endmodule
