#include "neptune_data_service.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

#define DMA_F_VALID (1U << 0)
#define DMA_F_DISCONTINUITY (1U << 1)
#define DMA_F_OVERFLOW (1U << 2)
#define DMA_F_RETUNE (1U << 3)
#define DMA_F_RESTART (1U << 4)
#define DMA_F_INTERFACE_FAULT (1U << 5)
#define DMA_F_DMA_FAULT (1U << 6)
#define DMA_F_FIFO_FAULT (1U << 7)
#define DMA_F_CLIPPED (1U << 8)
#define DMA_F_CONFIGURATION_CHANGE (1U << 9)
#define DMA_F_CALIBRATION_CHANGE (1U << 10)
#define DMA_F_DEVICE_STATE_CHANGE (1U << 11)
#define DMA_F_TIMESTAMP_RESET (1U << 12)
#define DMA_F_KNOWN_MASK ((1U << 13) - 1U)

_Static_assert(
    NDS_MAX_PREFIX_BYTES ==
        NEPTUNE_EDGE_DATA_HEADER_BYTES +
            sizeof(neptune_edge_data_rf_state_v1) +
            sizeof(neptune_edge_data_quantization_v1) +
            sizeof(neptune_edge_data_resampler_state_v1) +
            sizeof(neptune_edge_data_discontinuity_v1) +
            sizeof(neptune_edge_data_block_stats_v1) +
            sizeof(neptune_edge_data_payload_crc_v1),
    "NDS_MAX_PREFIX_BYTES must cover every canonical v1 extension");

static void put16(uint8_t *value, uint16_t number)
{
    value[0] = (uint8_t)number;
    value[1] = (uint8_t)(number >> 8);
}

static void put32(uint8_t *value, uint32_t number)
{
    value[0] = (uint8_t)number;
    value[1] = (uint8_t)(number >> 8);
    value[2] = (uint8_t)(number >> 16);
    value[3] = (uint8_t)(number >> 24);
}

static void put64(uint8_t *value, uint64_t number)
{
    put32(value, (uint32_t)number);
    put32(value + 4, (uint32_t)(number >> 32));
}

static unsigned channel_count(uint32_t mask)
{
    return (mask & 1U) + ((mask >> 1) & 1U);
}

static unsigned bytes_per_channel_sample(uint32_t format)
{
    switch (format) {
    case NEPTUNE_EDGE_SAMPLE_FORMAT_S16:
        return 4U;
    case NEPTUNE_EDGE_SAMPLE_FORMAT_S12P:
        return 3U;
    case NEPTUNE_EDGE_SAMPLE_FORMAT_S8:
    case NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF:
        return 2U;
    default:
        return 0U;
    }
}

static size_t statistics_extension_bytes(uint32_t format)
{
    return sizeof(neptune_edge_data_block_stats_v1) +
           (format == NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF
                ? sizeof(neptune_edge_data_quantization_v1)
                : 0U);
}

static int validate_product_format(uint32_t packet_type, uint32_t format)
{
    if (packet_type == NEPTUNE_EDGE_PACKET_TYPE_RAW_IQ) {
        return format == NEPTUNE_EDGE_SAMPLE_FORMAT_S16 ||
                       format == NEPTUNE_EDGE_SAMPLE_FORMAT_S12P
                   ? 0
                   : -EINVAL;
    }
    if (packet_type == NEPTUNE_EDGE_PACKET_TYPE_CALIBRATED_IQ) {
        return format == NEPTUNE_EDGE_SAMPLE_FORMAT_S16 ||
                       format == NEPTUNE_EDGE_SAMPLE_FORMAT_S12P
                   ? 0
                   : -EINVAL;
    }
    if (packet_type == NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ) {
        return format == NEPTUNE_EDGE_SAMPLE_FORMAT_S8 ||
                       format == NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF
                   ? 0
                   : -EINVAL;
    }
    return -ENOTSUP;
}

