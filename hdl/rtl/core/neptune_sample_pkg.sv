// SPDX-License-Identifier: MIT
package neptune_sample_pkg;
  localparam int SAMPLE_BITS = 16;
  localparam int ADC_BITS = 12;

  typedef struct packed {
    logic signed [SAMPLE_BITS-1:0] i;
    logic signed [SAMPLE_BITS-1:0] q;
  } iq16_t;

  typedef struct packed {
    iq16_t ch1;
    iq16_t ch0;
  } dual_iq16_t;

  typedef struct packed {
    logic        discontinuity;
    logic        valid_sample;
    logic        interface_error;
    logic        clipped;
    logic [3:0]  channel_mask;
    logic [31:0] config_revision;
    logic [31:0] calibration_revision;
    logic [63:0] sample_timestamp;
  } sample_metadata_t;

  function automatic logic signed [15:0] saturate_s16(input logic signed [39:0] value);
    if (value > 40'sd32767)
      saturate_s16 = 16'sh7fff;
    else if (value < -40'sd32768)
      saturate_s16 = 16'sh8000;
    else
      saturate_s16 = value[15:0];
  endfunction

  function automatic logic [15:0] magnitude_abs_s16(input logic signed [15:0] value);
    if (value == -16'sd32768)
      magnitude_abs_s16 = 16'h8000;
    else if (value < 0)
      magnitude_abs_s16 = $unsigned(-value);
    else
      magnitude_abs_s16 = $unsigned(value);
  endfunction
endpackage
