#define _POSIX_C_SOURCE 200809L

#include "neptune_data_service.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void usage(FILE *stream, const char *program)
{
    (void)fprintf(
        stream,
        "usage: %s --destination IPv4 --port PORT [options]\n"
        "  --device PATH             dedicated DMA ring (default /dev/neptune-stream)\n"
        "  --mtu 1500|9000           Ethernet MTU (default 9000)\n"
        "  --stream-id N             nonzero logical stream (default 1)\n"
        "  --product raw|calibrated|normalized (default normalized)\n"
        "  --format s16|s12p|s8|s8bf (default s8)\n"
        "  --channels 1|2|3          RX channel mask (default 1)\n"
        "  --samples-per-packet N    default 4096\n"
        "  --sample-rate N           default 55000000\n"
        "  --center-frequency HZ     required RF-state snapshot\n"
        "  --rf-bandwidth HZ         default 20000000\n"
        "  --configuration-revision N required, must match DMA blocks\n"
        "  --device-state-revision N required, must match DMA blocks\n"
        "  --state-activation-timestamp N required ingress tick\n"
        "  --rx1-gain-mdb N          required measurement metadata\n"
        "  --rx2-gain-mdb N          required when RX2 is enabled\n"
        "  --rx1-gain-mode MODE      manual|slow|fast|hybrid, required\n"
        "  --rx2-gain-mode MODE      required when RX2 is enabled\n"
        "  --digital-gain-q16-16 N   required measurement metadata\n"
        "  --temperature-mc N        required measurement metadata\n"
        "  --pll-lock-mask N         required measurement metadata\n"
        "  --device-flags N          required measurement metadata\n"
        "  --batch N                 1..32 (default 16)\n"
        "  --allow-loopback          explicit development-only destination exception\n",
        program);
}

static int parse_u32(const char *text, uint32_t minimum, uint32_t maximum,
                     uint32_t *value)
{
    char *end;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum ||
        parsed > maximum) {
        return -EINVAL;
    }
    *value = (uint32_t)parsed;
    return 0;
}

static int parse_u64(const char *text, uint64_t minimum, uint64_t maximum,
                     uint64_t *value)
{
    char *end;
    unsigned long long parsed;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum ||
        parsed > maximum) {
        return -EINVAL;
    }
    *value = (uint64_t)parsed;
    return 0;
}

static int parse_i32(const char *text, int32_t minimum, int32_t maximum,
                     int32_t *value)
{
    char *end;
    long parsed;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum ||
        parsed > maximum) {
        return -EINVAL;
    }
    *value = (int32_t)parsed;
    return 0;
}

static int parse_product(const char *text, uint32_t *product)
{
    if (strcmp(text, "raw") == 0) {
        *product = NEPTUNE_EDGE_PACKET_TYPE_RAW_IQ;
    } else if (strcmp(text, "calibrated") == 0) {
        *product = NEPTUNE_EDGE_PACKET_TYPE_CALIBRATED_IQ;
    } else if (strcmp(text, "normalized") == 0) {
        *product = NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ;
    } else {
        return -EINVAL;
    }
    return 0;
}

static int parse_format(const char *text, uint32_t *format)
{
    if (strcmp(text, "s16") == 0) {
        *format = NEPTUNE_EDGE_SAMPLE_FORMAT_S16;
    } else if (strcmp(text, "s12p") == 0) {
        *format = NEPTUNE_EDGE_SAMPLE_FORMAT_S12P;
    } else if (strcmp(text, "s8") == 0) {
        *format = NEPTUNE_EDGE_SAMPLE_FORMAT_S8;
    } else if (strcmp(text, "s8bf") == 0) {
        *format = NEPTUNE_EDGE_SAMPLE_FORMAT_S8BF;
    } else {
        return -EINVAL;
    }
    return 0;
}

