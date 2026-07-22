// SPDX-License-Identifier: GPL-2.0-or-later OR MIT
/*
 * Neptune SDR PL metadata+payload streaming foundation.
 *
 * The device owns one DMA-coherent ring.  PL writes a validated metadata
 * header and payload into DMA-owned slots through a DT-provided DMAEngine
 * channel.  A single userspace consumer acquires completed slots with ioctl,
 * reads them through a read-only mmap, and releases the exact slot+generation
 * token.  Payload bytes never pass through read(2).
 *
 * This source defines a contract, not evidence that a released Neptune
 * bitstream implements it.  Probe fails unless the PL ID, ABI, capabilities,
 * DMA mask, mmap support, reset, IRQ, and DT ring geometry all match.
 */

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/compat.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/math64.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/property.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <neptune_stream_uapi.h>

#include "neptune_stream_regs.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0) || \
	LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
#error "neptune_stream is audited only for Linux >= 5.10 and < 6.13"
#endif

#define NEPTUNE_MIN_SLOTS              4U
#define NEPTUNE_MAX_SLOTS              256U
#define NEPTUNE_MIN_SLOT_BYTES         PAGE_SIZE
#define NEPTUNE_MAX_SLOT_BYTES         (1024U * 1024U)
#define NEPTUNE_MAX_RING_BYTES         (64U * 1024U * 1024U)
#define NEPTUNE_CHANNEL_MASK            0x3U
#define NEPTUNE_STOP_TIMEOUT_US         100000U
#define NEPTUNE_START_TIMEOUT_US        100000U

#define NEPTUNE_FORMAT_BIT(format)      BIT(format)
#define NEPTUNE_PACKET_TYPE_BIT(type)   BIT(type)

enum neptune_slot_owner {
	NEPTUNE_SLOT_FREE = 0,
	NEPTUNE_SLOT_DMA,
	NEPTUNE_SLOT_READY,
	NEPTUNE_SLOT_USER,
};

struct neptune_slot {
	struct neptune_stream_dev *ns;
	u32 index;
	enum neptune_slot_owner owner;
	u32 generation;
	dma_cookie_t cookie;
	struct list_head ready_node;
	struct neptune_stream_completion completion;
};

struct neptune_counters {
	u64 produced_blocks;
	u64 acquired_blocks;
	u64 released_blocks;
	u64 dropped_blocks;
	u64 overrun_events;
	u64 fifo_errors;
	u64 dma_errors;
	u64 interface_errors;
	u64 malformed_blocks;
	u64 discontinuities;
	u64 last_source_sequence;
	u64 last_sample_timestamp;
};

struct neptune_stream_dev {
	struct device *dev;
	struct device *dma_dev;
	void __iomem *regs;
	int irq;
	struct clk *clock;
	struct reset_control *reset;
	struct dma_chan *rx_chan;
	struct work_struct recovery_work;

	void *ring_cpu;
	dma_addr_t ring_dma;
	size_t ring_bytes;
	u32 slot_count;
	u32 slot_bytes;
	u32 block_bytes;
	u32 capabilities;
	u32 supported_formats;
	u32 supported_packet_types;
	struct neptune_slot *slots;

	spinlock_t lock;
	struct mutex ioctl_lock;
	wait_queue_head_t waitq;
	struct list_head ready_list;
	atomic_t opened;
	u32 ready_count;
	u32 user_count;
	u32 state;
	u32 pending_flags;
	u32 discontinuity_revision;
	bool sequence_valid;
	bool timestamp_valid;
	u64 expected_sample_timestamp;
	u32 expected_resampler_phase;
	bool output_index_valid;
	u64 expected_output_sample_index;
	bool configured;
	bool poisoned;
	bool revision_exhausted;
	struct neptune_stream_config config;
	struct neptune_counters counters;

	struct miscdevice miscdev;
	struct miscdevice status_miscdev;
};

static u32 neptune_next_generation(u32 generation)
{
	generation++;
	return generation ? generation : 1U;
}

static bool neptune_all_zero(const void *value, size_t size)
{
	return !memchr_inv(value, 0, size);
}

static void *neptune_slot_address(struct neptune_stream_dev *ns, u32 index)
{
	return (u8 *)ns->ring_cpu + (size_t)index * ns->slot_bytes;
}

static int neptune_payload_bytes(u32 format, u32 channel_mask,
				  u32 sample_count, u32 *result)
{
	u32 bits_per_complex;
	u32 channels = hweight32(channel_mask & NEPTUNE_CHANNEL_MASK);
	u64 total_bits;
	u64 total_bytes;

	if (!sample_count || !channels)
		return -EINVAL;

	switch (format) {
	case NEPTUNE_STREAM_FORMAT_S16:
		bits_per_complex = 32;
		break;
	case NEPTUNE_STREAM_FORMAT_S12P:
		bits_per_complex = 24;
		break;
	case NEPTUNE_STREAM_FORMAT_S8:
	case NEPTUNE_STREAM_FORMAT_S8BF:
		bits_per_complex = 16;
		break;
	default:
		return -EOPNOTSUPP;
	}

	total_bits = (u64)sample_count * channels * bits_per_complex;
	total_bytes = DIV_ROUND_UP_ULL(total_bits, 8);
	if (total_bytes > U32_MAX)
		return -EOVERFLOW;
	*result = (u32)total_bytes;
	return 0;
}

static bool neptune_product_format_supported(u32 packet_type, u32 format)
{
	if (packet_type == NEPTUNE_STREAM_PACKET_RAW_IQ ||
	    packet_type == NEPTUNE_STREAM_PACKET_CALIBRATED_IQ)
		return format == NEPTUNE_STREAM_FORMAT_S16 ||
		       format == NEPTUNE_STREAM_FORMAT_S12P;
	if (packet_type == NEPTUNE_STREAM_PACKET_NORMALIZED_IQ)
		return format == NEPTUNE_STREAM_FORMAT_S8 ||
		       format == NEPTUNE_STREAM_FORMAT_S8BF;
	return false;
}

static int neptune_check_hw_contract(struct neptune_stream_dev *ns)
{
	u32 capabilities;
	u32 formats;
	u32 packet_types;
	u32 magic = readl(ns->regs + NEPTUNE_PL_REG_MAGIC);
	u32 version = readl(ns->regs + NEPTUNE_PL_REG_ABI_VERSION);

	if (magic != NEPTUNE_PL_MAGIC)
		return dev_err_probe(ns->dev, -ENODEV,
				     "PL identity mismatch: %#x\n", magic);
	if (version != NEPTUNE_PL_ABI_VERSION)
		return dev_err_probe(ns->dev, -ENODEV,
				     "unsupported PL ABI %#x\n", version);

	capabilities = readl(ns->regs + NEPTUNE_PL_REG_CAPABILITIES);
	if ((capabilities & NEPTUNE_STREAM_REQUIRED_PL_CAPS) !=
	    NEPTUNE_STREAM_REQUIRED_PL_CAPS)
		return dev_err_probe(ns->dev, -EOPNOTSUPP,
				     "PL capabilities %#x lack required %#x\n",
				     capabilities,
				     NEPTUNE_STREAM_REQUIRED_PL_CAPS);

	/* RAW_BYPASS canonically provides the native S16 and packed-S12 taps. */
	formats = NEPTUNE_FORMAT_BIT(NEPTUNE_STREAM_FORMAT_S16) |
		  NEPTUNE_FORMAT_BIT(NEPTUNE_STREAM_FORMAT_S12P);
	packet_types =
		NEPTUNE_PACKET_TYPE_BIT(NEPTUNE_STREAM_PACKET_RAW_IQ);
	if (capabilities & NEPTUNE_PL_CAP_CALIBRATED_IQ)
		packet_types |= NEPTUNE_PACKET_TYPE_BIT(
			NEPTUNE_STREAM_PACKET_CALIBRATED_IQ);
	if ((capabilities & NEPTUNE_PL_CAP_RESAMPLER_55M) &&
	    (capabilities & NEPTUNE_PL_CAP_S8)) {
		formats |= NEPTUNE_FORMAT_BIT(NEPTUNE_STREAM_FORMAT_S8);
		packet_types |= NEPTUNE_PACKET_TYPE_BIT(
			NEPTUNE_STREAM_PACKET_NORMALIZED_IQ);
	}
	if ((capabilities & NEPTUNE_PL_CAP_RESAMPLER_55M) &&
	    (capabilities & NEPTUNE_PL_CAP_S8BF)) {
		formats |= NEPTUNE_FORMAT_BIT(NEPTUNE_STREAM_FORMAT_S8BF);
		packet_types |= NEPTUNE_PACKET_TYPE_BIT(
			NEPTUNE_STREAM_PACKET_NORMALIZED_IQ);
	}
	ns->capabilities = capabilities;
	ns->supported_formats = formats;
	ns->supported_packet_types = packet_types;
	return 0;
}

