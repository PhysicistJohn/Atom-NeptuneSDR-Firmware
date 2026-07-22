#include "necp_daemon.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#define NECP_KNOWN_REQUEST_FLAGS \
    (NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED | NEPTUNE_EDGE_CONTROL_FLAG_ATOMIC | \
     NEPTUNE_EDGE_CONTROL_FLAG_SCHEDULED)
#define NECP_KNOWN_ITEM_FLAGS NEPTUNE_EDGE_ITEM_FLAG_REQUIRED
#define NECP_SAFE_BOUNDARY_TICKS UINT64_C(64)
#define NECP_ITEM_BIT(type) (UINT32_C(1) << ((uint32_t)(type) - 1U))
#define NECP_PIPELINE_KNOWN_MASK                                           \
    (NEPTUNE_EDGE_PIPELINE_BLOCK_RAW_TAP |                                \
     NEPTUNE_EDGE_PIPELINE_BLOCK_DC_CORRECTION |                          \
     NEPTUNE_EDGE_PIPELINE_BLOCK_IQ_CORRECTION |                          \
     NEPTUNE_EDGE_PIPELINE_BLOCK_CHANNEL_CALIBRATION |                    \
     NEPTUNE_EDGE_PIPELINE_BLOCK_EQUALIZER |                              \
     NEPTUNE_EDGE_PIPELINE_BLOCK_VALIDITY |                               \
     NEPTUNE_EDGE_PIPELINE_BLOCK_NORMALIZER |                             \
     NEPTUNE_EDGE_PIPELINE_BLOCK_QUANTIZER |                              \
     NEPTUNE_EDGE_PIPELINE_BLOCK_DDC |                                    \
     NEPTUNE_EDGE_PIPELINE_BLOCK_FFT_STFT |                               \
     NEPTUNE_EDGE_PIPELINE_BLOCK_DETECTORS |                              \
     NEPTUNE_EDGE_PIPELINE_BLOCK_CROSS_CHANNEL)
#define NECP_PIPELINE_NATIVE_PRODUCTS                                      \
    ((UINT32_C(1) << NEPTUNE_EDGE_PACKET_TYPE_RAW_IQ) |                   \
     (UINT32_C(1) << NEPTUNE_EDGE_PACKET_TYPE_CALIBRATED_IQ))
#define NECP_PIPELINE_NORMALIZED_PRODUCT                                   \
    (UINT32_C(1) << NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ)

typedef struct parsed_request {
    uint8_t flags;
    uint16_t command;
    uint32_t transaction_id;
    uint32_t configuration_revision;
    uint64_t activation_timestamp;
    uint32_t item_mask;
    const neptune_edge_control_rf_config_v1 *rf;
    const neptune_edge_control_atomic_commit_v1 *atomic;
    const neptune_edge_control_pipeline_config_v1 *pipeline;
    const neptune_edge_control_stream_config_v1 *stream;
    const neptune_edge_control_system_action_v1 *system_action;
} parsed_request;

static uint16_t load_u16(const uint8_t *value)
{
    return (uint16_t)value[0] | (uint16_t)((uint16_t)value[1] << 8);
}

