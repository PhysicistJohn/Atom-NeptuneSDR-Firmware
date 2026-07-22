#define _POSIX_C_SOURCE 200809L

#include "neptune_data_service.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define ARRAY_SIZE(value) (sizeof(value) / sizeof((value)[0]))
#define DMA_VALID 1U
#define DMA_DISCONTINUITY 2U
#define DMA_OVERFLOW 4U

static unsigned failures;

#define CHECK(expression)                                                        \
    do {                                                                         \
        if (!(expression)) {                                                     \
            (void)fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,       \
                          __LINE__, #expression);                                 \
            ++failures;                                                          \
            return;                                                              \
        }                                                                        \
    } while (0)

static uint16_t get16(const uint8_t *value)
{
    return (uint16_t)value[0] | (uint16_t)((uint16_t)value[1] << 8);
}

static uint32_t get32(const uint8_t *value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
           ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static uint64_t get64(const uint8_t *value)
{
    return (uint64_t)get32(value) | ((uint64_t)get32(value + 4) << 32);
}

static nds_rf_state base_rf_state(void)
{
    nds_rf_state state;
    memset(&state, 0, sizeof(state));
    state.center_frequency_hz = UINT64_C(915000000);
    state.sample_rate_hz = NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ;
    state.rf_bandwidth_hz = UINT32_C(20000000);
    state.rx1_gain_mdb = 30000;
    state.rx2_gain_mdb = 30000;
    state.digital_gain_q16_16 = 1 << 16;
    state.temperature_mc = 42000;
    state.pll_lock_mask = 1;
    state.configuration_revision = 2;
    state.metadata_complete = true;
    return state;
}

static nds_rf_snapshot base_snapshot(void)
{
    nds_rf_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.device_state_revision = 4U;
    snapshot.activation_timestamp = UINT64_C(100000);
    snapshot.state = base_rf_state();
    return snapshot;
}

static nds_stream_profile base_profile(void)
{
    nds_stream_profile profile;
    memset(&profile, 0, sizeof(profile));
    profile.stream_id = 1;
    profile.packet_type = NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ;
    profile.channel_mask = 1;
    profile.sample_format = NEPTUNE_EDGE_SAMPLE_FORMAT_S8;
    profile.samples_per_packet = 4096;
    profile.sample_rate_hz = NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ;
    profile.batch_size = 4;
    profile.destination.ipv4[0] = 192;
    profile.destination.ipv4[1] = 0;
    profile.destination.ipv4[2] = 2;
    profile.destination.ipv4[3] = 1;
    profile.destination.port = 50000;
    profile.destination.mtu = 9000;
    profile.initial_snapshot = base_snapshot();
    return profile;
}

static nds_dma_block base_block(const uint8_t *payload, size_t payload_bytes,
                                uint64_t source_sequence)
{
    nds_dma_block block;
    uint64_t total = source_sequence * UINT64_C(4096) *
                     NEPTUNE_EDGE_RESAMPLER_DECIMATION;
    memset(&block, 0, sizeof(block));
    block.flags = DMA_VALID;
    block.stream_id = 1;
    block.packet_type = NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ;
    block.channel_mask = 1;
    block.sample_format = NEPTUNE_EDGE_SAMPLE_FORMAT_S8;
    block.sample_count = 4096;
    block.block_rms_q16_16 = 100U << 16;
    block.block_peak_q16_16 = 120U << 16;
    block.configuration_revision = 2;
    block.calibration_revision = 3;
    block.device_state_revision = 4;
    block.source_sequence = source_sequence;
    block.sample_timestamp = UINT64_C(100000) +
                             total / NEPTUNE_EDGE_RESAMPLER_INTERPOLATION;
    block.output_sample_index = source_sequence * 4096U;
    block.resampler_phase_numerator =
        (uint32_t)(total % NEPTUNE_EDGE_RESAMPLER_INTERPOLATION);
    block.payload = payload;
    block.payload_bytes = payload_bytes;
    return block;
}

static void test_destination_validation(void)
{
    nds_destination destination;
    CHECK(nds_parse_destination(&destination, "192.0.2.1", 50000, 9000,
                                false) == 0);
    CHECK(destination.ipv4[0] == 192 && destination.port == 50000);
    CHECK(nds_parse_destination(&destination, "127.0.0.1", 50000, 9000,
                                false) == -EADDRNOTAVAIL);
    CHECK(nds_parse_destination(&destination, "127.0.0.1", 50000, 9000,
                                true) == 0);
    CHECK(nds_parse_destination(&destination, "239.1.2.3", 50000, 9000,
                                false) == -EADDRNOTAVAIL);
    CHECK(nds_parse_destination(&destination, "255.255.255.255", 50000, 9000,
                                false) == -EADDRNOTAVAIL);
    CHECK(nds_parse_destination(&destination, "192.0.2.1", 0, 9000,
                                false) == -EINVAL);
    CHECK(nds_parse_destination(&destination, "192.0.2.1", 50000, 2000,
                                false) == -EINVAL);
}

static void test_wire_rate_gate(void)
{
    nds_stream_profile profile = base_profile();
    uint64_t wire_rate = 0;
    uint32_t maximum = 0;
    CHECK(nds_validate_profile(&profile, &wire_rate, &maximum) == 0);
    CHECK(wire_rate < UINT64_C(1000000000));
    CHECK(maximum >= profile.samples_per_packet);
    profile.destination.mtu = 1500;
    profile.samples_per_packet = 600;
    CHECK(nds_validate_profile(&profile, &wire_rate, &maximum) == -ERANGE);
    profile = base_profile();
    profile.channel_mask = 3;
    profile.samples_per_packet = 2048;
    CHECK(nds_validate_profile(&profile, &wire_rate, &maximum) == -ERANGE);
    profile = base_profile();
    profile.initial_snapshot.state.metadata_complete = false;
    CHECK(nds_validate_profile(&profile, &wire_rate, &maximum) == -EINVAL);
    profile = base_profile();
    profile.initial_snapshot.state.digital_gain_q16_16 = 0;
    CHECK(nds_validate_profile(&profile, &wire_rate, &maximum) == -EINVAL);
    profile = base_profile();
    profile.initial_snapshot.state.temperature_mc = 125001;
    CHECK(nds_validate_profile(&profile, &wire_rate, &maximum) == -EINVAL);
    profile = base_profile();
    profile.initial_snapshot.state.pll_lock_mask = 4;
    CHECK(nds_validate_profile(&profile, &wire_rate, &maximum) == -EINVAL);
    profile = base_profile();
    memset(profile.destination.ipv4, 0, sizeof(profile.destination.ipv4));
    CHECK(nds_validate_profile(&profile, &wire_rate, &maximum) == -EINVAL);
    profile = base_profile();
    profile.destination.ipv4[0] = 239;
    CHECK(nds_validate_profile(&profile, &wire_rate, &maximum) == -EINVAL);
}

static int find_extension(const uint8_t *prefix, size_t prefix_length,
                          uint16_t type, const uint8_t **found)
{
    uint16_t count = get16(prefix + 38);
    size_t offset = NEPTUNE_EDGE_DATA_HEADER_BYTES;
    uint16_t index;
    for (index = 0; index < count; ++index) {
        uint16_t current;
        size_t length;
        if (prefix_length - offset < 8U) {
            return -1;
        }
        current = get16(prefix + offset);
        length = (size_t)get16(prefix + offset + 2) * 4U;
        if (length < 8U || length > prefix_length - offset) {
            return -1;
        }
        if (current == type) {
            *found = prefix + offset;
            return 1;
        }
        offset += length;
    }
    return 0;
}

static void check_header_crc(const uint8_t *prefix, size_t prefix_length)
{
    uint8_t copy[NDS_MAX_PREFIX_BYTES];
    CHECK(prefix_length <= sizeof(copy));
    memcpy(copy, prefix, prefix_length);
    memset(copy + 60, 0, 4);
    CHECK(neptune_edge_crc32c(copy, prefix_length) == get32(prefix + 60));
}

static void test_packet_contract(void)
{
    uint8_t payload[8192];
    nds_dma_block block;
    nds_packet packet;
    nds_rf_snapshot snapshot = base_snapshot();
    const uint8_t *extension;
    size_t index;
    for (index = 0; index < sizeof(payload); ++index) {
        payload[index] = (uint8_t)index;
    }
    block = base_block(payload, sizeof(payload), 7);
    CHECK(nds_build_packet(&block, &snapshot, 99, 0, 0, 0, 0, &packet) == 0);
    CHECK(memcmp(packet.prefix, "NEDP", 4) == 0);
    CHECK(packet.prefix[5] * 4U == packet.prefix_bytes);
    CHECK(packet.prefix[6] == NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ);
    CHECK(get64(packet.prefix + 16) == 99);
    CHECK(get64(packet.prefix + 24) == block.sample_timestamp);
    CHECK(get32(packet.prefix + 40) == sizeof(payload));
    CHECK(get16(packet.prefix + 38) == 4);
    check_header_crc(packet.prefix, packet.prefix_bytes);
    CHECK(get16(packet.prefix + NEPTUNE_EDGE_DATA_HEADER_BYTES) ==
          NEPTUNE_EDGE_DATA_EXTENSION_RF_STATE);
    CHECK(find_extension(packet.prefix, packet.prefix_bytes,
                         NEPTUNE_EDGE_DATA_EXTENSION_RF_STATE,
                         &extension) == 1);
    CHECK(get64(extension + 8) == snapshot.state.center_frequency_hz);
    CHECK(get32(extension + 16) == snapshot.state.sample_rate_hz);
    CHECK(get32(extension + 20) == snapshot.state.rf_bandwidth_hz);
    CHECK(get32(extension + 24) == (uint32_t)snapshot.state.rx1_gain_mdb);
    CHECK((get32(packet.prefix + 8) &
           NEPTUNE_EDGE_DATA_FLAG_METADATA_COMPLETE) != 0U);
    CHECK(find_extension(packet.prefix, packet.prefix_bytes,
                         NEPTUNE_EDGE_DATA_EXTENSION_RESAMPLER_STATE,
                         &extension) == 1);
    CHECK(get16(extension + 20) == block.resampler_phase_numerator);
    CHECK(get64(extension + 32) == block.output_sample_index);
    CHECK(find_extension(packet.prefix, packet.prefix_bytes,
                         NEPTUNE_EDGE_DATA_EXTENSION_PAYLOAD_CRC,
                         &extension) == 1);
    CHECK(get32(extension + 8) == neptune_edge_crc32c(payload, sizeof(payload)));
    block.flags |= DMA_DISCONTINUITY | DMA_OVERFLOW;
    block.discontinuity_revision = 8;
    CHECK(nds_build_packet(&block, &snapshot, 100,
                           NEPTUNE_EDGE_DISCONTINUITY_REASON_ETHERNET_BACKPRESSURE,
                           4096, 90000, 9, &packet) == 0);
    CHECK((get32(packet.prefix + 8) &
           NEPTUNE_EDGE_DATA_FLAG_DISCONTINUITY) != 0U);
    CHECK(get32(packet.prefix + 52) == 9);
    CHECK(find_extension(packet.prefix, packet.prefix_bytes,
                         NEPTUNE_EDGE_DATA_EXTENSION_DISCONTINUITY,
                         &extension) == 1);
    CHECK(get16(extension + 8) ==
          NEPTUNE_EDGE_DISCONTINUITY_REASON_ETHERNET_BACKPRESSURE);
    CHECK(get32(extension + 12) == 4096);

    block.flags = DMA_VALID;
    block.sample_format = NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF;
    block.quantization_exponent = 4;
    CHECK(nds_build_packet(&block, &snapshot, 101, 0, 0, 0, 0,
                           &packet) == 0);
    CHECK(get16(packet.prefix + NEPTUNE_EDGE_DATA_HEADER_BYTES) ==
          NEPTUNE_EDGE_DATA_EXTENSION_RF_STATE);
    CHECK(get16(packet.prefix + NEPTUNE_EDGE_DATA_HEADER_BYTES +
                sizeof(neptune_edge_data_rf_state_v1)) ==
          NEPTUNE_EDGE_DATA_EXTENSION_QUANTIZATION);
    CHECK(find_extension(packet.prefix, packet.prefix_bytes,
                         NEPTUNE_EDGE_DATA_EXTENSION_QUANTIZATION,
                         &extension) == 1);
    CHECK((int8_t)extension[8] == 4);
    CHECK(extension[9] == 1);
    CHECK(extension[10] == NEPTUNE_EDGE_QUANTIZATION_STRATEGY_PEAK);
    CHECK(extension[11] == NEPTUNE_EDGE_ROUNDING_MODE_ROUND_TO_NEAREST_EVEN);
    CHECK(get32(extension + 12) == 1);
    CHECK(get32(extension + 16) == 16);
    CHECK(find_extension(packet.prefix, packet.prefix_bytes,
                         NEPTUNE_EDGE_DATA_EXTENSION_BLOCK_STATS,
                         &extension) == 1);
    block.quantization_exponent = -3;
    CHECK(nds_build_packet(&block, &snapshot, 102, 0, 0, 0, 0,
                           &packet) == 0);
    CHECK(find_extension(packet.prefix, packet.prefix_bytes,
                         NEPTUNE_EDGE_DATA_EXTENSION_QUANTIZATION,
                         &extension) == 1);
    CHECK((int8_t)extension[8] == -3);
    CHECK(get32(extension + 12) == 8);
    CHECK(get32(extension + 16) == 1);
    block.flags = DMA_VALID | DMA_DISCONTINUITY;
    block.quantization_exponent = 0;
    block.discontinuity_revision = 10;
    CHECK(nds_build_packet(&block, &snapshot, 103, 0, 0, 0, 0,
                           &packet) == 0);
    CHECK(packet.prefix_bytes == NDS_MAX_PREFIX_BYTES);
    CHECK(get16(packet.prefix + 38) == 6);
    check_header_crc(packet.prefix, packet.prefix_bytes);
    block.flags = DMA_VALID;
    block.quantization_exponent = 32;
    CHECK(nds_build_packet(&block, &snapshot, 104, 0, 0, 0, 0,
                           &packet) == -EINVAL);
    block.quantization_exponent = -32;
    CHECK(nds_build_packet(&block, &snapshot, 105, 0, 0, 0, 0,
                           &packet) == -EINVAL);
    block.quantization_exponent = 0;
    snapshot.state.metadata_complete = false;
    CHECK(nds_build_packet(&block, &snapshot, 106, 0, 0, 0, 0,
                           &packet) == -EINVAL);
    snapshot.state.metadata_complete = true;
    snapshot.activation_timestamp = block.sample_timestamp + 1U;
    CHECK(nds_build_packet(&block, &snapshot, 107, 0, 0, 0, 0,
                           &packet) == -EINVAL);
    snapshot.activation_timestamp = block.sample_timestamp;
    ++snapshot.device_state_revision;
    CHECK(nds_build_packet(&block, &snapshot, 108, 0, 0, 0, 0,
                           &packet) == -EINVAL);
}

static void test_all_valid_extension_combinations(void)
{
    static const struct {
        uint32_t packet_type;
        uint32_t sample_format;
        uint32_t sample_rate_hz;
        uint32_t calibration_revision;
        bool discontinuity;
        size_t expected_prefix_bytes;
        uint16_t expected_extension_count;
    } cases[] = {
        {NEPTUNE_EDGE_PACKET_TYPE_RAW_IQ, NEPTUNE_EDGE_SAMPLE_FORMAT_S16,
         NEPTUNE_EDGE_INTERNAL_SAMPLE_RATE_HZ, 0U, false, 160U, 3U},
        {NEPTUNE_EDGE_PACKET_TYPE_RAW_IQ, NEPTUNE_EDGE_SAMPLE_FORMAT_S12P,
         NEPTUNE_EDGE_INTERNAL_SAMPLE_RATE_HZ, 0U, true, 192U, 4U},
        {NEPTUNE_EDGE_PACKET_TYPE_CALIBRATED_IQ,
         NEPTUNE_EDGE_SAMPLE_FORMAT_S16,
         NEPTUNE_EDGE_INTERNAL_SAMPLE_RATE_HZ, 1U, false, 160U, 3U},
        {NEPTUNE_EDGE_PACKET_TYPE_CALIBRATED_IQ,
         NEPTUNE_EDGE_SAMPLE_FORMAT_S16,
         NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ, 1U, true, 232U, 5U},
        {NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ,
         NEPTUNE_EDGE_SAMPLE_FORMAT_S8,
         NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ, 1U, false, 200U, 4U},
        {NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ,
         NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF,
         NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ, 1U, true,
         NDS_MAX_PREFIX_BYTES, 6U},
    };
    uint8_t payload[16] = {0};
    nds_dma_block block;
    nds_packet packet;
    nds_rf_snapshot snapshot;
    size_t index;
    for (index = 0; index < ARRAY_SIZE(cases); ++index) {
        unsigned bytes_per_sample;
        snapshot = base_snapshot();
        snapshot.state.sample_rate_hz = cases[index].sample_rate_hz;
        block = base_block(payload, sizeof(payload), 0);
        block.packet_type = cases[index].packet_type;
        block.sample_format = cases[index].sample_format;
        block.sample_count = 4U;
        block.calibration_revision = cases[index].calibration_revision;
        block.flags = DMA_VALID |
                      (cases[index].discontinuity ? DMA_DISCONTINUITY : 0U);
        block.discontinuity_revision = cases[index].discontinuity ? 1U : 0U;
        bytes_per_sample = cases[index].sample_format ==
                                   NEPTUNE_EDGE_SAMPLE_FORMAT_S16
                               ? 4U
                           : cases[index].sample_format ==
                                     NEPTUNE_EDGE_SAMPLE_FORMAT_S12P
                               ? 3U
                               : 2U;
        block.payload_bytes = block.sample_count * bytes_per_sample;
        CHECK(nds_build_packet(&block, &snapshot, index, 0, 0, 0, 0,
                               &packet) == 0);
        CHECK(packet.prefix_bytes == cases[index].expected_prefix_bytes);
        CHECK(get16(packet.prefix + 38) ==
              cases[index].expected_extension_count);
        check_header_crc(packet.prefix, packet.prefix_bytes);
    }
    snapshot = base_snapshot();
    snapshot.state.sample_rate_hz = NEPTUNE_EDGE_INTERNAL_SAMPLE_RATE_HZ;
    block = base_block(payload, 8U, 0);
    block.sample_format = NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF;
    block.sample_count = 4U;
    CHECK(nds_build_packet(&block, &snapshot, 0, 0, 0, 0, 0,
                           &packet) == -EINVAL);
}

static void test_malformed_block_fuzz(void)
{
    uint8_t payload[8192] = {0};
    nds_dma_block block = base_block(payload, sizeof(payload), 0);
    nds_packet packet;
    nds_rf_snapshot snapshot = base_snapshot();
    uint32_t value;
    block.flags |= UINT32_C(1) << 31;
    CHECK(nds_build_packet(&block, &snapshot, 0, 0, 0, 0, 0, &packet) == -EINVAL);
    block = base_block(payload, sizeof(payload), 0);
    block.resampler_phase_numerator = 1375;
    CHECK(nds_build_packet(&block, &snapshot, 0, 0, 0, 0, 0, &packet) == -EINVAL);
    block = base_block(payload, sizeof(payload), 0);
    block.payload_bytes--;
    CHECK(nds_build_packet(&block, &snapshot, 0, 0, 0, 0, 0, &packet) == -EMSGSIZE);
    for (value = 0; value < 16U; ++value) {
        block = base_block(payload, sizeof(payload), 0);
        block.packet_type = value;
        if (value != NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ) {
            CHECK(nds_build_packet(&block, &snapshot, 0, 0, 0, 0, 0,
                                   &packet) != 0);
        }
    }
}

static int receiver_open(uint16_t *port)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -errno;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        (void)close(fd);
        return -EINVAL;
    }
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        getsockname(fd, (struct sockaddr *)&address, &length) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        int result = -errno;
        (void)close(fd);
        return result;
    }
    *port = ntohs(address.sin_port);
    return fd;
}

