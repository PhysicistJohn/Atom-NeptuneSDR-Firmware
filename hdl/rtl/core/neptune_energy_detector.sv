// SPDX-License-Identifier: MIT
module neptune_energy_detector #(
  parameter int INTEGRATION_LOG2 = 8
) (
  input  logic               clk,
  input  logic               rst_n,
  input  logic               sample_valid,
  input  logic signed [15:0] sample_i,
  input  logic signed [15:0] sample_q,
  input  logic [63:0]        sample_timestamp,
  input  logic [63:0]        threshold,
  output logic               event_valid,
  output logic [63:0]        event_timestamp,
  output logic [63:0]        integrated_energy
);
  logic [INTEGRATION_LOG2-1:0] count;
  logic [63:0] accumulator;
  logic [31:0] energy_i;
  logic [31:0] energy_q;
  logic [63:0] next_energy;

  always_comb begin
    energy_i = $signed(sample_i) * $signed(sample_i);
    energy_q = $signed(sample_q) * $signed(sample_q);
    next_energy = accumulator + energy_i + energy_q;
  end

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      count              <= '0;
      accumulator        <= 64'd0;
      event_valid        <= 1'b0;
      event_timestamp    <= 64'd0;
      integrated_energy  <= 64'd0;
    end else begin
      event_valid <= 1'b0;
      if (sample_valid) begin
        if (&count) begin
          integrated_energy <= next_energy;
          accumulator       <= 64'd0;
          count             <= '0;
          if (next_energy >= threshold) begin
            event_valid     <= 1'b1;
            event_timestamp <= sample_timestamp;
          end
        end else begin
          accumulator <= next_energy;
          count       <= count + 1'b1;
        end
      end
    end
  end
endmodule
