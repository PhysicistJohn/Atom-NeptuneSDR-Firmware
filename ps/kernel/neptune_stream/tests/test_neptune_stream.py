#!/usr/bin/env python3
"""Host-static source and UAPI gates for neptune_stream.

These tests intentionally do not claim Linux-header compilation, module load,
DMA correctness, or hardware validation.
"""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


TEST_ROOT = Path(__file__).resolve().parent
DRIVER_ROOT = TEST_ROOT.parent
PS_ROOT = DRIVER_ROOT.parents[1]
UAPI = PS_ROOT / "include" / "neptune_stream_uapi.h"
SOURCE = DRIVER_ROOT / "neptune_stream.c"
REGISTERS = DRIVER_ROOT / "neptune_stream_regs.h"
PL_REGISTERS = PS_ROOT / "include" / "neptune_pl_registers_v1.h"
PROTOCOL_PL_REGISTERS = PS_ROOT.parent / "protocol" / "generated" / "neptune_pl_registers_v1.h"
WORKFLOW = PS_ROOT.parent / ".github" / "workflows" / "ci.yml"
BINDING = (
    DRIVER_ROOT
    / "Documentation"
    / "devicetree"
    / "bindings"
    / "misc"
    / "hamgeek,neptune-stream-dma-v1.yaml"
)


class NeptuneStreamUAPITests(unittest.TestCase):
    def test_shared_header_compiles_with_exact_layout(self):
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler, "a C compiler is required for the ABI gate")
        with tempfile.TemporaryDirectory(prefix="neptune-uapi-") as temporary:
            executable = Path(temporary) / "uapi-layout"
            command = [
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                "-I",
                str(TEST_ROOT / "include"),
                "-I",
                str(PS_ROOT / "include"),
                str(TEST_ROOT / "uapi_layout.c"),
                "-o",
                str(executable),
            ]
            subprocess.run(command, check=True, capture_output=True, text=True)
            subprocess.run([str(executable)], check=True)

    def test_uapi_is_fixed_width_pointer_free_and_edge_v1_aligned(self):
        text = UAPI.read_text(encoding="utf-8")
        for forbidden in ("unsigned long", "size_t", "void *", "S6P"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, text)
        for identity in (
            "NEPTUNE_STREAM_FORMAT_NONE = 0",
            "NEPTUNE_STREAM_FORMAT_S16 = 1",
            "NEPTUNE_STREAM_FORMAT_S12P = 2",
            "NEPTUNE_STREAM_FORMAT_S8 = 3",
            "NEPTUNE_STREAM_FORMAT_S8BF = 4",
            "NEPTUNE_STREAM_PACKET_VALIDITY_MASK = 11",
            "NEPTUNE_STREAM_PACKET_DUAL_CHANNEL_PRODUCT = 12",
        ):
            self.assertIn(identity, text)
        for field in (
            "source_sequence",
            "sample_timestamp",
            "packet_type",
            "configuration_revision",
            "calibration_revision",
            "device_state_revision",
            "discontinuity_revision",
            "quantization_exponent",
            "resampler_phase_numerator",
            "output_sample_index",
            "clipping_count",
        ):
            self.assertIn(field, text)
        for constant in (
            "NEPTUNE_STREAM_INGRESS_RATE_HZ         61440000U",
            "NEPTUNE_STREAM_EGRESS_RATE_HZ          55000000U",
            "NEPTUNE_STREAM_RESAMPLER_INTERPOLATION 1375U",
            "NEPTUNE_STREAM_RESAMPLER_DECIMATION    1536U",
            "NEPTUNE_STREAM_S8BF_HEADROOM_BITS      1U",
            "NEPTUNE_STREAM_PL_STATUS_SIZE         160U",
            "NEPTUNE_STREAM_IOC_GET_PL_STATUS",
        ):
            self.assertIn(constant, text)


