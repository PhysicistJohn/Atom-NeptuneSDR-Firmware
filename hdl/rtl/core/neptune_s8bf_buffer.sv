// SPDX-License-Identifier: MIT
// Peak-normalized ping-pong block buffer. The complete input block is measured
// before any S8 samples are emitted, so exponent and statistics describe the
// same samples as the payload.
module neptune_s8bf_buffer #(
  parameter int BLOCK_SAMPLES = 256,
  parameter int HEADROOM_BITS = 1,
  parameter int ADDRESS_BITS = $clog2(BLOCK_SAMPLES)
) (
  input  logic               clk,
  input  logic               rst_n,
  input  logic               s_valid,
  output logic               s_ready,
  input  logic signed [15:0] s_i,
  input  logic signed [15:0] s_q,
  input  logic               s_block_end,
  input  logic               s_flush_partial,
  input  logic [63:0]        s_source_timestamp,
  input  logic [31:0]        s_source_fraction_q32,
  input  logic               s_discontinuity,
  input  logic [31:0]        s_config_revision,
  input  logic [31:0]        s_calibration_revision,
  output logic               m_valid,
  input  logic               m_ready,
  output logic signed [7:0]  m_i,
  output logic signed [7:0]  m_q,
  output logic               m_block_start,
  output logic               m_block_end,
  output logic signed [5:0]  m_applied_power,
  output logic signed [5:0]  m_reconstruction_exponent,
  output logic [31:0]        m_block_sample_count,
  output logic [15:0]        m_block_peak,
  output logic [63:0]        m_block_sum_squares,
  output logic [31:0]        m_block_clipping_count,
  output logic [63:0]        m_source_timestamp,
  output logic [31:0]        m_source_fraction_q32,
  output logic               m_discontinuity,
  output logic [31:0]        m_config_revision,
  output logic [31:0]        m_calibration_revision
);
  localparam int TARGET_PEAK = 127 >> HEADROOM_BITS;

  logic [31:0] memory0 [0:BLOCK_SAMPLES-1];
  logic [31:0] memory1 [0:BLOCK_SAMPLES-1];
  logic [1:0] buffer_full;
  logic write_bank;
  logic [ADDRESS_BITS-1:0] write_index;
  logic [31:0] write_count;
  logic [15:0] write_peak;
  logic [63:0] write_sum_squares;
  logic [63:0] write_first_timestamp;
  logic [31:0] write_first_fraction_q32;
  logic write_discontinuity;
  logic [31:0] write_config_revision;
  logic [31:0] write_calibration_revision;

  logic [31:0] desc_count [0:1];
  logic [15:0] desc_peak [0:1];
  logic [63:0] desc_sum_squares [0:1];
  logic signed [5:0] desc_power [0:1];
  logic [63:0] desc_timestamp [0:1];
  logic [31:0] desc_fraction_q32 [0:1];
  logic desc_discontinuity [0:1];
  logic [31:0] desc_config_revision [0:1];
  logic [31:0] desc_calibration_revision [0:1];

  logic read_active;
  logic read_bank;
  logic [ADDRESS_BITS-1:0] read_index;
  logic [31:0] read_word;
  logic [15:0] abs_i;
  logic [15:0] abs_q;
  logic [15:0] next_peak;
  logic [31:0] square_i;
  logic [31:0] square_q;
  logic [63:0] next_sum_squares;
  logic close_block;
  logic revision_change;
  logic flush_partial;

  function automatic logic signed [5:0] choose_power(input logic [15:0] peak);
    integer power;
    integer shifted;
    integer selected;
    begin
      selected = 0;
      if (peak != 0) begin
        if (peak <= TARGET_PEAK) begin
          for (power = 1; power <= 15; power = power + 1)
            if ((peak << power) <= TARGET_PEAK)
              selected = power;
        end else begin
          selected = -15;
          for (power = 1; power <= 15; power = power + 1) begin
            shifted = (peak + (1 << (power - 1))) >> power;
            if (shifted <= TARGET_PEAK && selected == -15)
              selected = -power;
          end
        end
      end
      choose_power = selected;
    end
  endfunction

  function automatic logic signed [7:0] apply_power(
    input logic signed [15:0] value,
    input logic signed [5:0] power
  );
    integer wide;
    integer magnitude;
    integer shift;
    integer quotient;
    integer remainder;
    integer half;
    begin
      if (power >= 0)
        wide = value * (1 << power);
      else begin
        shift = -power;
        magnitude = value < 0 ? -value : value;
        quotient = magnitude >> shift;
        remainder = magnitude & ((1 << shift) - 1);
        half = 1 << (shift - 1);
        wide = quotient + (remainder > half ||
                           (remainder == half && (quotient & 1)));
        if (value < 0) wide = -wide;
      end
      if (wide > 127) apply_power = 8'sh7f;
      else if (wide < -128) apply_power = 8'sh80;
      else apply_power = wide[7:0];
    end
  endfunction

  always_comb begin
    abs_i = s_i == -16'sd32768 ? 16'h8000 :
            (s_i < 0 ? $unsigned(-s_i) : $unsigned(s_i));
    abs_q = s_q == -16'sd32768 ? 16'h8000 :
            (s_q < 0 ? $unsigned(-s_q) : $unsigned(s_q));
    next_peak = write_peak;
    if (abs_i > next_peak) next_peak = abs_i;
    if (abs_q > next_peak) next_peak = abs_q;
    square_i = $signed(s_i) * $signed(s_i);
    square_q = $signed(s_q) * $signed(s_q);
    next_sum_squares = write_sum_squares + square_i + square_q;
    close_block = s_block_end || write_count == BLOCK_SAMPLES - 1;
  end

  // A revision is part of the signal. Split before (and do not consume) the
  // first sample carrying a new revision so no block can be mislabeled.
  assign revision_change = s_valid && write_count != 0 &&
                           (s_config_revision != write_config_revision ||
                            s_calibration_revision != write_calibration_revision);
  assign flush_partial = s_flush_partial && write_count != 0 &&
                         !buffer_full[write_bank];
  assign s_ready = !buffer_full[write_bank] && !revision_change &&
                   !flush_partial;
  assign m_valid = read_active;
  assign m_block_start = read_active && read_index == 0;
  assign m_block_end = read_active && read_index + 1 == desc_count[read_bank];
  assign read_word = read_bank ? memory1[read_index] : memory0[read_index];
  assign m_i = apply_power(read_word[15:0], desc_power[read_bank]);
  assign m_q = apply_power(read_word[31:16], desc_power[read_bank]);
  assign m_applied_power = desc_power[read_bank];
  assign m_reconstruction_exponent = -desc_power[read_bank];
  assign m_block_sample_count = desc_count[read_bank];
  assign m_block_peak = desc_peak[read_bank];
  assign m_block_sum_squares = desc_sum_squares[read_bank];
  // Peak strategy and TARGET_PEAK <= 127 make clipping impossible by construction.
  assign m_block_clipping_count = 32'd0;
  assign m_source_timestamp = desc_timestamp[read_bank];
  assign m_source_fraction_q32 = desc_fraction_q32[read_bank];
  assign m_discontinuity = desc_discontinuity[read_bank];
  assign m_config_revision = desc_config_revision[read_bank];
  assign m_calibration_revision = desc_calibration_revision[read_bank];

  initial begin
    if (HEADROOM_BITS < 0 || HEADROOM_BITS > 6)
      $fatal(1, "HEADROOM_BITS must be in [0, 6]");
  end

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      buffer_full                 <= 2'b00;
      write_bank                  <= 1'b0;
      write_index                 <= '0;
      write_count                 <= 32'd0;
      write_peak                  <= 16'd0;
      write_sum_squares           <= 64'd0;
      write_first_timestamp       <= 64'd0;
      write_first_fraction_q32    <= 32'd0;
      write_discontinuity         <= 1'b0;
      write_config_revision       <= 32'd0;
      write_calibration_revision  <= 32'd0;
      read_active                 <= 1'b0;
      read_bank                   <= 1'b0;
      read_index                  <= '0;
      desc_count[0]               <= 32'd0;
      desc_count[1]               <= 32'd0;
      desc_peak[0]                <= 16'd0;
      desc_peak[1]                <= 16'd0;
      desc_sum_squares[0]         <= 64'd0;
      desc_sum_squares[1]         <= 64'd0;
      desc_power[0]               <= 6'sd0;
      desc_power[1]               <= 6'sd0;
      desc_timestamp[0]           <= 64'd0;
      desc_timestamp[1]           <= 64'd0;
      desc_fraction_q32[0]        <= 32'd0;
      desc_fraction_q32[1]        <= 32'd0;
      desc_discontinuity[0]       <= 1'b0;
      desc_discontinuity[1]       <= 1'b0;
      desc_config_revision[0]     <= 32'd0;
      desc_config_revision[1]     <= 32'd0;
      desc_calibration_revision[0] <= 32'd0;
      desc_calibration_revision[1] <= 32'd0;
    end else begin
      if (flush_partial) begin
        buffer_full[write_bank]                   <= 1'b1;
        desc_count[write_bank]                    <= write_count;
        desc_peak[write_bank]                     <= write_peak;
        desc_sum_squares[write_bank]              <= write_sum_squares;
        desc_power[write_bank]                    <= choose_power(write_peak);
        desc_timestamp[write_bank]                <= write_first_timestamp;
        desc_fraction_q32[write_bank]             <= write_first_fraction_q32;
        desc_discontinuity[write_bank]            <= write_discontinuity;
        desc_config_revision[write_bank]          <= write_config_revision;
        desc_calibration_revision[write_bank]     <= write_calibration_revision;
        write_bank                                <= !write_bank;
        write_index                               <= '0;
        write_count                               <= 32'd0;
        write_peak                                <= 16'd0;
        write_sum_squares                         <= 64'd0;
        write_discontinuity                       <= 1'b0;
      end else if (revision_change && !buffer_full[write_bank]) begin
        buffer_full[write_bank]                   <= 1'b1;
        desc_count[write_bank]                    <= write_count;
        desc_peak[write_bank]                     <= write_peak;
        desc_sum_squares[write_bank]              <= write_sum_squares;
        desc_power[write_bank]                    <= choose_power(write_peak);
        desc_timestamp[write_bank]                <= write_first_timestamp;
        desc_fraction_q32[write_bank]             <= write_first_fraction_q32;
        desc_discontinuity[write_bank]            <= write_discontinuity;
        desc_config_revision[write_bank]          <= write_config_revision;
        desc_calibration_revision[write_bank]     <= write_calibration_revision;
        write_bank                                <= !write_bank;
        write_index                               <= '0;
        write_count                               <= 32'd0;
        write_peak                                <= 16'd0;
        write_sum_squares                         <= 64'd0;
        write_discontinuity                       <= 1'b0;
      end else if (s_valid && s_ready) begin
        if (write_bank)
          memory1[write_index] <= {s_q, s_i};
        else
          memory0[write_index] <= {s_q, s_i};
        if (write_count == 0) begin
          write_first_timestamp      <= s_source_timestamp;
          write_first_fraction_q32   <= s_source_fraction_q32;
          write_config_revision      <= s_config_revision;
          write_calibration_revision <= s_calibration_revision;
        end
        write_count       <= write_count + 32'd1;
        write_index       <= write_index + 1'b1;
        write_peak        <= next_peak;
        write_sum_squares <= next_sum_squares;
        write_discontinuity <= write_discontinuity | s_discontinuity;
        if (close_block) begin
          buffer_full[write_bank]                  <= 1'b1;
          desc_count[write_bank]                   <= write_count + 32'd1;
          desc_peak[write_bank]                    <= next_peak;
          desc_sum_squares[write_bank]              <= next_sum_squares;
          desc_power[write_bank]                   <= choose_power(next_peak);
          desc_timestamp[write_bank]               <= write_count == 0 ? s_source_timestamp : write_first_timestamp;
          desc_fraction_q32[write_bank]            <= write_count == 0 ? s_source_fraction_q32 : write_first_fraction_q32;
          desc_discontinuity[write_bank]           <= write_discontinuity | s_discontinuity;
          desc_config_revision[write_bank]         <= write_count == 0 ? s_config_revision : write_config_revision;
          desc_calibration_revision[write_bank]    <= write_count == 0 ? s_calibration_revision : write_calibration_revision;
          write_bank                               <= !write_bank;
          write_index                              <= '0;
          write_count                              <= 32'd0;
          write_peak                               <= 16'd0;
          write_sum_squares                        <= 64'd0;
          write_discontinuity                      <= 1'b0;
        end
      end

      if (!read_active) begin
        if (buffer_full[0]) begin
          read_active <= 1'b1;
          read_bank   <= 1'b0;
          read_index  <= '0;
        end else if (buffer_full[1]) begin
          read_active <= 1'b1;
          read_bank   <= 1'b1;
          read_index  <= '0;
        end
      end else if (m_ready) begin
        if (read_index + 1 == desc_count[read_bank]) begin
          buffer_full[read_bank] <= 1'b0;
          read_active            <= 1'b0;
          read_index             <= '0;
        end else begin
          read_index <= read_index + 1'b1;
        end
      end
    end
  end
endmodule
