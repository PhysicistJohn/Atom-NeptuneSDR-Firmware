#define _POSIX_C_SOURCE 200809L

#include "necp_daemon.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef enum transport_kind {
    TRANSPORT_NONE,
    TRANSPORT_STDIO,
    TRANSPORT_UNIX,
    TRANSPORT_FUNCTIONFS,
} transport_kind;

static void usage(FILE *stream, const char *program)
{
    (void)fprintf(
        stream,
        "usage: %s [--mock] (--stdio | --unix PATH | --functionfs PATH)\n"
        "          [--sysfs PATH --status-device PATH --tx-inhibit PATH]\n\n"
        "Frames are raw concatenated NECP messages on stdio/Unix streams.\n"
        "The Linux backend fails closed unless all identity sysfs attributes,\n"
        "the read-only PL status device, and persistent TX inhibit are usable.\n",
        program);
}

int main(int argc, char **argv)
{
    const char *sysfs = "/sys/bus/platform/devices/neptune-control";
    const char *status_device = "/dev/neptune-pl-status";
    const char *tx_inhibit = "/var/lib/neptune/tx-inhibit";
    const char *transport_path = NULL;
    transport_kind transport = TRANSPORT_NONE;
    bool mock = false;
    necp_linux_options options;
    necp_backend backend;
    necp_context context;
    int index;
    int result;
    memset(&backend, 0, sizeof(backend));
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--mock") == 0) {
            mock = true;
        } else if (strcmp(argv[index], "--stdio") == 0) {
            if (transport != TRANSPORT_NONE) {
                usage(stderr, argv[0]);
                return 2;
            }
            transport = TRANSPORT_STDIO;
        } else if ((strcmp(argv[index], "--unix") == 0 ||
                    strcmp(argv[index], "--functionfs") == 0) &&
                   index + 1 < argc) {
            if (transport != TRANSPORT_NONE) {
                usage(stderr, argv[0]);
                return 2;
            }
            transport = strcmp(argv[index], "--unix") == 0
                            ? TRANSPORT_UNIX
                            : TRANSPORT_FUNCTIONFS;
            transport_path = argv[++index];
        } else if (strcmp(argv[index], "--sysfs") == 0 && index + 1 < argc) {
            sysfs = argv[++index];
        } else if (strcmp(argv[index], "--status-device") == 0 &&
                   index + 1 < argc) {
            status_device = argv[++index];
        } else if (strcmp(argv[index], "--tx-inhibit") == 0 &&
                   index + 1 < argc) {
            tx_inhibit = argv[++index];
        } else if (strcmp(argv[index], "--help") == 0) {
            usage(stdout, argv[0]);
            return 0;
        } else {
            usage(stderr, argv[0]);
            return 2;
        }
    }
    if (transport == TRANSPORT_NONE) {
        usage(stderr, argv[0]);
        return 2;
    }
    if (mock) {
        result = necp_mock_backend_create(&backend);
    } else {
        options.sysfs_root = sysfs;
        options.status_path = status_device;
        options.tx_inhibit_path = tx_inhibit;
        options.test_allow_regular_files = false;
        result = necp_linux_backend_create(&backend, &options);
    }
    if (result != 0) {
        (void)fprintf(stderr, "backend creation failed: %s\n", strerror(-result));
        return 1;
    }
    result = necp_context_init(&context, &backend);
    if (result != 0) {
        (void)fprintf(stderr, "backend initialization failed: %s\n",
                      strerror(-result));
        if (backend.ops != NULL && backend.ops->destroy != NULL) {
            backend.ops->destroy(&backend);
        }
        return 1;
    }
    if (transport == TRANSPORT_STDIO) {
        result = necp_run_stdio(&context, 0, 1);
    } else if (transport == TRANSPORT_UNIX) {
        result = necp_run_unix_server(&context, transport_path);
    } else {
        result = necp_run_functionfs(&context, transport_path);
    }
    necp_context_destroy(&context);
    if (result != 0) {
        (void)fprintf(stderr, "transport failed: %s\n", strerror(-result));
        return 1;
    }
    return 0;
}