static bool neptune_mark_discontinuity_locked(struct neptune_stream_dev *ns,
					       u32 flags)
{
	if (unlikely(ns->discontinuity_revision == U32_MAX)) {
		if (!ns->revision_exhausted) {
			ns->revision_exhausted = true;
			ns->counters.discontinuities++;
			dev_err(ns->dev,
				"discontinuity revision exhausted; device poisoned\n");
		}
		ns->poisoned = true;
		ns->state = NEPTUNE_STREAM_STATE_ERROR;
		return false;
	}
	ns->pending_flags |= flags | NEPTUNE_STREAM_BLOCK_F_DISCONTINUITY;
	ns->discontinuity_revision++;
	ns->counters.discontinuities++;
	return true;
}

static void neptune_reclaim_slots_locked(struct neptune_stream_dev *ns)
{
	u32 index;

	/* READY/USER blocks discarded by stop or process death are observable. */
	ns->counters.dropped_blocks += ns->ready_count + ns->user_count;
	INIT_LIST_HEAD(&ns->ready_list);
	ns->ready_count = 0;
	ns->user_count = 0;
	for (index = 0; index < ns->slot_count; index++) {
		struct neptune_slot *slot = &ns->slots[index];

		slot->owner = NEPTUNE_SLOT_FREE;
		slot->generation = neptune_next_generation(slot->generation);
		slot->cookie = 0;
		INIT_LIST_HEAD(&slot->ready_node);
		memset(&slot->completion, 0, sizeof(slot->completion));
	}
}

static void neptune_reclaim_dma_slots_locked(struct neptune_stream_dev *ns)
{
	u32 index;

	for (index = 0; index < ns->slot_count; index++) {
		struct neptune_slot *slot = &ns->slots[index];

		if (slot->owner != NEPTUNE_SLOT_DMA)
			continue;
		slot->owner = NEPTUNE_SLOT_FREE;
		slot->generation = neptune_next_generation(slot->generation);
		slot->cookie = 0;
	}
}

static u32 neptune_stream_format_register(
	const struct neptune_stream_config *config)
{
	return ((config->packet_type <<
		 NEPTUNE_PL_FIELD_STREAM0_FORMAT_PACKET_TYPE_SHIFT) &
		NEPTUNE_PL_FIELD_STREAM0_FORMAT_PACKET_TYPE_MASK) |
	       ((config->sample_format <<
		 NEPTUNE_PL_FIELD_STREAM0_FORMAT_SAMPLE_FORMAT_SHIFT) &
		NEPTUNE_PL_FIELD_STREAM0_FORMAT_SAMPLE_FORMAT_MASK) |
	       ((config->channel_mask <<
		 NEPTUNE_PL_FIELD_STREAM0_FORMAT_CHANNEL_MASK_SHIFT) &
		NEPTUNE_PL_FIELD_STREAM0_FORMAT_CHANNEL_MASK_MASK);
}

static void neptune_dma_complete(void *opaque);

static int neptune_queue_slot(struct neptune_stream_dev *ns, u32 index,
			      bool issue_pending)
{
	struct dma_async_tx_descriptor *descriptor;
	struct neptune_slot *slot = &ns->slots[index];
	dma_addr_t address = ns->ring_dma + (dma_addr_t)index * ns->slot_bytes;
	unsigned long flags;
	dma_cookie_t cookie;

	spin_lock_irqsave(&ns->lock, flags);
	if (ns->state != NEPTUNE_STREAM_STATE_RUNNING ||
	    slot->owner != NEPTUNE_SLOT_FREE) {
		spin_unlock_irqrestore(&ns->lock, flags);
		return -ECANCELED;
	}
	/* Reserve before prep so no prepared descriptor is abandoned on a fault. */
	slot->owner = NEPTUNE_SLOT_DMA;
	slot->cookie = 0;
	spin_unlock_irqrestore(&ns->lock, flags);

	memset(neptune_slot_address(ns, index), 0, ns->block_bytes);
	dma_wmb();
	descriptor = dmaengine_prep_slave_single(
		ns->rx_chan, address, ns->block_bytes, DMA_DEV_TO_MEM,
		DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!descriptor) {
		spin_lock_irqsave(&ns->lock, flags);
		slot->owner = NEPTUNE_SLOT_FREE;
		spin_unlock_irqrestore(&ns->lock, flags);
		return -EIO;
	}
	descriptor->callback = neptune_dma_complete;
	descriptor->callback_param = slot;

	/* All callers hold ioctl_lock; a fault IRQ may change state, not owner. */
	spin_lock_irqsave(&ns->lock, flags);
	cookie = dmaengine_submit(descriptor);
	if (dma_submit_error(cookie)) {
		slot->owner = NEPTUNE_SLOT_FREE;
		spin_unlock_irqrestore(&ns->lock, flags);
		return cookie;
	}
	slot->cookie = cookie;
	spin_unlock_irqrestore(&ns->lock, flags);
	if (issue_pending)
		dma_async_issue_pending(ns->rx_chan);
	return 0;
}

static int neptune_hard_reset(struct neptune_stream_dev *ns)
{
	int ret = reset_control_reset(ns->reset);

	if (ret) {
		ns->poisoned = true;
		dev_err(ns->dev, "stream reset failed: %d; device poisoned\n", ret);
		return ret;
	}
	ret = neptune_check_hw_contract(ns);
	if (ret)
		ns->poisoned = true;
	return ret;
}

static int neptune_hw_stop(struct neptune_stream_dev *ns)
{
	u32 status;
	int dma_ret;
	int ret;

	writel(0, ns->regs + NEPTUNE_PL_REG_STREAM0_CONTROL);
	ret = readl_poll_timeout(ns->regs + NEPTUNE_PL_REG_STREAM0_STATUS,
				 status,
				 !(status &
				   NEPTUNE_PL_FIELD_STREAM0_STATUS_RUNNING_MASK), 10,
				 NEPTUNE_STOP_TIMEOUT_US);
	dma_ret = dmaengine_terminate_sync(ns->rx_chan);
	synchronize_irq(ns->irq);
	if (dma_ret) {
		ns->poisoned = true;
		dev_err(ns->dev,
			"DMAEngine channel did not terminate: %d; device poisoned\n",
			dma_ret);
		return -EIO;
	}
	if (!ret)
		return 0;

	dev_err(ns->dev, "PL did not quiesce; forcing reset\n");
	if (neptune_hard_reset(ns))
		return -EIO;
	return -ETIMEDOUT;
}

