"""The custom firmware/twin seam is one explicit, fully hashed profile."""

import hashlib
import json
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / "protocol" / "generated" / "neptune_edge_profile_v1.json"


class EdgeProfileManifestTests(unittest.TestCase):
    def test_profile_is_nonrelease_and_binds_every_declared_artifact(self):
        document = json.loads(PROFILE.read_text(encoding="utf-8"))
        self.assertEqual(document["schema"], "neptunesdr.edge.firmware-profile/v1")
        self.assertEqual(document["profile"], "p210-edge-custom-v0")
        self.assertFalse(document["release_ready"])
        self.assertTrue(document["physical_validation_required"])
        self.assertEqual(document["clock_contract"]["ingress_sample_rate_hz"], 61_440_000)
        self.assertEqual(document["clock_contract"]["continuous_ethernet_sample_rate_hz"], 55_000_000)
        self.assertGreaterEqual(len(document["artifacts"]), 12)
        for name, artifact in document["artifacts"].items():
            with self.subTest(artifact=name):
                path = ROOT / artifact["path"]
                payload = path.read_bytes()
                self.assertEqual(artifact["bytes"], len(payload))
                self.assertEqual(artifact["sha256"], hashlib.sha256(payload).hexdigest())

    def test_profile_generator_is_current(self):
        result = subprocess.run(
            ("python3", "scripts/generate_edge_profile.py", "--check"),
            cwd=ROOT,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("NEPTUNE_EDGE_PROFILE_V1_GENERATED PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
