# Device architecture and tool interfaces

This document describes the maintained source and verified interfaces.
Addresses, instruction checks, and private driver layouts are firmware-specific.
The native host accepts exact firmware IDs `0x01100103` and `0x06000102`;
matching a version label alone does not establish compatibility.

## Host and kernel execution

`psvr2_krw_c` is a C11 host application using libusb. It implements kernel
memory access, firmware-profile validation, module loading, file transfer,
and an interactive command shell. Live instruction checks certify the selected
profile before injected kernel execution. Cached certification is invalidated
when the USB connection changes.

The device uses an AArch64 Linux 4.4.139 kernel. External modules require the
matching Sony kernel source, configuration, and symbol versions. Obtain these
from [Sony's open-source releases](https://www.playstation.com/en-us/oss/ps-vr2/);
the repository contains custom modules and small build patches rather than
a kernel source copy. Toolchain paths, imported sources, runtime libraries,
and build receipts are local inputs or generated output.

## Module chain

| Component | Purpose |
| --- | --- |
| Stage1 | Versioned command mailbox, kernel-backed command execution, and bulk file transfer. |
| USB gadget dependencies | Firmware-matched `u_serial` and `usb_f_acm`, built from the external kernel source. `libcomposite` is built for symbol validation. |
| Stage3 | ACM control shell, optional second ACM input port, software input ring, stream controls, and device-specific audio preparation. |
| `rmmod_helper` | Dependency-aware module teardown on kernels built without normal module unloading. |

Dependency setup is part of the runtime chain; there is no separate custom
Stage2 source module. Stage3's optional two-port layout uses `double_evict=1`
and changes the claimed USB-data endpoints. The control shell is `ttyGS0`;
the dedicated input bridge is `ttyGS1`. Host serial device names are assigned
by the host OS and must be selected explicitly.

`input bridge` creates `/dev/fast_input` without claiming an additional Sony
input endpoint. The ring holds 15 unread 16-byte CT frames and drops excess
frames. Readers must handle loss. Stage3 waits for raw tty readiness before
ACM activation and reopens a permanently hung-up input descriptor after
reconfiguration. `input status` reports tty and ring progress. Teardown drains
proc callbacks and workers before reclaiming their storage.

Persistent module replacement is an explicit per-file operation against
FACTORY_2. The host requires auto-detected firmware and a confirmation derived
from the candidate and existing file hashes. It revalidates the transaction
before writing and checks readback afterward. A RAM upload alone does not
replace the persistent module. See the
[maintenance reference](../psvr2_krw_c/docs/persistent-module-replacement.md).

## Display and audio

`open_vrhmd` owns its display resources and cooling worker. On the verified
06.00 path it drives a 4000×2040 RGB24 scanout, split into two 2000×2040 eyes,
using ION, MDP_RDMA, DSC, and DSI interfaces. CPU storage order, GPU texture
coordinates, and scanout byte order are separate contracts. Hardware takeover
must verify process ownership and preserve cooling; the tool does not stop
Sony's WARPA tracking worker through its unbounded STREAMOFF path.

Audio uses `/dev/audio`, the headset codec at I²C address `0x4a`, and a
4608-byte DL12 SRAM ring. The native default is stereo S32_LE at 48 kHz.
Headphone gain codes use a 0 dB origin of 121; DAC gain uses a different scale.
Microphone sidetone is a separate route from headphone playback. Cursor progress
and nonzero PCM establish buffer activity, while headphone audibility requires
a device check.

Media decoding uses authored C11 PNG, JPEG, MPEG-1 video, and MP3 implementations.
See the [codec reference](../target/psvr2/tools/open_vrhmd/codec/README.md) for
supported variants, memory bounds, and independent generated-media checks.
No codec library or media sample is bundled.

The current interfaces have host regression coverage and 06.00 device evidence.
Source builds and tests do not establish compatibility with every headset,
optical comfort, frame synchronization, tracking support, or long-run thermal
and audio behavior.
