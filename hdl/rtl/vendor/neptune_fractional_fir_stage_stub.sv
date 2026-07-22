// SPDX-License-Identifier: MIT
// Synthesis contact for the generated fractional FIR wrapper. The generated
// implementation replaces this declaration in the board Vivado project.
//
// NEPTUNE_SIM_FIR_MODEL enables a transaction-level model used only by the
// portable integration bench. It proves the exact rate and AXI backpressure
// contract, but deliberately does not claim bit-accurate FIR response.
`ifndef NEPTUNE_SIM_FIR_MODEL
(* black_box *)
`endif
module neptune_fractional_fir_stage #(
  parameter int INTERPOLATION = 1,
  parameter int DECIMATION = 1,
  parameter int INPUT_RATE_HZ = 1,
  parameter int OUTPUT_RATE_HZ = 1
) (
  input  logic        aclk,
  input  logic        aresetn,
  input  logic        flush,
  input  logic [31:0] s_axis_tdata,
  input  logic [95:0] s_axis_source_time_q32,
  input  logic [31:0] s_axis_config_revision,
  input  logic [31:0] s_axis_calibration_revision,
  input  logic        s_axis_tvalid,
  output logic        s_axis_tready,
  output logic [31:0] m_axis_tdata,
  output logic [95:0] m_axis_source_time_q32,
  output logic [31:0] m_axis_config_revision,
  output logic [31:0] m_axis_calibration_revision,
  output logic        m_axis_discontinuity,
  output logic        m_axis_tvalid,
  input  logic        m_axis_tready,
  output logic        metadata_overflow
);
`ifdef NEPTUNE_SIM_FIR_MODEL
  logic [31:0] phase_accumulator;
  logic        discontinuity_pending;
  wire [32:0] phase_sum = {1'b0, phase_accumulator} + INTERPOLATION;
  wire emit_for_input = phase_sum >= DECIMATION;

  // flush invalidates any held output and consumes the boundary input. The
  // first later output carries the discontinuity marker.
  assign s_axis_tready = flush || !m_axis_tvalid || m_axis_tready;

  initial begin
    if (INTERPOLATION <= 0 || DECIMATION <= 0 ||
        INTERPOLATION > DECIMATION)
      $fatal(1, "simulation model supports positive decimating ratios only");
  end

  always_ff @(posedge aclk) begin
    if (!aresetn) begin
      phase_accumulator              <= 32'd0;
      discontinuity_pending          <= 1'b1;
      m_axis_tdata                   <= 32'd0;
      m_axis_source_time_q32         <= 96'd0;
      m_axis_config_revision         <= 32'd0;
      m_axis_calibration_revision    <= 32'd0;
      m_axis_discontinuity           <= 1'b0;
      m_axis_tvalid                  <= 1'b0;
      metadata_overflow              <= 1'b0;
    end else if (flush) begin
      phase_accumulator              <= 32'd0;
      discontinuity_pending          <= 1'b1;
      m_axis_discontinuity           <= 1'b0;
      m_axis_tvalid                  <= 1'b0;
      metadata_overflow              <= 1'b0;
    end else begin
      metadata_overflow <= 1'b0;
      if (m_axis_tvalid && m_axis_tready)
        m_axis_tvalid <= 1'b0;

      if (s_axis_tvalid && s_axis_tready) begin
        if (emit_for_input) begin
          phase_accumulator           <= phase_sum - DECIMATION;
          m_axis_tdata                <= s_axis_tdata;
          m_axis_source_time_q32      <= s_axis_source_time_q32;
          m_axis_config_revision      <= s_axis_config_revision;
          m_axis_calibration_revision <= s_axis_calibration_revision;
          m_axis_discontinuity        <= discontinuity_pending;
          m_axis_tvalid               <= 1'b1;
          discontinuity_pending       <= 1'b0;
        end else begin
          phase_accumulator <= phase_sum[31:0];
        end
      end
    end
  end
`endif
endmodule
