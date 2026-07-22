#ifndef NECP_DAEMON_H
#define NECP_DAEMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "neptune_edge_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NECP_MAX_PAYLOAD_BYTES UINT32_C(4096)
#define NECP_MAX_FRAME_BYTES (NEPTUNE_EDGE_CONTROL_HEADER_BYTES + NECP_MAX_PAYLOAD_BYTES)
#define NECP_MAX_STREAMS 8U
#define NECP_EVENT_QUEUE_DEPTH 8U

#define NECP_RF_FLAG_TX_ENABLE UINT32_C(1)

#define NECP_HEALTH_FLAG_RF_PLL_LOCKED UINT32_C(1)
#define NECP_HEALTH_FLAG_ETHERNET_LINK UINT32_C(2)
#define NECP_HEALTH_FLAG_USB_CONFIGURED UINT32_C(4)
#define NECP_HEALTH_FLAG_BACKEND_READY UINT32_C(8)

enum necp_field_id {
    NECP_FIELD_NONE = 0,
    NECP_FIELD_HEADER = 1,
    NECP_FIELD_FLAGS = 2,
    NECP_FIELD_TRANSACTION_ID = 3,
    NECP_FIELD_PAYLOAD = 4,
    NECP_FIELD_ITEM_TYPE = 5,
    NECP_FIELD_ITEM_LENGTH = 6,
    NECP_FIELD_CONFIGURATION_REVISION = 7,
    NECP_FIELD_CALIBRATION_REVISION = 8,
    NECP_FIELD_ACTIVATION_TIMESTAMP = 9,
    NECP_FIELD_CENTER_FREQUENCY = 10,
    NECP_FIELD_SAMPLE_RATE = 11,
    NECP_FIELD_RF_BANDWIDTH = 12,
    NECP_FIELD_GAIN = 13,
    NECP_FIELD_CHANNEL_MASK = 14,
    NECP_FIELD_GAIN_MODE = 15,
    NECP_FIELD_TX_ENABLE = 16,
    NECP_FIELD_STREAM_ID = 17,
    NECP_FIELD_DESTINATION = 18,
    NECP_FIELD_MTU = 19,
    NECP_FIELD_SAMPLE_FORMAT = 20,
    NECP_FIELD_CHANGED_FIELDS = 21,
    NECP_FIELD_BACKEND = 22,
};

enum necp_detail_code {
    NECP_DETAIL_NONE = 0,
    NECP_DETAIL_MISSING = 1,
    NECP_DETAIL_DUPLICATE_OR_ORDER = 2,
    NECP_DETAIL_RESERVED_NONZERO = 3,
    NECP_DETAIL_OUT_OF_RANGE = 4,
    NECP_DETAIL_MISMATCH = 5,
    NECP_DETAIL_UNSUPPORTED_BIT = 6,
    NECP_DETAIL_REQUIRED_UNKNOWN = 7,
    NECP_DETAIL_TX_INHIBITED = 8,
    NECP_DETAIL_BACKEND_REJECTED = 9,
    NECP_DETAIL_STREAM_EXISTS = 10,
    NECP_DETAIL_STREAM_NOT_FOUND = 11,
    NECP_DETAIL_STREAM_STATE = 12,
};

typedef struct necp_identity {
    uint32_t hardware_id;
    uint32_t hardware_revision;
    uint64_t fpga_build_id;
    uint64_t firmware_build_id;
    uint32_t calibration_revision;
    uint32_t capability_bits;
    uint64_t device_serial_hash;
} necp_identity;

typedef struct necp_health {
    uint64_t uptime_ns;
    int32_t temperature_mc;
    uint32_t status_flags;
    uint32_t fault_flags;
    uint8_t ethernet_link_state;
    uint8_t usb_state;
    uint8_t pll_lock_mask;
    uint8_t active_stream_mask;
    uint32_t fifo_high_watermark;
    uint32_t fifo_overflows;
    uint32_t dma_overruns;
    uint32_t dropped_packets;
    uint32_t discontinuity_revision;
    uint32_t clipping_count;
    uint32_t watchdog_reset_count;
    uint32_t supply_mv;
    uint32_t supply_ma;
    uint32_t calibration_revision;
    uint32_t configuration_revision;
} necp_health;

