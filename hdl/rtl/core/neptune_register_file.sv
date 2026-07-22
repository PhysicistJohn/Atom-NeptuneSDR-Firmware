// SPDX-License-Identifier: MIT
module neptune_register_file (
  input  logic        clk,
  input  logic        rst_n,
  input  logic        write_valid,
  input  logic [11:0] write_address,
  input  logic [31:0] write_data,
  input  logic [3:0]  write_strobe,
  output logic        write_error,
  input  logic        read_valid,
  input  logic [11:0] read_address,
  output logic [31:0] read_data,
  output logic        read_error,

  input  logic [63:0] build_id,
  input  logic [31:0] capabilities,
  input  logic [31:0] global_status,
  input  logic [31:0] fault_events,
  input  logic [63:0] sample_count,
  input  logic [31:0] sample_epoch,
  input  logic [31:0] discontinuity_revision,
  input  logic [31:0] active_config_revision,
  input  logic [31:0] active_calibration_revision,
  input  logic [31:0] config_status,
  input  logic [31:0] stream_status,
  input  logic [63:0] stream_sequence,
  input  logic [63:0] stream_dropped,
  input  logic [31:0] dma_fifo_high_water,
  input  logic [31:0] dma_fifo_overflows,
  input  logic [31:0] dma_descriptor_starvations,
  input  logic [63:0] dma_completed_blocks,
  input  logic [31:0] coefficient_status,
  input  logic [63:0] trigger_timestamp,
  input  logic [31:0] trigger_status,
  input  logic [63:0] detector_events,
  input  logic [31:0] tx_safety_status,
  input  logic [31:0] tx_arm_challenge,
  input  logic [31:0] tx_disarm_revision,

  output logic [63:0] sample_set_value,
  output logic        sample_set_pulse,
  output logic [31:0] config_shadow_control,
  output logic [31:0] config_expected_revision,
  output logic [63:0] config_activation_timestamp,
  output logic        config_commit_pulse,
  output logic [31:0] stream_id,
  output logic [31:0] stream_control,
  output logic [31:0] stream_format,
  output logic [31:0] stream_packet_samples,
  output logic        dma_counter_reset_pulse,
  output logic [31:0] coefficient_address,
  output logic [31:0] coefficient_i,
  output logic [31:0] coefficient_q,
  output logic        coefficient_write_pulse,
  output logic        coefficient_commit_pulse,
  output logic [31:0] trigger_control,
  output logic [31:0] trigger_ring_samples,
  output logic [31:0] trigger_pre_samples,
  output logic [31:0] trigger_post_samples,
  output logic [31:0] detector_control,
  output logic [31:0] detector_threshold,
  output logic [31:0] detector_holdoff,
  output logic        tx_persistent_inhibit,
  output logic [31:0] tx_arm_response,
  output logic        tx_arm_response_pulse,
  output logic        tx_disarm_pulse,
  output logic [31:0] sticky_faults
);
  import neptune_pl_registers_v1_pkg::*;

  logic [31:0] sample_count_hi_latch;

  function automatic logic [31:0] merge_strobes(
    input logic [31:0] previous,
    input logic [31:0] value,
    input logic [3:0] strobes
  );
    integer byte_index;
    begin
      merge_strobes = previous;
      for (byte_index = 0; byte_index < 4; byte_index = byte_index + 1)
        if (strobes[byte_index])
          merge_strobes[byte_index*8 +: 8] = value[byte_index*8 +: 8];
    end
  endfunction

  always_comb begin
    read_error = 1'b0;
    case (read_address)
      REG_MAGIC:                         read_data = MAGIC;
      REG_ABI_VERSION:                   read_data = ABI_VERSION;
      REG_BUILD_ID_LO:                   read_data = build_id[31:0];
      REG_BUILD_ID_HI:                   read_data = build_id[63:32];
      REG_CAPABILITIES:                  read_data = capabilities;
      REG_GLOBAL_STATUS:                 read_data = global_status;
      REG_GLOBAL_FAULTS:                 read_data = sticky_faults;
      REG_SAMPLE_COUNT_LO:               read_data = sample_count[31:0];
      REG_SAMPLE_COUNT_HI:               read_data = sample_count_hi_latch;
      REG_SAMPLE_EPOCH:                  read_data = sample_epoch;
      REG_DISCONTINUITY_REVISION:        read_data = discontinuity_revision;
      REG_SAMPLE_SET_LO:                 read_data = sample_set_value[31:0];
      REG_SAMPLE_SET_HI:                 read_data = sample_set_value[63:32];
      REG_CONFIG_SHADOW_CONTROL:         read_data = config_shadow_control;
      REG_CONFIG_EXPECTED_REVISION:      read_data = config_expected_revision;
      REG_CONFIG_ACTIVE_REVISION:        read_data = active_config_revision;
      REG_CALIBRATION_ACTIVE_REVISION:   read_data = active_calibration_revision;
      REG_CONFIG_ACTIVATE_LO:            read_data = config_activation_timestamp[31:0];
      REG_CONFIG_ACTIVATE_HI:            read_data = config_activation_timestamp[63:32];
      REG_CONFIG_STATUS:                 read_data = config_status;
      REG_STREAM0_CONTROL:               read_data = stream_control;
      REG_STREAM0_STATUS:                read_data = stream_status;
      REG_STREAM0_FORMAT:                read_data = stream_format;
      REG_STREAM0_PACKET_SAMPLES:        read_data = stream_packet_samples;
      REG_STREAM0_SEQUENCE_LO:           read_data = stream_sequence[31:0];
      REG_STREAM0_SEQUENCE_HI:           read_data = stream_sequence[63:32];
      REG_STREAM0_DROPPED_LO:            read_data = stream_dropped[31:0];
      REG_STREAM0_DROPPED_HI:            read_data = stream_dropped[63:32];
      REG_STREAM0_ID:                    read_data = stream_id;
      REG_DMA_FIFO_HIGH_WATER:            read_data = dma_fifo_high_water;
      REG_DMA_FIFO_OVERFLOWS:             read_data = dma_fifo_overflows;
      REG_DMA_DESCRIPTOR_STARVATIONS:     read_data = dma_descriptor_starvations;
      REG_DMA_COMPLETED_BLOCKS_LO:        read_data = dma_completed_blocks[31:0];
      REG_DMA_COMPLETED_BLOCKS_HI:        read_data = dma_completed_blocks[63:32];
      REG_COEFFICIENT_ADDRESS:            read_data = coefficient_address;
      REG_COEFFICIENT_I:                  read_data = coefficient_i;
      REG_COEFFICIENT_Q:                  read_data = coefficient_q;
      REG_COEFFICIENT_STATUS:             read_data = coefficient_status;
      REG_TRIGGER_CONTROL:                read_data = trigger_control;
      REG_TRIGGER_RING_SAMPLES:           read_data = trigger_ring_samples;
      REG_TRIGGER_PRE_SAMPLES:            read_data = trigger_pre_samples;
      REG_TRIGGER_POST_SAMPLES:           read_data = trigger_post_samples;
      REG_TRIGGER_TIMESTAMP_LO:           read_data = trigger_timestamp[31:0];
      REG_TRIGGER_TIMESTAMP_HI:           read_data = trigger_timestamp[63:32];
      REG_TRIGGER_STATUS:                 read_data = trigger_status;
      REG_DETECTOR_CONTROL:               read_data = detector_control;
      REG_DETECTOR_THRESHOLD:             read_data = detector_threshold;
      REG_DETECTOR_HOLDOFF:               read_data = detector_holdoff;
      REG_DETECTOR_EVENTS_LO:             read_data = detector_events[31:0];
      REG_DETECTOR_EVENTS_HI:             read_data = detector_events[63:32];
      REG_TX_SAFETY_STATUS:              read_data = tx_safety_status;
      REG_TX_PERSISTENT_INHIBIT:         read_data = {31'd0, tx_persistent_inhibit};
      REG_TX_ARM_CHALLENGE:              read_data = tx_arm_challenge;
      REG_TX_DISARM_REVISION:            read_data = tx_disarm_revision;
      default: begin read_data = 32'd0; read_error = read_valid; end
    endcase
  end

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      sample_count_hi_latch       <= 32'd0;
      sample_set_value            <= 64'd0;
      sample_set_pulse            <= 1'b0;
      config_shadow_control       <= 32'd0;
      config_expected_revision    <= 32'd0;
      config_activation_timestamp <= 64'hffff_ffff_ffff_ffff;
      config_commit_pulse         <= 1'b0;
      stream_id                    <= 32'd0;
      stream_control              <= 32'd0;
      stream_format               <= 32'd0;
      stream_packet_samples       <= 32'd0;
      dma_counter_reset_pulse      <= 1'b0;
      coefficient_address         <= 32'd0;
      coefficient_i               <= 32'd0;
      coefficient_q               <= 32'd0;
      coefficient_write_pulse     <= 1'b0;
      coefficient_commit_pulse    <= 1'b0;
      trigger_control             <= 32'd0;
      trigger_ring_samples        <= 32'd0;
      trigger_pre_samples         <= 32'd0;
      trigger_post_samples        <= 32'd0;
      detector_control            <= 32'd0;
      detector_threshold          <= 32'd0;
      detector_holdoff            <= 32'd0;
      tx_persistent_inhibit       <= 1'b1;
      tx_arm_response             <= 32'd0;
      tx_arm_response_pulse       <= 1'b0;
      tx_disarm_pulse             <= 1'b0;
      sticky_faults               <= 32'd0;
      write_error                 <= 1'b0;
    end else begin
      sample_set_pulse      <= 1'b0;
      config_commit_pulse   <= 1'b0;
      dma_counter_reset_pulse  <= 1'b0;
      coefficient_write_pulse  <= 1'b0;
      coefficient_commit_pulse <= 1'b0;
      tx_arm_response_pulse <= 1'b0;
      tx_disarm_pulse       <= 1'b0;
      write_error           <= 1'b0;
      sticky_faults         <= sticky_faults | fault_events;

      if (read_valid && read_address == REG_SAMPLE_COUNT_LO)
        sample_count_hi_latch <= sample_count[63:32];

      if (write_valid) begin
        case (write_address)
          REG_GLOBAL_FAULTS:
            sticky_faults <= (sticky_faults &
                              ~merge_strobes(32'd0, write_data, write_strobe)) |
                             fault_events;
          REG_SAMPLE_SET_LO:
            sample_set_value[31:0] <= merge_strobes(sample_set_value[31:0], write_data, write_strobe);
          REG_SAMPLE_SET_HI:
            sample_set_value[63:32] <= merge_strobes(sample_set_value[63:32], write_data, write_strobe);
          REG_SAMPLE_COMMAND:
            if (&write_strobe) sample_set_pulse <= write_data[0];
            else write_error <= 1'b1;
          REG_CONFIG_SHADOW_CONTROL:
            config_shadow_control <= merge_strobes(config_shadow_control, write_data, write_strobe);
          REG_CONFIG_EXPECTED_REVISION:
            config_expected_revision <= merge_strobes(config_expected_revision, write_data, write_strobe);
          REG_CONFIG_ACTIVATE_LO:
            config_activation_timestamp[31:0] <= merge_strobes(config_activation_timestamp[31:0], write_data, write_strobe);
          REG_CONFIG_ACTIVATE_HI:
            config_activation_timestamp[63:32] <= merge_strobes(config_activation_timestamp[63:32], write_data, write_strobe);
          REG_CONFIG_COMMIT:
            if (&write_strobe) config_commit_pulse <= write_data[0];
            else write_error <= 1'b1;
          REG_STREAM0_ID:
            stream_id <= merge_strobes(stream_id, write_data, write_strobe);
          REG_STREAM0_CONTROL:
            stream_control <= merge_strobes(stream_control, write_data, write_strobe);
          REG_STREAM0_FORMAT:
            stream_format <= merge_strobes(stream_format, write_data, write_strobe);
          REG_STREAM0_PACKET_SAMPLES:
            stream_packet_samples <= merge_strobes(stream_packet_samples, write_data, write_strobe);
          REG_DMA_COUNTER_RESET:
            if (&write_strobe) dma_counter_reset_pulse <= write_data[0];
            else write_error <= 1'b1;
          REG_COEFFICIENT_ADDRESS:
            coefficient_address <= merge_strobes(coefficient_address, write_data, write_strobe);
          REG_COEFFICIENT_I:
            coefficient_i <= merge_strobes(coefficient_i, write_data, write_strobe);
          REG_COEFFICIENT_Q:
            coefficient_q <= merge_strobes(coefficient_q, write_data, write_strobe);
          REG_COEFFICIENT_WRITE:
            if (&write_strobe) coefficient_write_pulse <= write_data[0];
            else write_error <= 1'b1;
          REG_COEFFICIENT_COMMIT:
            if (&write_strobe) coefficient_commit_pulse <= write_data[0];
            else write_error <= 1'b1;
          REG_TRIGGER_CONTROL:
            trigger_control <= merge_strobes(trigger_control, write_data, write_strobe);
          REG_TRIGGER_RING_SAMPLES:
            trigger_ring_samples <= merge_strobes(trigger_ring_samples, write_data, write_strobe);
          REG_TRIGGER_PRE_SAMPLES:
            trigger_pre_samples <= merge_strobes(trigger_pre_samples, write_data, write_strobe);
          REG_TRIGGER_POST_SAMPLES:
            trigger_post_samples <= merge_strobes(trigger_post_samples, write_data, write_strobe);
          REG_DETECTOR_CONTROL:
            detector_control <= merge_strobes(detector_control, write_data, write_strobe);
          REG_DETECTOR_THRESHOLD:
            detector_threshold <= merge_strobes(detector_threshold, write_data, write_strobe);
          REG_DETECTOR_HOLDOFF:
            detector_holdoff <= merge_strobes(detector_holdoff, write_data, write_strobe);
          REG_TX_PERSISTENT_INHIBIT:
            if (&write_strobe) tx_persistent_inhibit <= write_data[0];
            else write_error <= 1'b1;
          REG_TX_ARM_RESPONSE: begin
            if (&write_strobe) begin
              tx_arm_response       <= write_data;
              tx_arm_response_pulse <= 1'b1;
            end else write_error <= 1'b1;
          end
          REG_TX_DISARM:
            if (&write_strobe) tx_disarm_pulse <= write_data != 0;
            else write_error <= 1'b1;
          default:
            write_error <= 1'b1;
        endcase
      end
    end
  end
endmodule