static int validate_product_rate(uint32_t packet_type, uint32_t sample_rate_hz)
{
    if (packet_type == NEPTUNE_EDGE_PACKET_TYPE_RAW_IQ) {
        return sample_rate_hz == NEPTUNE_EDGE_INTERNAL_SAMPLE_RATE_HZ
                   ? 0
                   : -EINVAL;
    }
    if (packet_type == NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ) {
        return sample_rate_hz == NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ
                   ? 0
                   : -EINVAL;
    }
    if (packet_type == NEPTUNE_EDGE_PACKET_TYPE_CALIBRATED_IQ) {
        return sample_rate_hz == NEPTUNE_EDGE_INTERNAL_SAMPLE_RATE_HZ ||
                       sample_rate_hz == NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ
                   ? 0
                   : -EINVAL;
    }
    return -ENOTSUP;
}

static int validate_destination_value(const nds_destination *destination)
{
    const uint8_t *octets = destination->ipv4;
    if (destination->port == 0U ||
        (destination->mtu != 1500U && destination->mtu != 9000U) ||
        octets[0] == 0U || octets[0] >= 224U ||
        (!destination->allow_loopback && octets[0] == 127U) ||
        (octets[0] == 255U && octets[1] == 255U && octets[2] == 255U &&
         octets[3] == 255U)) {
        return -EADDRNOTAVAIL;
    }
    return 0;
}

int nds_parse_destination(nds_destination *destination, const char *ipv4,
                          uint16_t port, uint16_t mtu, bool allow_loopback)
{
    struct in_addr address;
    const uint8_t *octets = (const uint8_t *)&address.s_addr;
    if (destination == NULL || ipv4 == NULL || port == 0U ||
        (mtu != 1500U && mtu != 9000U) ||
        inet_pton(AF_INET, ipv4, &address) != 1) {
        return -EINVAL;
    }
    if (octets[0] == 0U || octets[0] >= 224U ||
        (!allow_loopback && octets[0] == 127U) ||
        (octets[0] == 255U && octets[1] == 255U && octets[2] == 255U &&
         octets[3] == 255U)) {
        return -EADDRNOTAVAIL;
    }
    memset(destination, 0, sizeof(*destination));
    memcpy(destination->ipv4, octets, 4);
    destination->port = port;
    destination->mtu = mtu;
    destination->allow_loopback = allow_loopback;
    return 0;
}