static void test_mock_ring_generation(void)
{
    nds_dma_ring ring;
    nds_dma_block metadata;
    nds_dma_block acquired;
    uint8_t payload[8192] = {0};
    memset(&ring, 0, sizeof(ring));
    CHECK(nds_mock_ring_create(&ring) == 0);
    CHECK(ring.ops->configure(&ring, 1,
                              NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ, 1,
                              NEPTUNE_EDGE_SAMPLE_FORMAT_S8, 4096) == 0);
    CHECK(ring.ops->start(&ring) == 0);
    metadata = base_block(payload, sizeof(payload), 0);
    CHECK(nds_mock_ring_push(&ring, &metadata, payload, sizeof(payload)) == 0);
    CHECK(ring.ops->acquire(&ring, &acquired) == 0);
    ++acquired.generation;
    CHECK(ring.ops->release(&ring, &acquired) == -ESTALE);
    --acquired.generation;
    CHECK(ring.ops->release(&ring, &acquired) == 0);
    ring.ops->destroy(&ring);
}

static void test_batched_service_and_gap_propagation(void)
{
    nds_dma_ring ring;
    nds_stream_profile profile = base_profile();
    nds_service service;
    nds_dma_block block;
    uint8_t payload[8192];
    uint8_t datagram[10000];
    const uint8_t *extension;
    uint32_t expected_lost;
    uint16_t port;
    int receiver;
    ssize_t count;
    unsigned index;
    memset(&ring, 0, sizeof(ring));
    memset(payload, 0x5a, sizeof(payload));
    receiver = receiver_open(&port);
    CHECK(receiver >= 0);
    CHECK(nds_parse_destination(&profile.destination, "127.0.0.1", port, 9000,
                                true) == 0);
    CHECK(nds_mock_ring_create(&ring) == 0);
    CHECK(nds_service_init(&service, &ring, &profile) == 0);
    block = base_block(payload, sizeof(payload), 0);
    expected_lost = (uint32_t)(
        base_block(payload, sizeof(payload), 2).sample_timestamp -
        (block.sample_timestamp +
         (block.resampler_phase_numerator +
          (uint64_t)block.sample_count *
              NEPTUNE_EDGE_RESAMPLER_DECIMATION) /
             NEPTUNE_EDGE_RESAMPLER_INTERPOLATION));
    CHECK(nds_mock_ring_push(&ring, &block, payload, sizeof(payload)) == 0);
    block = base_block(payload, sizeof(payload), 2);
    CHECK(nds_mock_ring_push(&ring, &block, payload, sizeof(payload)) == 0);
    CHECK(nds_service_step(&service, 0) == 1);
    for (index = 0; index < 2U; ++index) {
        size_t prefix_length;
        count = recv(receiver, datagram, sizeof(datagram), 0);
        CHECK(count > 0);
        CHECK(memcmp(datagram, "NEDP", 4) == 0);
        prefix_length = (size_t)datagram[5] * 4U;
        CHECK(get64(datagram + 16) == index);
        check_header_crc(datagram, prefix_length);
        if (index == 1U) {
            CHECK((get32(datagram + 8) &
                   NEPTUNE_EDGE_DATA_FLAG_DISCONTINUITY) != 0U);
            CHECK(find_extension(datagram, prefix_length,
                                 NEPTUNE_EDGE_DATA_EXTENSION_DISCONTINUITY,
                                 &extension) == 1);
            CHECK(get16(extension + 8) ==
                  NEPTUNE_EDGE_DISCONTINUITY_REASON_DMA_OVERRUN);
            CHECK(get32(extension + 12) == expected_lost);
        }
    }
    CHECK(service.counters.datagrams_sent == 2);
    CHECK(service.counters.blocks_released == 2);
    CHECK(service.counters.source_sequence_gaps == 1);
    CHECK(service.counters.discontinuities_emitted == 1);
    nds_service_destroy(&service);
    ring.ops->destroy(&ring);
    (void)close(receiver);
}

