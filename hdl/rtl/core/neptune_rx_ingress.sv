// SPDX-License-Identifier: MIT
// One-entry elastic boundary from the non-backpressurable AD9361 sample clock.
module neptune_rx_ingress (
  input  logic         clk,
  input  logic         rst_n,
  input  logic         sample_tick,
  input  logic [63:0]  timestamp,
  input  logic         timestamp_discontinuity,
  input  logic         interface_error,
  input  logic [3:0]   channel_mask,
  input  logic [31:0]  config_revision,
  input  logic [31:0]  calibration_revision,
  input  logic [63:0]  sample_data,
  output logic         stream_valid,
  input  logic         stream_ready,
  output logic [63:0]  stream_data,
  output logic [63:0]  stream_timestamp,
  output logic [31:0]  stream_config_revision,
  output logic [31:0]  stream_calibration_revision,
  output logic [3:0]   stream_channel_mask,
  output logic [3:0]   stream_flags,
  output logic         overflow_sticky,
  output logic         overflow_event,
  output logic [63:0]  dropped_samples
);
  localparam int FLAG_DISCONTINUITY = 0;
  localparam int FLAG_VALID          = 1;
  localparam int FLAG_INTERFACE      = 2;
  localparam int FLAG_CLIPPED        = 3;

  logic discontinuity_pending;
  logic occupied;
  wire pop = occupied && stream_ready;
  wire can_store = !occupied || pop;

  assign stream_valid = occupied;

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      occupied                       <= 1'b0;
      stream_data                    <= '0;
      stream_timestamp               <= '0;
      stream_config_revision         <= '0;
      stream_calibration_revision    <= '0;
      stream_channel_mask            <= '0;
      stream_flags                   <= '0;
      overflow_sticky                <= 1'b0;
      overflow_event                 <= 1'b0;
      dropped_samples                <= 64'd0;
      discontinuity_pending          <= 1'b1;
    end else begin
      overflow_event <= 1'b0;
      if (pop)
        occupied <= 1'b0;

      if (sample_tick) begin
        if (can_store) begin
          occupied                    <= 1'b1;
          stream_data                 <= sample_data;
          stream_timestamp            <= timestamp;
          stream_config_revision      <= config_revision;
          stream_calibration_revision <= calibration_revision;
          stream_channel_mask         <= channel_mask;
          stream_flags[FLAG_DISCONTINUITY] <= timestamp_discontinuity |
                                               discontinuity_pending;
          stream_flags[FLAG_VALID]     <= !(interface_error |
                                              discontinuity_pending);
          stream_flags[FLAG_INTERFACE] <= interface_error;
          stream_flags[FLAG_CLIPPED]   <= 1'b0;
          discontinuity_pending       <= interface_error;
        end else begin
          overflow_sticky       <= 1'b1;
          overflow_event        <= 1'b1;
          dropped_samples       <= dropped_samples + 64'd1;
          discontinuity_pending <= 1'b1;
        end
      end
    end
  end
endmodule