static uint32_t load_u32(const uint8_t *value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
           ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static uint64_t load_u64(const uint8_t *value)
{
    return (uint64_t)load_u32(value) | ((uint64_t)load_u32(value + 4) << 32);
}

static void store_u16(uint8_t *value, uint16_t number)
{
    value[0] = (uint8_t)number;
    value[1] = (uint8_t)(number >> 8);
}

static void store_u32(uint8_t *value, uint32_t number)
{
    value[0] = (uint8_t)number;
    value[1] = (uint8_t)(number >> 8);
    value[2] = (uint8_t)(number >> 16);
    value[3] = (uint8_t)(number >> 24);
}

static void store_u64(uint8_t *value, uint64_t number)
{
    store_u32(value, (uint32_t)number);
    store_u32(value + 4, (uint32_t)(number >> 32));
}

static void set_error(necp_error *error, uint16_t status, uint16_t field,
                      uint16_t detail, int32_t minimum, int32_t maximum,
                      int32_t observed)
{
    error->status = status;
    error->field_id = field;
    error->detail_code = detail;
    error->minimum = minimum;
    error->maximum = maximum;
    error->observed = observed;
}

static uint32_t header_crc(const uint8_t *header)
{
    uint8_t copy[NEPTUNE_EDGE_CONTROL_HEADER_BYTES];
    memcpy(copy, header, sizeof(copy));
    memset(copy + 36, 0, 4);
    return neptune_edge_crc32c(copy, sizeof(copy));
}

static int encode_frame(uint8_t message_kind, uint8_t flags, uint16_t command,
                        uint16_t status, uint32_t transaction_id,
                        uint32_t configuration_revision,
                        uint64_t activation_timestamp, const void *payload,
                        size_t payload_length, uint8_t *output,
                        size_t output_capacity, size_t *output_length)
{
    const size_t total = NEPTUNE_EDGE_CONTROL_HEADER_BYTES + payload_length;
    if (payload_length > NECP_MAX_PAYLOAD_BYTES || total > output_capacity) {
        return -ENOSPC;
    }
    memset(output, 0, NEPTUNE_EDGE_CONTROL_HEADER_BYTES);
    memcpy(output, "NECP", 4);
    output[4] = NEPTUNE_EDGE_PROTOCOL_VERSION;
    output[5] = (uint8_t)(NEPTUNE_EDGE_CONTROL_HEADER_BYTES / 4U);
    output[6] = message_kind;
    output[7] = flags;
    store_u16(output + 8, command);
    store_u16(output + 10, status);
    store_u32(output + 12, transaction_id);
    store_u32(output + 16, (uint32_t)payload_length);
    store_u32(output + 20, configuration_revision);
    store_u64(output + 24, activation_timestamp);
    if (payload_length != 0U) {
        memcpy(output + NEPTUNE_EDGE_CONTROL_HEADER_BYTES, payload, payload_length);
    }
    store_u32(output + 32,
              neptune_edge_crc32c(output + NEPTUNE_EDGE_CONTROL_HEADER_BYTES,
                                  payload_length));
    store_u32(output + 36, header_crc(output));
    *output_length = total;
    return 0;
}

static int encode_error(const necp_context *context,
                        const parsed_request *request, const necp_error *error,
                        uint8_t *response, size_t response_capacity,
                        size_t *response_length)
{
    uint8_t payload[sizeof(neptune_edge_control_error_detail_v1)];
    memset(payload, 0, sizeof(payload));
    store_u16(payload, NEPTUNE_EDGE_CONTROL_ITEM_ERROR_DETAIL);
    store_u16(payload + 2, (uint16_t)(sizeof(payload) / 4U));
    store_u16(payload + 8, error->field_id);
    store_u16(payload + 10, error->detail_code);
    store_u32(payload + 12, (uint32_t)error->minimum);
    store_u32(payload + 16, (uint32_t)error->maximum);
    store_u32(payload + 20, (uint32_t)error->observed);
    return encode_frame(NEPTUNE_EDGE_MESSAGE_KIND_RESPONSE,
                        NEPTUNE_EDGE_CONTROL_FLAG_ERROR_DETAIL_PRESENT,
                        request->command, error->status,
                        request->transaction_id,
                        context->configuration_revision, UINT64_MAX, payload,
                        sizeof(payload), response, response_capacity,
                        response_length);
}

static size_t expected_item_size(uint16_t type)
{
    switch (type) {
    case NEPTUNE_EDGE_CONTROL_ITEM_RF_CONFIG:
        return sizeof(neptune_edge_control_rf_config_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_ATOMIC_COMMIT:
        return sizeof(neptune_edge_control_atomic_commit_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_STREAM_CONFIG:
        return sizeof(neptune_edge_control_stream_config_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_STATE_CHANGE:
        return sizeof(neptune_edge_control_state_change_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_ERROR_DETAIL:
        return sizeof(neptune_edge_control_error_detail_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_IDENTITY:
        return sizeof(neptune_edge_control_identity_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_HEALTH:
        return sizeof(neptune_edge_control_health_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_PIPELINE_CONFIG:
        return sizeof(neptune_edge_control_pipeline_config_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_DDC_CONFIG:
        return sizeof(neptune_edge_control_ddc_config_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_FFT_CONFIG:
        return sizeof(neptune_edge_control_fft_config_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_NORMALIZATION_CONFIG:
        return sizeof(neptune_edge_control_normalization_config_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_DETECTOR_CONFIG:
        return sizeof(neptune_edge_control_detector_config_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_TRIGGER_CONFIG:
        return sizeof(neptune_edge_control_trigger_config_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_CALIBRATION_DESCRIPTOR:
        return sizeof(neptune_edge_control_calibration_descriptor_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_CALIBRATION_CHUNK:
        return sizeof(neptune_edge_control_calibration_chunk_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_CALIBRATION_SELECT:
        return sizeof(neptune_edge_control_calibration_select_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_STREAM_STATUS:
        return sizeof(neptune_edge_control_stream_status_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_COUNTER_SNAPSHOT:
        return sizeof(neptune_edge_control_counter_snapshot_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_SYSTEM_ACTION:
        return sizeof(neptune_edge_control_system_action_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_UPDATE_DESCRIPTOR:
        return sizeof(neptune_edge_control_update_descriptor_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_UPDATE_CHUNK:
        return sizeof(neptune_edge_control_update_chunk_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_TX_CONFIG:
        return sizeof(neptune_edge_control_tx_config_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_LOG_CURSOR:
        return sizeof(neptune_edge_control_log_cursor_v1);
    case NEPTUNE_EDGE_CONTROL_ITEM_SAFETY:
        return sizeof(neptune_edge_control_safety_v1);
    default:
        return 0U;
    }
}

static int parse_items(const uint8_t *payload, size_t payload_length,
                       parsed_request *request, necp_error *error)
{
    size_t offset = 0;
    uint16_t previous_type = 0;
    while (offset < payload_length) {
        uint16_t type;
        uint16_t words;
        uint32_t flags;
        size_t item_length;
        size_t expected_length;
        if (payload_length - offset < NEPTUNE_EDGE_TYPED_HEADER_BYTES) {
            set_error(error, NEPTUNE_EDGE_STATUS_INVALID_LENGTH,
                      NECP_FIELD_ITEM_LENGTH, NECP_DETAIL_OUT_OF_RANGE,
                      NEPTUNE_EDGE_TYPED_HEADER_BYTES, INT32_MAX,
                      (int32_t)(payload_length - offset));
            return -1;
        }
        type = load_u16(payload + offset);
        words = load_u16(payload + offset + 2);
        flags = load_u32(payload + offset + 4);
        item_length = (size_t)words * 4U;
        if (words < 2U || item_length > payload_length - offset) {
            set_error(error, NEPTUNE_EDGE_STATUS_INVALID_LENGTH,
                      NECP_FIELD_ITEM_LENGTH, NECP_DETAIL_OUT_OF_RANGE, 8,
                      (int32_t)(payload_length - offset),
                      item_length > INT32_MAX ? INT32_MAX : (int32_t)item_length);
            return -1;
        }
        if (type <= previous_type) {
            set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                      NECP_FIELD_ITEM_TYPE, NECP_DETAIL_DUPLICATE_OR_ORDER,
                      previous_type + 1, UINT16_MAX, type);
            return -1;
        }
        if ((flags & ~NECP_KNOWN_ITEM_FLAGS) != 0U) {
            set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                      NECP_FIELD_FLAGS, NECP_DETAIL_UNSUPPORTED_BIT, 0,
                      NECP_KNOWN_ITEM_FLAGS, (int32_t)flags);
            return -1;
        }
        expected_length = expected_item_size(type);
        if (expected_length != 0U && item_length != expected_length) {
            set_error(error, NEPTUNE_EDGE_STATUS_INVALID_LENGTH,
                      NECP_FIELD_ITEM_LENGTH, NECP_DETAIL_MISMATCH,
                      (int32_t)expected_length, (int32_t)expected_length,
                      item_length > INT32_MAX ? INT32_MAX
                                              : (int32_t)item_length);
            return -1;
        }
        if (expected_length != 0U) {
            request->item_mask |= NECP_ITEM_BIT(type);
        }
        switch (type) {
        case NEPTUNE_EDGE_CONTROL_ITEM_RF_CONFIG:
            request->rf = (const neptune_edge_control_rf_config_v1 *)(payload + offset);
            break;
        case NEPTUNE_EDGE_CONTROL_ITEM_ATOMIC_COMMIT:
            request->atomic = (const neptune_edge_control_atomic_commit_v1 *)(payload + offset);
            break;
        case NEPTUNE_EDGE_CONTROL_ITEM_PIPELINE_CONFIG:
            request->pipeline =
                (const neptune_edge_control_pipeline_config_v1 *)(payload + offset);
            break;
        case NEPTUNE_EDGE_CONTROL_ITEM_STREAM_CONFIG:
            request->stream = (const neptune_edge_control_stream_config_v1 *)(payload + offset);
            break;
        case NEPTUNE_EDGE_CONTROL_ITEM_SYSTEM_ACTION:
            request->system_action =
                (const neptune_edge_control_system_action_v1 *)(payload + offset);
            break;
        default:
            if (expected_length == 0U &&
                (flags & NEPTUNE_EDGE_ITEM_FLAG_REQUIRED) != 0U) {
                set_error(error, NEPTUNE_EDGE_STATUS_UNSUPPORTED,
                          NECP_FIELD_ITEM_TYPE, NECP_DETAIL_REQUIRED_UNKNOWN, 0,
                          UINT16_MAX, type);
                return -1;
            }
            break;
        }
        previous_type = type;
        offset += item_length;
    }
    return 0;
}

static int parse_request(const uint8_t *frame, size_t frame_length,
                         parsed_request *request, necp_error *error)
{
    uint32_t payload_length;
    memset(request, 0, sizeof(*request));
    request->activation_timestamp = UINT64_MAX;
    if (frame_length < NEPTUNE_EDGE_CONTROL_HEADER_BYTES) {
        return -1;
    }
    request->flags = frame[7];
    request->command = load_u16(frame + 8);
    request->transaction_id = load_u32(frame + 12);
    request->configuration_revision = load_u32(frame + 20);
    request->activation_timestamp = load_u64(frame + 24);
    if (memcmp(frame, "NECP", 4) != 0) {
        return -1;
    }
    if (frame[4] != NEPTUNE_EDGE_PROTOCOL_VERSION) {
        set_error(error, NEPTUNE_EDGE_STATUS_UNSUPPORTED, NECP_FIELD_HEADER,
                  NECP_DETAIL_MISMATCH, NEPTUNE_EDGE_PROTOCOL_VERSION,
                  NEPTUNE_EDGE_PROTOCOL_VERSION, frame[4]);
        return 1;
    }
    if (frame[5] != NEPTUNE_EDGE_CONTROL_HEADER_BYTES / 4U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_LENGTH, NECP_FIELD_HEADER,
                  NECP_DETAIL_MISMATCH,
                  NEPTUNE_EDGE_CONTROL_HEADER_BYTES / 4U,
                  NEPTUNE_EDGE_CONTROL_HEADER_BYTES / 4U, frame[5]);
        return 1;
    }
    if (frame[6] != NEPTUNE_EDGE_MESSAGE_KIND_REQUEST ||
        load_u16(frame + 10) != NEPTUNE_EDGE_STATUS_OK) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE, NECP_FIELD_HEADER,
                  NECP_DETAIL_MISMATCH, NEPTUNE_EDGE_MESSAGE_KIND_REQUEST,
                  NEPTUNE_EDGE_MESSAGE_KIND_REQUEST, frame[6]);
        return 1;
    }
    if ((request->flags & ~NECP_KNOWN_REQUEST_FLAGS) != 0U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE, NECP_FIELD_FLAGS,
                  NECP_DETAIL_UNSUPPORTED_BIT, 0, NECP_KNOWN_REQUEST_FLAGS,
                  request->flags);
        return 1;
    }
    if (request->transaction_id == 0U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_TRANSACTION_ID, NECP_DETAIL_OUT_OF_RANGE, 1,
                  INT32_MAX, 0);
        return 1;
    }
    payload_length = load_u32(frame + 16);
    if (payload_length > NECP_MAX_PAYLOAD_BYTES ||
        frame_length != NEPTUNE_EDGE_CONTROL_HEADER_BYTES + (size_t)payload_length) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_LENGTH,
                  NECP_FIELD_PAYLOAD, NECP_DETAIL_MISMATCH, 0,
                  NECP_MAX_PAYLOAD_BYTES,
                  payload_length > INT32_MAX ? INT32_MAX : (int32_t)payload_length);
        return 1;
    }
    if (header_crc(frame) != load_u32(frame + 36) ||
        neptune_edge_crc32c(frame + NEPTUNE_EDGE_CONTROL_HEADER_BYTES,
                            payload_length) != load_u32(frame + 32)) {
        set_error(error, NEPTUNE_EDGE_STATUS_CRC_ERROR, NECP_FIELD_HEADER,
                  NECP_DETAIL_MISMATCH, 0, 0, 1);
        return 1;
    }
    if (parse_items(frame + NEPTUNE_EDGE_CONTROL_HEADER_BYTES, payload_length,
                    request, error) != 0) {
        return 1;
    }
    return 0;
}

static int validate_no_items(const parsed_request *request, necp_error *error)
{
    if (request->item_mask != 0U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE, NECP_FIELD_PAYLOAD,
                  NECP_DETAIL_MISMATCH, 0, 0, (int32_t)request->item_mask);
        return -1;
    }
    return 0;
}

static int validate_item_mask(const parsed_request *request, uint32_t expected,
                              necp_error *error)
{
    if (request->item_mask != expected) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_ITEM_TYPE, NECP_DETAIL_MISMATCH,
                  (int32_t)expected, (int32_t)expected,
                  (int32_t)request->item_mask);
        return -1;
    }
    return 0;
}

static int validate_revision(const necp_context *context,
                             const parsed_request *request,
                             const neptune_edge_control_atomic_commit_v1 *atomic,
                             necp_error *error)
{
    const uint32_t expected_config = load_u32((const uint8_t *)atomic + 8);
    const uint32_t expected_calibration = load_u32((const uint8_t *)atomic + 12);
    if (context->configuration_revision == UINT32_MAX) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_CONFIGURATION_REVISION,
                  NECP_DETAIL_OUT_OF_RANGE, 0, INT32_MAX, INT32_MAX);
        return -1;
    }
    if (request->configuration_revision != context->configuration_revision ||
        expected_config != context->configuration_revision) {
        set_error(error, NEPTUNE_EDGE_STATUS_REVISION_CONFLICT,
                  NECP_FIELD_CONFIGURATION_REVISION, NECP_DETAIL_MISMATCH,
                  (int32_t)context->configuration_revision,
                  (int32_t)context->configuration_revision,
                  (int32_t)expected_config);
        return -1;
    }
    if (expected_calibration != context->calibration_revision) {
        set_error(error, NEPTUNE_EDGE_STATUS_REVISION_CONFLICT,
                  NECP_FIELD_CALIBRATION_REVISION, NECP_DETAIL_MISMATCH,
                  (int32_t)context->calibration_revision,
                  (int32_t)context->calibration_revision,
                  (int32_t)expected_calibration);
        return -1;
    }
    return 0;
}

static int select_activation(necp_context *context,
                             const parsed_request *request,
                             const neptune_edge_control_atomic_commit_v1 *atomic,
                             uint64_t *activation, necp_error *error)
{
    uint64_t now;
    uint64_t requested;
    if ((request->flags & NEPTUNE_EDGE_CONTROL_FLAG_ATOMIC) == 0U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE, NECP_FIELD_FLAGS,
                  NECP_DETAIL_MISSING, NEPTUNE_EDGE_CONTROL_FLAG_ATOMIC,
                  NEPTUNE_EDGE_CONTROL_FLAG_ATOMIC, request->flags);
        return -1;
    }
    if (atomic == NULL) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE, NECP_FIELD_ITEM_TYPE,
                  NECP_DETAIL_MISSING,
                  NEPTUNE_EDGE_CONTROL_ITEM_ATOMIC_COMMIT,
                  NEPTUNE_EDGE_CONTROL_ITEM_ATOMIC_COMMIT, 0);
        return -1;
    }
    requested = load_u64((const uint8_t *)atomic + 16);
    if (requested != request->activation_timestamp) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_ACTIVATION_TIMESTAMP, NECP_DETAIL_MISMATCH, 0,
                  INT32_MAX, -1);
        return -1;
    }
    if (context->backend->ops->read_sample_counter(context->backend, &now) != 0) {
        set_error(error, NEPTUNE_EDGE_STATUS_INTERNAL_ERROR,
                  NECP_FIELD_BACKEND, NECP_DETAIL_BACKEND_REJECTED, 0, 0, 0);
        return -1;
    }
    if (requested == UINT64_MAX) {
        const uint64_t remainder = now % NECP_SAFE_BOUNDARY_TICKS;
        *activation = now + (NECP_SAFE_BOUNDARY_TICKS - remainder);
        if (*activation <= now) {
            *activation = now + NECP_SAFE_BOUNDARY_TICKS;
        }
    } else {
        if ((request->flags & NEPTUNE_EDGE_CONTROL_FLAG_SCHEDULED) == 0U ||
            requested <= now) {
            set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                      NECP_FIELD_ACTIVATION_TIMESTAMP, NECP_DETAIL_OUT_OF_RANGE,
                      now >= INT32_MAX ? INT32_MAX : (int32_t)(now + 1),
                      INT32_MAX,
                      requested > INT32_MAX ? INT32_MAX : (int32_t)requested);
            return -1;
        }
        *activation = requested;
    }
    return validate_revision(context, request, atomic, error);
}

static int validate_rf(const necp_context *context,
                       const neptune_edge_control_rf_config_v1 *rf,
                       necp_error *error)
{
    const uint8_t *bytes = (const uint8_t *)rf;
    const uint64_t frequency = load_u64(bytes + 8);
    const uint32_t internal_rate = load_u32(bytes + 16);
    const uint32_t egress_rate = load_u32(bytes + 20);
    const uint32_t bandwidth = load_u32(bytes + 24);
    const int32_t gain1 = (int32_t)load_u32(bytes + 28);
    const int32_t gain2 = (int32_t)load_u32(bytes + 32);
    const uint16_t channels = load_u16(bytes + 36);
    const uint32_t expected = load_u32(bytes + 40);
    const uint32_t rf_flags = load_u32(bytes + 44);
    if (frequency < UINT64_C(70000000) || frequency > UINT64_C(6000000000)) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_CENTER_FREQUENCY, NECP_DETAIL_OUT_OF_RANGE,
                  70000000, INT32_MAX,
                  frequency > INT32_MAX ? INT32_MAX : (int32_t)frequency);
        return -1;
    }
    if (internal_rate != NEPTUNE_EDGE_INTERNAL_SAMPLE_RATE_HZ ||
        egress_rate != NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_SAMPLE_RATE, NECP_DETAIL_MISMATCH,
                  (int32_t)NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ,
                  (int32_t)NEPTUNE_EDGE_INTERNAL_SAMPLE_RATE_HZ,
                  (int32_t)egress_rate);
        return -1;
    }
    if (bandwidth < UINT32_C(200000) || bandwidth > UINT32_C(56000000)) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_RF_BANDWIDTH, NECP_DETAIL_OUT_OF_RANGE, 200000,
                  56000000, (int32_t)bandwidth);
        return -1;
    }
    if (gain1 < -10000 || gain1 > 73000 || gain2 < -10000 || gain2 > 73000) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE, NECP_FIELD_GAIN,
                  NECP_DETAIL_OUT_OF_RANGE, -10000, 73000,
                  gain1 < -10000 || gain1 > 73000 ? gain1 : gain2);
        return -1;
    }
    if (channels == 0U || channels > 3U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_CHANNEL_MASK, NECP_DETAIL_OUT_OF_RANGE, 1, 3,
                  channels);
        return -1;
    }
    if (bytes[38] > NEPTUNE_EDGE_GAIN_MODE_HYBRID ||
        bytes[39] > NEPTUNE_EDGE_GAIN_MODE_HYBRID) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_GAIN_MODE, NECP_DETAIL_OUT_OF_RANGE, 0,
                  NEPTUNE_EDGE_GAIN_MODE_HYBRID,
                  bytes[38] > NEPTUNE_EDGE_GAIN_MODE_HYBRID ? bytes[38]
                                                            : bytes[39]);
        return -1;
    }
    if (expected != context->configuration_revision) {
        set_error(error, NEPTUNE_EDGE_STATUS_REVISION_CONFLICT,
                  NECP_FIELD_CONFIGURATION_REVISION, NECP_DETAIL_MISMATCH,
                  (int32_t)context->configuration_revision,
                  (int32_t)context->configuration_revision,
                  (int32_t)expected);
        return -1;
    }
    if ((rf_flags & ~NECP_RF_FLAG_TX_ENABLE) != 0U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE, NECP_FIELD_FLAGS,
                  NECP_DETAIL_UNSUPPORTED_BIT, 0, NECP_RF_FLAG_TX_ENABLE,
                  (int32_t)rf_flags);
        return -1;
    }
    if ((rf_flags & NECP_RF_FLAG_TX_ENABLE) != 0U && context->tx_inhibited) {
        set_error(error, NEPTUNE_EDGE_STATUS_AUTHORIZATION_REQUIRED,
                  NECP_FIELD_TX_ENABLE, NECP_DETAIL_TX_INHIBITED, 0, 0, 1);
        return -1;
    }
    return 0;
}