static void test_kernel_accounted_gap_is_exactly_once(void)
{
    nds_dma_ring ring;
    nds_stream_profile profile = base_profile();
    nds_service service;
    nds_dma_block block;
    uint8_t payload[8192] = {0};
    uint8_t datagram[10000];
    const uint8_t *extension;
    uint16_t port;
    int receiver = receiver_open(&port);
    ssize_t count;
    unsigned index;
    memset(&ring, 0, sizeof(ring));
    CHECK(receiver >= 0);
    CHECK(nds_parse_destination(&profile.destination, "127.0.0.1", port, 9000,
                                true) == 0);
    CHECK(nds_mock_ring_create(&ring) == 0);
    CHECK(nds_service_init(&service, &ring, &profile) == 0);
    block = base_block(payload, sizeof(payload), 0);
    CHECK(nds_mock_ring_push(&ring, &block, payload, sizeof(payload)) == 0);
    /* This is the shape of a completion for which neptune_stream already
     * observed the sequence/timestamp gap and advanced the revision. */
    block = base_block(payload, sizeof(payload), 2);
    block.flags |= DMA_DISCONTINUITY;
    block.discontinuity_revision = 1U;
    CHECK(nds_mock_ring_push(&ring, &block, payload, sizeof(payload)) == 0);
    CHECK(nds_service_step(&service, 0) == 1);
    for (index = 0; index < 2U; ++index) {
        count = recv(receiver, datagram, sizeof(datagram), 0);
        CHECK(count > 0);
        if (index == 1U) {
            size_t prefix_length = (size_t)datagram[5] * 4U;
            CHECK((get32(datagram + 8) &
                   NEPTUNE_EDGE_DATA_FLAG_DISCONTINUITY) != 0U);
            CHECK(get32(datagram + 52) == 1U);
            CHECK(find_extension(datagram, prefix_length,
                                 NEPTUNE_EDGE_DATA_EXTENSION_DISCONTINUITY,
                                 &extension) == 1);
            CHECK(get32(extension + 12) == 4576U);
        }
    }
    CHECK(service.discontinuity_revision == 1U);
    CHECK(service.counters.source_sequence_gaps == 1U);
    CHECK(service.counters.timestamp_discontinuities == 1U);
    CHECK(service.counters.discontinuities_emitted == 1U);
    CHECK(service.counters.blocks_released == 2U);
    nds_service_destroy(&service);
    ring.ops->destroy(&ring);
    (void)close(receiver);
}

