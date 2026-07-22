`timescale 1ns/1ps
module tb_neptune_stft_scheduler;
  logic clk = 0;
  logic rst_n = 0;
  logic flush = 0;
  logic sample_valid = 0;
  logic [63:0] sample_timestamp = 0;
  logic [12:0] fft_size = 8;
  logic [12:0] hop_size = 4;
  logic frame_valid;
  logic [63:0] frame_timestamp;
  logic [63:0] frame_index;
  logic configuration_error;
  integer frames = 0;

  always #5 clk = ~clk;
  neptune_stft_scheduler dut (.*);
  always @(posedge clk) begin
    #1;
    if (frame_valid) begin
      if ((frames == 0 && frame_timestamp != 0) ||
          (frames == 1 && frame_timestamp != 4))
        $fatal(1, "STFT timestamp mismatch");
      frames = frames + 1;
    end
  end

  initial begin
    repeat (2) @(posedge clk);
    @(negedge clk); rst_n = 1; sample_valid = 1;
    repeat (12) begin
      @(posedge clk); #1;
      @(negedge clk); sample_timestamp = sample_timestamp + 1;
    end
    if (frames != 2 || frame_index != 2 || configuration_error)
      $fatal(1, "STFT hop schedule mismatch frames=%0d", frames);
    $display("tb_neptune_stft_scheduler PASS");
    $finish;
  end
endmodule