static int validate_pipeline(
    const necp_context *context,
    const neptune_edge_control_pipeline_config_v1 *pipeline,
    necp_error *error)
{
    const uint8_t *bytes = (const uint8_t *)pipeline;
    const uint32_t enable_mask = load_u32(bytes + 8);
    const uint32_t bypass_mask = load_u32(bytes + 12);
    const uint32_t products = load_u32(bytes + 16);
    const uint32_t allowed_products =
        NECP_PIPELINE_NATIVE_PRODUCTS | NECP_PIPELINE_NORMALIZED_PRODUCT;
    const uint8_t format = bytes[20];
    const uint32_t expected_configuration = load_u32(bytes + 28);
    const uint32_t calibration_revision = load_u32(bytes + 32);
    if (((enable_mask | bypass_mask) & ~NECP_PIPELINE_KNOWN_MASK) != 0U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE, NECP_FIELD_FLAGS,
                  NECP_DETAIL_UNSUPPORTED_BIT, 0, NECP_PIPELINE_KNOWN_MASK,
                  (int32_t)(enable_mask | bypass_mask));
        return -1;
    }
    if (products == 0U || (products & ~allowed_products) != 0U ||
        ((products & NECP_PIPELINE_NORMALIZED_PRODUCT) != 0U &&
         (products & NECP_PIPELINE_NATIVE_PRODUCTS) != 0U)) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_PAYLOAD, NECP_DETAIL_UNSUPPORTED_BIT, 1,
                  (int32_t)allowed_products, (int32_t)products);
        return -1;
    }
    if ((products & NECP_PIPELINE_NORMALIZED_PRODUCT) != 0U) {
        if (format != NEPTUNE_EDGE_SAMPLE_FORMAT_S8 &&
            format != NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF) {
            set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                      NECP_FIELD_SAMPLE_FORMAT, NECP_DETAIL_MISMATCH,
                      NEPTUNE_EDGE_SAMPLE_FORMAT_S8,
                      NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF, format);
            return -1;
        }
    } else if (format != NEPTUNE_EDGE_SAMPLE_FORMAT_S16 &&
               format != NEPTUNE_EDGE_SAMPLE_FORMAT_S12P) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_SAMPLE_FORMAT, NECP_DETAIL_MISMATCH,
                  NEPTUNE_EDGE_SAMPLE_FORMAT_S16,
                  NEPTUNE_EDGE_SAMPLE_FORMAT_S12P, format);
        return -1;
    }
    if (bytes[21] > NEPTUNE_EDGE_NORMALIZATION_STRATEGY_RMS) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_PAYLOAD, NECP_DETAIL_OUT_OF_RANGE, 0,
                  NEPTUNE_EDGE_NORMALIZATION_STRATEGY_RMS, bytes[21]);
        return -1;
    }
    if (bytes[22] > 7U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_PAYLOAD, NECP_DETAIL_OUT_OF_RANGE, 0, 7,
                  bytes[22]);
        return -1;
    }
    if (bytes[23] != NEPTUNE_EDGE_ROUNDING_MODE_ROUND_TO_NEAREST_EVEN) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_PAYLOAD, NECP_DETAIL_MISMATCH,
                  NEPTUNE_EDGE_ROUNDING_MODE_ROUND_TO_NEAREST_EVEN,
                  NEPTUNE_EDGE_ROUNDING_MODE_ROUND_TO_NEAREST_EVEN,
                  bytes[23]);
        return -1;
    }
    if (bytes[24] > 31U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_PAYLOAD, NECP_DETAIL_OUT_OF_RANGE, 0, 31,
                  bytes[24]);
        return -1;
    }
    if (bytes[25] != 0U || load_u16(bytes + 26) != 0U ||
        load_u32(bytes + 40) != 0U || load_u32(bytes + 44) != 0U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_PAYLOAD, NECP_DETAIL_RESERVED_NONZERO, 0, 0, 1);
        return -1;
    }
    if (expected_configuration != context->configuration_revision) {
        set_error(error, NEPTUNE_EDGE_STATUS_REVISION_CONFLICT,
                  NECP_FIELD_CONFIGURATION_REVISION, NECP_DETAIL_MISMATCH,
                  (int32_t)context->configuration_revision,
                  (int32_t)context->configuration_revision,
                  (int32_t)expected_configuration);
        return -1;
    }
    if (calibration_revision != context->calibration_revision) {
        set_error(error, NEPTUNE_EDGE_STATUS_REVISION_CONFLICT,
                  NECP_FIELD_CALIBRATION_REVISION, NECP_DETAIL_MISMATCH,
                  (int32_t)context->calibration_revision,
                  (int32_t)context->calibration_revision,
                  (int32_t)calibration_revision);
        return -1;
    }
    return 0;
}