static void test_timestamped_rf_snapshot_transition(void)
{
    nds_dma_ring ring;
    nds_stream_profile profile = base_profile();
    nds_rf_snapshot transition = profile.initial_snapshot;
    nds_service service;
    nds_dma_block block;
    uint8_t payload[8192] = {0};
    uint8_t datagram[10000];
    const uint8_t *extension;
    uint16_t port;
    int receiver = receiver_open(&port);
    ssize_t count;
    unsigned index;
    memset(&ring, 0, sizeof(ring));
    CHECK(receiver >= 0);
    CHECK(nds_parse_destination(&profile.destination, "127.0.0.1", port, 9000,
                                true) == 0);
    CHECK(nds_mock_ring_create(&ring) == 0);
    CHECK(nds_service_init(&service, &ring, &profile) == 0);
    block = base_block(payload, sizeof(payload), 1);
    transition.device_state_revision = 5U;
    transition.activation_timestamp = block.sample_timestamp;
    transition.state.rx1_gain_mdb = 47000;
    transition.state.rx1_gain_mode = NEPTUNE_EDGE_GAIN_MODE_FAST_ATTACK;
    transition.state.temperature_mc = 51000;
    transition.state.pll_lock_mask = 0U;
    CHECK(nds_service_add_rf_snapshot(&service, &transition) == 0);
    CHECK(nds_service_add_rf_snapshot(&service, &transition) == -EINVAL);
    block = base_block(payload, sizeof(payload), 0);
    CHECK(nds_mock_ring_push(&ring, &block, payload, sizeof(payload)) == 0);
    block = base_block(payload, sizeof(payload), 1);
    block.device_state_revision = 5U;
    CHECK(nds_mock_ring_push(&ring, &block, payload, sizeof(payload)) == 0);
    CHECK(nds_service_step(&service, 0) == 1);
    for (index = 0; index < 2U; ++index) {
        size_t prefix_length;
        count = recv(receiver, datagram, sizeof(datagram), 0);
        CHECK(count > 0);
        prefix_length = (size_t)datagram[5] * 4U;
        CHECK(find_extension(datagram, prefix_length,
                             NEPTUNE_EDGE_DATA_EXTENSION_RF_STATE,
                             &extension) == 1);
        CHECK(get32(datagram + 56) == (index == 0U ? 4U : 5U));
        CHECK((int32_t)get32(extension + 24) ==
              (index == 0U ? 30000 : 47000));
        CHECK(extension[40] ==
              (index == 0U ? NEPTUNE_EDGE_GAIN_MODE_MANUAL
                           : NEPTUNE_EDGE_GAIN_MODE_FAST_ATTACK));
        CHECK((int32_t)get32(extension + 36) ==
              (index == 0U ? 42000 : 51000));
        CHECK(extension[42] == (index == 0U ? 1U : 0U));
    }
    /* Once revision 5 is active, revision 4 metadata is not reusable even
     * when the RF values happen to be unchanged. */
    block = base_block(payload, sizeof(payload), 2);
    CHECK(nds_mock_ring_push(&ring, &block, payload, sizeof(payload)) == 0);
    CHECK(nds_service_step(&service, 0) == -ESTALE);
    CHECK(service.counters.blocks_released == 3U);
    nds_service_destroy(&service);
    ring.ops->destroy(&ring);
    (void)close(receiver);
}

