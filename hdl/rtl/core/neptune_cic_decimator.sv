// SPDX-License-Identifier: MIT
module neptune_cic_decimator #(
  parameter int STAGES = 3,
  parameter int ACCUMULATOR_BITS = 48,
  parameter int MAX_RATE = 64
) (
  input  logic               clk,
  input  logic               rst_n,
  input  logic               flush,
  input  logic               sample_valid,
  input  logic signed [15:0] sample_i,
  input  logic signed [15:0] sample_q,
  input  logic [15:0]        rate,
  input  logic [5:0]         output_right_shift,
  output logic               output_valid,
  output logic signed [15:0] output_i,
  output logic signed [15:0] output_q,
  output logic               clipped,
  output logic               configuration_error
);
  logic signed [ACCUMULATOR_BITS-1:0] integrator_i [0:STAGES-1];
  logic signed [ACCUMULATOR_BITS-1:0] integrator_q [0:STAGES-1];
  logic signed [ACCUMULATOR_BITS-1:0] integrator_next_i [0:STAGES-1];
  logic signed [ACCUMULATOR_BITS-1:0] integrator_next_q [0:STAGES-1];
  logic signed [ACCUMULATOR_BITS-1:0] comb_delay_i [0:STAGES-1];
  logic signed [ACCUMULATOR_BITS-1:0] comb_delay_q [0:STAGES-1];
  logic signed [ACCUMULATOR_BITS-1:0] comb_value_i [0:STAGES];
  logic signed [ACCUMULATOR_BITS-1:0] comb_value_q [0:STAGES];
  logic [15:0] decimation_count;
  logic signed [ACCUMULATOR_BITS-1:0] scaled_i;
  logic signed [ACCUMULATOR_BITS-1:0] scaled_q;
  integer index;

  function automatic logic signed [ACCUMULATOR_BITS-1:0] round_shift(
    input logic signed [ACCUMULATOR_BITS-1:0] value,
    input logic [5:0] shift
  );
    logic [ACCUMULATOR_BITS-1:0] magnitude;
    logic [ACCUMULATOR_BITS-1:0] quotient;
    logic [ACCUMULATOR_BITS-1:0] remainder;
    logic [ACCUMULATOR_BITS-1:0] mask;
    logic [ACCUMULATOR_BITS-1:0] half;
    begin
      if (shift == 0) begin
        round_shift = value;
      end else if (shift >= ACCUMULATOR_BITS) begin
        round_shift = '0;
      end else begin
        magnitude = value < 0 ? $unsigned(-value) : $unsigned(value);
        quotient = magnitude >> shift;
        mask = ({ACCUMULATOR_BITS{1'b1}} >> (ACCUMULATOR_BITS - shift));
        remainder = magnitude & mask;
        half = {{(ACCUMULATOR_BITS-1){1'b0}}, 1'b1} << (shift - 1'b1);
        if (remainder > half || (remainder == half && quotient[0]))
          quotient = quotient + 1'b1;
        round_shift = value < 0 ? -$signed(quotient) : $signed(quotient);
      end
    end
  endfunction

  always_comb begin
    integrator_next_i[0] = integrator_i[0] +
      {{(ACCUMULATOR_BITS-16){sample_i[15]}}, sample_i};
    integrator_next_q[0] = integrator_q[0] +
      {{(ACCUMULATOR_BITS-16){sample_q[15]}}, sample_q};
    for (index = 1; index < STAGES; index = index + 1) begin
      integrator_next_i[index] = integrator_i[index] + integrator_next_i[index-1];
      integrator_next_q[index] = integrator_q[index] + integrator_next_q[index-1];
    end
    comb_value_i[0] = integrator_next_i[STAGES-1];
    comb_value_q[0] = integrator_next_q[STAGES-1];
    for (index = 0; index < STAGES; index = index + 1) begin
      comb_value_i[index+1] = comb_value_i[index] - comb_delay_i[index];
      comb_value_q[index+1] = comb_value_q[index] - comb_delay_q[index];
    end
    scaled_i = round_shift(comb_value_i[STAGES], output_right_shift);
    scaled_q = round_shift(comb_value_q[STAGES], output_right_shift);
  end

  always_ff @(posedge clk) begin
    if (!rst_n || flush) begin
      output_valid        <= 1'b0;
      output_i            <= 16'sd0;
      output_q            <= 16'sd0;
      clipped             <= 1'b0;
      configuration_error <= 1'b0;
      decimation_count    <= 16'd0;
      for (index = 0; index < STAGES; index = index + 1) begin
        integrator_i[index] <= '0;
        integrator_q[index] <= '0;
        comb_delay_i[index] <= '0;
        comb_delay_q[index] <= '0;
      end
    end else begin
      output_valid        <= 1'b0;
      clipped             <= 1'b0;
      configuration_error <= rate == 0 || rate > MAX_RATE;
      if (sample_valid && rate != 0 && rate <= MAX_RATE) begin
        for (index = 0; index < STAGES; index = index + 1) begin
          integrator_i[index] <= integrator_next_i[index];
          integrator_q[index] <= integrator_next_q[index];
        end
        if (decimation_count + 16'd1 >= rate) begin
          decimation_count <= 16'd0;
          for (index = 0; index < STAGES; index = index + 1) begin
            comb_delay_i[index] <= comb_value_i[index];
            comb_delay_q[index] <= comb_value_q[index];
          end
          output_valid <= 1'b1;
          if (scaled_i > 32767) begin output_i <= 16'sh7fff; clipped <= 1'b1; end
          else if (scaled_i < -32768) begin output_i <= 16'sh8000; clipped <= 1'b1; end
          else output_i <= scaled_i[15:0];
          if (scaled_q > 32767) begin output_q <= 16'sh7fff; clipped <= 1'b1; end
          else if (scaled_q < -32768) begin output_q <= 16'sh8000; clipped <= 1'b1; end
          else output_q <= scaled_q[15:0];
        end else begin
          decimation_count <= decimation_count + 16'd1;
        end
      end
    end
  end
endmodule