static unsigned sample_stride(uint8_t format, uint8_t channels)
{
    unsigned per_channel;
    unsigned count = (channels == 3U) ? 2U : 1U;
    switch (format) {
    case NEPTUNE_EDGE_SAMPLE_FORMAT_S16:
        per_channel = 4U;
        break;
    case NEPTUNE_EDGE_SAMPLE_FORMAT_S12P:
        per_channel = 3U;
        break;
    case NEPTUNE_EDGE_SAMPLE_FORMAT_S8:
    case NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF:
        per_channel = 2U;
        break;
    default:
        return 0U;
    }
    return per_channel * count;
}

static int validate_stream_config(const neptune_edge_control_stream_config_v1 *stream,
                                  necp_error *error)
{
    const uint8_t *bytes = (const uint8_t *)stream;
    const uint32_t id = load_u32(bytes + 8);
    const uint8_t first = bytes[12];
    const uint16_t port = load_u16(bytes + 16);
    const uint16_t mtu = load_u16(bytes + 18);
    const uint32_t products = load_u32(bytes + 20);
    const uint8_t format = bytes[24];
    const uint8_t channels = bytes[25];
    const uint32_t samples = load_u32(bytes + 28);
    const unsigned stride = sample_stride(format, channels);
    uint32_t normal_extension_bytes;
    uint32_t maximum_extension_bytes;
    uint32_t sample_rate;
    uint32_t maximum_samples;
    uint64_t packet_rate;
    uint64_t udp_payload_bytes;
    uint64_t ethernet_wire_bytes;
    uint64_t wire_bits_per_second;
    if (id == 0U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_STREAM_ID, NECP_DETAIL_OUT_OF_RANGE, 1, INT32_MAX,
                  0);
        return -1;
    }
    if (first == 0U || first == 127U || first >= 224U ||
        (bytes[12] == 255U && bytes[13] == 255U && bytes[14] == 255U &&
         bytes[15] == 255U) || port == 0U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_DESTINATION, NECP_DETAIL_OUT_OF_RANGE, 1,
                  UINT16_MAX, port);
        return -1;
    }
    if (mtu != 1500U && mtu != 9000U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE, NECP_FIELD_MTU,
                  NECP_DETAIL_MISMATCH, 1500, 9000, mtu);
        return -1;
    }
    if ((products != (UINT32_C(1) << NEPTUNE_EDGE_PACKET_TYPE_RAW_IQ) &&
         products != (UINT32_C(1) << NEPTUNE_EDGE_PACKET_TYPE_CALIBRATED_IQ) &&
         products != (UINT32_C(1) << NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ)) ||
        stride == 0U || channels == 0U || channels > 3U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  stride == 0U ? NECP_FIELD_SAMPLE_FORMAT
                               : NECP_FIELD_CHANNEL_MASK,
                  NECP_DETAIL_OUT_OF_RANGE, 1, 4,
                  stride == 0U ? format : channels);
        return -1;
    }
    if (products == (UINT32_C(1) << NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ)) {
        if (format != NEPTUNE_EDGE_SAMPLE_FORMAT_S8 &&
            format != NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF) {
            set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                      NECP_FIELD_SAMPLE_FORMAT, NECP_DETAIL_MISMATCH,
                      NEPTUNE_EDGE_SAMPLE_FORMAT_S8,
                      NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF, format);
            return -1;
        }
        sample_rate = NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ;
        normal_extension_bytes =
            (uint32_t)sizeof(neptune_edge_data_rf_state_v1) +
            (uint32_t)sizeof(neptune_edge_data_resampler_state_v1) +
            (uint32_t)sizeof(neptune_edge_data_block_stats_v1) +
            (format == NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF
                 ? (uint32_t)sizeof(neptune_edge_data_quantization_v1)
                 : 0U) +
            (uint32_t)sizeof(neptune_edge_data_payload_crc_v1);
    } else {
        if (format != NEPTUNE_EDGE_SAMPLE_FORMAT_S16 &&
            format != NEPTUNE_EDGE_SAMPLE_FORMAT_S12P) {
            set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                      NECP_FIELD_SAMPLE_FORMAT, NECP_DETAIL_MISMATCH,
                      NEPTUNE_EDGE_SAMPLE_FORMAT_S16,
                      NEPTUNE_EDGE_SAMPLE_FORMAT_S12P, format);
            return -1;
        }
        sample_rate = NEPTUNE_EDGE_INTERNAL_SAMPLE_RATE_HZ;
        normal_extension_bytes =
            (uint32_t)sizeof(neptune_edge_data_rf_state_v1) +
            (uint32_t)sizeof(neptune_edge_data_block_stats_v1) +
            (uint32_t)sizeof(neptune_edge_data_payload_crc_v1);
    }
    maximum_extension_bytes =
        normal_extension_bytes +
        (uint32_t)sizeof(neptune_edge_data_discontinuity_v1);
    if (load_u16(bytes + 26) != 0U || load_u32(bytes + 36) != 0U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_PAYLOAD, NECP_DETAIL_RESERVED_NONZERO, 0, 0, 1);
        return -1;
    }
    maximum_samples = ((uint32_t)mtu - 28U -
                       NEPTUNE_EDGE_DATA_HEADER_BYTES -
                       maximum_extension_bytes) /
                      stride;
    if (samples == 0U || samples > maximum_samples) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_PAYLOAD, NECP_DETAIL_OUT_OF_RANGE, 1,
                  (int32_t)maximum_samples,
                  samples > INT32_MAX ? INT32_MAX : (int32_t)samples);
        return -1;
    }
    udp_payload_bytes = NEPTUNE_EDGE_DATA_HEADER_BYTES +
                        normal_extension_bytes + (uint64_t)samples * stride;
    if (udp_payload_bytes + 28U > mtu) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE, NECP_FIELD_MTU,
                  NECP_DETAIL_OUT_OF_RANGE, (int32_t)(udp_payload_bytes + 28U),
                  9000, mtu);
        return -1;
    }
    packet_rate = (sample_rate + (uint64_t)samples - 1U) /
                  samples;
    /* Per packet: 8-byte preamble/SFD, 14-byte MAC header, IPv4+UDP,
     * NEDP bytes, 4-byte FCS, and 12-byte inter-packet gap. */
    ethernet_wire_bytes = UINT64_C(66) + udp_payload_bytes;
    wire_bits_per_second = packet_rate * ethernet_wire_bytes * UINT64_C(8);
    if (wire_bits_per_second > UINT64_C(1000000000)) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_SAMPLE_RATE, NECP_DETAIL_OUT_OF_RANGE, 0,
                  1000000000, INT32_MAX);
        return -1;
    }
    return 0;
}

static int backend_error(necp_error *error, int result)
{
    uint16_t status = NEPTUNE_EDGE_STATUS_INTERNAL_ERROR;
    if (result == -EBUSY) {
        status = NEPTUNE_EDGE_STATUS_BUSY;
    } else if (result == -ENOTSUP || result == -EOPNOTSUPP || result == -ENOSYS) {
        status = NEPTUNE_EDGE_STATUS_UNSUPPORTED;
    } else if (result == -EACCES || result == -EPERM) {
        status = NEPTUNE_EDGE_STATUS_AUTHORIZATION_REQUIRED;
    }
    set_error(error, status, NECP_FIELD_BACKEND, NECP_DETAIL_BACKEND_REJECTED,
              0, 0, -result);
    return -1;
}

static int refresh_health(necp_context *context, necp_health *health)
{
    int result = context->backend->ops->get_health(context->backend, health);
    if (result != 0) {
        *health = context->last_health;
        health->status_flags &= ~NECP_HEALTH_FLAG_BACKEND_READY;
        return result;
    }
    context->last_health = *health;
    context->configuration_revision = health->configuration_revision;
    context->calibration_revision = health->calibration_revision;
    context->identity.calibration_revision = health->calibration_revision;
    return 0;
}

static int validate_backend_rf(const necp_context *context,
                               const neptune_edge_control_rf_config_v1 *config)
{
    const uint8_t *bytes = (const uint8_t *)config;
    necp_error ignored_error;
    memset(&ignored_error, 0, sizeof(ignored_error));
    if (load_u16(bytes) != NEPTUNE_EDGE_CONTROL_ITEM_RF_CONFIG ||
        load_u16(bytes + 2) != sizeof(*config) / 4U ||
        load_u32(bytes + 4) != 0U ||
        validate_rf(context, config, &ignored_error) != 0) {
        return -EPROTO;
    }
    return 0;
}

static void hash_from_hex(const char *hex, uint8_t output[32])
{
    unsigned index;
    for (index = 0; index < 32U; ++index) {
        unsigned high = (unsigned)(hex[index * 2U] <= '9'
                                       ? hex[index * 2U] - '0'
                                       : hex[index * 2U] - 'a' + 10);
        unsigned low = (unsigned)(hex[index * 2U + 1U] <= '9'
                                      ? hex[index * 2U + 1U] - '0'
                                      : hex[index * 2U + 1U] - 'a' + 10);
        output[index] = (uint8_t)((high << 4) | low);
    }
}

