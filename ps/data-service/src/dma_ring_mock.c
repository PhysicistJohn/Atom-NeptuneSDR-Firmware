#include "neptune_data_service.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MOCK_SLOTS 32U

typedef enum mock_slot_state {
    MOCK_FREE,
    MOCK_READY,
    MOCK_USER,
} mock_slot_state;

typedef struct mock_slot {
    mock_slot_state state;
    uint32_t generation;
    uint64_t insertion;
    nds_dma_block metadata;
    uint8_t *payload;
} mock_slot;

typedef struct mock_ring_state {
    bool configured;
    bool started;
    bool fail_release;
    unsigned acquire_successes_before_failure;
    unsigned acquire_successes_since_arm;
    int acquire_failure;
    uint32_t stream_id;
    uint32_t packet_type;
    uint32_t channel_mask;
    uint32_t sample_format;
    uint32_t samples_per_slot;
    uint64_t insertion_counter;
    mock_slot slots[MOCK_SLOTS];
    nds_dma_stats stats;
} mock_ring_state;

static int mock_configure(nds_dma_ring *ring, uint32_t stream_id,
                          uint32_t packet_type, uint32_t channel_mask,
                          uint32_t sample_format, uint32_t samples_per_slot)
{
    mock_ring_state *state = (mock_ring_state *)ring->state;
    if (state->started) {
        return -EBUSY;
    }
    if (stream_id == 0U || packet_type < NEPTUNE_EDGE_PACKET_TYPE_RAW_IQ ||
        packet_type > NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ ||
        channel_mask == 0U || channel_mask > 3U ||
        sample_format < NEPTUNE_EDGE_SAMPLE_FORMAT_S16 ||
        sample_format > NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF ||
        samples_per_slot == 0U) {
        return -EINVAL;
    }
    state->stream_id = stream_id;
    state->packet_type = packet_type;
    state->channel_mask = channel_mask;
    state->sample_format = sample_format;
    state->samples_per_slot = samples_per_slot;
    state->configured = true;
    return 0;
}

static int mock_start(nds_dma_ring *ring)
{
    mock_ring_state *state = (mock_ring_state *)ring->state;
    if (!state->configured) {
        return -EINVAL;
    }
    state->started = true;
    return 0;
}

static int mock_stop(nds_dma_ring *ring)
{
    mock_ring_state *state = (mock_ring_state *)ring->state;
    state->started = false;
    return 0;
}

static int mock_wait(nds_dma_ring *ring, int timeout_ms)
{
    mock_ring_state *state = (mock_ring_state *)ring->state;
    unsigned index;
    (void)timeout_ms;
    if (!state->started) {
        return -EINVAL;
    }
    for (index = 0; index < MOCK_SLOTS; ++index) {
        if (state->slots[index].state == MOCK_READY) {
            return 1;
        }
    }
    return 0;
}

static int mock_acquire(nds_dma_ring *ring, nds_dma_block *block)
{
    mock_ring_state *state = (mock_ring_state *)ring->state;
    mock_slot *selected = NULL;
    unsigned index;
    if (!state->started) {
        return -EINVAL;
    }
    if (state->acquire_failure != 0 &&
        state->acquire_successes_since_arm >=
            state->acquire_successes_before_failure) {
        int result = state->acquire_failure;
        state->acquire_failure = 0;
        return result;
    }
    for (index = 0; index < MOCK_SLOTS; ++index) {
        mock_slot *slot = &state->slots[index];
        if (slot->state == MOCK_READY &&
            (selected == NULL || slot->insertion < selected->insertion)) {
            selected = slot;
        }
    }
    if (selected == NULL) {
        return -EAGAIN;
    }
    selected->state = MOCK_USER;
    *block = selected->metadata;
    block->slot_index = (uint32_t)(selected - state->slots);
    block->generation = selected->generation;
    block->payload = selected->payload;
    if (state->acquire_failure != 0) {
        ++state->acquire_successes_since_arm;
    }
    ++state->stats.acquired_blocks;
    return 0;
}

