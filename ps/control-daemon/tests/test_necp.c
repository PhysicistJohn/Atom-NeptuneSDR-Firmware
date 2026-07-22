#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE

#include "necp_daemon.h"
#include "neptune_pl_registers_v1.h"
#include "neptune_stream_uapi.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ARRAY_SIZE(value) (sizeof(value) / sizeof((value)[0]))

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

static void refresh_crc(uint8_t *frame)
{
    uint32_t payload_length = get32(frame + 16);
    uint8_t header[NEPTUNE_EDGE_CONTROL_HEADER_BYTES];
    put32(frame + 32,
          neptune_edge_crc32c(frame + NEPTUNE_EDGE_CONTROL_HEADER_BYTES,
                              payload_length));
    memcpy(header, frame, sizeof(header));
    memset(header + 36, 0, 4);
    put32(frame + 36, neptune_edge_crc32c(header, sizeof(header)));
}

static size_t build_request(uint8_t *frame, size_t capacity, uint16_t command,
                            uint8_t flags, uint32_t transaction,
                            uint32_t revision, uint64_t activation,
                            const uint8_t *payload, size_t payload_length)
{
    size_t length = NEPTUNE_EDGE_CONTROL_HEADER_BYTES + payload_length;
    if (length > capacity) {
        abort();
    }
    memset(frame, 0, length);
    memcpy(frame, "NECP", 4);
    frame[4] = NEPTUNE_EDGE_PROTOCOL_VERSION;
    frame[5] = NEPTUNE_EDGE_CONTROL_HEADER_BYTES / 4U;
    frame[6] = NEPTUNE_EDGE_MESSAGE_KIND_REQUEST;
    frame[7] = flags;
    put16(frame + 8, command);
    put32(frame + 12, transaction);
    put32(frame + 16, (uint32_t)payload_length);
    put32(frame + 20, revision);
    put64(frame + 24, activation);
    if (payload_length != 0U) {
        memcpy(frame + NEPTUNE_EDGE_CONTROL_HEADER_BYTES, payload,
               payload_length);
    }
    refresh_crc(frame);
    return length;
}

static size_t make_atomic(uint8_t *item, uint32_t config_revision,
                          uint32_t calibration_revision, uint64_t activation,
                          uint64_t changed)
{
    memset(item, 0, sizeof(neptune_edge_control_atomic_commit_v1));
    put16(item, NEPTUNE_EDGE_CONTROL_ITEM_ATOMIC_COMMIT);
    put16(item + 2,
          sizeof(neptune_edge_control_atomic_commit_v1) / 4U);
    put32(item + 8, config_revision);
    put32(item + 12, calibration_revision);
    put64(item + 16, activation);
    put64(item + 24, changed);
    return sizeof(neptune_edge_control_atomic_commit_v1);
}

static size_t make_rf(uint8_t *item, uint32_t revision, uint32_t flags)
{
    memset(item, 0, sizeof(neptune_edge_control_rf_config_v1));
    put16(item, NEPTUNE_EDGE_CONTROL_ITEM_RF_CONFIG);
    put16(item + 2, sizeof(neptune_edge_control_rf_config_v1) / 4U);
    put64(item + 8, UINT64_C(915000000));
    put32(item + 16, NEPTUNE_EDGE_INTERNAL_SAMPLE_RATE_HZ);
    put32(item + 20, NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ);
    put32(item + 24, UINT32_C(20000000));
    put32(item + 28, UINT32_C(30000));
    put32(item + 32, UINT32_C(31000));
    put16(item + 36, 1);
    item[38] = NEPTUNE_EDGE_GAIN_MODE_MANUAL;
    item[39] = NEPTUNE_EDGE_GAIN_MODE_SLOW_ATTACK;
    put32(item + 40, revision);
    put32(item + 44, flags);
    return sizeof(neptune_edge_control_rf_config_v1);
}

static size_t make_set_rf_request(uint8_t *frame, uint32_t revision,
                                  uint32_t calibration_revision,
                                  uint32_t rf_flags)
{
    uint8_t payload[sizeof(neptune_edge_control_rf_config_v1) +
                    sizeof(neptune_edge_control_atomic_commit_v1)];
    size_t offset = make_rf(payload, revision, rf_flags);
    offset += make_atomic(payload + offset, revision, calibration_revision,
                          UINT64_MAX,
                          NEPTUNE_EDGE_CHANGED_FIELD_CENTER_FREQUENCY |
                              NEPTUNE_EDGE_CHANGED_FIELD_ANALOG_GAIN);
    return build_request(frame, NECP_MAX_FRAME_BYTES,
                         NEPTUNE_EDGE_COMMAND_SET_RF,
                         NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED |
                             NEPTUNE_EDGE_CONTROL_FLAG_ATOMIC,
                         17, revision, UINT64_MAX, payload, offset);
}

