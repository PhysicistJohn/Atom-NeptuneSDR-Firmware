// SPDX-License-Identifier: MIT
module neptune_dc_correct (
  input  logic               bypass,
  input  logic signed [15:0] sample_i,
  input  logic signed [15:0] sample_q,
  input  logic signed [15:0] dc_i,
  input  logic signed [15:0] dc_q,
  output logic signed [15:0] corrected_i,
  output logic signed [15:0] corrected_q,
  output logic [1:0]         clipping,
  output logic               clipped
);
  logic signed [39:0] wide_i;
  logic signed [39:0] wide_q;

  always_comb begin
    wide_i = {{24{sample_i[15]}}, sample_i} - {{24{dc_i[15]}}, dc_i};
    wide_q = {{24{sample_q[15]}}, sample_q} - {{24{dc_q[15]}}, dc_q};
    clipping = 2'b00;
    if (bypass) begin
      corrected_i = sample_i;
      corrected_q = sample_q;
    end else begin
      if (wide_i > 40'sd32767) begin corrected_i = 16'sh7fff; clipping[0] = 1'b1; end
      else if (wide_i < -40'sd32768) begin corrected_i = 16'sh8000; clipping[0] = 1'b1; end
      else corrected_i = wide_i[15:0];
      if (wide_q > 40'sd32767) begin corrected_q = 16'sh7fff; clipping[1] = 1'b1; end
      else if (wide_q < -40'sd32768) begin corrected_q = 16'sh8000; clipping[1] = 1'b1; end
      else corrected_q = wide_q[15:0];
    end
  end
  assign clipped = |clipping;
endmodule