static int neptune_hw_start(struct neptune_stream_dev *ns)
{
	unsigned long irq_flags;
	u32 global_status;
	u32 format;
	u32 index;
	u32 status;
	int ret;

	/* A normal start must preserve sample-timestamped pipeline/calibration state. */
	ret = neptune_check_hw_contract(ns);
	if (ret)
		return ret;
	status = readl(ns->regs + NEPTUNE_PL_REG_STREAM0_STATUS);
	if (status & NEPTUNE_PL_FIELD_STREAM0_STATUS_RUNNING_MASK)
		return -EBUSY;

	ret = readl_poll_timeout(
		ns->regs + NEPTUNE_PL_REG_GLOBAL_STATUS, global_status,
		(global_status &
		 (NEPTUNE_PL_FIELD_GLOBAL_STATUS_RX_PLL_LOCKED_MASK |
		  NEPTUNE_PL_FIELD_GLOBAL_STATUS_AD9361_INTERFACE_READY_MASK)) ==
		(NEPTUNE_PL_FIELD_GLOBAL_STATUS_RX_PLL_LOCKED_MASK |
		 NEPTUNE_PL_FIELD_GLOBAL_STATUS_AD9361_INTERFACE_READY_MASK),
		10, NEPTUNE_START_TIMEOUT_US);
	if (ret)
		return ret;

	memset(ns->ring_cpu, 0, ns->ring_bytes);
	format = neptune_stream_format_register(&ns->config);
	writel(ns->config.stream_id, ns->regs + NEPTUNE_PL_REG_STREAM0_ID);
	writel(format, ns->regs + NEPTUNE_PL_REG_STREAM0_FORMAT);
	writel(ns->config.samples_per_slot,
	       ns->regs + NEPTUNE_PL_REG_STREAM0_PACKET_SAMPLES);
	if (readl(ns->regs + NEPTUNE_PL_REG_STREAM0_ID) !=
	    ns->config.stream_id ||
	    readl(ns->regs + NEPTUNE_PL_REG_STREAM0_FORMAT) != format ||
	    readl(ns->regs + NEPTUNE_PL_REG_STREAM0_PACKET_SAMPLES) !=
	    ns->config.samples_per_slot)
		return -EIO;
	writel(NEPTUNE_STREAM_PL_FAULT_MASK,
	       ns->regs + NEPTUNE_PL_REG_GLOBAL_FAULTS);

	spin_lock_irqsave(&ns->lock, irq_flags);
	neptune_reclaim_slots_locked(ns);
	ns->sequence_valid = false;
	ns->timestamp_valid = false;
	ns->output_index_valid = false;
	ns->state = NEPTUNE_STREAM_STATE_RUNNING;
	if (!neptune_mark_discontinuity_locked(
			ns, NEPTUNE_STREAM_BLOCK_F_RESTART))
		ret = -EOVERFLOW;
	spin_unlock_irqrestore(&ns->lock, irq_flags);
	if (ret)
		goto fail;

	for (index = 0; index < ns->slot_count; index++) {
		ret = neptune_queue_slot(ns, index, false);
		if (ret)
			goto fail;
	}
	dma_async_issue_pending(ns->rx_chan);
	writel(NEPTUNE_PL_FIELD_STREAM0_CONTROL_ENABLE_MASK,
	       ns->regs + NEPTUNE_PL_REG_STREAM0_CONTROL);

	ret = readl_poll_timeout(ns->regs + NEPTUNE_PL_REG_STREAM0_STATUS,
				 status,
				 status &
				 NEPTUNE_PL_FIELD_STREAM0_STATUS_RUNNING_MASK, 10,
				 NEPTUNE_START_TIMEOUT_US);
	if (!ret) {
		spin_lock_irqsave(&ns->lock, irq_flags);
		if (ns->state == NEPTUNE_STREAM_STATE_ERROR)
			ret = -EIO;
		spin_unlock_irqrestore(&ns->lock, irq_flags);
		if (!ret)
			return 0;
	}

fail:
	if (neptune_hw_stop(ns) == -EIO)
		ret = -EIO;
	spin_lock_irqsave(&ns->lock, irq_flags);
	if (ns->poisoned) {
		ns->state = NEPTUNE_STREAM_STATE_ERROR;
	} else {
		neptune_reclaim_slots_locked(ns);
		ns->state = NEPTUNE_STREAM_STATE_CONFIGURED;
	}
	spin_unlock_irqrestore(&ns->lock, irq_flags);
	return ret;
}

static bool neptune_validate_header_locked(struct neptune_stream_dev *ns,
					   u32 index,
					   struct neptune_stream_completion *out)
{
	struct neptune_stream_block_header *header =
		neptune_slot_address(ns, index);
	u32 expected_payload;
	u32 flags;
	u32 payload_bytes;
	u32 phase;
	u32 sample_count;
	s32 quantization_exponent;
	u32 next_phase;
	u32 calibration_revision;
	u64 phase_total;
	u64 timestamp_advance;
	u64 sequence;
	u64 timestamp;
	u64 output_sample_index;
	u32 pl_discontinuity_revision;
	bool continuity_error = false;

	dma_rmb();
	if (le32_to_cpu(READ_ONCE(header->magic)) !=
	    NEPTUNE_STREAM_BLOCK_MAGIC ||
	    le32_to_cpu(READ_ONCE(header->header_bytes)) !=
	    NEPTUNE_STREAM_BLOCK_HEADER_SIZE)
		return false;

	flags = le32_to_cpu(READ_ONCE(header->flags));
	if (!(flags & NEPTUNE_STREAM_BLOCK_F_VALID) ||
	    (flags & ~NEPTUNE_STREAM_BLOCK_F_V1_MASK))
		return false;
	if (le32_to_cpu(READ_ONCE(header->stream_id)) !=
	    ns->config.stream_id ||
	    le32_to_cpu(READ_ONCE(header->channel_mask)) !=
	    ns->config.channel_mask ||
	    le32_to_cpu(READ_ONCE(header->sample_format)) !=
	    ns->config.sample_format ||
	    le32_to_cpu(READ_ONCE(header->packet_type)) !=
	    ns->config.packet_type)
		return false;
	phase = le32_to_cpu(READ_ONCE(header->resampler_phase_numerator));
	if (phase > NEPTUNE_STREAM_RESAMPLER_PHASE_MAX)
		return false;
	if (ns->config.packet_type != NEPTUNE_STREAM_PACKET_NORMALIZED_IQ &&
	    phase)
		return false;
	if (!neptune_all_zero(header->reserved, sizeof(header->reserved)))
		return false;

	sample_count = le32_to_cpu(READ_ONCE(header->sample_count));
	if (sample_count != ns->config.samples_per_slot ||
	    neptune_payload_bytes(ns->config.sample_format,
				 ns->config.channel_mask, sample_count,
				 &expected_payload))
		return false;
	payload_bytes = le32_to_cpu(READ_ONCE(header->payload_bytes));
	if (payload_bytes != expected_payload ||
	    payload_bytes !=
	    ns->block_bytes - NEPTUNE_STREAM_BLOCK_HEADER_SIZE)
		return false;
	quantization_exponent =
		(s32)le32_to_cpu(READ_ONCE(header->quantization_exponent));
	if (ns->config.sample_format == NEPTUNE_STREAM_FORMAT_S8BF &&
	    (quantization_exponent < NEPTUNE_STREAM_S8BF_EXPONENT_MIN ||
	     quantization_exponent > NEPTUNE_STREAM_S8BF_EXPONENT_MAX))
		return false;
	calibration_revision =
		le32_to_cpu(READ_ONCE(header->calibration_revision));
	if (ns->config.packet_type == NEPTUNE_STREAM_PACKET_RAW_IQ &&
	    calibration_revision)
		return false;
	if (ns->config.packet_type == NEPTUNE_STREAM_PACKET_CALIBRATED_IQ &&
	    !calibration_revision)
		return false;

	sequence = le64_to_cpu(READ_ONCE(header->source_sequence));
	timestamp = le64_to_cpu(READ_ONCE(header->sample_timestamp));
	output_sample_index =
		le64_to_cpu(READ_ONCE(header->output_sample_index));
	if (ns->sequence_valid && sequence != ns->counters.last_source_sequence + 1) {
		u64 expected = ns->counters.last_source_sequence + 1;
		u64 lost = sequence > expected ? sequence - expected : 1;

		ns->counters.dropped_blocks += lost;
		continuity_error = true;
	}
	ns->sequence_valid = true;
	ns->counters.last_source_sequence = sequence;
	ns->counters.last_sample_timestamp = timestamp;
	if (ns->timestamp_valid &&
	    (timestamp != ns->expected_sample_timestamp ||
	     phase != ns->expected_resampler_phase))
		continuity_error = true;
	ns->timestamp_valid = true;
	if (ns->config.packet_type == NEPTUNE_STREAM_PACKET_NORMALIZED_IQ) {
		phase_total = (u64)phase +
			      (u64)sample_count *
			      NEPTUNE_STREAM_RESAMPLER_DECIMATION;
		timestamp_advance = div_u64_rem(
			phase_total, NEPTUNE_STREAM_RESAMPLER_INTERPOLATION,
			&next_phase);
		ns->expected_sample_timestamp = timestamp + timestamp_advance;
		ns->expected_resampler_phase = next_phase;
	} else {
		ns->expected_sample_timestamp = timestamp + sample_count;
		ns->expected_resampler_phase = 0;
	}
	if (ns->output_index_valid &&
	    output_sample_index != ns->expected_output_sample_index)
		continuity_error = true;
	ns->output_index_valid = true;
	ns->expected_output_sample_index = output_sample_index + sample_count;
	if (continuity_error && !neptune_mark_discontinuity_locked(ns, 0))
		return false;

