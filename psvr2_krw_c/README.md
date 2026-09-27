# Native PSVR2 host toolkit

`psvr2_krw_c` is a C11 host program for PSVR2 firmware **01.10**
(`0x01100103`) and **06.00** (`0x06000102`). It implements firmware detection,
bounded kernel memory access, live instruction checks, ARM64 execution helpers,
Stage1 mailbox commands, supervised BusyBox jobs, file transfers, Stage3 serial
deployment, and guarded persistent module replacement.

The tree contains source and host tests. Firmware images, device certificates,
factory filesystem contents, target executables, and kernel SDK sources are
external inputs. The small instruction anchors in `src/kernel_images.c` check
specific live instruction sites; they are not firmware images. Helper payloads
are constructed from ARM64 instruction source.

## Build and test on the host

Install a C11 compiler, CMake 3.20 or newer, pkg-config, and libusb 1.0 development
files. Threads are required. SDL2 enables the optional stream viewer. Python 3
is used only for several host test harnesses; the toolkit itself is native C.

From the repository root:

```sh
./psvr2_krw_c/build.sh release
./psvr2_krw_c/build.sh ci
./psvr2_krw_c/build.sh sanitize
```

These presets write only to `psvr2_krw_c/.local/build-<profile>`. For an external
build directory, use CMake directly:

```sh
cmake -S psvr2_krw_c -B /tmp/psvr2-host-build \
  -DCMAKE_BUILD_TYPE=Debug -DPSVR2_WARNINGS_AS_ERRORS=ON
cmake --build /tmp/psvr2-host-build --parallel
ctest --test-dir /tmp/psvr2-host-build --output-on-failure
```

Tests cover profile and instruction generation, protocol parsing, command
validation, Stage1 discovery, bounded bulk preparation, cached VFS reads,
persistence failure/durability paths, serial reentry, line editing, completion,
and supervised jobs using local harnesses. Default tests do not open a headset.
They do not establish hardware compatibility or headset presentation quality.

Optional exact-image regression tests need private fixtures supplied explicitly
with `-DPSVR2_FIRMWARE_FIXTURES_DIR=/path/to/fixtures`. The directory layout is:

```text
01.10/kernel.bin
01.10/sieusb.ko
06.00/kernel.elf
06.00/kernel.bin
06.00/kernel-dump-a.bin
06.00/kernel-dump-b.bin
06.00/sieusb.ko
06.00/libcomposite.ko
```

The fixture scripts verify fixed hashes, offsets, and symbols. Missing or
mismatched fixtures fail those opt-in tests. Do not place private images in the
source tree.

## Use

The help and version commands exit before opening USB:

```sh
psvr2_krw_c/.local/build-release/psvr2_krw_c --help
psvr2_krw_c/.local/build-release/psvr2_krw_c --version
```

Device commands operate on the connected headset. Start a maintenance session
without automatically deploying Stage3:

```sh
psvr2_krw_c/.local/build-release/psvr2_krw_c --no-serial
```

On macOS, use your regular account when USB permissions allow it. The program
normally detects firmware and verifies live profile anchors before later
writes. `--read-only` blocks later writes, but its initial reader bootstrap
still executes on the target. A firmware override must exactly match the device
report; persistent operations require automatic detection.

Target modules must be built against the matching external kernel SDK; see
[the repository build guide](../docs/build.md). An explicit Stage1 can be loaded
through volatile `/tmp` with `--stage1 /path/to/stage1.ko`. Without an explicit
file, the loader reuses a verified live mailbox, then prefers the selected
target `/data/modules/stage1.ko` or `/tmp/stage1.ko`; it also discovers
`output/psvr2-build/<firmware>/modules/stage1.ko` on the host.

Provide a static AArch64 Linux BusyBox by setting `PSVR2_BUSYBOX` to its host
path. The native shell can use it for supervised jobs and persistence. Serial
deployment requires matching `u_serial.ko`, `usb_f_acm.ko`, and
`stage3_serial.ko` already staged together in `/tmp` or `/data/modules`.
The kernel SDK supplies the first two dependencies; this repository supplies
the custom module source.

In an interactive toolkit session, `krw help` lists native commands. Typical
commands are `krw serial status`, `krw serial reset double-evict`,
`krw fast_upload /path/to/file`, and `krw stage1_verify /path/to/stage1.ko`.
`serial reset` intentionally reconnects USB; `serial reload` uses the no-reset
path. `double-evict` changes endpoint ownership and enables a second ACM port.
Existing serial state is checked before an ordinary deployment is reused.

The standalone native helpers are `psvr2_serial_tool`,
`psvr2_controller_bridge`, `psvr2_convert_ar30`, and, when SDL2 is present,
`psvr2_stream_viewer`. Run their help for exact argument syntax. Do not run
multiple host consumers against the same serial or USB endpoint.

See the [shell reference](docs/shell-reference.md) for command semantics and the
[persistent replacement guide](docs/persistent-module-replacement.md) for
file transactions, verification, and recovery.
