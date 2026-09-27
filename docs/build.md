# Native builds

This repository contains source, build recipes, small kernel patches, and tests.
It does not contain a kernel SDK, target libraries, firmware images, a
toolchain, converted menu assets, or prebuilt executables. Build products and
downloaded sources stay in ignored local directories.

## Inputs and supported profiles

The maintained source profiles are `01.10` and `06.00`. Only `06.00` has current
device validation. The native display tool and external Wii Menu adapter are
restricted to `06.00`. Profile selection never changes the device firmware.

Use Python 3.9 or newer, Git, GNU make, a native C compiler, `bc`, `perl`, and an
AArch64 GCC/binutils toolchain for kernel modules. Userspace tools can use an
AArch64 Linux GCC toolchain or `zig cc`. Zig userspace builds target glibc 2.28;
static BusyBox uses the compiler's musl target. The compiler is an external
input and is never bundled. Run these read-only checks first:

```sh
./build.sh --help
./build.sh list
./build.sh bootstrap --check
```

`bootstrap` can install missing macOS prerequisites after asking about each
installation. `--check` does not install anything. Linux users should supply
their distribution's GNU make and cross compiler.

Sony provides the public source downloads at its
[PSVR2 open source page](https://www.playstation.com/en-us/oss/ps-vr2/), including
[Linux](https://www.playstation.com/en-us/oss/ps-vr2/linux-kernel/),
[BusyBox](https://www.playstation.com/en-us/oss/ps-vr2/busybox/), and
[glibc](https://www.playstation.com/en-us/oss/ps-vr2/glibc/). Archive URLs and
project-pinned SHA-256 identities are in `target/psvr2/build-manifest.json`.
These are project pins, not Sony-signed checksums. A changed download fails
verification; importing a deliberately modified kernel requires the explicit
`--allow-unverified-source` option.

## Kernel source and modules

Fetch the selected public archive, then import the path printed by the fetch
command:

```sh
./build.sh sources kernel --firmware 06.00
./build.sh sdk import .local/psvr2-external/archives/linux-PSVR2_06.00.tar.zip --firmware 06.00
./build.sh configure --firmware 06.00 --cross-prefix aarch64-linux-gnu- --tool-cc aarch64-linux-gnu-gcc --make make
./build.sh prepare --firmware 06.00
./build.sh module stage3_serial --firmware 06.00
```

On macOS, use `--make gmake`, the installed AArch64 GCC prefix, and
`--tool-cc 'zig cc'`. Before preparing a macOS kernel build, run
`./build.sh sources glibc`. It downloads the public archive and generates a
host-only `elf.h` directly from the nested source archive under `.local/inputs/host-include`, preserving the source's
copyright and license. It removes glibc-specific declaration wrappers so
macOS host tools can use the public ELF types. The build also adapts the
kernel's existing Mach-O modpost branch for modern macOS. Nothing from these
host accommodations becomes a target module.

The source importer verifies archive and extracted-tree identities before
publishing a local snapshot. Tracked kernel patches are applied idempotently
to the generated source checkout. An unfamiliar patch target fails rather
than being partly modified. `--no-prepare` requires an already prepared,
matching build tree. Use `./build.sh sdk verify --require-official` to verify
the local source store. The maintained kernel source changes are exactly the
tracked `u_serial` throughput patch, USB factory cleanup patch, and guarded
macOS `file2alias.c` adaptation. Fresh 06.00 source preparation and all three
custom module builds have been checked independently of an existing SDK.

The custom modules are `stage1`, `stage3_serial`, and `rmmod_helper`. Stage3
also builds its matching public-kernel gadget dependencies in order:
`libcomposite`, `u_serial`, and `usb_f_acm`. The stock kernel provides
libcomposite; the packaged runtime dependency order is `u_serial.ko`,
`usb_f_acm.ko`, then `stage3_serial.ko`. There is no separate maintained
Stage2 module source: that part of the workflow loads the runtime gadget
dependencies. Building does not upload or install modules.

```sh
./build.sh modules --firmware 06.00
./build.sh matrix modules --firmwares all
```

Outputs go to `output/psvr2-build/<profile>/modules`. Build receipts record
source identities, artifact hashes, and the selected profile. Use `matrix
modules --help` for explicit profile selection; the default is `06.00`.

## Userspace tools

```sh
./build.sh tool open_vrhmd --firmware 06.00
./build.sh tool input_verify --firmware 06.00
./build.sh tool kill --firmware 06.00
./build.sh tool busybox --firmware 06.00 --download
```

BusyBox uses Sony's pinned public archive, containing BusyBox 1.29.0. The
helper unpacks the nested archive and builds its upstream GPL source tree;
it does not invoke the archive's surrounding SDK wrapper. A tracked small
linker patch probes optional diagnostic flags before using them, allowing
both GNU linkers and Zig's target linker to build this older source. The maintained
configuration builds a static executable with ash and hush and excludes
unavailable external integrations. `--source /path/to/archive` can substitute
an explicit copy of the same pinned archive. Downloaded archives, extracted
source, and generated binaries are never part of the repository.

Userspace output is `output/psvr2-build/<profile>/tools`. The display package
contains the executable only; supply your own media when invoking its media
commands. PNG/JPEG images, MP3 audio, and MPEG-1 video use the project's authored
codec sources. MPEG program-stream audio is skipped; video playback does not
decode an MP2 soundtrack. See the [codec reference](../target/psvr2/tools/open_vrhmd/codec/README.md)
for supported formats and test dependencies. Host USB tooling has a separate native build:

```sh
./psvr2_krw_c/build.sh release
```

## RAM deployment check

After building the modules, BusyBox, and host toolkit, start with the matching
Stage1 and an explicit RAM location. Run from the repository root:

```sh
export PSVR2_BUSYBOX=output/psvr2-build/06.00/tools/busybox
psvr2_krw_c/.local/build-release/psvr2_krw_c --tmp --no-serial --stage1 output/psvr2-build/06.00/modules/stage1.ko
```

In the toolkit session, upload the dependency chain before loading it. The
serial command reads target `/tmp` files; it does not discover host module
output automatically:

```text
krw fast_upload output/psvr2-build/06.00/tools/busybox
krw fast_upload output/psvr2-build/06.00/modules/rmmod_helper.ko
krw fast_upload output/psvr2-build/06.00/modules/u_serial.ko
krw fast_upload output/psvr2-build/06.00/modules/usb_f_acm.ko
krw fast_upload output/psvr2-build/06.00/modules/stage3_serial.ko
krw s1exec test -w /proc/rmmod_helper || insmod /tmp/rmmod_helper.ko
krw serial reset double-evict
krw serial status
```

Reset re-enumerates USB; reconnect when necessary. These commands test the RAM
candidate. Persistent replacement is a separate guarded transaction described
in the [host toolkit guide](../psvr2_krw_c/docs/persistent-module-replacement.md).
Match the automatically detected device profile before any deployment.

Upload and run the display tool in the same toolkit session:

```text
krw fast_upload output/psvr2-build/06.00/tools/open_vrhmd_folder/open_vrhmd
```

Select the control ACM port assigned by your OS, then open the target shell:

```sh
psvr2_krw_c/.local/build-release/psvr2_serial_tool /dev/cu.usbmodemXXXX shell
```

Use `/dev/ttyACM0` on Linux when it is the control port. Do not select the
second input port for a shell. In the target shell, `/tmp/open_vrhmd --help`
lists commands; `/tmp/open_vrhmd gradient` starts a display pattern. Ctrl-C
stops that foreground tool and runs its teardown. `open_vrhmd stop` provides
explicit display/audio cleanup. Ctrl-] closes only the host serial client.

## External Wii Menu project

The menu stays in its own source repository. Supply a checkout containing the
PSVR2 backend and a local runtime root with AArch64 ELF libraries
`lib/libEGL.so.1` and `lib/libGLESv2.so.2`:

```sh
./build.sh wii-menu --firmware 06.00 --project /path/to/Wii_Menu_C --runtime-root /path/to/psvr2-runtime
```

The helper checks source prerequisites and ELF architecture before invoking
the project's CMake build. It uses this repository's display source, the
selected userspace compiler, and the target loader. The public glibc source
archive is not a graphics runtime or a ready-made sysroot. EGL/GLES binaries
are required external inputs; this repository neither supplies nor locates
private firmware dumps. Without `--runtime-root`, the neutral ignored input
location is `.local/inputs/psvr2-runtime`.

The menu output is `output/psvr2-build/06.00/tools/wii-menu-folder/wii-menu`.
Create its `.wm` asset pack using the menu project's host tools. Upload the
executable first, followed by its adjacent `wii-menu.wm` package.

## External build state and checks

To keep all generated state outside the checkout, set `PSVR2_BUILD_STATE`
for configuration, source snapshots, and download caches, and
`PSVR2_BUILD_OUTPUT` for build output. `sources --cache` sets an explicit
archive location; `sources glibc --host-include` and `PSVR2_HOST_INCLUDE`
select an external host header directory. `--config` also accepts an explicit
configuration file. These overrides are useful for a source-only review.

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests
```

The build tests cover archive bounds, hash mismatches, source snapshot
rollback, profile selection, patch idempotence, compiler arguments, external
graphics validation, and public-source unpacking. A successful host test run
does not replace hardware validation of kernel modules or the headset.
