# Native toolkit and BusyBox shell reference

This reference describes the current `psvr2_krw_c` host program, its 66 registered
native command names, and the BusyBox command mode available through Stage1.
Here, **native shell** means the host toolkit dispatcher, distinct from the
firmware `/bin/sh` and a target BusyBox ash terminal.
Commands run on the headset unless an argument is explicitly marked **host**.
Examples start from the repository root. Replace sample filenames, addresses,
PIDs, and device paths with values from your own session.

The native command inventory is defined in [shell.c](../src/shell.c) and the
`shell_*_commands` tables linked below. BusyBox is an external target executable. Query the selected binary for its
actual version, applets, and usage.

## Starting the host program

```sh
# Print help/version without opening the headset.
psvr2_krw_c/.local/build-release/psvr2_krw_c --help
psvr2_krw_c/.local/build-release/psvr2_krw_c --version

# Maintenance session: reuse/discover Stage1, skip automatic Stage3 deployment.
psvr2_krw_c/.local/build-release/psvr2_krw_c --no-serial

# Supply a matching 06.00 Stage1 explicitly; this replaces a loaded Stage1.
psvr2_krw_c/.local/build-release/psvr2_krw_c --no-serial \
  --stage1 output/psvr2-build/06.00/modules/stage1.ko

# One native command, with the host shell preserving the entire command string.
psvr2_krw_c/.local/build-release/psvr2_krw_c --no-serial \
  --command 'hexdump 0xffffffc00059f530 64'
```

On macOS, run the toolkit as your regular user when host USB and output
permissions permit it. `sudo` is only a host-permission workaround; target
commands already run with the toolkit's target privileges.

Normal startup connects to the headset, detects firmware, initializes arbitrary
read, certifies the live kernel profile, discovers the write primitive, and
installs native execution helpers. It then attempts the persistent
`rmmod_helper`, discovers or loads Stage1, performs requested `--fast` uploads,
and deploys Stage3 serial unless disabled. Failure of automatic serial deployment
ends startup; use `--no-serial` for a maintenance session with no deployment.
That flag does not unload an already active Stage3.

| Host option | Meaning and default |
|---|---|
| `-h`, `--help` | Show host CLI help and exit before connecting. |
| `--version` | Show host toolkit version and exit before connecting. Current host version is `1.0.0`. |
| `--fw VERSION` | Select `01.10`, `06.00`, or the corresponding numeric firmware ID. The device must report an exact match; an unavailable or contradictory report is rejected. Default: detect firmware. Persistent operations reject an override even when it matches. |
| `--read-only` | Skip later write discovery/helper injection and gate write-classified shell commands. The initial bounded exploit reader bootstrap still executes; this is not passive USB observation. Automatic serial deployment is skipped. |
| `--stage1 HOST_FILE` | Authoritative replacement: unload a current Stage1, upload this host module as `/tmp/stage1.ko`, and load it. Ignored by the startup Stage1 phase when `--s1off` or `--read-only` is selected. |
| `--s1off` | Skip startup Stage1 discovery/loading. It does not unload Stage1 or permanently prohibit an explicit shell `stage1`/`s1exec` command. |
| `--no-rmmod-helper` | Skip automatic loading of `/data/modules/rmmod_helper.ko`. Module replacement/reload may then be unavailable. |
| `--fast HOST_FILE` | Repeatable bulk upload, after Stage1 initialization and before automatic serial deployment or `--command`. Same file/path restrictions as native `fast_upload`. |
| `--serial` | Request normal automatic serial deployment; already the default. Mutually exclusive with `--no-serial`. |
| `--no-serial` | Skip automatic serial deployment. Existing serial remains active; a missing serial chain is not installed. |
| `--tmp` | Prefer `/tmp` over persistent `/data/modules` for Stage1 loading and serial module selection. It does not change BusyBox discovery, which still probes `/data/modules/busybox` first. |
| `--noevict` | Endpoint-preservation preference. The current native `serial` deployment already passes `noevict=1`, preserving the toolkit's control/bulk path, even without this flag. |
| `--double-evict` | Serial deployment additionally evicts the Sony data8/data9 endpoints and enables the controller ACM port. This changes USB endpoint ownership. |
| `--command TEXT` | Execute one line through the **native** dispatcher and exit. Use `bb ...` for a BusyBox command. Host exit status is 0 for success and 1 for failure; it is not the remote program's full exit code. |
| `--kernel-probe` | Standalone reversible exact-image execution/write probe, followed by USB/read health checks. |
| `--safety-audit` | Standalone exact-image memory audit after reader bootstrap; no later write setup. |
| `--reboot` | Standalone immediate kernel restart. Does not sync filesystems. |

The three standalone operations are mutually exclusive and must be used alone:
no firmware override, mode, upload, Stage1, serial, or command options. Help and
version are processed before this combination check. Supported firmware IDs are
`01.10` = `0x01100103` and `06.00` = `0x06000102`.

With no explicit Stage1 file, an existing verified mailbox is reused. Otherwise
the loader prefers `/data/modules/stage1.ko` unless `--tmp` is set; the host also
looks for `output/psvr2-build/<firmware>/modules/stage1.ko` and
`../output/psvr2-build/<firmware>/modules/stage1.ko` to populate `/tmp`. An explicit
file is a host path, not a path already on the headset.

The supported Stage1 is the current firmware-matched build with an immutable,
versioned mailbox descriptor. Unverified layouts and mutable ready-status strings
are not discovery authorities. If Stage1 is present but its descriptor cannot
be verified, normal startup must not silently insmod another copy or unload it.
Use `--no-serial`, stop live jobs/transfers, and explicitly replace the module
with `--stage1` or the guarded `stage1_install` workflow. Module/mailbox changes
require that matching module; host-only shell changes do not require replacing
an already matching module. See the
[persistent replacement guide](persistent-module-replacement.md).

