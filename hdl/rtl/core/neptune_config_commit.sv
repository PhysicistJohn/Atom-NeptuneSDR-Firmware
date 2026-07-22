// SPDX-License-Identifier: MIT
// Atomic digital configuration activation on a sample boundary.
module neptune_config_commit #(
  parameter int CONFIG_BITS = 256
) (
  input  logic                   clk,
  input  logic                   rst_n,
  input  logic                   sample_tick,
  input  logic [63:0]            sample_timestamp,
  input  logic [CONFIG_BITS-1:0] shadow_config,
  input  logic                   commit_request,
  output logic                   commit_ready,
  output logic [CONFIG_BITS-1:0] active_config,
  output logic [31:0]            active_revision,
  output logic                   commit_event,
  output logic [63:0]            activation_timestamp
);
  logic pending;
  logic [CONFIG_BITS-1:0] pending_config;

  assign commit_ready = !pending;

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      pending              <= 1'b0;
      pending_config       <= '0;
      active_config        <= '0;
      active_revision      <= 32'd0;
      commit_event         <= 1'b0;
      activation_timestamp <= 64'd0;
    end else begin
      commit_event <= 1'b0;
      if (commit_request && commit_ready) begin
        pending        <= 1'b1;
        pending_config <= shadow_config;
      end
      if (sample_tick && pending) begin
        active_config        <= pending_config;
        active_revision      <= active_revision + 32'd1;
        activation_timestamp <= sample_timestamp;
        commit_event         <= 1'b1;
        pending              <= 1'b0;
      end
    end
  end
endmodule