static int parse_gain_mode(const char *text, uint8_t *mode)
{
    if (strcmp(text, "manual") == 0) {
        *mode = NEPTUNE_EDGE_GAIN_MODE_MANUAL;
    } else if (strcmp(text, "slow") == 0) {
        *mode = NEPTUNE_EDGE_GAIN_MODE_SLOW_ATTACK;
    } else if (strcmp(text, "fast") == 0) {
        *mode = NEPTUNE_EDGE_GAIN_MODE_FAST_ATTACK;
    } else if (strcmp(text, "hybrid") == 0) {
        *mode = NEPTUNE_EDGE_GAIN_MODE_HYBRID;
    } else {
        return -EINVAL;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *device = "/dev/neptune-stream";
    const char *destination_text = NULL;
    uint32_t port = 0;
    uint32_t mtu = 9000;
    bool allow_loopback = false;
    bool have_frequency = false;
    bool have_revision = false;
    bool have_device_state_revision = false;
    bool have_state_activation = false;
    bool have_rx1_gain = false;
    bool have_rx2_gain = false;
    bool have_rx1_gain_mode = false;
    bool have_rx2_gain_mode = false;
    bool have_digital_gain = false;
    bool have_temperature = false;
    bool have_pll_lock = false;
    bool have_device_flags = false;
    nds_stream_profile profile;
    nds_dma_ring ring;
    nds_service service;
    nds_dma_stats dma_stats;
    uint64_t wire_rate;
    int index;
    int result;
    memset(&profile, 0, sizeof(profile));
    memset(&ring, 0, sizeof(ring));
    profile.stream_id = 1;
    profile.packet_type = NEPTUNE_EDGE_PACKET_TYPE_NORMALIZED_IQ;
    profile.channel_mask = 1;
    profile.sample_format = NEPTUNE_EDGE_SAMPLE_FORMAT_S8;
    profile.samples_per_packet = 4096;
    profile.sample_rate_hz = NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ;
    profile.initial_snapshot.state.sample_rate_hz =
        NEPTUNE_EDGE_EGRESS_SAMPLE_RATE_HZ;
    profile.initial_snapshot.state.rf_bandwidth_hz = UINT32_C(20000000);
    profile.batch_size = 16;
    for (index = 1; index < argc; ++index) {
        const char *option = argv[index];
        if (strcmp(option, "--help") == 0) {
            usage(stdout, argv[0]);
            return 0;
        }
        if (strcmp(option, "--allow-loopback") == 0) {
            allow_loopback = true;
            continue;
        }
        if (index + 1 >= argc) {
            usage(stderr, argv[0]);
            return 2;
        }
        if (strcmp(option, "--device") == 0) {
            device = argv[++index];
        } else if (strcmp(option, "--destination") == 0) {
            destination_text = argv[++index];
        } else if (strcmp(option, "--port") == 0) {
            if (parse_u32(argv[++index], 1, 65535, &port) != 0) {
                return 2;
            }
        } else if (strcmp(option, "--mtu") == 0) {
            if (parse_u32(argv[++index], 1500, 9000, &mtu) != 0) {
                return 2;
            }
        } else if (strcmp(option, "--stream-id") == 0) {
            if (parse_u32(argv[++index], 1, UINT32_MAX, &profile.stream_id) != 0) {
                return 2;
            }
        } else if (strcmp(option, "--product") == 0) {
            if (parse_product(argv[++index], &profile.packet_type) != 0) {
                return 2;
            }
        } else if (strcmp(option, "--format") == 0) {
            if (parse_format(argv[++index], &profile.sample_format) != 0) {
                return 2;
            }
        } else if (strcmp(option, "--channels") == 0) {
            if (parse_u32(argv[++index], 1, 3, &profile.channel_mask) != 0) {
                return 2;
            }
        } else if (strcmp(option, "--samples-per-packet") == 0) {
            if (parse_u32(argv[++index], 1, UINT32_MAX,
                          &profile.samples_per_packet) != 0) {
                return 2;
            }
        } else if (strcmp(option, "--sample-rate") == 0) {
            if (parse_u32(argv[++index], 1, UINT32_MAX,
                          &profile.sample_rate_hz) != 0) {
                return 2;
            }
            profile.initial_snapshot.state.sample_rate_hz =
                profile.sample_rate_hz;
        } else if (strcmp(option, "--center-frequency") == 0) {
            if (parse_u64(argv[++index], UINT64_C(70000000),
                          UINT64_C(6000000000),
                          &profile.initial_snapshot.state.center_frequency_hz) !=
                0) {
                return 2;
            }
            have_frequency = true;
        } else if (strcmp(option, "--rf-bandwidth") == 0) {
            if (parse_u32(argv[++index], UINT32_C(200000),
                          UINT32_C(56000000),
                          &profile.initial_snapshot.state.rf_bandwidth_hz) != 0) {
                return 2;
            }
        } else if (strcmp(option, "--configuration-revision") == 0) {
            if (parse_u32(argv[++index], 0, UINT32_MAX,
                          &profile.initial_snapshot.state.configuration_revision) !=
                0) {
                return 2;
            }
            have_revision = true;
        } else if (strcmp(option, "--device-state-revision") == 0) {
            if (parse_u32(argv[++index], 0, UINT32_MAX,
                          &profile.initial_snapshot.device_state_revision) != 0) {
                return 2;
            }
            have_device_state_revision = true;
        } else if (strcmp(option, "--state-activation-timestamp") == 0) {
            if (parse_u64(argv[++index], 0, UINT64_MAX,
                          &profile.initial_snapshot.activation_timestamp) != 0) {
                return 2;
            }
            have_state_activation = true;
        } else if (strcmp(option, "--rx1-gain-mdb") == 0) {
            if (parse_i32(argv[++index], -10000, 73000,
                          &profile.initial_snapshot.state.rx1_gain_mdb) != 0) {
                return 2;
            }
            have_rx1_gain = true;
        } else if (strcmp(option, "--rx2-gain-mdb") == 0) {
            if (parse_i32(argv[++index], -10000, 73000,
                          &profile.initial_snapshot.state.rx2_gain_mdb) != 0) {
                return 2;
            }
            have_rx2_gain = true;
        } else if (strcmp(option, "--rx1-gain-mode") == 0) {
            if (parse_gain_mode(
                    argv[++index],
                    &profile.initial_snapshot.state.rx1_gain_mode) != 0) {
                return 2;
            }
            have_rx1_gain_mode = true;
        } else if (strcmp(option, "--rx2-gain-mode") == 0) {
            if (parse_gain_mode(
                    argv[++index],
                    &profile.initial_snapshot.state.rx2_gain_mode) != 0) {
                return 2;
            }
            have_rx2_gain_mode = true;
        } else if (strcmp(option, "--digital-gain-q16-16") == 0) {
            if (parse_i32(argv[++index], 1, INT32_MAX,
                          &profile.initial_snapshot.state.digital_gain_q16_16) !=
                0) {
                return 2;
            }
            have_digital_gain = true;
        } else if (strcmp(option, "--temperature-mc") == 0) {
            if (parse_i32(argv[++index], -40000, 125000,
                          &profile.initial_snapshot.state.temperature_mc) != 0) {
                return 2;
            }
            have_temperature = true;
        } else if (strcmp(option, "--pll-lock-mask") == 0) {
            uint32_t mask;
            if (parse_u32(argv[++index], 0, 3, &mask) != 0) {
                return 2;
            }
            profile.initial_snapshot.state.pll_lock_mask = (uint8_t)mask;
            have_pll_lock = true;
        } else if (strcmp(option, "--device-flags") == 0) {
            if (parse_u32(argv[++index], 0, UINT32_MAX,
                          &profile.initial_snapshot.state.device_flags) != 0) {
                return 2;
            }
            have_device_flags = true;
        } else if (strcmp(option, "--batch") == 0) {
            uint32_t batch;
            if (parse_u32(argv[++index], 1, NDS_MAX_BATCH, &batch) != 0) {
                return 2;
            }
            profile.batch_size = (unsigned)batch;
        } else {
            usage(stderr, argv[0]);
            return 2;
        }
    }
    if (destination_text == NULL || port == 0U || !have_frequency ||
        !have_revision || !have_device_state_revision ||
        !have_state_activation || !have_rx1_gain ||
        ((profile.channel_mask & 2U) != 0U && !have_rx2_gain) ||
        !have_rx1_gain_mode ||
        ((profile.channel_mask & 2U) != 0U && !have_rx2_gain_mode) ||
        !have_digital_gain || !have_temperature || !have_pll_lock ||
        !have_device_flags ||
        nds_parse_destination(&profile.destination, destination_text,
                              (uint16_t)port, (uint16_t)mtu,
                              allow_loopback) != 0) {
        (void)fprintf(stderr,
                      "invalid or over-capacity destination/stream profile\n");
        return 2;
    }
    profile.initial_snapshot.state.metadata_complete = true;
    if (nds_validate_profile(&profile, &wire_rate, NULL) != 0) {
        (void)fprintf(stderr,
                      "invalid or over-capacity destination/stream profile\n");
        return 2;
    }
    (void)fprintf(stderr, "computed Ethernet wire rate: %llu bit/s\n",
                  (unsigned long long)wire_rate);
    result = nds_linux_ring_open(&ring, device);
    if (result != 0) {
        (void)fprintf(stderr, "DMA ring open failed: %s\n", strerror(-result));
        return 1;
    }
    result = nds_service_init(&service, &ring, &profile);
    if (result != 0) {
        (void)fprintf(stderr, "data service initialization failed: %s\n",
                      strerror(-result));
        ring.ops->destroy(&ring);
        return 1;
    }
    (void)signal(SIGINT, handle_signal);
    (void)signal(SIGTERM, handle_signal);
    while (!stop_requested) {
        result = nds_service_step(&service, 1000);
        if (result < 0) {
            (void)fprintf(stderr, "data service failed: %s\n", strerror(-result));
            break;
        }
    }
    nds_service_destroy(&service);
    memset(&dma_stats, 0, sizeof(dma_stats));
    if (ring.ops->get_stats(&ring, &dma_stats) == 0) {
        (void)fprintf(stderr,
                      "sent=%llu send_errors=%llu dma_drops=%llu overruns=%llu\n",
                      (unsigned long long)service.counters.datagrams_sent,
                      (unsigned long long)service.counters.send_errors,
                      (unsigned long long)dma_stats.dropped_blocks,
                      (unsigned long long)dma_stats.overrun_events);
    }
    ring.ops->destroy(&ring);
    return result < 0 ? 1 : 0;
}
