/* SPDX-License-Identifier: MIT */
#ifndef NEPTUNE_STREAM_UAPI_H
#define NEPTUNE_STREAM_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * Neptune PL-to-PS streaming ABI v1.
 *
 * ioctl structures use native-endian fixed-width integers and contain no
 * pointers, longs, or architecture-dependent types.  The header at the start
 * of every mmap slot is written by PL in little-endian byte order.  Reserved
 * input fields must be zero and reserved output fields must be ignored.
 */
#define NEPTUNE_STREAM_ABI_MAJOR              1U
#define NEPTUNE_STREAM_ABI_MINOR              0U
#define NEPTUNE_STREAM_BLOCK_MAGIC            0x4c42534eU /* LE bytes "NSBL" */
#define NEPTUNE_STREAM_BLOCK_HEADER_SIZE      128U
#define NEPTUNE_STREAM_MMAP_OFFSET            0ULL
#define NEPTUNE_STREAM_INGRESS_RATE_HZ         61440000U
#define NEPTUNE_STREAM_EGRESS_RATE_HZ          55000000U
#define NEPTUNE_STREAM_RESAMPLER_INTERPOLATION 1375U
#define NEPTUNE_STREAM_RESAMPLER_DECIMATION    1536U
#define NEPTUNE_STREAM_RESAMPLER_PHASE_MAX     \
	(NEPTUNE_STREAM_RESAMPLER_INTERPOLATION - 1U)
#define NEPTUNE_STREAM_S8BF_EXPONENT_MIN       (-31)
#define NEPTUNE_STREAM_S8BF_EXPONENT_MAX       31
#define NEPTUNE_STREAM_S8BF_HEADROOM_BITS      1U

#define NEPTUNE_STREAM_ABI_INFO_SIZE          96U
#define NEPTUNE_STREAM_CONFIG_SIZE            64U
#define NEPTUNE_STREAM_COMPLETION_SIZE        128U
#define NEPTUNE_STREAM_RELEASE_SIZE           64U
#define NEPTUNE_STREAM_STATS_SIZE             128U
#define NEPTUNE_STREAM_PL_STATUS_SIZE         160U

/* Feature flags returned by GET_ABI. */
#define NEPTUNE_STREAM_FEAT_READ_ONLY_MMAP    (1U << 0)
#define NEPTUNE_STREAM_FEAT_POLL              (1U << 1)
#define NEPTUNE_STREAM_FEAT_DMA_COHERENT_RING (1U << 2)
#define NEPTUNE_STREAM_FEAT_EXCLUSIVE_OPEN    (1U << 3)
#define NEPTUNE_STREAM_FEAT_STRICT_GENERATION (1U << 4)
#define NEPTUNE_STREAM_FEAT_ZERO_COPY_RX      (1U << 5)
#define NEPTUNE_STREAM_FEAT_PL_STATUS         (1U << 6)

/* These values intentionally match the locked Neptune Edge v1 wire enum. */
enum neptune_stream_sample_format {
	NEPTUNE_STREAM_FORMAT_NONE = 0,
	NEPTUNE_STREAM_FORMAT_S16 = 1,
	NEPTUNE_STREAM_FORMAT_S12P = 2,
	NEPTUNE_STREAM_FORMAT_S8 = 3,
	NEPTUNE_STREAM_FORMAT_S8BF = 4,
	/* Value 5 is reserved for a future versioned ABI; v1 rejects it. */
};

/* These values intentionally match the locked Neptune Edge v1 wire enum. */
enum neptune_stream_packet_type {
	NEPTUNE_STREAM_PACKET_NONE = 0,
	NEPTUNE_STREAM_PACKET_RAW_IQ = 1,
	NEPTUNE_STREAM_PACKET_CALIBRATED_IQ = 2,
	NEPTUNE_STREAM_PACKET_NORMALIZED_IQ = 3,
	NEPTUNE_STREAM_PACKET_FFT = 4,
	NEPTUNE_STREAM_PACKET_STFT = 5,
	NEPTUNE_STREAM_PACKET_DETECTOR_EVENT = 6,
	NEPTUNE_STREAM_PACKET_TRIGGERED_CAPTURE = 7,
	NEPTUNE_STREAM_PACKET_STATUS = 8,
	NEPTUNE_STREAM_PACKET_STATE_CHANGE = 9,
	NEPTUNE_STREAM_PACKET_DISCONTINUITY = 10,
	NEPTUNE_STREAM_PACKET_VALIDITY_MASK = 11,
	NEPTUNE_STREAM_PACKET_DUAL_CHANNEL_PRODUCT = 12,
};

enum neptune_stream_state {
	NEPTUNE_STREAM_STATE_IDLE = 0,
	NEPTUNE_STREAM_STATE_CONFIGURED = 1,
	NEPTUNE_STREAM_STATE_RUNNING = 2,
	NEPTUNE_STREAM_STATE_ERROR = 3,
};

