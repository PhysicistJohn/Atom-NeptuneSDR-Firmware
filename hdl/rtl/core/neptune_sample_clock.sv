// SPDX-License-Identifier: MIT
// Free-running sample-time authority. sample_tick is never backpressured.
module neptune_sample_clock (
  input  logic        clk,
  input  logic        rst_n,
  input  logic        sample_tick,
  input  logic        counter_set_valid,
  input  logic [63:0] counter_set_value,
  input  logic        discontinuity_event,
  output logic [63:0] current_timestamp,
  output logic [63:0] sample_timestamp,
  output logic        sample_discontinuity,
  output logic [31:0] epoch
);
  logic discontinuity_pending;

  assign current_timestamp = sample_timestamp;
  assign sample_discontinuity = discontinuity_pending |
                                discontinuity_event |
                                counter_set_valid;

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      sample_timestamp      <= 64'd0;
      discontinuity_pending <= 1'b1;
      epoch                 <= 32'd0;
    end else begin
      if (counter_set_valid) begin
        sample_timestamp      <= counter_set_value;
        discontinuity_pending <= 1'b1;
        epoch                 <= epoch + 32'd1;
      end else if (sample_tick) begin
        discontinuity_pending <= discontinuity_event || (&sample_timestamp);
        sample_timestamp      <= sample_timestamp + 64'd1;
      end else if (discontinuity_event) begin
        discontinuity_pending <= 1'b1;
      end
    end
  end
endmodule