static int mock_release(nds_dma_ring *ring, const nds_dma_block *block)
{
    mock_ring_state *state = (mock_ring_state *)ring->state;
    mock_slot *slot;
    if (state->fail_release) {
        state->fail_release = false;
        return -EIO;
    }
    if (block->slot_index >= MOCK_SLOTS) {
        return -EINVAL;
    }
    slot = &state->slots[block->slot_index];
    if (slot->state != MOCK_USER || slot->generation != block->generation) {
        return -ESTALE;
    }
    free(slot->payload);
    slot->payload = NULL;
    memset(&slot->metadata, 0, sizeof(slot->metadata));
    slot->state = MOCK_FREE;
    ++state->stats.released_blocks;
    return 0;
}

static int mock_get_stats(nds_dma_ring *ring, nds_dma_stats *stats)
{
    mock_ring_state *state = (mock_ring_state *)ring->state;
    *stats = state->stats;
    return 0;
}

static void mock_destroy(nds_dma_ring *ring)
{
    mock_ring_state *state = (mock_ring_state *)ring->state;
    unsigned index;
    if (state == NULL) {
        return;
    }
    for (index = 0; index < MOCK_SLOTS; ++index) {
        free(state->slots[index].payload);
    }
    free(state);
    ring->state = NULL;
    ring->ops = NULL;
}

static const nds_dma_ops MOCK_OPS = {
    .configure = mock_configure,
    .start = mock_start,
    .stop = mock_stop,
    .wait = mock_wait,
    .acquire = mock_acquire,
    .release = mock_release,
    .get_stats = mock_get_stats,
    .destroy = mock_destroy,
};

int nds_mock_ring_create(nds_dma_ring *ring)
{
    mock_ring_state *state;
    if (ring == NULL) {
        return -EINVAL;
    }
    state = (mock_ring_state *)calloc(1, sizeof(*state));
    if (state == NULL) {
        return -ENOMEM;
    }
    ring->ops = &MOCK_OPS;
    ring->state = state;
    return 0;
}

int nds_mock_ring_push(nds_dma_ring *ring, const nds_dma_block *metadata,
                       const void *payload, size_t payload_bytes)
{
    mock_ring_state *state;
    unsigned index;
    mock_slot *slot = NULL;
    uint8_t *copy;
    if (ring == NULL || metadata == NULL || payload == NULL ||
        payload_bytes == 0U) {
        return -EINVAL;
    }
    state = (mock_ring_state *)ring->state;
    if (state == NULL || !state->configured ||
        metadata->stream_id != state->stream_id ||
        metadata->packet_type != state->packet_type ||
        metadata->channel_mask != state->channel_mask ||
        metadata->sample_format != state->sample_format ||
        metadata->sample_count != state->samples_per_slot ||
        metadata->payload_bytes != payload_bytes) {
        return -EINVAL;
    }
    for (index = 0; index < MOCK_SLOTS; ++index) {
        if (state->slots[index].state == MOCK_FREE) {
            slot = &state->slots[index];
            break;
        }
    }
    if (slot == NULL) {
        ++state->stats.dropped_blocks;
        ++state->stats.overrun_events;
        return -ENOSPC;
    }
    copy = (uint8_t *)malloc(payload_bytes);
    if (copy == NULL) {
        return -ENOMEM;
    }
    memcpy(copy, payload, payload_bytes);
    ++slot->generation;
    if (slot->generation == 0U) {
        ++slot->generation;
    }
    slot->metadata = *metadata;
    slot->metadata.payload = NULL;
    slot->payload = copy;
    slot->insertion = ++state->insertion_counter;
    slot->state = MOCK_READY;
    ++state->stats.produced_blocks;
    return 0;
}

void nds_mock_ring_fail_next_release(nds_dma_ring *ring)
{
    mock_ring_state *state = ring != NULL ? (mock_ring_state *)ring->state : NULL;
    if (state != NULL) {
        state->fail_release = true;
    }
}

void nds_mock_ring_fail_acquire_after(nds_dma_ring *ring,
                                      unsigned successful_acquires,
                                      int error_code)
{
    mock_ring_state *state = ring != NULL ? (mock_ring_state *)ring->state : NULL;
    if (state != NULL && error_code < 0) {
        state->acquire_successes_before_failure = successful_acquires;
        state->acquire_successes_since_arm = 0U;
        state->acquire_failure = error_code;
    }
}
