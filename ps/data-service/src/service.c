#include "neptune_data_service.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static uint32_t saturating_add_u32(uint32_t left, uint64_t right)
{
    return right > UINT32_MAX - left ? UINT32_MAX : left + (uint32_t)right;
}

static uint64_t last_source_tick(const nds_service *service,
                                 const nds_dma_block *block)
{
    uint64_t numerator;
    if (block->sample_count == 0U) {
        return block->sample_timestamp;
    }
    if (service->profile.sample_rate_hz !=
        NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ) {
        return block->sample_timestamp + block->sample_count - 1U;
    }
    numerator = block->resampler_phase_numerator +
                ((uint64_t)block->sample_count - 1U) *
                    NEPTUNE_EDGE_RESAMPLER_DECIMATION;
    return block->sample_timestamp +
           numerator / NEPTUNE_EDGE_RESAMPLER_INTERPOLATION;
}

static uint64_t source_span_ticks(const nds_service *service,
                                  const nds_dma_block *block)
{
    if (service->profile.sample_rate_hz ==
        NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ) {
        uint64_t total = block->resampler_phase_numerator +
                         (uint64_t)block->sample_count *
                             NEPTUNE_EDGE_RESAMPLER_DECIMATION;
        return total / NEPTUNE_EDGE_RESAMPLER_INTERPOLATION;
    }
    return block->sample_count;
}

static uint32_t lost_input_ticks_before(const nds_service *service,
                                        const nds_dma_block *block)
{
    uint64_t lost;
    if (!service->have_timing_state ||
        block->sample_timestamp < service->expected_input_timestamp) {
        return UINT32_MAX;
    }
    if (block->sample_timestamp == service->expected_input_timestamp &&
        (block->output_sample_index !=
             service->expected_output_sample_index ||
         (service->profile.sample_rate_hz ==
              NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ &&
          block->resampler_phase_numerator !=
              service->expected_resampler_phase))) {
        return UINT32_MAX;
    }
    lost = block->sample_timestamp - service->expected_input_timestamp;
    return lost > UINT32_MAX ? UINT32_MAX : (uint32_t)lost;
}

static bool timing_matches(const nds_service *service,
                           const nds_dma_block *block)
{
    if (!service->have_timing_state) {
        return true;
    }
    if (block->sample_timestamp != service->expected_input_timestamp ||
        block->output_sample_index != service->expected_output_sample_index) {
        return false;
    }
    return service->profile.sample_rate_hz !=
               NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ ||
           block->resampler_phase_numerator ==
               service->expected_resampler_phase;
}

static void advance_timing(nds_service *service, const nds_dma_block *block)
{
    if (service->profile.sample_rate_hz ==
        NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ) {
        uint64_t total = block->resampler_phase_numerator +
                         (uint64_t)block->sample_count *
                             NEPTUNE_EDGE_RESAMPLER_DECIMATION;
        service->expected_input_timestamp =
            block->sample_timestamp +
            total / NEPTUNE_EDGE_RESAMPLER_INTERPOLATION;
        service->expected_resampler_phase =
            (uint32_t)(total % NEPTUNE_EDGE_RESAMPLER_INTERPOLATION);
    } else {
        service->expected_input_timestamp =
            block->sample_timestamp + block->sample_count;
        service->expected_resampler_phase = 0U;
    }
    service->expected_output_sample_index =
        block->output_sample_index + block->sample_count;
    service->have_timing_state = true;
}

static int schedule_pending(nds_service *service, uint16_t reason,
                            uint64_t lost_samples)
{
    if (service->pending_discontinuity_reason == 0U) {
        if (service->discontinuity_revision == UINT32_MAX) {
            return -EOVERFLOW;
        }
        service->pending_discontinuity_reason = reason;
        ++service->discontinuity_revision;
        service->pending_discontinuity_revision =
            service->discontinuity_revision;
    }
    service->pending_lost_samples =
        saturating_add_u32(service->pending_lost_samples, lost_samples);
    return 0;
}

static int block_matches_profile(const nds_service *service,
                                 const nds_dma_block *block)
{
    return block->stream_id == service->profile.stream_id &&
                   block->packet_type == service->profile.packet_type &&
                   block->channel_mask == service->profile.channel_mask &&
                   block->sample_format == service->profile.sample_format &&
                   block->sample_count == service->profile.samples_per_packet
               ? 0
               : -EPROTO;
}

static const nds_rf_snapshot *resolve_snapshot(const nds_service *service,
                                               const nds_dma_block *block)
{
    const nds_rf_snapshot *active = NULL;
    size_t index;
    for (index = 0; index < service->snapshot_count; ++index) {
        const nds_rf_snapshot *candidate = &service->snapshots[index];
        if (candidate->activation_timestamp > block->sample_timestamp) {
            break;
        }
        active = candidate;
    }
    return active != NULL &&
                   active->device_state_revision == block->device_state_revision
               ? active
               : NULL;
}