class NeptuneStreamSourceTests(unittest.TestCase):
    def test_driver_owns_coherent_ring_without_copy_read_path(self):
        text = SOURCE.read_text(encoding="utf-8")
        for required in (
            "dma_request_chan",
            "dmaengine_prep_slave_single",
            "dmaengine_submit",
            "dma_async_issue_pending",
            "dmaengine_terminate_sync",
            "dma_alloc_coherent",
            "dma_free_coherent",
            "dma_can_mmap",
            "dma_mmap_coherent",
            "NEPTUNE_SLOT_DMA",
            "NEPTUNE_SLOT_READY",
            "NEPTUNE_SLOT_USER",
            "slot->generation != release.generation",
            "poll_wait",
            "O_RDONLY",
            'ns->miscdev.name = "neptune-stream"',
            'ns->status_miscdev.name = "neptune-pl-status"',
            "NEPTUNE_STREAM_IOC_GET_PL_STATUS",
            "neptune_read_sample_state",
            "NEPTUNE_STREAM_FEAT_PL_STATUS",
            "VM_WRITE | VM_EXEC",
            "VM_MAYWRITE",
            "VM_MAYEXEC",
            ".suppress_bind_attrs = true",
        ):
            with self.subTest(required=required):
                self.assertIn(required, text)
        for forbidden in (
            "/dev/mem",
            "iio_readdev",
            "iio_buffer_refill",
            "remap_pfn_range",
            "virt_to_phys",
            ".read =",
            ".write =",
            "dmam_alloc_coherent",
            "NEPTUNE_REG_",
            "NEPTUNE_STATUS_",
            "NEPTUNE_CONTROL_",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, text)

    def test_source_has_explicit_kernel_and_live_hardware_boundaries(self):
        text = SOURCE.read_text(encoding="utf-8")
        self.assertRegex(
            text,
            r"LINUX_VERSION_CODE < KERNEL_VERSION\(5, 10, 0\)[\s\S]*"
            r"LINUX_VERSION_CODE >= KERNEL_VERSION\(6, 13, 0\)",
        )
        self.assertRegex(
            text,
            r"LINUX_VERSION_CODE >= KERNEL_VERSION\(6, 11, 0\)[\s\S]*"
            r"static void neptune_stream_remove\([\s\S]*#else[\s\S]*"
            r"static int neptune_stream_remove\(",
        )
        for required in (
            "NEPTUNE_PL_REG_MAGIC",
            "NEPTUNE_PL_MAGIC",
            "NEPTUNE_PL_ABI_VERSION",
            "NEPTUNE_STREAM_REQUIRED_PL_CAPS",
            "NEPTUNE_PL_CAP_CALIBRATED_IQ",
            "NEPTUNE_PL_FIELD_STREAM0_FORMAT_PACKET_TYPE_MASK",
            "NEPTUNE_PL_REG_STREAM0_ID",
            "resource_size(mmio) != NEPTUNE_PL_SPAN_BYTES",
            "dma_set_mask_and_coherent(ns->dma_dev, DMA_BIT_MASK(32))",
            "devm_reset_control_get_exclusive",
        ):
            self.assertIn(required, text)

    def test_normalized_continuity_uses_exact_ingress_timebase(self):
        text = SOURCE.read_text(encoding="utf-8")
        for required in (
            "NEPTUNE_STREAM_RESAMPLER_DECIMATION",
            "NEPTUNE_STREAM_RESAMPLER_INTERPOLATION",
            "div_u64_rem",
            "expected_resampler_phase",
            "timestamp + timestamp_advance",
        ):
            self.assertIn(required, text)
        self.assertNotIn(
            "expected_sample_timestamp = timestamp + sample_count;\n\t}\n\tif (ns->output_index_valid",
            text,
        )

    def test_output_sample_index_is_contiguous_for_every_iq_product(self):
        source = SOURCE.read_text(encoding="utf-8")
        readme = (DRIVER_ROOT / "README.md").read_text(encoding="utf-8")
        self.assertIn(
            "output_sample_index != ns->expected_output_sample_index", source
        )
        self.assertIn(
            "expected_output_sample_index = output_sample_index + sample_count",
            source,
        )
        self.assertIn("Every IQ completion carries", readme)
        self.assertIn("including for native RAW and CALIBRATED products", readme)

    def test_normal_start_preserves_pipeline_configuration(self):
        text = SOURCE.read_text(encoding="utf-8")
        start = text.index("static int neptune_hw_start")
        validate = text.index("static bool neptune_validate_header_locked", start)
        body = text[start:validate]
        self.assertIn("neptune_check_hw_contract(ns)", body)
        self.assertNotIn("neptune_hard_reset(ns)", body)

    def test_faults_and_discontinuities_are_observable(self):
        source = SOURCE.read_text(encoding="utf-8")
        header = UAPI.read_text(encoding="utf-8")
        for counter in (
            "produced_blocks",
            "acquired_blocks",
            "released_blocks",
            "dropped_blocks",
            "overrun_events",
            "fifo_errors",
            "dma_errors",
            "interface_errors",
            "malformed_blocks",
            "discontinuities",
        ):
            self.assertIn(counter, source)
            self.assertIn(counter, header)
        self.assertIn("NEPTUNE_STREAM_BLOCK_F_DISCONTINUITY", source)
        self.assertIn("EPOLLERR", source)
        self.assertIn("EPOLLPRI", source)
        self.assertIn("discontinuity_revision == U32_MAX", source)
        self.assertIn("ns->poisoned = true", source)
        self.assertNotIn("discontinuity_revision = 1", source)

    def test_register_contract_is_generated_and_has_no_private_mmio_map(self):
        text = REGISTERS.read_text(encoding="utf-8")
        self.assertIn("#include <neptune_pl_registers_v1.h>", text)
        self.assertIn("DMAEngine", text)
        self.assertNotRegex(text, r"#define\s+NEPTUNE_(?:REG|HW|STATUS|CONTROL)_")
        self.assertEqual(
            PL_REGISTERS.read_text(encoding="utf-8"),
            PROTOCOL_PL_REGISTERS.read_text(encoding="utf-8"),
        )
        generated = PL_REGISTERS.read_text(encoding="utf-8")
        for required in (
            "NEPTUNE_PL_REG_STREAM0_ID",
            "NEPTUNE_PL_FIELD_STREAM0_CONTROL_ENABLE_MASK",
            "NEPTUNE_PL_FIELD_GLOBAL_FAULTS_DMA_OVERRUN_MASK",
            "NEPTUNE_PL_CAP_DMA_SLOT_HEADER_V1",
            "NEPTUNE_PL_CAP_CALIBRATED_IQ",
        ):
            self.assertIn(required, generated)