Sources: [app.c](../src/app.c), [Stage1 loader](../src/stage1.c),
[firmware parser](../src/constants.c).

## Which shell receives a line?

| Context or form | Dispatch and working directory |
|---|---|
| Interactive startup, write-capable session with usable BusyBox | Automatically enters BusyBox mode. Prompt: `psvr2-busybox:/ #`. |
| Native mode | Prompt: `psvr2:/ #`, or `psvr2 [RO]:/ #`. Registered native names take precedence. Unknown commands fall back to BusyBox when available and writes are allowed. |
| `krw COMMAND` | Force the native dispatcher from either mode. Bare `krw` shows native help. In BusyBox mode it returns to BusyBox mode after the native command unless the command ends the session. |
| Native `busybox`, `ash`, or bare `bb` | Enter BusyBox command mode. Requires executable BusyBox and a write-capable session. |
| Native `bb COMMAND` | Run one supervised BusyBox command from target `/`. It does not change the BusyBox mode's retained working directory. |
| Native unknown command | Same one-command BusyBox fallback from `/`; native `ls`, `cd`, `cat`, `ps`, `chmod`, etc. still win by name. |
| BusyBox mode ordinary line | Supervised BusyBox ash script, starting in the retained BusyBox working directory. `krw help` lists native commands; bare `help` asks ash for builtin help. |
| Native `sh COMMAND` or `s1exec COMMAND` | Direct synchronous Stage1 mailbox execution. The payload is preserved as raw text, including shell quotes, pipes, semicolons, variables, and redirects. |

Native `cd` changes a kernel-VFS dentry and its displayed path; BusyBox `cd`
changes a separate target filesystem working directory. Neither changes the
host process's directory. Relative **host** file arguments are always relative
to the directory from which `psvr2_krw_c` was launched.

Native tokenization accepts whitespace, single/double quotes, and backslash
escaping. It does not perform glob expansion, `$variable` expansion, command
substitution, pipes, redirects, or `;` command sequencing. A quoted path is one
argument. `sh`, `s1exec`, `bb`, and BusyBox mode pass a shell payload instead.
Native `exec` tokenizes and joins its arguments, so quote grouping is lost;
prefer raw `s1exec` or `bb` when quoting matters. Native commands do not share a
generic per-command `--help` parser; use `krw help` and the syntax here.

Unsigned native numbers use C base-0 notation: decimal, `0x` hexadecimal, or
leading-zero octal. Negative values are rejected. Hex byte buffers use an even
number of adjacent hexadecimal digits, without `0x`, separators, or spaces.
Job IDs use decimal notation. Native VFS `cd` and `get` interpret a numeric
first argument as an address, so use a path such as `./123` for a numeric filename.

The host editor supports arrow/history navigation, Home/End/Delete,
Ctrl-A/Ctrl-E, Ctrl-K/Ctrl-U, Ctrl-L, and Ctrl-D. Ctrl-D on an empty input line
ends the host session. History is held in this process. Bracketed paste converts
embedded newlines/tabs to spaces; use a target script for multiple lines that
must retain line breaks.

### Tab completion and Option-arrow navigation

Press **Tab** to complete a command or filename at the cursor. A unique file or
command match gains a trailing space; a directory gains `/` so you can continue
the path. For example, after `cd /tmp`, `./open` followed by Tab becomes
`./open_vrhmd ` when that is the only matching executable. Multiple matches
extend their common prefix; press Tab again when no further extension is
possible to list the choices. No match leaves the line unchanged.

BusyBox target paths use the working directory shown in its prompt. Native VFS
paths use the native directory; arguments that name host upload/module files
use the host working directory instead. Quoted paths and spaces are completed
with shell-safe escaping. Completion enumerates names and never executes the command
you are editing. It does not evaluate variables or command substitutions.
Read-only sessions and unavailable target queries can use cached native VFS
entries, which may miss uncached files or symlink targets. If a directory query
has too many matches or its path is too long, narrow the prefix rather than
assuming that an unchanged line means the file is absent.

**Option–Left** moves to the beginning of the current/previous word;
**Option–Right** moves to the beginning of the next word. This follows Terminal's
default macOS zsh behavior, including treating path punctuation such as `/`,
`_`, `-`, and `.` as part of a word. It uses those default boundaries independently
of any custom settings in the host's own shell. Terminal's default `ESC b`/`ESC f`
sequences and modified arrow encodings are recognized; ordinary arrows still
move one character. These controls belong to the toolkit's host prompt; a
serial ash terminal uses its own line editor and bindings.

## Native command tables

**RO** means the dispatcher allows the command under `--read-only`.
**W** means it blocks the command. This is a dispatch policy, not a promise that
an allowed command has no host/USB effects: downloads create host files,
`probe_in` claims an interface, and job queries execute Stage1 control requests.
Conversely, target reads such as `fts_info`, `stage1_verify`, `fast_download`,
and `emmc` are classified W because they use execution, mailbox, mount, or
transfer paths. Use the documented argument forms; some simple handlers ignore
extra arguments, but they have no additional supported options.

### Session control and supervised jobs

| Syntax | Gate | Behavior |
|---|---|---|
| `help` | RO | List all native names and their short descriptions. |
| `exit`, `quit`, `q` | RO | End the **native** host shell. In BusyBox mode, exact bare `exit` returns to native mode, while exact bare `quit`/`q` end the host session. `exit 1` in BusyBox mode is an ash command, not a host exit request. |
| `jobs` | RO | List retained jobs: running, remote exit code, or unknown status, with command and target log path. Takes no arguments. |
| `joblog ID` | RO | Print the retained job's current last 64,000 bytes of output. Repeated calls repeat the tail; this is not a streaming cursor. |
| `jobstop ID` | W | Request SIGTERM for the supervised process group. A cancel file also covers a request made before session startup. It does not escalate to SIGKILL. Use `jobs` to observe completion. |

