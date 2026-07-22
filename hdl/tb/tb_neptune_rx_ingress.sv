`timescale 1ns/1ps
module tb_neptune_rx_ingress;
  logic clk = 0;
  logic rst_n = 0;
  logic sample_tick = 0;
  logic [63:0] timestamp = 0;
  logic timestamp_discontinuity = 0;
  logic interface_error = 0;
  logic [3:0] channel_mask = 1;
  logic [31:0] config_revision = 7;
  logic [31:0] calibration_revision = 9;
  logic [63:0] sample_data = 0;
  logic stream_valid;
  logic stream_ready = 0;
  logic [63:0] stream_data;
  logic [63:0] stream_timestamp;
  logic [31:0] stream_config_revision;
  logic [31:0] stream_calibration_revision;
  logic [3:0] stream_channel_mask;
  logic [3:0] stream_flags;
  logic overflow_sticky;
  logic overflow_event;
  logic [63:0] dropped_samples;

  always #5 clk = ~clk;
  neptune_rx_ingress dut (.*);

  task push(input logic [63:0] data, input logic [63:0] stamp);
    begin
      @(negedge clk); sample_data = data; timestamp = stamp; sample_tick = 1;
      @(posedge clk); #1;
      @(negedge clk); sample_tick = 0;
    end
  endtask

  initial begin
    repeat (2) @(posedge clk);
    @(negedge clk); rst_n = 1;
    push(64'h1111, 64'd10);
    if (!stream_valid || stream_data !== 64'h1111 || !stream_flags[0])
      $fatal(1, "first raw sample was not retained and marked");
    push(64'h2222, 64'd11);
    if (!overflow_sticky || dropped_samples !== 64'd1)
      $fatal(1, "non-backpressurable overflow was not counted");
    @(negedge clk); stream_ready = 1;
    push(64'h3333, 64'd12);
    if (!stream_valid || stream_data !== 64'h3333 || !stream_flags[0])
      $fatal(1, "post-overflow sample lacks discontinuity");
    $display("tb_neptune_rx_ingress PASS");
    $finish;
  end
endmodule
