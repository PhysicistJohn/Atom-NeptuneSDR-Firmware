`timescale 1ns/1ps
module tb_neptune_s8bf_revision_boundary;
  logic clk = 0;
  logic rst_n = 0;
  logic s_valid = 0;
  logic s_ready;
  logic signed [15:0] s_i = 0;
  logic signed [15:0] s_q = 0;
  logic s_block_end = 0;
  logic s_flush_partial = 0;
  logic [63:0] s_source_timestamp = 0;
  logic [31:0] s_source_fraction_q32 = 0;
  logic s_discontinuity = 0;
  logic [31:0] s_config_revision = 7;
  logic [31:0] s_calibration_revision = 9;
  logic m_valid;
  logic m_ready = 1;
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

  always #5 clk = ~clk;
  neptune_s8bf_buffer #(.BLOCK_SAMPLES(4), .HEADROOM_BITS(1)) dut (.*);

  initial begin
    #100000;
    $fatal(1, "tb_neptune_s8bf_revision_boundary timeout");
  end

  task push(input integer value, input integer timestamp,
            input integer revision, input logic final_sample);
    integer accepted;
    begin
      @(negedge clk);
      s_i = value;
      s_q = 0;
      s_source_timestamp = timestamp;
      s_source_fraction_q32 = 32'h10000000 + timestamp;
      s_discontinuity = timestamp == 101;
      s_config_revision = revision;
      s_block_end = final_sample;
      s_valid = 1;
      accepted = 0;
      while (!accepted) begin
        @(posedge clk);
        if (s_ready) accepted = 1;
        #1;
      end
      @(negedge clk);
      s_valid = 0;
      s_block_end = 0;
      s_discontinuity = 0;
    end
  endtask

  always @(posedge clk) begin
    #1;
    if (m_valid && m_ready) begin
      case (observed)
        0: if ($signed(m_i) !== 20 || !m_block_start || m_block_end ||
               m_block_sample_count !== 2 || m_source_timestamp !== 100 ||
               m_source_fraction_q32 !== 32'h10000064 || !m_discontinuity ||
               m_config_revision !== 7) $fatal(1, "old revision sample zero");
        1: if ($signed(m_i) !== 40 || m_block_start || !m_block_end ||
               m_block_sample_count !== 2 || m_source_timestamp !== 100 ||
               m_source_fraction_q32 !== 32'h10000064 || !m_discontinuity ||
               m_config_revision !== 7) $fatal(1, "old revision sample one");
        2: if ($signed(m_i) !== 60 || !m_block_start || !m_block_end ||
               m_block_sample_count !== 1 || m_source_timestamp !== 102 ||
               m_source_fraction_q32 !== 32'h10000066 || m_discontinuity ||
               m_config_revision !== 8) $fatal(1, "new revision sample");
        default: $fatal(1, "unexpected output");
      endcase
      observed = observed + 1;
    end
  end

  initial begin
    repeat (2) @(posedge clk);
    @(negedge clk); rst_n = 1;
    push(10, 100, 7, 0);
    push(20, 101, 7, 0);
    push(30, 102, 8, 1);
    wait (observed == 3);
    @(posedge clk); #1;
    $display("tb_neptune_s8bf_revision_boundary PASS");
    $finish;
  end
endmodule
