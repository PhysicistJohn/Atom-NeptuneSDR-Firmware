`timescale 1ns/1ps
module tb_neptune_data_header_builder;
  logic clk = 0;
  logic rst_n = 0;
  logic clear_extensions = 0;
  logic extension_valid = 0;
  logic extension_ready;
  logic [7:0] extension_byte = 0;
  logic start = 0;
  logic start_ready;
  logic [7:0] packet_type = 8;
  logic [7:0] sample_format = 0;
  logic [31:0] flags = 32'h80;
  logic [31:0] stream_id = 1;
  logic [63:0] sequence_number = 2;
  logic [63:0] sample_timestamp = 3;
  logic [31:0] sample_count = 0;
  logic [15:0] channel_mask = 0;
  logic [15:0] extension_count = 0;
  logic [31:0] payload_length = 0;
  logic [31:0] configuration_revision = 4;
  logic [31:0] calibration_revision = 5;
  logic [31:0] discontinuity_revision = 6;
  logic [31:0] device_state_revision = 7;
  logic m_valid;
  logic m_ready = 1;
  logic [31:0] m_data;
  logic m_last;
  logic busy;
  logic error;
  logic [31:0] expected [0:17];
  integer observed = 0;
  integer expected_words = 16;
  integer byte_index;

  always #5 clk = ~clk;
  neptune_data_header_builder dut (.*);

  always @(posedge clk) begin
    #1;
    if (m_valid && m_ready) begin
      if (m_data !== expected[observed])
        $fatal(1, "NEDP word %0d mismatch got=%08x expected=%08x",
               observed, m_data, expected[observed]);
      if ((observed == expected_words - 1) !== m_last)
        $fatal(1, "NEDP last mismatch");
      observed = observed + 1;
    end
  end

  initial begin
    expected[0]  = 32'h5044454e; expected[1]  = 32'h00081001;
    expected[2]  = 32'h00000080; expected[3]  = 32'h00000001;
    expected[4]  = 32'h00000002; expected[5]  = 32'h00000000;
    expected[6]  = 32'h00000003; expected[7]  = 32'h00000000;
    expected[8]  = 32'h00000000; expected[9]  = 32'h00000000;
    expected[10] = 32'h00000000; expected[11] = 32'h00000004;
    expected[12] = 32'h00000005; expected[13] = 32'h00000006;
    expected[14] = 32'h00000007; expected[15] = 32'h27c177db;
    repeat (2) @(posedge clk);
    @(negedge clk); rst_n = 1; start = 1;
    @(posedge clk); #1;
    @(negedge clk); start = 0;
    wait (observed == 16);
    @(posedge clk); #1;
    if (busy || error) $fatal(1, "header builder did not return idle");

    // A clear and start in the same cycle is rejected rather than committing
    // stale or ambiguously cleared extension state.
    @(negedge clk); clear_extensions = 1; start = 1;
    @(posedge clk); #1;
    if (!error || busy) $fatal(1, "conflicting clear/start was not rejected");
    @(negedge clk); clear_extensions = 0; start = 0;

    // Load an unknown, structurally valid eight-byte extension. Start is a
    // separate handshake after the final byte, so the captured length and CRC
    // must include that last byte.
    for (byte_index = 0; byte_index < 8; byte_index = byte_index + 1) begin
      @(negedge clk);
      case (byte_index)
        0: extension_byte = 8'h64;
        2: extension_byte = 8'h02;
        default: extension_byte = 8'h00;
      endcase
      extension_valid = 1;
      if (!extension_ready) $fatal(1, "extension byte was not accepted");
      @(posedge clk); #1;
      @(negedge clk); extension_valid = 0;
    end
    expected[0]  = 32'h5044454e; expected[1]  = 32'h00081201;
    expected[2]  = 32'h00000080; expected[3]  = 32'h00000001;
    expected[4]  = 32'h00000002; expected[5]  = 32'h00000000;
    expected[6]  = 32'h00000003; expected[7]  = 32'h00000000;
    expected[8]  = 32'h00000000; expected[9]  = 32'h00010000;
    expected[10] = 32'h00000000; expected[11] = 32'h00000004;
    expected[12] = 32'h00000005; expected[13] = 32'h00000006;
    expected[14] = 32'h00000007; expected[15] = 32'h5a97e068;
    expected[16] = 32'h00020064; expected[17] = 32'h00000000;
    observed = 0; expected_words = 18; extension_count = 1;
    @(negedge clk); start = 1;
    if (!start_ready) $fatal(1, "completed extension block was not committable");
    @(posedge clk); #1;
    @(negedge clk); start = 0;
    wait (observed == 18);
    @(posedge clk); #1;
    if (busy || error) $fatal(1, "extension header did not return idle");
    $display("tb_neptune_data_header_builder PASS");
    $finish;
  end
endmodule
