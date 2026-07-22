`timescale 1ns/1ps
module tb_neptune_register_file;
  import neptune_pl_registers_v1_pkg::*;
  logic clk = 0;
  logic rst_n = 0;
  logic write_valid = 0;
  logic [11:0] write_address = 0;
  logic [31:0] write_data = 0;
  logic [3:0] write_strobe = 4'hf;
  logic write_error;
  logic read_valid = 0;
  logic [11:0] read_address = 0;
  logic [31:0] read_data;
  logic read_error;
  logic [63:0] build_id = 64'h1122334455667788;
  logic [31:0] capabilities = 1;
  logic [31:0] global_status = 0;
  logic [31:0] fault_events = 0;
  logic [63:0] sample_count = 64'haabbccdd12345678;
  logic [31:0] sample_epoch = 0;
  logic [31:0] discontinuity_revision = 0;
  logic [31:0] active_config_revision = 0;
  logic [31:0] active_calibration_revision = 0;
  logic [31:0] config_status = 0;
  logic [31:0] stream_status = 0;
  logic [63:0] stream_sequence = 0;
  logic [63:0] stream_dropped = 0;
  logic [31:0] dma_fifo_high_water = 32'd9;
  logic [31:0] dma_fifo_overflows = 0;
  logic [31:0] dma_descriptor_starvations = 0;
  logic [63:0] dma_completed_blocks = 0;
  logic [31:0] coefficient_status = 0;
  logic [63:0] trigger_timestamp = 0;
  logic [31:0] trigger_status = 0;
  logic [63:0] detector_events = 0;
  logic [31:0] tx_safety_status = 1;
  logic [31:0] tx_arm_challenge = 32'hcafebabe;
  logic [31:0] tx_disarm_revision = 1;
  logic [63:0] sample_set_value;
  logic sample_set_pulse;
  logic [31:0] config_shadow_control;
  logic [31:0] config_expected_revision;
  logic [63:0] config_activation_timestamp;
  logic config_commit_pulse;
  logic [31:0] stream_id;
  logic [31:0] stream_control;
  logic [31:0] stream_format;
  logic [31:0] stream_packet_samples;
  logic dma_counter_reset_pulse;
  logic [31:0] coefficient_address;
  logic [31:0] coefficient_i;
  logic [31:0] coefficient_q;
  logic coefficient_write_pulse;
  logic coefficient_commit_pulse;
  logic [31:0] trigger_control;
  logic [31:0] trigger_ring_samples;
  logic [31:0] trigger_pre_samples;
  logic [31:0] trigger_post_samples;
  logic [31:0] detector_control;
  logic [31:0] detector_threshold;
  logic [31:0] detector_holdoff;
  logic tx_persistent_inhibit;
  logic [31:0] tx_arm_response;
  logic tx_arm_response_pulse;
  logic tx_disarm_pulse;
  logic [31:0] sticky_faults;

  always #5 clk = ~clk;
  neptune_register_file dut (.*);

  task write_reg(input logic [11:0] address, input logic [31:0] value);
    begin
      @(negedge clk); write_address = address; write_data = value; write_valid = 1;
      @(posedge clk); #1;
      @(negedge clk); write_valid = 0;
    end
  endtask

  initial begin
    repeat (2) @(posedge clk);
    if (!tx_persistent_inhibit || stream_control != 0 || stream_id != 0)
      $fatal(1, "unsafe register reset state");
    @(negedge clk); rst_n = 1; read_valid = 1; read_address = REG_SAMPLE_COUNT_LO;
    @(posedge clk); #1;
    @(negedge clk); read_address = REG_SAMPLE_COUNT_HI;
    #1;
    if (read_data !== 32'haabbccdd) $fatal(1, "coherent sample counter latch failed");
    read_valid = 0;
    write_reg(REG_TX_PERSISTENT_INHIBIT, 0);
    if (tx_persistent_inhibit || write_error) $fatal(1, "full-word inhibit write failed");
    write_reg(REG_TX_DISARM, 1);
    if (!tx_disarm_pulse) $fatal(1, "TX disarm pulse failed");
    write_reg(REG_STREAM0_ID, 32'd1);
    if (stream_id != 32'd1) $fatal(1, "stream ID register failed");
    write_reg(REG_COEFFICIENT_ADDRESS, 32'h1234);
    write_reg(REG_COEFFICIENT_I, 32'hfeedbeef);
    write_reg(REG_COEFFICIENT_WRITE, 1);
    if (coefficient_address != 32'h1234 || coefficient_i != 32'hfeedbeef ||
        !coefficient_write_pulse)
      $fatal(1, "coefficient bank register contact failed");
    write_reg(REG_TRIGGER_PRE_SAMPLES, 32'd1000);
    write_reg(REG_DETECTOR_THRESHOLD, 32'd42);
    if (trigger_pre_samples != 32'd1000 || detector_threshold != 32'd42)
      $fatal(1, "trigger/detector register contact failed");
    @(negedge clk); fault_events = FIELD_GLOBAL_FAULTS_FIFO_OVERFLOW_MASK;
    write_reg(REG_GLOBAL_FAULTS, FIELD_GLOBAL_FAULTS_FIFO_OVERFLOW_MASK);
    if (!(sticky_faults & FIELD_GLOBAL_FAULTS_FIFO_OVERFLOW_MASK))
      $fatal(1, "new fault lost during simultaneous W1C write");
    @(negedge clk); fault_events = 0;
    write_reg(REG_GLOBAL_FAULTS, FIELD_GLOBAL_FAULTS_FIFO_OVERFLOW_MASK);
    if (sticky_faults & FIELD_GLOBAL_FAULTS_FIFO_OVERFLOW_MASK)
      $fatal(1, "sticky fault W1C failed");
    @(negedge clk); read_valid = 1; read_address = REG_DMA_FIFO_HIGH_WATER;
    #1;
    if (read_data !== 32'd9 || read_error) $fatal(1, "DMA counter read failed");
    read_valid = 0;
    $display("tb_neptune_register_file PASS");
    $finish;
  end
endmodule