	memset(out, 0, sizeof(*out));
	out->struct_size = sizeof(*out);
	out->flags = flags | ns->pending_flags;
	out->payload_offset = NEPTUNE_STREAM_BLOCK_HEADER_SIZE;
	out->payload_bytes = payload_bytes;
	out->stream_id = ns->config.stream_id;
	out->channel_mask = ns->config.channel_mask;
	out->sample_format = ns->config.sample_format;
	out->sample_count = sample_count;
	out->quantization_exponent = quantization_exponent;
	out->block_rms_q16 = le32_to_cpu(READ_ONCE(header->block_rms_q16));
	out->block_peak_q16 = le32_to_cpu(READ_ONCE(header->block_peak_q16));
	out->clipping_count = le32_to_cpu(READ_ONCE(header->clipping_count));
	out->configuration_revision =
		le32_to_cpu(READ_ONCE(header->configuration_revision));
	out->calibration_revision = calibration_revision;
	out->device_state_revision =
		le32_to_cpu(READ_ONCE(header->device_state_revision));
	pl_discontinuity_revision =
		le32_to_cpu(READ_ONCE(header->discontinuity_revision));
	if (pl_discontinuity_revision > ns->discontinuity_revision)
		ns->discontinuity_revision = pl_discontinuity_revision;
	out->discontinuity_revision = ns->discontinuity_revision;
	out->source_sequence = sequence;
	out->sample_timestamp = timestamp;
	out->packet_type = ns->config.packet_type;
	out->resampler_phase_numerator = phase;
	out->output_sample_index = output_sample_index;

	/* Make driver-observed faults visible in both ioctl and mapped metadata. */
	WRITE_ONCE(header->flags, cpu_to_le32(out->flags));
	WRITE_ONCE(header->discontinuity_revision,
		   cpu_to_le32(out->discontinuity_revision));
	dma_wmb();
	ns->pending_flags = 0;
	return true;
}

static void neptune_dma_complete(void *opaque)
{
	struct neptune_slot *slot = opaque;
	struct neptune_stream_dev *ns = slot->ns;
	unsigned long flags;
	bool schedule_recovery = false;

	spin_lock_irqsave(&ns->lock, flags);
	slot->cookie = 0;
	if (slot->owner != NEPTUNE_SLOT_DMA) {
		ns->counters.dma_errors++;
		neptune_mark_discontinuity_locked(ns,
					     NEPTUNE_STREAM_BLOCK_F_DMA_FAULT);
		ns->state = NEPTUNE_STREAM_STATE_ERROR;
		schedule_recovery = true;
	} else if (ns->state != NEPTUNE_STREAM_STATE_RUNNING) {
		slot->owner = NEPTUNE_SLOT_FREE;
		slot->generation = neptune_next_generation(slot->generation);
	} else if (!neptune_validate_header_locked(
			   ns, slot->index, &slot->completion)) {
		ns->counters.malformed_blocks++;
		ns->counters.dropped_blocks++;
		neptune_mark_discontinuity_locked(ns,
					     NEPTUNE_STREAM_BLOCK_F_DMA_FAULT);
		slot->generation = neptune_next_generation(slot->generation);
		slot->owner = NEPTUNE_SLOT_FREE;
		schedule_recovery = true;
	} else {
		slot->owner = NEPTUNE_SLOT_READY;
		list_add_tail(&slot->ready_node, &ns->ready_list);
		ns->ready_count++;
		ns->counters.produced_blocks++;
	}
	if (ns->state == NEPTUNE_STREAM_STATE_ERROR)
		writel(0, ns->regs + NEPTUNE_PL_REG_STREAM0_CONTROL);
	spin_unlock_irqrestore(&ns->lock, flags);

	if (schedule_recovery)
		schedule_work(&ns->recovery_work);
	wake_up_interruptible(&ns->waitq);
}

static irqreturn_t neptune_stream_irq(int irq, void *opaque)
{
	struct neptune_stream_dev *ns = opaque;
	unsigned long flags;
	u32 faults;
	bool fatal = false;

	faults = readl(ns->regs + NEPTUNE_PL_REG_GLOBAL_FAULTS) &
		 NEPTUNE_STREAM_PL_FAULT_MASK;
	if (!faults)
		return IRQ_NONE;
	writel(faults, ns->regs + NEPTUNE_PL_REG_GLOBAL_FAULTS);

	spin_lock_irqsave(&ns->lock, flags);
	if (faults &
	    NEPTUNE_PL_FIELD_GLOBAL_FAULTS_FIFO_OVERFLOW_MASK) {
		ns->counters.overrun_events++;
		ns->counters.fifo_errors++;
		neptune_mark_discontinuity_locked(
			ns, NEPTUNE_STREAM_BLOCK_F_OVERFLOW |
			    NEPTUNE_STREAM_BLOCK_F_FIFO_FAULT);
	}
	if (faults & NEPTUNE_PL_FIELD_GLOBAL_FAULTS_DMA_OVERRUN_MASK) {
		ns->counters.overrun_events++;
		ns->counters.dma_errors++;
		neptune_mark_discontinuity_locked(ns,
					     NEPTUNE_STREAM_BLOCK_F_DMA_FAULT);
		fatal = true;
	}
	if (faults & NEPTUNE_PL_FIELD_GLOBAL_FAULTS_INTERFACE_ERROR_MASK) {
		ns->counters.interface_errors++;
		neptune_mark_discontinuity_locked(
			ns, NEPTUNE_STREAM_BLOCK_F_INTERFACE_FAULT);
		fatal = true;
	}
	fatal = fatal || ns->state == NEPTUNE_STREAM_STATE_ERROR;
	if (fatal) {
		ns->state = NEPTUNE_STREAM_STATE_ERROR;
		writel(0, ns->regs + NEPTUNE_PL_REG_STREAM0_CONTROL);
	}
	spin_unlock_irqrestore(&ns->lock, flags);

	if (fatal)
		schedule_work(&ns->recovery_work);
	wake_up_interruptible(&ns->waitq);
	return IRQ_HANDLED;
}

static void neptune_recovery_work(struct work_struct *work)
{
	struct neptune_stream_dev *ns = container_of(
		work, struct neptune_stream_dev, recovery_work);
	unsigned long flags;
	u32 index;
	bool fatal;
	int ret;

	mutex_lock(&ns->ioctl_lock);
	spin_lock_irqsave(&ns->lock, flags);
	fatal = ns->state == NEPTUNE_STREAM_STATE_ERROR;
	spin_unlock_irqrestore(&ns->lock, flags);
	if (!fatal) {
		for (index = 0; index < ns->slot_count; index++) {
			spin_lock_irqsave(&ns->lock, flags);
			if (ns->state != NEPTUNE_STREAM_STATE_RUNNING) {
				spin_unlock_irqrestore(&ns->lock, flags);
				break;
			}
			if (ns->slots[index].owner != NEPTUNE_SLOT_FREE) {
				spin_unlock_irqrestore(&ns->lock, flags);
				continue;
			}
			spin_unlock_irqrestore(&ns->lock, flags);
			ret = neptune_queue_slot(ns, index, false);
			if (!ret)
				continue;
			spin_lock_irqsave(&ns->lock, flags);
			ns->counters.dma_errors++;
			neptune_mark_discontinuity_locked(
				ns, NEPTUNE_STREAM_BLOCK_F_DMA_FAULT);
			ns->state = NEPTUNE_STREAM_STATE_ERROR;
			spin_unlock_irqrestore(&ns->lock, flags);
			writel(0, ns->regs + NEPTUNE_PL_REG_STREAM0_CONTROL);
			fatal = true;
			break;
		}
		if (!fatal)
			dma_async_issue_pending(ns->rx_chan);
	}

	spin_lock_irqsave(&ns->lock, flags);
	fatal = fatal || ns->state == NEPTUNE_STREAM_STATE_ERROR;
	spin_unlock_irqrestore(&ns->lock, flags);
	if (fatal) {
		ret = dmaengine_terminate_sync(ns->rx_chan);
		spin_lock_irqsave(&ns->lock, flags);
		if (ret) {
			ns->poisoned = true;
			ns->counters.dma_errors++;
		} else {
			neptune_reclaim_dma_slots_locked(ns);
		}
		spin_unlock_irqrestore(&ns->lock, flags);
		if (ret)
			dev_err(ns->dev,
				"DMAEngine fault recovery failed: %d; device poisoned\n",
				ret);
	}
	mutex_unlock(&ns->ioctl_lock);
	wake_up_interruptible(&ns->waitq);
}

static int neptune_stream_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct neptune_stream_dev *ns =
		container_of(misc, struct neptune_stream_dev, miscdev);

	if ((file->f_flags & O_ACCMODE) != O_RDONLY)
		return -EPERM;
	if (atomic_cmpxchg(&ns->opened, 0, 1))
		return -EBUSY;
	if (READ_ONCE(ns->poisoned)) {
		atomic_set(&ns->opened, 0);
		return -EIO;
	}
	file->private_data = ns;
	return nonseekable_open(inode, file);
}

