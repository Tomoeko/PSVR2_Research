"""Exercise Stage3 hardware and software input routes with mocked kernel I/O."""
import ast
import os
from pathlib import Path
import re
import signal
import time
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Stage3InputBridgeTests(unittest.TestCase):
    def test_proc_callbacks_drain_before_worker_and_endpoint_teardown(self):
        source = (ROOT / "target/psvr2/modules/stage3_serial/stage3_serial.c").read_text()
        start = source.index("static void stage3_exit(void) {")
        end = source.index("module_init(stage3_init);", start)
        teardown = source[start:end]
        removal = teardown.index("proc_remove(s3_proc)")
        for operation in ("s3_stop_worker(", "kthread_stop(", "s3_stop_shell_process(",
                          "s3_fast_input_cleanup(", "s3_fast_stream_cleanup(",
                          "s3_remove_acm_function(", "usb_put_function(",
                          "usb_put_function_instance("):
            self.assertLess(removal, teardown.index(operation), operation)
        self.assertNotIn("mutex_lock(", teardown[:removal])
        self.assertNotIn("spin_lock", teardown[:removal])
        self.assertEqual(teardown.count("proc_remove(s3_proc)"), 1)

    def test_initial_activation_waits_for_both_tty_ports(self):
        source = (ROOT / "target/psvr2/modules/stage3_serial/stage3_serial.c").read_text()
        start = source.index("static int __init stage3_init(void)")
        end = source.index("fail_func:", start)
        initialization = source[start:end]
        activation = initialization.index('s3_activate_acm_ports("initial after tty open")')
        for operation in ("s3_start_shell()", "s3_start_bridge()",
                          "wait_for_completion_timeout(&bridge_tty_ready",
                          's3_wait_for_path("/tmp/.stage3_shell_ready"'):
            self.assertLess(initialization.index(operation), activation, operation)

    def test_shell_ready_marker_follows_tty_redirections(self):
        source = (ROOT / "target/psvr2/modules/stage3_serial/stage3_serial.c").read_text()
        start = source.index("snprintf(exec_cmd, sizeof(commands->exec_cmd),")
        end = source.index("cleanup_cmd, shell_binary, shell_binary, inner_cmd,", start)
        template = "".join(ast.literal_eval(literal) for literal in
                           re.findall(r'"(?:[^"\\]|\\.)*"', source[start:end]))
        cleanup = "rm -f /tmp/.stage3_shell_pid /tmp/.stage3_shell_child " \
                  "/tmp/.stage3_shell_ready /tmp/.stage3_shell_stop"
        # Exercise the actual wrapper format with a local FIFO representing a
        # tty whose child redirections have not opened yet. No device is used.
        script = template % (cleanup, "/bin/sh", "/bin/sh", "cat", "sleep 1", cleanup)
        with tempfile.TemporaryDirectory(prefix="stage3-shell-") as directory:
            directory = Path(directory)
            fifo = directory / "tty"
            os.mkfifo(fifo)
            script = script.replace("/tmp/.stage3_shell", str(directory / "shell"))
            script = script.replace("/tmp/.shell_diag", str(directory / "diagnostic"))
            script = script.replace("/dev/ttyGS0", str(fifo))
            process = subprocess.Popen(["/bin/sh", "-c", script], start_new_session=True,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            fifo_fd = None
            try:
                deadline = time.monotonic() + 2
                while not (directory / "shell_child").exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue((directory / "shell_child").exists())
                self.assertFalse((directory / "shell_ready").exists())
                fifo_fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
                deadline = time.monotonic() + 2
                while not (directory / "shell_ready").exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue((directory / "shell_ready").exists())
            finally:
                os.killpg(process.pid, signal.SIGTERM)
                process.communicate(timeout=2)
                if fifo_fd is not None:
                    os.close(fifo_fd)

    @unittest.skipUnless(shutil.which("cc"), "a host C compiler is required")
    def test_hardware_software_ring_partial_packets_and_cleanup(self):
        with tempfile.TemporaryDirectory(prefix="stage3-input-") as directory:
            executable = Path(directory) / "input-harness"
            result = subprocess.run([
                shutil.which("cc"), "-std=gnu11", "-O1", "-g", "-Wall",
                "-Wextra", "-Werror", "-Wno-unused-parameter",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                str(ROOT / "tests/fixtures/stage3_input_bridge_harness.c"),
                "-pthread", "-o", str(executable),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Stage3 delayed input setup", result.stdout)


if __name__ == "__main__":
    unittest.main()
