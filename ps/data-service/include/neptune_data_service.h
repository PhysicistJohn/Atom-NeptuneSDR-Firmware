#ifndef NEPTUNE_DATA_SERVICE_H
#define NEPTUNE_DATA_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "neptune_edge_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NDS_MAX_BATCH 32U
#define NDS_MAX_RF_SNAPSHOTS 16U
/* Canonical v1 maximum: base + RF + quantization + resampler +
 * discontinuity + block statistics + payload CRC. */
#define NDS_MAX_PREFIX_BYTES 272U

/* DMA metadata is deliberately isomorphic to the kernel UAPI but does not
 * expose Linux ioctl types to packetization/tests. */
typedef struct nds_dma_block {
    uint32_t slot_index;
    uint32_t generation;
    uint32_t flags;
    uint32_t stream_id;
    uint32_t packet_type;
    uint32_t channel_mask;
    uint32_t sample_format;
    uint32_t sample_count;
    int32_t quantization_exponent;
    uint32_t block_rms_q16_16;
    uint32_t block_peak_q16_16;
    uint32_t clipping_count;
    uint32_t configuration_revision;
    uint32_t calibration_revision;
    uint32_t device_state_revision;
    uint32_t discontinuity_revision;
    uint32_t resampler_phase_numerator;
    uint64_t source_sequence;
    uint64_t sample_timestamp;
    uint64_t output_sample_index;
    const uint8_t *payload;
    size_t payload_bytes;
} nds_dma_block;

typedef struct nds_dma_stats {
    uint64_t produced_blocks;
    uint64_t acquired_blocks;
    uint64_t released_blocks;
    uint64_t dropped_blocks;
    uint64_t overrun_events;
    uint64_t fifo_errors;
    uint64_t dma_errors;
    uint64_t interface_errors;
    uint64_t malformed_blocks;
    uint64_t discontinuities;
} nds_dma_stats;

typedef struct nds_dma_ring nds_dma_ring;

typedef struct nds_dma_ops {
    int (*configure)(nds_dma_ring *ring, uint32_t stream_id,
                     uint32_t packet_type, uint32_t channel_mask,
                     uint32_t sample_format,
                     uint32_t samples_per_slot);
    int (*start)(nds_dma_ring *ring);
    int (*stop)(nds_dma_ring *ring);
    int (*wait)(nds_dma_ring *ring, int timeout_ms);
    int (*acquire)(nds_dma_ring *ring, nds_dma_block *block);
    int (*release)(nds_dma_ring *ring, const nds_dma_block *block);
    int (*get_stats)(nds_dma_ring *ring, nds_dma_stats *stats);
    void (*destroy)(nds_dma_ring *ring);
} nds_dma_ops;

struct nds_dma_ring {
    const nds_dma_ops *ops;
    void *state;
};

typedef struct nds_packet {
    uint8_t prefix[NDS_MAX_PREFIX_BYTES];
    size_t prefix_bytes;
    const uint8_t *payload;
    size_t payload_bytes;
    uint64_t sequence;
} nds_packet;

typedef struct nds_destination {
    uint8_t ipv4[4];
    uint16_t port;
    uint16_t mtu;
    bool allow_loopback;
} nds_destination;

typedef struct nds_rf_state {
    uint64_t center_frequency_hz;
    uint32_t sample_rate_hz;
    uint32_t rf_bandwidth_hz;
    int32_t rx1_gain_mdb;
    int32_t rx2_gain_mdb;
    int32_t digital_gain_q16_16;
    int32_t temperature_mc;
    uint8_t rx1_gain_mode;
    uint8_t rx2_gain_mode;
    uint8_t pll_lock_mask;
    uint32_t device_flags;
    uint32_t configuration_revision;
    /* Internal assertion that every RF_STATE field was sourced from an
     * explicit, temporally aligned snapshot. This is not serialized. */
    bool metadata_complete;
} nds_rf_state;

typedef struct nds_rf_snapshot {
    uint32_t device_state_revision;
    uint64_t activation_timestamp;
    nds_rf_state state;
} nds_rf_snapshot;

typedef struct nds_stream_profile {
    uint32_t stream_id;
    uint32_t packet_type;
    uint32_t channel_mask;
    uint32_t sample_format;
    uint32_t samples_per_packet;
    uint32_t sample_rate_hz;
    nds_destination destination;
    nds_rf_snapshot initial_snapshot;
    unsigned batch_size;
} nds_stream_profile;

typedef struct nds_counters {
    uint64_t blocks_acquired;
    uint64_t blocks_released;
    uint64_t datagrams_attempted;
    uint64_t datagrams_sent;
    uint64_t payload_bytes_sent;
    uint64_t send_errors;
    uint64_t malformed_blocks;
    uint64_t discontinuities_emitted;
    uint64_t source_sequence_gaps;
    uint64_t timestamp_discontinuities;
} nds_counters;

typedef struct nds_udp_sink {
    int fd;
    uint8_t address_storage[32];
    size_t address_length;
} nds_udp_sink;

typedef struct nds_service {
    nds_dma_ring *ring;
    nds_udp_sink sink;
    nds_stream_profile profile;
    nds_rf_snapshot snapshots[NDS_MAX_RF_SNAPSHOTS];
    size_t snapshot_count;
    nds_counters counters;
    uint64_t next_sequence;
    uint64_t last_source_sequence;
    uint64_t last_good_timestamp;
    uint64_t expected_input_timestamp;
    uint64_t expected_output_sample_index;
    uint32_t discontinuity_revision;
    uint32_t expected_resampler_phase;
    bool have_source_sequence;
    bool have_timing_state;
    uint16_t pending_discontinuity_reason;
    uint32_t pending_lost_samples;
    uint32_t pending_discontinuity_revision;
} nds_service;

int nds_validate_profile(const nds_stream_profile *profile,
                         uint64_t *wire_bits_per_second,
                         uint32_t *maximum_samples_per_packet);
int nds_parse_destination(nds_destination *destination, const char *ipv4,
                          uint16_t port, uint16_t mtu, bool allow_loopback);

int nds_build_packet(const nds_dma_block *block,
                     const nds_rf_snapshot *snapshot,
                     uint64_t sequence,
                     uint16_t service_discontinuity_reason,
                     uint32_t lost_samples,
                     uint64_t last_good_timestamp,
                     uint32_t service_discontinuity_revision,
                     nds_packet *packet);

int nds_udp_open(nds_udp_sink *sink, const nds_destination *destination);
void nds_udp_close(nds_udp_sink *sink);
int nds_udp_send_batch(nds_udp_sink *sink, const nds_packet *packets,
                       size_t packet_count, size_t *sent_count);

int nds_service_init(nds_service *service, nds_dma_ring *ring,
                     const nds_stream_profile *profile);
int nds_service_add_rf_snapshot(nds_service *service,
                                const nds_rf_snapshot *snapshot);
int nds_service_step(nds_service *service, int timeout_ms);
void nds_service_destroy(nds_service *service);

int nds_linux_ring_open(nds_dma_ring *ring, const char *device_path);

int nds_mock_ring_create(nds_dma_ring *ring);
int nds_mock_ring_push(nds_dma_ring *ring, const nds_dma_block *metadata,
                       const void *payload, size_t payload_bytes);
void nds_mock_ring_fail_next_release(nds_dma_ring *ring);
void nds_mock_ring_fail_acquire_after(nds_dma_ring *ring,
                                      unsigned successful_acquires,
                                      int error_code);

#ifdef __cplusplus
}
#endif

#endif