static size_t encode_identity_payload(const necp_context *context,
                                      uint8_t *payload)
{
    neptune_edge_control_identity_v1 *item =
        (neptune_edge_control_identity_v1 *)payload;
    memset(item, 0, sizeof(*item));
    store_u16((uint8_t *)item, NEPTUNE_EDGE_CONTROL_ITEM_IDENTITY);
    store_u16((uint8_t *)item + 2, (uint16_t)(sizeof(*item) / 4U));
    store_u32((uint8_t *)item + 8, context->identity.hardware_id);
    store_u32((uint8_t *)item + 12, context->identity.hardware_revision);
    store_u64((uint8_t *)item + 16, context->identity.fpga_build_id);
    store_u64((uint8_t *)item + 24, context->identity.firmware_build_id);
    hash_from_hex(NEPTUNE_EDGE_DATA_SPEC_SHA256, item->protocol_data_sha256);
    hash_from_hex(NEPTUNE_EDGE_CONTROL_SPEC_SHA256,
                  item->protocol_control_sha256);
    store_u32((uint8_t *)item + 96, context->calibration_revision);
    store_u32((uint8_t *)item + 100, context->identity.capability_bits);
    store_u64((uint8_t *)item + 104, context->identity.device_serial_hash);
    return sizeof(*item);
}

static size_t encode_safety_payload(const necp_context *context,
                                    uint8_t *payload)
{
    neptune_edge_control_safety_v1 *safety =
        (neptune_edge_control_safety_v1 *)payload;
    memset(safety, 0, sizeof(*safety));
    store_u16((uint8_t *)safety, NEPTUNE_EDGE_CONTROL_ITEM_SAFETY);
    store_u16((uint8_t *)safety + 2, (uint16_t)(sizeof(*safety) / 4U));
    safety->tx_enabled = context->tx_enabled ? 1U : 0U;
    safety->tx_inhibited = context->tx_inhibited ? 1U : 0U;
    safety->tx_default_off = 1U;
    safety->qspi_write_allowed = 0U;
    return sizeof(*safety);
}

static size_t encode_identity_and_safety_payload(const necp_context *context,
                                                 uint8_t *payload)
{
    size_t length = encode_identity_payload(context, payload);
    length += encode_safety_payload(context, payload + length);
    return length;
}

static size_t encode_health_payload(necp_context *context, uint8_t *payload)
{
    necp_health health;
    neptune_edge_control_health_v1 *item =
        (neptune_edge_control_health_v1 *)payload;
    uint8_t active_mask = 0U;
    unsigned index;
    (void)refresh_health(context, &health);
    for (index = 0; index < NECP_MAX_STREAMS; ++index) {
        if (context->streams[index].running) {
            active_mask |= (uint8_t)(1U << index);
        }
    }
    active_mask |= health.active_stream_mask;
    memset(item, 0, sizeof(*item));
    store_u16((uint8_t *)item, NEPTUNE_EDGE_CONTROL_ITEM_HEALTH);
    store_u16((uint8_t *)item + 2, (uint16_t)(sizeof(*item) / 4U));
    store_u64((uint8_t *)item + 8, health.uptime_ns);
    store_u32((uint8_t *)item + 16, (uint32_t)health.temperature_mc);
    store_u32((uint8_t *)item + 20, health.status_flags);
    store_u32((uint8_t *)item + 24, health.fault_flags);
    ((uint8_t *)item)[28] = health.ethernet_link_state;
    ((uint8_t *)item)[29] = health.usb_state;
    ((uint8_t *)item)[30] = health.pll_lock_mask;
    ((uint8_t *)item)[31] = active_mask;
    store_u32((uint8_t *)item + 32, health.fifo_overflows);
    store_u32((uint8_t *)item + 36, health.dma_overruns);
    store_u32((uint8_t *)item + 40, health.dropped_packets);
    store_u32((uint8_t *)item + 44,
              health.discontinuity_revision > context->discontinuity_revision
                  ? health.discontinuity_revision
                  : context->discontinuity_revision);
    store_u32((uint8_t *)item + 48, health.clipping_count);
    store_u32((uint8_t *)item + 52, health.watchdog_reset_count);
    store_u32((uint8_t *)item + 56, health.supply_mv);
    store_u32((uint8_t *)item + 60, health.supply_ma);
    store_u32((uint8_t *)item + 64, health.calibration_revision);
    store_u32((uint8_t *)item + 68, health.configuration_revision);
    return sizeof(*item);
}

static size_t encode_health_and_safety_payload(necp_context *context,
                                               uint8_t *payload)
{
    size_t length = encode_health_payload(context, payload);
    length += encode_safety_payload(context, payload + length);
    return length;
}

static int encode_counter_snapshot_payload(necp_context *context,
                                           uint8_t *payload,
                                           size_t *payload_length)
{
    necp_health health;
    uint64_t sample_counter = 0U;
    neptune_edge_control_counter_snapshot_v1 *item =
        (neptune_edge_control_counter_snapshot_v1 *)payload;
    int result = refresh_health(context, &health);
    if (result != 0) {
        return result;
    }
    result = context->backend->ops->read_sample_counter(context->backend,
                                                        &sample_counter);
    if (result != 0) {
        return result;
    }
    memset(item, 0, sizeof(*item));
    store_u16((uint8_t *)item, NEPTUNE_EDGE_CONTROL_ITEM_COUNTER_SNAPSHOT);
    store_u16((uint8_t *)item + 2, (uint16_t)(sizeof(*item) / 4U));
    store_u32((uint8_t *)item + 8, health.fifo_high_watermark);
    store_u32((uint8_t *)item + 12, health.fifo_overflows);
    store_u32((uint8_t *)item + 16, health.dma_overruns);
    store_u32((uint8_t *)item + 28, health.dropped_packets);
    store_u32((uint8_t *)item + 36,
              health.discontinuity_revision > context->discontinuity_revision
                  ? health.discontinuity_revision
                  : context->discontinuity_revision);
    store_u64((uint8_t *)item + 40, health.clipping_count);
    store_u64((uint8_t *)item + 48, sample_counter);
    *payload_length = sizeof(*item);
    return 0;
}

static size_t encode_rf_payload(const necp_context *context, uint8_t *payload)
{
    memcpy(payload, &context->rf_config, sizeof(context->rf_config));
    return sizeof(context->rf_config);
}

static int prepare_authoritative_rf_payload(necp_context *context,
                                            uint8_t *payload,
                                            size_t *payload_length,
                                            necp_error *error)
{
    necp_health health;
    neptune_edge_control_rf_config_v1 config;
    int result = refresh_health(context, &health);
    if (result != 0) {
        return backend_error(error, result);
    }
    result = context->backend->ops->get_rf(context->backend, &config);
    if (result != 0) {
        return backend_error(error, result);
    }
    result = validate_backend_rf(context, &config);
    if (result != 0) {
        return backend_error(error, result);
    }
    context->rf_config = config;
    context->tx_enabled =
        (load_u32((const uint8_t *)&config + 44) & NECP_RF_FLAG_TX_ENABLE) != 0U;
    *payload_length = encode_rf_payload(context, payload);
    return 0;
}

static size_t encode_state_change_payload(uint32_t previous_config,
                                          uint32_t new_config,
                                          uint32_t previous_calibration,
                                          uint32_t new_calibration,
                                          uint64_t activation,
                                          uint64_t changed_fields,
                                          uint8_t *payload)
{
    memset(payload, 0, sizeof(neptune_edge_control_state_change_v1));
    store_u16(payload, NEPTUNE_EDGE_CONTROL_ITEM_STATE_CHANGE);
    store_u16(payload + 2,
              (uint16_t)(sizeof(neptune_edge_control_state_change_v1) / 4U));
    store_u32(payload + 8, previous_config);
    store_u32(payload + 12, new_config);
    store_u32(payload + 16, previous_calibration);
    store_u32(payload + 20, new_calibration);
    store_u64(payload + 24, activation);
    store_u64(payload + 32, changed_fields);
    return sizeof(neptune_edge_control_state_change_v1);
}

static void queue_state_event(necp_context *context, uint32_t previous_config,
                              uint32_t previous_calibration,
                              uint64_t activation, uint64_t changed_fields)
{
    uint8_t payload[sizeof(neptune_edge_control_state_change_v1)];
    necp_event_slot *slot;
    size_t length = encode_state_change_payload(
        previous_config, context->configuration_revision,
        previous_calibration, context->calibration_revision, activation,
        changed_fields, payload);
    if (context->event_count == NECP_EVENT_QUEUE_DEPTH) {
        context->event_read = (context->event_read + 1U) % NECP_EVENT_QUEUE_DEPTH;
        --context->event_count;
    }
    slot = &context->event_queue[context->event_write];
    if (encode_frame(NEPTUNE_EDGE_MESSAGE_KIND_EVENT, 0,
                     NEPTUNE_EDGE_COMMAND_STATE_CHANGE_EVENT,
                     NEPTUNE_EDGE_STATUS_OK, 0,
                     context->configuration_revision, activation, payload,
                     length, slot->bytes, sizeof(slot->bytes), &slot->length) == 0) {
        context->event_write =
            (context->event_write + 1U) % NECP_EVENT_QUEUE_DEPTH;
        ++context->event_count;
    }
}

static int finish_state_change(necp_context *context,
                               const parsed_request *request,
                               uint32_t previous_config,
                               uint32_t previous_calibration,
                               uint64_t activation, uint64_t changed_fields,
                               uint8_t *response, size_t response_capacity,
                               size_t *response_length)
{
    uint8_t payload[sizeof(neptune_edge_control_state_change_v1)];
    const size_t payload_length = encode_state_change_payload(
        previous_config, context->configuration_revision,
        previous_calibration, context->calibration_revision, activation,
        changed_fields, payload);
    queue_state_event(context, previous_config, previous_calibration, activation,
                      changed_fields);
    return encode_frame(NEPTUNE_EDGE_MESSAGE_KIND_RESPONSE, 0, request->command,
                        NEPTUNE_EDGE_STATUS_OK, request->transaction_id,
                        context->configuration_revision, activation, payload,
                        payload_length, response, response_capacity,
                        response_length);
}