static void test_stale_rf_snapshot_stops_stream(void)
{
    nds_dma_ring ring;
    nds_stream_profile profile = base_profile();
    nds_service service;
    nds_dma_block block;
    uint8_t payload[8192] = {0};
    uint16_t port;
    int receiver = receiver_open(&port);
    memset(&ring, 0, sizeof(ring));
    CHECK(receiver >= 0);
    CHECK(nds_parse_destination(&profile.destination, "127.0.0.1", port, 9000,
                                true) == 0);
    CHECK(nds_mock_ring_create(&ring) == 0);
    CHECK(nds_service_init(&service, &ring, &profile) == 0);
    block = base_block(payload, sizeof(payload), 0);
    block.configuration_revision =
        profile.initial_snapshot.state.configuration_revision + 1U;
    CHECK(nds_mock_ring_push(&ring, &block, payload, sizeof(payload)) == 0);
    block = base_block(payload, sizeof(payload), 1);
    block.configuration_revision =
        profile.initial_snapshot.state.configuration_revision + 1U;
    CHECK(nds_mock_ring_push(&ring, &block, payload, sizeof(payload)) == 0);
    CHECK(nds_service_step(&service, 0) == -ESTALE);
    CHECK(service.counters.datagrams_sent == 0);
    CHECK(service.counters.blocks_acquired == 2);
    CHECK(service.counters.blocks_released == 2);
    nds_service_destroy(&service);
    ring.ops->destroy(&ring);
    (void)close(receiver);
}