In BusyBox mode, prefix these native controls, for example `krw jobs` and
`krw jobstop 1`. They are separate from ash's `jobs`, `fg`, `bg`, and `wait`
builtins.

### Kernel VFS and host downloads

Source: [shell_vfs.c](../src/shell_vfs.c).

| Syntax | Gate | Arguments, defaults, and behavior |
|---|---|---|
| `ls [TARGET_PATH]` | RO | List the current native VFS directory by default. Prints type, size, kernel dentry address, and name. |
| `cd TARGET_PATH_OR_DENTRY` | RO | Set the native VFS directory. Accepts a path or numeric kernel dentry address; the inode must be a directory. |
| `pwd` | RO | Print native displayed path and current dentry address. |
| `mounts` | RO | List kernel mounts, root dentries, and mount addresses. |
| `cat TARGET_FILE` | RO | Read resident page-cache bytes, up to 1 MiB, to host stdout. Not the target `cat` applet. |
| `get TARGET_FILE [HOST_FILE]` | RO | Download resident page-cache bytes, up to 128 MiB. Default host filename: target basename. Existing host output is overwritten. |
| `get KERNEL_ADDRESS SIZE [HOST_FILE]` | RO | Download `SIZE` bytes of kernel memory. Default host output: `memory.bin`. Allocates the requested buffer; use bounded `dumpmem` for a capture with partial-file protection. |
| `get_all [HOST_DIRECTORY]` | RO | Recursively download the **current** native VFS directory, appending its displayed target path beneath the host directory. Default base: `psvr2_krw_c/downloads/VFS`. Regular files are limited to 64 MiB each; traversal tracks at most 4096 directories. Existing host files may be overwritten. |
| `passthrough on\|off` | W | Send the headset's vendor passthrough setting (`on` payload 1,3; `off` payload 0,0). |

These VFS operations walk in-memory dentries and resident file pages; they do
not make the kernel read an evicted file from storage. Symlinks are not resolved
as a normal filesystem shell would resolve them. `get_all` skips special files,
oversize files, and entries whose pages cannot be read; a successful return is
not proof of a complete filesystem backup. Use BusyBox or Stage1 bulk transfer
when normal target filesystem access is required. The native prompt path is a
display value; selecting a numeric dentry does not reconstruct its canonical
path.