static int neptune_stream_release_file(struct inode *inode, struct file *file)
{
	struct neptune_stream_dev *ns = file->private_data;
	unsigned long flags;
	int stop_ret = 0;

	mutex_lock(&ns->ioctl_lock);
	if (ns->state == NEPTUNE_STREAM_STATE_RUNNING ||
	    ns->state == NEPTUNE_STREAM_STATE_ERROR)
		stop_ret = neptune_hw_stop(ns);
	spin_lock_irqsave(&ns->lock, flags);
	neptune_reclaim_slots_locked(ns);
	ns->configured = false;
	ns->state = ns->poisoned || stop_ret == -EIO ?
		NEPTUNE_STREAM_STATE_ERROR : NEPTUNE_STREAM_STATE_IDLE;
	memset(&ns->config, 0, sizeof(ns->config));
	spin_unlock_irqrestore(&ns->lock, flags);
	mutex_unlock(&ns->ioctl_lock);
	atomic_set(&ns->opened, 0);
	wake_up_interruptible(&ns->waitq);
	return 0;
}

static long neptune_get_abi(struct neptune_stream_dev *ns, void __user *target)
{
	struct neptune_stream_abi_info abi = {
		.struct_size = sizeof(abi),
		.abi_major = NEPTUNE_STREAM_ABI_MAJOR,
		.abi_minor = NEPTUNE_STREAM_ABI_MINOR,
		.feature_flags = NEPTUNE_STREAM_FEAT_READ_ONLY_MMAP |
				 NEPTUNE_STREAM_FEAT_POLL |
				 NEPTUNE_STREAM_FEAT_DMA_COHERENT_RING |
				 NEPTUNE_STREAM_FEAT_EXCLUSIVE_OPEN |
				 NEPTUNE_STREAM_FEAT_STRICT_GENERATION |
				 NEPTUNE_STREAM_FEAT_ZERO_COPY_RX |
				 NEPTUNE_STREAM_FEAT_PL_STATUS,
		.slot_count = ns->slot_count,
		.slot_bytes = ns->slot_bytes,
		.slot_header_bytes = NEPTUNE_STREAM_BLOCK_HEADER_SIZE,
		.payload_capacity = ns->slot_bytes -
				    NEPTUNE_STREAM_BLOCK_HEADER_SIZE,
		.mmap_offset = NEPTUNE_STREAM_MMAP_OFFSET,
		.mmap_length = ns->ring_bytes,
		.supported_sample_formats = ns->supported_formats,
		.supported_packet_types = ns->supported_packet_types,
	};

	return copy_to_user(target, &abi, sizeof(abi)) ? -EFAULT : 0;
}

static int neptune_read_sample_state(struct neptune_stream_dev *ns,
				     u64 *sample_timestamp, u32 *sample_epoch)
{
	u32 epoch_before;
	u32 epoch_after;
	u32 high;
	u32 low;
	unsigned int attempt;

	for (attempt = 0; attempt < 4; attempt++) {
		epoch_before = readl(ns->regs + NEPTUNE_PL_REG_SAMPLE_EPOCH);
		/* Reading LOW latches the matching HIGH word in canonical RTL. */
		low = readl(ns->regs + NEPTUNE_PL_REG_SAMPLE_COUNT_LO);
		high = readl(ns->regs + NEPTUNE_PL_REG_SAMPLE_COUNT_HI);
		epoch_after = readl(ns->regs + NEPTUNE_PL_REG_SAMPLE_EPOCH);
		if (epoch_before == epoch_after) {
			*sample_timestamp = ((u64)high << 32) | low;
			*sample_epoch = epoch_after;
			return 0;
		}
	}
	return -EAGAIN;
}

static int neptune_read_stable_dma_count(struct neptune_stream_dev *ns,
					 u64 *value)
{
	u32 high_before;
	u32 high_after;
	u32 low;
	unsigned int attempt;

	for (attempt = 0; attempt < 4; attempt++) {
		high_before = readl(ns->regs +
				    NEPTUNE_PL_REG_DMA_COMPLETED_BLOCKS_HI);
		low = readl(ns->regs + NEPTUNE_PL_REG_DMA_COMPLETED_BLOCKS_LO);
		high_after = readl(ns->regs +
				   NEPTUNE_PL_REG_DMA_COMPLETED_BLOCKS_HI);
		if (high_before == high_after) {
			*value = ((u64)high_after << 32) | low;
			return 0;
		}
	}
	return -EAGAIN;
}

static long neptune_get_pl_status(struct neptune_stream_dev *ns,
				  void __user *target)
{
	struct neptune_stream_pl_status status = {
		.struct_size = sizeof(status),
	};
	u32 build_low;
	u32 build_high;
	int ret;

	build_low = readl(ns->regs + NEPTUNE_PL_REG_BUILD_ID_LO);
	build_high = readl(ns->regs + NEPTUNE_PL_REG_BUILD_ID_HI);
	status.pl_magic = readl(ns->regs + NEPTUNE_PL_REG_MAGIC);
	status.pl_abi_version = readl(ns->regs + NEPTUNE_PL_REG_ABI_VERSION);
	status.pl_build_id = ((u64)build_high << 32) | build_low;
	status.pl_capabilities = readl(ns->regs + NEPTUNE_PL_REG_CAPABILITIES);
	status.global_status = readl(ns->regs + NEPTUNE_PL_REG_GLOBAL_STATUS);
	status.global_faults = readl(ns->regs + NEPTUNE_PL_REG_GLOBAL_FAULTS);
	ret = neptune_read_sample_state(ns, &status.sample_timestamp,
					&status.sample_epoch);
	if (ret)
		return ret;
	status.discontinuity_revision = readl(
		ns->regs + NEPTUNE_PL_REG_DISCONTINUITY_REVISION);
	status.configuration_revision = readl(
		ns->regs + NEPTUNE_PL_REG_CONFIG_ACTIVE_REVISION);
	status.calibration_revision = readl(
		ns->regs + NEPTUNE_PL_REG_CALIBRATION_ACTIVE_REVISION);
	status.stream_status = readl(ns->regs + NEPTUNE_PL_REG_STREAM0_STATUS);
	status.stream_id = readl(ns->regs + NEPTUNE_PL_REG_STREAM0_ID);
	status.stream_format = readl(ns->regs + NEPTUNE_PL_REG_STREAM0_FORMAT);
	status.stream_packet_samples = readl(
		ns->regs + NEPTUNE_PL_REG_STREAM0_PACKET_SAMPLES);
	status.dma_fifo_high_water = readl(
		ns->regs + NEPTUNE_PL_REG_DMA_FIFO_HIGH_WATER);
	status.dma_fifo_overflows = readl(
		ns->regs + NEPTUNE_PL_REG_DMA_FIFO_OVERFLOWS);
	status.dma_descriptor_starvations = readl(
		ns->regs + NEPTUNE_PL_REG_DMA_DESCRIPTOR_STARVATIONS);
	ret = neptune_read_stable_dma_count(ns, &status.dma_completed_blocks);
	if (ret)
		return ret;
	status.tx_safety_status = readl(
		ns->regs + NEPTUNE_PL_REG_TX_SAFETY_STATUS);
	status.tx_disarm_revision = readl(
		ns->regs + NEPTUNE_PL_REG_TX_DISARM_REVISION);

	return copy_to_user(target, &status, sizeof(status)) ? -EFAULT : 0;
}

