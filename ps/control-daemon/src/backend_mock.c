#include "necp_daemon.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct mock_state {
    uint64_t sample_counter;
    uint64_t uptime_ns;
    uint64_t uptime_fraction;
    unsigned commits;
    bool tx_enabled;
    bool tx_inhibited;
    necp_health health;
    neptune_edge_control_rf_config_v1 rf_config;
    neptune_edge_control_pipeline_config_v1 pipeline_config;
} mock_state;

static void mock_store_u16(uint8_t *value, uint16_t number)
{
    value[0] = (uint8_t)number;
    value[1] = (uint8_t)(number >> 8);
}

static void mock_store_u32(uint8_t *value, uint32_t number)
{
    value[0] = (uint8_t)number;
    value[1] = (uint8_t)(number >> 8);
    value[2] = (uint8_t)(number >> 16);
    value[3] = (uint8_t)(number >> 24);
}

static void mock_store_u64(uint8_t *value, uint64_t number)
{
    mock_store_u32(value, (uint32_t)number);
    mock_store_u32(value + 4, (uint32_t)(number >> 32));
}

static uint32_t mock_load_u32(const uint8_t *value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
           ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static void mock_initialize_rf(mock_state *state)
{
    uint8_t *rf = (uint8_t *)&state->rf_config;
    memset(rf, 0, sizeof(state->rf_config));
    mock_store_u16(rf, NEPTUNE_EDGE_CONTROL_ITEM_RF_CONFIG);
    mock_store_u16(rf + 2,
                   (uint16_t)(sizeof(state->rf_config) / 4U));
    mock_store_u64(rf + 8, UINT64_C(1000000000));
    mock_store_u32(rf + 16, NEPTUNE_EDGE_INTERNAL_SAMPLE_RATE_HZ);
    mock_store_u32(rf + 20, NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ);
    mock_store_u32(rf + 24, UINT32_C(20000000));
    mock_store_u16(rf + 36, UINT16_C(1));
}

static int mock_initialize(necp_backend *backend)
{
    mock_state *state = (mock_state *)backend->state;
    state->sample_counter = UINT64_C(4096);
    state->uptime_ns = UINT64_C(1000000000);
    state->uptime_fraction = 0U;
    state->health.temperature_mc = 42000;
    state->health.supply_mv = UINT32_C(1000);
    state->health.status_flags = NECP_HEALTH_FLAG_RF_PLL_LOCKED |
                                NECP_HEALTH_FLAG_ETHERNET_LINK |
                                NECP_HEALTH_FLAG_USB_CONFIGURED |
                                NECP_HEALTH_FLAG_BACKEND_READY;
    state->health.ethernet_link_state = 1U;
    state->health.usb_state = 1U;
    state->health.pll_lock_mask = 3U;
    mock_initialize_rf(state);
    return 0;
}

static void mock_destroy(necp_backend *backend)
{
    free(backend->state);
    backend->state = NULL;
    backend->ops = NULL;
}

static int mock_get_identity(necp_backend *backend, necp_identity *identity)
{
    (void)backend;
    memset(identity, 0, sizeof(*identity));
    identity->hardware_id = UINT32_C(0x50323130);
    identity->hardware_revision = 0U;
    identity->firmware_build_id = UINT64_C(0x4d4f434b00000001);
    identity->device_serial_hash = UINT64_C(0x4d4f434b4e455054);
    return 0;
}

static int mock_get_health(necp_backend *backend, necp_health *health)
{
    mock_state *state = (mock_state *)backend->state;
    *health = state->health;
    health->uptime_ns = state->uptime_ns;
    return 0;
}

static int mock_get_rf(necp_backend *backend,
                       neptune_edge_control_rf_config_v1 *config)
{
    mock_state *state = (mock_state *)backend->state;
    *config = state->rf_config;
    return 0;
}

static int mock_read_sample_counter(necp_backend *backend, uint64_t *timestamp)
{
    mock_state *state = (mock_state *)backend->state;
    *timestamp = state->sample_counter;
    return 0;
}

static int mock_commit_rf(necp_backend *backend,
                          const neptune_edge_control_rf_config_v1 *config,
                          uint64_t activation_timestamp)
{
    mock_state *state = (mock_state *)backend->state;
    const uint8_t *bytes = (const uint8_t *)config;
    uint32_t flags = mock_load_u32(bytes + 44);
    if (activation_timestamp <= state->sample_counter ||
        state->health.configuration_revision == UINT32_MAX ||
        mock_load_u32(bytes + 40) !=
            state->health.configuration_revision) {
        return -EINVAL;
    }
    state->sample_counter = activation_timestamp;
    state->tx_enabled = (flags & NECP_RF_FLAG_TX_ENABLE) != 0U;
    state->rf_config = *config;
    ++state->health.configuration_revision;
    mock_store_u32((uint8_t *)&state->rf_config + 40,
                   state->health.configuration_revision);
    ++state->commits;
    return 0;
}

static int mock_commit_pipeline(
    necp_backend *backend,
    const neptune_edge_control_pipeline_config_v1 *config,
    uint64_t changed_fields, uint64_t activation_timestamp)
{
    mock_state *state = (mock_state *)backend->state;
    const uint8_t *bytes = (const uint8_t *)config;
    if (changed_fields == 0U || activation_timestamp <= state->sample_counter ||
        state->health.configuration_revision == UINT32_MAX ||
        mock_load_u32(bytes + 28) != state->health.configuration_revision ||
        mock_load_u32(bytes + 32) != state->health.calibration_revision) {
        return -EINVAL;
    }
    state->sample_counter = activation_timestamp;
    state->pipeline_config = *config;
    ++state->health.configuration_revision;
    mock_store_u32((uint8_t *)&state->pipeline_config + 28,
                   state->health.configuration_revision);
    ++state->commits;
    return 0;
}

static int mock_stream_update(necp_backend *backend, uint16_t command,
                              const neptune_edge_control_stream_config_v1 *config,
                              uint64_t activation_timestamp)
{
    mock_state *state = (mock_state *)backend->state;
    (void)config;
    if (command == NEPTUNE_EDGE_COMMAND_CREATE_STREAM ||
        command == NEPTUNE_EDGE_COMMAND_DESTROY_STREAM) {
        ++state->commits;
        return 0;
    }
    if (activation_timestamp <= state->sample_counter ||
        state->health.configuration_revision == UINT32_MAX) {
        return -EINVAL;
    }
    state->sample_counter = activation_timestamp;
    ++state->health.configuration_revision;
    ++state->commits;
    return 0;
}

static int mock_reset_counters(necp_backend *backend)
{
    mock_state *state = (mock_state *)backend->state;
    state->health.fifo_overflows = 0;
    state->health.dma_overruns = 0;
    state->health.dropped_packets = 0;
    return 0;
}

static int mock_force_tx_off(necp_backend *backend)
{
    mock_state *state = (mock_state *)backend->state;
    uint32_t flags = mock_load_u32((uint8_t *)&state->rf_config + 44);
    state->tx_enabled = false;
    mock_store_u32((uint8_t *)&state->rf_config + 44,
                   flags & ~NECP_RF_FLAG_TX_ENABLE);
    return 0;
}

static int mock_tx_is_inhibited(necp_backend *backend, bool *inhibited)
{
    mock_state *state = (mock_state *)backend->state;
    *inhibited = state->tx_inhibited;
    return 0;
}

static const necp_backend_ops MOCK_OPS = {
    .initialize = mock_initialize,
    .destroy = mock_destroy,
    .get_identity = mock_get_identity,
    .get_health = mock_get_health,
    .get_rf = mock_get_rf,
    .read_sample_counter = mock_read_sample_counter,
    .commit_rf = mock_commit_rf,
    .commit_pipeline = mock_commit_pipeline,
    .stream_update = mock_stream_update,
    .reset_counters = mock_reset_counters,
    .force_tx_off = mock_force_tx_off,
    .tx_is_inhibited = mock_tx_is_inhibited,
};

int necp_mock_backend_create(necp_backend *backend)
{
    mock_state *state;
    if (backend == NULL) {
        return -EINVAL;
    }
    state = (mock_state *)calloc(1, sizeof(*state));
    if (state == NULL) {
        return -ENOMEM;
    }
    state->tx_inhibited = true;
    backend->ops = &MOCK_OPS;
    backend->state = state;
    return 0;
}

void necp_mock_backend_advance(necp_backend *backend, uint64_t ticks)
{
    mock_state *state = backend != NULL ? (mock_state *)backend->state : NULL;
    if (state != NULL) {
        const uint64_t rate = NEPTUNE_EDGE_INTERNAL_SAMPLE_RATE_HZ;
        const uint64_t nanoseconds_per_second = UINT64_C(1000000000);
        uint64_t seconds = ticks / rate;
        uint64_t scaled_remainder =
            (ticks % rate) * nanoseconds_per_second + state->uptime_fraction;
        uint64_t fractional_ns;
        uint64_t delta_ns;
        state->sample_counter = UINT64_MAX - state->sample_counter < ticks
                                    ? UINT64_MAX
                                    : state->sample_counter + ticks;
        if (seconds > UINT64_MAX / nanoseconds_per_second) {
            state->uptime_ns = UINT64_MAX;
            state->uptime_fraction = 0U;
            return;
        }
        delta_ns = seconds * nanoseconds_per_second;
        fractional_ns = scaled_remainder / rate;
        if (UINT64_MAX - delta_ns < fractional_ns) {
            state->uptime_ns = UINT64_MAX;
            state->uptime_fraction = 0U;
            return;
        }
        delta_ns += fractional_ns;
        state->uptime_fraction = scaled_remainder % rate;
        if (UINT64_MAX - state->uptime_ns < delta_ns) {
            state->uptime_ns = UINT64_MAX;
            state->uptime_fraction = 0U;
        } else {
            state->uptime_ns += delta_ns;
        }
    }
}

unsigned necp_mock_backend_commit_count(const necp_backend *backend)
{
    const mock_state *state =
        backend != NULL ? (const mock_state *)backend->state : NULL;
    return state != NULL ? state->commits : 0U;
}
