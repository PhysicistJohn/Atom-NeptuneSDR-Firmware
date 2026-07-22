// SPDX-License-Identifier: MIT
// Rate/phase scheduler for a FILTERED 1375/1536 resampler.
// This block never selects unfiltered ADC samples for transport. emit_output
// is consumed by the FIR/polyphase bank in neptune_resampler_1375_1536.
module neptune_rate_scheduler #(
  parameter int INPUT_UNITS  = 1536,
  parameter int OUTPUT_UNITS = 1375
) (
  input  logic        clk,
  input  logic        rst_n,
  input  logic        input_tick,
  output logic        emit_output,
  output logic [10:0] phase,
  output logic [63:0] input_count,
  output logic [63:0] output_count
);
  logic [11:0] accumulator;
  logic [12:0] sum;

  always_comb sum = {1'b0, accumulator} + OUTPUT_UNITS;

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      accumulator <= '0;
      emit_output <= 1'b0;
      phase        <= '0;
      input_count  <= 64'd0;
      output_count <= 64'd0;
    end else begin
      emit_output <= 1'b0;
      if (input_tick) begin
        input_count <= input_count + 64'd1;
        if (sum >= INPUT_UNITS) begin
          accumulator <= sum - INPUT_UNITS;
          phase        <= sum - INPUT_UNITS;
          emit_output  <= 1'b1;
          output_count <= output_count + 64'd1;
        end else begin
          accumulator <= sum[11:0];
        end
      end
    end
  end
endmodule
