// SPDX-License-Identifier: MIT
// Builds the canonical 64-byte NEDP v1 base header plus prevalidated typed
// extensions. The header is buffered logically until CRC-32C is known.
module neptune_data_header_builder #(
  parameter int MAX_EXTENSION_BYTES = 192
) (
  input  logic        clk,
  input  logic        rst_n,
  input  logic        clear_extensions,
  input  logic        extension_valid,
  output logic        extension_ready,
  input  logic [7:0]  extension_byte,
  input  logic        start,
  output logic        start_ready,
  input  logic [7:0]  packet_type,
  input  logic [7:0]  sample_format,
  input  logic [31:0] flags,
  input  logic [31:0] stream_id,
  input  logic [63:0] sequence_number,
  input  logic [63:0] sample_timestamp,
  input  logic [31:0] sample_count,
  input  logic [15:0] channel_mask,
  input  logic [15:0] extension_count,
  input  logic [31:0] payload_length,
  input  logic [31:0] configuration_revision,
  input  logic [31:0] calibration_revision,
  input  logic [31:0] discontinuity_revision,
  input  logic [31:0] device_state_revision,
  output logic        m_valid,
  input  logic        m_ready,
  output logic [31:0] m_data,
  output logic        m_last,
  output logic        busy,
  output logic        error
);
  import neptune_crc32c_pkg::crc32c_byte;

  typedef enum logic [1:0] {LOAD, CRC, OUTPUT} state_t;
  state_t state;
  logic [7:0] extension_memory [0:MAX_EXTENSION_BYTES-1];
  logic [$clog2(MAX_EXTENSION_BYTES+1)-1:0] extension_bytes;
  logic [9:0] header_bytes;
  logic [7:0] header_words;
  logic [9:0] crc_index;
  logic [31:0] crc_state;
  logic [31:0] final_crc;
  logic [7:0] output_word_index;

  logic [7:0] packet_type_q;
  logic [7:0] sample_format_q;
  logic [31:0] flags_q;
  logic [31:0] stream_id_q;
  logic [63:0] sequence_number_q;
  logic [63:0] sample_timestamp_q;
  logic [31:0] sample_count_q;
  logic [15:0] channel_mask_q;
  logic [15:0] extension_count_q;
  logic [31:0] payload_length_q;
  logic [31:0] configuration_revision_q;
  logic [31:0] calibration_revision_q;
  logic [31:0] discontinuity_revision_q;
  logic [31:0] device_state_revision_q;

  function automatic logic [7:0] byte_u16(input logic [15:0] value, input integer lane);
    byte_u16 = value >> (lane * 8);
  endfunction
  function automatic logic [7:0] byte_u32(input logic [31:0] value, input integer lane);
    byte_u32 = value >> (lane * 8);
  endfunction
  function automatic logic [7:0] byte_u64(input logic [63:0] value, input integer lane);
    byte_u64 = value >> (lane * 8);
  endfunction

  function automatic logic [7:0] header_byte(input integer index, input logic zero_crc);
    begin
      if (index == 0) header_byte = 8'h4e;
      else if (index == 1) header_byte = 8'h45;
      else if (index == 2) header_byte = 8'h44;
      else if (index == 3) header_byte = 8'h50;
      else if (index == 4) header_byte = 8'd1;
      else if (index == 5) header_byte = header_words;
      else if (index == 6) header_byte = packet_type_q;
      else if (index == 7) header_byte = sample_format_q;
      else if (index < 12) header_byte = byte_u32(flags_q, index - 8);
      else if (index < 16) header_byte = byte_u32(stream_id_q, index - 12);
      else if (index < 24) header_byte = byte_u64(sequence_number_q, index - 16);
      else if (index < 32) header_byte = byte_u64(sample_timestamp_q, index - 24);
      else if (index < 36) header_byte = byte_u32(sample_count_q, index - 32);
      else if (index < 38) header_byte = byte_u16(channel_mask_q, index - 36);
      else if (index < 40) header_byte = byte_u16(extension_count_q, index - 38);
      else if (index < 44) header_byte = byte_u32(payload_length_q, index - 40);
      else if (index < 48) header_byte = byte_u32(configuration_revision_q, index - 44);
      else if (index < 52) header_byte = byte_u32(calibration_revision_q, index - 48);
      else if (index < 56) header_byte = byte_u32(discontinuity_revision_q, index - 52);
      else if (index < 60) header_byte = byte_u32(device_state_revision_q, index - 56);
      else if (index < 64) header_byte = zero_crc ? 8'd0 : byte_u32(final_crc, index - 60);
      else header_byte = extension_memory[index - 64];
    end
  endfunction

  wire [31:0] crc_next = crc32c_byte(crc_state, header_byte(crc_index, 1'b1));

  always_comb begin
    // Loading, clearing, and committing are mutually exclusive handshakes.
    // In particular, start may never capture a stale extension count while a
    // final extension byte is still scheduled by a nonblocking assignment.
    extension_ready = state == LOAD && !start && !clear_extensions &&
                      extension_bytes < MAX_EXTENSION_BYTES;
    start_ready = state == LOAD && !extension_valid && !clear_extensions;
    busy = state != LOAD;
    m_valid = state == OUTPUT;
    m_data[7:0]   = header_byte(output_word_index * 4, 1'b0);
    m_data[15:8]  = header_byte(output_word_index * 4 + 1, 1'b0);
    m_data[23:16] = header_byte(output_word_index * 4 + 2, 1'b0);
    m_data[31:24] = header_byte(output_word_index * 4 + 3, 1'b0);
    m_last = state == OUTPUT && output_word_index + 1 == header_words;
  end

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      state                       <= LOAD;
      extension_bytes             <= '0;
      header_bytes                <= 10'd64;
      header_words                <= 8'd16;
      crc_index                   <= 10'd0;
      crc_state                   <= 32'hffff_ffff;
      final_crc                   <= 32'd0;
      output_word_index           <= 8'd0;
      error                       <= 1'b0;
      packet_type_q               <= 8'd0;
      sample_format_q              <= 8'd0;
      flags_q                     <= 32'd0;
      stream_id_q                 <= 32'd0;
      sequence_number_q            <= 64'd0;
      sample_timestamp_q           <= 64'd0;
      sample_count_q               <= 32'd0;
      channel_mask_q               <= 16'd0;
      extension_count_q            <= 16'd0;
      payload_length_q              <= 32'd0;
      configuration_revision_q     <= 32'd0;
      calibration_revision_q       <= 32'd0;
      discontinuity_revision_q     <= 32'd0;
      device_state_revision_q       <= 32'd0;
    end else begin
      error <= 1'b0;
      if (state == LOAD) begin
        if (clear_extensions)
          extension_bytes <= '0;
        else if (extension_valid && extension_ready) begin
          extension_memory[extension_bytes] <= extension_byte;
          extension_bytes <= extension_bytes + 1'b1;
        end
        if (start) begin
          if (!start_ready || extension_bytes[1:0] != 0 ||
              extension_bytes > MAX_EXTENSION_BYTES) begin
            error <= 1'b1;
          end else begin
            packet_type_q               <= packet_type;
            sample_format_q              <= sample_format;
            flags_q                     <= flags;
            stream_id_q                 <= stream_id;
            sequence_number_q            <= sequence_number;
            sample_timestamp_q           <= sample_timestamp;
            sample_count_q               <= sample_count;
            channel_mask_q               <= channel_mask;
            extension_count_q            <= extension_count;
            payload_length_q              <= payload_length;
            configuration_revision_q     <= configuration_revision;
            calibration_revision_q       <= calibration_revision;
            discontinuity_revision_q     <= discontinuity_revision;
            device_state_revision_q       <= device_state_revision;
            header_bytes                 <= 64 + extension_bytes;
            header_words                 <= (64 + extension_bytes) >> 2;
            crc_index                    <= 10'd0;
            crc_state                    <= 32'hffff_ffff;
            state                        <= CRC;
          end
        end
      end else if (state == CRC) begin
        crc_state <= crc_next;
        if (crc_index + 1 == header_bytes) begin
          final_crc         <= crc_next ^ 32'hffff_ffff;
          output_word_index <= 8'd0;
          state             <= OUTPUT;
        end else begin
          crc_index <= crc_index + 10'd1;
        end
      end else if (state == OUTPUT && m_ready) begin
        if (m_last) begin
          output_word_index <= 8'd0;
          extension_bytes   <= '0;
          state             <= LOAD;
        end else begin
          output_word_index <= output_word_index + 8'd1;
        end
      end
    end
  end
endmodule