static long neptune_configure(struct neptune_stream_dev *ns, void __user *source)
{
	struct neptune_stream_config config;
	unsigned long flags;
	u32 payload_bytes;
	int ret;

	if (copy_from_user(&config, source, sizeof(config)))
		return -EFAULT;
	if (config.struct_size != sizeof(config) || config.flags ||
	    config.reserved0 ||
	    !neptune_all_zero(config.reserved, sizeof(config.reserved)))
		return -EINVAL;
	if (!config.stream_id || !config.channel_mask ||
	    (config.channel_mask & ~NEPTUNE_CHANNEL_MASK))
		return -EINVAL;
	if ((config.channel_mask & BIT(1)) &&
	    !(ns->capabilities & NEPTUNE_PL_CAP_DUAL_RX))
		return -EOPNOTSUPP;
	if (config.sample_format <= NEPTUNE_STREAM_FORMAT_NONE ||
	    config.sample_format > NEPTUNE_STREAM_FORMAT_S8BF ||
	    !(ns->supported_formats & NEPTUNE_FORMAT_BIT(config.sample_format)))
		return -EOPNOTSUPP;
	if (config.packet_type <= NEPTUNE_STREAM_PACKET_NONE ||
	    config.packet_type > NEPTUNE_STREAM_PACKET_NORMALIZED_IQ ||
	    !(ns->supported_packet_types &
	      NEPTUNE_PACKET_TYPE_BIT(config.packet_type)))
		return -EOPNOTSUPP;
	if (!neptune_product_format_supported(config.packet_type,
					      config.sample_format))
		return -EINVAL;
	if (config.packet_type == NEPTUNE_STREAM_PACKET_NORMALIZED_IQ &&
	    hweight32(config.channel_mask) != 1)
		return -EOPNOTSUPP;

	ret = neptune_payload_bytes(config.sample_format, config.channel_mask,
				    config.samples_per_slot, &payload_bytes);
	if (ret)
		return ret;
	if (payload_bytes > ns->slot_bytes - NEPTUNE_STREAM_BLOCK_HEADER_SIZE)
		return -EMSGSIZE;

	spin_lock_irqsave(&ns->lock, flags);
	if (ns->state == NEPTUNE_STREAM_STATE_RUNNING ||
	    ns->state == NEPTUNE_STREAM_STATE_ERROR || ns->user_count) {
		ret = -EBUSY;
	} else {
		ns->config = config;
		ns->block_bytes = NEPTUNE_STREAM_BLOCK_HEADER_SIZE + payload_bytes;
		ns->configured = true;
		ns->state = NEPTUNE_STREAM_STATE_CONFIGURED;
		ret = 0;
	}
	spin_unlock_irqrestore(&ns->lock, flags);
	return ret;
}

static long neptune_start(struct neptune_stream_dev *ns)
{
	if (ns->poisoned)
		return -EIO;
	if (!ns->configured || ns->state != NEPTUNE_STREAM_STATE_CONFIGURED)
		return -EINVAL;
	return neptune_hw_start(ns);
}

static long neptune_stop(struct neptune_stream_dev *ns)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&ns->lock, flags);
	if (ns->state != NEPTUNE_STREAM_STATE_RUNNING &&
	    ns->state != NEPTUNE_STREAM_STATE_ERROR) {
		spin_unlock_irqrestore(&ns->lock, flags);
		return -EINVAL;
	}
	if (ns->user_count) {
		spin_unlock_irqrestore(&ns->lock, flags);
		return -EBUSY;
	}
	spin_unlock_irqrestore(&ns->lock, flags);

	ret = neptune_hw_stop(ns);
	spin_lock_irqsave(&ns->lock, flags);
	if (!ns->poisoned) {
		neptune_reclaim_slots_locked(ns);
		if (neptune_mark_discontinuity_locked(
				ns, NEPTUNE_STREAM_BLOCK_F_RESTART)) {
			ns->state = NEPTUNE_STREAM_STATE_CONFIGURED;
		} else if (!ret) {
			ret = -EOVERFLOW;
		}
	}
	spin_unlock_irqrestore(&ns->lock, flags);
	wake_up_interruptible(&ns->waitq);
	return ret;
}

static long neptune_acquire(struct neptune_stream_dev *ns, void __user *target)
{
	struct neptune_stream_completion completion;
	struct neptune_slot *slot;
	unsigned long flags;
	long ret = 0;

	spin_lock_irqsave(&ns->lock, flags);
	if (list_empty(&ns->ready_list)) {
		ret = ns->state == NEPTUNE_STREAM_STATE_ERROR ? -EIO : -EAGAIN;
		spin_unlock_irqrestore(&ns->lock, flags);
		return ret;
	}
	slot = list_first_entry(&ns->ready_list, struct neptune_slot,
				ready_node);
	list_del_init(&slot->ready_node);
	ns->ready_count--;
	slot->owner = NEPTUNE_SLOT_USER;
	ns->user_count++;
	ns->counters.acquired_blocks++;
	completion = slot->completion;
	completion.slot_index = slot - ns->slots;
	completion.generation = slot->generation;
	spin_unlock_irqrestore(&ns->lock, flags);

	if (!copy_to_user(target, &completion, sizeof(completion)))
		return 0;

	/* ioctl_lock prevents a concurrent RELEASE while copy_to_user executes. */
	spin_lock_irqsave(&ns->lock, flags);
	if (slot->owner == NEPTUNE_SLOT_USER &&
	    slot->generation == completion.generation) {
		slot->owner = NEPTUNE_SLOT_READY;
		list_add(&slot->ready_node, &ns->ready_list);
		ns->ready_count++;
		ns->user_count--;
		ns->counters.acquired_blocks--;
	}
	spin_unlock_irqrestore(&ns->lock, flags);
	return -EFAULT;
}

static long neptune_release_slot(struct neptune_stream_dev *ns,
				 void __user *source)
{
	struct neptune_stream_release release;
	struct neptune_slot *slot;
	unsigned long flags;
	bool return_to_dma;
	bool newly_fatal = false;
	int ret;

	if (copy_from_user(&release, source, sizeof(release)))
		return -EFAULT;
	if (release.struct_size != sizeof(release) || release.flags ||
	    !neptune_all_zero(release.reserved, sizeof(release.reserved)))
		return -EINVAL;
	if (release.slot_index >= ns->slot_count)
		return -ERANGE;

	spin_lock_irqsave(&ns->lock, flags);
	slot = &ns->slots[release.slot_index];
	if (slot->owner != NEPTUNE_SLOT_USER ||
	    slot->generation != release.generation) {
		spin_unlock_irqrestore(&ns->lock, flags);
		return -ESTALE;
	}
	return_to_dma = ns->state == NEPTUNE_STREAM_STATE_RUNNING;
	slot->generation = neptune_next_generation(slot->generation);
	slot->owner = NEPTUNE_SLOT_FREE;
	ns->user_count--;
	ns->counters.released_blocks++;
	spin_unlock_irqrestore(&ns->lock, flags);
	if (!return_to_dma)
		return 0;

	ret = neptune_queue_slot(ns, release.slot_index, true);
	if (!ret)
		return 0;
	spin_lock_irqsave(&ns->lock, flags);
	if (ns->state == NEPTUNE_STREAM_STATE_RUNNING) {
		ns->counters.dma_errors++;
		neptune_mark_discontinuity_locked(ns,
					     NEPTUNE_STREAM_BLOCK_F_DMA_FAULT);
		ns->state = NEPTUNE_STREAM_STATE_ERROR;
		newly_fatal = true;
	}
	spin_unlock_irqrestore(&ns->lock, flags);
	if (newly_fatal) {
		writel(0, ns->regs + NEPTUNE_PL_REG_STREAM0_CONTROL);
		schedule_work(&ns->recovery_work);
	}
	wake_up_interruptible(&ns->waitq);
	return ret == -ECANCELED ? -EIO : ret;
}

static long neptune_get_stats(struct neptune_stream_dev *ns, void __user *target)
{
	struct neptune_stream_stats stats = { .struct_size = sizeof(stats) };
	unsigned long flags;

	spin_lock_irqsave(&ns->lock, flags);
	stats.state = ns->state;
	stats.slot_count = ns->slot_count;
	stats.ready_slots = ns->ready_count;
	stats.user_slots = ns->user_count;
	stats.produced_blocks = ns->counters.produced_blocks;
	stats.acquired_blocks = ns->counters.acquired_blocks;
	stats.released_blocks = ns->counters.released_blocks;
	stats.dropped_blocks = ns->counters.dropped_blocks;
	stats.overrun_events = ns->counters.overrun_events;
	stats.fifo_errors = ns->counters.fifo_errors;
	stats.dma_errors = ns->counters.dma_errors;
	stats.interface_errors = ns->counters.interface_errors;
	stats.malformed_blocks = ns->counters.malformed_blocks;
	stats.discontinuities = ns->counters.discontinuities;
	stats.last_source_sequence = ns->counters.last_source_sequence;
	stats.last_sample_timestamp = ns->counters.last_sample_timestamp;
	spin_unlock_irqrestore(&ns->lock, flags);

	return copy_to_user(target, &stats, sizeof(stats)) ? -EFAULT : 0;
}