static int release_acquired(nds_service *service, nds_dma_block *blocks,
                            bool *released, size_t block_count)
{
    int first_error = 0;
    size_t index;
    for (index = 0; index < block_count; ++index) {
        int result;
        if (released[index]) {
            continue;
        }
        result = service->ring->ops->release(service->ring, &blocks[index]);
        if (result == 0) {
            released[index] = true;
            ++service->counters.blocks_released;
        } else if (first_error == 0) {
            first_error = result;
        }
    }
    return first_error;
}

int nds_service_init(nds_service *service, nds_dma_ring *ring,
                     const nds_stream_profile *profile)
{
    uint64_t wire_rate;
    int result;
    if (service == NULL || ring == NULL || ring->ops == NULL ||
        profile == NULL) {
        return -EINVAL;
    }
    result = nds_validate_profile(profile, &wire_rate, NULL);
    if (result != 0) {
        return result;
    }
    (void)wire_rate;
    memset(service, 0, sizeof(*service));
    service->ring = ring;
    service->profile = *profile;
    service->sink.fd = -1;
    result = nds_service_add_rf_snapshot(service, &profile->initial_snapshot);
    if (result != 0) {
        return result;
    }
    result = ring->ops->configure(ring, profile->stream_id,
                                  profile->packet_type,
                                  profile->channel_mask,
                                  profile->sample_format,
                                  profile->samples_per_packet);
    if (result != 0) {
        return result;
    }
    result = nds_udp_open(&service->sink, &profile->destination);
    if (result != 0) {
        return result;
    }
    result = ring->ops->start(ring);
    if (result != 0) {
        nds_udp_close(&service->sink);
        return result;
    }
    return 0;
}

int nds_service_add_rf_snapshot(nds_service *service,
                                const nds_rf_snapshot *snapshot)
{
    nds_stream_profile candidate;
    uint64_t wire_rate;
    if (service == NULL || snapshot == NULL) {
        return -EINVAL;
    }
    if (service->snapshot_count >= NDS_MAX_RF_SNAPSHOTS) {
        return -ENOSPC;
    }
    candidate = service->profile;
    candidate.initial_snapshot = *snapshot;
    if (nds_validate_profile(&candidate, &wire_rate, NULL) != 0) {
        return -EINVAL;
    }
    (void)wire_rate;
    if (service->snapshot_count != 0U) {
        const nds_rf_snapshot *previous =
            &service->snapshots[service->snapshot_count - 1U];
        if (snapshot->activation_timestamp <= previous->activation_timestamp ||
            snapshot->device_state_revision <=
                previous->device_state_revision ||
            snapshot->state.configuration_revision <
                previous->state.configuration_revision) {
            return -EINVAL;
        }
    }
    service->snapshots[service->snapshot_count++] = *snapshot;
    return 0;
}

