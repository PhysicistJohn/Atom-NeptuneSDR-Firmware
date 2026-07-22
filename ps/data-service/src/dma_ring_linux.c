#define _POSIX_C_SOURCE 200809L

#include "neptune_data_service.h"

#include <errno.h>
#include <stdlib.h>

#if defined(__linux__)

#include "neptune_stream_uapi.h"

#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

typedef struct linux_ring_state {
    int fd;
    uint8_t *mapping;
    size_t mapping_bytes;
    struct neptune_stream_abi_info abi;
    struct neptune_stream_config config;
    bool configured;
    bool started;
} linux_ring_state;

_Static_assert(sizeof(struct neptune_stream_abi_info) ==
                   NEPTUNE_STREAM_ABI_INFO_SIZE,
               "stream ABI info size");
_Static_assert(sizeof(struct neptune_stream_config) == NEPTUNE_STREAM_CONFIG_SIZE,
               "stream config size");
_Static_assert(sizeof(struct neptune_stream_completion) ==
                   NEPTUNE_STREAM_COMPLETION_SIZE,
               "stream completion size");
_Static_assert(sizeof(struct neptune_stream_release) ==
                   NEPTUNE_STREAM_RELEASE_SIZE,
               "stream release size");

static int linux_configure(nds_dma_ring *ring, uint32_t stream_id,
                           uint32_t packet_type, uint32_t channel_mask,
                           uint32_t sample_format,
                           uint32_t samples_per_slot)
{
    linux_ring_state *state = (linux_ring_state *)ring->state;
    struct neptune_stream_config config;
    if (state->started) {
        return -EBUSY;
    }
    if ((state->abi.supported_sample_formats & (1U << sample_format)) == 0U ||
        (state->abi.supported_packet_types & (1U << packet_type)) == 0U) {
        return -ENOTSUP;
    }
    memset(&config, 0, sizeof(config));
    config.struct_size = sizeof(config);
    config.stream_id = stream_id;
    config.packet_type = packet_type;
    config.channel_mask = channel_mask;
    config.sample_format = sample_format;
    config.samples_per_slot = samples_per_slot;
    if (ioctl(state->fd, NEPTUNE_STREAM_IOC_CONFIGURE, &config) != 0) {
        return -errno;
    }
    state->config = config;
    state->configured = true;
    return 0;
}

static int linux_start(nds_dma_ring *ring)
{
    linux_ring_state *state = (linux_ring_state *)ring->state;
    if (!state->configured) {
        return -EINVAL;
    }
    if (state->started) {
        return -EALREADY;
    }
    if (ioctl(state->fd, NEPTUNE_STREAM_IOC_START) != 0) {
        return -errno;
    }
    state->started = true;
    return 0;
}

static int linux_stop(nds_dma_ring *ring)
{
    linux_ring_state *state = (linux_ring_state *)ring->state;
    if (!state->started) {
        return 0;
    }
    if (ioctl(state->fd, NEPTUNE_STREAM_IOC_STOP) != 0) {
        return -errno;
    }
    state->started = false;
    return 0;
}