static size_t make_pipeline(uint8_t *item, uint32_t revision,
                            uint32_t calibration_revision,
                            uint8_t rounding_mode)
{
    memset(item, 0, sizeof(neptune_edge_control_pipeline_config_v1));
    put16(item, NEPTUNE_EDGE_CONTROL_ITEM_PIPELINE_CONFIG);
    put16(item + 2,
          sizeof(neptune_edge_control_pipeline_config_v1) / 4U);
    put32(item + 8, NEPTUNE_EDGE_PIPELINE_BLOCK_RAW_TAP |
                            NEPTUNE_EDGE_PIPELINE_BLOCK_NORMALIZER |
                            NEPTUNE_EDGE_PIPELINE_BLOCK_QUANTIZER);
    put32(item + 16, 1U << NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ);
    item[20] = NEPTUNE_EDGE_SAMPLE_FORMAT_S8;
    item[21] = NEPTUNE_EDGE_NORMALIZATION_STRATEGY_FIXED_SHIFT;
    item[22] = 1U;
    item[23] = rounding_mode;
    item[24] = 4U;
    put32(item + 28, revision);
    put32(item + 32, calibration_revision);
    put32(item + 36, revision + 1U);
    return sizeof(neptune_edge_control_pipeline_config_v1);
}

static size_t make_pipeline_request(uint8_t *frame, uint32_t revision,
                                    uint32_t calibration_revision,
                                    uint8_t rounding_mode)
{
    uint8_t payload[sizeof(neptune_edge_control_atomic_commit_v1) +
                    sizeof(neptune_edge_control_pipeline_config_v1)];
    size_t offset = make_atomic(payload, revision, calibration_revision,
                                UINT64_MAX,
                                NEPTUNE_EDGE_CHANGED_FIELD_QUANTIZATION);
    offset += make_pipeline(payload + offset, revision, calibration_revision,
                            rounding_mode);
    return build_request(frame, NECP_MAX_FRAME_BYTES,
                         NEPTUNE_EDGE_COMMAND_CONFIGURE_PIPELINE,
                         NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED |
                             NEPTUNE_EDGE_CONTROL_FLAG_ATOMIC,
                         19, revision, UINT64_MAX, payload, offset);
}

static int response_status(const uint8_t *response, size_t length)
{
    uint8_t header[NEPTUNE_EDGE_CONTROL_HEADER_BYTES];
    if (length < sizeof(header) || memcmp(response, "NECP", 4) != 0) {
        return -1;
    }
    memcpy(header, response, sizeof(header));
    memset(header + 36, 0, 4);
    if (neptune_edge_crc32c(header, sizeof(header)) != get32(response + 36) ||
        neptune_edge_crc32c(response + sizeof(header), get32(response + 16)) !=
            get32(response + 32)) {
        return -1;
    }
    return get16(response + 10);
}

static void initialize(necp_backend *backend, necp_context *context)
{
    CHECK(necp_mock_backend_create(backend) == 0);
    CHECK(necp_context_init(context, backend) == 0);
}

