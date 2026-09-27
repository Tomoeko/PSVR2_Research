"""Cooling ownership regression; Linux flock independence is modeled on Darwin.

Real child PIDs, signals, and POSIX locks are used. Hardware paths are redirected.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
SOURCE = REPO / "target/psvr2/tools/open_vrhmd"


class FanOwnershipTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise unittest.SkipTest("host C compiler unavailable")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        (cls.directory / "sys").mkdir()
        (cls.directory / "sys/prctl.h").write_text("#pragma once\n#define PR_SET_NAME 15\n")
        cls.proc = cls.directory / "proc"
        cls.proc.mkdir()
        cls.binary = cls.directory / "fan_checks"
        cls.lock = cls.directory / "fan.lock"
        cls.fifo = cls.directory / "fan.fifo"
        cls.log = cls.directory / "sysfs.log"
        subprocess.run(
            [compiler, "-std=gnu11", "-DPSVR2_SOURCE_FAMILY_0600=1",
             '-DOPEN_VRHMD_PROC_ROOT="' + str(cls.proc) + '"',
             '-DFAN_LOCK_PATH="' + str(cls.lock) + '"',
             '-DFAN_CMD_FIFO="' + str(cls.fifo) + '"', "-DFAN_STOP_WAIT_MS=400",
             "-I" + str(cls.directory), "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", str(REPO / "tests/fixtures/open_vrhmd_fan_harness.c"),
             "-o", str(cls.binary)], check=True, capture_output=True, text=True)

    def check(self, scenario, **extra):
        self.lock.unlink(missing_ok=True)
        self.log.unlink(missing_ok=True)
        environment = dict(os.environ, TEST_BINARY=str(self.binary), SYSFS_LOG=str(self.log))
        environment.pop("FAIL_PWM", None)
        environment.update(extra)
        result = subprocess.run([str(self.binary), scenario], env=environment,
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return result

    def test_initialized_daemon_persists_is_reused_and_stops_natively(self):
        result = self.check("init")
        self.assertIn("init=0", result.stdout)
        log = self.log.read_text()
        self.assertIn("/pwm20/enable 1", log)
        self.assertIn("/pwm20/enable 0", log)
        self.assertTrue(self.lock.exists())

    def test_initial_pwm_failure_fails_startup(self):
        result = self.check("initfail", FAIL_PWM="1")
        self.assertIn("init=-1", result.stdout)

    def test_busy_lock_during_startup_is_bounded_and_stopped(self):
        self.check("gap")

    def test_unready_daemon_cannot_be_reused(self):
        self.check("notready")

    def test_term_ignoring_daemon_preserves_pwm(self):
        self.check("timeout")

    def test_different_executable_is_not_signalled(self):
        self.check("exe")

    def test_unrelated_process_name_is_not_signalled(self):
        self.check("comm")


if __name__ == "__main__":
    unittest.main()
