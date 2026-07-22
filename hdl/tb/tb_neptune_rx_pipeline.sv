`timescale 1ns/1ps
module tb_neptune_rx_pipeline;
  logic sample_clk = 0;
  logic rst_n = 0;
  logic sample_tick = 0;
  logic signed [15:0] rx0_i = 16'sd10;
  logic signed [15:0] rx0_q = -16'sd20;
  logic signed [15:0] rx1_i = 16'sd30;
  logic signed [15:0] rx1_q = -16'sd40;
  logic [3:0] channel_mask = 4'b1111;
  logic interface_error = 0;
  logic external_discontinuity = 0;
  logic counter_set_valid = 0;
  logic [63:0] counter_set_value = 0;
  logic [31:0] config_revision = 7;
  logic [31:0] calibration_revision = 9;

  logic [1:0] dc_bypass = 2'b11;
  logic signed [15:0] rx0_dc_i = 0;
  logic signed [15:0] rx0_dc_q = 0;
  logic signed [15:0] rx1_dc_i = 0;
  logic signed [15:0] rx1_dc_q = 0;
  logic [1:0] iq_bypass = 2'b11;
  logic signed [15:0] rx0_a_i = 16'sh7fff;
  logic signed [15:0] rx0_a_q = 0;
  logic signed [15:0] rx0_b_i = 0;
  logic signed [15:0] rx0_b_q = 0;
  logic signed [15:0] rx1_a_i = 16'sh7fff;
  logic signed [15:0] rx1_a_q = 0;
  logic signed [15:0] rx1_b_i = 0;
  logic signed [15:0] rx1_b_q = 0;
  logic [1:0] channel_cal_bypass = 2'b11;
  logic signed [15:0] rx0_gain_i = 16'sh7fff;
  logic signed [15:0] rx0_gain_q = 0;
  logic signed [15:0] rx1_gain_i = 16'sh7fff;
  logic signed [15:0] rx1_gain_q = 0;

  logic egress_channel = 0;
  logic [3:0] s8_right_shift = 0;
  logic resampler_flush = 0;
  logic s8bf_enable = 0;
  logic s8bf_force_block_end = 0;

  logic raw_valid;
  logic raw_ready = 1;
  logic [63:0] raw_s16;
  logic [63:0] raw_timestamp;
  logic [3:0] raw_flags;
  logic [63:0] raw_dropped_samples;

  logic egress_valid;
  logic egress_ready = 1;
  logic [31:0] egress_s16;
  logic [23:0] egress_s12p;
  logic [15:0] egress_s8;
  logic [1:0] egress_clipping;
  logic [1:0] egress_s12p_clipping;
  logic correction_clipping_event;
  logic [63:0] correction_clipping_components;
  logic [63:0] egress_source_timestamp;
  logic [31:0] egress_source_fraction_q32;
  logic [31:0] egress_config_revision;
  logic [31:0] egress_calibration_revision;
  logic egress_discontinuity;
  logic [63:0] egress_dropped_samples;
  logic overflow_sticky;

  logic s8bf_valid;
  logic s8bf_ready = 1;
  logic signed [7:0] s8bf_i;
  logic signed [7:0] s8bf_q;
  logic s8bf_block_start;
  logic s8bf_block_end;
  logic signed [5:0] s8bf_reconstruction_exponent;
  logic [31:0] s8bf_block_sample_count;
  logic [15:0] s8bf_block_peak;
  logic [63:0] s8bf_block_sum_squares;
  logic [31:0] s8bf_block_clipping_count;
  logic [63:0] s8bf_source_timestamp;
  logic [31:0] s8bf_source_fraction_q32;
  logic s8bf_discontinuity;
  logic [31:0] s8bf_config_revision;
  logic [31:0] s8bf_calibration_revision;

  integer fixed_count = 0;
  integer accepted_count = 0;
  integer s8bf_output_count = 0;
  integer block_count = 0;
  integer block_remaining = 0;
  integer index;
  integer expected_peak;
  integer expected_sum;
  integer expected_power;
  integer last_fixed_timestamp;
  logic track_fixed = 0;
  logic track_s8bf = 0;
  logic signed [15:0] accepted_i [0:255];
  logic signed [15:0] accepted_q [0:255];
  logic [63:0] accepted_timestamp [0:255];
  logic [31:0] accepted_fraction_q32 [0:255];
  logic accepted_discontinuity [0:255];
  logic [31:0] accepted_config_revision [0:255];
  logic [31:0] accepted_calibration_revision [0:255];
  integer observed_block_count [0:15];
  integer observed_block_revision [0:15];
  integer observed_block_discontinuity [0:15];

  always #5 sample_clk = ~sample_clk;

  neptune_rx_pipeline #(.S8BF_BLOCK_SAMPLES(4)) dut (.*);

  initial begin
    #1000000;
    $fatal(1, "tb_neptune_rx_pipeline timeout");
  end

  function automatic integer abs16(input logic signed [15:0] value);
    begin
      abs16 = value < 0 ? -value : value;
    end
  endfunction

  function automatic integer apply_power(
    input logic signed [15:0] value,
    input integer power
  );
    integer magnitude;
    integer quotient;
    integer remainder;
    integer half;
    integer rounded;
    begin
      if (power >= 0)
        rounded = value * (1 << power);
      else begin
        magnitude = value < 0 ? -value : value;
        quotient = magnitude >> -power;
        remainder = magnitude & ((1 << -power) - 1);
        half = 1 << (-power - 1);
        rounded = quotient + (remainder > half ||
                               (remainder == half && (quotient & 1)));
        if (value < 0) rounded = -rounded;
      end
      if (rounded > 127) apply_power = 127;
      else if (rounded < -128) apply_power = -128;
      else apply_power = rounded;
    end
  endfunction

  // Sample transfer intent at the falling edge, after all DUT state from the
  // preceding rising edge has settled. This avoids counting mere presentation
  // of a beat as a completed ready/valid transfer.
  always @(negedge sample_clk) begin
    if (track_fixed && egress_valid && egress_ready) begin
      if (s8bf_enable)
        $fatal(1, "fixed output asserted in S8BF mode");
      if (egress_s16 !== {-16'sd20, 16'sd10})
        $fatal(1, "fixed bypass changed the selected sample");
      if (egress_config_revision !== 7 ||
          egress_calibration_revision !== 9)
        $fatal(1, "fixed metadata revision mismatch");
      if (fixed_count != 0 && egress_source_timestamp <= last_fixed_timestamp)
        $fatal(1, "fixed output timestamps are not strictly increasing");
      last_fixed_timestamp = egress_source_timestamp;
      fixed_count = fixed_count + 1;
    end

    if (track_s8bf && dut.resampled_valid && dut.s8bf_input_ready &&
        dut.s8bf_mode_active) begin
      if (accepted_count >= 256)
        $fatal(1, "S8BF scoreboard overflow");
      accepted_i[accepted_count] = $signed(dut.resampled_data[15:0]);
      accepted_q[accepted_count] = $signed(dut.resampled_data[31:16]);
      accepted_timestamp[accepted_count] = dut.resampled_timestamp;
      accepted_fraction_q32[accepted_count] = dut.resampled_fraction;
      accepted_discontinuity[accepted_count] =
        dut.resampled_discontinuity | dut.metadata_overflow;
      accepted_config_revision[accepted_count] =
        dut.resampled_config_revision;
      accepted_calibration_revision[accepted_count] =
        dut.resampled_calibration_revision;
      accepted_count = accepted_count + 1;
    end

    if (track_s8bf && s8bf_valid && s8bf_ready) begin
      if (egress_valid)
        $fatal(1, "fixed and S8BF outputs asserted together");
      if (s8bf_output_count >= accepted_count)
        $fatal(1, "S8BF output was not accepted at its input");

      expected_power = -$signed(s8bf_reconstruction_exponent);
      if ($signed(s8bf_i) !== apply_power(
            accepted_i[s8bf_output_count], expected_power) ||
          $signed(s8bf_q) !== apply_power(
            accepted_q[s8bf_output_count], expected_power))
        $fatal(1, "S8BF payload/order mismatch at %0d", s8bf_output_count);

      if (s8bf_block_start) begin
        if (block_remaining != 0 || s8bf_block_sample_count == 0)
          $fatal(1, "invalid S8BF block start");
        block_remaining = s8bf_block_sample_count;
        observed_block_count[block_count] = s8bf_block_sample_count;
        observed_block_revision[block_count] = s8bf_config_revision;
        observed_block_discontinuity[block_count] = s8bf_discontinuity;
        expected_peak = 0;
        expected_sum = 0;
        for (index = 0; index < s8bf_block_sample_count; index = index + 1) begin
          if (accepted_config_revision[s8bf_output_count + index] !==
                s8bf_config_revision ||
              accepted_calibration_revision[s8bf_output_count + index] !==
                s8bf_calibration_revision)
            $fatal(1, "mixed revision in S8BF block");
          if (abs16(accepted_i[s8bf_output_count + index]) > expected_peak)
            expected_peak = abs16(accepted_i[s8bf_output_count + index]);
          if (abs16(accepted_q[s8bf_output_count + index]) > expected_peak)
            expected_peak = abs16(accepted_q[s8bf_output_count + index]);
          expected_sum = expected_sum +
            $signed(accepted_i[s8bf_output_count + index]) *
            $signed(accepted_i[s8bf_output_count + index]) +
            $signed(accepted_q[s8bf_output_count + index]) *
            $signed(accepted_q[s8bf_output_count + index]);
        end
        if (s8bf_source_timestamp !== accepted_timestamp[s8bf_output_count] ||
            s8bf_source_fraction_q32 !==
              accepted_fraction_q32[s8bf_output_count] ||
            s8bf_block_peak !== expected_peak ||
            s8bf_block_sum_squares !== expected_sum ||
            s8bf_block_clipping_count !== 0)
          $fatal(1, "S8BF block metadata mismatch");
        expected_sum = 0;
        for (index = 0; index < s8bf_block_sample_count; index = index + 1)
          expected_sum = expected_sum |
            accepted_discontinuity[s8bf_output_count + index];
        if (s8bf_discontinuity !== (expected_sum != 0))
          $fatal(1, "S8BF discontinuity aggregation mismatch");
        block_count = block_count + 1;
      end

      if (block_remaining <= 0 ||
          s8bf_block_end !== (block_remaining == 1))
        $fatal(1, "S8BF block boundary mismatch: remaining=%0d start=%0b end=%0b out=%0d count=%0d",
               block_remaining, s8bf_block_start, s8bf_block_end,
               s8bf_output_count, s8bf_block_sample_count);
      block_remaining = block_remaining - 1;
      s8bf_output_count = s8bf_output_count + 1;
    end
  end

  task automatic reset_pipeline;
    begin
      @(negedge sample_clk);
      rst_n = 0;
      sample_tick = 0;
      resampler_flush = 0;
      s8bf_force_block_end = 0;
      repeat (3) @(posedge sample_clk);
      @(negedge sample_clk);
      rst_n = 1;
    end
  endtask

  task automatic drive_samples(input integer count);
    begin
      @(negedge sample_clk);
      sample_tick = 1;
      repeat (count) @(negedge sample_clk);
      sample_tick = 0;
    end
  endtask

  task automatic drive_one_and_drain;
    begin
      drive_samples(1);
      repeat (5) @(posedge sample_clk);
    end
  endtask

  task automatic wait_pipeline_empty;
    integer timeout;
    begin
      timeout = 0;
      while ((dut.proc_valid ||
              dut.egress_resampler.stage1_valid ||
              dut.resampled_valid) && timeout < 100) begin
        @(posedge sample_clk);
        timeout = timeout + 1;
      end
      if (timeout == 100)
        $fatal(1, "pipeline did not drain");
      repeat (2) @(posedge sample_clk);
    end
  endtask

  task automatic feed_until_accepted(input integer target);
    integer timeout;
    begin
      timeout = 0;
      while (accepted_count < target && timeout < 100) begin
        drive_one_and_drain();
        timeout = timeout + 1;
      end
      if (accepted_count != target)
        $fatal(1, "could not reach exact accepted count %0d (got %0d)",
               target, accepted_count);
    end
  endtask

  initial begin
    // Fixed-format path: warm through reset discontinuity, then prove that a
    // complete 1536-input phase produces exactly 1375 continuous outputs.
    s8bf_enable = 0;
    egress_ready = 1;
    reset_pipeline();
    drive_samples(24);
    wait_pipeline_empty();
    fixed_count = 0;
    last_fixed_timestamp = -1;
    track_fixed = 1;
    drive_samples(1536);
    wait_pipeline_empty();
    track_fixed = 0;
    if (fixed_count != 1375)
      $fatal(1, "pipeline 55 MSPS ratio mismatch: %0d/1536", fixed_count);
    if (egress_dropped_samples !== 0 || raw_dropped_samples !== 0 ||
        overflow_sticky)
      $fatal(1, "unstalled fixed path reported loss");

    // Fixed output must hold every payload and metadata field while stalled,
    // and the S8BF output must remain inactive.
    egress_ready = 0;
    drive_samples(4);
    wait (egress_valid);
    sample_tick = 0;
    repeat (4) begin
      @(negedge sample_clk);
      if (!egress_valid || s8bf_valid ||
          egress_s16 !== {-16'sd20, 16'sd10} ||
          egress_config_revision !== 7 ||
          egress_calibration_revision !== 9)
        $fatal(1, "fixed output was not stable under backpressure");
    end
    s8bf_enable = 1;
    repeat (3) begin
      @(negedge sample_clk);
      if (!egress_valid || dut.s8bf_mode_active || s8bf_valid)
        $fatal(1, "stalled fixed beat was rerouted by a mode request");
    end
    @(posedge sample_clk); #1; egress_ready = 1;
    wait (dut.s8bf_mode_active);
    wait_pipeline_empty();

    // S8BF path: accept exactly two old-revision outputs, then change both
    // revisions. The held new-revision beat closes (but does not enter) the
    // old block, after which a four-sample new block is accepted in order.
    track_fixed = 0;
    s8bf_enable = 1;
    s8bf_ready = 1;
    config_revision = 7;
    calibration_revision = 9;
    reset_pipeline();
    accepted_count = 0;
    s8bf_output_count = 0;
    block_count = 0;
    block_remaining = 0;
    force dut.resampled_fraction = 32'h89abcdef;
    track_s8bf = 1;
    feed_until_accepted(2);
    config_revision = 8;
    calibration_revision = 10;
    force dut.resampled_fraction = 32'h10203040;
    s8bf_ready = 0;
    feed_until_accepted(3);
    wait (s8bf_valid);
    repeat (4) begin
      @(negedge sample_clk);
      if (!s8bf_valid || !s8bf_block_start ||
          s8bf_block_sample_count !== 2 || s8bf_config_revision !== 7 ||
          s8bf_calibration_revision !== 9 ||
          s8bf_source_fraction_q32 !== 32'h89abcdef ||
          !s8bf_discontinuity)
        $fatal(1, "S8BF output was not stable under backpressure");
    end
    // Change ready just after an active edge so the monitor observes the same
    // first transfer as the DUT; avoid a testbench/DUT scheduling race.
    @(posedge sample_clk); #1; s8bf_ready = 1;
    feed_until_accepted(6);
    wait (s8bf_output_count == 6);
    if (block_count != 2 || observed_block_count[0] != 2 ||
        observed_block_revision[0] != 7 || observed_block_count[1] != 4 ||
        observed_block_revision[1] != 8 ||
        observed_block_discontinuity[0] != 1 ||
        observed_block_discontinuity[1] != 0 || block_remaining != 0)
      $fatal(1, "pipeline revision split did not produce 2+4 blocks");

    // Disabling S8BF with one partial sample must publish that sample as a
    // final block; it must not be retained and merged after a later enable.
    feed_until_accepted(7);
    s8bf_ready = 0;
    s8bf_enable = 0;
    wait (!dut.s8bf_mode_active);
    wait (s8bf_valid);
    repeat (3) begin
      @(negedge sample_clk);
      if (!s8bf_valid || !s8bf_block_start || !s8bf_block_end ||
          s8bf_block_sample_count !== 1 || s8bf_config_revision !== 8 ||
          s8bf_source_fraction_q32 !== 32'h10203040 ||
          s8bf_discontinuity)
        $fatal(1, "partial S8BF block did not flush on mode disable");
    end
    @(posedge sample_clk); #1; s8bf_ready = 1;
    wait (s8bf_output_count == 7);
    if (block_count != 3 || observed_block_count[2] != 1 ||
        observed_block_revision[2] != 8 ||
        observed_block_discontinuity[2] != 0)
      $fatal(1, "flushed S8BF block metadata mismatch");

    release dut.resampled_fraction;

    // The next resampled beat uses the requested fixed path and still carries
    // the unmodified bypass sample.
    egress_ready = 0;
    drive_samples(4);
    wait (egress_valid);
    @(negedge sample_clk);
    if (dut.s8bf_mode_active || egress_s16 !== {-16'sd20, 16'sd10})
      $fatal(1, "fixed path did not activate after S8BF drain");
    @(posedge sample_clk); #1; egress_ready = 1;
    wait_pipeline_empty();
    @(negedge sample_clk);
    track_s8bf = 0;

    $display("tb_neptune_rx_pipeline PASS");
    $finish;
  end
endmodule
