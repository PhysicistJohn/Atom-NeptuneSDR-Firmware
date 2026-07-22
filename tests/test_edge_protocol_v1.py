"""Canonical Neptune Edge v1 schema, binding, and golden-vector proofs."""

import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
DATA_SPEC = ROOT / "specs" / "neptune-edge-data-v1.json"
CONTROL_SPEC = ROOT / "specs" / "neptune-edge-control-v1.json"
LEGACY_INTERFACE = ROOT / "specs" / "p210-firmware-interface-v1.json"
PYTHON_BINDING = ROOT / "protocol" / "generated" / "neptune_edge_v1.py"
C_BINDING = ROOT / "protocol" / "generated" / "neptune_edge_v1.h"
GOLDEN = ROOT / "protocol" / "golden" / "neptune_edge_v1_vectors.json"


def _load_binding():
    spec = importlib.util.spec_from_file_location("neptune_edge_v1_test_binding", PYTHON_BINDING)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load generated Neptune Edge binding")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


EDGE = _load_binding()


class EdgeSchemaTests(unittest.TestCase):
    def test_legacy_interface_and_nsft_v1_remain_immutable(self):
        self.assertEqual(
            hashlib.sha256(LEGACY_INTERFACE.read_bytes()).hexdigest(),
            "e2a95801452d4f5573acf3e13a0000d548382dee82d87644dd2dac231afea27c",
        )
        legacy = json.loads(LEGACY_INTERFACE.read_text(encoding="utf-8"))
        self.assertEqual(legacy["schema"], "neptunesdr.p210-firmware-interface/v1")
        self.assertEqual(legacy["spectrum_stream"]["protocol"], "NSFT-v1")
        self.assertEqual(legacy["spectrum_stream"]["header_bytes"], 68)

    def test_canonical_clock_and_wire_layouts_are_exact(self):
        data = json.loads(DATA_SPEC.read_text(encoding="utf-8"))
        control = json.loads(CONTROL_SPEC.read_text(encoding="utf-8"))
        clock = data["clock_model"]
        self.assertEqual(clock["timestamp_timebase_hz"], 61_440_000)
        self.assertEqual(clock["internal_sample_rate_hz"], 61_440_000)
        self.assertEqual(clock["continuous_egress_sample_rate_hz"], 55_000_000)
        self.assertEqual((clock["interpolation"], clock["decimation"]), (1375, 1536))
        self.assertEqual(61_440_000 * 1375, 55_000_000 * 1536)
        self.assertEqual(data["base_header"]["size_bytes"], 64)
        self.assertEqual(control["base_header"]["size_bytes"], 40)
        self.assertEqual(EDGE.DATA_HEADER_STRUCT.size, 64)
        self.assertEqual(EDGE.CONTROL_HEADER_STRUCT.size, 40)
        self.assertEqual(set(data["sample_formats"]), {"S16", "S12P", "S8", "S8BF", "channel_order"})
        self.assertEqual(set(control["command_contracts"]), set(control["enums"]["command"]))
        self.assertTrue(
            {
                "FFT_METADATA",
                "STFT_METADATA",
                "DETECTOR_EVENT",
                "TRIGGER_CAPTURE",
                "STATUS_SNAPSHOT",
                "VALIDITY_MASK",
                "DUAL_CHANNEL_PRODUCT",
            }
            <= set(data["extensions"])
        )
        self.assertTrue(
            {
                "IDENTITY",
                "HEALTH",
                "PIPELINE_CONFIG",
                "CALIBRATION_CHUNK",
                "TRIGGER_CONFIG",
                "UPDATE_DESCRIPTOR",
                "TX_CONFIG",
            }
            <= set(control["items"])
        )

    def test_generated_hashes_bind_both_canonical_specs(self):
        self.assertEqual(EDGE.DATA_SPEC_SHA256, hashlib.sha256(DATA_SPEC.read_bytes()).hexdigest())
        self.assertEqual(EDGE.CONTROL_SPEC_SHA256, hashlib.sha256(CONTROL_SPEC.read_bytes()).hexdigest())
        c_header = C_BINDING.read_text(encoding="utf-8")
        self.assertIn(EDGE.DATA_SPEC_SHA256, c_header)
        self.assertIn(EDGE.CONTROL_SPEC_SHA256, c_header)

    def test_generator_check_is_nonmutating_and_current(self):
        result = subprocess.run(
            ("python3", "scripts/generate_protocol.py", "--check"),
            cwd=ROOT,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("NEPTUNE_EDGE_V1_GENERATED PASS", result.stdout)


class EdgeBindingTests(unittest.TestCase):
    def test_crc32c_known_answer(self):
        self.assertEqual(EDGE.crc32c(b"123456789"), 0xE3069283)
        self.assertEqual(EDGE.crc32c(b""), 0)

    def test_exact_resampler_state_has_no_long_term_drift(self):
        next_timestamp, next_phase = EDGE.advance_resampler(0, 0, 55_000_000)
        self.assertEqual(next_timestamp, 61_440_000)
        self.assertEqual(next_phase, 0)

        timestamp = 0x12345678
        phase = 123
        sample_count = 4096
        self.assertEqual(EDGE.source_tick(timestamp, phase, 0), timestamp)
        self.assertEqual(
            EDGE.source_tick(timestamp, phase, sample_count),
            EDGE.advance_resampler(timestamp, phase, sample_count)[0],
        )

    def test_iq_format_round_trips_include_signed_boundaries(self):
        native = ((-2048, 2047), (-1, 0), (1, -1), (1024, -1024))
        s16 = EDGE.pack_iq_samples(EDGE.SampleFormat.S16, native)
        self.assertEqual(len(s16), len(native) * 4)
        self.assertEqual(EDGE.unpack_iq_samples(EDGE.SampleFormat.S16, s16), native)

        s12p = EDGE.pack_iq_samples(EDGE.SampleFormat.S12P, native)
        self.assertEqual(s12p[:3], bytes.fromhex("00f87f"))
        self.assertEqual(len(s12p), len(native) * 3)
        self.assertEqual(EDGE.unpack_iq_samples(EDGE.SampleFormat.S12P, s12p), native)

        compact = ((-128, 127), (-1, 0), (1, -1))
        for sample_format in (EDGE.SampleFormat.S8, EDGE.SampleFormat.S8BF):
            with self.subTest(sample_format=sample_format):
                payload = EDGE.pack_iq_samples(sample_format, compact)
                self.assertEqual(EDGE.unpack_iq_samples(sample_format, payload), compact)

    def test_s8_rounding_is_nearest_even_then_saturating(self):
        self.assertEqual(EDGE.quantize_s8(8, 4), 0)
        self.assertEqual(EDGE.quantize_s8(24, 4), 2)
        self.assertEqual(EDGE.quantize_s8(-8, 4), 0)
        self.assertEqual(EDGE.quantize_s8(-24, 4), -2)
        self.assertEqual(EDGE.quantize_s8(2047, 4), 127)
        self.assertEqual(EDGE.quantize_s8(-2048, 4), -128)

    def test_python_binding_rejects_bad_format_and_ranges(self):
        with self.assertRaisesRegex(EDGE.ProtocolError, "signed 12-bit"):
            EDGE.pack_iq_samples(EDGE.SampleFormat.S12P, ((2048, 0),))
        with self.assertRaisesRegex(EDGE.ProtocolError, "length"):
            EDGE.unpack_iq_samples(EDGE.SampleFormat.S12P, b"\x00\x01")

    def test_every_derived_product_has_a_checked_fixed_layout(self):
        timestamp = 0x1234

        def rf_state():
            return EDGE.pack_data_extension(
                "RF_STATE",
                center_frequency_hz=915_000_000,
                sample_rate_hz=61_440_000,
                rf_bandwidth_hz=50_000_000,
                rx1_gain_mdb=20_000,
                rx2_gain_mdb=20_000,
                digital_gain_q16_16=65_536,
                temperature_mc=40_000,
                rx1_gain_mode=0,
                rx2_gain_mode=0,
                pll_lock_mask=3,
                channel_mask=3,
                device_flags=0,
            )

        common = dict(
            flags=EDGE.DataFlag.METADATA_COMPLETE,
            stream_id=7,
            sequence_number=9,
            sample_timestamp=timestamp,
            configuration_revision=1,
            calibration_revision=1,
            discontinuity_revision=0,
            device_state_revision=1,
        )
        fft = EDGE.pack_data_extension(
            "FFT_METADATA",
            fft_size=256,
            bin_count=2,
            first_bin=0,
            window=EDGE.WindowKind.HANN,
            encoding=EDGE.ProductEncoding.POWER_U32,
            stage_shift=8,
            center_offset_hz=0,
            output_rate_hz=61_440_000,
            block_exponent=0,
            reserved16=0,
            overflow_count=0,
        )
        fft_wire = EDGE.pack_data_packet(
            packet_type=EDGE.PacketType.FFT,
            sample_format=EDGE.SampleFormat.NONE,
            sample_count=256,
            channel_mask=1,
            extensions=(rf_state(), fft),
            payload=b"\0" * 8,
            **common,
        )
        self.assertEqual(EDGE.unpack_data_packet(fft_wire)["header"]["packet_type"], EDGE.PacketType.FFT)

        stft = EDGE.pack_data_extension(
            "STFT_METADATA",
            fft_size=4,
            hop_size=2,
            frame_count=2,
            bin_count=2,
            timestamp_reference=EDGE.TimestampReference.FIRST_SAMPLE,
            window=EDGE.WindowKind.HANN,
            encoding=EDGE.ProductEncoding.LOG_POWER_I16_Q8_8,
            stage_shift=2,
            output_rate_hz=61_440_000,
            first_frame_timestamp=timestamp,
            overflow_count=0,
            reserved=0,
        )
        EDGE.unpack_data_packet(
            EDGE.pack_data_packet(
                packet_type=EDGE.PacketType.STFT,
                sample_format=EDGE.SampleFormat.NONE,
                sample_count=6,
                channel_mask=1,
                extensions=(rf_state(), stft),
                payload=b"\0" * 8,
                **common,
            )
        )

        detector = EDGE.pack_data_extension(
            "DETECTOR_EVENT",
            detector_id=1,
            detector_revision=2,
            event_kind=EDGE.DetectorEventKind.PEAK,
            channel=0,
            event_flags=0,
            frequency_hz=915_001_000,
            magnitude_q16_16=100 << 16,
            duration_samples=4,
            start_timestamp=timestamp,
            end_timestamp=timestamp + 4,
        )
        EDGE.unpack_data_packet(
            EDGE.pack_data_packet(
                packet_type=EDGE.PacketType.DETECTOR_EVENT,
                sample_format=EDGE.SampleFormat.NONE,
                sample_count=0,
                channel_mask=1,
                extensions=(rf_state(), detector),
                payload=b"",
                **common,
            )
        )

        validity = EDGE.pack_data_extension(
            "VALIDITY_MASK",
            valid_sample_count=3,
            invalid_sample_count=1,
            first_sample_index=0,
            bit_count=4,
            encoding=EDGE.ProductEncoding.VALIDITY_BITSET_LSB0,
            valid_bit_value=1,
            reserved16=0,
            reason_mask=1,
        )
        EDGE.unpack_data_packet(
            EDGE.pack_data_packet(
                packet_type=EDGE.PacketType.VALIDITY_MASK,
                sample_format=EDGE.SampleFormat.NONE,
                sample_count=4,
                channel_mask=1,
                extensions=(rf_state(), validity),
                payload=b"\x0b",
                **common,
            )
        )

        dual = EDGE.pack_data_extension(
            "DUAL_CHANNEL_PRODUCT",
            product_kind=EDGE.DualChannelProductKind.CROSS_SPECTRUM,
            encoding=EDGE.ProductEncoding.CROSS_COMPLEX_I32,
            reserved8=0,
            fft_size=256,
            element_count=2,
            averaging_count=1,
            rx1_scale_q16_16=65_536,
            rx2_scale_q16_16=65_536,
            reference_timestamp=timestamp,
        )
        EDGE.unpack_data_packet(
            EDGE.pack_data_packet(
                packet_type=EDGE.PacketType.DUAL_CHANNEL_PRODUCT,
                sample_format=EDGE.SampleFormat.NONE,
                sample_count=256,
                channel_mask=3,
                extensions=(rf_state(), dual),
                payload=b"\0" * 16,
                **common,
            )
        )

        trigger = EDGE.pack_data_extension(
            "TRIGGER_CAPTURE",
            capture_id=3,
            segment_index=0,
            segment_count=1,
            trigger_source=EDGE.TriggerSource.SOFTWARE,
            capture_format=EDGE.SampleFormat.S16,
            capture_channel_mask=1,
            trigger_timestamp=timestamp,
            first_timestamp=timestamp,
            pre_trigger_samples=0,
            post_trigger_samples=1,
            capture_flags=0,
        )
        EDGE.unpack_data_packet(
            EDGE.pack_data_packet(
                packet_type=EDGE.PacketType.TRIGGERED_CAPTURE,
                sample_format=EDGE.SampleFormat.S16,
                sample_count=1,
                channel_mask=1,
                extensions=(rf_state(), trigger),
                payload=EDGE.pack_iq_samples(EDGE.SampleFormat.S16, ((1, -1),)),
                **common,
            )
        )

        status = EDGE.pack_data_extension(
            "STATUS_SNAPSHOT",
            uptime_ticks=100,
            temperature_mc=40_000,
            status_flags=0,
            fault_flags=0,
            fifo_high_watermark=1,
            fifo_overflow_count=0,
            dma_starvation_count=0,
            packet_drop_count=0,
            sample_discontinuity_count=0,
            ethernet_backpressure_count=0,
            clipping_count=0,
            active_stream_mask=1,
            fpga_build_id=1,
            firmware_build_id=2,
            ethernet_link_state=1,
            usb_state=1,
            pll_lock_mask=3,
            reserved=0,
        )
        EDGE.unpack_data_packet(
            EDGE.pack_data_packet(
                packet_type=EDGE.PacketType.STATUS_SNAPSHOT,
                sample_format=EDGE.SampleFormat.NONE,
                sample_count=0,
                channel_mask=0,
                extensions=(status,),
                payload=b"",
                **common,
            )
        )

    def test_chunk_items_are_bounded_crc_checked_and_zero_padded(self):
        chunk = b"calibration"
        values = dict(
            transfer_id=1,
            total_length=len(chunk),
            chunk_offset=0,
            chunk_length=len(chunk),
            reserved16=0,
            chunk_crc32c=EDGE.crc32c(chunk),
            data=chunk + (b"\0" * (256 - len(chunk))),
        )
        item = EDGE.pack_control_item("UPDATE_CHUNK", **values)
        message = EDGE.pack_control_message(
            message_kind=EDGE.MessageKind.REQUEST,
            flags=EDGE.ControlFlag.ACK_REQUIRED,
            command_id=EDGE.Command.WRITE_UPDATE_CHUNK,
            status=EDGE.Status.OK,
            transaction_id=1,
            configuration_revision=0,
            activation_timestamp=EDGE.UINT64_MAX,
            items=(item,),
        )
        self.assertEqual(
            EDGE.unpack_control_message(message)["items"][0]["values"]["data"][: len(chunk)],
            chunk,
        )
        invalid = dict(values)
        invalid["data"] = chunk + b"\x01" + (b"\0" * (255 - len(chunk)))
        with self.assertRaisesRegex(EDGE.ProtocolError, "padding"):
            EDGE.pack_control_message(
                message_kind=EDGE.MessageKind.REQUEST,
                flags=0,
                command_id=EDGE.Command.WRITE_UPDATE_CHUNK,
                status=EDGE.Status.OK,
                transaction_id=2,
                configuration_revision=0,
                activation_timestamp=EDGE.UINT64_MAX,
                items=(EDGE.pack_control_item("UPDATE_CHUNK", **invalid),),
            )

    def test_command_contracts_reject_mismatches_and_accept_error_detail(self):
        self.assertEqual(
            EDGE.CONTROL_COMMAND_CONTRACTS[int(EDGE.Command.CREATE_STREAM)],
            {"request": ("STREAM_CONFIG",), "response": ("STREAM_STATUS",)},
        )
        with self.assertRaisesRegex(EDGE.ProtocolError, "command contract"):
            EDGE.pack_control_message(
                message_kind=EDGE.MessageKind.REQUEST,
                flags=EDGE.ControlFlag.ACK_REQUIRED,
                command_id=EDGE.Command.SET_RF,
                status=EDGE.Status.OK,
                transaction_id=1,
                configuration_revision=0,
                activation_timestamp=EDGE.UINT64_MAX,
                items=(),
            )
        with self.assertRaisesRegex(EDGE.ProtocolError, "command contract"):
            EDGE.pack_control_message(
                message_kind=EDGE.MessageKind.RESPONSE,
                flags=0,
                command_id=EDGE.Command.GET_IDENTITY,
                status=EDGE.Status.OK,
                transaction_id=1,
                configuration_revision=0,
                activation_timestamp=EDGE.UINT64_MAX,
                items=(),
            )

        detail = EDGE.pack_control_item(
            "ERROR_DETAIL",
            field_id=1,
            detail_code=2,
            minimum=0,
            maximum=1,
            observed=2,
        )
        error_response = EDGE.pack_control_message(
            message_kind=EDGE.MessageKind.RESPONSE,
            flags=EDGE.ControlFlag.ERROR_DETAIL_PRESENT,
            command_id=EDGE.Command.SET_RF,
            status=EDGE.Status.INVALID_VALUE,
            transaction_id=7,
            configuration_revision=0,
            activation_timestamp=EDGE.UINT64_MAX,
            items=(detail,),
        )
        self.assertEqual(
            EDGE.unpack_control_message(error_response)["items"][0]["name"],
            "ERROR_DETAIL",
        )
        with self.assertRaisesRegex(EDGE.ProtocolError, "ERROR_DETAIL_PRESENT"):
            EDGE.pack_control_message(
                message_kind=EDGE.MessageKind.RESPONSE,
                flags=0,
                command_id=EDGE.Command.SET_RF,
                status=EDGE.Status.INVALID_VALUE,
                transaction_id=8,
                configuration_revision=0,
                activation_timestamp=EDGE.UINT64_MAX,
                items=(detail,),
            )

    def test_c_binding_compiles_and_matches_crc_quantizer_and_s12p(self):
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler, "a C11 compiler is required by the source gate")
        source = r'''
#include <stdint.h>
#include "neptune_edge_v1.h"

int main(void)
{
    static const uint8_t check[] = "123456789";
    uint8_t packed[3];
    int16_t i_value = 0;
    int16_t q_value = 0;
    if (neptune_edge_crc32c(check, 9U) != UINT32_C(0xe3069283)) return 1;
    neptune_edge_pack_s12p(packed, -2048, 2047);
    if (packed[0] != 0x00U || packed[1] != 0xf8U || packed[2] != 0x7fU) return 2;
    neptune_edge_unpack_s12p(packed, &i_value, &q_value);
    if (i_value != -2048 || q_value != 2047) return 3;
    if (neptune_edge_quantize_s8(24, 4) != 2) return 4;
    if (neptune_edge_quantize_s8(8, 4) != 0) return 5;
    if (neptune_edge_quantize_s8(2047, 4) != 127) return 6;
    if (neptune_edge_quantize_s8(-2048, 4) != -128) return 7;
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "edge-v1-c-binding-test"
            compile_result = subprocess.run(
                (
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(C_BINDING.parent),
                    "-x",
                    "c",
                    "-",
                    "-o",
                    str(executable),
                ),
                input=source.encode("utf-8"),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr.decode("utf-8"))
            run_result = subprocess.run((str(executable),), check=False)
            self.assertEqual(run_result.returncode, 0)


class EdgeGoldenVectorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.document = json.loads(GOLDEN.read_text(encoding="utf-8"))
        cls.vectors = {vector["name"]: vector for vector in cls.document["vectors"]}

    def _wire(self, name):
        vector = self.vectors[name]
        wire = bytes.fromhex(vector["wire_hex"])
        self.assertEqual(len(wire), vector["bytes"])
        self.assertEqual(hashlib.sha256(wire).hexdigest(), vector["sha256"])
        return wire

    def test_vector_set_is_complete_and_spec_bound(self):
        self.assertEqual(self.document["schema"], "neptunesdr.edge.golden-vectors/v1")
        self.assertEqual(self.document["data_spec_sha256"], EDGE.DATA_SPEC_SHA256)
        self.assertEqual(self.document["control_spec_sha256"], EDGE.CONTROL_SPEC_SHA256)
        self.assertEqual(
            set(self.vectors),
            {
                "normalized_s8bf_dual_rx_55msps",
                "configuration_state_change",
                "dma_overrun_discontinuity",
                "atomic_set_rf_request",
                "atomic_set_rf_response",
                "iq_payload_s16",
                "iq_payload_s12p",
                "iq_payload_s8",
            },
        )

    def test_normalized_vector_carries_scale_power_and_exact_rate_state(self):
        decoded = EDGE.unpack_data_packet(self._wire("normalized_s8bf_dual_rx_55msps"))
        header = decoded["header"]
        extensions = {item["name"]: item["values"] for item in decoded["extensions"]}
        self.assertEqual(header["packet_type"], EDGE.PacketType.NORMALIZED_IQ)
        self.assertEqual(header["sample_format"], EDGE.SampleFormat.S8BF)
        self.assertEqual(header["sample_count"], 4)
        self.assertEqual(header["channel_mask"], 3)
        self.assertEqual(header["configuration_revision"], 7)
        self.assertEqual(header["calibration_revision"], 3)
        self.assertEqual(extensions["RF_STATE"]["sample_rate_hz"], 55_000_000)
        self.assertEqual(extensions["RF_STATE"]["rx1_gain_mdb"], 30_000)
        self.assertEqual(extensions["RF_STATE"]["rx2_gain_mdb"], 29_500)
        self.assertEqual(extensions["RESAMPLER_STATE"]["input_rate_hz"], 61_440_000)
        self.assertEqual(extensions["RESAMPLER_STATE"]["interpolation"], 1375)
        self.assertEqual(extensions["RESAMPLER_STATE"]["decimation"], 1536)
        self.assertEqual(extensions["QUANTIZATION"]["exponent"], 4)
        self.assertEqual(extensions["QUANTIZATION"]["clipping_count"], 1)
        self.assertEqual(extensions["BLOCK_STATS"]["peak_q16_16"], 0x007F0000)

    def test_state_change_and_discontinuity_are_explicit(self):
        state = EDGE.unpack_data_packet(self._wire("configuration_state_change"))
        self.assertEqual(state["header"]["sample_count"], 0)
        state_extension = state["extensions"][0]["values"]
        self.assertEqual(state_extension["new_configuration_revision"], 8)
        self.assertEqual(state_extension["activation_timestamp"], state["header"]["sample_timestamp"])

        gap = EDGE.unpack_data_packet(self._wire("dma_overrun_discontinuity"))
        self.assertTrue(gap["header"]["flags"] & EDGE.DataFlag.DISCONTINUITY)
        self.assertEqual(gap["header"]["discontinuity_revision"], 1)
        self.assertEqual(gap["extensions"][0]["values"]["reason"], EDGE.DiscontinuityReason.DMA_OVERRUN)
        self.assertEqual(gap["extensions"][0]["values"]["lost_input_samples"], 0xFFFFFFFF)

    def test_atomic_control_request_and_response_share_activation(self):
        request = EDGE.unpack_control_message(self._wire("atomic_set_rf_request"))
        response = EDGE.unpack_control_message(self._wire("atomic_set_rf_response"))
        self.assertEqual(request["header"]["transaction_id"], 0xA1B2C3D4)
        self.assertEqual(response["header"]["transaction_id"], 0xA1B2C3D4)
        self.assertEqual(request["header"]["activation_timestamp"], response["header"]["activation_timestamp"])
        self.assertEqual(request["header"]["configuration_revision"], 7)
        self.assertEqual(response["header"]["configuration_revision"], 8)
        self.assertEqual([item["name"] for item in request["items"]], ["RF_CONFIG", "ATOMIC_COMMIT"])
        self.assertEqual([item["name"] for item in response["items"]], ["STATE_CHANGE"])

    def test_payload_vectors_cover_every_fixed_width_layout(self):
        format_by_name = {
            "iq_payload_s16": EDGE.SampleFormat.S16,
            "iq_payload_s12p": EDGE.SampleFormat.S12P,
            "iq_payload_s8": EDGE.SampleFormat.S8,
        }
        for name, sample_format in format_by_name.items():
            with self.subTest(name=name):
                expected = tuple(tuple(pair) for pair in self.vectors[name]["expected"]["samples"])
                self.assertEqual(EDGE.unpack_iq_samples(sample_format, self._wire(name)), expected)

    def test_corruption_is_detected_in_header_and_payload(self):
        data = bytearray(self._wire("normalized_s8bf_dual_rx_55msps"))
        data[12] ^= 0x01
        with self.assertRaisesRegex(EDGE.ProtocolError, "header CRC"):
            EDGE.unpack_data_packet(data)

        data = bytearray(self._wire("normalized_s8bf_dual_rx_55msps"))
        data[-1] ^= 0x01
        with self.assertRaisesRegex(EDGE.ProtocolError, "payload CRC"):
            EDGE.unpack_data_packet(data)

        control = bytearray(self._wire("atomic_set_rf_request"))
        control[-1] ^= 0x01
        with self.assertRaisesRegex(EDGE.ProtocolError, "payload CRC"):
            EDGE.unpack_control_message(control)

    def test_s8bf_without_quantization_is_rejected(self):
        rf_state = EDGE.pack_data_extension(
            "RF_STATE",
            center_frequency_hz=2_450_000_000,
            sample_rate_hz=55_000_000,
            rf_bandwidth_hz=50_000_000,
            rx1_gain_mdb=30_000,
            rx2_gain_mdb=30_000,
            digital_gain_q16_16=65_536,
            temperature_mc=42_000,
            rx1_gain_mode=0,
            rx2_gain_mode=0,
            pll_lock_mask=3,
            channel_mask=1,
            device_flags=0,
        )
        resampler = EDGE.pack_data_extension(
            "RESAMPLER_STATE",
            input_rate_hz=61_440_000,
            output_rate_hz=55_000_000,
            interpolation=1375,
            decimation=1536,
            phase_numerator=0,
            phase_denominator=1375,
            input_timestamp=1,
            output_sample_index=0,
        )
        with self.assertRaisesRegex(EDGE.ProtocolError, "QUANTIZATION"):
            EDGE.pack_data_packet(
                packet_type=EDGE.PacketType.NORMALIZED_IQ,
                sample_format=EDGE.SampleFormat.S8BF,
                flags=0,
                stream_id=1,
                sequence_number=1,
                sample_timestamp=1,
                sample_count=1,
                channel_mask=1,
                configuration_revision=1,
                calibration_revision=1,
                discontinuity_revision=0,
                device_state_revision=1,
                extensions=(rf_state, resampler),
                payload=b"\x00\x00",
            )


if __name__ == "__main__":
    unittest.main()
