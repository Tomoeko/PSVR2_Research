"""Input diagnostics must select the software bridge and fail closed on errors."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class InputVerifyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("a native C compiler is required")
        cls.temporary = tempfile.TemporaryDirectory(prefix="psvr2-input-verify-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.program = Path(cls.temporary.name) / "verify"
        subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/fixtures/input_verify_harness.c"),
                        "-o", str(cls.program)], check=True, capture_output=True)

    def run_verify(self, *arguments, **overrides):
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith("VERIFY_")}
        environment.update(overrides)
        return subprocess.run([str(self.program), *arguments], env=environment,
                              capture_output=True, text=True, timeout=5)

    def test_default_and_explicit_bridge_never_select_hardware(self):
        for arguments in ((), ("--bridge",)):
            with self.subTest(arguments=arguments):
                result = self.run_verify(*arguments, VERIFY_FAIL_DEVICE="1")
                self.assertEqual(result.returncode, 1)
                self.assertIn("ROUTE=input bridge\n", result.stderr)
                self.assertIn("DEVICE_OPEN", result.stderr)

    def test_hardware_route_requires_explicit_valid_endpoint(self):
        for endpoint in ("1", "9"):
            result = self.run_verify("--endpoint", endpoint, VERIFY_FAIL_DEVICE="1")
            self.assertIn("ROUTE=input " + endpoint + "\n", result.stderr)

    def test_invalid_options_do_not_open_input(self):
        for arguments in (("2",), ("--endpoint",), ("--endpoint", "0"),
                          ("--endpoint", "10"), ("--endpoint", "-1"),
                          ("--bridge", "--endpoint", "2")):
            with self.subTest(arguments=arguments):
                result = self.run_verify(*arguments)
                self.assertEqual(result.returncode, 1)
                self.assertIn("Usage:", result.stderr)
                self.assertNotIn("ROUTE=", result.stderr)
                self.assertNotIn("DEVICE_OPEN", result.stderr)

    def test_help_needs_no_device(self):
        result = self.run_verify("--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("software ACM", result.stdout)
        self.assertEqual(result.stderr, "")

    def test_proc_failure_stops_before_input_device(self):
        result = self.run_verify(VERIFY_FAIL_PROC="1")
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("DEVICE_OPEN", result.stderr)

    def test_incomplete_route_command_stops_before_input_device(self):
        result = self.run_verify(VERIFY_SHORT_WRITE="1")
        self.assertEqual(result.returncode, 1)
        self.assertIn("incomplete command write", result.stderr)
        self.assertNotIn("DEVICE_OPEN", result.stderr)

    def test_interrupted_route_write_is_retried(self):
        result = self.run_verify(VERIFY_EINTR_WRITE="1", VERIFY_FAIL_DEVICE="1")
        self.assertEqual(result.stderr.count("ROUTE=input bridge"), 1)
        self.assertIn("DEVICE_OPEN", result.stderr)

    def test_closed_and_failed_input_transport_return_failure(self):
        for overrides in ({}, {"VERIFY_FAIL_READ": "1"}):
            result = self.run_verify(**overrides)
            self.assertEqual(result.returncode, 1)
            self.assertIn("DEVICE_OPEN", result.stderr)


if __name__ == "__main__":
    unittest.main()
