// SPDX-License-Identifier: MIT
// Exact filtered conversion: 61.44 * 125/128 = 60; 60 * 11/12 = 55 MSPS.
module neptune_resampler_55m (
  input  logic        clk,
  input  logic        rst_n,
  input  logic        flush,
  input  logic [31:0] s_axis_tdata,
  input  logic [63:0] s_axis_timestamp,
  input  logic [31:0] s_axis_config_revision,
  input  logic [31:0] s_axis_calibration_revision,
  input  logic        s_axis_tvalid,
  output logic        s_axis_tready,
  output logic [31:0] m_axis_tdata,
  output logic [63:0] m_axis_source_timestamp,
  output logic [31:0] m_axis_source_fraction_q32,
  output logic [31:0] m_axis_config_revision,
  output logic [31:0] m_axis_calibration_revision,
  output logic        m_axis_discontinuity,
  output logic        m_axis_tvalid,
  input  logic        m_axis_tready,
  output logic        metadata_overflow
);
  logic [31:0] stage1_data;
  logic [95:0] stage1_source_time;
  logic [31:0] stage1_config_revision;
  logic [31:0] stage1_calibration_revision;
  logic stage1_discontinuity;
  logic stage1_valid;
  logic stage1_ready;
  logic stage1_metadata_overflow;
  logic stage2_metadata_overflow;
  logic [95:0] final_source_time;

  neptune_fractional_fir_stage #(
    .INTERPOLATION(125),
    .DECIMATION(128),
    .INPUT_RATE_HZ(61440000),
    .OUTPUT_RATE_HZ(60000000)
  ) stage_125_128 (
    .aclk(clk),
    .aresetn(rst_n),
    .flush(flush),
    .s_axis_tdata(s_axis_tdata),
    .s_axis_source_time_q32({s_axis_timestamp, 32'd0}),
    .s_axis_config_revision(s_axis_config_revision),
    .s_axis_calibration_revision(s_axis_calibration_revision),
    .s_axis_tvalid(s_axis_tvalid),
    .s_axis_tready(s_axis_tready),
    .m_axis_tdata(stage1_data),
    .m_axis_source_time_q32(stage1_source_time),
    .m_axis_config_revision(stage1_config_revision),
    .m_axis_calibration_revision(stage1_calibration_revision),
    .m_axis_discontinuity(stage1_discontinuity),
    .m_axis_tvalid(stage1_valid),
    .m_axis_tready(stage1_ready),
    .metadata_overflow(stage1_metadata_overflow)
  );

  neptune_fractional_fir_stage #(
    .INTERPOLATION(11),
    .DECIMATION(12),
    .INPUT_RATE_HZ(60000000),
    .OUTPUT_RATE_HZ(55000000)
  ) stage_11_12 (
    .aclk(clk),
    .aresetn(rst_n),
    .flush(flush | stage1_discontinuity),
    .s_axis_tdata(stage1_data),
    .s_axis_source_time_q32(stage1_source_time),
    .s_axis_config_revision(stage1_config_revision),
    .s_axis_calibration_revision(stage1_calibration_revision),
    .s_axis_tvalid(stage1_valid),
    .s_axis_tready(stage1_ready),
    .m_axis_tdata(m_axis_tdata),
    .m_axis_source_time_q32(final_source_time),
    .m_axis_config_revision(m_axis_config_revision),
    .m_axis_calibration_revision(m_axis_calibration_revision),
    .m_axis_discontinuity(m_axis_discontinuity),
    .m_axis_tvalid(m_axis_tvalid),
    .m_axis_tready(m_axis_tready),
    .metadata_overflow(stage2_metadata_overflow)
  );

  assign m_axis_source_timestamp = final_source_time[95:32];
  assign m_axis_source_fraction_q32 = final_source_time[31:0];
  assign metadata_overflow = stage1_metadata_overflow |
                             stage2_metadata_overflow;
endmodule
