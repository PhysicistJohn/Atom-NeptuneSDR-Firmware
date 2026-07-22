#include "neptune_data_service.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    nds_dma_block block;
    nds_rf_snapshot snapshot;
    nds_packet packet;
    memset(&block, 0, sizeof(block));
    memset(&snapshot, 0, sizeof(snapshot));
    block.flags = 1U;
    block.stream_id = 7U;
    block.packet_type = NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ;
    block.channel_mask = 1U;
    block.sample_format = NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF;
    block.sample_count = 4U;
    block.block_rms_q16_16 = 2U << 16;
    block.block_peak_q16_16 = 7U << 16;
    block.quantization_exponent = 4;
    block.configuration_revision = 11U;
    block.calibration_revision = 3U;
    block.device_state_revision = 2U;
    block.source_sequence = 9U;
    block.sample_timestamp = UINT64_C(123456);
    block.output_sample_index = 100U;
    block.resampler_phase_numerator = 17U;
    block.payload = payload;
    block.payload_bytes = sizeof(payload);
    snapshot.device_state_revision = 2U;
    snapshot.activation_timestamp = 0U;
    snapshot.state.center_frequency_hz = UINT64_C(915000000);
    snapshot.state.sample_rate_hz = NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ;
    snapshot.state.rf_bandwidth_hz = UINT32_C(20000000);
    snapshot.state.rx1_gain_mdb = 30000;
    snapshot.state.digital_gain_q16_16 = 1 << 16;
    snapshot.state.temperature_mc = 42000;
    snapshot.state.pll_lock_mask = 1U;
    snapshot.state.configuration_revision = 11U;
    snapshot.state.metadata_complete = true;
    if (nds_build_packet(&block, &snapshot, 5U, 0, 0, 0, 0, &packet) != 0) {
        return 1;
    }
    if (fwrite(packet.prefix, 1, packet.prefix_bytes, stdout) !=
            packet.prefix_bytes ||
        fwrite(packet.payload, 1, packet.payload_bytes, stdout) !=
            packet.payload_bytes) {
        return 1;
    }
    return 0;
}
