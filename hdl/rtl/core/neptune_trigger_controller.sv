// SPDX-License-Identifier: MIT
// Circular DDR capture bookkeeping. DMA owns memory writes; this block owns
// deterministic trigger position and pre/post window completion metadata.
module neptune_trigger_controller (
  input  logic        clk,
  input  logic        rst_n,
  input  logic        arm,
  input  logic        disarm,
  input  logic        sample_written,
  input  logic [63:0] sample_timestamp,
  input  logic        trigger,
  input  logic [7:0]  trigger_source,
  input  logic [31:0] ring_samples,
  input  logic [31:0] pretrigger_samples,
  input  logic [31:0] posttrigger_samples,
  output logic        armed,
  output logic        capture_active,
  output logic        capture_done,
  output logic [31:0] write_index,
  output logic [31:0] capture_start_index,
  output logic [31:0] capture_sample_count,
  output logic [63:0] trigger_timestamp,
  output logic [7:0]  latched_trigger_source,
  output logic        configuration_error
);
  logic [31:0] post_remaining;
  logic [32:0] requested_window;

  always_comb requested_window = {1'b0, pretrigger_samples} +
                                   {1'b0, posttrigger_samples} + 33'd1;

  function automatic logic [31:0] subtract_modulo(
    input logic [31:0] value,
    input logic [31:0] amount,
    input logic [31:0] modulus
  );
    logic [32:0] extended;
    begin
      if (value >= amount)
        subtract_modulo = value - amount;
      else begin
        extended = {1'b0, value} + {1'b0, modulus} - {1'b0, amount};
        subtract_modulo = extended[31:0];
      end
    end
  endfunction

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      armed                   <= 1'b0;
      capture_active          <= 1'b0;
      capture_done            <= 1'b0;
      write_index             <= 32'd0;
      capture_start_index     <= 32'd0;
      capture_sample_count    <= 32'd0;
      trigger_timestamp       <= 64'd0;
      latched_trigger_source  <= 8'd0;
      post_remaining          <= 32'd0;
      configuration_error     <= 1'b0;
    end else begin
      capture_done <= 1'b0;
      if (disarm) begin
        armed          <= 1'b0;
        capture_active <= 1'b0;
      end
      if (arm) begin
        configuration_error <= ring_samples == 0 ||
                               requested_window > {1'b0, ring_samples};
        if (ring_samples != 0 &&
            requested_window <= {1'b0, ring_samples}) begin
          armed          <= 1'b1;
          capture_active <= 1'b0;
        end
      end
      if (sample_written && ring_samples != 0) begin
        write_index <= write_index + 32'd1 == ring_samples ? 32'd0 : write_index + 32'd1;
        if (armed && !capture_active && trigger) begin
          capture_active         <= 1'b1;
          armed                  <= 1'b0;
          trigger_timestamp      <= sample_timestamp;
          latched_trigger_source <= trigger_source;
          capture_start_index    <= subtract_modulo(write_index, pretrigger_samples, ring_samples);
          capture_sample_count   <= requested_window[31:0];
          post_remaining         <= posttrigger_samples;
          if (posttrigger_samples == 0) begin
            capture_active <= 1'b0;
            capture_done   <= 1'b1;
          end
        end else if (capture_active) begin
          if (post_remaining == 32'd1) begin
            post_remaining <= 32'd0;
            capture_active <= 1'b0;
            capture_done   <= 1'b1;
          end else if (post_remaining != 0) begin
            post_remaining <= post_remaining - 32'd1;
          end
        end
      end
    end
  end
endmodule