static int handle_set_rf(necp_context *context, const parsed_request *request,
                         uint8_t *response, size_t response_capacity,
                         size_t *response_length, necp_error *error)
{
    uint64_t activation;
    uint64_t changed_fields;
    uint32_t previous;
    int result;
    bool inhibited;
    if (validate_item_mask(
            request,
            NECP_ITEM_BIT(NEPTUNE_EDGE_CONTROL_ITEM_RF_CONFIG) |
                NECP_ITEM_BIT(NEPTUNE_EDGE_CONTROL_ITEM_ATOMIC_COMMIT),
            error) != 0) {
        return -1;
    }
    if (select_activation(context, request, request->atomic, &activation,
                          error) != 0) {
        return -1;
    }
    if (context->backend->ops->tx_is_inhibited(context->backend, &inhibited) != 0) {
        return backend_error(error, -EIO);
    }
    context->tx_inhibited = inhibited;
    if (validate_rf(context, request->rf, error) != 0) {
        return -1;
    }
    changed_fields = load_u64((const uint8_t *)request->atomic + 24);
    if (changed_fields == 0U ||
        (changed_fields & ~((uint64_t)NEPTUNE_EDGE_CHANGED_FIELD_CENTER_FREQUENCY |
                            (uint64_t)NEPTUNE_EDGE_CHANGED_FIELD_SAMPLE_RATE |
                            (uint64_t)NEPTUNE_EDGE_CHANGED_FIELD_RF_BANDWIDTH |
                            (uint64_t)NEPTUNE_EDGE_CHANGED_FIELD_ANALOG_GAIN |
                            (uint64_t)NEPTUNE_EDGE_CHANGED_FIELD_FILTER)) != 0U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_CHANGED_FIELDS, NECP_DETAIL_UNSUPPORTED_BIT, 1,
                  INT32_MAX, (int32_t)changed_fields);
        return -1;
    }
    result = context->backend->ops->commit_rf(context->backend, request->rf,
                                               activation);
    if (result != 0) {
        return backend_error(error, result);
    }
    previous = context->configuration_revision;
    memcpy(&context->rf_config, request->rf, sizeof(context->rf_config));
    context->tx_enabled =
        (load_u32((const uint8_t *)request->rf + 44) & NECP_RF_FLAG_TX_ENABLE) != 0U;
    ++context->configuration_revision;
    store_u32((uint8_t *)&context->rf_config + 40,
              context->configuration_revision);
    return finish_state_change(context, request, previous,
                               context->calibration_revision, activation,
                               changed_fields, response, response_capacity,
                               response_length);
}

static int handle_configure_pipeline(
    necp_context *context, const parsed_request *request, uint8_t *response,
    size_t response_capacity, size_t *response_length, necp_error *error)
{
    const uint64_t allowed_changed_fields =
        (uint64_t)NEPTUNE_EDGE_CHANGED_FIELD_FILTER |
        (uint64_t)NEPTUNE_EDGE_CHANGED_FIELD_CALIBRATION |
        (uint64_t)NEPTUNE_EDGE_CHANGED_FIELD_QUANTIZATION |
        (uint64_t)NEPTUNE_EDGE_CHANGED_FIELD_DDC |
        (uint64_t)NEPTUNE_EDGE_CHANGED_FIELD_DETECTOR;
    uint64_t activation;
    uint64_t changed_fields;
    uint32_t previous;
    int result;
    if (validate_item_mask(
            request,
            NECP_ITEM_BIT(NEPTUNE_EDGE_CONTROL_ITEM_ATOMIC_COMMIT) |
                NECP_ITEM_BIT(NEPTUNE_EDGE_CONTROL_ITEM_PIPELINE_CONFIG),
            error) != 0 ||
        select_activation(context, request, request->atomic, &activation,
                          error) != 0 ||
        validate_pipeline(context, request->pipeline, error) != 0) {
        return -1;
    }
    changed_fields = load_u64((const uint8_t *)request->atomic + 24);
    if (changed_fields == 0U ||
        (changed_fields & ~allowed_changed_fields) != 0U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_CHANGED_FIELDS, NECP_DETAIL_UNSUPPORTED_BIT, 1,
                  (int32_t)allowed_changed_fields, (int32_t)changed_fields);
        return -1;
    }
    result = context->backend->ops->commit_pipeline(
        context->backend, request->pipeline, changed_fields, activation);
    if (result != 0) {
        return backend_error(error, result);
    }
    previous = context->configuration_revision;
    ++context->configuration_revision;
    return finish_state_change(context, request, previous,
                               context->calibration_revision, activation,
                               changed_fields, response, response_capacity,
                               response_length);
}

static int stream_slot(const necp_context *context, uint32_t id)
{
    unsigned index;
    for (index = 0; index < NECP_MAX_STREAMS; ++index) {
        if (context->streams[index].created &&
            load_u32((const uint8_t *)&context->streams[index].config + 8) == id) {
            return (int)index;
        }
    }
    return -1;
}

static int free_stream_slot(const necp_context *context)
{
    unsigned index;
    for (index = 0; index < NECP_MAX_STREAMS; ++index) {
        if (!context->streams[index].created) {
            return (int)index;
        }
    }
    return -1;
}

static size_t encode_stream_status_payload(uint32_t stream_id,
                                           uint32_t stream_state,
                                           uint64_t sample_timestamp,
                                           const necp_stream *stream,
                                           uint8_t *payload)
{
    uint64_t packets = stream != NULL ? stream->packets : 0U;
    uint64_t drops = stream != NULL ? stream->drops : 0U;
    memset(payload, 0, sizeof(neptune_edge_control_stream_status_v1));
    store_u16(payload, NEPTUNE_EDGE_CONTROL_ITEM_STREAM_STATUS);
    store_u16(payload + 2,
              (uint16_t)(sizeof(neptune_edge_control_stream_status_v1) / 4U));
    store_u32(payload + 8, stream_id);
    store_u32(payload + 12, stream_state);
    store_u64(payload + 16, packets);
    store_u64(payload + 24, sample_timestamp);
    store_u64(payload + 32, packets);
    store_u32(payload + 40,
              drops > UINT32_MAX ? UINT32_MAX : (uint32_t)drops);
    return sizeof(neptune_edge_control_stream_status_v1);
}

static int finish_stream_state_change(necp_context *context,
                                      const parsed_request *request,
                                      uint32_t previous_config,
                                      uint64_t activation,
                                      uint64_t changed_fields,
                                      uint32_t stream_state,
                                      const necp_stream *stream,
                                      uint8_t *response,
                                      size_t response_capacity,
                                      size_t *response_length)
{
    uint8_t payload[sizeof(neptune_edge_control_state_change_v1) +
                    sizeof(neptune_edge_control_stream_status_v1)];
    uint32_t stream_id = load_u32((const uint8_t *)request->stream + 8);
    size_t length = encode_state_change_payload(
        previous_config, context->configuration_revision,
        context->calibration_revision, context->calibration_revision,
        activation, changed_fields, payload);
    length += encode_stream_status_payload(stream_id, stream_state, activation,
                                           stream, payload + length);
    queue_state_event(context, previous_config, context->calibration_revision,
                      activation, changed_fields);
    return encode_frame(NEPTUNE_EDGE_MESSAGE_KIND_RESPONSE, 0,
                        request->command, NEPTUNE_EDGE_STATUS_OK,
                        request->transaction_id,
                        context->configuration_revision, activation, payload,
                        length, response, response_capacity, response_length);
}

static int validate_non_atomic_stream_request(const necp_context *context,
                                              const parsed_request *request,
                                              necp_error *error)
{
    if ((request->flags & (NEPTUNE_EDGE_CONTROL_FLAG_ATOMIC |
                           NEPTUNE_EDGE_CONTROL_FLAG_SCHEDULED)) != 0U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE, NECP_FIELD_FLAGS,
                  NECP_DETAIL_UNSUPPORTED_BIT, 0,
                  NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED, request->flags);
        return -1;
    }
    if (request->activation_timestamp != UINT64_MAX) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_ACTIVATION_TIMESTAMP, NECP_DETAIL_MISMATCH, -1,
                  -1, -1);
        return -1;
    }
    if (request->configuration_revision != context->configuration_revision) {
        set_error(error, NEPTUNE_EDGE_STATUS_REVISION_CONFLICT,
                  NECP_FIELD_CONFIGURATION_REVISION, NECP_DETAIL_MISMATCH,
                  (int32_t)context->configuration_revision,
                  (int32_t)context->configuration_revision,
                  (int32_t)request->configuration_revision);
        return -1;
    }
    return 0;
}