int nds_validate_profile(const nds_stream_profile *profile,
                         uint64_t *wire_bits_per_second,
                         uint32_t *maximum_samples_per_packet)
{
    uint64_t prefix_bytes;
    uint64_t sample_bytes;
    uint64_t udp_payload;
    uint64_t packet_rate;
    uint64_t wire_rate;
    unsigned channels;
    unsigned stride;
    uint32_t maximum;
    if (profile == NULL || profile->stream_id == 0U ||
        profile->channel_mask == 0U || profile->channel_mask > 3U ||
        !profile->initial_snapshot.state.metadata_complete ||
        profile->sample_rate_hz == 0U ||
        profile->sample_rate_hz !=
            profile->initial_snapshot.state.sample_rate_hz ||
        profile->initial_snapshot.state.center_frequency_hz <
            UINT64_C(70000000) ||
        profile->initial_snapshot.state.center_frequency_hz >
            UINT64_C(6000000000) ||
        profile->initial_snapshot.state.rf_bandwidth_hz < UINT32_C(200000) ||
        profile->initial_snapshot.state.rf_bandwidth_hz > UINT32_C(56000000) ||
        ((profile->channel_mask & 1U) != 0U &&
         (profile->initial_snapshot.state.rx1_gain_mdb < -10000 ||
          profile->initial_snapshot.state.rx1_gain_mdb > 73000)) ||
        ((profile->channel_mask & 2U) != 0U &&
         (profile->initial_snapshot.state.rx2_gain_mdb < -10000 ||
          profile->initial_snapshot.state.rx2_gain_mdb > 73000)) ||
        profile->initial_snapshot.state.digital_gain_q16_16 <= 0 ||
        profile->initial_snapshot.state.temperature_mc < -40000 ||
        profile->initial_snapshot.state.temperature_mc > 125000 ||
        profile->initial_snapshot.state.rx1_gain_mode >
            NEPTUNE_EDGE_GAIN_MODE_HYBRID ||
        profile->initial_snapshot.state.rx2_gain_mode >
            NEPTUNE_EDGE_GAIN_MODE_HYBRID ||
        (profile->initial_snapshot.state.pll_lock_mask & ~3U) != 0U ||
        profile->batch_size == 0U ||
        profile->batch_size > NDS_MAX_BATCH ||
        validate_destination_value(&profile->destination) != 0 ||
        validate_product_format(profile->packet_type,
                                profile->sample_format) != 0) {
        return -EINVAL;
    }
    if (validate_product_rate(profile->packet_type,
                              profile->sample_rate_hz) != 0) {
        return -EINVAL;
    }
    channels = channel_count(profile->channel_mask);
    stride = bytes_per_channel_sample(profile->sample_format) * channels;
    if (stride == 0U) {
        return -EINVAL;
    }
    prefix_bytes = NEPTUNE_EDGE_DATA_HEADER_BYTES +
                   sizeof(neptune_edge_data_rf_state_v1) +
                   statistics_extension_bytes(profile->sample_format) +
                   sizeof(neptune_edge_data_discontinuity_v1) +
                   sizeof(neptune_edge_data_payload_crc_v1);
    if (profile->sample_rate_hz == NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ) {
        prefix_bytes += sizeof(neptune_edge_data_resampler_state_v1);
    }
    if ((uint64_t)profile->destination.mtu <= 28U + prefix_bytes) {
        return -EMSGSIZE;
    }
    maximum = (uint32_t)(((uint64_t)profile->destination.mtu - 28U -
                          prefix_bytes) /
                         stride);
    if (profile->samples_per_packet == 0U ||
        profile->samples_per_packet > maximum) {
        return -EMSGSIZE;
    }
    sample_bytes = (uint64_t)profile->samples_per_packet * stride;
    udp_payload = prefix_bytes + sample_bytes;
    packet_rate = ((uint64_t)profile->sample_rate_hz +
                   profile->samples_per_packet - 1U) /
                  profile->samples_per_packet;
    /* Ethernet preamble/SFD + MAC + IPv4 + UDP + FCS + IFG. */
    wire_rate = packet_rate * (UINT64_C(66) + udp_payload) * UINT64_C(8);
    if (wire_bits_per_second != NULL) {
        *wire_bits_per_second = wire_rate;
    }
    if (maximum_samples_per_packet != NULL) {
        *maximum_samples_per_packet = maximum;
    }
    return wire_rate <= UINT64_C(1000000000) ? 0 : -ERANGE;
}

static size_t append_quantization(uint8_t *output, const nds_dma_block *block)
{
    uint32_t numerator = 1U;
    uint32_t denominator = 1U;
    if (block->quantization_exponent >= 0) {
        denominator <<= (unsigned)block->quantization_exponent;
    } else {
        numerator <<= (unsigned)(-block->quantization_exponent);
    }
    memset(output, 0, sizeof(neptune_edge_data_quantization_v1));
    put16(output, NEPTUNE_EDGE_DATA_EXTENSION_QUANTIZATION);
    put16(output + 2,
          sizeof(neptune_edge_data_quantization_v1) / 4U);
    output[8] = (uint8_t)block->quantization_exponent;
    output[9] = 1U;
    output[10] = NEPTUNE_EDGE_QUANTIZATION_STRATEGY_PEAK;
    output[11] = NEPTUNE_EDGE_ROUNDING_MODE_ROUND_TO_NEAREST_EVEN;
    put32(output + 12, numerator);
    put32(output + 16, denominator);
    put32(output + 20, block->block_rms_q16_16);
    put32(output + 24, block->block_peak_q16_16);
    put32(output + 28, block->clipping_count);
    put32(output + 32, block->sample_count * channel_count(block->channel_mask));
    put32(output + 36, 0U);
    return sizeof(neptune_edge_data_quantization_v1);
}