static long neptune_reset_stats(struct neptune_stream_dev *ns)
{
	unsigned long flags;

	spin_lock_irqsave(&ns->lock, flags);
	if (ns->state == NEPTUNE_STREAM_STATE_RUNNING || ns->user_count) {
		spin_unlock_irqrestore(&ns->lock, flags);
		return -EBUSY;
	}
	memset(&ns->counters, 0, sizeof(ns->counters));
	ns->sequence_valid = false;
	ns->timestamp_valid = false;
	ns->output_index_valid = false;
	spin_unlock_irqrestore(&ns->lock, flags);
	return 0;
}

static long neptune_stream_ioctl(struct file *file, unsigned int command,
				 unsigned long argument)
{
	struct neptune_stream_dev *ns = file->private_data;
	void __user *pointer = (void __user *)argument;
	long ret;

	if (_IOC_TYPE(command) != NEPTUNE_STREAM_IOC_MAGIC)
		return -ENOTTY;

	mutex_lock(&ns->ioctl_lock);
	switch (command) {
	case NEPTUNE_STREAM_IOC_GET_ABI:
		ret = neptune_get_abi(ns, pointer);
		break;
	case NEPTUNE_STREAM_IOC_CONFIGURE:
		ret = neptune_configure(ns, pointer);
		break;
	case NEPTUNE_STREAM_IOC_START:
		ret = argument ? -EINVAL : neptune_start(ns);
		break;
	case NEPTUNE_STREAM_IOC_STOP:
		ret = argument ? -EINVAL : neptune_stop(ns);
		break;
	case NEPTUNE_STREAM_IOC_ACQUIRE:
		ret = neptune_acquire(ns, pointer);
		break;
	case NEPTUNE_STREAM_IOC_RELEASE:
		ret = neptune_release_slot(ns, pointer);
		break;
	case NEPTUNE_STREAM_IOC_GET_STATS:
		ret = neptune_get_stats(ns, pointer);
		break;
	case NEPTUNE_STREAM_IOC_RESET_STATS:
		ret = argument ? -EINVAL : neptune_reset_stats(ns);
		break;
	case NEPTUNE_STREAM_IOC_GET_PL_STATUS:
		ret = neptune_get_pl_status(ns, pointer);
		break;
	default:
		ret = -ENOTTY;
		break;
	}
	mutex_unlock(&ns->ioctl_lock);
	return ret;
}

static __poll_t neptune_stream_poll(struct file *file, poll_table *wait)
{
	struct neptune_stream_dev *ns = file->private_data;
	unsigned long flags;
	__poll_t mask = 0;

	poll_wait(file, &ns->waitq, wait);
	spin_lock_irqsave(&ns->lock, flags);
	if (ns->ready_count)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (ns->pending_flags)
		mask |= EPOLLPRI;
	if (ns->state == NEPTUNE_STREAM_STATE_ERROR || ns->poisoned)
		mask |= EPOLLERR;
	spin_unlock_irqrestore(&ns->lock, flags);
	return mask;
}

static int neptune_status_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct neptune_stream_dev *ns = container_of(
		misc, struct neptune_stream_dev, status_miscdev);

	if ((file->f_flags & O_ACCMODE) != O_RDONLY)
		return -EPERM;
	file->private_data = ns;
	return nonseekable_open(inode, file);
}

static long neptune_status_ioctl(struct file *file, unsigned int command,
				 unsigned long argument)
{
	struct neptune_stream_dev *ns = file->private_data;
	void __user *pointer = (void __user *)argument;
	long ret;

	if (_IOC_TYPE(command) != NEPTUNE_STREAM_IOC_MAGIC)
		return -ENOTTY;
	mutex_lock(&ns->ioctl_lock);
	switch (command) {
	case NEPTUNE_STREAM_IOC_GET_ABI:
		ret = neptune_get_abi(ns, pointer);
		break;
	case NEPTUNE_STREAM_IOC_GET_PL_STATUS:
		ret = neptune_get_pl_status(ns, pointer);
		break;
	default:
		ret = -ENOTTY;
		break;
	}
	mutex_unlock(&ns->ioctl_lock);
	return ret;
}

static int neptune_stream_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct neptune_stream_dev *ns = file->private_data;
	unsigned long length = vma->vm_end - vma->vm_start;

	if (vma->vm_pgoff != NEPTUNE_STREAM_MMAP_OFFSET / PAGE_SIZE ||
	    length != ns->ring_bytes)
		return -EINVAL;
	if (!(vma->vm_flags & VM_SHARED) ||
	    (vma->vm_flags & (VM_WRITE | VM_EXEC)))
		return -EPERM;
	if (!dma_can_mmap(ns->dma_dev))
		return -EOPNOTSUPP;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	vm_flags_clear(vma, VM_MAYWRITE | VM_MAYEXEC);
#else
	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
	vma->vm_flags &= ~(VM_MAYWRITE | VM_MAYEXEC);
#endif
	return dma_mmap_coherent(ns->dma_dev, vma, ns->ring_cpu, ns->ring_dma,
				 ns->ring_bytes);
}

static const struct file_operations neptune_stream_fops = {
	.owner = THIS_MODULE,
	.open = neptune_stream_open,
	.release = neptune_stream_release_file,
	.unlocked_ioctl = neptune_stream_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
	.poll = neptune_stream_poll,
	.mmap = neptune_stream_mmap,
	.llseek = no_llseek,
};

static const struct file_operations neptune_status_fops = {
	.owner = THIS_MODULE,
	.open = neptune_status_open,
	.unlocked_ioctl = neptune_status_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
	.llseek = no_llseek,
};

static void neptune_clock_disable(void *data)
{
	clk_disable_unprepare(data);
}

static void neptune_reset_assert(void *data)
{
	reset_control_assert(data);
}

static void neptune_dma_channel_release(void *data)
{
	dma_release_channel(data);
}

static void neptune_dma_ring_free(void *data)
{
	struct neptune_stream_dev *ns = data;

	dma_free_coherent(ns->dma_dev, ns->ring_bytes, ns->ring_cpu,
			  ns->ring_dma);
}