static void test_identity_and_default_safety(void)
{
    necp_backend backend;
    necp_context context;
    uint8_t request[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    size_t request_length;
    size_t response_length;
    initialize(&backend, &context);
    CHECK(!context.tx_enabled);
    CHECK(context.tx_inhibited);
    request_length = build_request(request, sizeof(request),
                                   NEPTUNE_EDGE_COMMAND_GET_IDENTITY,
                                   NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED, 1, 0,
                                   UINT64_MAX, NULL, 0);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(get32(response + 16) ==
          sizeof(neptune_edge_control_identity_v1) +
              sizeof(neptune_edge_control_safety_v1));
    CHECK(get16(response + 40) == NEPTUNE_EDGE_CONTROL_ITEM_IDENTITY);
    CHECK(get32(response + 40 + 8) == UINT32_C(0x50323130));
    CHECK(get64(response + 40 + 24) == UINT64_C(0x4d4f434b00000001));
    CHECK(get16(response + 40 + sizeof(neptune_edge_control_identity_v1)) ==
          NEPTUNE_EDGE_CONTROL_ITEM_SAFETY);
    CHECK(response[40 + sizeof(neptune_edge_control_identity_v1) + 8] == 0);
    CHECK(response[40 + sizeof(neptune_edge_control_identity_v1) + 9] == 1);
    CHECK(response[40 + sizeof(neptune_edge_control_identity_v1) + 10] == 1);
    necp_context_destroy(&context);
}

static void test_mock_health_uses_nanoseconds_and_authoritative_revisions(void)
{
    necp_backend backend;
    necp_context context;
    uint8_t request[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    size_t request_length;
    size_t response_length;
    initialize(&backend, &context);
    request_length = build_request(
        request, sizeof(request), NEPTUNE_EDGE_COMMAND_GET_HEALTH,
        NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED, 2, 0, UINT64_MAX, NULL, 0);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(get16(response + 40) == NEPTUNE_EDGE_CONTROL_ITEM_HEALTH);
    CHECK(get64(response + 48) == UINT64_C(1000000000));
    CHECK(get32(response + 40 + 64) == 0U);
    CHECK(get32(response + 40 + 68) == 0U);

    necp_mock_backend_advance(&backend,
                              NEPTUNE_EDGE_INTERNAL_SAMPLE_RATE_HZ);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(get64(response + 48) == UINT64_C(2000000000));
    necp_context_destroy(&context);
}

static void test_get_rf_reads_authoritative_backend_state(void)
{
    necp_backend backend;
    necp_context context;
    uint8_t request[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    size_t request_length;
    size_t response_length;
    initialize(&backend, &context);
    request_length = build_request(
        request, sizeof(request), NEPTUNE_EDGE_COMMAND_GET_RF,
        NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED, 3, 0, UINT64_MAX, NULL, 0);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(get16(response + 40) == NEPTUNE_EDGE_CONTROL_ITEM_RF_CONFIG);
    CHECK(get64(response + 48) == UINT64_C(1000000000));
    CHECK(get32(response + 40 + 40) == 0U);
    necp_context_destroy(&context);
}

static void test_atomic_rf_and_matching_event(void)
{
    necp_backend backend;
    necp_context context;
    uint8_t request[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    uint8_t event[NECP_MAX_FRAME_BYTES];
    size_t request_length;
    size_t response_length;
    size_t event_length;
    uint64_t activation;
    initialize(&backend, &context);
    request_length = make_set_rf_request(request, 0, 0, 0);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(context.configuration_revision == 1);
    CHECK(necp_mock_backend_commit_count(&backend) == 1);
    activation = get64(response + 24);
    CHECK(activation > 4096);
    CHECK(get32(response + 20) == 1);
    CHECK(get16(response + 40) == NEPTUNE_EDGE_CONTROL_ITEM_STATE_CHANGE);
    CHECK(get64(response + 40 + 24) == activation);
    CHECK(necp_pop_event(&context, event, sizeof(event), &event_length) == 1);
    CHECK(response_status(event, event_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(event[6] == NEPTUNE_EDGE_MESSAGE_KIND_EVENT);
    CHECK(get16(event + 8) == NEPTUNE_EDGE_COMMAND_STATE_CHANGE_EVENT);
    CHECK(get64(event + 24) == activation);
    CHECK(get64(event + 40 + 24) == activation);
    necp_context_destroy(&context);
}

static void test_revision_conflict_has_no_side_effect(void)
{
    necp_backend backend;
    necp_context context;
    uint8_t request[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    size_t request_length;
    size_t response_length;
    initialize(&backend, &context);
    request_length = make_set_rf_request(request, 1, 0, 0);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) ==
          NEPTUNE_EDGE_STATUS_REVISION_CONFLICT);
    CHECK(context.configuration_revision == 0);
    CHECK(necp_mock_backend_commit_count(&backend) == 0);
    necp_context_destroy(&context);
}

static void test_revision_wrap_is_rejected_before_backend(void)
{
    necp_backend backend;
    necp_context context;
    uint8_t request[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    size_t request_length;
    size_t response_length;
    initialize(&backend, &context);
    context.configuration_revision = UINT32_MAX;
    request_length = make_set_rf_request(request, UINT32_MAX, 0, 0);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) ==
          NEPTUNE_EDGE_STATUS_INVALID_VALUE);
    CHECK(context.configuration_revision == UINT32_MAX);
    CHECK(necp_mock_backend_commit_count(&backend) == 0);
    necp_context_destroy(&context);
}

static void test_tx_inhibit_is_authoritative(void)
{
    necp_backend backend;
    necp_context context;
    uint8_t request[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    size_t request_length;
    size_t response_length;
    initialize(&backend, &context);
    request_length =
        make_set_rf_request(request, 0, 0, NECP_RF_FLAG_TX_ENABLE);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) ==
          NEPTUNE_EDGE_STATUS_AUTHORIZATION_REQUIRED);
    CHECK(!context.tx_enabled);
    CHECK(necp_mock_backend_commit_count(&backend) == 0);
    necp_context_destroy(&context);
}

static void test_pipeline_commit_uses_canonical_state_change_contract(void)
{
    necp_backend backend;
    necp_context context;
    uint8_t request[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    uint8_t event[NECP_MAX_FRAME_BYTES];
    size_t request_length;
    size_t response_length;
    size_t event_length;
    initialize(&backend, &context);
    request_length = make_pipeline_request(
        request, 0, 0, NEPTUNE_EDGE_ROUNDING_MODE_ROUND_TO_NEAREST_EVEN);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(get16(response + 40) == NEPTUNE_EDGE_CONTROL_ITEM_STATE_CHANGE);
    CHECK(get32(response + 40 + 8) == 0U);
    CHECK(get32(response + 40 + 12) == 1U);
    CHECK(get64(response + 40 + 32) ==
          NEPTUNE_EDGE_CHANGED_FIELD_QUANTIZATION);
    CHECK(context.configuration_revision == 1U);
    CHECK(necp_mock_backend_commit_count(&backend) == 1U);
    CHECK(necp_pop_event(&context, event, sizeof(event), &event_length) == 1);
    CHECK(response_status(event, event_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(get32(event + 20) == 1U);
    necp_context_destroy(&context);
}

static void test_invalid_pipeline_never_reaches_backend(void)
{
    necp_backend backend;
    necp_context context;
    uint8_t request[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    size_t request_length;
    size_t response_length;
    initialize(&backend, &context);
    request_length = make_pipeline_request(request, 0, 0, 0U);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) ==
          NEPTUNE_EDGE_STATUS_INVALID_VALUE);
    CHECK(get16(response + 40) == NEPTUNE_EDGE_CONTROL_ITEM_ERROR_DETAIL);
    CHECK(context.configuration_revision == 0U);
    CHECK(necp_mock_backend_commit_count(&backend) == 0U);
    necp_context_destroy(&context);
}

static size_t make_stream_request(uint8_t *frame, uint16_t command,
                                  uint32_t revision, uint16_t mtu,
                                  uint32_t samples)
{
    uint8_t payload[sizeof(neptune_edge_control_atomic_commit_v1) +
                    sizeof(neptune_edge_control_stream_config_v1)];
    bool atomic = command == NEPTUNE_EDGE_COMMAND_START_STREAM ||
                  command == NEPTUNE_EDGE_COMMAND_STOP_STREAM;
    size_t offset = atomic
                        ? make_atomic(
                              payload, revision, 0, UINT64_MAX,
                              NEPTUNE_EDGE_CHANGED_FIELD_STREAM_DESTINATION)
                        : 0U;
    uint8_t *stream = payload + offset;
    memset(stream, 0, sizeof(neptune_edge_control_stream_config_v1));
    put16(stream, NEPTUNE_EDGE_CONTROL_ITEM_STREAM_CONFIG);
    put16(stream + 2,
          sizeof(neptune_edge_control_stream_config_v1) / 4U);
    put32(stream + 8, 9);
    stream[12] = 192;
    stream[13] = 0;
    stream[14] = 2;
    stream[15] = 1;
    put16(stream + 16, 50000);
    put16(stream + 18, mtu);
    put32(stream + 20, 1U << NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ);
    stream[24] = NEPTUNE_EDGE_SAMPLE_FORMAT_S8;
    stream[25] = 1;
    put32(stream + 28, samples);
    offset += sizeof(neptune_edge_control_stream_config_v1);
    return build_request(frame, NECP_MAX_FRAME_BYTES, command,
                         NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED |
                             (atomic ? NEPTUNE_EDGE_CONTROL_FLAG_ATOMIC : 0U),
                         22, revision, UINT64_MAX, payload, offset);
}

static void test_full_rate_stream_requires_jumbo(void)
{
    necp_backend backend;
    necp_context context;
    uint8_t request[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    size_t request_length;
    size_t response_length;
    initialize(&backend, &context);
    request_length = make_stream_request(
        request, NEPTUNE_EDGE_COMMAND_CREATE_STREAM, 0, 1500, 600);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) ==
          NEPTUNE_EDGE_STATUS_INVALID_VALUE);
    CHECK(necp_mock_backend_commit_count(&backend) == 0);
    request_length = make_stream_request(
        request, NEPTUNE_EDGE_COMMAND_CREATE_STREAM, 0, 9000, 4096);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(necp_mock_backend_commit_count(&backend) == 1);
    CHECK(context.streams[0].created);
    CHECK(context.configuration_revision == 0U);
    CHECK(get16(response + 40) == NEPTUNE_EDGE_CONTROL_ITEM_STREAM_STATUS);
    CHECK(get32(response + 40 + 12) == NEPTUNE_EDGE_STREAM_STATE_CREATED);
    necp_context_destroy(&context);
}

static void test_stream_lifecycle_uses_canonical_status_contract(void)
{
    necp_backend backend;
    necp_context context;
    uint8_t request[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    uint8_t event[NECP_MAX_FRAME_BYTES];
    size_t request_length;
    size_t response_length;
    size_t event_length;
    initialize(&backend, &context);
    request_length = make_stream_request(
        request, NEPTUNE_EDGE_COMMAND_CREATE_STREAM, 0, 9000, 4096);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(get16(response + 40) == NEPTUNE_EDGE_CONTROL_ITEM_STREAM_STATUS);
    CHECK(get32(response + 52) == NEPTUNE_EDGE_STREAM_STATE_CREATED);

    request_length = make_stream_request(
        request, NEPTUNE_EDGE_COMMAND_START_STREAM, 0, 9000, 4096);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(get32(response + 20) == 1U);
    CHECK(get16(response + 40) == NEPTUNE_EDGE_CONTROL_ITEM_STATE_CHANGE);
    CHECK(get16(response + 40 + sizeof(neptune_edge_control_state_change_v1)) ==
          NEPTUNE_EDGE_CONTROL_ITEM_STREAM_STATUS);
    CHECK(get32(response + 40 +
                sizeof(neptune_edge_control_state_change_v1) + 12) ==
          NEPTUNE_EDGE_STREAM_STATE_RUNNING);
    CHECK(necp_pop_event(&context, event, sizeof(event), &event_length) == 1);

    request_length = make_stream_request(
        request, NEPTUNE_EDGE_COMMAND_STOP_STREAM, 1, 9000, 4096);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(get32(response + 20) == 2U);
    CHECK(get32(response + 40 +
                sizeof(neptune_edge_control_state_change_v1) + 12) ==
          NEPTUNE_EDGE_STREAM_STATE_CREATED);
    CHECK(necp_pop_event(&context, event, sizeof(event), &event_length) == 1);

    request_length = make_stream_request(
        request, NEPTUNE_EDGE_COMMAND_DESTROY_STREAM, 2, 9000, 4096);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(get32(response + 20) == 2U);
    CHECK(get16(response + 40) == NEPTUNE_EDGE_CONTROL_ITEM_STREAM_STATUS);
    CHECK(get32(response + 52) == NEPTUNE_EDGE_STREAM_STATE_NONE);
    CHECK(!context.streams[0].created);
    CHECK(necp_pop_event(&context, event, sizeof(event), &event_length) == 0);
    necp_context_destroy(&context);
}

static void test_reset_counters_requires_system_action_and_returns_snapshot(void)
{
    necp_backend backend;
    necp_context context;
    uint8_t request[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    uint8_t action[sizeof(neptune_edge_control_system_action_v1)] = {0};
    size_t request_length;
    size_t response_length;
    initialize(&backend, &context);
    request_length = build_request(
        request, sizeof(request), NEPTUNE_EDGE_COMMAND_RESET_COUNTERS,
        NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED, 31, 0, UINT64_MAX, NULL, 0);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) ==
          NEPTUNE_EDGE_STATUS_INVALID_VALUE);

    put16(action, NEPTUNE_EDGE_CONTROL_ITEM_SYSTEM_ACTION);
    put16(action + 2, sizeof(action) / 4U);
    put16(action + 8, NEPTUNE_EDGE_SYSTEM_ACTION_KIND_RESET_COUNTERS);
    put32(action + 12, UINT32_C(0x52455345));
    request_length = build_request(
        request, sizeof(request), NEPTUNE_EDGE_COMMAND_RESET_COUNTERS,
        NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED, 32, 0, UINT64_MAX, action,
        sizeof(action));
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(get32(response + 16) ==
          sizeof(neptune_edge_control_counter_snapshot_v1));
    CHECK(get16(response + 40) ==
          NEPTUNE_EDGE_CONTROL_ITEM_COUNTER_SNAPSHOT);
    necp_context_destroy(&context);
}

static void test_get_counters_returns_authoritative_sample_counter(void)
{
    necp_backend backend;
    necp_context context;
    uint8_t request[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    size_t request_length;
    size_t response_length;
    initialize(&backend, &context);
    necp_mock_backend_advance(&backend, UINT64_C(1234));
    request_length = build_request(
        request, sizeof(request), NEPTUNE_EDGE_COMMAND_GET_COUNTERS,
        NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED, 33, 0, UINT64_MAX, NULL, 0);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(get32(response + 16) ==
          sizeof(neptune_edge_control_counter_snapshot_v1));
    CHECK(get16(response + 40) ==
          NEPTUNE_EDGE_CONTROL_ITEM_COUNTER_SNAPSHOT);
    CHECK(get64(response + 40 + 48) == UINT64_C(5330));
    necp_context_destroy(&context);
}

static void test_hard_disable_tx_is_timestamped_without_revision_fabrication(void)
{
    necp_backend backend;
    necp_context context;
    uint8_t action[sizeof(neptune_edge_control_system_action_v1)] = {0};
    uint8_t request[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    uint8_t event[NECP_MAX_FRAME_BYTES];
    size_t request_length;
    size_t response_length;
    size_t event_length;
    initialize(&backend, &context);
    put16(action, NEPTUNE_EDGE_CONTROL_ITEM_SYSTEM_ACTION);
    put16(action + 2, sizeof(action) / 4U);
    put16(action + 8, NEPTUNE_EDGE_SYSTEM_ACTION_KIND_HARD_DISABLE_TX);
    put32(action + 12, UINT32_C(0x54584f46));
    request_length = build_request(
        request, sizeof(request), NEPTUNE_EDGE_COMMAND_HARD_DISABLE_TX,
        NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED, 34, 0, UINT64_MAX, action,
        sizeof(action));
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(get32(response + 20) == 0U);
    CHECK(get64(response + 24) == UINT64_C(4096));
    CHECK(get16(response + 40) == NEPTUNE_EDGE_CONTROL_ITEM_STATE_CHANGE);
    CHECK(get32(response + 40 + 8) == 0U);
    CHECK(get32(response + 40 + 12) == 0U);
    CHECK(get64(response + 40 + 32) == 0U);
    CHECK(necp_mock_backend_commit_count(&backend) == 0U);
    CHECK(necp_pop_event(&context, event, sizeof(event), &event_length) == 1);
    CHECK(get32(event + 20) == 0U);
    CHECK(get64(event + 24) == UINT64_C(4096));
    necp_context_destroy(&context);
}

static void test_unknown_required_item_rejected(void)
{
    necp_backend backend;
    necp_context context;
    uint8_t request[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    uint8_t payload[8] = {0};
    size_t request_length;
    size_t response_length;
    initialize(&backend, &context);
    put16(payload, 99);
    put16(payload + 2, 2);
    put32(payload + 4, NEPTUNE_EDGE_ITEM_FLAG_REQUIRED);
    request_length = build_request(request, sizeof(request),
                                   NEPTUNE_EDGE_COMMAND_GET_HEALTH,
                                   NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED, 3, 0,
                                   UINT64_MAX, payload, sizeof(payload));
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) ==
          NEPTUNE_EDGE_STATUS_UNSUPPORTED);
    necp_context_destroy(&context);
}

typedef struct callback_state {
    unsigned count;
    size_t last_length;
} callback_state;

static int count_frame(void *opaque, const uint8_t *frame, size_t length)
{
    callback_state *state = (callback_state *)opaque;
    if (memcmp(frame, "NECP", 4) != 0) {
        return -1;
    }
    ++state->count;
    state->last_length = length;
    return 0;
}

static void test_incremental_decoder(void)
{
    uint8_t frame[NECP_MAX_FRAME_BYTES];
    size_t length = build_request(frame, sizeof(frame),
                                  NEPTUNE_EDGE_COMMAND_GET_HEALTH,
                                  NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED, 3, 0,
                                  UINT64_MAX, NULL, 0);
    necp_decoder decoder;
    callback_state state = {0, 0};
    size_t index;
    necp_decoder_init(&decoder);
    for (index = 0; index < length; ++index) {
        CHECK(necp_decoder_feed(&decoder, frame + index, 1, count_frame, &state,
                                NULL) == 0);
    }
    CHECK(state.count == 1);
    CHECK(state.last_length == length);
    put32(frame + 16, NECP_MAX_PAYLOAD_BYTES + 1U);
    necp_decoder_init(&decoder);
    CHECK(necp_decoder_feed(&decoder, frame,
                            NEPTUNE_EDGE_CONTROL_HEADER_BYTES, count_frame,
                            &state, NULL) == -EMSGSIZE);
}

static void test_crc_fuzz_never_commits(void)
{
    necp_backend backend;
    necp_context context;
    uint8_t valid[NECP_MAX_FRAME_BYTES];
    uint8_t mutated[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    size_t length;
    size_t response_length;
    size_t index;
    initialize(&backend, &context);
    length = make_set_rf_request(valid, 0, 0, 0);
    for (index = 0; index < length; ++index) {
        int result;
        memcpy(mutated, valid, length);
        mutated[index] ^= (uint8_t)(UINT8_C(0x5a) ^ (uint8_t)index);
        if (mutated[index] == valid[index]) {
            mutated[index] ^= 1U;
        }
        result = necp_handle_frame(&context, mutated, length, response,
                                   sizeof(response), &response_length);
        CHECK(result == 0 || result == -EBADMSG);
        CHECK(necp_mock_backend_commit_count(&backend) == 0);
        CHECK(context.configuration_revision == 0);
    }
    necp_context_destroy(&context);
}

static void test_semantic_length_fuzz_never_commits(void)
{
    necp_backend backend;
    necp_context context;
    uint8_t frame[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    size_t length;
    size_t response_length;
    unsigned words;
    initialize(&backend, &context);
    length = make_set_rf_request(frame, 0, 0, 0);
    for (words = 0; words < 32U; ++words) {
        if (words == sizeof(neptune_edge_control_rf_config_v1) / 4U) {
            continue;
        }
        put16(frame + NEPTUNE_EDGE_CONTROL_HEADER_BYTES + 2, (uint16_t)words);
        refresh_crc(frame);
        CHECK(necp_handle_frame(&context, frame, length, response,
                                sizeof(response), &response_length) == 0);
        CHECK(response_status(response, response_length) != NEPTUNE_EDGE_STATUS_OK);
        CHECK(necp_mock_backend_commit_count(&backend) == 0);
    }
    necp_context_destroy(&context);
}

static int write_file(const char *path, const char *value)
{
    size_t length = strlen(value);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    ssize_t count;
    if (fd < 0) {
        return -errno;
    }
    count = write(fd, value, length);
    if (close(fd) != 0 && count >= 0) {
        return -errno;
    }
    return count == (ssize_t)length ? 0 : -EIO;
}

static int write_status_fixture(
    const char *path, const struct neptune_stream_abi_info *abi,
    const struct neptune_stream_pl_status *status)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    ssize_t first;
    ssize_t second;
    if (fd < 0) {
        return -errno;
    }
    first = write(fd, abi, sizeof(*abi));
    second = first == (ssize_t)sizeof(*abi)
                 ? write(fd, status, sizeof(*status))
                 : -1;
    if (close(fd) != 0 && second >= 0) {
        return -errno;
    }
    return first == (ssize_t)sizeof(*abi) &&
                   second == (ssize_t)sizeof(*status)
               ? 0
               : -EIO;
}

static void initialize_status_fixture(struct neptune_stream_abi_info *abi,
                                      struct neptune_stream_pl_status *status)
{
    memset(abi, 0, sizeof(*abi));
    memset(status, 0, sizeof(*status));
    abi->struct_size = sizeof(*abi);
    abi->abi_major = NEPTUNE_STREAM_ABI_MAJOR;
    abi->abi_minor = NEPTUNE_STREAM_ABI_MINOR;
    abi->feature_flags = NEPTUNE_STREAM_FEAT_PL_STATUS;
    status->struct_size = sizeof(*status);
    status->pl_magic = NEPTUNE_PL_MAGIC;
    status->pl_abi_version = NEPTUNE_PL_ABI_VERSION;
    status->pl_build_id = UINT64_C(0x0123456789abcdef);
    status->pl_capabilities = UINT32_C(0x000000a5);
    status->global_status =
        NEPTUNE_PL_FIELD_GLOBAL_STATUS_RX_PLL_LOCKED_MASK;
    status->sample_timestamp = UINT64_C(0x0123456789abcdef);
    status->configuration_revision = 11U;
    status->calibration_revision = 9U;
    status->discontinuity_revision = 7U;
    status->stream_status = NEPTUNE_PL_FIELD_STREAM0_STATUS_RUNNING_MASK;
    status->dma_fifo_high_water = 29U;
    status->dma_fifo_overflows = 5U;
    status->dma_descriptor_starvations = 3U;
    status->tx_safety_status =
        NEPTUNE_PL_FIELD_TX_SAFETY_STATUS_DISARMED_MASK |
        NEPTUNE_PL_FIELD_TX_SAFETY_STATUS_PERSISTENT_INHIBIT_MASK;
}

static void test_linux_backend_uses_read_only_status_node(void)
{
    static const struct {
        const char *name;
        const char *value;
    } attributes[] = {
        {"hardware_id", "0x50323130\n"},
        {"hardware_revision", "2\n"},
        {"firmware_build_id", "0x1122334455667788\n"},
        {"device_serial_hash", "0x8877665544332211\n"},
    };
    char directory[] = "/tmp/necp-linux-backend-XXXXXX";
    char path[512];
    char status_path[512];
    char inhibit_path[512];
    struct neptune_stream_abi_info abi;
    struct neptune_stream_pl_status pl_status;
    necp_linux_options options;
    necp_backend backend;
    necp_context context;
    necp_health health;
    uint8_t action[sizeof(neptune_edge_control_system_action_v1)] = {0};
    uint8_t request[NECP_MAX_FRAME_BYTES];
    uint8_t response[NECP_MAX_FRAME_BYTES];
    size_t request_length;
    size_t response_length;
    uint64_t timestamp;
    size_t index;
    CHECK(mkdtemp(directory) != NULL);
    for (index = 0; index < ARRAY_SIZE(attributes); ++index) {
        CHECK(snprintf(path, sizeof(path), "%s/%s", directory,
                       attributes[index].name) > 0);
        CHECK(write_file(path, attributes[index].value) == 0);
    }
    CHECK(snprintf(status_path, sizeof(status_path), "%s/neptune-pl-status",
                   directory) > 0);
    initialize_status_fixture(&abi, &pl_status);
    CHECK(write_status_fixture(status_path, &abi, &pl_status) == 0);
    CHECK(snprintf(inhibit_path, sizeof(inhibit_path), "%s/tx-inhibit",
                   directory) > 0);
    CHECK(write_file(inhibit_path, "0\n") == 0);
    memset(&backend, 0, sizeof(backend));
    options.sysfs_root = directory;
    options.status_path = status_path;
    options.tx_inhibit_path = inhibit_path;
    options.test_allow_regular_files = true;
    CHECK(necp_linux_backend_create(&backend, &options) == 0);
    CHECK(necp_context_init(&context, &backend) == 0);
    CHECK(context.tx_inhibited);
    CHECK(context.backend->ops->read_sample_counter(context.backend,
                                                     &timestamp) == 0);
    CHECK(timestamp == UINT64_C(0x0123456789abcdef));
    CHECK(context.backend->ops->get_health(context.backend, &health) == 0);
    CHECK(health.uptime_ns > 0U);
    CHECK(health.uptime_ns != UINT64_C(0x0123456789abcdef));
    CHECK(health.discontinuity_revision == 7U);
    CHECK(health.active_stream_mask == 1U);
    CHECK(health.fifo_high_watermark == 29U);
    CHECK(health.fifo_overflows == 5U);
    CHECK(health.dma_overruns == 3U);
    CHECK(context.identity.hardware_id == UINT32_C(0x50323130));
    CHECK(context.identity.hardware_revision == 2U);
    CHECK(context.identity.fpga_build_id == UINT64_C(0x0123456789abcdef));
    CHECK(context.identity.firmware_build_id ==
          UINT64_C(0x1122334455667788));
    CHECK(context.identity.calibration_revision == 9U);
    CHECK(context.identity.capability_bits == UINT32_C(0x000000a5));
    CHECK(context.identity.device_serial_hash ==
          UINT64_C(0x8877665544332211));
    CHECK(context.configuration_revision == 11U);
    CHECK(context.calibration_revision == 9U);

    request_length = build_request(
        request, sizeof(request), NEPTUNE_EDGE_COMMAND_GET_HEALTH,
        NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED, 40, 11, UINT64_MAX, NULL, 0);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(get32(response + 20) == 11U);
    CHECK(get64(response + 40 + 8) > 0U);
    CHECK(get32(response + 40 + 64) == 9U);
    CHECK(get32(response + 40 + 68) == 11U);

    request_length = build_request(
        request, sizeof(request), NEPTUNE_EDGE_COMMAND_GET_RF,
        NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED, 41, 11, UINT64_MAX, NULL, 0);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) ==
          NEPTUNE_EDGE_STATUS_UNSUPPORTED);

    request_length = build_request(
        request, sizeof(request), NEPTUNE_EDGE_COMMAND_GET_COUNTERS,
        NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED, 42, 11, UINT64_MAX, NULL, 0);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(get32(response + 20) == 11U);
    CHECK(get64(response + 40 + 48) == UINT64_C(0x0123456789abcdef));

    request_length = make_pipeline_request(
        request, 11, 9,
        NEPTUNE_EDGE_ROUNDING_MODE_ROUND_TO_NEAREST_EVEN);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) ==
          NEPTUNE_EDGE_STATUS_UNSUPPORTED);
    CHECK(context.configuration_revision == 11U);

    put16(action, NEPTUNE_EDGE_CONTROL_ITEM_SYSTEM_ACTION);
    put16(action + 2, sizeof(action) / 4U);
    put16(action + 8, NEPTUNE_EDGE_SYSTEM_ACTION_KIND_HARD_DISABLE_TX);
    put32(action + 12, UINT32_C(0x54584f46));
    request_length = build_request(
        request, sizeof(request), NEPTUNE_EDGE_COMMAND_HARD_DISABLE_TX,
        NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED, 43, 11, UINT64_MAX, action,
        sizeof(action));
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) == NEPTUNE_EDGE_STATUS_OK);
    CHECK(get32(response + 20) == 11U);
    CHECK(get32(response + 40 + 8) == 11U);
    CHECK(get32(response + 40 + 12) == 11U);
    CHECK(get64(response + 40 + 32) == 0U);

    request_length = make_set_rf_request(request, 11, 9, 0);
    CHECK(necp_handle_frame(&context, request, request_length, response,
                            sizeof(response), &response_length) == 0);
    CHECK(response_status(response, response_length) ==
          NEPTUNE_EDGE_STATUS_UNSUPPORTED);
    CHECK(context.configuration_revision == 11U);
    necp_context_destroy(&context);
    for (index = 0; index < ARRAY_SIZE(attributes); ++index) {
        CHECK(snprintf(path, sizeof(path), "%s/%s", directory,
                       attributes[index].name) > 0);
        CHECK(unlink(path) == 0);
    }
    CHECK(unlink(inhibit_path) == 0);
    CHECK(unlink(status_path) == 0);
    CHECK(rmdir(directory) == 0);
}