typedef struct necp_stream {
    bool created;
    bool running;
    neptune_edge_control_stream_config_v1 config;
    uint64_t packets;
    uint64_t drops;
} necp_stream;

typedef struct necp_backend necp_backend;

typedef struct necp_backend_ops {
    int (*initialize)(necp_backend *backend);
    void (*destroy)(necp_backend *backend);
    int (*get_identity)(necp_backend *backend, necp_identity *identity);
    int (*get_health)(necp_backend *backend, necp_health *health);
    int (*get_rf)(necp_backend *backend,
                  neptune_edge_control_rf_config_v1 *config);
    int (*read_sample_counter)(necp_backend *backend, uint64_t *timestamp);
    int (*commit_rf)(necp_backend *backend,
                     const neptune_edge_control_rf_config_v1 *config,
                     uint64_t activation_timestamp);
    int (*commit_pipeline)(
        necp_backend *backend,
        const neptune_edge_control_pipeline_config_v1 *config,
        uint64_t changed_fields, uint64_t activation_timestamp);
    int (*stream_update)(necp_backend *backend, uint16_t command,
                         const neptune_edge_control_stream_config_v1 *config,
                         uint64_t activation_timestamp);
    int (*reset_counters)(necp_backend *backend);
    int (*force_tx_off)(necp_backend *backend);
    int (*tx_is_inhibited)(necp_backend *backend, bool *inhibited);
} necp_backend_ops;

struct necp_backend {
    const necp_backend_ops *ops;
    void *state;
};

typedef struct necp_error {
    uint16_t status;
    uint16_t field_id;
    uint16_t detail_code;
    int32_t minimum;
    int32_t maximum;
    int32_t observed;
} necp_error;

typedef struct necp_event_slot {
    uint8_t bytes[NECP_MAX_FRAME_BYTES];
    size_t length;
} necp_event_slot;

typedef struct necp_context {
    necp_backend *backend;
    necp_identity identity;
    necp_health last_health;
    neptune_edge_control_rf_config_v1 rf_config;
    necp_stream streams[NECP_MAX_STREAMS];
    uint32_t configuration_revision;
    uint32_t calibration_revision;
    uint32_t discontinuity_revision;
    uint32_t device_state_revision;
    bool tx_enabled;
    bool tx_inhibited;
    necp_event_slot event_queue[NECP_EVENT_QUEUE_DEPTH];
    unsigned event_read;
    unsigned event_write;
    unsigned event_count;
} necp_context;

typedef struct necp_decoder {
    uint8_t bytes[NECP_MAX_FRAME_BYTES];
    size_t used;
    size_t expected;
} necp_decoder;

typedef int (*necp_frame_callback)(void *opaque, const uint8_t *frame,
                                   size_t frame_length);

int necp_context_init(necp_context *context, necp_backend *backend);
void necp_context_destroy(necp_context *context);

int necp_handle_frame(necp_context *context, const uint8_t *request,
                      size_t request_length, uint8_t *response,
                      size_t response_capacity, size_t *response_length);
int necp_pop_event(necp_context *context, uint8_t *event,
                   size_t event_capacity, size_t *event_length);

void necp_decoder_init(necp_decoder *decoder);
int necp_decoder_feed(necp_decoder *decoder, const uint8_t *bytes,
                      size_t length, necp_frame_callback callback,
                      void *opaque, size_t *frames_delivered);

int necp_mock_backend_create(necp_backend *backend);
void necp_mock_backend_advance(necp_backend *backend, uint64_t ticks);
unsigned necp_mock_backend_commit_count(const necp_backend *backend);

typedef struct necp_linux_options {
    const char *sysfs_root;
    const char *status_path;
    const char *tx_inhibit_path;
    bool test_allow_regular_files;
} necp_linux_options;

int necp_linux_backend_create(necp_backend *backend,
                              const necp_linux_options *options);

int necp_run_stdio(necp_context *context, int input_fd, int output_fd);
int necp_run_unix_server(necp_context *context, const char *socket_path);
int necp_run_functionfs(necp_context *context, const char *mount_path);

#ifdef __cplusplus
}
#endif

#endif
