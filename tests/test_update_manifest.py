"""Signed SD-only update validation tests."""

import base64
from contextlib import redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

import neptunesdr_firmwave.update_manifest as update_manifest_module

from neptunesdr_firmwave.update_manifest import (
    MANIFEST_SCHEMA,
    PLATFORM_ID,
    UpdateManifestError,
    canonical_signing_bytes,
    validate_update_manifest,
)
from neptunesdr_firmwave.cli import main as cli_main


OPENSSL3 = "/opt/homebrew/opt/openssl@3/bin/openssl"


@unittest.skipUnless(Path(OPENSSL3).is_file() or shutil.which("openssl"), "OpenSSL unavailable")
class UpdateManifestTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="neptune-update-test-")
        self.root = Path(self.temporary.name)
        self.openssl = OPENSSL3 if Path(OPENSSL3).is_file() else "openssl"
        self.private = self.root / "private.pem"
        self.public = self.root / "public.pem"
        subprocess.run([self.openssl, "genpkey", "-algorithm", "ED25519", "-out", str(self.private)], check=True, capture_output=True)
        subprocess.run([self.openssl, "pkey", "-in", str(self.private), "-pubout", "-out", str(self.public)], check=True, capture_output=True)
        (self.root / "boot.bin").write_bytes(b"boot-v1")
        (self.root / "rootfs.img").write_bytes(b"rootfs-v1")

    def tearDown(self):
        self.temporary.cleanup()

    def manifest(self):
        artifacts = []
        for name, role in (("boot.bin", "boot"), ("rootfs.img", "rootfs")):
            payload = (self.root / name).read_bytes()
            artifacts.append({"path": name, "role": role, "bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest()})
        value = {
            "schema": MANIFEST_SCHEMA,
            "platform_id": PLATFORM_ID,
            "target_media": "removable-sd",
            "target_slot": "B",
            "qspi_update_allowed": False,
            "rollback_index": 11,
            "signing_key_id": "test-key-1",
            "artifacts": artifacts,
        }
        return self.sign(value)

    def sign(self, value):
        value.pop("signature_base64", None)
        payload = self.root / "canonical.json"
        signature = self.root / "signature.bin"
        payload.write_bytes(canonical_signing_bytes(value))
        subprocess.run([self.openssl, "pkeyutl", "-sign", "-inkey", str(self.private), "-rawin", "-in", str(payload), "-out", str(signature)], check=True, capture_output=True)
        value["signature_base64"] = base64.b64encode(signature.read_bytes()).decode("ascii")
        return value

    def artifact(self, relative, role):
        payload = (self.root / relative).read_bytes()
        return {
            "path": relative,
            "role": role,
            "bytes": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
        }

    def test_signed_update_verifies(self):
        value = validate_update_manifest(self.manifest(), self.root, public_key=self.public, accepted_rollback_index=10, openssl=self.openssl)
        self.assertEqual(value["rollback_index"], 11)

    def test_payload_tamper_and_rollback_are_rejected(self):
        value = self.manifest()
        (self.root / "rootfs.img").write_bytes(b"tampered")
        with self.assertRaises(UpdateManifestError):
            validate_update_manifest(value, self.root, public_key=self.public, accepted_rollback_index=10, openssl=self.openssl)
        (self.root / "rootfs.img").write_bytes(b"rootfs-v1")
        with self.assertRaises(UpdateManifestError):
            validate_update_manifest(value, self.root, public_key=self.public, accepted_rollback_index=11, openssl=self.openssl)

    def test_signature_and_qspi_policy_are_rejected(self):
        value = self.manifest()
        value["qspi_update_allowed"] = True
        with self.assertRaises(UpdateManifestError):
            validate_update_manifest(value, self.root, public_key=self.public, accepted_rollback_index=10, openssl=self.openssl)

    def test_symlinked_final_and_parent_artifact_paths_are_rejected(self):
        final_link = self.manifest()
        (self.root / "boot-real.bin").write_bytes((self.root / "boot.bin").read_bytes())
        (self.root / "boot.bin").unlink()
        (self.root / "boot.bin").symlink_to("boot-real.bin")
        with self.assertRaisesRegex(UpdateManifestError, "unsafe update artifact"):
            validate_update_manifest(
                final_link, self.root, public_key=self.public,
                accepted_rollback_index=10, openssl=self.openssl,
            )

        (self.root / "boot.bin").unlink()
        (self.root / "boot.bin").write_bytes(b"boot-v1")
        real_parent = self.root / "real-parent"
        real_parent.mkdir()
        (real_parent / "boot.bin").write_bytes(b"boot-v1")
        (self.root / "linked-parent").symlink_to(real_parent, target_is_directory=True)
        parent_link = self.manifest()
        parent_link["artifacts"][0] = self.artifact("linked-parent/boot.bin", "boot")
        self.sign(parent_link)
        with self.assertRaisesRegex(UpdateManifestError, "unsafe update artifact parent"):
            validate_update_manifest(
                parent_link, self.root, public_key=self.public,
                accepted_rollback_index=10, openssl=self.openssl,
            )

    def test_symlinked_artifact_root_is_rejected(self):
        value = self.manifest()
        root_alias = self.root / "artifact-root-alias"
        root_alias.symlink_to(self.root, target_is_directory=True)
        with self.assertRaisesRegex(UpdateManifestError, "artifact root"):
            validate_update_manifest(
                value, root_alias, public_key=self.public,
                accepted_rollback_index=10, openssl=self.openssl,
            )

        real_parent = self.root / "real-root-parent"
        nested_root = real_parent / "artifacts"
        nested_root.mkdir(parents=True)
        (nested_root / "boot.bin").write_bytes(b"boot-v1")
        (nested_root / "rootfs.img").write_bytes(b"rootfs-v1")
        parent_alias = self.root / "root-parent-alias"
        parent_alias.symlink_to(real_parent, target_is_directory=True)
        with self.assertRaisesRegex(UpdateManifestError, "artifact root parent"):
            validate_update_manifest(
                value, parent_alias / "artifacts", public_key=self.public,
                accepted_rollback_index=10, openssl=self.openssl,
            )

    def test_resolved_file_aliases_and_duplicate_roles_are_rejected(self):
        alias_value = self.manifest()
        os.link(self.root / "boot.bin", self.root / "boot-alias.bin")
        alias_value["artifacts"].append(self.artifact("boot-alias.bin", "fpga"))
        self.sign(alias_value)
        with self.assertRaisesRegex(UpdateManifestError, "alias the same file"):
            validate_update_manifest(
                alias_value, self.root, public_key=self.public,
                accepted_rollback_index=10, openssl=self.openssl,
            )

        duplicate_role = self.manifest()
        (self.root / "second-boot.bin").write_bytes(b"second-boot")
        duplicate_role["artifacts"].append(self.artifact("second-boot.bin", "boot"))
        self.sign(duplicate_role)
        with self.assertRaisesRegex(UpdateManifestError, "duplicate update artifact role"):
            validate_update_manifest(
                duplicate_role, self.root, public_key=self.public,
                accepted_rollback_index=10, openssl=self.openssl,
            )

    def test_artifact_mutation_during_hash_is_rejected(self):
        value = self.manifest()
        original = update_manifest_module._sha256_fd
        mutated = False

        def mutate_after_hash(descriptor):
            nonlocal mutated
            digest = original(descriptor)
            if not mutated:
                mutated = True
                (self.root / "boot.bin").write_bytes(b"boot-v1-mutated")
            return digest

        with mock.patch.object(
            update_manifest_module, "_sha256_fd", side_effect=mutate_after_hash
        ):
            with self.assertRaisesRegex(UpdateManifestError, "changed during admission"):
                validate_update_manifest(
                    value, self.root, public_key=self.public,
                    accepted_rollback_index=10, openssl=self.openssl,
                )

    def test_trusted_key_is_snapshotted_without_following_final_symlink(self):
        value = self.manifest()
        key_link = self.root / "public-link.pem"
        key_link.symlink_to(self.public)
        with self.assertRaisesRegex(UpdateManifestError, "public key.*unsafe"):
            validate_update_manifest(
                value, self.root, public_key=key_link,
                accepted_rollback_index=10, openssl=self.openssl,
            )

        real_keys = self.root / "real-keys"
        real_keys.mkdir()
        (real_keys / "public.pem").write_bytes(self.public.read_bytes())
        key_parent_link = self.root / "key-parent-link"
        key_parent_link.symlink_to(real_keys, target_is_directory=True)
        with self.assertRaisesRegex(UpdateManifestError, "public key parent.*unsafe"):
            validate_update_manifest(
                value, self.root, public_key=key_parent_link / "public.pem",
                accepted_rollback_index=10, openssl=self.openssl,
            )

    def test_validation_cli_uses_explicit_trust_and_rollback_inputs(self):
        manifest = self.root / "manifest.json"
        manifest.write_text(json.dumps(self.manifest()), encoding="utf-8")
        output = io.StringIO()
        with redirect_stdout(output):
            status = cli_main([
                "validate-update",
                str(manifest),
                "--artifact-root",
                str(self.root),
                "--public-key",
                str(self.public),
                "--accepted-rollback-index",
                "10",
                "--openssl",
                self.openssl,
            ])
        self.assertEqual(status, 0)
        self.assertIn("target_slot=B", output.getvalue())


if __name__ == "__main__":
    unittest.main()
