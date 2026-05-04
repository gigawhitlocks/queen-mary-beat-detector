#!/usr/bin/env python3
"""Test default-mode BPM detection against known reference BPMs.

Runs ../analyze_audio (default mode, no flags) on each WAV file and
compares the extracted BPM to the expected value with 2-decimal-place
accuracy.

Files must exist on disk; the test uses the *actual* filenames found
in the song-tests and metronome-ticks directories where they differ
from the canonical reference table (e.g. "manaya" vs "manyana").
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

# ------------------------------------------------------------------
# Configuration
# ------------------------------------------------------------------

DIR = Path(__file__).resolve().parent          # …/extracted_qm_bpm/tests
ANALYZER = DIR.parent / "analyze_audio"       # …/extracted_qm_bpm/analyze_audio

# (relative_path, expected_bpm)
# Relative paths are joined with DIR so they resolve even when cwd
# differs.  Filenames here match what actually exists on disk.
TESTS: list[tuple[str, float]] = [
    (
        "metronome-ticks/metronome-ticks_4-4_120-BPM.wav",
        120.00,
    ),
    (
        "metronome-ticks/metronome-ticks_4-4_129-BPM.wav",
        129.00,
    ),
    (
        "metronome-ticks/metronome-ticks_4-4_144-BPM.wav",
        144.00,
    ),
    (
        "metronome-ticks/metronome-ticks_4-4_60-BPM.wav",
        60.00,
    ),
    (
        "metronome-ticks/metronome-ticks_4-4_88-BPM.wav",
        88.00,
    ),
    (
        "song-tests/01 - Duvet-excerpt.wav",
        93.33,
    ),
    (
        "song-tests/01 - Duvet.wav",  # on disk as "01" not "1"
        93.12,
    ),
    (
        "song-tests/06 - aNYway-excerpt.wav",  # on disk as "06" not "6"
        128.00,
    ),
    (
        "song-tests/06 - aNYway.wav",
        128.00,
    ),
    (
        "song-tests/07 - Jake's House-excerpt.wav",  # on disk as "07" not "7"
        118.00,
    ),
    (
        "song-tests/07 - Jake's House.wav",
        118.00,
    ),
    (
        "song-tests/08 - Television Rules the Nation-excerpt.wav",  # "08"
        115.33,
    ),
    (
        "song-tests/08 - Television Rules the Nation.wav",
        115.32,
    ),
    (
        "song-tests/manaya.wav",  # on disk as "manaya" not "manyana"
        128.00,
    ),
    (
        "song-tests/manaya-excerpt.wav",
        128.00,
    ),
]


# ------------------------------------------------------------------
# Helpers
# ------------------------------------------------------------------

def extract_bpm(output: str) -> float | None:
    """Return the BPM number from analyze_audio stdout or None."""
    # Lines look like:  "BPM: 120  (method: musical)"
    m = re.search(r"BPM:\s*([\d.]+)", output)
    return float(m.group(1)) if m else None


def run_analyzer(rel_path: str) -> tuple[str, int]:
    """Run analyze_audio on *rel_path* (relative to this script's dir).
    Returns (stdout, returncode).  The analyzer is always invoked with
    **no** extra flags so we test the default mode.
    """
    full = str(DIR / rel_path)
    result = subprocess.run(
        [str(ANALYZER), full],
        capture_output=True,
        text=True,
        timeout=60,
    )
    return result.stdout, result.returncode


# ------------------------------------------------------------------
# Test cases
# ------------------------------------------------------------------

class TestDefaultBpm(unittest.TestCase):
    # ------------------------------------------------------------------
    # fixture-level sanity: binary must exist
    # ------------------------------------------------------------------
    @classmethod
    def setUpClass(cls) -> None:
        if not ANALYZER.is_file():
            cls.fail(f"analyze_audio not found at {ANALYZER}")


def _make_test(rel_path: str, expected_bpm: float) -> None:
    """Factory that creates an individual test method."""

    def test_method(self: object) -> None:
        stdout, rc = run_analyzer(rel_path)

        # Return code check
        self.assertEqual(
            rc,
            0,
            f"Non-zero exit code ({rc}) for {rel_path}\n"
            f"stderr:\n{stdout}",
        )

        # File must actually exist
        self.assertTrue(
            (DIR / rel_path).is_file(),
            f"Test file missing: {rel_path}",
        )

        # Extract BPM
        bpm = extract_bpm(stdout)
        self.assertIsNotNone(
            bpm,
            f"Could not parse BPM from output of {rel_path}\n"
            f"stdout:\n{stdout}",
        )

        # Numerical comparison to the hundredth place
        self.assertAlmostEqual(
            bpm,
            expected_bpm,
            places=2,
            msg=(
                f"BPM mismatch for {rel_path}:\n"
                f"  expected = {expected_bpm}\n"
                f"  got      = {bpm}"
            ),
        )

    return test_method


# ------------------------------------------------------------------
# Dynamically attach one test method per reference file
# ------------------------------------------------------------------
for rel_path, exp_bpm in TESTS:
    name = f"test_bpm_{os.path.splitext(os.path.basename(rel_path))[0]}"
    setattr(TestDefaultBpm, name, _make_test(rel_path, exp_bpm))


# ------------------------------------------------------------------
# entry point
# ------------------------------------------------------------------

if __name__ == "__main__":
    suite = unittest.TestSuite()
    loader = unittest.TestLoader()
    suite.addTests(loader.loadTestsFromTestCase(TestDefaultBpm))
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
