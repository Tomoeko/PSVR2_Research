"""Host checks for command validation; no headset devices are accessed."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import unittest

REPO_ROOT = Path(__file__).resolve().parents[1]
SOURCE = REPO_ROOT / "target/psvr2/tools/open_vrhmd"


class CommandTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            raise unittest.SkipTest("host C compiler unavailable")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        cls.proc_root = directory / "proc"
        cls.proc_root.mkdir()
        stub = directory / "hardware_stubs.c"
        stub.write_text(r'''
#include "open_vrhmd.h"
static int status(const char *name) {
  const char *value = getenv(name);
  return value ? atoi(value) : 0;
}
void perform_tuning(void) { LOG("STUB tuning"); }
int cmd_info(void) { return 0; }
int open_devices(void) {
  LOG("STUB open");
  usleep(status("OPEN_DELAY_MS") * 1000);
  return status("OPEN_STATUS");
}
void close_devices(void) {
  const char *marker = getenv("OWNER_CLEANED_PATH");
  if (marker) { FILE *file = fopen(marker, "w"); if (file) fclose(file); }
  LOG("STUB close");
}
int fan_ctrl_init(void) {
  if (status("FORK_FAN")) {
    pid_t child = fork();
    if (child == 0) { close(0); close(1); close(2); sleep(10); _exit(0); }
    LOG("STUB fan child=%ld", (long)child);
  }
  return status("FAN_STATUS");
}
int cmd_go(uint8_t r, uint8_t g, uint8_t b, int pattern) {
  if (status("IGNORE_TERM")) signal(SIGTERM, SIG_IGN);
  LOG("STUB display %d %d %d pattern=%d stream=%d", r, g, b, pattern, g_stream);
  while (status("HOLD_DISPLAY") && !application_stop_requested) usleep(10000);
  return status("COMMAND_STATUS");
}
int cmd_stop(void) { LOG("STUB stop"); return status("COMMAND_STATUS"); }
int audio_init(int volume) { return status("COMMAND_STATUS"); }
int audio_play_file(const char *path, int volume) { return status("COMMAND_STATUS"); }
int audio_play_tone(int seconds) { return status("COMMAND_STATUS"); }
void audio_cleanup(void) {}
''')
        cls.binary = directory / "open_vrhmd"
        subprocess.run(
            [compiler, "-std=gnu11", "-DGPU_RENDER", "-DPSVR2_SOURCE_FAMILY_0600=1",
             '-DOPEN_VRHMD_LOCK_PATH="' + str(directory / "owner.lock") + '"',
             '-DOPEN_VRHMD_PROC_ROOT="' + str(cls.proc_root) + '"',
             "-DOPEN_VRHMD_STOP_WAIT_MS=500",
             "-I" + str(SOURCE), str(SOURCE / "main.c"), str(stub),
             str(REPO_ROOT / "tests/fixtures/open_vrhmd_lock_harness.c"),
             "-o", str(cls.binary)],
            check=True, capture_output=True, text=True,
        )

    def environment(self, **status) -> dict:
        environment = dict(os.environ)
        for name in ("OPEN_STATUS", "COMMAND_STATUS", "HOLD_DISPLAY", "IGNORE_TERM",
                     "FORK_FAN", "FAN_STATUS", "OPEN_DELAY_MS", "OWNER_CLEANED_PATH", "TEST_EXE_MISMATCH"):
            environment.pop(name, None)
        environment["TEST_BINARY"] = str(self.binary)
        environment.update({name: str(value) for name, value in status.items()})
        return environment

    def run_command(self, *arguments: str, **status) -> subprocess.CompletedProcess:
        environment = self.environment(**status)
        return subprocess.run([str(self.binary), *arguments], env=environment,
                              capture_output=True, text=True, timeout=5)

    def start_owner(self, ready="STUB display", **status):
        process = subprocess.Popen([str(self.binary), "bench"],
                                   env=self.environment(HOLD_DISPLAY=1, **status),
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                   text=True)
        def cleanup():
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            process.stderr.close()
        self.addCleanup(cleanup)
        messages = []
        while True:
            line = process.stderr.readline()
            self.assertTrue(line, "owner exited before readiness: " + "".join(messages))
            messages.append(line)
            if ready in line:
                break
        return process

    def test_invalid_arguments_do_not_touch_hardware(self) -> None:
        for arguments in [(), ("unknown",), ("go", "1"), ("go", "256", "0", "0"),
                          ("go", "red", "0", "0"), ("image",), ("video",),
                          ("audio",), ("audio", "sample.wav", "-1"),
                          ("tone", "31"), ("tone", "NaN"), ("stop", "--stream")]:
            with self.subTest(arguments=arguments):
                result = self.run_command(*arguments)
                self.assertEqual(result.returncode, 1)
                self.assertNotIn("STUB", result.stderr)

    def test_help_and_info_do_not_tune_or_open_devices(self) -> None:
        for command in ("--help", "help", "info"):
            with self.subTest(command=command):
                result = self.run_command(command)
                self.assertEqual(result.returncode, 0)
                self.assertNotIn("STUB", result.stderr)

    def test_rgb_and_stream_arguments_reach_dispatch(self) -> None:
        result = self.run_command("go", "12", "34", "56")
        self.assertEqual(result.returncode, 0)
        self.assertIn("display 12 34 56 pattern=0 stream=0", result.stderr)
        result = self.run_command("gradient", "--stream")
        self.assertEqual(result.returncode, 0)
        self.assertIn("pattern=1 stream=1", result.stderr)
        result = self.run_command("image", "--stream", "sample.png")
        self.assertEqual(result.returncode, 0)
        self.assertIn("pattern=4 stream=1", result.stderr)

    def test_positive_and_negative_command_errors_fail(self) -> None:
        for status in (-1, 1):
            with self.subTest(status=status):
                result = self.run_command("go", COMMAND_STATUS=status)
                self.assertEqual(result.returncode, 1)
                self.assertIn("STUB close", result.stderr)

    def test_fan_initialization_failure_skips_display_and_cleans_up(self) -> None:
        result = self.run_command("bench", FAN_STATUS=-1)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertNotIn("STUB display", result.stderr)
        self.assertIn("STUB stop", result.stderr)
        self.assertIn("STUB close", result.stderr)

    def test_open_failure_skips_dispatch(self) -> None:
        result = self.run_command("go", OPEN_STATUS=-1)
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("STUB display", result.stderr)

    def test_stop_does_not_apply_performance_tuning(self) -> None:
        result = self.run_command("stop")
        self.assertEqual(result.returncode, 0)
        self.assertNotIn("STUB tuning", result.stderr)

    def test_concurrent_display_audio_and_tone_do_not_touch_hardware(self) -> None:
        owner = self.start_owner()
        for arguments in (("bench",), ("audio", "sample.wav"), ("tone",)):
            result = self.run_command(*arguments)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("use 'stop' first", result.stderr)
            self.assertNotIn("STUB", result.stderr)
        self.assertIsNone(owner.poll())

    def test_stop_signals_owner_then_opens_devices(self) -> None:
        marker = Path(self.temporary.name) / "owner-cleaned"
        marker.unlink(missing_ok=True)
        owner = self.start_owner(OWNER_CLEANED_PATH=marker)
        result = self.run_command("stop")
        self.assertEqual(result.returncode, 0, result.stderr)
        owner.wait(timeout=5)
        self.assertTrue(marker.exists())
        self.assertIn("Stopping open_vrhmd owner PID", result.stderr)
        self.assertIn("STUB open", result.stderr)
        self.assertEqual(self.run_command("go").returncode, 0)

    def test_stop_received_during_initialization_skips_rendering(self) -> None:
        owner = self.start_owner(ready="STUB open", OPEN_DELAY_MS=300)
        result = self.run_command("stop")
        self.assertEqual(result.returncode, 0, result.stderr)
        owner.wait(timeout=5)
        remaining = owner.stderr.read()
        self.assertNotIn("STUB display", remaining)
        self.assertIn("STUB stop", remaining)

    def test_stop_timeout_does_not_touch_hardware(self) -> None:
        owner = self.start_owner(IGNORE_TERM=1)
        result = self.run_command("stop")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("did not finish cleanup", result.stderr)
        self.assertNotIn("STUB", result.stderr)
        self.assertIsNone(owner.poll())

    def test_stop_refuses_a_different_executable(self) -> None:
        owner = self.start_owner(TEST_EXE_MISMATCH=1)
        result = self.run_command("stop")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("executable differs", result.stderr)
        self.assertNotIn("STUB", result.stderr)
        self.assertIsNone(owner.poll())

    def test_process_exit_recovers_the_persistent_lock(self) -> None:
        owner = self.start_owner()
        owner.kill()
        owner.wait(timeout=5)
        result = self.run_command("go")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_fan_child_does_not_retain_application_ownership(self) -> None:
        environment = self.environment(FORK_FAN=1)
        result = subprocess.run([str(self.binary), "go"], env=environment,
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        child_line = next(line for line in result.stderr.splitlines() if "STUB fan child=" in line)
        child_pid = int(child_line.rsplit("=", 1)[1])
        try:
            self.assertEqual(self.run_command("go").returncode, 0)
        finally:
            try:
                os.kill(child_pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


if __name__ == "__main__":
    unittest.main()
