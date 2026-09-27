# Persistent module replacement

The native host implements file transactions for the **64 MiB FACTORY_2 VFAT
filesystem at `/dev/mmcblk0p3`** on the maintained auto-detected 01.10/06.00
profiles. Runtime checks verify the device size, mount state, filesystem,
`pcbid` file, and `disp` directory before using its `modules` directory. These
checks must pass on the actual device; a firmware name alone is insufficient.

`/tmp` is volatile. `/data/modules` is a RAM copy and changing it does not by
itself update the factory filesystem. The transaction mount is
`/tmp/.psvr2-persist-mount`; recovery files live in the factory filesystem's
`modules/.psvr2-backups` directory. Persisting a module does not load it or
change the current USB configuration.

## Required inputs and session

Build the host program and matching target modules with the external kernel
SDK. Provide a static AArch64 Linux BusyBox with `fsync`, `sha256sum`, `stat`,
`mount`, `umount`, `cp`, `mv`, `mkdir`, and `sync` applets. Set `PSVR2_BUSYBOX` to
its host path before starting the toolkit. The transaction compares the target
BusyBox against that local file, uploading a verified copy to `/tmp` when
needed. No BusyBox or firmware binary is bundled.

```sh
export PSVR2_BUSYBOX=/path/to/target/busybox
psvr2_krw_c/.local/build-release/psvr2_krw_c --no-serial
```

Use automatically detected firmware. A write-capable connection and verified
Stage1 mailbox are required, including for hash/list commands that mount the
factory filesystem read-only. `--read-only` and `--fw` overrides are refused.
Stop live jobs and transfers before replacing Stage1 or Stage3. Keep the factory
filesystem unmounted on the headset and any host that can access it; headset
mount checks cannot observe a host-mounted USB volume.

## Stage1

In a toolkit session, native commands can always be selected with `krw`:

```text
krw stage1_verify /path/to/stage1.ko
krw persist_backups stage1.ko
krw stage1_install /path/to/stage1.ko
```

A missing persistent file or recovery directory can make the first two commands
fail on a first installation. A mount, identity, hash, or transport failure must
be resolved before a write.

`stage1_install` validates a bounded little-endian ELF64 AArch64 relocatable
module, description, and vermagic. It first replaces the live Stage1 through
`/tmp/stage1.ko` and tests its mailbox. If that test fails, the persistent
filesystem is not touched. The persistent preflight then reads candidate and
existing destination hashes, unmounts, and displays the complete plan.

The command requires the full displayed **transaction SHA-256**, which includes
the operation, filename, candidate, and previous destination. It is not the
candidate file hash. Interactive sessions request that hash. Noninteractive
commands require the trailing `--confirm <transaction-sha256>` argument. A
changed candidate or destination invalidates the plan.

After success, verify persistent bytes with `stage1_verify /path/to/stage1.ko`.
This checks the file on the factory filesystem, not the current live mailbox.

## Stage3 and other explicit files

Generic `persist` installs one already staged `/tmp` file and checks its bytes;
it does not validate an ELF module or test-load it. First build, upload, and test
a candidate through RAM using the matching dependencies. For a Stage3 candidate,
`--tmp` selects `/tmp`; `krw serial reset double-evict` reloads the chain with a
USB reset. Check `krw serial status` after reconnect. The no-reset reload path
is explicit: `krw serial reload double-evict`.

After validating the intended RAM candidate, use:

```text
krw fast_upload /path/to/stage3_serial.ko
krw persist stage3_serial.ko
krw persist_backups stage3_serial.ko
```

`persist` accepts one filename, at most 95 bytes, made of letters, digits, `.`,
`_`, or `-`, excluding `.` and `..`. It never accepts a directory, wildcard, or
batch. The same transaction hash confirmation applies. The persistent copy is
for later use; writing it does not replace the active module.

The Stage3 input bridge can be inspected through the matching module's
`input status` command. Its receive counters, raw-mode result, actual TTY line,
and endpoint state distinguish an idle bridge from a broken receive path.
Use one host consumer per ACM port. USB reactivation can hang up an existing
TTY file; the maintained bridge detects that hangup, discards partial packet
residue, and reopens the TTY. A normal idle read does not trigger that recovery.

## Transaction durability and recovery

Before opening a write mount, the command checks BusyBox `fsync`. It revalidates
the confirmed plan, copies an existing destination into a hash-named recovery
file, and checks the backup's hash. It stages the candidate, verifies its hash,
flushes files and affected directories, renames into place, verifies again,
then performs checked global sync and unmount. A failure at any step prevents a
success report. Failure after replacement invokes the checked restoration path.
VFAT and the storage device still determine crash behavior; flush checks are
not a guarantee against power loss.

```text
krw persist_backups stage1.ko
krw stage1_rollback <full-backup-sha256>
krw unpersist stage3_serial.ko
```

`stage1_rollback` stages a verified recovery file and restores persistent bytes;
it does not load that Stage1 or replace the live mailbox. `unpersist` moves a
file into a hash-named `.disabled` recovery entry and verifies both removal and
recovery hash. It does not delete the recovery copy or unload a live module.
Both operations require their own transaction confirmation.

A failed sync or unmount leaves uncertain filesystem state. Do not continue
with another persistent operation or reboot until the reported failure is
resolved. Recovery copies are retained; the toolkit does not silently delete
them to make space.

## Optional independent capture

A full partition capture is separate from the per-file transaction. When one
is wanted, acquire two independent reads while all writers are idle, compare
exact lengths and full hashes, and retain the outputs outside this source
repository. Host reads must be checked and flushed before relying on them.
Do not accept partially written downloads or repeated stale buffers.

### RAM-staged acquisition when bulk-IN cannot become idle

Stop a failing bulk capture. Stage bounded chunks from the unmounted block
device into a new target `/tmp` directory through a checked Stage1 command,
record each chunk's target length and SHA-256, and retrieve each file through
`krw get <target-file> <new-host-file>`. Verify exact host lengths and hashes,
then concatenate chunks in recorded order. Repeat independently and compare
the complete captures. This reads the logical block device and needs no raw
storage-writing module. Use fresh host destinations: `get` can overwrite them.

The implementation and test harness are in
[shell_persistence.c](../src/shell_persistence.c) and
[persistence_durability_harness.c](../tests/persistence_durability_harness.c).
The harness exercises failed flushes, failed unmounts, staged replacement, and
recovery without device access. Host tests do not establish persistent storage
behavior on every headset.