static void test_linux_backend_rejects_wrong_pl_identity(void)
{
    char directory[] = "/tmp/necp-linux-identity-XXXXXX";
    char status_path[512];
    char inhibit_path[512];
    struct neptune_stream_abi_info abi;
    struct neptune_stream_pl_status pl_status;
    necp_linux_options options;
    necp_backend backend;
    necp_context context;
    CHECK(mkdtemp(directory) != NULL);
    CHECK(snprintf(status_path, sizeof(status_path), "%s/neptune-pl-status",
                   directory) > 0);
    CHECK(snprintf(inhibit_path, sizeof(inhibit_path), "%s/tx-inhibit",
                   directory) > 0);
    initialize_status_fixture(&abi, &pl_status);
    pl_status.pl_magic ^= 1U;
    CHECK(write_status_fixture(status_path, &abi, &pl_status) == 0);
    CHECK(write_file(inhibit_path, "1\n") == 0);
    memset(&backend, 0, sizeof(backend));
    options.sysfs_root = directory;
    options.status_path = status_path;
    options.tx_inhibit_path = inhibit_path;
    options.test_allow_regular_files = true;
    CHECK(necp_linux_backend_create(&backend, &options) == 0);
    CHECK(necp_context_init(&context, &backend) == -EPROTONOSUPPORT);
    CHECK(backend.ops != NULL);
    backend.ops->destroy(&backend);
    CHECK(unlink(inhibit_path) == 0);
    CHECK(unlink(status_path) == 0);
    CHECK(rmdir(directory) == 0);
}