static int handle_stream(necp_context *context, const parsed_request *request,
                         uint8_t *response, size_t response_capacity,
                         size_t *response_length, necp_error *error)
{
    uint64_t activation = UINT64_MAX;
    uint64_t changed_fields = 0U;
    uint32_t id;
    uint32_t previous;
    uint32_t state;
    int slot;
    int result;
    bool atomic = request->command == NEPTUNE_EDGE_COMMAND_START_STREAM ||
                  request->command == NEPTUNE_EDGE_COMMAND_STOP_STREAM;
    if (validate_item_mask(
            request,
            NECP_ITEM_BIT(NEPTUNE_EDGE_CONTROL_ITEM_STREAM_CONFIG) |
                (atomic
                     ? NECP_ITEM_BIT(NEPTUNE_EDGE_CONTROL_ITEM_ATOMIC_COMMIT)
                     : 0U),
            error) != 0 ||
        validate_stream_config(request->stream, error) != 0) {
        return -1;
    }
    if (atomic) {
        if (select_activation(context, request, request->atomic, &activation,
                              error) != 0) {
            return -1;
        }
        changed_fields = load_u64((const uint8_t *)request->atomic + 24);
        if (changed_fields !=
            NEPTUNE_EDGE_CHANGED_FIELD_STREAM_DESTINATION) {
            set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                      NECP_FIELD_CHANGED_FIELDS, NECP_DETAIL_MISMATCH,
                      NEPTUNE_EDGE_CHANGED_FIELD_STREAM_DESTINATION,
                      NEPTUNE_EDGE_CHANGED_FIELD_STREAM_DESTINATION,
                      (int32_t)changed_fields);
            return -1;
        }
    } else if (validate_non_atomic_stream_request(context, request, error) !=
               0) {
        return -1;
    }
    id = load_u32((const uint8_t *)request->stream + 8);
    slot = stream_slot(context, id);
    if (request->command == NEPTUNE_EDGE_COMMAND_CREATE_STREAM) {
        if (slot >= 0) {
            set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                      NECP_FIELD_STREAM_ID, NECP_DETAIL_STREAM_EXISTS, 0, 0,
                      (int32_t)id);
            return -1;
        }
        slot = free_stream_slot(context);
        if (slot < 0) {
            set_error(error, NEPTUNE_EDGE_STATUS_BUSY, NECP_FIELD_STREAM_ID,
                      NECP_DETAIL_OUT_OF_RANGE, 0, NECP_MAX_STREAMS, 0);
            return -1;
        }
    } else if (slot < 0) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_STREAM_ID, NECP_DETAIL_STREAM_NOT_FOUND, 0, 0,
                  (int32_t)id);
        return -1;
    } else if (request->command == NEPTUNE_EDGE_COMMAND_START_STREAM &&
               context->streams[slot].running) {
        set_error(error, NEPTUNE_EDGE_STATUS_BUSY, NECP_FIELD_STREAM_ID,
                  NECP_DETAIL_STREAM_STATE, 0, 0, (int32_t)id);
        return -1;
    } else if ((request->command == NEPTUNE_EDGE_COMMAND_STOP_STREAM ||
                request->command == NEPTUNE_EDGE_COMMAND_DESTROY_STREAM) &&
               !context->streams[slot].running &&
               request->command == NEPTUNE_EDGE_COMMAND_STOP_STREAM) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_STREAM_ID, NECP_DETAIL_STREAM_STATE, 0, 0,
                  (int32_t)id);
        return -1;
    } else if (request->command == NEPTUNE_EDGE_COMMAND_DESTROY_STREAM &&
               context->streams[slot].running) {
        set_error(error, NEPTUNE_EDGE_STATUS_BUSY, NECP_FIELD_STREAM_ID,
                  NECP_DETAIL_STREAM_STATE, 0, 0, (int32_t)id);
        return -1;
    }
    if (slot >= 0 && request->command != NEPTUNE_EDGE_COMMAND_CREATE_STREAM &&
        memcmp((const uint8_t *)request->stream + 8,
               (const uint8_t *)&context->streams[slot].config + 8,
               sizeof(context->streams[slot].config) - 8U) != 0) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_STREAM_ID, NECP_DETAIL_MISMATCH, (int32_t)id,
                  (int32_t)id, (int32_t)id);
        return -1;
    }
    if (!atomic &&
        context->backend->ops->read_sample_counter(context->backend,
                                                    &activation) != 0) {
        set_error(error, NEPTUNE_EDGE_STATUS_INTERNAL_ERROR,
                  NECP_FIELD_BACKEND, NECP_DETAIL_BACKEND_REJECTED, 0, 0, 0);
        return -1;
    }
    result = context->backend->ops->stream_update(
        context->backend, request->command, request->stream, activation);
    if (result != 0) {
        return backend_error(error, result);
    }
    if (request->command == NEPTUNE_EDGE_COMMAND_CREATE_STREAM) {
        memset(&context->streams[slot], 0, sizeof(context->streams[slot]));
        context->streams[slot].created = true;
        memcpy(&context->streams[slot].config, request->stream,
               sizeof(context->streams[slot].config));
        state = NEPTUNE_EDGE_STREAM_STATE_CREATED;
    } else if (request->command == NEPTUNE_EDGE_COMMAND_START_STREAM) {
        context->streams[slot].running = true;
        state = NEPTUNE_EDGE_STREAM_STATE_RUNNING;
    } else if (request->command == NEPTUNE_EDGE_COMMAND_STOP_STREAM) {
        context->streams[slot].running = false;
        state = NEPTUNE_EDGE_STREAM_STATE_CREATED;
    } else {
        memset(&context->streams[slot], 0, sizeof(context->streams[slot]));
        state = NEPTUNE_EDGE_STREAM_STATE_NONE;
    }
    if (!atomic) {
        uint8_t payload[sizeof(neptune_edge_control_stream_status_v1)];
        const size_t payload_length = encode_stream_status_payload(
            id, state, activation,
            state == NEPTUNE_EDGE_STREAM_STATE_NONE ? NULL
                                                     : &context->streams[slot],
            payload);
        return encode_frame(
            NEPTUNE_EDGE_MESSAGE_KIND_RESPONSE, 0, request->command,
            NEPTUNE_EDGE_STATUS_OK, request->transaction_id,
            context->configuration_revision, UINT64_MAX, payload,
            payload_length, response, response_capacity, response_length);
    }
    previous = context->configuration_revision;
    ++context->configuration_revision;
    return finish_stream_state_change(
        context, request, previous, activation, changed_fields, state,
        &context->streams[slot], response, response_capacity, response_length);
}

static int validate_system_action_request(const necp_context *context,
                                          const parsed_request *request,
                                          uint16_t expected_kind,
                                          necp_error *error)
{
    const uint8_t *action;
    if (validate_item_mask(
            request, NECP_ITEM_BIT(NEPTUNE_EDGE_CONTROL_ITEM_SYSTEM_ACTION),
            error) != 0) {
        return -1;
    }
    if ((request->flags & (NEPTUNE_EDGE_CONTROL_FLAG_ATOMIC |
                           NEPTUNE_EDGE_CONTROL_FLAG_SCHEDULED)) != 0U ||
        request->activation_timestamp != UINT64_MAX) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE, NECP_FIELD_FLAGS,
                  NECP_DETAIL_UNSUPPORTED_BIT, 0,
                  NEPTUNE_EDGE_CONTROL_FLAG_ACK_REQUIRED, request->flags);
        return -1;
    }
    if (request->configuration_revision != context->configuration_revision) {
        set_error(error, NEPTUNE_EDGE_STATUS_REVISION_CONFLICT,
                  NECP_FIELD_CONFIGURATION_REVISION, NECP_DETAIL_MISMATCH,
                  (int32_t)context->configuration_revision,
                  (int32_t)context->configuration_revision,
                  (int32_t)request->configuration_revision);
        return -1;
    }
    action = (const uint8_t *)request->system_action;
    if (load_u16(action + 8) != expected_kind ||
        load_u16(action + 10) != 0U || load_u64(action + 16) != 0U) {
        set_error(error, NEPTUNE_EDGE_STATUS_INVALID_VALUE,
                  NECP_FIELD_PAYLOAD, NECP_DETAIL_MISMATCH, expected_kind,
                  expected_kind, load_u16(action + 8));
        return -1;
    }
    return 0;
}

static int handle_get_counters(necp_context *context,
                               const parsed_request *request,
                               uint8_t *response, size_t response_capacity,
                               size_t *response_length, necp_error *error)
{
    uint8_t payload[sizeof(neptune_edge_control_counter_snapshot_v1)];
    size_t payload_length;
    int result;
    if (validate_no_items(request, error) != 0) {
        return -1;
    }
    result = encode_counter_snapshot_payload(context, payload,
                                             &payload_length);
    if (result != 0) {
        return backend_error(error, result);
    }
    return encode_frame(
        NEPTUNE_EDGE_MESSAGE_KIND_RESPONSE, 0, request->command,
        NEPTUNE_EDGE_STATUS_OK, request->transaction_id,
        context->configuration_revision, UINT64_MAX, payload, payload_length,
        response, response_capacity, response_length);
}

static int handle_hard_disable_tx(necp_context *context,
                                  const parsed_request *request,
                                  uint8_t *response,
                                  size_t response_capacity,
                                  size_t *response_length,
                                  necp_error *error)
{
    uint64_t activation;
    bool inhibited;
    int result;
    if (validate_system_action_request(
            context, request,
            NEPTUNE_EDGE_SYSTEM_ACTION_KIND_HARD_DISABLE_TX, error) != 0) {
        return -1;
    }
    result = context->backend->ops->force_tx_off(context->backend);
    if (result != 0) {
        return backend_error(error, result);
    }
    context->tx_enabled = false;
    result = context->backend->ops->read_sample_counter(context->backend,
                                                        &activation);
    if (result != 0) {
        return backend_error(error, result);
    }
    result = context->backend->ops->tx_is_inhibited(context->backend,
                                                    &inhibited);
    if (result != 0) {
        return backend_error(error, result);
    }
    context->tx_inhibited = inhibited;
    return finish_state_change(
        context, request, context->configuration_revision,
        context->calibration_revision, activation, 0U, response,
        response_capacity, response_length);
}

static int handle_reset_counters(necp_context *context,
                                 const parsed_request *request,
                                 uint8_t *response,
                                 size_t response_capacity,
                                 size_t *response_length, necp_error *error)
{
    uint8_t payload[sizeof(neptune_edge_control_counter_snapshot_v1)];
    size_t payload_length;
    int result;
    if (validate_system_action_request(
            context, request, NEPTUNE_EDGE_SYSTEM_ACTION_KIND_RESET_COUNTERS,
            error) != 0) {
        return -1;
    }
    result = context->backend->ops->reset_counters(context->backend);
    if (result != 0) {
        return backend_error(error, result);
    }
    result = encode_counter_snapshot_payload(context, payload,
                                             &payload_length);
    if (result != 0) {
        return backend_error(error, result);
    }
    return encode_frame(
        NEPTUNE_EDGE_MESSAGE_KIND_RESPONSE, 0, request->command,
        NEPTUNE_EDGE_STATUS_OK, request->transaction_id,
        context->configuration_revision, UINT64_MAX, payload, payload_length,
        response, response_capacity, response_length);
}

