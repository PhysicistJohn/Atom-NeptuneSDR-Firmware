#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "necp_daemon.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct stream_session {
    necp_context *context;
    int output_fd;
} stream_session;

static int write_all(int fd, const uint8_t *bytes, size_t length)
{
    while (length != 0U) {
        ssize_t count = write(fd, bytes, length);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        if (count == 0) {
            return -EIO;
        }
        bytes += (size_t)count;
        length -= (size_t)count;
    }
    return 0;
}

static int deliver_frame(void *opaque, const uint8_t *frame, size_t frame_length)
{
    stream_session *session = (stream_session *)opaque;
    uint8_t response[NECP_MAX_FRAME_BYTES];
    uint8_t event[NECP_MAX_FRAME_BYTES];
    size_t response_length;
    int result = necp_handle_frame(session->context, frame, frame_length, response,
                                   sizeof(response), &response_length);
    if (result != 0) {
        return result;
    }
    /* Deliver events caused by this transaction before its response.  The host
     * transaction pump can therefore collect them deterministically, including
     * for a one-shot CLI that closes immediately after the response. */
    for (;;) {
        size_t event_length;
        int available = necp_pop_event(session->context, event, sizeof(event),
                                       &event_length);
        if (available <= 0) {
            if (available < 0) {
                return available;
            }
            break;
        }
        result = write_all(session->output_fd, event, event_length);
        if (result != 0) {
            return result;
        }
    }
    return write_all(session->output_fd, response, response_length);
}

int necp_run_stdio(necp_context *context, int input_fd, int output_fd)
{
    uint8_t input[4096];
    necp_decoder decoder;
    stream_session session;
    necp_decoder_init(&decoder);
    session.context = context;
    session.output_fd = output_fd;
    for (;;) {
        ssize_t count = read(input_fd, input, sizeof(input));
        int result;
        if (count == 0) {
            return decoder.used == 0U ? 0 : -EBADMSG;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        result = necp_decoder_feed(&decoder, input, (size_t)count,
                                   deliver_frame, &session, NULL);
        if (result != 0) {
            return result;
        }
    }
}

int necp_run_unix_server(necp_context *context, const char *socket_path)
{
    struct sockaddr_un address;
    struct stat existing;
    int server;
    int result = 0;
    if (context == NULL || socket_path == NULL || socket_path[0] == '\0' ||
        strlen(socket_path) >= sizeof(address.sun_path)) {
        return -EINVAL;
    }
    if (lstat(socket_path, &existing) == 0) {
        return -EEXIST;
    }
    if (errno != ENOENT) {
        return -errno;
    }
    server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) {
        return -errno;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1U);
    if (bind(server, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(socket_path, S_IRUSR | S_IWUSR) != 0 || listen(server, 4) != 0) {
        result = -errno;
        (void)close(server);
        (void)unlink(socket_path);
        return result;
    }
    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            result = -errno;
            break;
        }
        result = necp_run_stdio(context, client, client);
        (void)close(client);
        if (result != 0 && result != -ECONNRESET && result != -EPIPE) {
            break;
        }
        result = 0;
    }
    (void)close(server);
    (void)unlink(socket_path);
    return result;
}

#if defined(__linux__)
#include <endian.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>

typedef struct functionfs_descriptors {
    struct usb_functionfs_descs_head_v2 header;
    uint32_t fs_count;
    uint32_t hs_count;
    struct usb_interface_descriptor fs_interface;
    struct usb_endpoint_descriptor_no_audio fs_out;
    struct usb_endpoint_descriptor_no_audio fs_in;
    struct usb_interface_descriptor hs_interface;
    struct usb_endpoint_descriptor_no_audio hs_out;
    struct usb_endpoint_descriptor_no_audio hs_in;
} __attribute__((packed)) functionfs_descriptors;

typedef struct functionfs_strings {
    struct usb_functionfs_strings_head header;
    struct {
        uint16_t code;
        char interface_name[21];
    } __attribute__((packed)) language;
} __attribute__((packed)) functionfs_strings;

static int functionfs_configure(int ep0)
{
    const functionfs_descriptors descriptors = {
        .header = {
            .magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2),
            .length = htole32(sizeof(functionfs_descriptors)),
            .flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC),
        },
        .fs_count = htole32(3),
        .hs_count = htole32(3),
        .fs_interface = {
            .bLength = sizeof(struct usb_interface_descriptor),
            .bDescriptorType = USB_DT_INTERFACE,
            .bNumEndpoints = 2,
            .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
            .iInterface = 1,
        },
        .fs_out = {
            .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
            .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = 1,
            .bmAttributes = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize = htole16(64),
        },
        .fs_in = {
            .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
            .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = USB_DIR_IN | 2,
            .bmAttributes = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize = htole16(64),
        },
        .hs_interface = {
            .bLength = sizeof(struct usb_interface_descriptor),
            .bDescriptorType = USB_DT_INTERFACE,
            .bNumEndpoints = 2,
            .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
            .iInterface = 1,
        },
        .hs_out = {
            .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
            .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = 1,
            .bmAttributes = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize = htole16(512),
        },
        .hs_in = {
            .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
            .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = USB_DIR_IN | 2,
            .bmAttributes = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize = htole16(512),
        },
    };
    const functionfs_strings strings = {
        .header = {
            .magic = htole32(FUNCTIONFS_STRINGS_MAGIC),
            .length = htole32(sizeof(functionfs_strings)),
            .str_count = htole32(1),
            .lang_count = htole32(1),
        },
        .language = {
            .code = htole16(0x0409),
            .interface_name = "Neptune Edge Control",
        },
    };
    int result = write_all(ep0, (const uint8_t *)&descriptors,
                           sizeof(descriptors));
    return result == 0
               ? write_all(ep0, (const uint8_t *)&strings, sizeof(strings))
               : result;
}

int necp_run_functionfs(necp_context *context, const char *mount_path)
{
    char ep0_path[512];
    char out_path[512];
    char in_path[512];
    int ep0;
    int out;
    int in;
    int count;
    int result;
    count = snprintf(ep0_path, sizeof(ep0_path), "%s/ep0", mount_path);
    if (count < 0 || (size_t)count >= sizeof(ep0_path)) {
        return -ENAMETOOLONG;
    }
    count = snprintf(out_path, sizeof(out_path), "%s/ep1", mount_path);
    if (count < 0 || (size_t)count >= sizeof(out_path)) {
        return -ENAMETOOLONG;
    }
    count = snprintf(in_path, sizeof(in_path), "%s/ep2", mount_path);
    if (count < 0 || (size_t)count >= sizeof(in_path)) {
        return -ENAMETOOLONG;
    }
    ep0 = open(ep0_path, O_RDWR);
    if (ep0 < 0) {
        return -errno;
    }
    result = functionfs_configure(ep0);
    if (result != 0) {
        (void)close(ep0);
        return result;
    }
    out = open(out_path, O_RDONLY);
    if (out < 0) {
        result = -errno;
        (void)close(ep0);
        return result;
    }
    in = open(in_path, O_WRONLY);
    if (in < 0) {
        result = -errno;
        (void)close(out);
        (void)close(ep0);
        return result;
    }
    result = necp_run_stdio(context, out, in);
    (void)close(in);
    (void)close(out);
    (void)close(ep0);
    return result;
}
#else
int necp_run_functionfs(necp_context *context, const char *mount_path)
{
    (void)context;
    (void)mount_path;
    return -ENOTSUP;
}
#endif
