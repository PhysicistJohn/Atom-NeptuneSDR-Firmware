/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>

#include <neptune_stream_uapi.h>

_Static_assert(sizeof(struct neptune_stream_abi_info) ==
	       NEPTUNE_STREAM_ABI_INFO_SIZE, "ABI info size changed");
_Static_assert(sizeof(struct neptune_stream_config) ==
	       NEPTUNE_STREAM_CONFIG_SIZE, "configuration size changed");
_Static_assert(sizeof(struct neptune_stream_block_header) ==
	       NEPTUNE_STREAM_BLOCK_HEADER_SIZE, "mapped header size changed");
_Static_assert(sizeof(struct neptune_stream_completion) ==
	       NEPTUNE_STREAM_COMPLETION_SIZE, "completion size changed");
_Static_assert(sizeof(struct neptune_stream_release) ==
	       NEPTUNE_STREAM_RELEASE_SIZE, "release size changed");
_Static_assert(sizeof(struct neptune_stream_stats) ==
	       NEPTUNE_STREAM_STATS_SIZE, "stats size changed");
_Static_assert(sizeof(struct neptune_stream_pl_status) ==
	       NEPTUNE_STREAM_PL_STATUS_SIZE, "PL status size changed");

_Static_assert(offsetof(struct neptune_stream_block_header, source_sequence) == 32,
	       "mapped source sequence offset changed");
_Static_assert(offsetof(struct neptune_stream_block_header,
		       configuration_revision) == 48,
	       "mapped revision offset changed");
_Static_assert(offsetof(struct neptune_stream_block_header,
		       quantization_exponent) == 64,
	       "mapped quantization offset changed");
_Static_assert(offsetof(struct neptune_stream_block_header, packet_type) == 80,
	       "mapped packet type offset changed");
_Static_assert(offsetof(struct neptune_stream_block_header,
		       output_sample_index) == 88,
	       "mapped resampler state offset changed");
_Static_assert(offsetof(struct neptune_stream_completion, source_sequence) == 72,
	       "completion sequence offset changed");
_Static_assert(offsetof(struct neptune_stream_completion, packet_type) == 88,
	       "completion packet type offset changed");
_Static_assert(offsetof(struct neptune_stream_completion,
		       output_sample_index) == 96,
	       "completion resampler state offset changed");
_Static_assert(offsetof(struct neptune_stream_stats, produced_blocks) == 32,
	       "stats counter offset changed");
_Static_assert(offsetof(struct neptune_stream_pl_status, pl_build_id) == 16,
	       "PL status build ID offset changed");
_Static_assert(offsetof(struct neptune_stream_pl_status, sample_timestamp) == 40,
	       "PL status sample timestamp offset changed");
_Static_assert(offsetof(struct neptune_stream_pl_status,
		       dma_completed_blocks) == 88,
	       "PL status DMA count offset changed");
_Static_assert(offsetof(struct neptune_stream_pl_status, tx_safety_status) == 96,
	       "PL status TX safety offset changed");

_Static_assert(NEPTUNE_STREAM_FORMAT_NONE == 0, "NONE format identity changed");
_Static_assert(NEPTUNE_STREAM_FORMAT_S16 == 1, "S16 format identity changed");
_Static_assert(NEPTUNE_STREAM_FORMAT_S12P == 2, "S12P format identity changed");
_Static_assert(NEPTUNE_STREAM_FORMAT_S8 == 3, "S8 format identity changed");
_Static_assert(NEPTUNE_STREAM_FORMAT_S8BF == 4, "S8BF format identity changed");
_Static_assert(NEPTUNE_STREAM_PACKET_RAW_IQ == 1,
	       "RAW_IQ packet identity changed");
_Static_assert(NEPTUNE_STREAM_PACKET_CALIBRATED_IQ == 2,
	       "CALIBRATED_IQ packet identity changed");
_Static_assert(NEPTUNE_STREAM_PACKET_NORMALIZED_IQ == 3,
	       "NORMALIZED_IQ packet identity changed");
_Static_assert(NEPTUNE_STREAM_PACKET_DISCONTINUITY == 10,
	       "DISCONTINUITY packet identity changed");
_Static_assert(NEPTUNE_STREAM_BLOCK_MAGIC == 0x4c42534eU,
	       "mapped block magic changed");
_Static_assert((uint64_t)NEPTUNE_STREAM_INGRESS_RATE_HZ *
	       NEPTUNE_STREAM_RESAMPLER_INTERPOLATION ==
	       (uint64_t)NEPTUNE_STREAM_EGRESS_RATE_HZ *
	       NEPTUNE_STREAM_RESAMPLER_DECIMATION,
	       "exact 61.44-to-55 MHz identity changed");
_Static_assert(NEPTUNE_STREAM_RESAMPLER_PHASE_MAX == 1374,
	       "resampler phase range changed");
_Static_assert(NEPTUNE_STREAM_S8BF_EXPONENT_MIN == -31 &&
	       NEPTUNE_STREAM_S8BF_EXPONENT_MAX == 31,
	       "S8BF exponent range changed");
_Static_assert(NEPTUNE_STREAM_S8BF_HEADROOM_BITS == 1,
	       "S8BF headroom policy changed");

_Static_assert(_IOC_SIZE(NEPTUNE_STREAM_IOC_GET_ABI) ==
	       NEPTUNE_STREAM_ABI_INFO_SIZE, "GET_ABI ioctl size changed");
_Static_assert(_IOC_SIZE(NEPTUNE_STREAM_IOC_CONFIGURE) ==
	       NEPTUNE_STREAM_CONFIG_SIZE, "CONFIGURE ioctl size changed");
_Static_assert(_IOC_SIZE(NEPTUNE_STREAM_IOC_ACQUIRE) ==
	       NEPTUNE_STREAM_COMPLETION_SIZE, "ACQUIRE ioctl size changed");
_Static_assert(_IOC_SIZE(NEPTUNE_STREAM_IOC_RELEASE) ==
	       NEPTUNE_STREAM_RELEASE_SIZE, "RELEASE ioctl size changed");
_Static_assert(_IOC_SIZE(NEPTUNE_STREAM_IOC_GET_STATS) ==
	       NEPTUNE_STREAM_STATS_SIZE, "GET_STATS ioctl size changed");
_Static_assert(_IOC_NR(NEPTUNE_STREAM_IOC_RESET_STATS) == 7,
	       "ioctl numbering changed");
_Static_assert(_IOC_SIZE(NEPTUNE_STREAM_IOC_GET_PL_STATUS) ==
	       NEPTUNE_STREAM_PL_STATUS_SIZE, "GET_PL_STATUS ioctl size changed");
_Static_assert(_IOC_NR(NEPTUNE_STREAM_IOC_GET_PL_STATUS) == 8,
	       "GET_PL_STATUS ioctl number changed");

int main(void)
{
	return 0;
}