int necp_context_init(necp_context *context, necp_backend *backend)
{
    necp_health health;
    neptune_edge_control_rf_config_v1 rf_config;
    int result;
    bool inhibited;
    if (context == NULL || backend == NULL || backend->ops == NULL ||
        backend->ops->initialize == NULL || backend->ops->destroy == NULL ||
        backend->ops->get_identity == NULL || backend->ops->get_health == NULL ||
        backend->ops->get_rf == NULL ||
        backend->ops->read_sample_counter == NULL ||
        backend->ops->commit_rf == NULL ||
        backend->ops->commit_pipeline == NULL ||
        backend->ops->stream_update == NULL ||
        backend->ops->reset_counters == NULL ||
        backend->ops->force_tx_off == NULL ||
        backend->ops->tx_is_inhibited == NULL) {
        return -EINVAL;
    }
    memset(context, 0, sizeof(*context));
    context->backend = backend;
    result = backend->ops->initialize(backend);
    if (result != 0) {
        return result;
    }
    result = backend->ops->force_tx_off(backend);
    if (result != 0) {
        backend->ops->destroy(backend);
        return result;
    }
    result = backend->ops->tx_is_inhibited(backend, &inhibited);
    if (result != 0) {
        backend->ops->destroy(backend);
        return result;
    }
    context->tx_enabled = false;
    context->tx_inhibited = inhibited;
    result = backend->ops->get_identity(backend, &context->identity);
    if (result != 0) {
        backend->ops->destroy(backend);
        return result;
    }
    result = refresh_health(context, &health);
    if (result != 0) {
        backend->ops->destroy(backend);
        return result;
    }
    context->discontinuity_revision = health.discontinuity_revision;
    result = backend->ops->get_rf(backend, &rf_config);
    if (result == 0) {
        result = validate_backend_rf(context, &rf_config);
        if (result != 0) {
            backend->ops->destroy(backend);
            return result;
        }
        context->rf_config = rf_config;
    } else if (result != -ENOTSUP && result != -EOPNOTSUPP &&
               result != -ENOSYS) {
        backend->ops->destroy(backend);
        return result;
    }
    return 0;
}

void necp_context_destroy(necp_context *context)
{
    if (context != NULL && context->backend != NULL &&
        context->backend->ops != NULL && context->backend->ops->destroy != NULL) {
        (void)context->backend->ops->force_tx_off(context->backend);
        context->backend->ops->destroy(context->backend);
    }
    if (context != NULL) {
        memset(context, 0, sizeof(*context));
    }
}

int necp_handle_frame(necp_context *context, const uint8_t *request_bytes,
                      size_t request_length, uint8_t *response,
                      size_t response_capacity, size_t *response_length)
{
    parsed_request request;
    necp_error error;
    uint8_t payload[NECP_MAX_PAYLOAD_BYTES];
    size_t payload_length = 0;
    int parsed;
    int result = -1;
    if (context == NULL || request_bytes == NULL || response == NULL ||
        response_length == NULL) {
        return -EINVAL;
    }
    *response_length = 0;
    memset(&error, 0, sizeof(error));
    parsed = parse_request(request_bytes, request_length, &request, &error);
    if (parsed < 0) {
        return -EBADMSG;
    }
    if (parsed > 0) {
        return encode_error(context, &request, &error, response,
                            response_capacity, response_length);
    }
    switch (request.command) {
    case NEPTUNE_EDGE_COMMAND_HELLO:
    case NEPTUNE_EDGE_COMMAND_GET_IDENTITY:
        if (validate_no_items(&request, &error) == 0) {
            payload_length =
                encode_identity_and_safety_payload(context, payload);
        }
        break;
    case NEPTUNE_EDGE_COMMAND_GET_HEALTH:
        if (validate_no_items(&request, &error) == 0) {
            payload_length =
                encode_health_and_safety_payload(context, payload);
        }
        break;
    case NEPTUNE_EDGE_COMMAND_GET_RF:
        if (validate_no_items(&request, &error) == 0) {
            result = prepare_authoritative_rf_payload(
                context, payload, &payload_length, &error);
        }
        break;
    case NEPTUNE_EDGE_COMMAND_SET_RF:
        result = handle_set_rf(context, &request, response, response_capacity,
                               response_length, &error);
        if (result == 0) {
            return 0;
        }
        break;
    case NEPTUNE_EDGE_COMMAND_CREATE_STREAM:
    case NEPTUNE_EDGE_COMMAND_DESTROY_STREAM:
    case NEPTUNE_EDGE_COMMAND_START_STREAM:
    case NEPTUNE_EDGE_COMMAND_STOP_STREAM:
        result = handle_stream(context, &request, response, response_capacity,
                               response_length, &error);
        if (result == 0) {
            return 0;
        }
        break;
    case NEPTUNE_EDGE_COMMAND_RESET_COUNTERS:
        result = handle_reset_counters(context, &request, response,
                                       response_capacity, response_length,
                                       &error);
        if (result == 0) {
            return 0;
        }
        break;
    case NEPTUNE_EDGE_COMMAND_CONFIGURE_PIPELINE:
        result = handle_configure_pipeline(
            context, &request, response, response_capacity, response_length,
            &error);
        if (result == 0) {
            return 0;
        }
        break;
    case NEPTUNE_EDGE_COMMAND_GET_COUNTERS:
        result = handle_get_counters(context, &request, response,
                                     response_capacity, response_length,
                                     &error);
        if (result == 0) {
            return 0;
        }
        break;
    case NEPTUNE_EDGE_COMMAND_HARD_DISABLE_TX:
        result = handle_hard_disable_tx(context, &request, response,
                                        response_capacity, response_length,
                                        &error);
        if (result == 0) {
            return 0;
        }
        break;
    case NEPTUNE_EDGE_COMMAND_CONFIGURE_DDC:
    case NEPTUNE_EDGE_COMMAND_CONFIGURE_FFT:
    case NEPTUNE_EDGE_COMMAND_CONFIGURE_STFT:
    case NEPTUNE_EDGE_COMMAND_CONFIGURE_NORMALIZATION:
    case NEPTUNE_EDGE_COMMAND_CONFIGURE_DETECTOR:
    case NEPTUNE_EDGE_COMMAND_LOAD_CALIBRATION:
    case NEPTUNE_EDGE_COMMAND_SELECT_CALIBRATION:
    case NEPTUNE_EDGE_COMMAND_CONFIGURE_TRIGGER:
    case NEPTUNE_EDGE_COMMAND_SOFTWARE_TRIGGER:
    case NEPTUNE_EDGE_COMMAND_READ_LOGS:
    case NEPTUNE_EDGE_COMMAND_REBOOT:
    case NEPTUNE_EDGE_COMMAND_ENTER_RECOVERY:
    case NEPTUNE_EDGE_COMMAND_CONFIGURE_TX:
    case NEPTUNE_EDGE_COMMAND_BEGIN_UPDATE:
    case NEPTUNE_EDGE_COMMAND_WRITE_UPDATE_CHUNK:
    case NEPTUNE_EDGE_COMMAND_COMMIT_UPDATE:
        set_error(&error, NEPTUNE_EDGE_STATUS_UNSUPPORTED,
                  NECP_FIELD_ITEM_TYPE, NECP_DETAIL_BACKEND_REJECTED, 0, 0,
                  request.command);
        break;
    default:
        set_error(&error, NEPTUNE_EDGE_STATUS_INVALID_COMMAND,
                  NECP_FIELD_HEADER, NECP_DETAIL_OUT_OF_RANGE, 1,
                  NEPTUNE_EDGE_COMMAND_ENTER_RECOVERY, request.command);
        break;
    }
    if (error.status != 0U) {
        return encode_error(context, &request, &error, response,
                            response_capacity, response_length);
    }
    return encode_frame(NEPTUNE_EDGE_MESSAGE_KIND_RESPONSE, 0, request.command,
                        NEPTUNE_EDGE_STATUS_OK, request.transaction_id,
                        context->configuration_revision, UINT64_MAX, payload,
                        payload_length, response, response_capacity,
                        response_length);
}

int necp_pop_event(necp_context *context, uint8_t *event,
                   size_t event_capacity, size_t *event_length)
{
    necp_event_slot *slot;
    if (context == NULL || event == NULL || event_length == NULL) {
        return -EINVAL;
    }
    if (context->event_count == 0U) {
        *event_length = 0;
        return 0;
    }
    slot = &context->event_queue[context->event_read];
    if (slot->length > event_capacity) {
        return -ENOSPC;
    }
    memcpy(event, slot->bytes, slot->length);
    *event_length = slot->length;
    context->event_read = (context->event_read + 1U) % NECP_EVENT_QUEUE_DEPTH;
    --context->event_count;
    return 1;
}

void necp_decoder_init(necp_decoder *decoder)
{
    memset(decoder, 0, sizeof(*decoder));
    decoder->expected = NEPTUNE_EDGE_CONTROL_HEADER_BYTES;
}

int necp_decoder_feed(necp_decoder *decoder, const uint8_t *bytes,
                      size_t length, necp_frame_callback callback,
                      void *opaque, size_t *frames_delivered)
{
    size_t delivered = 0;
    if (decoder == NULL || (bytes == NULL && length != 0U) || callback == NULL) {
        return -EINVAL;
    }
    while (length != 0U) {
        size_t needed = decoder->expected - decoder->used;
        size_t take = length < needed ? length : needed;
        memcpy(decoder->bytes + decoder->used, bytes, take);
        decoder->used += take;
        bytes += take;
        length -= take;
        if (decoder->used != decoder->expected) {
            continue;
        }
        if (decoder->expected == NEPTUNE_EDGE_CONTROL_HEADER_BYTES) {
            uint32_t payload_length;
            if (memcmp(decoder->bytes, "NECP", 4) != 0) {
                necp_decoder_init(decoder);
                return -EBADMSG;
            }
            payload_length = load_u32(decoder->bytes + 16);
            if (payload_length > NECP_MAX_PAYLOAD_BYTES) {
                necp_decoder_init(decoder);
                return -EMSGSIZE;
            }
            decoder->expected =
                NEPTUNE_EDGE_CONTROL_HEADER_BYTES + (size_t)payload_length;
            if (payload_length != 0U) {
                continue;
            }
        }
        if (callback(opaque, decoder->bytes, decoder->expected) != 0) {
            necp_decoder_init(decoder);
            return -ECANCELED;
        }
        ++delivered;
        necp_decoder_init(decoder);
    }
    if (frames_delivered != NULL) {
        *frames_delivered = delivered;
    }
    return 0;
}
