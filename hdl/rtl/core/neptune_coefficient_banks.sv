// SPDX-License-Identifier: MIT
// Dual-bank coefficient store. Uploads can never modify the active bank and a
// requested swap occurs only at a declared DSP block boundary.
module neptune_coefficient_banks #(
  parameter int COEFFICIENT_COUNT = 128,
  parameter int ADDRESS_BITS = $clog2(COEFFICIENT_COUNT),
  parameter int COEFFICIENT_BITS = 18
) (
  input  logic                               clk,
  input  logic                               rst_n,
  input  logic                               write_enable,
  input  logic [ADDRESS_BITS-1:0]            write_address,
  input  logic signed [COEFFICIENT_BITS-1:0] write_i,
  input  logic signed [COEFFICIENT_BITS-1:0] write_q,
  output logic                               write_error,
  input  logic                               commit_request,
  output logic                               commit_ready,
  input  logic                               block_boundary,
  input  logic [63:0]                        sample_timestamp,
  input  logic [ADDRESS_BITS-1:0]            read_address,
  output logic signed [COEFFICIENT_BITS-1:0] read_i,
  output logic signed [COEFFICIENT_BITS-1:0] read_q,
  output logic                               active_bank,
  output logic [31:0]                        active_revision,
  output logic                               activation_event,
  output logic [63:0]                        activation_timestamp
);
  logic signed [COEFFICIENT_BITS-1:0] bank0_i [0:COEFFICIENT_COUNT-1];
  logic signed [COEFFICIENT_BITS-1:0] bank0_q [0:COEFFICIENT_COUNT-1];
  logic signed [COEFFICIENT_BITS-1:0] bank1_i [0:COEFFICIENT_COUNT-1];
  logic signed [COEFFICIENT_BITS-1:0] bank1_q [0:COEFFICIENT_COUNT-1];
  logic commit_pending;

  assign commit_ready = !commit_pending;
  assign read_i = active_bank ? bank1_i[read_address] : bank0_i[read_address];
  assign read_q = active_bank ? bank1_q[read_address] : bank0_q[read_address];

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      active_bank         <= 1'b0;
      active_revision     <= 32'd0;
      activation_event    <= 1'b0;
      activation_timestamp <= 64'd0;
      commit_pending      <= 1'b0;
      write_error         <= 1'b0;
    end else begin
      activation_event <= 1'b0;
      write_error      <= 1'b0;
      if (write_enable) begin
        if (write_address >= COEFFICIENT_COUNT) begin
          write_error <= 1'b1;
        end else if (active_bank) begin
          bank0_i[write_address] <= write_i;
          bank0_q[write_address] <= write_q;
        end else begin
          bank1_i[write_address] <= write_i;
          bank1_q[write_address] <= write_q;
        end
      end
      if (commit_request && commit_ready)
        commit_pending <= 1'b1;
      if (block_boundary && commit_pending) begin
        active_bank          <= !active_bank;
        active_revision      <= active_revision + 32'd1;
        activation_timestamp <= sample_timestamp;
        activation_event     <= 1'b1;
        commit_pending       <= 1'b0;
      end
    end
  end
endmodule