/* Metadata/fault flags returned by ACQUIRE and stored in each block header. */
#define NEPTUNE_STREAM_BLOCK_F_VALID              (1U << 0)
#define NEPTUNE_STREAM_BLOCK_F_DISCONTINUITY      (1U << 1)
#define NEPTUNE_STREAM_BLOCK_F_OVERFLOW           (1U << 2)
#define NEPTUNE_STREAM_BLOCK_F_RETUNE              (1U << 3)
#define NEPTUNE_STREAM_BLOCK_F_RESTART             (1U << 4)
#define NEPTUNE_STREAM_BLOCK_F_INTERFACE_FAULT     (1U << 5)
#define NEPTUNE_STREAM_BLOCK_F_DMA_FAULT           (1U << 6)
#define NEPTUNE_STREAM_BLOCK_F_FIFO_FAULT          (1U << 7)
#define NEPTUNE_STREAM_BLOCK_F_CLIPPED             (1U << 8)
#define NEPTUNE_STREAM_BLOCK_F_CONFIGURATION_CHANGE (1U << 9)
#define NEPTUNE_STREAM_BLOCK_F_CALIBRATION_CHANGE  (1U << 10)
#define NEPTUNE_STREAM_BLOCK_F_DEVICE_STATE_CHANGE (1U << 11)
#define NEPTUNE_STREAM_BLOCK_F_TIMESTAMP_RESET     (1U << 12)

#define NEPTUNE_STREAM_BLOCK_F_V1_MASK \
	(NEPTUNE_STREAM_BLOCK_F_VALID | \
	 NEPTUNE_STREAM_BLOCK_F_DISCONTINUITY | \
	 NEPTUNE_STREAM_BLOCK_F_OVERFLOW | \
	 NEPTUNE_STREAM_BLOCK_F_RETUNE | \
	 NEPTUNE_STREAM_BLOCK_F_RESTART | \
	 NEPTUNE_STREAM_BLOCK_F_INTERFACE_FAULT | \
	 NEPTUNE_STREAM_BLOCK_F_DMA_FAULT | \
	 NEPTUNE_STREAM_BLOCK_F_FIFO_FAULT | \
	 NEPTUNE_STREAM_BLOCK_F_CLIPPED | \
	 NEPTUNE_STREAM_BLOCK_F_CONFIGURATION_CHANGE | \
	 NEPTUNE_STREAM_BLOCK_F_CALIBRATION_CHANGE | \
	 NEPTUNE_STREAM_BLOCK_F_DEVICE_STATE_CHANGE | \
	 NEPTUNE_STREAM_BLOCK_F_TIMESTAMP_RESET)

struct neptune_stream_abi_info {
	__u32 struct_size;
	__u32 abi_major;
	__u32 abi_minor;
	__u32 feature_flags;
	__u32 slot_count;
	__u32 slot_bytes;
	__u32 slot_header_bytes;
	__u32 payload_capacity;
	__u64 mmap_offset;
	__u64 mmap_length;
	__u32 supported_sample_formats;
	__u32 supported_packet_types;
	__u64 reserved[5];
};

/*
 * CONFIGURE is valid only while stopped. flags and reserved fields are zero.
 * packet_type and sample_format retain their exact Edge-v1 numeric identities.
 */
struct neptune_stream_config {
	__u32 struct_size;
	__u32 flags;
	__u32 stream_id;
	__u32 channel_mask;
	__u32 sample_format;
	__u32 samples_per_slot;
	__u32 packet_type;
	__u32 reserved0;
	__u64 reserved[4];
};

/*
 * PL writes this 128-byte header at offset zero in each slot, followed by the
 * payload at NEPTUNE_STREAM_BLOCK_HEADER_SIZE.  All members are little-endian.
 * The kernel validates it before changing the slot from DMA-owned to ready.
 */
struct neptune_stream_block_header {
	__le32 magic;
	__le32 header_bytes;
	__le32 flags;
	__le32 payload_bytes;
	__le32 stream_id;
	__le32 channel_mask;
	__le32 sample_format;
	__le32 sample_count;
	__le64 source_sequence;
	/* Ingress hardware sample-counter value of the block's first sample. */
	__le64 sample_timestamp;
	__le32 configuration_revision;
	__le32 calibration_revision;
	__le32 device_state_revision;
	__le32 discontinuity_revision;
	/*
	 * Signed two's-complement exponent encoded in one little-endian u32 word.
	 * S8BF reconstructs ADC codes as q * 2^exponent and restricts this to
	 * -31..31; v1 policy is peak scaling, one headroom bit, nearest-even.
	 */
	__le32 quantization_exponent;
	/* Unsigned Q16.16 magnitude summaries in pre-quantization scale. */
	__le32 block_rms_q16;
	__le32 block_peak_q16;
	__le32 clipping_count;
	__le32 packet_type;
	/*
	 * Exact 61.44-to-55 MHz state at output sample zero. After n samples,
	 * total = phase + n * 1536, timestamp += total / 1375, and the next
	 * phase is total % 1375. Native RAW/CALIBRATED requires phase zero.
	 */
	__le32 resampler_phase_numerator;
	/*
	 * Epoch-local payload index, required for every IQ product. It advances
	 * by sample_count and resets only at an explicitly marked new continuity
	 * epoch; it need not equal the absolute ingress sample timestamp.
	 */
	__le64 output_sample_index;
	__le64 reserved[4];
};