class NeptuneStreamIntegrationFilesTests(unittest.TestCase):
    def test_generic_kernel_ci_is_explicitly_non_release(self):
        workflow = WORKFLOW.read_text(encoding="utf-8")
        readme = (DRIVER_ROOT / "README.md").read_text(encoding="utf-8")
        self.assertIn("name: Generic Linux API compile (non-release)", workflow)
        self.assertIn(
            "Compile source against runner generic headers only", workflow
        )
        for boundary in (
            "exact target-kernel/config/toolchain",
            "target-tree `dt_binding_check`",
            "final board DTS/DTB hashes",
            "sparse output",
            "live module load",
        ):
            with self.subTest(boundary=boundary):
                self.assertIn(boundary, readme)

    def test_binding_is_strict_and_contains_a_complete_example(self):
        text = BINDING.read_text(encoding="utf-8")
        self.assertIn("additionalProperties: false", text)
        self.assertIn("hamgeek,neptune-stream-dma-v1", text)
        for required in (
            "  - reg",
            "  - interrupts",
            "  - dmas",
            "  - dma-names",
            "  - resets",
            "  - ring-slots",
            "  - slot-bytes",
            "examples:",
            'dma-names = "rx";',
            'interrupt-names = "fault";',
        ):
            self.assertIn(required, text)
        self.assertNotIn("memory-region", text)

    def test_kbuild_files_are_minimal_and_scoped(self):
        kconfig = (DRIVER_ROOT / "Kconfig").read_text(encoding="utf-8")
        makefile = (DRIVER_ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("config NEPTUNE_STREAM", kconfig)
        self.assertIn("depends on ARCH_ZYNQ || COMPILE_TEST", kconfig)
        self.assertIn("DMA_ENGINE", kconfig)
        self.assertIn("KDIR ?=", makefile)
        self.assertIn('M="$(CURDIR)" CONFIG_NEPTUNE_STREAM=m modules', makefile)
        self.assertEqual(
            [line for line in makefile.splitlines() if line.startswith("obj-")],
            ["obj-$(CONFIG_NEPTUNE_STREAM) += neptune_stream.o"],
        )


if __name__ == "__main__":
    unittest.main()