static int neptune_stream_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct neptune_stream_dev *ns;
	struct resource *mmio;
	struct dma_slave_caps dma_caps;
	struct dma_slave_config dma_config = {
		.direction = DMA_DEV_TO_MEM,
	};
	size_t ring_bytes;
	u32 index;
	int ret;

	ns = devm_kzalloc(dev, sizeof(*ns), GFP_KERNEL);
	if (!ns)
		return -ENOMEM;
	ns->dev = dev;
	if (!dev->of_node)
		return dev_err_probe(dev, -ENODEV,
				     "a validated device-tree node is required\n");
	spin_lock_init(&ns->lock);
	mutex_init(&ns->ioctl_lock);
	init_waitqueue_head(&ns->waitq);
	INIT_LIST_HEAD(&ns->ready_list);
	INIT_WORK(&ns->recovery_work, neptune_recovery_work);
	atomic_set(&ns->opened, 0);
	ns->state = NEPTUNE_STREAM_STATE_IDLE;

	ret = device_property_read_u32(dev, "ring-slots", &ns->slot_count);
	if (ret)
		return dev_err_probe(dev, ret, "ring-slots is required\n");
	ret = device_property_read_u32(dev, "slot-bytes", &ns->slot_bytes);
	if (ret)
		return dev_err_probe(dev, ret, "slot-bytes is required\n");
	if (ns->slot_count < NEPTUNE_MIN_SLOTS ||
	    ns->slot_count > NEPTUNE_MAX_SLOTS)
		return dev_err_probe(dev, -ERANGE,
				     "ring-slots must be %u..%u\n",
				     NEPTUNE_MIN_SLOTS, NEPTUNE_MAX_SLOTS);
	if (ns->slot_bytes < NEPTUNE_MIN_SLOT_BYTES ||
	    ns->slot_bytes > NEPTUNE_MAX_SLOT_BYTES ||
	    !IS_ALIGNED(ns->slot_bytes, PAGE_SIZE))
		return dev_err_probe(dev, -ERANGE,
				     "slot-bytes must be page aligned and %lu..%u\n",
				     NEPTUNE_MIN_SLOT_BYTES,
				     NEPTUNE_MAX_SLOT_BYTES);
	if (check_mul_overflow((size_t)ns->slot_count,
			       (size_t)ns->slot_bytes, &ring_bytes) ||
	    ring_bytes > NEPTUNE_MAX_RING_BYTES)
		return dev_err_probe(dev, -EOVERFLOW,
				     "DMA ring exceeds %u bytes\n",
				     NEPTUNE_MAX_RING_BYTES);
	ns->ring_bytes = ring_bytes;

	ns->clock = devm_clk_get_optional(dev, NULL);
	if (IS_ERR(ns->clock))
		return dev_err_probe(dev, PTR_ERR(ns->clock),
				     "cannot get stream clock\n");
	ret = clk_prepare_enable(ns->clock);
	if (ret)
		return dev_err_probe(dev, ret, "cannot enable stream clock\n");
	ret = devm_add_action_or_reset(dev, neptune_clock_disable, ns->clock);
	if (ret)
		return ret;

	ns->reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(ns->reset))
		return dev_err_probe(dev, PTR_ERR(ns->reset),
				     "a dedicated stream reset is required\n");
	ret = reset_control_deassert(ns->reset);
	if (ret)
		return dev_err_probe(dev, ret, "cannot deassert stream reset\n");
	ret = devm_add_action_or_reset(dev, neptune_reset_assert, ns->reset);
	if (ret)
		return ret;

	ns->rx_chan = dma_request_chan(dev, "rx");
	if (IS_ERR(ns->rx_chan))
		return dev_err_probe(dev, PTR_ERR(ns->rx_chan),
				     "cannot request rx DMAEngine channel\n");
	ret = devm_add_action_or_reset(dev, neptune_dma_channel_release,
				       ns->rx_chan);
	if (ret)
		return ret;
	ns->dma_dev = dmaengine_get_dma_device(ns->rx_chan);
	if (!ns->dma_dev)
		return dev_err_probe(dev, -ENODEV,
				     "DMAEngine channel has no DMA device\n");
	ret = dma_get_slave_caps(ns->rx_chan, &dma_caps);
	if (ret)
		return dev_err_probe(dev, ret,
				     "cannot query DMAEngine capabilities\n");
	if (!(dma_caps.directions & BIT(DMA_DEV_TO_MEM)))
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "DMAEngine channel cannot receive into memory\n");
	ret = dmaengine_slave_config(ns->rx_chan, &dma_config);
	if (ret)
		return dev_err_probe(dev, ret,
				     "cannot configure rx DMAEngine channel\n");

	ret = dma_set_mask_and_coherent(ns->dma_dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "32-bit coherent DMA unavailable\n");
	if (!dma_can_mmap(ns->dma_dev))
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "DMA allocation cannot be mapped to userspace\n");

	mmio = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mmio || resource_size(mmio) != NEPTUNE_PL_SPAN_BYTES)
		return dev_err_probe(dev, -EINVAL,
				     "canonical PL MMIO resource must be exactly %#x bytes\n",
				     NEPTUNE_PL_SPAN_BYTES);
	ns->regs = devm_ioremap_resource(dev, mmio);
	if (IS_ERR(ns->regs))
		return PTR_ERR(ns->regs);
	ret = neptune_hard_reset(ns);
	if (ret)
		return ret;

	ns->irq = platform_get_irq(pdev, 0);
	if (ns->irq < 0)
		return ns->irq;
	writel(0, ns->regs + NEPTUNE_PL_REG_STREAM0_CONTROL);
	writel(NEPTUNE_STREAM_PL_FAULT_MASK,
	       ns->regs + NEPTUNE_PL_REG_GLOBAL_FAULTS);
	ret = devm_request_irq(dev, ns->irq, neptune_stream_irq, 0,
			       dev_name(dev), ns);
	if (ret)
		return dev_err_probe(dev, ret, "cannot request PL fault IRQ\n");

	ns->slots = devm_kcalloc(dev, ns->slot_count, sizeof(*ns->slots),
				 GFP_KERNEL);
	if (!ns->slots)
		return -ENOMEM;
	ns->ring_cpu = dma_alloc_coherent(ns->dma_dev, ns->ring_bytes,
					  &ns->ring_dma, GFP_KERNEL);
	if (!ns->ring_cpu)
		return dev_err_probe(dev, -ENOMEM,
				     "cannot allocate %zu-byte coherent ring\n",
				     ns->ring_bytes);
	ret = devm_add_action_or_reset(dev, neptune_dma_ring_free, ns);
	if (ret)
		return ret;
	if (upper_32_bits(ns->ring_dma) ||
	    ns->ring_bytes - 1 >
	    (size_t)(U32_MAX - lower_32_bits(ns->ring_dma)))
		return dev_err_probe(dev, -ERANGE,
				     "coherent ring crosses the DMA32 limit\n");
	memset(ns->ring_cpu, 0, ns->ring_bytes);
	for (index = 0; index < ns->slot_count; index++) {
		ns->slots[index].ns = ns;
		ns->slots[index].index = index;
		INIT_LIST_HEAD(&ns->slots[index].ready_node);
	}

	ns->miscdev.minor = MISC_DYNAMIC_MINOR;
	ns->miscdev.name = "neptune-stream";
	ns->miscdev.fops = &neptune_stream_fops;
	ns->miscdev.parent = dev;
	ret = misc_register(&ns->miscdev);
	if (ret)
		return dev_err_probe(dev, ret, "cannot register misc device\n");
	ns->status_miscdev.minor = MISC_DYNAMIC_MINOR;
	ns->status_miscdev.name = "neptune-pl-status";
	ns->status_miscdev.fops = &neptune_status_fops;
	ns->status_miscdev.parent = dev;
	ret = misc_register(&ns->status_miscdev);
	if (ret) {
		misc_deregister(&ns->miscdev);
		return dev_err_probe(dev, ret,
				     "cannot register PL status device\n");
	}

	platform_set_drvdata(pdev, ns);
	dev_info(dev,
		 "%s: %u x %u-byte coherent slots, formats %#x, packet types %#x\n",
		 ns->miscdev.name, ns->slot_count, ns->slot_bytes,
		 ns->supported_formats, ns->supported_packet_types);
	return 0;
}

static void neptune_stream_remove_common(struct platform_device *pdev)
{
	struct neptune_stream_dev *ns = platform_get_drvdata(pdev);
	int ret;

	/* Manual unbind is suppressed; an open descriptor pins THIS_MODULE. */
	misc_deregister(&ns->status_miscdev);
	misc_deregister(&ns->miscdev);
	mutex_lock(&ns->ioctl_lock);
	if (WARN_ON(atomic_read(&ns->opened)))
		ns->poisoned = true;
	writel(0, ns->regs + NEPTUNE_PL_REG_STREAM0_CONTROL);
	disable_irq(ns->irq);
	ret = dmaengine_terminate_sync(ns->rx_chan);
	mutex_unlock(&ns->ioctl_lock);
	cancel_work_sync(&ns->recovery_work);
	if (ret)
		dev_err(ns->dev, "DMAEngine remove termination failed: %d\n", ret);
	ret = reset_control_assert(ns->reset);
	if (ret)
		dev_err(ns->dev, "stream reset assertion failed on remove: %d\n",
			ret);
}

/* platform_driver::remove changed from int to void in Linux 6.11. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static void neptune_stream_remove(struct platform_device *pdev)
{
	neptune_stream_remove_common(pdev);
}
#else
static int neptune_stream_remove(struct platform_device *pdev)
{
	neptune_stream_remove_common(pdev);
	return 0;
}
#endif

static void neptune_stream_shutdown(struct platform_device *pdev)
{
	struct neptune_stream_dev *ns = platform_get_drvdata(pdev);

	mutex_lock(&ns->ioctl_lock);
	writel(0, ns->regs + NEPTUNE_PL_REG_STREAM0_CONTROL);
	disable_irq(ns->irq);
	dmaengine_terminate_sync(ns->rx_chan);
	mutex_unlock(&ns->ioctl_lock);
	cancel_work_sync(&ns->recovery_work);
	reset_control_assert(ns->reset);
}

static const struct of_device_id neptune_stream_of_match[] = {
	{ .compatible = "hamgeek,neptune-stream-dma-v1" },
	{ }
};
MODULE_DEVICE_TABLE(of, neptune_stream_of_match);

static struct platform_driver neptune_stream_driver = {
	.probe = neptune_stream_probe,
	.remove = neptune_stream_remove,
	.shutdown = neptune_stream_shutdown,
	.driver = {
		.name = "neptune-stream",
		.of_match_table = neptune_stream_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(neptune_stream_driver);

MODULE_DESCRIPTION("Neptune SDR fail-closed PL DMA ring driver");
MODULE_AUTHOR("Atom-NeptuneSDR_Firmwave contributors");
MODULE_LICENSE("Dual MIT/GPL");