static void test_cleanup_after_late_acquire_error(void)
{
    nds_dma_ring ring;
    nds_stream_profile profile = base_profile();
    nds_service service;
    nds_dma_block block;
    nds_dma_stats stats;
    uint8_t payload[8192] = {0};
    uint16_t port;
    int receiver = receiver_open(&port);
    memset(&ring, 0, sizeof(ring));
    memset(&stats, 0, sizeof(stats));
    CHECK(receiver >= 0);
    CHECK(nds_parse_destination(&profile.destination, "127.0.0.1", port, 9000,
                                true) == 0);
    CHECK(nds_mock_ring_create(&ring) == 0);
    CHECK(nds_service_init(&service, &ring, &profile) == 0);
    block = base_block(payload, sizeof(payload), 0);
    CHECK(nds_mock_ring_push(&ring, &block, payload, sizeof(payload)) == 0);
    block = base_block(payload, sizeof(payload), 1);
    CHECK(nds_mock_ring_push(&ring, &block, payload, sizeof(payload)) == 0);
    nds_mock_ring_fail_acquire_after(&ring, 1U, -EIO);
    CHECK(nds_service_step(&service, 0) == -EIO);
    CHECK(service.counters.blocks_acquired == 1);
    CHECK(service.counters.blocks_released == 1);
    CHECK(ring.ops->get_stats(&ring, &stats) == 0);
    CHECK(stats.acquired_blocks == 1);
    CHECK(stats.released_blocks == 1);
    nds_service_destroy(&service);
    ring.ops->destroy(&ring);
    (void)close(receiver);
}