static size_t append_rf_state(uint8_t *output, const nds_rf_state *rf_state,
                              uint32_t channel_mask)
{
    memset(output, 0, sizeof(neptune_edge_data_rf_state_v1));
    put16(output, NEPTUNE_EDGE_DATA_EXTENSION_RF_STATE);
    put16(output + 2, sizeof(neptune_edge_data_rf_state_v1) / 4U);
    put64(output + 8, rf_state->center_frequency_hz);
    put32(output + 16, rf_state->sample_rate_hz);
    put32(output + 20, rf_state->rf_bandwidth_hz);
    put32(output + 24, (uint32_t)rf_state->rx1_gain_mdb);
    put32(output + 28, (uint32_t)rf_state->rx2_gain_mdb);
    put32(output + 32, (uint32_t)rf_state->digital_gain_q16_16);
    put32(output + 36, (uint32_t)rf_state->temperature_mc);
    output[40] = rf_state->rx1_gain_mode;
    output[41] = rf_state->rx2_gain_mode;
    output[42] = rf_state->pll_lock_mask;
    output[43] = (uint8_t)channel_mask;
    put32(output + 44, rf_state->device_flags);
    return sizeof(neptune_edge_data_rf_state_v1);
}

static size_t append_resampler(uint8_t *output, const nds_dma_block *block)
{
    memset(output, 0, sizeof(neptune_edge_data_resampler_state_v1));
    put16(output, NEPTUNE_EDGE_DATA_EXTENSION_RESAMPLER_STATE);
    put16(output + 2,
          sizeof(neptune_edge_data_resampler_state_v1) / 4U);
    put32(output + 8, NEPTUNE_EDGE_INTERNAL_SAMPLE_RATE_HZ);
    put32(output + 12, NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ);
    put16(output + 16, NEPTUNE_EDGE_RESAMPLER_INTERPOLATION);
    put16(output + 18, NEPTUNE_EDGE_RESAMPLER_DECIMATION);
    put16(output + 20, (uint16_t)block->resampler_phase_numerator);
    put16(output + 22, NEPTUNE_EDGE_RESAMPLER_INTERPOLATION);
    put64(output + 24, block->sample_timestamp);
    put64(output + 32, block->output_sample_index);
    return sizeof(neptune_edge_data_resampler_state_v1);
}

static uint16_t discontinuity_reason(const nds_dma_block *block,
                                     uint16_t service_reason)
{
    if (service_reason != 0U) {
        return service_reason;
    }
    if ((block->flags & DMA_F_TIMESTAMP_RESET) != 0U) {
        return NEPTUNE_EDGE_DISCONTINUITY_REASON_SAMPLE_COUNTER_RESET;
    }
    if ((block->flags & DMA_F_FIFO_FAULT) != 0U) {
        return NEPTUNE_EDGE_DISCONTINUITY_REASON_FIFO_OVERFLOW;
    }
    if ((block->flags & (DMA_F_DMA_FAULT | DMA_F_OVERFLOW)) != 0U) {
        return NEPTUNE_EDGE_DISCONTINUITY_REASON_DMA_OVERRUN;
    }
    if ((block->flags & DMA_F_RETUNE) != 0U) {
        return NEPTUNE_EDGE_DISCONTINUITY_REASON_RETUNE;
    }
    if ((block->flags & DMA_F_RESTART) != 0U) {
        return NEPTUNE_EDGE_DISCONTINUITY_REASON_PIPELINE_RESTART;
    }
    if ((block->flags & DMA_F_INTERFACE_FAULT) != 0U) {
        return NEPTUNE_EDGE_DISCONTINUITY_REASON_CLOCK_OR_INTERFACE_ERROR;
    }
    return NEPTUNE_EDGE_DISCONTINUITY_REASON_UNSPECIFIED;
}

