`timescale 1ns/1ps
module tb_neptune_s8bf_buffer;
  logic clk = 0;
  logic rst_n = 0;
  logic s_valid = 0;
  logic s_ready;
  logic signed [15:0] s_i = 0;
  logic signed [15:0] s_q = 0;
  logic s_block_end = 0;
  logic s_flush_partial = 0;
  logic [63:0] s_source_timestamp = 100;
  logic [31:0] s_source_fraction_q32 = 0;
  logic s_discontinuity = 0;
  logic [31:0] s_config_revision = 7;
  logic [31:0] s_calibration_revision = 9;
  logic m_valid;
  logic m_ready = 0;
  logic signed [7:0] m_i;
  logic signed [7:0] m_q;
  logic m_block_start;
  logic m_block_end;
  logic signed [5:0] m_applied_power;
  logic signed [5:0] m_reconstruction_exponent;
  logic [31:0] m_block_sample_count;
  logic [15:0] m_block_peak;
  logic [63:0] m_block_sum_squares;
  logic [31:0] m_block_clipping_count;
  logic [63:0] m_source_timestamp;
  logic [31:0] m_source_fraction_q32;
  logic m_discontinuity;
  logic [31:0] m_config_revision;
  logic [31:0] m_calibration_revision;
  integer observed = 0;
  integer values [0:3];

  always #5 clk = ~clk;
  neptune_s8bf_buffer #(.BLOCK_SAMPLES(4), .HEADROOM_BITS(1)) dut (.*);

  initial begin
    #100000;
    $fatal(1, "tb_neptune_s8bf_buffer timeout");
  end

  task push(input integer value, input logic final_sample,
            input logic [31:0] source_fraction, input logic discontinuity);
    begin
      @(negedge clk);
      s_i = value;
      s_q = 0;
      s_block_end = final_sample;
      s_source_fraction_q32 = source_fraction;
      s_discontinuity = discontinuity;
      s_valid = 1;
      @(posedge clk); #1;
      @(negedge clk);
      s_valid = 0;
      s_block_end = 0;
      s_discontinuity = 0;
    end
  endtask

  always @(posedge clk) begin
    #1;
    if (m_valid && m_ready) begin
      if ($signed(m_i) !== values[observed] * 4 || $signed(m_q) !== 0)
        $fatal(1, "S8BF payload mismatch at %0d", observed);
      if (m_applied_power !== 2 || m_reconstruction_exponent !== -2 ||
          m_block_sample_count !== 4 || m_block_peak !== 10 ||
          m_block_sum_squares !== 250 || m_block_clipping_count !== 0 ||
          m_source_timestamp !== 100 ||
          m_source_fraction_q32 !== 32'h12345678 || !m_discontinuity ||
          m_config_revision !== 7 ||
          m_calibration_revision !== 9)
        $fatal(1, "S8BF metadata mismatch");
      if ((observed == 0) !== m_block_start || (observed == 3) !== m_block_end)
        $fatal(1, "S8BF boundary mismatch");
      observed = observed + 1;
    end
  end

  initial begin
    values[0] = 10; values[1] = -10; values[2] = 5; values[3] = -5;
    repeat (2) @(posedge clk);
    @(negedge clk); rst_n = 1;
    push(values[0], 0, 32'h12345678, 0);
    push(values[1], 0, 32'h23456789, 0);
    push(values[2], 0, 32'h3456789a, 1);
    push(values[3], 1, 32'h456789ab, 0);
    wait (m_valid);
    repeat (3) begin
      @(negedge clk);
      if (!m_valid || !m_block_start ||
          m_source_fraction_q32 !== 32'h12345678 || !m_discontinuity)
        $fatal(1, "S8BF metadata changed under backpressure");
    end
    @(posedge clk); #1; m_ready = 1;
    wait (observed == 4);
    @(posedge clk); #1;
    $display("tb_neptune_s8bf_buffer PASS");
    $finish;
  end
endmodule