int nds_service_step(nds_service *service, int timeout_ms)
{
    nds_dma_block blocks[NDS_MAX_BATCH];
    nds_packet packets[NDS_MAX_BATCH];
    size_t packet_block[NDS_MAX_BATCH];
    bool released[NDS_MAX_BATCH];
    bool carries_pending[NDS_MAX_BATCH];
    bool carries_discontinuity[NDS_MAX_BATCH];
    size_t block_count = 0;
    size_t packet_count = 0;
    size_t sent_count = 0;
    size_t index;
    bool pending_assigned = false;
    int result;
    int status = 0;
    int release_result;
    if (service == NULL || service->ring == NULL || service->ring->ops == NULL) {
        return -EINVAL;
    }
    result = service->ring->ops->wait(service->ring, timeout_ms);
    if (result <= 0) {
        return result;
    }
    memset(released, 0, sizeof(released));
    memset(carries_pending, 0, sizeof(carries_pending));
    memset(carries_discontinuity, 0, sizeof(carries_discontinuity));
    while (block_count < service->profile.batch_size) {
        result = service->ring->ops->acquire(service->ring,
                                             &blocks[block_count]);
        if (result == -EAGAIN) {
            break;
        }
        if (result != 0) {
            status = result;
            goto cleanup;
        }
        ++service->counters.blocks_acquired;
        ++block_count;
    }
    for (index = 0; index < block_count; ++index) {
        nds_dma_block *block = &blocks[index];
        uint16_t synthetic_reason = 0;
        uint32_t synthetic_lost = 0;
        uint32_t synthetic_revision = 0;
        const nds_rf_snapshot *snapshot;
        bool source_gap = false;
        bool timing_gap = false;
        bool incoming_discontinuity = false;
        if (block_matches_profile(service, block) != 0) {
            ++service->counters.malformed_blocks;
            result = schedule_pending(
                service, NEPTUNE_EDGE_DISCONTINUITY_REASON_DMA_OVERRUN,
                UINT32_MAX);
            if (result != 0) {
                status = result;
                goto cleanup;
            }
            continue;
        }
        snapshot = resolve_snapshot(service, block);
        if (snapshot == NULL ||
            block->configuration_revision !=
                snapshot->state.configuration_revision) {
            ++service->counters.malformed_blocks;
            /* Never label samples with a guessed or temporally stale device
             * state. A matching immutable snapshot must already exist. */
            status = -ESTALE;
            goto cleanup;
        }
        incoming_discontinuity =
            (block->flags & (1U << 1)) != 0U &&
            block->discontinuity_revision > service->discontinuity_revision;
        if (block->discontinuity_revision > service->discontinuity_revision) {
            service->discontinuity_revision = block->discontinuity_revision;
        }
        if (service->have_source_sequence &&
            block->source_sequence != service->last_source_sequence + 1U) {
            source_gap = true;
            ++service->counters.source_sequence_gaps;
        }
        service->last_source_sequence = block->source_sequence;
        service->have_source_sequence = true;
        if (!timing_matches(service, block)) {
            timing_gap = true;
            ++service->counters.timestamp_discontinuities;
        }
        if (source_gap || timing_gap) {
            synthetic_lost = lost_input_ticks_before(service, block);
        }
        advance_timing(service, block);
        if (service->pending_discontinuity_reason != 0U && !pending_assigned) {
            synthetic_reason = service->pending_discontinuity_reason;
            synthetic_lost = saturating_add_u32(
                service->pending_lost_samples, synthetic_lost);
            synthetic_revision = service->pending_discontinuity_revision;
            carries_pending[packet_count] = true;
            pending_assigned = true;
        } else if ((source_gap || timing_gap) && !incoming_discontinuity) {
            synthetic_reason = source_gap
                                   ? NEPTUNE_EDGE_DISCONTINUITY_REASON_DMA_OVERRUN
                                   : NEPTUNE_EDGE_DISCONTINUITY_REASON_CLOCK_OR_INTERFACE_ERROR;
            if (service->discontinuity_revision == UINT32_MAX) {
                status = -EOVERFLOW;
                goto cleanup;
            }
            ++service->discontinuity_revision;
            synthetic_revision = service->discontinuity_revision;
        }
        result = nds_build_packet(block, snapshot,
                                  service->next_sequence,
                                  synthetic_reason, synthetic_lost,
                                  service->last_good_timestamp,
                                  synthetic_revision, &packets[packet_count]);
        ++service->next_sequence;
        if (result != 0) {
            if (carries_pending[packet_count]) {
                carries_pending[packet_count] = false;
                pending_assigned = false;
            }
            ++service->counters.malformed_blocks;
            result = schedule_pending(
                service, NEPTUNE_EDGE_DISCONTINUITY_REASON_DMA_OVERRUN,
                source_span_ticks(service, block));
            if (result != 0) {
                status = result;
                goto cleanup;
            }
            continue;
        }
        carries_discontinuity[packet_count] =
            synthetic_reason != 0U || (block->flags & (1U << 1)) != 0U;
        packet_block[packet_count] = index;
        ++packet_count;
    }
    if (packet_count != 0U) {
        result = nds_udp_send_batch(&service->sink, packets, packet_count,
                                    &sent_count);
        service->counters.datagrams_attempted += packet_count;
        service->counters.datagrams_sent += sent_count;
        for (index = 0; index < sent_count; ++index) {
            const nds_dma_block *block = &blocks[packet_block[index]];
            service->counters.payload_bytes_sent += block->payload_bytes;
            service->last_good_timestamp = last_source_tick(service, block);
            if (carries_pending[index]) {
                service->pending_discontinuity_reason = 0U;
                service->pending_lost_samples = 0U;
                service->pending_discontinuity_revision = 0U;
            }
            if (carries_discontinuity[index]) {
                ++service->counters.discontinuities_emitted;
            }
        }
        if (sent_count < packet_count) {
            uint64_t lost = 0;
            for (index = sent_count; index < packet_count; ++index) {
                lost += source_span_ticks(service,
                                          &blocks[packet_block[index]]);
            }
            ++service->counters.send_errors;
            result = schedule_pending(
                service,
                NEPTUNE_EDGE_DISCONTINUITY_REASON_ETHERNET_BACKPRESSURE,
                lost);
            if (result != 0) {
                status = result;
                goto cleanup;
            }
        }
        (void)result;
    }
    status = block_count != 0U ? 1 : 0;

cleanup:
    release_result = release_acquired(service, blocks, released, block_count);
    if (status >= 0 && release_result != 0) {
        return release_result;
    }
    return status;
}

void nds_service_destroy(nds_service *service)
{
    if (service == NULL) {
        return;
    }
    if (service->ring != NULL && service->ring->ops != NULL) {
        (void)service->ring->ops->stop(service->ring);
    }
    nds_udp_close(&service->sink);
}