static size_t append_discontinuity(uint8_t *output,
                                   const nds_dma_block *block,
                                   uint16_t service_reason,
                                   uint32_t lost_samples,
                                   uint64_t last_good_timestamp)
{
    memset(output, 0, sizeof(neptune_edge_data_discontinuity_v1));
    put16(output, NEPTUNE_EDGE_DATA_EXTENSION_DISCONTINUITY);
    put16(output + 2,
          sizeof(neptune_edge_data_discontinuity_v1) / 4U);
    put16(output + 8, discontinuity_reason(block, service_reason));
    put16(output + 10,
          (block->flags & (DMA_F_INTERFACE_FAULT | DMA_F_DMA_FAULT |
                           DMA_F_FIFO_FAULT)) != 0U
              ? NEPTUNE_EDGE_DISCONTINUITY_ACTION_INVALIDATE_RANGE
              : NEPTUNE_EDGE_DISCONTINUITY_ACTION_MARK_ONLY);
    put32(output + 12, lost_samples);
    put64(output + 16, last_good_timestamp);
    put64(output + 24, block->sample_timestamp);
    return sizeof(neptune_edge_data_discontinuity_v1);
}

static size_t append_block_stats(uint8_t *output, const nds_dma_block *block)
{
    memset(output, 0, sizeof(neptune_edge_data_block_stats_v1));
    put16(output, NEPTUNE_EDGE_DATA_EXTENSION_BLOCK_STATS);
    put16(output + 2, sizeof(neptune_edge_data_block_stats_v1) / 4U);
    put32(output + 8, block->block_rms_q16_16);
    put32(output + 12, block->block_peak_q16_16);
    put32(output + 16, block->clipping_count);
    put32(output + 20, block->sample_count * channel_count(block->channel_mask));
    put32(output + 24, 0U);
    return sizeof(neptune_edge_data_block_stats_v1);
}

static size_t append_payload_crc(uint8_t *output, const nds_dma_block *block)
{
    memset(output, 0, sizeof(neptune_edge_data_payload_crc_v1));
    put16(output, NEPTUNE_EDGE_DATA_EXTENSION_PAYLOAD_CRC);
    put16(output + 2, sizeof(neptune_edge_data_payload_crc_v1) / 4U);
    put32(output + 8,
          neptune_edge_crc32c(block->payload, block->payload_bytes));
    return sizeof(neptune_edge_data_payload_crc_v1);
}

static int prefix_has_space(size_t offset, size_t extension_bytes)
{
    return offset <= NDS_MAX_PREFIX_BYTES &&
                   extension_bytes <= NDS_MAX_PREFIX_BYTES - offset
               ? 0
               : -EOVERFLOW;
}

static int required_prefix_bytes(const nds_dma_block *block,
                                 const nds_rf_state *rf_state,
                                 bool discontinuity, size_t *required)
{
    size_t bytes = NEPTUNE_EDGE_DATA_HEADER_BYTES +
                   sizeof(neptune_edge_data_rf_state_v1) +
                   sizeof(neptune_edge_data_block_stats_v1) +
                   sizeof(neptune_edge_data_payload_crc_v1);
    if (block->sample_format == NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF) {
        bytes += sizeof(neptune_edge_data_quantization_v1);
    }
    if (rf_state->sample_rate_hz == NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ) {
        bytes += sizeof(neptune_edge_data_resampler_state_v1);
    }
    if (discontinuity) {
        bytes += sizeof(neptune_edge_data_discontinuity_v1);
    }
    if (bytes > NDS_MAX_PREFIX_BYTES) {
        return -EOVERFLOW;
    }
    *required = bytes;
    return 0;
}

