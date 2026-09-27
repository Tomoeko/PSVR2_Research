# Kernel modules

The build manifest selects firmware-specific offsets and flags for 01.10 and
06.00. Compile against the matching public kernel source/configuration through
[the root build system](../../../build.sh); a module built for one kernel is not
a portable replacement for another firmware.

- `stage1`: authenticated command/transfer bridge, with `/proc/stage1` control
  and `/proc/stage1_out` status/output. USB endpoint publication and teardown
  retain references until active transfers and callbacks finish.
- `stage3_serial`: gadget serial shell plus controller/input and frame-stream
  bridges. `/proc/stage3` selects routes and reports bounded diagnostics;
  `/dev/fast_input` supplies complete input packets and `/dev/fast_stream`
  carries display frames. TTY startup waits for readiness, raw input checks its
  configuration, and a hung-up descriptor is closed and reopened. Proc
  callbacks drain before worker, endpoint and device cleanup.
- `rmmod_helper`: targeted unload support for the firmware module lifecycle.

Only these maintained modules are included. Their Makefiles are Kbuild entry
points; the root build helper supplies the matching kernel and `PSVR2_CFLAGS`.
The host regression harnesses mock kernel I/O and exercise publication,
partial packets, delayed setup, hung-up TTY recovery and cleanup ordering.
They do not connect to a headset.

These authored modules use the repository MIT license; `Dual MIT/GPL` metadata
identifies that license as GPL-compatible to the kernel module loader.