static int linux_wait(nds_dma_ring *ring, int timeout_ms)
{
    linux_ring_state *state = (linux_ring_state *)ring->state;
    struct pollfd descriptor;
    int result;
    if (timeout_ms < -1) {
        return -EINVAL;
    }
    descriptor.fd = state->fd;
    descriptor.events = POLLIN | POLLERR;
    descriptor.revents = 0;
    do {
        result = poll(&descriptor, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        return -errno;
    }
    if (result == 0) {
        return 0;
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return -EIO;
    }
    return (descriptor.revents & POLLIN) != 0 ? 1 : 0;
}

static int linux_release_token(linux_ring_state *state, uint32_t slot_index,
                               uint32_t generation)
{
    struct neptune_stream_release release;
    memset(&release, 0, sizeof(release));
    release.struct_size = sizeof(release);
    release.slot_index = slot_index;
    release.generation = generation;
    return ioctl(state->fd, NEPTUNE_STREAM_IOC_RELEASE, &release) == 0
               ? 0
               : -errno;
}

static int linux_acquire_validation_error(linux_ring_state *state,
                                          const struct neptune_stream_completion *completion,
                                          int validation_error)
{
    int release_result = linux_release_token(
        state, completion->slot_index, completion->generation);
    return release_result != 0 ? release_result : validation_error;
}

static int linux_acquire(nds_dma_ring *ring, nds_dma_block *block)
{
    linux_ring_state *state = (linux_ring_state *)ring->state;
    struct neptune_stream_completion completion;
    size_t slot_offset;
    size_t payload_offset;
    memset(&completion, 0, sizeof(completion));
    completion.struct_size = sizeof(completion);
    if (ioctl(state->fd, NEPTUNE_STREAM_IOC_ACQUIRE, &completion) != 0) {
        return -errno;
    }
    if (completion.struct_size != sizeof(completion) ||
        completion.slot_index >= state->abi.slot_count ||
        completion.payload_offset < state->abi.slot_header_bytes ||
        completion.payload_bytes > state->abi.payload_capacity ||
        completion.resampler_phase_numerator >
            NEPTUNE_STREAM_RESAMPLER_PHASE_MAX) {
        return linux_acquire_validation_error(state, &completion, -EPROTO);
    }
    slot_offset = (size_t)completion.slot_index * state->abi.slot_bytes;
    payload_offset = slot_offset + completion.payload_offset;
    if (slot_offset / state->abi.slot_bytes != completion.slot_index ||
        payload_offset < slot_offset ||
        payload_offset > state->mapping_bytes ||
        completion.payload_bytes > state->mapping_bytes - payload_offset) {
        return linux_acquire_validation_error(state, &completion, -EOVERFLOW);
    }
    memset(block, 0, sizeof(*block));
    block->slot_index = completion.slot_index;
    block->generation = completion.generation;
    block->flags = completion.flags;
    block->stream_id = completion.stream_id;
    block->packet_type = completion.packet_type;
    block->channel_mask = completion.channel_mask;
    block->sample_format = completion.sample_format;
    block->sample_count = completion.sample_count;
    block->quantization_exponent = completion.quantization_exponent;
    block->block_rms_q16_16 = completion.block_rms_q16;
    block->block_peak_q16_16 = completion.block_peak_q16;
    block->clipping_count = completion.clipping_count;
    block->configuration_revision = completion.configuration_revision;
    block->calibration_revision = completion.calibration_revision;
    block->device_state_revision = completion.device_state_revision;
    block->discontinuity_revision = completion.discontinuity_revision;
    block->resampler_phase_numerator =
        completion.resampler_phase_numerator;
    block->source_sequence = completion.source_sequence;
    block->sample_timestamp = completion.sample_timestamp;
    block->output_sample_index = completion.output_sample_index;
    block->payload = state->mapping + payload_offset;
    block->payload_bytes = completion.payload_bytes;
    return 0;
}

static int linux_release(nds_dma_ring *ring, const nds_dma_block *block)
{
    linux_ring_state *state = (linux_ring_state *)ring->state;
    return linux_release_token(state, block->slot_index, block->generation);
}

static int linux_get_stats(nds_dma_ring *ring, nds_dma_stats *stats)
{
    linux_ring_state *state = (linux_ring_state *)ring->state;
    struct neptune_stream_stats kernel_stats;
    memset(&kernel_stats, 0, sizeof(kernel_stats));
    kernel_stats.struct_size = sizeof(kernel_stats);
    if (ioctl(state->fd, NEPTUNE_STREAM_IOC_GET_STATS, &kernel_stats) != 0) {
        return -errno;
    }
    memset(stats, 0, sizeof(*stats));
    stats->produced_blocks = kernel_stats.produced_blocks;
    stats->acquired_blocks = kernel_stats.acquired_blocks;
    stats->released_blocks = kernel_stats.released_blocks;
    stats->dropped_blocks = kernel_stats.dropped_blocks;
    stats->overrun_events = kernel_stats.overrun_events;
    stats->fifo_errors = kernel_stats.fifo_errors;
    stats->dma_errors = kernel_stats.dma_errors;
    stats->interface_errors = kernel_stats.interface_errors;
    stats->malformed_blocks = kernel_stats.malformed_blocks;
    stats->discontinuities = kernel_stats.discontinuities;
    return 0;
}

static void linux_destroy(nds_dma_ring *ring)
{
    linux_ring_state *state = (linux_ring_state *)ring->state;
    if (state == NULL) {
        return;
    }
    (void)linux_stop(ring);
    if (state->mapping != NULL) {
        (void)munmap(state->mapping, state->mapping_bytes);
    }
    if (state->fd >= 0) {
        (void)close(state->fd);
    }
    free(state);
    ring->state = NULL;
    ring->ops = NULL;
}

static const nds_dma_ops LINUX_OPS = {
    .configure = linux_configure,
    .start = linux_start,
    .stop = linux_stop,
    .wait = linux_wait,
    .acquire = linux_acquire,
    .release = linux_release,
    .get_stats = linux_get_stats,
    .destroy = linux_destroy,
};

int nds_linux_ring_open(nds_dma_ring *ring, const char *device_path)
{
    const uint32_t required_features =
        NEPTUNE_STREAM_FEAT_READ_ONLY_MMAP | NEPTUNE_STREAM_FEAT_POLL |
        NEPTUNE_STREAM_FEAT_DMA_COHERENT_RING |
        NEPTUNE_STREAM_FEAT_EXCLUSIVE_OPEN |
        NEPTUNE_STREAM_FEAT_STRICT_GENERATION |
        NEPTUNE_STREAM_FEAT_ZERO_COPY_RX;
    linux_ring_state *state;
    struct stat metadata;
    void *mapping;
    if (ring == NULL || device_path == NULL || device_path[0] == '\0') {
        return -EINVAL;
    }
    state = (linux_ring_state *)calloc(1, sizeof(*state));
    if (state == NULL) {
        return -ENOMEM;
    }
    state->fd = open(device_path,
                     O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (state->fd < 0) {
        int result = -errno;
        free(state);
        return result;
    }
    if (fstat(state->fd, &metadata) != 0 || !S_ISCHR(metadata.st_mode)) {
        (void)close(state->fd);
        free(state);
        return -ENODEV;
    }
    memset(&state->abi, 0, sizeof(state->abi));
    if (ioctl(state->fd, NEPTUNE_STREAM_IOC_GET_ABI, &state->abi) != 0) {
        int result = -errno;
        (void)close(state->fd);
        free(state);
        return result;
    }
    if (state->abi.struct_size != sizeof(state->abi) ||
        state->abi.abi_major != NEPTUNE_STREAM_ABI_MAJOR ||
        state->abi.slot_count == 0U || state->abi.slot_bytes == 0U ||
        state->abi.slot_header_bytes != NEPTUNE_STREAM_BLOCK_HEADER_SIZE ||
        state->abi.slot_bytes < state->abi.slot_header_bytes ||
        state->abi.payload_capacity >
            state->abi.slot_bytes - state->abi.slot_header_bytes ||
        state->abi.mmap_offset != NEPTUNE_STREAM_MMAP_OFFSET ||
        state->abi.mmap_length == 0U ||
        state->abi.mmap_length > SIZE_MAX ||
        state->abi.slot_count > SIZE_MAX / state->abi.slot_bytes ||
        state->abi.mmap_length !=
            (uint64_t)state->abi.slot_count * state->abi.slot_bytes ||
        (state->abi.feature_flags & required_features) != required_features) {
        (void)close(state->fd);
        free(state);
        return -EPROTONOSUPPORT;
    }
    state->mapping_bytes = (size_t)state->abi.mmap_length;
    mapping = mmap(NULL, state->mapping_bytes, PROT_READ, MAP_SHARED, state->fd,
                   (off_t)state->abi.mmap_offset);
    if (mapping == MAP_FAILED) {
        int result = -errno;
        (void)close(state->fd);
        free(state);
        return result;
    }
    state->mapping = (uint8_t *)mapping;
    ring->ops = &LINUX_OPS;
    ring->state = state;
    return 0;
}

#else

int nds_linux_ring_open(nds_dma_ring *ring, const char *device_path)
{
    (void)ring;
    (void)device_path;
    return -ENOTSUP;
}

#endif
