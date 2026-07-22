#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "necp_daemon.h"
#include "neptune_pl_registers_v1.h"

#if defined(__linux__)
#include <sys/ioctl.h>
#include "neptune_stream_uapi.h"
#else
#include "neptune_stream_uapi.h"
extern int ioctl(int descriptor, unsigned long request, ...);
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct linux_state {
    char *sysfs_root;
    char *status_path;
    char *tx_inhibit_path;
    bool test_allow_regular_files;
    int status_fd;
    struct neptune_stream_abi_info abi;
    struct neptune_stream_pl_status status;
} linux_state;

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

_Static_assert(sizeof(struct neptune_stream_abi_info) ==
                   NEPTUNE_STREAM_ABI_INFO_SIZE,
               "stream ABI info size");
_Static_assert(sizeof(struct neptune_stream_pl_status) ==
                   NEPTUNE_STREAM_PL_STATUS_SIZE,
               "PL status ABI size");

static char *copy_string(const char *value)
{
    size_t length;
    char *copy;
    if (value == NULL) {
        return NULL;
    }
    length = strlen(value) + 1U;
    copy = (char *)malloc(length);
    if (copy != NULL) {
        memcpy(copy, value, length);
    }
    return copy;
}

static int path_join(char *output, size_t capacity, const char *root,
                     const char *name)
{
    int count = snprintf(output, capacity, "%s/%s", root, name);
    return count < 0 || (size_t)count >= capacity ? -ENAMETOOLONG : 0;
}

static int secure_read_file(const linux_state *state, const char *path,
                            char *output, size_t capacity)
{
    struct stat metadata;
    ssize_t count;
    char extra;
    int fd;
    if (capacity < 2U) {
        return -EINVAL;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return -errno;
    }
    if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        (!state->test_allow_regular_files &&
         (metadata.st_uid != 0 ||
          (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0))) {
        (void)close(fd);
        return -EPERM;
    }
    count = read(fd, output, capacity - 1U);
    if (count >= 0 && (size_t)count == capacity - 1U) {
        ssize_t more = read(fd, &extra, 1);
        if (more != 0) {
            (void)close(fd);
            return more < 0 ? -errno : -EOVERFLOW;
        }
    }
    if (close(fd) != 0 && count >= 0) {
        return -errno;
    }
    if (count < 0) {
        return -errno;
    }
    output[count] = '\0';
    return (int)count;
}

static int read_text(const linux_state *state, const char *name, char *output,
                     size_t capacity)
{
    char path[512];
    int count;
    if (capacity == 0U ||
        path_join(path, sizeof(path), state->sysfs_root, name) != 0) {
        return -EINVAL;
    }
    count = secure_read_file(state, path, output, capacity);
    if (count < 0) {
        return count;
    }
    while (count > 0 &&
           (output[count - 1] == '\n' || output[count - 1] == '\r')) {
        output[--count] = '\0';
    }
    return 0;
}

static int read_number(const linux_state *state, const char *name,
                       long long *number)
{
    char text[64];
    char *end;
    long long parsed;
    int result = read_text(state, name, text, sizeof(text));
    if (result != 0) {
        return result;
    }
    errno = 0;
    parsed = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return -EINVAL;
    }
    *number = parsed;
    return 0;
}

static int read_unsigned(const linux_state *state, const char *name,
                         uint64_t *number)
{
    char text[64];
    char *end;
    unsigned long long parsed;
    int result = read_text(state, name, text, sizeof(text));
    if (result != 0) {
        return result;
    }
    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return -EINVAL;
    }
    *number = (uint64_t)parsed;
    return 0;
}

static int pread_exact(int descriptor, void *output, size_t length,
                       off_t offset)
{
    uint8_t *bytes = (uint8_t *)output;
    size_t consumed = 0;
    while (consumed < length) {
        ssize_t count = pread(descriptor, bytes + consumed, length - consumed,
                              offset + (off_t)consumed);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return -errno;
        }
        if (count == 0) {
            return -EIO;
        }
        consumed += (size_t)count;
    }
    return 0;
}

