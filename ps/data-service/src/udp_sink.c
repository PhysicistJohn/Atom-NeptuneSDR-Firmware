#define _GNU_SOURCE

#include "neptune_data_service.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

int nds_udp_open(nds_udp_sink *sink, const nds_destination *destination)
{
    struct sockaddr_in address;
    int send_buffer = 4 * 1024 * 1024;
    int fd;
    if (sink == NULL || destination == NULL || destination->port == 0U ||
        (destination->mtu != 1500U && destination->mtu != 9000U)) {
        return -EINVAL;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -errno;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer,
                   sizeof(send_buffer)) != 0) {
        int result = -errno;
        (void)close(fd);
        return result;
    }
#if defined(__linux__) && defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_DO)
    {
        int discovery = IP_PMTUDISC_DO;
        if (setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &discovery,
                       sizeof(discovery)) != 0) {
            int result = -errno;
            (void)close(fd);
            return result;
        }
    }
#endif
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(destination->port);
    memcpy(&address.sin_addr.s_addr, destination->ipv4, 4);
    memset(sink, 0, sizeof(*sink));
    sink->fd = fd;
    memcpy(sink->address_storage, &address, sizeof(address));
    sink->address_length = sizeof(address);
    return 0;
}

void nds_udp_close(nds_udp_sink *sink)
{
    if (sink != NULL && sink->fd >= 0) {
        (void)close(sink->fd);
        sink->fd = -1;
    }
}

int nds_udp_send_batch(nds_udp_sink *sink, const nds_packet *packets,
                       size_t packet_count, size_t *sent_count)
{
    size_t index;
    if (sent_count != NULL) {
        *sent_count = 0;
    }
    if (sink == NULL || packets == NULL || packet_count == 0U ||
        packet_count > NDS_MAX_BATCH || sink->fd < 0) {
        return -EINVAL;
    }
#if defined(__linux__)
    {
        struct mmsghdr messages[NDS_MAX_BATCH];
        struct iovec vectors[NDS_MAX_BATCH][2];
        int count;
        memset(messages, 0, sizeof(messages));
        memset(vectors, 0, sizeof(vectors));
        for (index = 0; index < packet_count; ++index) {
            vectors[index][0].iov_base = (void *)(uintptr_t)packets[index].prefix;
            vectors[index][0].iov_len = packets[index].prefix_bytes;
            vectors[index][1].iov_base = (void *)(uintptr_t)packets[index].payload;
            vectors[index][1].iov_len = packets[index].payload_bytes;
            messages[index].msg_hdr.msg_name = sink->address_storage;
            messages[index].msg_hdr.msg_namelen =
                (socklen_t)sink->address_length;
            messages[index].msg_hdr.msg_iov = vectors[index];
            messages[index].msg_hdr.msg_iovlen = 2;
        }
        do {
            count = sendmmsg(sink->fd, messages, (unsigned)packet_count,
                             MSG_DONTWAIT);
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            return -errno;
        }
        if (sent_count != NULL) {
            *sent_count = (size_t)count;
        }
        return (size_t)count == packet_count ? 0 : -EAGAIN;
    }
#else
    for (index = 0; index < packet_count; ++index) {
        struct msghdr message;
        struct iovec vectors[2];
        ssize_t count;
        memset(&message, 0, sizeof(message));
        vectors[0].iov_base = (void *)(uintptr_t)packets[index].prefix;
        vectors[0].iov_len = packets[index].prefix_bytes;
        vectors[1].iov_base = (void *)(uintptr_t)packets[index].payload;
        vectors[1].iov_len = packets[index].payload_bytes;
        message.msg_name = sink->address_storage;
        message.msg_namelen = (socklen_t)sink->address_length;
        message.msg_iov = vectors;
        message.msg_iovlen = 2;
        do {
            count = sendmsg(sink->fd, &message, 0);
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            return -errno;
        }
        if ((size_t)count != packets[index].prefix_bytes +
                                 packets[index].payload_bytes) {
            return -EIO;
        }
        if (sent_count != NULL) {
            *sent_count = index + 1U;
        }
    }
    return 0;
#endif
}
