# Kernel overlays

These small, project-maintained diffs apply to separately obtained public
Sony Linux source. They are not a bundled SDK or a copy of the source tree.
The build helper accepts original or already patched inputs and rejects
unfamiliar versions.

`u_serial-throughput.patch` increases request queue depth and transmit ring
capacity, allocates larger request buffers, and allows transmit requests to
use their available length. It changes three short sections of `u_serial.c`.

`usb-function-unload.patch` keeps the USB function factory cleanup callback in
module core on target kernels that disable `CONFIG_MODULE_UNLOAD`. The
cleanup-aware teardown helper needs that callback to unregister the factory
before reclaiming the module. The patch changes only the callback annotation
and its explanatory comment in `DECLARE_USB_FUNCTION_INIT`.

The guarded macOS modpost adaptation lives in `tools/psvr2_build.py`. It updates
the kernel's existing Mach-O symbol/section lookup for current host headers.
Source preparation checks each original or already adapted form before writing;
the Linux target build is unchanged by this host-only accommodation.

Obtain public sources through the [Sony PSVR2 source page](https://www.playstation.com/en-us/oss/ps-vr2/).
The repository's build manifest pins the maintained source identities; the
build guide explains source import and verification.