/*
 * ACQUIRE is nonblocking: use poll/epoll and retry on EAGAIN.  slot_index and
 * generation form the exact token required by RELEASE.  The payload is at
 * mmap_base + slot_index * slot_bytes + payload_offset.
 */
struct neptune_stream_completion {
	__u32 struct_size;
	__u32 slot_index;
	__u32 generation;
	__u32 flags;
	__u32 payload_offset;
	__u32 payload_bytes;
	__u32 stream_id;
	__u32 channel_mask;
	__u32 sample_format;
	__u32 sample_count;
	__s32 quantization_exponent;
	__u32 block_rms_q16;
	__u32 block_peak_q16;
	__u32 clipping_count;
	__u32 configuration_revision;
	__u32 calibration_revision;
	__u32 device_state_revision;
	__u32 discontinuity_revision;
	__u64 source_sequence;
	__u64 sample_timestamp;
	__u32 packet_type;
	__u32 resampler_phase_numerator;
	__u64 output_sample_index;
	__u64 reserved[3];
};

struct neptune_stream_release {
	__u32 struct_size;
	__u32 slot_index;
	__u32 generation;
	__u32 flags;
	__u64 reserved[6];
};

struct neptune_stream_stats {
	__u32 struct_size;
	__u32 state;
	__u32 slot_count;
	__u32 ready_slots;
	__u32 user_slots;
	__u32 reserved0[3];
	__u64 produced_blocks;
	__u64 acquired_blocks;
	__u64 released_blocks;
	__u64 dropped_blocks;
	__u64 overrun_events;
	__u64 fifo_errors;
	__u64 dma_errors;
	__u64 interface_errors;
	__u64 malformed_blocks;
	__u64 discontinuities;
	__u64 last_source_sequence;
	__u64 last_sample_timestamp;
};

/*
 * Read-only canonical PL snapshot returned on either device node. The sample
 * counter is a coherent low-then-latched-high read. Other counters/status
 * fields can advance while this snapshot is assembled and are diagnostic,
 * not an atomic state-changing transaction.
 */
struct neptune_stream_pl_status {
	__u32 struct_size;
	__u32 flags;
	__u32 pl_magic;
	__u32 pl_abi_version;
	__u64 pl_build_id;
	__u32 pl_capabilities;
	__u32 global_status;
	__u32 global_faults;
	__u32 sample_epoch;
	__u64 sample_timestamp;
	__u32 discontinuity_revision;
	__u32 configuration_revision;
	__u32 calibration_revision;
	__u32 stream_status;
	__u32 stream_id;
	__u32 stream_format;
	__u32 stream_packet_samples;
	__u32 dma_fifo_high_water;
	__u32 dma_fifo_overflows;
	__u32 dma_descriptor_starvations;
	__u64 dma_completed_blocks;
	__u32 tx_safety_status;
	__u32 tx_disarm_revision;
	__u64 reserved[7];
};

#define NEPTUNE_STREAM_IOC_MAGIC       'N'
#define NEPTUNE_STREAM_IOC_GET_ABI \
	_IOR(NEPTUNE_STREAM_IOC_MAGIC, 0x00, struct neptune_stream_abi_info)
#define NEPTUNE_STREAM_IOC_CONFIGURE \
	_IOW(NEPTUNE_STREAM_IOC_MAGIC, 0x01, struct neptune_stream_config)
#define NEPTUNE_STREAM_IOC_START       _IO(NEPTUNE_STREAM_IOC_MAGIC, 0x02)
#define NEPTUNE_STREAM_IOC_STOP        _IO(NEPTUNE_STREAM_IOC_MAGIC, 0x03)
#define NEPTUNE_STREAM_IOC_ACQUIRE \
	_IOR(NEPTUNE_STREAM_IOC_MAGIC, 0x04, struct neptune_stream_completion)
#define NEPTUNE_STREAM_IOC_RELEASE \
	_IOW(NEPTUNE_STREAM_IOC_MAGIC, 0x05, struct neptune_stream_release)
#define NEPTUNE_STREAM_IOC_GET_STATS \
	_IOR(NEPTUNE_STREAM_IOC_MAGIC, 0x06, struct neptune_stream_stats)
#define NEPTUNE_STREAM_IOC_RESET_STATS _IO(NEPTUNE_STREAM_IOC_MAGIC, 0x07)
#define NEPTUNE_STREAM_IOC_GET_PL_STATUS \
	_IOR(NEPTUNE_STREAM_IOC_MAGIC, 0x08, struct neptune_stream_pl_status)

#endif /* NEPTUNE_STREAM_UAPI_H */