int nds_build_packet(const nds_dma_block *block,
                     const nds_rf_snapshot *snapshot,
                     uint64_t sequence,
                     uint16_t service_discontinuity_reason,
                     uint32_t lost_samples,
                     uint64_t last_good_timestamp,
                     uint32_t service_discontinuity_revision,
                     nds_packet *packet)
{
    unsigned channels;
    unsigned stride;
    bool discontinuity;
    uint32_t data_flags = NEPTUNE_EDGE_DATA_FLAG_PAYLOAD_CRC_PRESENT |
                          NEPTUNE_EDGE_DATA_FLAG_METADATA_COMPLETE;
    uint32_t discontinuity_revision;
    uint16_t extension_count = 0;
    size_t offset = NEPTUNE_EDGE_DATA_HEADER_BYTES;
    size_t required_prefix;
    uint8_t crc_header[NDS_MAX_PREFIX_BYTES];
    const nds_rf_state *rf_state;
    if (block == NULL || snapshot == NULL || packet == NULL) {
        return -EINVAL;
    }
    rf_state = &snapshot->state;
    if (block->payload == NULL ||
        !rf_state->metadata_complete ||
        block->stream_id == 0U || block->channel_mask == 0U ||
        block->channel_mask > 3U || block->sample_count == 0U ||
        block->resampler_phase_numerator >=
            NEPTUNE_EDGE_RESAMPLER_INTERPOLATION ||
        (block->flags & ~DMA_F_KNOWN_MASK) != 0U ||
        (block->flags & DMA_F_VALID) == 0U ||
        validate_product_format(block->packet_type, block->sample_format) != 0 ||
        block->device_state_revision != snapshot->device_state_revision ||
        block->sample_timestamp < snapshot->activation_timestamp ||
        block->configuration_revision != rf_state->configuration_revision ||
        validate_product_rate(block->packet_type, rf_state->sample_rate_hz) != 0 ||
        (block->sample_format == NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF &&
         (block->quantization_exponent < -31 ||
          block->quantization_exponent > 31))) {
        return -EINVAL;
    }
    if (block->packet_type == NEPTUNE_EDGE_PACKET_TYPE_RAW_IQ &&
        block->calibration_revision != 0U) {
        return -EINVAL;
    }
    if (block->packet_type == NEPTUNE_EDGE_PACKET_TYPE_CALIBRATED_IQ &&
        block->calibration_revision == 0U) {
        return -EINVAL;
    }
    channels = channel_count(block->channel_mask);
    stride = bytes_per_channel_sample(block->sample_format) * channels;
    if ((uint64_t)block->sample_count * stride != block->payload_bytes ||
        block->payload_bytes > UINT32_MAX) {
        return -EMSGSIZE;
    }
    discontinuity = service_discontinuity_reason != 0U ||
                    (block->flags & DMA_F_DISCONTINUITY) != 0U;
    if (required_prefix_bytes(block, rf_state, discontinuity,
                              &required_prefix) != 0) {
        return -EOVERFLOW;
    }
    memset(packet, 0, sizeof(*packet));
    if (prefix_has_space(offset, sizeof(neptune_edge_data_rf_state_v1)) != 0) {
        return -EOVERFLOW;
    }
    offset += append_rf_state(packet->prefix + offset, rf_state,
                              block->channel_mask);
    ++extension_count;
    if (block->sample_format == NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF) {
        if (prefix_has_space(offset,
                             sizeof(neptune_edge_data_quantization_v1)) != 0) {
            return -EOVERFLOW;
        }
        offset += append_quantization(packet->prefix + offset, block);
        ++extension_count;
    }
    if (rf_state->sample_rate_hz == NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ) {
        if (prefix_has_space(offset,
                             sizeof(neptune_edge_data_resampler_state_v1)) != 0) {
            return -EOVERFLOW;
        }
        offset += append_resampler(packet->prefix + offset, block);
        ++extension_count;
    }
    discontinuity_revision = block->discontinuity_revision;
    if (service_discontinuity_reason != 0U &&
        service_discontinuity_revision > discontinuity_revision) {
        discontinuity_revision = service_discontinuity_revision;
    }
    if (discontinuity) {
        if (prefix_has_space(
                offset, sizeof(neptune_edge_data_discontinuity_v1)) != 0) {
            return -EOVERFLOW;
        }
        offset += append_discontinuity(packet->prefix + offset, block,
                                       service_discontinuity_reason, lost_samples,
                                       last_good_timestamp);
        ++extension_count;
        data_flags |= NEPTUNE_EDGE_DATA_FLAG_DISCONTINUITY;
    }
    if (prefix_has_space(offset,
                         sizeof(neptune_edge_data_block_stats_v1)) != 0) {
        return -EOVERFLOW;
    }
    offset += append_block_stats(packet->prefix + offset, block);
    ++extension_count;
    if (prefix_has_space(offset,
                         sizeof(neptune_edge_data_payload_crc_v1)) != 0) {
        return -EOVERFLOW;
    }
    offset += append_payload_crc(packet->prefix + offset, block);
    ++extension_count;
    if (offset != required_prefix || offset > sizeof(packet->prefix) ||
        offset % 4U != 0U) {
        return -EOVERFLOW;
    }
    if ((block->flags & (DMA_F_OVERFLOW | DMA_F_DMA_FAULT | DMA_F_FIFO_FAULT)) !=
        0U) {
        data_flags |= NEPTUNE_EDGE_DATA_FLAG_OVERFLOW;
    }
    if ((block->flags & DMA_F_RETUNE) != 0U) {
        data_flags |= NEPTUNE_EDGE_DATA_FLAG_RETUNE;
    }
    if ((block->flags & DMA_F_CLIPPED) != 0U || block->clipping_count != 0U) {
        data_flags |= NEPTUNE_EDGE_DATA_FLAG_CLIPPED;
    }
    if ((block->flags & (DMA_F_CONFIGURATION_CHANGE |
                         DMA_F_CALIBRATION_CHANGE |
                         DMA_F_DEVICE_STATE_CHANGE)) != 0U) {
        data_flags |= NEPTUNE_EDGE_DATA_FLAG_STATE_CHANGE;
    }
    memcpy(packet->prefix, "NEDP", 4);
    packet->prefix[4] = NEPTUNE_EDGE_PROTOCOL_VERSION;
    packet->prefix[5] = (uint8_t)(offset / 4U);
    packet->prefix[6] = (uint8_t)block->packet_type;
    packet->prefix[7] = (uint8_t)block->sample_format;
    put32(packet->prefix + 8, data_flags);
    put32(packet->prefix + 12, block->stream_id);
    put64(packet->prefix + 16, sequence);
    put64(packet->prefix + 24, block->sample_timestamp);
    put32(packet->prefix + 32, block->sample_count);
    put16(packet->prefix + 36, (uint16_t)block->channel_mask);
    put16(packet->prefix + 38, extension_count);
    put32(packet->prefix + 40, (uint32_t)block->payload_bytes);
    put32(packet->prefix + 44, block->configuration_revision);
    put32(packet->prefix + 48, block->calibration_revision);
    put32(packet->prefix + 52, discontinuity_revision);
    put32(packet->prefix + 56, block->device_state_revision);
    put32(packet->prefix + 60, 0U);
    memcpy(crc_header, packet->prefix, offset);
    memset(crc_header + 60, 0, 4);
    put32(packet->prefix + 60, neptune_edge_crc32c(crc_header, offset));
    packet->prefix_bytes = offset;
    packet->payload = block->payload;
    packet->payload_bytes = block->payload_bytes;
    packet->sequence = sequence;
    return 0;
}