int main(void)
{
    void (*const tests[])(void) = {
        test_identity_and_default_safety,
        test_mock_health_uses_nanoseconds_and_authoritative_revisions,
        test_get_rf_reads_authoritative_backend_state,
        test_atomic_rf_and_matching_event,
        test_revision_conflict_has_no_side_effect,
        test_revision_wrap_is_rejected_before_backend,
        test_tx_inhibit_is_authoritative,
        test_pipeline_commit_uses_canonical_state_change_contract,
        test_invalid_pipeline_never_reaches_backend,
        test_full_rate_stream_requires_jumbo,
        test_stream_lifecycle_uses_canonical_status_contract,
        test_reset_counters_requires_system_action_and_returns_snapshot,
        test_get_counters_returns_authoritative_sample_counter,
        test_hard_disable_tx_is_timestamped_without_revision_fabrication,
        test_unknown_required_item_rejected,
        test_incremental_decoder,
        test_crc_fuzz_never_commits,
        test_semantic_length_fuzz_never_commits,
        test_linux_backend_uses_read_only_status_node,
        test_linux_backend_rejects_wrong_pl_identity,
    };
    size_t index;
    for (index = 0; index < ARRAY_SIZE(tests); ++index) {
        tests[index]();
    }
    if (failures != 0U) {
        (void)fprintf(stderr, "%u control-daemon tests failed\n", failures);
        return 1;
    }
    (void)printf("control-daemon tests: %zu PASS\n", ARRAY_SIZE(tests));
    return 0;
}
