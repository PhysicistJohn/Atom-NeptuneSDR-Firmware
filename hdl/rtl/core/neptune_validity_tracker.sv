// SPDX-License-Identifier: MIT
module neptune_validity_tracker (
  input  logic        clk,
  input  logic        rst_n,
  input  logic        sample_tick,
  input  logic        invalidate,
  input  logic [31:0] invalid_sample_count,
  input  logic [7:0]  invalid_reason,
  output logic        sample_valid,
  output logic [7:0]  active_reason,
  output logic        transition_event
);
  logic [31:0] remaining;

  assign sample_valid = remaining == 0;

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      remaining        <= 32'hffff_ffff;
      active_reason    <= 8'h01;
      transition_event <= 1'b0;
    end else begin
      transition_event <= 1'b0;
      if (invalidate) begin
        remaining        <= invalid_sample_count;
        active_reason    <= invalid_reason;
        transition_event <= 1'b1;
      end else if (sample_tick && remaining != 0) begin
        remaining <= remaining - 32'd1;
        if (remaining == 32'd1) begin
          active_reason    <= 8'd0;
          transition_event <= 1'b1;
        end
      end
    end
  end
endmodule
