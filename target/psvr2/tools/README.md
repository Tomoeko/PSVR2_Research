# Native tools

Build with the [root build helper](../../../build.sh), which selects the
firmware ABI and native userspace toolchain. Generated executables and runtime
libraries belong in the build output, not in this source directory.

- `open_vrhmd/`: display, GLES rendering, image/video viewing, WAV/MP3 playback,
  controller/frame streaming, and cooling support.
- `input_verify.c`: diagnostic reader for the legacy 16-byte controller packet
  layout. It requests `input bridge` through `/proc/stage3` by default;
  hardware `input N` is explicit with `--endpoint N` (1–9). It is not a decoder
  for the Wii Menu mouse protocol.
- `kill.c`: small AArch64 syscall helper accepting a PID and sending `SIGKILL`.
- `busybox/`: target configuration overrides for an externally obtained
  public BusyBox source archive.

Firmware binaries, private SDKs, extracted calibration data and media samples
are not part of these sources.
