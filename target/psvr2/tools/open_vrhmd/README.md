# open_vrhmd

A native userspace display/audio tool for the firmware-specific PSVR2 research
runtime. Build through the [root build helper](../../../../build.sh); `--help`
lists commands and arguments. Display commands own a process lock and start a
cooling child that is stopped during teardown.

The display path allocates an ION buffer, performs the required ION cache sync,
and programs the existing RDMA pipeline. GLES commands render into that native
surface. `--stream` exports display frames through the Stage3 frame bridge.
The runtime reads headset optical calibration on-device; calibration files are
not distributed with the tool.

The authored C11 codecs support PNG/JPEG images and MPEG1 video elementary/program
streams. MPEG reconstruction covers I/P/B frames, motion compensation, custom
quantizers, padded YUV420 planes and display-order timestamps. MPEG2 video
extensions, dimensions above 2048×2048 and dimension changes are explicit errors. Program-stream audio is
skipped; the video command does not play an embedded audio track. Raw packed
monochrome video remains available through the existing video path. The decoder
API supports rewind and linear seek, and reports malformed input separately
from a clean end.

Audio supports PCM WAV and MPEG1 Layer III MP3. The hardware path uses stereo
S32_LE DL12 SRAM at 48 kHz; other source rates retain the existing speed/pitch
warning. Buffer writes use aligned 32-bit stores and a DMA-cursor watchdog.
The headphone gain is bounded at 0 dB; the default 50% setting retains the
existing -18 dB attenuation policy. Playback clears DAC sidetone mixing and
reapplies the gain after the driver startup sequence. Teardown mutes the DAC
and clears playback SRAM. Microphone capture gain is not changed by playback.

Host tests use mocked hardware and sanitizer builds. Codec tests generate
synthetic inputs locally: PNG checks exact pixels, JPEG compares against
Pillow, and MPEG/MP3 compare against FFmpeg. These are optional validation
dependencies; the target uses the authored C decoders. See the
[codec reference](codec/README.md) for limits. No third-party decoder code or
media sample is included.