For a block-device backup when bulk-IN cannot become idle, stage bounded,
fresh `/dev/mmcblk0p3` read chunks into unique `/tmp` regular files, then use
native `get` to retrieve their resident pages. Require exact sizes and matching
device/host SHA-256 for every chunk, concatenate in order into a new host file,
and compare two independent captures with full target-device hashes. `get`
overwrites host destinations, so reserve a fresh private host directory and
check every destination is absent. This is ordinary post-XTS block reading;
it does not require `emmc_plain` or another storage module. Follow the complete
[RAM-staged backup procedure](persistent-module-replacement.md#ram-staged-acquisition-when-bulk-in-cannot-become-idle).

### Tasks, memory, mappings, and patching

Source: [shell_memory.c](../src/shell_memory.c).

| Syntax | Gate | Arguments, defaults, and behavior |
|---|---|---|
| `ps` | RO | List kernel task addresses, `mm` addresses, and exact task `comm` names. |
| `hexdump KERNEL_ADDRESS [LENGTH]` | RO | Print kernel bytes. Default 256 bytes; maximum 16 MiB. |
| `dumpmem KERNEL_ADDRESS LENGTH HOST_FILE` | RO | Capture 1 byte through 16 MiB of kernel memory in 4096-byte chunks. Rejects address overflow and existing output/`.partial`. |
| `dumpuser TASK_ADDRESS_OR_COMM USER_VA LENGTH HOST_FILE` | RO | Capture 1 byte through 16 MiB of a process's virtual memory, using its page tables. `COMM` is an exact task name from native `ps`. Same output protection as `dumpmem`. |
| `readptr KERNEL_ADDRESS` | RO | Read and print a 64-bit kernel value. |
| `write KERNEL_ADDRESS U64` | W | Write eight little-endian bytes with the verified byte/STRB path. |
| `fwrite KERNEL_ADDRESS U64` | W | Fast eight-byte STR write; it does not perform the same byte-by-byte readback verification as `write`. |
| `write8 KERNEL_ADDRESS BYTE` | W | Write one verified byte; value must be 0..255. |
| `writebuf KERNEL_ADDRESS HEX_BYTES` | W | Write a byte buffer using fast eight-byte writes followed by verified trailing byte writes. |
| `testwrite` | W | Run the reversible exact-image kernel execution/write probe. This is not a free-form scratch-memory writer. |
| `kpte KERNEL_VA` | RO | Walk a live kernel mapping and print page-table entry address/value and permissions, or `unmapped`. |
| `kmap` | RO | Print profile-defined kernel mapping samples at 2 MiB steps and BSS samples at 4 KiB steps. It is not an exhaustive map of all addresses. |
| `physmap PHYSICAL_ADDRESS` | RO | Scan live kernel page tables for mappings containing this physical address; print matching kernel VAs and L1/L2 block or L3 page type. |
| `vma TASK_ADDRESS` | RO | List process VMAs and r/w/x flags. Numeric task address only; bounded to 500 VMA entries. |
| `base TASK_ADDRESS USER_VA` | RO | Resolve a process user VA through its PGD to a kernel address. Numeric task address only. |
| `patch COMM OFFSET HEX_BYTES` | W | Write process bytes at first-VMA start plus `OFFSET`; exact process name, not a PID or an arbitrary absolute user VA. Uses byte writes. |
| `safety_audit`, `scan_bss` | RO | Run the exact-image live memory safety audit. `scan_bss` is an alias; finding zero bytes does not certify an arbitrary region as an execution workspace. |
| `modlist` | RO | List loaded module structure address, core address, size, and name. |
| `backdoor TARGET_PATH` | W | Patch the kernel `uevent_helper` path, including its terminator. Maximum 255 path bytes. It does not upload, run, or validate that executable. |
| `et` | W | Wait up to five seconds for the live `VrhmdMain` host-type patch site, patch its gate byte, activate eye tracking through vendor commands, and verify bulk endpoint 0x87. |

`dumpmem` and `dumpuser` write `HOST_FILE.partial` first and publish the final
filename without replacement after successful capture. A failed capture retains
its partial bytes. Raw memory writes and process patches have no general undo
transaction: an incorrect address or value can damage the live kernel/process.
Firmware-profile certification does not validate an address supplied by the user.

### Execution, Stage1, authentication, and device control

Source: [shell_stage1.c](../src/shell_stage1.c).

| Syntax | Gate | Arguments, defaults, and behavior |
|---|---|---|
| `exec COMMAND...` | W | Execute through the independent UMH helper, with captured output. No Stage1 required. Uses the firmware `/bin/sh` and its normal system PATH. Native argument tokenization is applied before joining the command. |
| `sh RAW_COMMAND`, `s1exec RAW_COMMAND` | W | Aliases for direct synchronous Stage1 execution, preserving raw shell syntax. Host wait: 15 seconds. |
| `upload HOST_FILE [REMOTE_NAME]` | W | Upload one nonempty host file through UMH plus kernel page-cache writes. Default target: `/tmp/<host-basename>`; optional argument is a simple target filename, not a destination directory. Existing target is renamed to `<target>.previous`. Attempts executable mode 0755. Does not need Stage1. Maximum page collection: 16384 pages, approximately 64 MiB. |
| `stage1 [HOST_MODULE]` | W | No argument: reuse/discover a valid mailbox or load selected target Stage1. With a host file: force live replacement through `/tmp/stage1.ko`, ignoring the persistent copy. This does not persist the file. |
| `auth_reflect` | W | Bounded device-certificate reflection on 01.10/06.00. Requires matching Stage1, BusyBox, and bulk-IN. Kills `VrhmdMain`, arms a controlled relaunch, receives device/session events, and performs the authentication exchange. This alters the live stock application/session. |
| `inject` | W | Inject or repair native STR execution helpers. |
| `chmod MODE TARGET_PATH` | W | Execute target `chmod` via Stage1. Use normal target chmod modes, for example `755` or `+x`. Because native arguments are joined without requoting, use `s1exec chmod ...` for paths requiring shell quotes. |
| `serial [noreset\|reload\|reset\|status] [double-evict]` | W | Deploy or inspect the serial module chain; details below. At most one mode plus the endpoint option. |
| `wdt [COUNT]` | W | Send the vendor boot-failure-count setting. Default 31; accepted range 0..255. This is distinct from changing the persistent `disable_wdt` FTS flag. |
| `rcm` | W | Stage1 writes `2` to `/sys/bus/mmc/devices/mmc0:0001/boot_partition_for_boot`, changing boot-partition selection. The handler itself does not perform a reboot. |
| `disable_autoboot [on\|off]` | W | No argument: query `disable_autoboot` and `disable_wdt` through `/sie/bin/fts.elf`. `on`: set `disable_autoboot=1`; `off`: set it to 0. Changes a persistent boot flag. |
| `fts_info` | W | Query those two persistent FTS boot flags. |
| `fts_fix` | W | Set both `disable_autoboot` and `disable_wdt` to 0 through FTS. Changes persistent boot flags. |
| `recv_test` | W | Ask `/proc/stage1` to receive 100 bytes into `/tmp/test`, then print proc status. It arms a receiver; it does not send those host bytes. |
| `reboot`, `kernel_reboot` | W | Immediate exact-image kernel emergency restart; leave the host shell even if dispatch fails. No filesystem sync. |
| `shutdown`, `poweroff` | W | Immediate exact-image kernel power-off path; leave the host shell even if dispatch fails. Finish writes/unmounts first. |

For `upload`, keep the remote name simple and free of slashes or shell quoting
characters; the page-cache lookup expects a child of the `/tmp` mount. Use
`fast_upload` or a tar archive for larger packages. Neither uploader recursively
uploads a host directory. Build modules for the detected firmware; these
commands do not convert a module between firmware families.

`serial` normally uses `/data/modules`, falling back to `/tmp`; `--tmp` restricts
module selection to `/tmp`. Initial deployment requires `u_serial.ko`,
`usb_f_acm.ko`, and `stage3_serial.ko` together in the chosen target directory,
plus executable BusyBox at `/data/modules/busybox` or `/tmp/busybox`.

| Serial mode | Behavior |
|---|---|
| No mode | Initial full-chain deployment with scheduled USB reset. Reuses an already active Stage3, or waits for a pending reset, when appropriate. |
| `noreset` | Deploy the full chain without requesting USB reset when absent; reuse a verified active chain. |
| `reload` | Reload Stage3 without USB reset; requires active writable `/proc/rmmod_helper` when Stage3 is loaded. If Stage3 is absent, falls back to full-chain deployment. |
| `reset` | Reload Stage3 with USB reset; if Stage3 is absent, deploy the dependency chain first. Validates reconnect and live status. |
| `status` | Inspect live Stage3 status without deploying. Still requires BusyBox and is W-gated. |
| `double-evict` | Can accompany a mode; enables the extra controller ACM port and evicts the data8/data9 endpoint owners. Also inherited from host `--double-evict`. |

Default `serial` and `serial noreset`, without `double-evict`, reuse an existing
chain only after checking its live state. The USB serial configuration must
have an active handle, be marked serial-safe, and have a certified exact kernel
profile for the same connection generation; `stage3_serial`, `u_serial`, and
`usb_f_acm` must be loaded. A bounded Stage1 query checks readable/writable
`/proc/stage3`, the `ttyGS0` character device, absence of the shell-stop marker,
live numeric wrapper/child
PID receipts, and the child's standard input/output/error descriptors pointing
to `/dev/ttyGS0`. It uses existence, permission, `kill -0`, and readlink checks;
it does not open the tty, alter endpoints, or terminate processes.

`/proc/stage3` is a mutable last-operation log, not the health authority. An
unrelated audiopatch/takeover success or error does not trigger redeployment.
Both the live probe and subsequent status read must succeed. If an existing
Stage3 or serial USB configuration cannot be verified, the host preserves it
and fails without automatic load, unload, or reset; use `--no-serial` for
maintenance.

Automatic waiting for a pending reset requires loaded Stage3, a USB
configuration not yet marked serial-safe, and the initialization status
beginning `OK acm x`, containing ` injected iface_next=`, and ending exactly
` (USB reset pending)`. A generic `OK` operation is insufficient. Explicit
`serial reload`, `serial reset`, and `double-evict` remain intentional
configuration changes; `serial status` prints the operation log.

## Bulk transfers and eMMC capture

Sources: [shell_bulk.c](../src/shell_bulk.c),
[shell_emmc.c](../src/shell_emmc.c).

| Syntax | Gate | Arguments, defaults, and behavior |
|---|---|---|
| `fast_upload HOST_FILE...` | W | Bulk-upload one or more regular host files to `/tmp/<basename>`. Matching Stage1 and interface-5 bulk OUT required. Names may contain letters/digits, `.`, `_`, `-`; no target directory or rename option. All files share one receive manifest, which must fit the Stage1 command limit. Avoid duplicate basenames. |
| `fast_download ABSOLUTE_TARGET_PATH [HOST_FILE] [SIZE]` | W | Matching Stage1 and bulk IN required. Default host name: target basename. Optional nonzero size overrides the requested byte count, useful for block devices. To supply size, also supply the host filename. A zero size means automatic size. Target path permits letters/digits, `/`, `.`, `_`, `-` only. |
| `probe_in` | RO | Discover/claim interface 5, alternate 0, bulk IN and OUT; print interface, endpoint addresses, and maximum packet size. No arguments. |
| `emmc list\|all\|PARTITION` | W | `list` prints accepted names. A partition or `all` queries target size, then calls bulk download, saving `<partition>.img` in the host working directory. Requires Stage1, bulk IN, and `/data/modules/busybox`. |
| `emmc_plain ABSOLUTE_START_LBA SECTOR_COUNT [HOST_FILE]` | W | 06.00-only read-only post-XTS acquisition using an already loaded managed `msdc_plaintext_reader`. Sectors are 512 bytes. Default host name: `emmc_plain_<start>_<count>.img`. |

`fast_download` creates `HOST_FILE.partial` exclusively and publishes the final
name without replacement; existing output or partial names prevent completion.
A failed transfer retains partial bytes. Transfers stop/drain the Stage1 sender
before receiving file bytes. The current host first requires the matching
Stage1 `send_prepare` protocol: stop the prior sender, validate and cancel
queued native IN requests with their completion bookkeeping preserved, then
require native busy count zero. It drains a bounded controller tail and
requires a timeout with **zero bytes transferred** before starting the new
sender. A successful zero-length packet or a timeout carrying bytes does not
establish idle. A missing protocol, preparation error, or continuous traffic
fails the transport; it does not authorize a persistent write. Cancellation
does not block concurrent target writers, so quiesce them and keep other host
consumers off the same endpoint. Use the latest matching host/module build;
older Stage1 layouts or missing preparation commands are unsupported.

A reported full byte count does not establish fresh device traffic. Verify
capture sizes and device/host hashes before using a download. If bulk idle
cannot be established, stop the bulk capture and use the
[control-memory capture procedure](persistent-module-replacement.md#ram-staged-acquisition-when-bulk-in-cannot-become-idle).
Sources: [probe classifier](../src/gaze.c), [USB capture guards](../src/shell_bulk.c).

Accepted eMMC names: `mmcblk0boot0`, `mmcblk0boot1`, `mmcblk0p1` through
`mmcblk0p20`, and `mmcblk0`. `all` includes both the entire main device and its
individual partitions, so the captures overlap and require substantial host
space. These commands capture bytes presented by the selected device nodes;
`emmc` is not a promise of decrypted partition contents.

`emmc_plain` does not load the reader. It verifies reader ABI 1, firmware family
0x0600, read-only capability, and the configured LBA window. The requested
absolute range must lie inside that window, and `aes_switch_on` must remain 1.
It reads up to 16 MiB per chunk through `/dev/msdc_plaintext`, stages bytes in
`/tmp/.psvr2_emmc_plain`, and retrieves resident pages. It needs Stage1 and
`/data/modules/busybox`, uses a 60-second wait for each target `dd`, and protects
the host final/partial names. Its W classification reflects target execution,
staging, and cleanup; the managed reader itself is read-only. Its external
module must already be loaded with the intended LBA window. No reader module
or decrypted storage image is included.

## Persistent installation and recovery

Source: [shell_persistence.c](../src/shell_persistence.c). All six commands are
**W**, including read-only hash/list operations, because they require Stage1
execution and may upload a verified BusyBox to volatile `/tmp`.

| Syntax | What it changes or checks |
|---|---|
| `stage1_install HOST_STAGE1_KO [--confirm TRANSACTION_SHA256]` | Validate a bounded ELF64 little-endian AArch64 relocatable Stage1 module, its description and vermagic; replace the **live** Stage1 through `/tmp`; self-test its mailbox; then plan/confirm/install its persistent bytes. |
| `stage1_verify [HOST_STAGE1_KO]` | Hash persistent `stage1.ko` using a read-only mount; optionally compare with a validated host candidate. Does not test the current live module or reload it. |
| `stage1_rollback FULL_BACKUP_SHA256 [--confirm TRANSACTION_SHA256]` | Find a hash-verified `stage1.ko.<hash>.bak` or `.disabled` recovery file, stage it as `/tmp/stage1.rollback.ko`, and restore persistent bytes. Does not load/test the backup or replace current live Stage1. |
| `persist_backups [FILENAME]` | List recovery filenames, optionally restricted to a single filename prefix. Uses a read-only mount. |
| `persist FILENAME [--confirm TRANSACTION_SHA256]` | Install the already present `/tmp/FILENAME` as persistent `modules/FILENAME`. Generic filename/hash transaction; it does not validate module ABI/ELF or test-load the file. |
| `unpersist FILENAME [--confirm TRANSACTION_SHA256]` | Move persistent `modules/FILENAME` into `.psvr2-backups/FILENAME.<sha256>.disabled`, then verify removal/recovery hash. Does not delete the recovery copy or unload a live module. |

Names are a single filename, at most 95 bytes, made of letters/digits, `.`, `_`,
or `-`, excluding `.` and `..`. No directory paths or globs. `--confirm` must be
the trailing option/value pair, and its value must exactly match the displayed
full **transaction hash**, not merely the candidate or backup file hash.

Persistence requires auto-detected 01.10/06.00, a verified Stage1 mailbox, and
the selected local BusyBox hash. It verifies `/dev/mmcblk0p3` is the exact
64 MiB FACTORY_2 filesystem, currently unmounted; mounts vfat at
`/tmp/.psvr2-persist-mount` with `nosuid,nodev,noexec`; checks `pcbid` and `disp`
identity entries; and targets its `modules` directory. Preflight is read-only.
The write phase requires the verified BusyBox's `fsync` applet before opening
a write mount. It revalidates the confirmed plan, preserves existing bytes by
hash, stages replacement, and checks `fsync 'PATH'` for regular files and the
recovery, modules, and mount-root directories before and after replacement.
Restoration and disable paths use the same flush checks. Global sync and
checked unmount remain required; global sync alone does not establish per-file
writeback success. A missing applet or failed flush prevents success. An
unmount or
rollback error is not a completed transaction; resolve it before rebooting or
starting another persistent operation.

Changed persistent bytes require typing the full transaction hash on a TTY, or
supplying `--confirm` for noninteractive use. An identical candidate/destination
can succeed without persistent writes or confirmation. `stage1_install` changes
the live module **before** persistent confirmation: cancelling the disk
transaction leaves its candidate live. The first attempt may also upload
BusyBox to `/tmp` before confirmation. There is no `--force`, `--yes`, or
wildcard installation mode.

`/data/modules` is the RAM copy populated during boot, not the mounted persistent
filesystem. Editing it with BusyBox does not create a durable install. Installing
to FACTORY_2 makes bytes available on a subsequent boot; loading a module is a
separate runtime operation, and a loaded module does not survive reboot.

## BusyBox execution, job lifetime, and status

BusyBox availability means an executable binary at `/data/modules/busybox`,
otherwise `/tmp/busybox`. The host caches the selected path. Stage1 itself probes
those locations for each request and uses BusyBox ash, falling back to `/bin/sh`
if neither is executable. Normal BusyBox mode requires a matching Stage1 mailbox
as well as the binary; merely uploading BusyBox does not create that mailbox.

Each BusyBox line becomes a target script and a supervised `setsid` process
group. Scripts are uploaded in bounded mailbox chunks, so a user line may exceed
the direct Stage1 payload limit. The host waits up to 15 seconds for ordinary
completion. A longer command is reported as **running**, releases the mailbox,
and leaves the prompt usable for another command. Ctrl-C while the host is
waiting requests group SIGTERM and may leave a **stopping** job for inspection.

```text
psvr2-busybox:/ # /tmp/open_vrhmd audio /tmp/sample.mp3 20
[running job 1] ...
psvr2-busybox:/ # krw jobs
psvr2-busybox:/ # krw joblog 1
psvr2-busybox:/ # krw jobstop 1
psvr2-busybox:/ # krw jobs
```

Use the foreground command form above. Adding `&` inside the user's script
lets that script finish before its background descendants and can bypass the
intended job lifetime/cleanup tracking. Processes that create a new session or
change process group can also escape group termination.

The private target directory is `/tmp/.psvr2-job-<host-pid>-<session>-<id>`:
`cmd` and `run` hold scripts, `in` is the stdin FIFO, `out` the full log, `pid`
the published supervisor group ID, `launch` the launcher PID, `cancel` an early
stop request, and `status` the final shell exit code. FIFO stdin retains a writer
to avoid EOF-sensitive applications exiting immediately. This is **not a
terminal**: later prompt input runs a new command instead of reaching the job.
For simple input, write to the reported job directory's FIFO from another
BusyBox command, for example `printf 'stop\n' > /tmp/.psvr2-job-.../in`.
Interactive editors, passwords, full-screen tools, and media key controls belong
on the serial shell.

Short jobs print captured output and are removed with their private directory.
Retained jobs keep their logs and metadata; `jobs`/`joblog` do not remove finished
entries. Automatic output and `joblog` show at most the last 64,000 bytes;
redirect or retrieve the full target `out` file when needed. Host session exit
does not signal retained jobs or delete their directories. Job IDs exist only
in that host process: a new invocation cannot rediscover them through `jobs`.
Consequently, a one-shot `--command 'bb ...'` that returns running leaves target
work without reusable host job-ID controls. Prefer a retained REPL, serial, or
the application's own stop command for long-lived work.

BusyBox mode preserves only the working directory from a command that completes
during its wait and emits the normal completion marker. Each line runs a fresh
ash: exports, aliases, functions, shell options, and shell job tables do not
persist across lines. An explicit `exit`/`exec` inside a script can bypass the
working-directory marker. A retained long job's later cwd does not update the
prompt. Keep related shell setup and use on the same line, or put it in a script.

Remote UMH/Stage1 wait statuses are normalized once: exit 1 is shown as 1, not
256; a signal is conventionally `128 + signal`; negative kernel errors are
preserved. BusyBox commands return host success for completed exit 0 or a job
successfully left running. A completed nonzero code/transport failure returns
host failure. Direct `s1exec`/`sh` returns failure for a nonzero remote result.
Some fixed native wrappers report the remote result separately and principally
return mailbox/transport success, so inspect printed `[exit ...]` and device
status as well. A missing final supervisor status, failed liveness probe, or
lost response leaves the job's status unknown and preserves its directory and
log. Only a published `status` file proves completion. Group probes and signals
use the verified BusyBox `kill` applet without `--`, which the headset's shell
and applet reject.

### Direct mailbox and UMH limits

`s1exec`/`sh` is synchronous and is not supervised. Its host timeout is 15
seconds; timing out does not kill the process or free a still-busy Stage1 worker.
The current host formatter accepts at most 477 raw command bytes after allowing
for its PATH prefix, with up to 65,535 output bytes. Do not run a persistent
foreground application through it. Explicit background launch requires stdin,
stdout, and stderr to be detached correctly, and supplies none of the automatic
job tracking above.

`exec` uses the separate UMH helper with a five-second host wait, a 224-byte
command buffer including PATH/capture wrapper, and up to 65,536 bytes from
`/tmp/.kout`. It does not automatically discover BusyBox or manage jobs. Its
firmware-shell commands may have fewer utilities than the selected BusyBox.
Stage1 captures its direct requests in `/tmp/.s1out`; commands sharing either
control channel should be serialized.

## Discovering BusyBox, builtins, applets, and target applications

Provide a static AArch64 Linux BusyBox executable with the applets needed by
these commands. Set `PSVR2_BUSYBOX` to its host path before starting the toolkit.
The persistent installer compares that local file with the target executable
and requires its `fsync` applet. A helper at
`output/psvr2-build/<firmware>/tools/busybox` is also discovered automatically.
No BusyBox binary is included. The installed target binary's own output is
authoritative.

Run these in BusyBox mode, choosing the executable path shown by the host:

```sh
/data/modules/busybox --version
/data/modules/busybox --list-full
/data/modules/busybox --help ls
/data/modules/busybox ls --help
help
type cd
command -v busybox
command -v open_vrhmd
ls -l /tmp /data/modules /sie/bin
```

Use `/tmp/busybox` instead when that is the selected executable. `--list` prints
applet names; `--list-full` includes each applet's install-directory prefix.
`busybox --help APPLET` is also useful for `test`, `[` and `[[`, whose normal
`--help` argument can have expression semantics. These queries inspect the
target build rather than assuming GNU coreutils or a desktop Bash installation.

BusyBox applets and shell features depend on the supplied build configuration.
Compiling an applet does not ensure that the headset has its required kernel
feature, device node, filesystem, network service, or permissions. Inspect the
selected executable rather than assuming a fixed applet set.

Stage1's environment provides `HOME=/` and a PATH containing `/data/modules`,
`/tmp`, `/tmp/bin`, `/bin`, `/sbin`, `/usr/bin`, and `/usr/sbin`; the host prefix
prepends `/tmp/bin:/tmp`. Standalone ash can execute compiled applets even when
no symlinks are installed. Use `type`/`command -v` to distinguish builtins,
applets, and target executables, and use an absolute path to select a custom
program unambiguously. There is no toolkit command that guarantees an exhaustive
list of every application in all target filesystems.

### Useful applets and shell features

These are examples of useful applets when the selected binary includes them.
Ask the installed applet for `--help` before relying on options; BusyBox options
differ from similarly named GNU/BSD tools.

| Task | Useful applets or ash forms |
|---|---|
| Files and paths | `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `touch`, `ln`, `readlink`, `realpath`, `stat`, `find`, `du`, `df`, `tree`, `chmod`, `chown`. |
| Text and pipelines | `printf`, `echo`, `grep`, `sed`, `awk`, `head`, `tail`, `cut`, `tr`, `sort`, `uniq`, `wc`, `xargs`, `tee`; shell `\|`, `&&`, `\|\|`, `;`, `>`, `>>`, `<`, `2>&1`. |
| Binary inspection and hashes | `hexdump`, `od`, `xxd`, `strings`, `cmp`, `sha256sum`, `md5sum`, `base64`, `base32`. |
| Packages and compression | `tar`, `cpio`, `ar`, `gzip`/`gunzip`, `bzip2`/`bunzip2`, `xz`/`unxz`, `zip` is not in this manifest; `unzip` is. |
| Processes and timing | `ps`, `top`, `pgrep`, `pidof`, `pstree`, `kill`, `killall`, `timeout`, `sleep`, `usleep`, `nice`, `renice`, `taskset`, `setsid`, `nohup`. |
| Kernel and module inspection | `uname`, `dmesg`, `lsmod`, `modinfo`, `lsusb`, `lsblk`, `lsof`, `free`, `uptime`, `vmstat`. |
| Target writes and module changes | `insmod`, `rmmod`, `modprobe`, `mount`, `umount`, `sync`, `sysctl`, `devmem`, `i2cset`, `i2ctransfer`; these change target state and bypass native transaction guards. |
| Storage inspection/acquisition | `blockdev`, `blkid`, `dd`, `losetup`, `fdisk`, `hexdump`. `dd of=/dev/...`, filesystem makers, erase/discard applets, and partition editors can overwrite persistent storage directly. |
| Network diagnostics/transfers | `ip`, `ifconfig`, `netstat`, `ping`, `nslookup`, `wget`, `nc`/`netcat`, `tftp`, `ftpget`, `ftpput`, `httpd`; usable connectivity must exist first. |
| FIFO and scripts | `mkfifo`, `test`/`[`/`[[`, `read`, `export`, `set`, `unset`, `trap`, `wait`, `getopts`, `if`, `case`, `for`, `while`, functions, `$?`, `$!`, `$(...)`, `$((...))`. |
| Interactive terminal tools | `vi`, `less`, `more`, `top`, `stty`, `tty`, `resize`, `clear`; use the serial terminal rather than the mailbox command prompt. |

For example, preserve a package's directories by uploading a tar file and
extracting it on the target:

```sh
# Host, before opening the toolkit:
tar -cf /tmp/demo.tar -C /host/path/to/package .
```

```text
# Toolkit BusyBox prompt:
krw fast_upload /tmp/demo.tar
mkdir -p /tmp/demo; tar -xf /tmp/demo.tar -C /tmp/demo
chmod +x /tmp/demo/program
/tmp/demo/program --help
```

Host `/tmp/demo.tar` and target `/tmp/demo.tar` are different files on different
machines. Extraction changes target paths. Use the native persistence commands
for guarded durable module installation; arbitrary BusyBox `cp`, `dd`, mount,
FTS, or sysfs writes do not inherit those checks.

## Serial shell is a separate terminal

The host BusyBox command prompt uses Stage1 mailbox RPCs. Stage3 provides an
interactive ash on target `/dev/ttyGS0` through USB ACM. Native `krw` commands,
host job IDs, and the host line editor are not available inside that ash terminal.
Stage3 can fall back to firmware `/bin/sh` when BusyBox is unavailable, although
native serial deployment requires BusyBox.

```sh
# macOS: substitute the actual callout ACM device.
psvr2_krw_c/.local/build-release/psvr2_serial_tool /dev/cu.usbmodemXXXX shell
# Linux:
psvr2_krw_c/.local/build-release/psvr2_serial_tool /dev/ttyACM0 shell
```

Ctrl-] disconnects the **host serial client**; it is not a remote application
stop request. Ctrl-C is passed to the target terminal. End an application's
interactive session with its own controls or target signal before disconnecting
when that is the intended outcome. Stage3 respawns its interactive shell after
shell exit while the module remains active.

The separate serial helper also accepts:

| Host serial syntax | Semantics |
|---|---|
| `psvr2_serial_tool DEVICE command TEXT` | Write one command line to the target terminal; does not wait for or capture its exit status. |
| `psvr2_serial_tool DEVICE send HOST_FILE [TARGET_PATH]` | Base64-transfer one file. Default target `/tmp/<host-basename>`; relative target arguments are placed beneath `/tmp`, absolute target paths are used as supplied. This path does not automatically chmod the file. |
| `psvr2_serial_tool DEVICE receive TARGET_PATH [HOST_FILE]` | Base64-download one target file. Default host filename is the target basename. Host output can be overwritten. |

Do not issue serial transfer framing while another application owns the terminal
stdin. For long-running interactive media or scripts that need a terminal,
serial supplies the target stdin/stdout interaction the mailbox mode does not.
Sources: [serial_tool.c](../tools/serial_tool.c),
[Stage3 serial implementation](../../target/psvr2/modules/stage3_serial/stage3_serial.c).

## State changes and recovery expectations

- Native reads and captures inspect live memory or create host files; restricted
  mode still includes the initial reader bootstrap.
- Volatile operations such as uploads, process patches, module loading, device
  configuration, and job execution change the current boot. They can interrupt
  stock services and are not automatically undone when the host shell exits.
- Persistent module transactions, FTS boot flags, and boot-partition selection
  affect later boots. Only the persistent module transaction commands implement
  the described hash confirmation, backup, verification, and rollback protocol.
- Immediate `reboot`/`shutdown` paths are reset controls, not graceful filesystem
  shutdown. In BusyBox mode, exact bare `reboot`, `shutdown`, and `poweroff` are
  routed to these native paths. In that mode, added arguments or explicit BusyBox
  paths use ordinary shell lookup/applet semantics; native reset commands have
  no applet options. The current applet manifest includes `reboot` and `poweroff`
  but no `shutdown` applet. Ordinary BusyBox power applets depend on PID 1
  behavior, which may not reliably complete a restart on this headset.

No live device commands were issued to prepare this reference. Native syntax,
dispatch gates, limits, and lifecycle descriptions are source-derived; applet
availability/features describe the generated local build and must be checked
against the deployed binary when using a different artifact.