static void test_cleanup_continues_after_release_error(void)
{
    nds_dma_ring ring;
    nds_stream_profile profile = base_profile();
    nds_service service;
    nds_dma_block block;
    nds_dma_stats stats;
    uint8_t payload[8192] = {0};
    uint16_t port;
    int receiver = receiver_open(&port);
    memset(&ring, 0, sizeof(ring));
    memset(&stats, 0, sizeof(stats));
    CHECK(receiver >= 0);
    CHECK(nds_parse_destination(&profile.destination, "127.0.0.1", port, 9000,
                                true) == 0);
    CHECK(nds_mock_ring_create(&ring) == 0);
    CHECK(nds_service_init(&service, &ring, &profile) == 0);
    block = base_block(payload, sizeof(payload), 0);
    CHECK(nds_mock_ring_push(&ring, &block, payload, sizeof(payload)) == 0);
    block = base_block(payload, sizeof(payload), 1);
    CHECK(nds_mock_ring_push(&ring, &block, payload, sizeof(payload)) == 0);
    nds_mock_ring_fail_next_release(&ring);
    CHECK(nds_service_step(&service, 0) == -EIO);
    CHECK(service.counters.blocks_acquired == 2);
    CHECK(service.counters.blocks_released == 1);
    CHECK(ring.ops->get_stats(&ring, &stats) == 0);
    CHECK(stats.acquired_blocks == 2);
    CHECK(stats.released_blocks == 1);
    nds_service_destroy(&service);
    ring.ops->destroy(&ring);
    (void)close(receiver);
}

