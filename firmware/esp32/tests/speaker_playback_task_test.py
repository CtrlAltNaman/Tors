"""Execute the current firmware speaker task with deterministic RTOS/I2S adapters.

Compile the extracted task through GCC stdin; do not copy its implementation or
write generated source into the workspace. Real buffer and gain code are linked.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SpeakerPlaybackPolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gcc = shutil.which("gcc")
        if gcc is None:
            raise RuntimeError("GCC is required for executable speaker policy tests")
        # Keep generated compiler outputs inside the owned test tree.
        cls.temp = tempfile.TemporaryDirectory(prefix="speaker-policy-",
                                               dir=ROOT / "tests/playback_stubs")
        assert Path(cls.temp.name).resolve().parent == (ROOT / "tests/playback_stubs").resolve()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.executables = {}
        main = (ROOT / "main/main.c").read_text()
        start = main.index("static void speaker_playback_task(void *argument) {")
        end = main.index("static esp_err_t play_boot_audio", start)
        constants = []
        for name in ("I2S_DMA_DESCRIPTORS", "SPEAKER_DRAIN_MS", "SPEAKER_IDLE_WAIT_MS"):
            constants.append(re.search(rf"^#define {name}\b[^\n]*", main, re.M)[0])
        source = "\n".join(constants) + '\n#include "speaker_playback_harness.h"\n'
        source += f'#line {main.count(chr(10), 0, start) + 1} "main/main.c"\n'
        source += main[start:end] + '\n#include "speaker_playback_harness.c"\n'
        common = [gcc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                  "-Imain", "-Itests/playback_stubs", "-Itests"]
        builds = {
            "speaker": (["-DportTICK_PERIOD_MS=10", "-x", "c", "-",
                         "main/playback_buffer.c", "main/audio_conversion.c"], source),
            "buffer": (["tests/playback_buffer_test.c", "main/playback_buffer.c"], None),
            "gain": (["tests/pcm_conversion_test.c", "main/audio_conversion.c"], None),
        }
        for name, (inputs, stdin) in builds.items():
            exe = Path(cls.temp.name) / f"{name}.exe"
            result = subprocess.run(common + inputs + ["-o", str(exe)], cwd=ROOT,
                                    input=stdin, capture_output=True, text=True, timeout=60)
            if result.returncode:
                raise AssertionError(f"{name} compile failed:\n{result.stdout}{result.stderr}")
            cls.executables[name] = exe

    def run_native(self, executable, *args):
        result = subprocess.run([str(self.executables[executable]), *args], cwd=self.temp.name,
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_native_buffer_regressions(self):
        self.run_native("buffer")

    def test_all_pcm16_samples_preserve_70_percent_gain(self):
        self.run_native("gain")

    def test_five_packets_required_before_first_write(self):
        self.run_native("speaker", "five")

    def test_short_last_bypasses_threshold_and_drains(self):
        self.run_native("speaker", "short")

    def test_explicit_stop_releases_short_clip(self):
        self.run_native("speaker", "explicit_stop")

    def test_stalled_sender_starts_at_250ms_without_repeating_audio(self):
        self.run_native("speaker", "fallback")

    def test_steady_playback_is_not_regated(self):
        self.run_native("speaker", "steady")

    def test_long_gap_restarts_with_five_packets(self):
        self.run_native("speaker", "restart")

    def test_generation_reset_cancels_frame_held_during_wait(self):
        self.run_native("speaker", "reset")

    def test_timeout_last_race_acknowledges_once_after_tail(self):
        self.run_native("speaker", "timeout_last_race")


if __name__ == "__main__":
    unittest.main()
