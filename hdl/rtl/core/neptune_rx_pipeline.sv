// SPDX-License-Identifier: MIT
// Board-independent dual-RX proof pipeline. The raw output remains full-rate;
// the selected continuous channel is corrected at the input rate and then
// filtered to 55 MSPS for Ethernet transport.
module neptune_rx_pipeline #(
  parameter int S8BF_BLOCK_SAMPLES = 4096
) (
  input  logic               sample_clk,
  input  logic               rst_n,
  input  logic               sample_tick,
  input  logic signed [15:0] rx0_i,
  input  logic signed [15:0] rx0_q,
  input  logic signed [15:0] rx1_i,
  input  logic signed [15:0] rx1_q,
  input  logic [3:0]         channel_mask,
  input  logic               interface_error,
  input  logic               external_discontinuity,
  input  logic               counter_set_valid,
  input  logic [63:0]        counter_set_value,
  input  logic [31:0]        config_revision,
  input  logic [31:0]        calibration_revision,

  input  logic [1:0]         dc_bypass,
  input  logic signed [15:0] rx0_dc_i,
  input  logic signed [15:0] rx0_dc_q,
  input  logic signed [15:0] rx1_dc_i,
  input  logic signed [15:0] rx1_dc_q,
  input  logic [1:0]         iq_bypass,
  input  logic signed [15:0] rx0_a_i,
  input  logic signed [15:0] rx0_a_q,
  input  logic signed [15:0] rx0_b_i,
  input  logic signed [15:0] rx0_b_q,
  input  logic signed [15:0] rx1_a_i,
  input  logic signed [15:0] rx1_a_q,
  input  logic signed [15:0] rx1_b_i,
  input  logic signed [15:0] rx1_b_q,
  input  logic [1:0]         channel_cal_bypass,
  input  logic signed [15:0] rx0_gain_i,
  input  logic signed [15:0] rx0_gain_q,
  input  logic signed [15:0] rx1_gain_i,
  input  logic signed [15:0] rx1_gain_q,

  input  logic               egress_channel,
  input  logic [3:0]         s8_right_shift,
  input  logic               resampler_flush,
  input  logic               s8bf_enable,
  input  logic               s8bf_force_block_end,

  output logic               raw_valid,
  input  logic               raw_ready,
  output logic [63:0]        raw_s16,
  output logic [63:0]        raw_timestamp,
  output logic [3:0]         raw_flags,
  output logic [63:0]        raw_dropped_samples,

  output logic               egress_valid,
  input  logic               egress_ready,
  output logic [31:0]        egress_s16,
  output logic [23:0]        egress_s12p,
  output logic [15:0]        egress_s8,
  output logic [1:0]         egress_clipping,
  output logic [1:0]         egress_s12p_clipping,
  output logic               correction_clipping_event,
  output logic [63:0]        correction_clipping_components,
  output logic [63:0]        egress_source_timestamp,
  output logic [31:0]        egress_source_fraction_q32,
  output logic [31:0]        egress_config_revision,
  output logic [31:0]        egress_calibration_revision,
  output logic               egress_discontinuity,
  output logic [63:0]        egress_dropped_samples,
  output logic               overflow_sticky,
  output logic               s8bf_valid,
  input  logic               s8bf_ready,
  output logic signed [7:0]  s8bf_i,
  output logic signed [7:0]  s8bf_q,
  output logic               s8bf_block_start,
  output logic               s8bf_block_end,
  output logic signed [5:0]  s8bf_reconstruction_exponent,
  output logic [31:0]        s8bf_block_sample_count,
  output logic [15:0]        s8bf_block_peak,
  output logic [63:0]        s8bf_block_sum_squares,
  output logic [31:0]        s8bf_block_clipping_count,
  output logic [63:0]        s8bf_source_timestamp,
  output logic [31:0]        s8bf_source_fraction_q32,
  output logic               s8bf_discontinuity,
  output logic [31:0]        s8bf_config_revision,
  output logic [31:0]        s8bf_calibration_revision
);
  logic [63:0] timestamp;
  logic timestamp_discontinuity;
  logic [31:0] timestamp_epoch;
  logic raw_overflow;
  logic raw_overflow_event;
  logic proc_overflow;
  logic proc_overflow_event;
  logic [63:0] proc_data;
  logic [63:0] proc_timestamp;
  logic [31:0] proc_config_revision;
  logic [31:0] proc_calibration_revision;
  logic [3:0] proc_channel_mask;
  logic [3:0] proc_flags;
  logic proc_valid;
  logic proc_ready;
  logic [31:0] selected_corrected;
  logic [31:0] resampled_data;
  logic [63:0] resampled_timestamp;
  logic [31:0] resampled_fraction;
  logic [31:0] resampled_config_revision;
  logic [31:0] resampled_calibration_revision;
  logic resampled_discontinuity;
  logic resampled_valid;
  logic resampler_output_ready;
  logic s8bf_input_ready;
  logic s8bf_mode_active;
  logic s8bf_flush_partial;
  logic s8bf_end_current;
  logic signed [5:0] s8bf_applied_power;
  logic metadata_overflow;

  logic signed [15:0] dc0_i;
  logic signed [15:0] dc0_q;
  logic signed [15:0] dc1_i;
  logic signed [15:0] dc1_q;
  logic dc0_clipped;
  logic dc1_clipped;
  logic [1:0] dc0_clipping;
  logic [1:0] dc1_clipping;
  logic signed [15:0] iq0_i;
  logic signed [15:0] iq0_q;
  logic signed [15:0] iq1_i;
  logic signed [15:0] iq1_q;
  logic iq0_clipped;
  logic iq1_clipped;
  logic [1:0] iq0_clipping;
  logic [1:0] iq1_clipping;
  logic signed [15:0] cal0_i;
  logic signed [15:0] cal0_q;
  logic signed [15:0] cal1_i;
  logic signed [15:0] cal1_q;
  logic cal0_clipped;
  logic cal1_clipped;
  logic [1:0] cal0_clipping;
  logic [1:0] cal1_clipping;
  logic [1:0] selected_correction_clipping;
  logic signed [7:0] quantized_i;
  logic signed [7:0] quantized_q;

  wire [63:0] native_sample = {rx1_q, rx1_i, rx0_q, rx0_i};
  wire global_discontinuity = external_discontinuity | interface_error |
                              raw_overflow_event | proc_overflow_event |
                              metadata_overflow;

  neptune_sample_clock sample_time (
    .clk(sample_clk), .rst_n(rst_n), .sample_tick(sample_tick),
    .counter_set_valid(counter_set_valid), .counter_set_value(counter_set_value),
    .discontinuity_event(global_discontinuity), .current_timestamp(),
    .sample_timestamp(timestamp), .sample_discontinuity(timestamp_discontinuity),
    .epoch(timestamp_epoch)
  );

  neptune_rx_ingress raw_tap (
    .clk(sample_clk), .rst_n(rst_n), .sample_tick(sample_tick),
    .timestamp(timestamp), .timestamp_discontinuity(timestamp_discontinuity),
    .interface_error(interface_error), .channel_mask(channel_mask),
    .config_revision(config_revision), .calibration_revision(calibration_revision),
    .sample_data(native_sample), .stream_valid(raw_valid), .stream_ready(raw_ready),
    .stream_data(raw_s16), .stream_timestamp(raw_timestamp),
    .stream_config_revision(), .stream_calibration_revision(),
    .stream_channel_mask(), .stream_flags(raw_flags),
    .overflow_sticky(raw_overflow), .overflow_event(raw_overflow_event),
    .dropped_samples(raw_dropped_samples)
  );

  neptune_rx_ingress processing_tap (
    .clk(sample_clk), .rst_n(rst_n), .sample_tick(sample_tick),
    .timestamp(timestamp), .timestamp_discontinuity(timestamp_discontinuity),
    .interface_error(interface_error), .channel_mask(channel_mask),
    .config_revision(config_revision), .calibration_revision(calibration_revision),
    .sample_data(native_sample), .stream_valid(proc_valid), .stream_ready(proc_ready),
    .stream_data(proc_data), .stream_timestamp(proc_timestamp),
    .stream_config_revision(proc_config_revision),
    .stream_calibration_revision(proc_calibration_revision),
    .stream_channel_mask(proc_channel_mask), .stream_flags(proc_flags),
    .overflow_sticky(proc_overflow), .overflow_event(proc_overflow_event),
    .dropped_samples(egress_dropped_samples)
  );

  neptune_dc_correct dc0 (
    .bypass(dc_bypass[0]), .sample_i(proc_data[15:0]), .sample_q(proc_data[31:16]),
    .dc_i(rx0_dc_i), .dc_q(rx0_dc_q), .corrected_i(dc0_i), .corrected_q(dc0_q),
    .clipping(dc0_clipping), .clipped(dc0_clipped)
  );
  neptune_dc_correct dc1 (
    .bypass(dc_bypass[1]), .sample_i(proc_data[47:32]), .sample_q(proc_data[63:48]),
    .dc_i(rx1_dc_i), .dc_q(rx1_dc_q), .corrected_i(dc1_i), .corrected_q(dc1_q),
    .clipping(dc1_clipping), .clipped(dc1_clipped)
  );
  neptune_widely_linear_correct iq0 (
    .bypass(iq_bypass[0]), .sample_i(dc0_i), .sample_q(dc0_q),
    .a_i(rx0_a_i), .a_q(rx0_a_q), .b_i(rx0_b_i), .b_q(rx0_b_q),
    .corrected_i(iq0_i), .corrected_q(iq0_q),
    .clipping(iq0_clipping), .clipped(iq0_clipped)
  );
  neptune_widely_linear_correct iq1 (
    .bypass(iq_bypass[1]), .sample_i(dc1_i), .sample_q(dc1_q),
    .a_i(rx1_a_i), .a_q(rx1_a_q), .b_i(rx1_b_i), .b_q(rx1_b_q),
    .corrected_i(iq1_i), .corrected_q(iq1_q),
    .clipping(iq1_clipping), .clipped(iq1_clipped)
  );
  neptune_widely_linear_correct channel_cal0 (
    .bypass(channel_cal_bypass[0]), .sample_i(iq0_i), .sample_q(iq0_q),
    .a_i(rx0_gain_i), .a_q(rx0_gain_q), .b_i(16'sd0), .b_q(16'sd0),
    .corrected_i(cal0_i), .corrected_q(cal0_q),
    .clipping(cal0_clipping), .clipped(cal0_clipped)
  );
  neptune_widely_linear_correct channel_cal1 (
    .bypass(channel_cal_bypass[1]), .sample_i(iq1_i), .sample_q(iq1_q),
    .a_i(rx1_gain_i), .a_q(rx1_gain_q), .b_i(16'sd0), .b_q(16'sd0),
    .corrected_i(cal1_i), .corrected_q(cal1_q),
    .clipping(cal1_clipping), .clipped(cal1_clipped)
  );

  always_comb begin
    if (egress_channel) begin
      selected_corrected = {cal1_q, cal1_i};
      selected_correction_clipping = dc1_clipping | iq1_clipping | cal1_clipping;
    end else begin
      selected_corrected = {cal0_q, cal0_i};
      selected_correction_clipping = dc0_clipping | iq0_clipping | cal0_clipping;
    end
  end

  assign correction_clipping_event = proc_valid && proc_ready &&
                                     (|selected_correction_clipping);

  always_ff @(posedge sample_clk) begin
    if (!rst_n)
      correction_clipping_components <= 64'd0;
    else if (correction_clipping_event)
      correction_clipping_components <= correction_clipping_components +
        selected_correction_clipping[0] + selected_correction_clipping[1];
  end

  neptune_resampler_55m egress_resampler (
    .clk(sample_clk), .rst_n(rst_n), .flush(resampler_flush | proc_flags[0]),
    .s_axis_tdata(selected_corrected), .s_axis_timestamp(proc_timestamp),
    .s_axis_config_revision(proc_config_revision),
    .s_axis_calibration_revision(proc_calibration_revision),
    .s_axis_tvalid(proc_valid), .s_axis_tready(proc_ready),
    .m_axis_tdata(resampled_data),
    .m_axis_source_timestamp(resampled_timestamp),
    .m_axis_source_fraction_q32(resampled_fraction),
    .m_axis_config_revision(resampled_config_revision),
    .m_axis_calibration_revision(resampled_calibration_revision),
    .m_axis_discontinuity(resampled_discontinuity),
    .m_axis_tvalid(resampled_valid), .m_axis_tready(resampler_output_ready),
    .metadata_overflow(metadata_overflow)
  );

  // Hold the selected destination while a resampler beat is stalled. A mode
  // request becomes active only after that beat transfers (or while no beat is
  // present), so an already-produced sample can never be silently rerouted.
  always_ff @(posedge sample_clk) begin
    if (!rst_n)
      s8bf_mode_active <= s8bf_enable;
    else if (!resampled_valid || resampler_output_ready)
      s8bf_mode_active <= s8bf_enable;
  end

  assign resampler_output_ready = s8bf_mode_active ? s8bf_input_ready :
                                                       egress_ready;
  assign s8bf_flush_partial = s8bf_mode_active && !s8bf_enable &&
                              !resampled_valid;
  assign s8bf_end_current = s8bf_force_block_end ||
                            (s8bf_mode_active && !s8bf_enable);

  neptune_s8bf_buffer #(
    .BLOCK_SAMPLES(S8BF_BLOCK_SAMPLES),
    .HEADROOM_BITS(1)
  ) s8bf_quantizer (
    .clk(sample_clk), .rst_n(rst_n),
    .s_valid(resampled_valid && s8bf_mode_active), .s_ready(s8bf_input_ready),
    .s_i(resampled_data[15:0]), .s_q(resampled_data[31:16]),
    .s_block_end(s8bf_end_current), .s_flush_partial(s8bf_flush_partial),
    .s_source_timestamp(resampled_timestamp),
    .s_source_fraction_q32(resampled_fraction),
    .s_discontinuity(resampled_discontinuity | metadata_overflow),
    .s_config_revision(resampled_config_revision),
    .s_calibration_revision(resampled_calibration_revision),
    .m_valid(s8bf_valid), .m_ready(s8bf_ready), .m_i(s8bf_i), .m_q(s8bf_q),
    .m_block_start(s8bf_block_start), .m_block_end(s8bf_block_end),
    .m_applied_power(s8bf_applied_power),
    .m_reconstruction_exponent(s8bf_reconstruction_exponent),
    .m_block_sample_count(s8bf_block_sample_count),
    .m_block_peak(s8bf_block_peak),
    .m_block_sum_squares(s8bf_block_sum_squares),
    .m_block_clipping_count(s8bf_block_clipping_count),
    .m_source_timestamp(s8bf_source_timestamp),
    .m_source_fraction_q32(s8bf_source_fraction_q32),
    .m_discontinuity(s8bf_discontinuity),
    .m_config_revision(s8bf_config_revision),
    .m_calibration_revision(s8bf_calibration_revision)
  );

  neptune_quantize_s8 quantizer (
    .sample_i(resampled_data[15:0]), .sample_q(resampled_data[31:16]),
    .right_shift(s8_right_shift), .quantized_i(quantized_i),
    .quantized_q(quantized_q), .clipping(egress_clipping)
  );
  neptune_pack_samples packer (
    .sample_i(resampled_data[15:0]), .sample_q(resampled_data[31:16]),
    .sample_i_s8(quantized_i), .sample_q_s8(quantized_q),
    .s16(egress_s16), .s12p(egress_s12p), .s8(egress_s8),
    .s12p_clipping(egress_s12p_clipping)
  );

  assign egress_valid                = resampled_valid && !s8bf_mode_active;
  assign egress_source_timestamp     = resampled_timestamp;
  assign egress_source_fraction_q32  = resampled_fraction;
  assign egress_config_revision      = resampled_config_revision;
  assign egress_calibration_revision = resampled_calibration_revision;
  assign egress_discontinuity        = resampled_discontinuity |
                                        metadata_overflow;
  assign overflow_sticky             = raw_overflow | proc_overflow |
                                        metadata_overflow;
endmodule