static void test_discontinuity_revision_exhaustion(void)
{
    nds_dma_ring ring;
    nds_stream_profile profile = base_profile();
    nds_service service;
    nds_dma_block block;
    uint8_t payload[8192] = {0};
    uint16_t port;
    int receiver = receiver_open(&port);
    memset(&ring, 0, sizeof(ring));
    CHECK(receiver >= 0);
    CHECK(nds_parse_destination(&profile.destination, "127.0.0.1", port, 9000,
                                true) == 0);
    CHECK(nds_mock_ring_create(&ring) == 0);
    CHECK(nds_service_init(&service, &ring, &profile) == 0);
    service.discontinuity_revision = UINT32_MAX;
    service.have_source_sequence = true;
    service.last_source_sequence = 0U;
    block = base_block(payload, sizeof(payload), 2);
    CHECK(nds_mock_ring_push(&ring, &block, payload, sizeof(payload)) == 0);
    CHECK(nds_service_step(&service, 0) == -EOVERFLOW);
    CHECK(service.discontinuity_revision == UINT32_MAX);
    CHECK(service.counters.datagrams_sent == 0);
    CHECK(service.counters.blocks_released == 1);
    nds_service_destroy(&service);
    ring.ops->destroy(&ring);
    (void)close(receiver);
}

int main(void)
{
    void (*const tests[])(void) = {
        test_destination_validation,
        test_wire_rate_gate,
        test_packet_contract,
        test_all_valid_extension_combinations,
        test_malformed_block_fuzz,
        test_mock_ring_generation,
        test_batched_service_and_gap_propagation,
        test_kernel_accounted_gap_is_exactly_once,
        test_timestamped_rf_snapshot_transition,
        test_stale_rf_snapshot_stops_stream,
        test_cleanup_after_late_acquire_error,
        test_cleanup_continues_after_release_error,
        test_discontinuity_revision_exhaustion,
    };
    size_t index;
    for (index = 0; index < ARRAY_SIZE(tests); ++index) {
        tests[index]();
    }
    if (failures != 0U) {
        (void)fprintf(stderr, "%u data-service tests failed\n", failures);
        return 1;
    }
    (void)printf("data-service tests: %zu PASS\n", ARRAY_SIZE(tests));
    return 0;
}