static bool all_zero(const void *value, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)value;
    size_t index;
    for (index = 0; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static int get_abi(linux_state *state, struct neptune_stream_abi_info *abi)
{
    int result;
    memset(abi, 0, sizeof(*abi));
    abi->struct_size = sizeof(*abi);
    if (state->test_allow_regular_files) {
        result = pread_exact(state->status_fd, abi, sizeof(*abi), 0);
    } else {
        result = ioctl(state->status_fd, NEPTUNE_STREAM_IOC_GET_ABI, abi) == 0
                     ? 0
                     : -errno;
    }
    if (result != 0) {
        return result;
    }
    if (abi->struct_size != sizeof(*abi) ||
        abi->abi_major != NEPTUNE_STREAM_ABI_MAJOR ||
        (abi->feature_flags & NEPTUNE_STREAM_FEAT_PL_STATUS) == 0U ||
        !all_zero(abi->reserved, sizeof(abi->reserved))) {
        return -EPROTONOSUPPORT;
    }
    return 0;
}

static int get_pl_status(linux_state *state,
                         struct neptune_stream_pl_status *status)
{
    int result;
    memset(status, 0, sizeof(*status));
    status->struct_size = sizeof(*status);
    if (state->test_allow_regular_files) {
        result = pread_exact(state->status_fd, status, sizeof(*status),
                             (off_t)sizeof(struct neptune_stream_abi_info));
    } else {
        result =
            ioctl(state->status_fd, NEPTUNE_STREAM_IOC_GET_PL_STATUS, status) ==
                    0
                ? 0
                : -errno;
    }
    if (result != 0) {
        return result;
    }
    if (status->struct_size != sizeof(*status) || status->flags != 0U ||
        status->pl_magic != NEPTUNE_PL_MAGIC ||
        status->pl_abi_version != NEPTUNE_PL_ABI_VERSION ||
        !all_zero(status->reserved, sizeof(status->reserved))) {
        return -EPROTONOSUPPORT;
    }
    state->status = *status;
    return 0;
}

static int linux_initialize(necp_backend *backend)
{
    linux_state *state = (linux_state *)backend->state;
    struct stat metadata;
    int result;
    state->status_fd = open(state->status_path,
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (state->status_fd < 0) {
        return -errno;
    }
    if (fstat(state->status_fd, &metadata) != 0 ||
        (!S_ISCHR(metadata.st_mode) &&
         !(state->test_allow_regular_files && S_ISREG(metadata.st_mode)))) {
        (void)close(state->status_fd);
        state->status_fd = -1;
        return -ENODEV;
    }
    result = get_abi(state, &state->abi);
    if (result == 0) {
        result = get_pl_status(state, &state->status);
    }
    if (result != 0) {
        (void)close(state->status_fd);
        state->status_fd = -1;
    }
    return result;
}

static void linux_destroy(necp_backend *backend)
{
    linux_state *state = (linux_state *)backend->state;
    if (state == NULL) {
        return;
    }
    if (state->status_fd >= 0) {
        (void)close(state->status_fd);
    }
    free(state->sysfs_root);
    free(state->status_path);
    free(state->tx_inhibit_path);
    free(state);
    backend->state = NULL;
    backend->ops = NULL;
}

static int linux_get_identity(necp_backend *backend, necp_identity *identity)
{
    linux_state *state = (linux_state *)backend->state;
    struct neptune_stream_pl_status status;
    uint64_t hardware_id;
    uint64_t hardware_revision;
    uint64_t firmware_build_id;
    uint64_t device_serial_hash;
    memset(identity, 0, sizeof(*identity));
    if (get_pl_status(state, &status) != 0 ||
        read_unsigned(state, "hardware_id", &hardware_id) != 0 ||
        hardware_id > UINT32_MAX ||
        read_unsigned(state, "hardware_revision", &hardware_revision) != 0 ||
        hardware_revision > UINT32_MAX ||
        read_unsigned(state, "firmware_build_id", &firmware_build_id) != 0 ||
        read_unsigned(state, "device_serial_hash", &device_serial_hash) != 0) {
        return -ENOENT;
    }
    identity->hardware_id = (uint32_t)hardware_id;
    identity->hardware_revision = (uint32_t)hardware_revision;
    identity->fpga_build_id = status.pl_build_id;
    identity->firmware_build_id = firmware_build_id;
    identity->calibration_revision = status.calibration_revision;
    identity->capability_bits = status.pl_capabilities;
    identity->device_serial_hash = device_serial_hash;
    return 0;
}

static int linux_read_sample_counter(necp_backend *backend, uint64_t *timestamp)
{
    linux_state *state = (linux_state *)backend->state;
    struct neptune_stream_pl_status status;
    int result = get_pl_status(state, &status);
    if (result == 0) {
        *timestamp = status.sample_timestamp;
    }
    return result;
}

static int linux_read_uptime_ns(uint64_t *uptime_ns)
{
    struct timespec value;
    clockid_t clock_id = CLOCK_MONOTONIC;
#if defined(__linux__) && defined(CLOCK_BOOTTIME)
    clock_id = CLOCK_BOOTTIME;
#endif
    if (clock_gettime(clock_id, &value) != 0) {
        return -errno;
    }
    if (value.tv_sec < 0 || value.tv_nsec < 0 ||
        value.tv_nsec >= 1000000000L ||
        (uint64_t)value.tv_sec > UINT64_MAX / UINT64_C(1000000000)) {
        return -EOVERFLOW;
    }
    *uptime_ns = (uint64_t)value.tv_sec * UINT64_C(1000000000) +
                 (uint64_t)value.tv_nsec;
    return 0;
}

static int linux_get_health(necp_backend *backend, necp_health *health)
{
    linux_state *state = (linux_state *)backend->state;
    struct neptune_stream_pl_status status;
    long long value;
    int result;
    memset(health, 0, sizeof(*health));
    result = get_pl_status(state, &status);
    if (result != 0) {
        return result;
    }
    result = linux_read_uptime_ns(&health->uptime_ns);
    if (result != 0) {
        return result;
    }
    health->fault_flags = status.global_faults;
    health->fifo_high_watermark = status.dma_fifo_high_water;
    health->fifo_overflows = status.dma_fifo_overflows;
    health->dma_overruns = status.dma_descriptor_starvations;
    health->discontinuity_revision = status.discontinuity_revision;
    health->configuration_revision = status.configuration_revision;
    health->calibration_revision = status.calibration_revision;
    health->active_stream_mask =
        (status.stream_status &
         NEPTUNE_PL_FIELD_STREAM0_STATUS_RUNNING_MASK) != 0U
            ? 1U
            : 0U;
    if (read_number(state, "temperature_mc", &value) == 0 &&
        value >= INT32_MIN && value <= INT32_MAX) {
        health->temperature_mc = (int32_t)value;
    }
    if (read_number(state, "supply_uv", &value) == 0 && value >= 0 &&
        (unsigned long long)value / 1000U <= UINT32_MAX) {
        health->supply_mv = (uint32_t)((unsigned long long)value / 1000U);
    }
    if (read_number(state, "supply_ua", &value) == 0 && value >= 0 &&
        (unsigned long long)value / 1000U <= UINT32_MAX) {
        health->supply_ma = (uint32_t)((unsigned long long)value / 1000U);
    }
    if (read_number(state, "dropped_packets", &value) == 0 && value >= 0 &&
        (unsigned long long)value <= UINT32_MAX) {
        health->dropped_packets = (uint32_t)value;
    }
    health->status_flags = NECP_HEALTH_FLAG_BACKEND_READY;
    if ((status.global_status &
         NEPTUNE_PL_FIELD_GLOBAL_STATUS_RX_PLL_LOCKED_MASK) != 0U) {
        health->status_flags |= NECP_HEALTH_FLAG_RF_PLL_LOCKED;
        health->pll_lock_mask = 3U;
    }
    if (read_number(state, "ethernet_link", &value) == 0 && value == 1) {
        health->status_flags |= NECP_HEALTH_FLAG_ETHERNET_LINK;
        health->ethernet_link_state = 1U;
    }
    if (read_number(state, "usb_configured", &value) == 0 && value == 1) {
        health->status_flags |= NECP_HEALTH_FLAG_USB_CONFIGURED;
        health->usb_state = 1U;
    }
    return 0;
}

static int linux_get_rf(necp_backend *backend,
                        neptune_edge_control_rf_config_v1 *config)
{
    (void)backend;
    (void)config;
    return -ENOTSUP;
}

static int linux_commit_rf(necp_backend *backend,
                           const neptune_edge_control_rf_config_v1 *config,
                           uint64_t activation_timestamp)
{
    (void)backend;
    (void)config;
    (void)activation_timestamp;
    return -ENOTSUP;
}

static int linux_commit_pipeline(
    necp_backend *backend,
    const neptune_edge_control_pipeline_config_v1 *config,
    uint64_t changed_fields, uint64_t activation_timestamp)
{
    (void)backend;
    (void)config;
    (void)changed_fields;
    (void)activation_timestamp;
    return -ENOTSUP;
}

static int linux_stream_update(necp_backend *backend, uint16_t command,
                               const neptune_edge_control_stream_config_v1 *config,
                               uint64_t activation_timestamp)
{
    (void)backend;
    (void)command;
    (void)config;
    (void)activation_timestamp;
    return -ENOTSUP;
}

static int linux_reset_counters(necp_backend *backend)
{
    (void)backend;
    return -ENOTSUP;
}

static int linux_force_tx_off(necp_backend *backend)
{
    linux_state *state = (linux_state *)backend->state;
    struct neptune_stream_pl_status status;
    int result = get_pl_status(state, &status);
    if (result != 0) {
        return result;
    }
    /* The sole-owner kernel driver disarms TX at probe. The status node is
     * deliberately read-only, so fail closed unless that disarm is proven. */
    return (status.tx_safety_status &
            NEPTUNE_PL_FIELD_TX_SAFETY_STATUS_DISARMED_MASK) != 0U
               ? 0
               : -ENOTSUP;
}

static int linux_tx_is_inhibited(necp_backend *backend, bool *inhibited)
{
    linux_state *state = (linux_state *)backend->state;
    struct neptune_stream_pl_status status;
    char value[4];
    int count;
    int result = get_pl_status(state, &status);
    if (result != 0) {
        return result;
    }
    count = secure_read_file(state, state->tx_inhibit_path, value,
                             sizeof(value));
    if (count == -ENOENT) {
        *inhibited = true;
        return 0;
    }
    if (count < 0) {
        return count;
    }
    if (count != 2 || (value[0] != '0' && value[0] != '1') ||
        value[1] != '\n' || value[2] != '\0') {
        return -EINVAL;
    }
    *inhibited =
        value[0] != '0' ||
        (status.tx_safety_status &
         (NEPTUNE_PL_FIELD_TX_SAFETY_STATUS_PERSISTENT_INHIBIT_MASK |
          NEPTUNE_PL_FIELD_TX_SAFETY_STATUS_FAULT_INHIBIT_MASK)) != 0U;
    return 0;
}

static const necp_backend_ops LINUX_OPS = {
    .initialize = linux_initialize,
    .destroy = linux_destroy,
    .get_identity = linux_get_identity,
    .get_health = linux_get_health,
    .get_rf = linux_get_rf,
    .read_sample_counter = linux_read_sample_counter,
    .commit_rf = linux_commit_rf,
    .commit_pipeline = linux_commit_pipeline,
    .stream_update = linux_stream_update,
    .reset_counters = linux_reset_counters,
    .force_tx_off = linux_force_tx_off,
    .tx_is_inhibited = linux_tx_is_inhibited,
};

int necp_linux_backend_create(necp_backend *backend,
                              const necp_linux_options *options)
{
    linux_state *state;
    if (backend == NULL || options == NULL || options->sysfs_root == NULL ||
        options->status_path == NULL || options->tx_inhibit_path == NULL) {
        return -EINVAL;
    }
    state = (linux_state *)calloc(1, sizeof(*state));
    if (state == NULL) {
        return -ENOMEM;
    }
    state->status_fd = -1;
    state->sysfs_root = copy_string(options->sysfs_root);
    state->status_path = copy_string(options->status_path);
    state->tx_inhibit_path = copy_string(options->tx_inhibit_path);
    state->test_allow_regular_files = options->test_allow_regular_files;
    if (state->sysfs_root == NULL || state->status_path == NULL ||
        state->tx_inhibit_path == NULL) {
        free(state->sysfs_root);
        free(state->status_path);
        free(state->tx_inhibit_path);
        free(state);
        return -ENOMEM;
    }
    backend->ops = &LINUX_OPS;
    backend->state = state;
    return 0;
}
