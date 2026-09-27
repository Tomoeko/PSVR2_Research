#!/usr/bin/env python3
"""Native PSVR2 target build orchestrator.

This tool intentionally uses only Python's standard library and the host
toolchain. It does not invoke Docker.
"""

from __future__ import annotations

import argparse
import copy
import fcntl
import hashlib
import json
import os
import platform
import shlex
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
import zipfile
from contextlib import contextmanager
from dataclasses import dataclass
from functools import wraps
from pathlib import Path
from typing import Any, Iterable, Iterator


REPO_ROOT = Path(__file__).resolve().parents[1]
TARGET_ROOT = REPO_ROOT / "target" / "psvr2"
MANIFEST_PATH = TARGET_ROOT / "build-manifest.json"
SOURCE_CATALOG_PATH = MANIFEST_PATH
DEFAULT_STATE = Path(os.environ.get("PSVR2_BUILD_STATE", str(REPO_ROOT / ".local"))).expanduser().resolve()
DEFAULT_CONFIG = DEFAULT_STATE / "psvr2-build.json"
DEFAULT_OUTPUT = Path(os.environ.get("PSVR2_BUILD_OUTPUT", str(REPO_ROOT / "output/psvr2-build"))).expanduser().resolve()
DEFAULT_SDK_ROOT = DEFAULT_STATE / "psvr2-sdk"
DEFAULT_EXTERNAL_ROOT = DEFAULT_STATE / "psvr2-external"
DEFAULT_FIRMWARE = "06.00"
KERNEL_PATCH_ROOT = TARGET_ROOT / "kernel-patches"
KERNEL_SOURCE_PATCHES = (
    KERNEL_PATCH_ROOT / "u_serial-throughput.patch",
    KERNEL_PATCH_ROOT / "usb-function-unload.patch",
)
MAX_ARCHIVE_MEMBERS = 250_000
MAX_ARCHIVE_EXPANDED_BYTES = 2 * 1024 * 1024 * 1024
MAX_EXTERNAL_ARCHIVE_BYTES = 128 * 1024 * 1024
ZERO_GIT_OID = "0" * 40


class BuildError(RuntimeError):
    pass


_SOURCE_LEASE_DEPTH = 0


@contextmanager
def source_lease() -> Iterator[None]:
    global _SOURCE_LEASE_DEPTH
    if _SOURCE_LEASE_DEPTH:
        _SOURCE_LEASE_DEPTH += 1
        try:
            yield
        finally:
            _SOURCE_LEASE_DEPTH -= 1
        return

    DEFAULT_SDK_ROOT.mkdir(parents=True, exist_ok=True)
    lock_path = DEFAULT_SDK_ROOT / ".source-operation.lock"
    with lock_path.open("a+", encoding="utf-8") as stream:
        fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
        _SOURCE_LEASE_DEPTH = 1
        try:
            yield
        finally:
            _SOURCE_LEASE_DEPTH = 0
            fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def requires_source_lease(function: Any) -> Any:
    @wraps(function)
    def wrapped(*args: Any, **kwargs: Any) -> Any:
        with source_lease():
            return function(*args, **kwargs)

    return wrapped


@dataclass(frozen=True)
class Settings:
    firmware: str
    kernel_source: Path | None
    kernel_build: Path
    cross_prefix: str
    tool_cc: tuple[str, ...]
    glibc_version: str
    make: str
    jobs: int
    sysroot: Path | None
    dynamic_linker: str
    host_paths: tuple[Path, ...]
    source_archive_sha256: str | None
    verbose: bool
    source_snapshot: str | None = None
    source_tree_git_sha1: str | None = None
    source_tree_sha256: str | None = None
    source_verification: str | None = None

    @property
    def output(self) -> Path:
        return DEFAULT_OUTPUT / self.firmware


@dataclass(frozen=True)
class SourceCandidate:
    firmware: str
    archive: Path
    archive_sha256: str
    source_tree_git_sha1: str
    source_tree_sha256: str
    verification: str


def read_json(path: Path, default: Any) -> Any:
    if not path.exists():
        return default
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BuildError(f"cannot read {path}: {exc}") from exc


def write_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def manifest() -> dict[str, Any]:
    data = read_json(MANIFEST_PATH, None)
    if not isinstance(data, dict) or data.get("schema") != 1:
        raise BuildError(f"unsupported or missing manifest: {MANIFEST_PATH}")
    return data


def source_catalog() -> dict[str, Any]:
    data = manifest()["sources"]["kernel"]
    if not isinstance(data, dict) or data.get("schema") != 1:
        raise BuildError(f"unsupported or missing source catalog: {SOURCE_CATALOG_PATH}")
    profiles = data.get("firmwares")
    if not isinstance(profiles, dict) or set(profiles) != set(manifest()["firmwares"]):
        raise BuildError(
            "source catalog firmware profiles do not match the build manifest"
        )
    for firmware, profile in profiles.items():
        hashes = profile.get("archive_sha256")
        tree = profile.get("source_tree_git_sha1")
        tree_sha256 = profile.get("source_tree_sha256")
        if (
            not isinstance(hashes, list)
            or not hashes
            or any(
                not isinstance(value, str)
                or len(value) != 64
                or any(character not in "0123456789abcdef" for character in value)
                for value in hashes
            )
            or not isinstance(tree, str)
            or len(tree) != 40
            or any(character not in "0123456789abcdef" for character in tree)
            or not isinstance(tree_sha256, str)
            or len(tree_sha256) != 64
            or any(
                character not in "0123456789abcdef"
                for character in tree_sha256
            )
        ):
            raise BuildError(f"invalid source identity catalog entry for {firmware}")
    return data


def catalog_archive_owner(archive_sha256: str) -> str | None:
    owners = [
        firmware
        for firmware, profile in source_catalog()["firmwares"].items()
        if archive_sha256 in profile["archive_sha256"]
    ]
    if len(owners) > 1:
        raise BuildError(
            f"archive hash is ambiguously assigned in {SOURCE_CATALOG_PATH}: "
            + ", ".join(owners)
        )
    return owners[0] if owners else None


def catalog_tree_owner(source_tree_sha256: str) -> str | None:
    owners = [
        firmware
        for firmware, profile in source_catalog()["firmwares"].items()
        if source_tree_sha256 == profile["source_tree_sha256"]
    ]
    if len(owners) > 1:
        raise BuildError(
            f"source-tree identity is ambiguously assigned in {SOURCE_CATALOG_PATH}: "
            + ", ".join(owners)
        )
    return owners[0] if owners else None


def firmware_version(value: str) -> tuple[int, int]:
    try:
        major_text, minor_text = value.strip().split(".", 1)
        major = int(major_text, 10)
        minor = int(minor_text, 10)
    except (AttributeError, TypeError, ValueError) as exc:
        raise BuildError(
            f"invalid firmware version {value!r}; expected a value such as 6.00"
        ) from exc
    if major < 0 or minor < 0 or minor > 99:
        raise BuildError(f"invalid firmware version {value!r}")
    return major, minor


def resolve_firmware_profile(value: str) -> str:
    profiles = manifest()["firmwares"]
    if value in profiles:
        return value
    requested = firmware_version(value)
    matches = [
        name
        for name, profile in profiles.items()
        if firmware_version(profile["system_min"])
        <= requested
        <= firmware_version(profile["system_max"])
    ]
    if len(matches) == 1:
        return matches[0]
    supported = ", ".join(profiles)
    raise BuildError(
        f"unsupported firmware {value!r}; choose a source family or supported "
        f"system version from: {supported}"
    )


def firmware_profiles() -> list[str]:
    return list(manifest()["firmwares"])


def sdk_store_path() -> Path:
    return DEFAULT_SDK_ROOT / "source-store.git"


def sdk_active_path() -> Path:
    return DEFAULT_SDK_ROOT / "active"


def config_path(args: argparse.Namespace) -> Path:
    value = getattr(args, "config", None)
    return Path(value).expanduser().resolve() if value else DEFAULT_CONFIG


def load_config(args: argparse.Namespace) -> dict[str, Any]:
    data = read_json(config_path(args), {"schema": 1, "firmwares": {}})
    if not isinstance(data, dict) or data.get("schema") != 1:
        raise BuildError(f"unsupported configuration schema in {config_path(args)}")
    data.setdefault("firmwares", {})
    return data


def default_make() -> str:
    return shutil.which("gmake") or shutil.which("make") or "make"


def default_cross_prefix() -> str:
    configured = os.environ.get("CROSS_COMPILE")
    if configured:
        return configured
    for prefix in ("aarch64-linux-gnu-", "aarch64-none-elf-"):
        if executable(prefix + "gcc"):
            return prefix
    return "aarch64-none-elf-" if platform.system() == "Darwin" else "aarch64-linux-gnu-"


def default_tool_cc() -> tuple[str, ...]:
    if executable("aarch64-linux-gnu-gcc"):
        return ("aarch64-linux-gnu-gcc",)
    if executable("zig"):
        return ("zig", "cc")
    return ("zig", "cc") if platform.system() == "Darwin" else ("aarch64-linux-gnu-gcc",)


def command_tuple(value: str | list[str] | tuple[str, ...] | None) -> tuple[str, ...]:
    if value is None:
        return ()
    if isinstance(value, str):
        result = tuple(shlex.split(value))
    else:
        result = tuple(value)
    if not result:
        raise BuildError("compiler command cannot be empty")
    return result


def positive_jobs(value: str) -> int:
    jobs = int(value)
    if jobs < 1:
        raise argparse.ArgumentTypeError("jobs must be at least 1")
    return jobs


def resolve_path(value: str | None) -> Path | None:
    return Path(value).expanduser().resolve() if value else None


def resolve_settings(args: argparse.Namespace, require_kernel: bool) -> Settings:
    data = load_config(args)
    requested_firmware = getattr(args, "firmware", None) or DEFAULT_FIRMWARE
    firmware = resolve_firmware_profile(requested_firmware)

    firmware_config = data.get("firmwares", {}).get(firmware, {})
    source_snapshot = firmware_config.get("source_snapshot")
    if source_snapshot:
        activate_source_snapshot(firmware, source_snapshot)
        kernel_source = sdk_active_path()
    else:
        kernel_source = resolve_path(
            getattr(args, "kernel_source", None) or firmware_config.get("kernel_source")
        )
    kernel_build = resolve_path(
        getattr(args, "kernel_build", None) or firmware_config.get("kernel_build")
    )
    if kernel_build is None:
        kernel_build = DEFAULT_OUTPUT / firmware / "kernel"

    cross_prefix = (
        getattr(args, "cross_prefix", None)
        or data.get("cross_prefix")
        or default_cross_prefix()
    )
    tool_cc = command_tuple(
        getattr(args, "tool_cc", None) or data.get("tool_cc")
    ) or default_tool_cc()
    glibc_version = (
        getattr(args, "glibc_version", None) or data.get("glibc_version") or "2.28"
    )
    make = getattr(args, "make", None) or data.get("make") or default_make()
    jobs = getattr(args, "jobs", None) or data.get("jobs") or max(1, os.cpu_count() or 1)
    sysroot = resolve_path(getattr(args, "sysroot", None) or data.get("sysroot"))
    dynamic_linker = (
        getattr(args, "dynamic_linker", None)
        or data.get("dynamic_linker")
        or "/lib/ld-2.28.so"
    )
    host_paths = tuple(
        Path(value).expanduser().resolve()
        for value in data.get("host_paths", [])
    )
    source_archive_sha256 = firmware_config.get("source_archive_sha256")
    source_tree_git_sha1 = firmware_config.get("source_tree_git_sha1")
    source_tree_sha256 = firmware_config.get("source_tree_sha256")
    source_verification = firmware_config.get("source_verification")
    if require_kernel and kernel_source is None:
        raise BuildError(
            "no kernel source configured; run './build.sh configure "
            "--firmware VERSION --kernel-source /path/to/linux' or "
            "'./build.sh sdk import /path/to/archive --firmware VERSION'"
        )
    if require_kernel:
        assert kernel_source is not None
        apply_kernel_source_patches(kernel_source)
    return Settings(
        firmware=firmware,
        kernel_source=kernel_source,
        kernel_build=kernel_build,
        cross_prefix=cross_prefix,
        tool_cc=tool_cc,
        glibc_version=glibc_version,
        make=make,
        jobs=jobs,
        sysroot=sysroot,
        dynamic_linker=dynamic_linker,
        host_paths=host_paths,
        source_archive_sha256=source_archive_sha256,
        verbose=getattr(args, "verbose", False),
        source_snapshot=source_snapshot,
        source_tree_git_sha1=source_tree_git_sha1,
        source_tree_sha256=source_tree_sha256,
        source_verification=source_verification,
    )


def display_command(command: Iterable[str]) -> str:
    return shlex.join(str(part) for part in command)


def run(
    command: list[str],
    *,
    cwd: Path | None = None,
    verbose: bool = False,
    env: dict[str, str] | None = None,
) -> None:
    if verbose:
        location = f" (in {cwd})" if cwd else ""
        print(f"+ {display_command(command)}{location}")
    try:
        process_env = os.environ.copy()
        if env:
            process_env.update(env)
        subprocess.run(command, cwd=cwd, env=process_env, check=True)
    except FileNotFoundError as exc:
        raise BuildError(f"command not found: {command[0]}") from exc
    except subprocess.CalledProcessError as exc:
        raise BuildError(f"command failed with exit status {exc.returncode}") from exc


def capture(
    command: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    input_text: str | None = None,
) -> str:
    try:
        process_env = os.environ.copy()
        if env:
            process_env.update(env)
        result = subprocess.run(
            command,
            cwd=cwd,
            env=process_env,
            input=input_text,
            capture_output=True,
            text=True,
            check=True,
        )
    except FileNotFoundError as exc:
        raise BuildError(f"command not found: {command[0]}") from exc
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.strip() or exc.stdout.strip()
        suffix = f": {detail}" if detail else ""
        raise BuildError(
            f"command failed with exit status {exc.returncode}{suffix}"
        ) from exc
    return result.stdout.strip()


def ensure_snapshot_store() -> Path:
    git = executable("git")
    if not git:
        raise BuildError(
            "Git is required for deduplicated kernel-source snapshots; install "
            "Apple Command Line Tools or Git"
        )
    store = sdk_store_path()
    if not (store / "HEAD").is_file():
        store.parent.mkdir(parents=True, exist_ok=True)
        run([git, "init", "--bare", "--quiet", str(store)])
        run([git, "--git-dir", str(store), "config", "core.autocrlf", "false"])
        run([git, "--git-dir", str(store), "config", "core.filemode", "true"])
    refs_directory = store / "refs"
    if refs_directory.is_dir():
        for metadata_junk in refs_directory.rglob(".DS_Store"):
            metadata_junk.unlink()
    return store


def snapshot_ref(firmware: str) -> str:
    return f"refs/heads/firmware/{firmware}"


def snapshot_environment() -> dict[str, str]:
    return {
        "GIT_AUTHOR_NAME": "PSVR2 source importer",
        "GIT_AUTHOR_EMAIL": "local@psvr2.invalid",
        "GIT_AUTHOR_DATE": "2000-01-01T00:00:00Z",
        "GIT_COMMITTER_NAME": "PSVR2 source importer",
        "GIT_COMMITTER_EMAIL": "local@psvr2.invalid",
        "GIT_COMMITTER_DATE": "2000-01-01T00:00:00Z",
    }


def store_source_tree(source: Path) -> str:
    """Write a source tree to the object store without changing any refs."""
    store = ensure_snapshot_store()
    git = executable("git")
    assert git is not None
    descriptor, index_name = tempfile.mkstemp(
        prefix=".snapshot-index-", dir=DEFAULT_SDK_ROOT
    )
    os.close(descriptor)
    index = Path(index_name)
    index.unlink()
    environment = snapshot_environment()
    environment["GIT_INDEX_FILE"] = str(index)
    base = [git, "--git-dir", str(store), "--work-tree", str(source)]
    try:
        capture(base + ["add", "--all", "--force"], env=environment)
        return capture(
            [git, "--git-dir", str(store), "write-tree"],
            env=environment,
        )
    finally:
        if index.exists():
            index.unlink()


def commit_source_tree(
    tree: str,
    *,
    firmware: str,
    archive_sha256: str,
    parent_snapshot: str | None = None,
) -> str:
    store = ensure_snapshot_store()
    git = executable("git")
    assert git is not None
    commit_command = [git, "--git-dir", str(store), "commit-tree", tree]
    if parent_snapshot:
        commit_command.extend(["-p", parent_snapshot])
    return capture(
        commit_command,
        env=snapshot_environment(),
        input_text=(
            f"PSVR2 Linux source family {firmware}\n\n"
            f"Archive-SHA256: {archive_sha256}\n"
            f"Source-Tree-Git-SHA1: {tree}\n"
        ),
    )


def read_snapshot_ref(firmware: str) -> str | None:
    store = sdk_store_path()
    git = executable("git")
    if not git or not (store / "HEAD").is_file():
        return None
    result = subprocess.run(
        [
            git,
            "--git-dir",
            str(store),
            "show-ref",
            "--verify",
            "--hash",
            snapshot_ref(firmware),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    return result.stdout.strip() if result.returncode == 0 else None


def update_snapshot_refs(
    updates: dict[str, str | None],
    *,
    expected: dict[str, str | None],
) -> None:
    if not updates:
        return
    store = ensure_snapshot_store()
    git = executable("git")
    assert git is not None
    commands: list[str] = []
    for firmware, new_value in updates.items():
        old_value = expected.get(firmware)
        ref = snapshot_ref(firmware)
        if new_value is None:
            if old_value is not None:
                commands.append(f"delete {ref} {old_value}")
        else:
            commands.append(f"update {ref} {new_value} {old_value or ZERO_GIT_OID}")
    if commands:
        capture(
            [git, "--git-dir", str(store), "update-ref", "--stdin"],
            input_text="\n".join(commands) + "\n",
        )


def activate_source_snapshot(firmware: str, snapshot: str) -> Path:
    store = sdk_store_path()
    if not (store / "HEAD").is_file():
        raise BuildError(
            f"source snapshot store is missing for firmware {firmware}: {store}"
        )
    active = sdk_active_path()
    marker = active / ".psvr2-source-snapshot.json"
    current = read_json(marker, {}) if marker.is_file() else {}
    if (
        current.get("firmware") == firmware
        and current.get("snapshot") == snapshot
    ):
        validate_kernel_source(active)
        return active

    git = executable("git")
    if not git:
        raise BuildError("Git is required to activate a kernel-source snapshot")
    DEFAULT_SDK_ROOT.mkdir(parents=True, exist_ok=True)
    staging = Path(
        tempfile.mkdtemp(prefix=f".active-{firmware}-", dir=DEFAULT_SDK_ROOT)
    )
    descriptor, index_name = tempfile.mkstemp(
        prefix=".checkout-index-", dir=DEFAULT_SDK_ROOT
    )
    os.close(descriptor)
    index = Path(index_name)
    index.unlink()
    backup: Path | None = None
    try:
        environment = {"GIT_INDEX_FILE": str(index)}
        capture(
            [
                git,
                "--git-dir",
                str(store),
                "--work-tree",
                str(staging),
                "read-tree",
                "--reset",
                "-u",
                snapshot,
            ],
            env=environment,
        )
        validate_kernel_source(staging)
        write_json(
            staging / ".psvr2-source-snapshot.json",
            {"schema": 1, "firmware": firmware, "snapshot": snapshot},
        )
        if active.exists():
            backup = Path(
                tempfile.mkdtemp(prefix=".active-previous-", dir=DEFAULT_SDK_ROOT)
            )
            backup.rmdir()
            active.replace(backup)
        staging.replace(active)
        if backup:
            shutil.rmtree(backup)
            backup = None
    except BaseException:
        if staging.exists():
            shutil.rmtree(staging)
        if backup and backup.exists():
            if not active.exists():
                backup.replace(active)
            else:
                shutil.rmtree(backup)
        raise
    finally:
        if index.exists():
            index.unlink()
    print(f"Activated Linux source family {firmware} ({snapshot[:12]}).")
    return active


def compact_snapshot_store() -> None:
    store = sdk_store_path()
    git = executable("git")
    if git and (store / "HEAD").is_file():
        print("Packing shared kernel-source objects...")
        run([git, "--git-dir", str(store), "gc", "--prune=now"])


def executable(candidate: str) -> str | None:
    if os.sep in candidate:
        path = Path(candidate).expanduser()
        return str(path) if path.is_file() and os.access(path, os.X_OK) else None
    return shutil.which(candidate)


def find_homebrew() -> str | None:
    discovered = executable("brew")
    if platform.system() == "Darwin":
        native_prefix = "/opt/homebrew" if platform.machine() == "arm64" else "/usr/local"
        native = executable(native_prefix + "/bin/brew")
        if native:
            return native
        # Avoid installing an Intel dependency chain into an Apple Silicon
        # build (or conversely). Explicit nonstandard installations remain valid.
        if discovered and not discovered.startswith(("/opt/homebrew/", "/usr/local/")):
            return discovered
        return None
    return discovered


def executable_in_common_macos_locations(name: str) -> str | None:
    candidates = (
        Path("/opt/homebrew/bin") / name,
        Path("/usr/local/bin") / name,
        Path("/opt/local/bin") / name,
    )
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    if name.startswith("aarch64-none-elf-"):
        toolchain_pattern = (
            "/Applications/ArmGNUToolchain/*/aarch64-none-elf/bin/" + name
        )
        for candidate in sorted(Path("/").glob(toolchain_pattern.lstrip("/")), reverse=True):
            if candidate.is_file() and os.access(candidate, os.X_OK):
                return str(candidate)
    return None


def homebrew_prefix(brew: str) -> Path | None:
    try:
        result = subprocess.run(
            [brew, "--prefix"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    prefix = Path(result.stdout.strip())
    return prefix if prefix.is_dir() else None


def discover_macos_tool(
    name: str,
    brew: str | None = None,
    configured: str | None = None,
) -> str | None:
    discovered = executable(configured) if configured else None
    if discovered:
        return discovered
    if brew:
        prefix = homebrew_prefix(brew)
        if prefix:
            candidate = executable(str(prefix / "bin" / name))
            if candidate:
                return candidate
    return executable(name) or executable_in_common_macos_locations(name)


def is_gnu_make(candidate: str) -> bool:
    try:
        result = subprocess.run(
            [candidate, "--version"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return False
    return result.stdout.startswith("GNU Make ")


def discover_gnu_make(data: dict[str, Any], brew: str | None = None) -> str | None:
    configured = data.get("make")
    candidates = (
        discover_macos_tool("gmake", brew, configured),
        discover_macos_tool("gmake", brew),
        discover_macos_tool("make", brew),
    )
    for candidate in dict.fromkeys(item for item in candidates if item):
        if is_gnu_make(candidate):
            return candidate
    return None


def cross_prefix_from_gcc(gcc: str) -> str:
    if not gcc.endswith("gcc"):
        raise BuildError(f"cannot derive a cross-tool prefix from {gcc}")
    return gcc[: -len("gcc")]


def complete_cross_prefix(prefix: str) -> bool:
    return all(executable(prefix + tool) for tool in ("gcc", "strip", "objcopy"))


def discover_cross_prefix(data: dict[str, Any], brew: str | None = None) -> str | None:
    configured = data.get("cross_prefix") or os.environ.get("CROSS_COMPILE")
    if configured and complete_cross_prefix(configured):
        resolved_gcc = executable(configured + "gcc")
        if resolved_gcc:
            return cross_prefix_from_gcc(resolved_gcc)
    for compiler_name in ("aarch64-none-elf-gcc", "aarch64-linux-gnu-gcc"):
        gcc = discover_macos_tool(compiler_name, brew)
        if gcc:
            prefix = cross_prefix_from_gcc(gcc)
            if complete_cross_prefix(prefix):
                return prefix
    return None


def discover_target_compiler(
    data: dict[str, Any],
    brew: str | None = None,
) -> tuple[str, ...] | None:
    configured = command_tuple(data.get("tool_cc")) if data.get("tool_cc") else ()
    if configured:
        resolved = executable(configured[0])
        if resolved:
            return (resolved, *configured[1:])
    linux_gcc = discover_macos_tool("aarch64-linux-gnu-gcc", brew)
    if linux_gcc:
        return (linux_gcc,)
    zig = discover_macos_tool("zig", brew)
    return (zig, "cc") if zig else None


def discover_kernel_host_helpers(brew: str | None = None) -> dict[str, str | None]:
    return {
        "bc": discover_macos_tool("bc", brew),
        "perl": discover_macos_tool("perl", brew),
    }


def host_tool_version_ok(path: str, minimum: tuple[int, int] = (3, 20)) -> bool:
    try:
        result = subprocess.run([path, "--version"], check=True,
                                capture_output=True, text=True)
        version = result.stdout.splitlines()[0].split()[-1].split(".")
        return tuple(int(part) for part in version[:2]) >= minimum
    except (OSError, subprocess.CalledProcessError, ValueError, IndexError):
        return False


def host_pkg_config_environment(brew: str | None) -> dict[str, str]:
    environment = os.environ.copy()
    prefix = homebrew_prefix(brew) if brew else None
    if prefix:
        paths = [str(prefix / "lib/pkgconfig"), str(prefix / "share/pkgconfig")]
        if environment.get("PKG_CONFIG_PATH"):
            paths.append(environment["PKG_CONFIG_PATH"])
        environment["PKG_CONFIG_PATH"] = os.pathsep.join(paths)
    return environment


def discover_host_build_helpers(brew: str | None = None) -> dict[str, str | None]:
    cmake = discover_macos_tool("cmake", brew)
    ctest = discover_macos_tool("ctest", brew)
    cmake = cmake if cmake and host_tool_version_ok(cmake) else None
    ctest = ctest if ctest and host_tool_version_ok(ctest) else None
    pkg_config = discover_macos_tool("pkg-config", brew) or discover_macos_tool("pkgconf", brew)
    libusb = None
    if pkg_config:
        try:
            result = subprocess.run([pkg_config, "--exists", "libusb-1.0"],
                                    env=host_pkg_config_environment(brew),
                                    check=False, capture_output=True, text=True)
            if result.returncode == 0:
                libusb = "libusb-1.0"
        except OSError:
            pass
    return {"cmake": cmake, "ctest": ctest, "pkg_config": pkg_config, "libusb": libusb}


def confirm(question: str, *, input_fn: Any = input) -> bool:
    while True:
        try:
            answer = input_fn(f"{question} yes (y) or no (n): ").strip().lower()
        except EOFError as exc:
            raise BuildError(
                "interactive input is unavailable; rerun in a terminal or pass --yes"
            ) from exc
        if answer in ("yes", "y"):
            return True
        if answer in ("no", "n"):
            return False
        print("Please answer yes (y) or no (n).")


def dragged_path(value: str) -> Path:
    """Accept plain, quoted, or shell-escaped paths pasted by terminal drag/drop."""
    raw = value.strip()
    if not raw:
        raise BuildError("no archive path was supplied")
    direct = Path(raw).expanduser()
    if direct.exists():
        return direct.resolve()
    try:
        parts = shlex.split(raw)
    except ValueError as exc:
        raise BuildError(f"cannot parse archive path: {exc}") from exc
    if len(parts) != 1:
        raise BuildError(
            "the archive path is ambiguous; drag one file into the terminal prompt"
        )
    return Path(parts[0]).expanduser().resolve()


def cross_tool(settings: Settings, name: str, required: bool = True) -> str:
    candidate = settings.cross_prefix + name
    resolved = executable(candidate)
    if resolved:
        return resolved
    if required:
        raise BuildError(
            f"missing cross tool {candidate!r}; configure --cross-prefix with the "
            "prefix of an AArch64 Linux GCC/binutils toolchain"
        )
    return candidate


def firmware_cflags(firmware: str) -> list[str]:
    return list(manifest()["firmwares"][firmware].get("cflags", []))


def kernel_make_base(settings: Settings) -> list[str]:
    assert settings.kernel_source is not None
    command = [
        settings.make,
        "-C",
        str(settings.kernel_source),
        f"O={settings.kernel_build}",
        "ARCH=arm64",
        f"CROSS_COMPILE={settings.cross_prefix}",
        f"PSVR2_CFLAGS={' '.join(firmware_cflags(settings.firmware))}",
    ]
    return command


def host_path_environment(settings: Settings) -> dict[str, str]:
    environment: dict[str, str] = {}
    if settings.host_paths:
        existing = os.environ.get("PATH", "")
        entries = [str(path) for path in settings.host_paths]
        if existing:
            entries.append(existing)
        environment["PATH"] = os.pathsep.join(entries)
    return environment


def kernel_host_environment(settings: Settings) -> dict[str, str] | None:
    environment = host_path_environment(settings)
    if platform.system() == "Darwin":
        compatibility_headers = Path(os.environ.get("PSVR2_HOST_INCLUDE", str(REPO_ROOT / ".local/inputs/host-include"))).expanduser().resolve()
        if not (compatibility_headers / "elf.h").is_file():
            raise BuildError("macOS needs an external ELF header; run ./build.sh sources glibc or provide .local/inputs/host-include/elf.h")
        flags = (
            f"-I{compatibility_headers} "
            "-Wno-string-plus-int -Wno-format-security"
        )
        existing_flags = os.environ.get("HOST_EXTRACFLAGS")
        environment["HOST_EXTRACFLAGS"] = (
            f"{existing_flags} {flags}" if existing_flags else flags
        )
    return environment or None


def executable_with_host_paths(name: str, settings: Settings) -> str | None:
    discovered = executable(name)
    if discovered:
        return discovered
    for directory in settings.host_paths:
        candidate = directory / name
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    return None


def validate_kernel_source(path: Path) -> None:
    expected = [path / "Makefile", path / "Kconfig", path / "arch" / "arm64"]
    missing = [str(item) for item in expected if not item.exists()]
    if missing:
        raise BuildError(
            f"{path} is not a complete Linux kernel source tree; missing: "
            + ", ".join(missing)
        )


def apply_kernel_source_patches(kernel_source: Path) -> None:
    """Apply tracked PSVR2 kernel patches, accepting an already-patched tree."""
    kernel_source = kernel_source.resolve()
    git = executable("git")
    if not git:
        raise BuildError("Git is required to apply tracked kernel-source patches")

    for patch in KERNEL_SOURCE_PATCHES:
        if not patch.is_file():
            raise BuildError(f"tracked kernel patch is missing: {patch}")
        apply_prefix = [
            git,
            "apply",
            "--unsafe-paths",
            f"--directory={kernel_source}",
        ]
        command = [
            *apply_prefix,
            "--check",
            "--whitespace=nowarn",
            str(patch),
        ]
        applicable = subprocess.run(
            command,
            capture_output=True,
            text=True,
            check=False,
        )
        if applicable.returncode == 0:
            run(
                [
                    *apply_prefix,
                    "--whitespace=nowarn",
                    str(patch),
                ]
            )
            print(f"Applied tracked kernel patch {patch.name}.")
            continue

        already_applied = subprocess.run(
            [
                *apply_prefix,
                "--reverse",
                "--check",
                "--whitespace=nowarn",
                str(patch),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if already_applied.returncode == 0:
            continue

        detail = applicable.stderr.strip() or already_applied.stderr.strip()
        suffix = f": {detail}" if detail else ""
        raise BuildError(
            f"kernel source does not match either side of {patch.name}{suffix}"
        )


def apply_darwin_kernel_compatibility(kernel_source: Path) -> None:
    """Make Linux 4.4's existing Mach-O modpost support work with modern ld64."""
    if platform.system() != "Darwin":
        return
    file2alias = kernel_source / "scripts" / "mod" / "file2alias.c"
    try:
        contents = file2alias.read_text(encoding="utf-8")
    except OSError as exc:
        raise BuildError(f"cannot inspect Darwin host helper {file2alias}: {exc}") from exc
    old_section = 'section("__TEXT, " #name)'
    new_section = 'section("__DATA, " #name)'
    if "_mh_execute_header" in contents and new_section in contents:
        return
    old_call_text = (
        'char *__cat(pstart_,name) = getsectdata("__TEXT",\t\\\n'
        '\t\t\t#name, &__cat(name,_len));\t\t\t\\'
    )
    old_call_data = old_call_text.replace('"__TEXT"', '"__DATA"')
    new_call = (
        "char *__cat(pstart_,name) = (char *)getsectiondata(\t\\\n"
        '\t\t\t&_mh_execute_header, "__DATA", #name,\t\t\\\n'
        "\t\t\t&__cat(name,_len));\t\t\t\t\\"
    )
    old_call = old_call_text if old_call_text in contents else old_call_data
    if old_call not in contents or (
        old_section not in contents and new_section not in contents
    ):
        raise BuildError(
            "the kernel's Darwin modpost layout is unrecognized; refusing to patch "
            f"{file2alias}"
        )
    contents = contents.replace(
        "#include <mach-o/getsect.h>",
        "#include <mach-o/getsect.h>\n#include <mach-o/ldsyms.h>",
        1,
    )
    contents = contents.replace(old_call, new_call, 1)
    contents = contents.replace(old_section, new_section, 1)
    try:
        file2alias.write_text(contents, encoding="utf-8")
    except OSError as exc:
        raise BuildError(
            f"cannot apply the required Darwin host-only compatibility patch: {exc}"
        ) from exc
    print(
        "Applied modern macOS compatibility to Linux modpost "
        "(header-aware Mach-O lookup in __DATA)."
    )


def prepare_kernel(settings: Settings, defconfig: str, force_config: bool = False) -> None:
    assert settings.kernel_source is not None
    validate_kernel_source(settings.kernel_source)
    apply_darwin_kernel_compatibility(settings.kernel_source)
    if not executable(settings.make):
        raise BuildError(f"GNU make not found: {settings.make}")
    if platform.system() == "Darwin" and not is_gnu_make(settings.make):
        raise BuildError(f"kernel builds require GNU make, not {settings.make}")
    cross_tool(settings, "gcc")
    build_marker = settings.kernel_build / ".psvr2-source-snapshot.json"
    marker_data = read_json(build_marker, {}) if build_marker.is_file() else {}
    if settings.source_snapshot and marker_data.get("snapshot") != settings.source_snapshot:
        if settings.kernel_build.exists():
            print(
                f"Resetting stale kernel output for source family "
                f"{settings.firmware}..."
            )
            shutil.rmtree(settings.kernel_build)
    settings.kernel_build.mkdir(parents=True, exist_ok=True)

    config = settings.kernel_build / ".config"
    if force_config or not config.exists():
        print(f"Configuring Linux {settings.firmware} with {defconfig}...")
        run(
            kernel_make_base(settings) + [defconfig],
            verbose=settings.verbose,
            env=kernel_host_environment(settings),
        )

    print(f"Preparing Linux {settings.firmware} for external modules...")
    run(
        kernel_make_base(settings)
        + ["scripts", "modules_prepare", f"-j{settings.jobs}"],
        verbose=settings.verbose,
        env=kernel_host_environment(settings),
    )
    if settings.source_snapshot:
        write_json(
            build_marker,
            {
                "schema": 1,
                "firmware": settings.firmware,
                "snapshot": settings.source_snapshot,
                "archive_sha256": settings.source_archive_sha256,
                "source_tree_git_sha1": settings.source_tree_git_sha1,
                "source_tree_sha256": settings.source_tree_sha256,
            },
        )


def kernel_ready(settings: Settings) -> bool:
    ready = all(
        (
            (settings.kernel_build / ".config").is_file(),
            (settings.kernel_build / "include" / "config" / "auto.conf").is_file(),
            (settings.kernel_build / "include" / "generated" / "autoconf.h").is_file(),
        )
    )
    if ready and settings.source_snapshot:
        marker = read_json(
            settings.kernel_build / ".psvr2-source-snapshot.json", {}
        )
        ready = marker.get("snapshot") == settings.source_snapshot
    return ready


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_source_tree_sha256(path: Path) -> str:
    """Hash Git-relevant paths, executable modes, symlink targets, and contents."""
    digest = hashlib.sha256()
    entries = sorted(
        (
            candidate
            for candidate in path.rglob("*")
            if (candidate.is_symlink() or candidate.is_file())
            and ".git" not in candidate.relative_to(path).parts
        ),
        key=lambda candidate: os.fsencode(candidate.relative_to(path).as_posix()),
    )
    for item in entries:
        relative = os.fsencode(item.relative_to(path).as_posix())
        item_stat = item.lstat()
        if item.is_symlink():
            mode = b"120000"
            content_hash = hashlib.sha256(os.fsencode(os.readlink(item))).digest()
        else:
            mode = b"100755" if item_stat.st_mode & stat.S_IXUSR else b"100644"
            content_hash = bytes.fromhex(sha256_file(item))
        digest.update(mode + b"\0" + relative + b"\0" + content_hash + b"\0")
    return digest.hexdigest()


def sha256_tree(path: Path) -> str:
    digest = hashlib.sha256()
    for item in sorted(candidate for candidate in path.rglob("*") if candidate.is_file()):
        relative = item.relative_to(path).as_posix()
        if relative.startswith(".") or "/." in relative:
            continue
        digest.update(relative.encode("utf-8") + b"\0")
        digest.update(bytes.fromhex(sha256_file(item)))
    return digest.hexdigest()


def write_receipt(
    output: Path,
    *,
    kind: str,
    name: str,
    settings: Settings,
    source_hash: str,
    artifact: Path,
    metadata: dict[str, Any] | None = None,
    include_kernel_source: bool = True,
) -> None:
    data = {
        "schema": 1,
        "kind": kind,
        "name": name,
        "firmware": settings.firmware,
        "source_sha256": source_hash,
        "artifact": artifact.name,
        "artifact_sha256": sha256_file(artifact),
        "cross_prefix": settings.cross_prefix,
        "tool_cc": list(settings.tool_cc),
        "glibc_version": settings.glibc_version,
    }
    if include_kernel_source and settings.source_archive_sha256:
        data["kernel_source_archive_sha256"] = settings.source_archive_sha256
    if include_kernel_source and settings.source_snapshot:
        data["kernel_source_snapshot"] = settings.source_snapshot
    if include_kernel_source and settings.source_tree_git_sha1:
        data["kernel_source_tree_git_sha1"] = settings.source_tree_git_sha1
    if include_kernel_source and settings.source_tree_sha256:
        data["kernel_source_tree_sha256"] = settings.source_tree_sha256
    if include_kernel_source and settings.source_verification:
        data["kernel_source_verification"] = settings.source_verification
    if metadata:
        data.update(metadata)
    write_json(output, data)


def strip_module(
    source: Path,
    destination: Path,
    recipe: dict[str, Any],
    settings: Settings,
) -> None:
    strip = cross_tool(settings, "strip")
    remove_sections = recipe.get("remove_sections", [])
    strip_source = source
    temporary: Path | None = None
    if remove_sections:
        objcopy = cross_tool(settings, "objcopy")
        temporary = destination.with_suffix(".objcopy.ko")
        command = [objcopy]
        for section in remove_sections:
            command.append(f"--remove-section={section}")
        command.extend([str(source), str(temporary)])
        run(command, verbose=settings.verbose)
        strip_source = temporary
    try:
        run(
            [strip, "-s", "--strip-unneeded", "-o", str(destination), str(strip_source)],
            verbose=settings.verbose,
        )
    finally:
        if temporary and temporary.exists():
            temporary.unlink()


def kernel_source_hash(kernel_source: Path, sources: list[str]) -> str:
    digest = hashlib.sha256()
    for source_name in sources:
        source = kernel_source / source_name
        if not source.is_file():
            raise BuildError(f"kernel runtime-module source is missing: {source}")
        digest.update(source_name.encode("utf-8") + b"\0")
        digest.update(bytes.fromhex(sha256_file(source)))
    return digest.hexdigest()


def build_kernel_runtime_modules(
    modules: list[dict[str, Any]],
    settings: Settings,
) -> list[Path]:
    """Build and package ordered in-tree modules required by a custom module."""
    assert settings.kernel_source is not None
    if not modules:
        return []

    apply_darwin_kernel_compatibility(settings.kernel_source)
    targets = [module["target"] for module in modules]
    names = [module["name"] for module in modules]
    print(
        "Building kernel runtime modules in load order: "
        + " -> ".join(f"{name}.ko" for name in names)
    )
    # Build in dependency order.  Separate Kbuild targets launched in one
    # parallel make can reach modpost before libcomposite/u_serial have
    # published their exports.
    for target in targets:
        run(
            kernel_make_base(settings)
            + [
                "KCFLAGS=-Wno-error=missing-attributes "
                "-Wno-error=address-of-packed-member",
                target,
                f"-j{settings.jobs}",
            ],
            verbose=settings.verbose,
            env=kernel_host_environment(settings),
        )

    output = settings.output / "modules"
    output.mkdir(parents=True, exist_ok=True)
    artifacts: list[Path] = []
    packaged_modules = [
        module for module in modules if module.get("package", True)
    ]
    for load_order, module in enumerate(packaged_modules, start=1):
        name = module["name"]
        built = settings.kernel_build / module["target"]
        if not built.is_file():
            raise BuildError(f"Kbuild completed without producing {built}")
        debug_output = output / f"{name}.ko.debug"
        final_output = output / f"{name}.ko"
        shutil.copy2(built, debug_output)
        strip_module(debug_output, final_output, {}, settings)
        write_receipt(
            output / f"{name}.json",
            kind="kernel-runtime-module",
            name=name,
            settings=settings,
            source_hash=kernel_source_hash(
                settings.kernel_source,
                module["sources"],
            ),
            artifact=final_output,
            metadata={
                "kernel_target": module["target"],
                "load_order": load_order,
            },
        )
        artifacts.append(final_output)
        print(f"Built {final_output} ({final_output.stat().st_size} bytes)")
    return artifacts


def build_module(
    name: str,
    settings: Settings,
    built: set[str],
    *,
    prepare: bool,
    defconfig: str,
) -> Path:
    modules = manifest()["modules"]
    if name not in modules:
        raise BuildError(f"unknown module {name!r}; use './build.sh list'")
    if name in built:
        return settings.output / "modules" / f"{name}.ko"

    recipe = modules[name]
    supported_firmwares = recipe.get("firmwares")
    if supported_firmwares and settings.firmware not in supported_firmwares:
        raise BuildError(
            f"module {name} supports source families "
            f"{', '.join(supported_firmwares)}, not {settings.firmware}"
        )
    for dependency in recipe.get("dependencies", []):
        build_module(
            dependency,
            settings,
            built,
            prepare=prepare,
            defconfig=defconfig,
        )

    if prepare and not kernel_ready(settings):
        prepare_kernel(settings, defconfig)
    elif not kernel_ready(settings):
        raise BuildError(
            f"kernel build tree is not prepared: {settings.kernel_build}; "
            "run './build.sh prepare' first or omit --no-prepare"
        )

    runtime_modules = recipe.get("runtime_modules", [])
    build_kernel_runtime_modules(runtime_modules, settings)

    source = TARGET_ROOT / recipe["path"]
    work = settings.output / "work" / "modules" / name
    output = settings.output / "modules"
    work.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    (work / "Makefile").touch()

    extra_symbols: list[str] = []
    for dependency in recipe.get("dependencies", []):
        symbol_file = settings.output / "work" / "modules" / dependency / "Module.symvers"
        if not symbol_file.is_file():
            raise BuildError(f"dependency symbols missing: {symbol_file}")
        extra_symbols.append(str(symbol_file))

    print(f"Building module {name} for firmware {settings.firmware}...")
    command = kernel_make_base(settings) + [
        f"M={work}",
        f"src={source}",
    ]
    if extra_symbols:
        command.append(f"KBUILD_EXTRA_SYMBOLS={' '.join(extra_symbols)}")
    command.extend(["modules", f"-j{settings.jobs}"])
    run(
        command,
        verbose=settings.verbose,
        env=kernel_host_environment(settings),
    )

    module = work / f"{name}.ko"
    if not module.is_file():
        raise BuildError(f"Kbuild completed without producing {module}")
    debug_output = output / f"{name}.ko.debug"
    final_output = output / f"{name}.ko"
    shutil.copy2(module, debug_output)
    strip_module(debug_output, final_output, recipe, settings)
    write_receipt(
        output / f"{name}.json",
        kind="module",
        name=name,
        settings=settings,
        source_hash=sha256_tree(source),
        artifact=final_output,
        metadata=(
            {
                "runtime_module_load_order": [
                    *(f"{module['name']}.ko" for module in runtime_modules
                      if module.get("package", True)),
                    f"{name}.ko",
                ]
            }
            if runtime_modules
            else None
        ),
    )
    built.add(name)
    print(f"Built {final_output} ({final_output.stat().st_size} bytes)")
    return final_output


def expand_target_flag(flag: str) -> str:
    if flag.startswith("-I") and len(flag) > 2:
        include = Path(flag[2:])
        if not include.is_absolute():
            return "-I" + str(TARGET_ROOT / include)
    return flag


def tool_compile_command(
    name: str,
    recipe: dict[str, Any],
    destination: Path,
    settings: Settings,
) -> list[str]:
    compiler = list(settings.tool_cc)
    if not executable(compiler[0]):
        raise BuildError(
            f"missing target compiler {compiler[0]!r}; on macOS run "
            "'./build.sh bootstrap' or configure --tool-cc"
        )
    flags = ["-O2", "-Wall", "-Wextra", "-Wno-unused-parameter"]
    if Path(compiler[0]).name == "zig" and compiler[1:] == ["cc"]:
        target = (
            "aarch64-linux-musl"
            if "-static" in recipe.get("cflags", [])
            else f"aarch64-linux-gnu.{settings.glibc_version}"
        )
        flags.extend(["-target", target])
    flags.extend(firmware_cflags(settings.firmware))
    flags.extend(expand_target_flag(flag) for flag in recipe.get("cflags", []))
    if settings.sysroot:
        flags.append(f"--sysroot={settings.sysroot}")
    sources = [str(TARGET_ROOT / source) for source in recipe["sources"]]
    link_flags: list[str] = []
    if recipe.get("link") == "dynamic":
        if Path(compiler[0]).name != "zig":
            link_flags.append(f"-Wl,--dynamic-linker={settings.dynamic_linker}")
        link_flags.append("-Wl,-rpath=/lib")
    link_flags.extend(recipe.get("ldflags", []))
    libraries = [f"-l{library}" for library in recipe.get("libraries", [])]
    return [*compiler, *flags, *sources, "-o", str(destination), *link_flags, *libraries]


def patch_elf_interpreter(path: Path, interpreter: str) -> None:
    """Replace a 64-bit little-endian ELF PT_INTERP without external tools."""
    contents = bytearray(path.read_bytes())
    if contents[:4] != b"\x7fELF" or contents[4:6] != b"\x02\x01":
        raise BuildError(f"cannot patch interpreter in non-AArch64 ELF64 file: {path}")
    program_offset = struct.unpack_from("<Q", contents, 32)[0]
    entry_size = struct.unpack_from("<H", contents, 54)[0]
    entry_count = struct.unpack_from("<H", contents, 56)[0]
    encoded = interpreter.encode("utf-8") + b"\0"
    for index in range(entry_count):
        header = program_offset + index * entry_size
        if struct.unpack_from("<I", contents, header)[0] != 3:  # PT_INTERP
            continue
        offset = struct.unpack_from("<Q", contents, header + 8)[0]
        size = struct.unpack_from("<Q", contents, header + 32)[0]
        if len(encoded) > size:
            raise BuildError(
                f"target interpreter {interpreter!r} is longer than the ELF slot "
                f"({size - 1} bytes)"
            )
        contents[offset : offset + size] = encoded.ljust(size, b"\0")
        path.write_bytes(contents)
        return
    raise BuildError(f"dynamic executable has no PT_INTERP segment: {path}")


def run_logged(
    command: list[str],
    *,
    log: Path,
    cwd: Path | None = None,
    verbose: bool = False,
    env: dict[str, str] | None = None,
) -> None:
    if verbose:
        location = f" (in {cwd})" if cwd else ""
        print(f"+ {display_command(command)}{location}")
    log.parent.mkdir(parents=True, exist_ok=True)
    process_env = os.environ.copy()
    if env:
        process_env.update(env)
    try:
        with log.open("a", encoding="utf-8") as stream:
            result = subprocess.run(
                command,
                cwd=cwd,
                env=process_env,
                stdin=subprocess.DEVNULL,
                stdout=stream,
                stderr=subprocess.STDOUT,
                check=False,
            )
    except FileNotFoundError as exc:
        raise BuildError(f"command not found: {command[0]}") from exc
    if result.returncode:
        lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
        detail = "\n".join(lines[-40:])
        raise BuildError(
            f"command failed with exit status {result.returncode}; "
            f"see {log}\n{detail}"
        )


def download_verified_archive(url: str, expected_sha256: str, destination: Path) -> Path:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(f".{destination.name}.download")
    if temporary.exists():
        temporary.unlink()
    digest = hashlib.sha256()
    total = 0
    request = urllib.request.Request(
        url,
        headers={"User-Agent": "psvr2-native-build/1"},
    )
    print(f"Downloading pinned source from {url}...")
    try:
        with (
            urllib.request.urlopen(request, timeout=60) as response,
            temporary.open("wb") as stream,
        ):
            if not response.geturl().lower().startswith("https://"):
                raise BuildError("external source download redirected away from HTTPS")
            declared = response.headers.get("Content-Length")
            if declared and int(declared) > MAX_EXTERNAL_ARCHIVE_BYTES:
                raise BuildError(
                    f"external archive exceeds {MAX_EXTERNAL_ARCHIVE_BYTES} bytes"
                )
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                total += len(chunk)
                if total > MAX_EXTERNAL_ARCHIVE_BYTES:
                    raise BuildError(
                        f"external archive exceeds {MAX_EXTERNAL_ARCHIVE_BYTES} bytes"
                    )
                digest.update(chunk)
                stream.write(chunk)
    except (OSError, urllib.error.URLError, ValueError) as exc:
        raise BuildError(f"cannot download external source: {exc}") from exc
    finally:
        if temporary.exists() and digest.hexdigest() != expected_sha256:
            temporary.unlink()
    actual = digest.hexdigest()
    if actual != expected_sha256:
        raise BuildError(
            f"external source SHA-256 mismatch: expected {expected_sha256}, got {actual}"
        )
    temporary.replace(destination)
    print(f"Verified external source SHA-256: {actual}")
    return destination


def resolve_external_archive(
    recipe: dict[str, Any],
    *,
    supplied_source: str | None,
    download: bool,
) -> Path:
    expected = recipe["source_sha256"]
    cache = DEFAULT_EXTERNAL_ROOT / "archives" / expected
    if supplied_source:
        source = dragged_path(supplied_source)
        if not source.is_file() or not is_supported_archive(source):
            raise BuildError(f"unsupported or missing external source archive: {source}")
        actual = sha256_file(source)
        if actual != expected:
            raise BuildError(
                f"external source SHA-256 mismatch: expected {expected}, got {actual}"
            )
        cache.parent.mkdir(parents=True, exist_ok=True)
        if source != cache:
            temporary = cache.with_name(f".{cache.name}.import")
            shutil.copy2(source, temporary)
            temporary.replace(cache)
        print(f"Verified external source SHA-256: {actual}")
        return cache

    if cache.is_file():
        actual = sha256_file(cache)
        if actual == expected:
            print(f"Using cached external source {expected}.")
            return cache
        cache.unlink()

    if download:
        return download_verified_archive(recipe["source_url"], expected, cache)

    raise BuildError(
        "BusyBox source is not cached; use '--download' for the pinned official "
        "archive or '--source /path/to/archive' for an explicit local copy"
    )


def find_busybox_source(root: Path, expected_version: str) -> Path:
    candidates = [
        makefile.parent
        for makefile in root.rglob("Makefile")
        if (makefile.parent / "Config.in").is_file()
        and (makefile.parent / "include" / "libbb.h").is_file()
        and (makefile.parent / "applets" / "busybox.mkll").is_file()
    ]
    if len(candidates) != 1:
        raise BuildError(
            f"expected exactly one BusyBox source tree in the archive, found "
            f"{len(candidates)}"
        )
    source = candidates[0]
    assignments: dict[str, str] = {}
    for line in (source / "Makefile").read_text(
        encoding="utf-8", errors="replace"
    ).splitlines():
        key, separator, value = line.partition("=")
        key = key.strip()
        if separator and key in {"VERSION", "PATCHLEVEL", "SUBLEVEL"}:
            assignments[key] = value.strip()
    actual_version = ".".join(
        assignments.get(key, "") for key in ("VERSION", "PATCHLEVEL", "SUBLEVEL")
    )
    if actual_version != expected_version:
        raise BuildError(
            f"BusyBox source version mismatch: expected {expected_version}, "
            f"found {actual_version or 'unknown'}"
        )
    return source


def read_kconfig_values(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            values[line[2 : -len(" is not set")]] = "n"
    return values


def apply_kconfig_fragment(config: Path, fragment: Path) -> dict[str, str]:
    overrides = read_kconfig_values(fragment)
    if not overrides:
        raise BuildError(f"BusyBox configuration fragment is empty: {fragment}")
    lines = config.read_text(encoding="utf-8").splitlines()
    positions: dict[str, int] = {}
    for index, line in enumerate(lines):
        if line.startswith("CONFIG_") and "=" in line:
            positions[line.split("=", 1)[0]] = index
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            positions[line[2 : -len(" is not set")]] = index
    for key, value in overrides.items():
        replacement = f"{key}={value}" if value != "n" else f"# {key} is not set"
        if key in positions:
            lines[positions[key]] = replacement
        else:
            lines.append(replacement)
    config.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return overrides


def busybox_applets(header: Path) -> list[str]:
    applets: list[str] = []
    in_names = False
    for line in header.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith("const char applet_names[]"):
            in_names = True
            continue
        if not in_names:
            continue
        if stripped == ";":
            break
        suffix = ' "\\0"'
        if stripped.startswith('"') and stripped.endswith(suffix):
            applets.append(json.loads(stripped[: -len(suffix)]))
    if not applets:
        raise BuildError(f"could not read generated BusyBox applets from {header}")
    return applets


def busybox_make_tools(settings: Settings) -> dict[str, str]:
    compiler = list(settings.tool_cc)
    if not executable(compiler[0]):
        raise BuildError(
            f"missing target compiler {compiler[0]!r}; on macOS run "
            "'./build.sh bootstrap'"
        )
    if Path(compiler[0]).name == "zig" and compiler[1:] == ["cc"]:
        compiler.extend(["-target", "aarch64-linux-musl"])
        archive = [compiler[0], "ar"]
        ranlib = [compiler[0], "ranlib"]
    else:
        compiler_name = Path(compiler[0]).name
        compiler_directory = Path(compiler[0]).parent
        compiler_prefix = ""
        for suffix in ("gcc", "clang", "cc"):
            if compiler_name.endswith(suffix):
                compiler_prefix = compiler_name[: -len(suffix)]
                break

        def companion(name: str) -> list[str]:
            candidate = compiler_directory / f"{compiler_prefix}{name}"
            found = executable(str(candidate))
            return [found or cross_tool(settings, name)]

        archive = companion("ar")
        ranlib = companion("ranlib")
    return {
        "CC": display_command(compiler),
        "AR": display_command(archive),
        "RANLIB": display_command(ranlib),
        "NM": cross_tool(settings, "nm"),
        "STRIP": cross_tool(settings, "strip"),
    }


def build_busybox_tool(
    name: str,
    recipe: dict[str, Any],
    settings: Settings,
    *,
    no_strip: bool,
    supplied_source: str | None,
    download: bool,
) -> Path:
    archive = resolve_external_archive(
        recipe,
        supplied_source=supplied_source,
        download=download,
    )
    output = settings.output / "tools"
    work = settings.output / "work" / "tools" / name
    output.mkdir(parents=True, exist_ok=True)
    if work.exists():
        shutil.rmtree(work)
    source_root = work / "source"
    build_root = work / "build"
    source_root.mkdir(parents=True)
    build_root.mkdir()
    extract_archive(archive, source_root)
    unpack_nested_source_archive(source_root)
    source = find_busybox_source(source_root, recipe["version"])
    for patch_name in recipe.get("patches", []):
        patch = TARGET_ROOT / patch_name
        if not patch.is_file():
            raise BuildError(f"missing tracked BusyBox patch: {patch}")
        git = executable("git")
        if not git:
            raise BuildError("Git is required to apply tracked BusyBox patches")
        run([git, "apply", "--unsafe-paths", f"--directory={source}",
             "--check", str(patch)])
        run([git, "apply", "--unsafe-paths", f"--directory={source}", str(patch)])
    fragment = TARGET_ROOT / recipe["config"]
    if not fragment.is_file():
        raise BuildError(f"missing BusyBox configuration fragment: {fragment}")

    log = output / f"{name}.build.log"
    log.write_text("", encoding="utf-8")
    environment = {
        "KCONFIG_NOTIMESTAMP": "1",
        "LANG": "C",
        "LC_ALL": "C",
        "SOURCE_DATE_EPOCH": "0",
    }
    if Path(settings.tool_cc[0]).name == "zig":
        zig_cache = work / "zig-cache"
        environment.update(
            {
                "ZIG_GLOBAL_CACHE_DIR": str(zig_cache / "global"),
                "ZIG_LOCAL_CACHE_DIR": str(zig_cache / "local"),
            }
        )

    print(
        f"Configuring BusyBox {recipe['version']} for static AArch64 Linux..."
    )
    make_base = [
        settings.make,
        "-C",
        str(source),
        f"O={build_root}",
    ]
    run_logged(
        [*make_base, "allyesconfig"],
        log=log,
        verbose=settings.verbose,
        env=environment,
    )
    overrides = apply_kconfig_fragment(build_root / ".config", fragment)
    run_logged(
        [settings.make, "-C", str(build_root), "oldconfig"],
        log=log,
        verbose=settings.verbose,
        env=environment,
    )
    resolved = read_kconfig_values(build_root / ".config")
    unresolved = {
        key: (expected, resolved.get(key))
        for key, expected in overrides.items()
        if resolved.get(key) != expected
    }
    if unresolved:
        details = ", ".join(
            f"{key}={actual!r} (expected {expected!r})"
            for key, (expected, actual) in sorted(unresolved.items())
        )
        raise BuildError(f"BusyBox Kconfig rejected target overrides: {details}")

    make_tools = busybox_make_tools(settings)
    make_arguments = [f"{key}={value}" for key, value in make_tools.items()]
    print(f"Building BusyBox with {settings.jobs} jobs...")
    run_logged(
        [
            settings.make,
            "-C",
            str(build_root),
            f"-j{settings.jobs}",
            *make_arguments,
        ],
        log=log,
        verbose=settings.verbose,
        env=environment,
    )

    built = build_root / ("busybox_unstripped" if no_strip else "busybox")
    if not built.is_file():
        raise BuildError(f"BusyBox build completed without producing {built}")
    final_output = output / name
    shutil.copy2(built, final_output)
    final_output.chmod(0o755)
    resolved_config = output / f"{name}.config"
    shutil.copy2(build_root / ".config", resolved_config)
    applets = busybox_applets(build_root / "include" / "applet_tables.h")
    applet_list = output / f"{name}.applets"
    applet_list.write_text("\n".join(applets) + "\n", encoding="utf-8")

    source_digest = hashlib.sha256()
    source_digest.update(bytes.fromhex(recipe["source_sha256"]))
    source_digest.update(bytes.fromhex(sha256_file(fragment)))
    for patch_name in recipe.get("patches", []):
        source_digest.update(bytes.fromhex(sha256_file(TARGET_ROOT / patch_name)))
    write_receipt(
        output / f"{name}.json",
        kind="external-target-tool",
        name=name,
        settings=settings,
        source_hash=source_digest.hexdigest(),
        artifact=final_output,
        metadata={
            "upstream_version": recipe["version"],
            "upstream_url": recipe["source_url"],
            "upstream_archive_sha256": recipe["source_sha256"],
            "configuration": resolved_config.name,
            "configuration_sha256": sha256_file(resolved_config),
            "applets": applet_list.name,
            "applet_count": len(applets),
            "static": True,
        },
        include_kernel_source=False,
    )
    print(
        f"Built {final_output} ({final_output.stat().st_size} bytes, "
        f"{len(applets)} applets)"
    )
    return final_output


def tool_source_hash(recipe: dict[str, Any]) -> str:
    """Receipt identity includes authored headers as well as compilation units."""
    paths = {TARGET_ROOT / name for name in recipe["sources"]}
    for pattern in recipe.get("headers", []):
        matched = {path for path in TARGET_ROOT.glob(pattern) if path.is_file()}
        if not matched:
            raise BuildError(f"tool header pattern matched no files: {pattern}")
        paths.update(matched)
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(path.relative_to(TARGET_ROOT).as_posix().encode("utf-8") + b"\0")
        digest.update(bytes.fromhex(sha256_file(path)))
    return digest.hexdigest()


def build_tool(
    name: str,
    settings: Settings,
    no_strip: bool,
    *,
    supplied_source: str | None = None,
    download: bool = False,
) -> Path:
    tools = manifest()["tools"]
    if name not in tools:
        raise BuildError(f"unknown target tool {name!r}; use './build.sh list'")
    recipe = tools[name]
    if settings.firmware not in recipe.get("firmwares", firmware_profiles()):
        raise BuildError(f"tool {name} is not supported on source family {settings.firmware}")
    if recipe.get("build_system") == "busybox":
        return build_busybox_tool(
            name,
            recipe,
            settings,
            no_strip=no_strip,
            supplied_source=supplied_source,
            download=download,
        )
    if supplied_source or download:
        raise BuildError(
            f"--source/--download apply only to external-source target tools, "
            f"not {name}"
        )
    output = settings.output / "tools"
    work = settings.output / "work" / "tools"
    output.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)

    output_name = recipe.get("output", name)
    debug_output = work / (output_name + ".debug")
    final_output = output / output_name
    print(f"Building target tool {name} for AArch64 Linux...")
    tool_env: dict[str, str] | None = None
    if Path(settings.tool_cc[0]).name == "zig":
        zig_cache = settings.output / "work" / "zig-cache"
        tool_env = {
            "ZIG_GLOBAL_CACHE_DIR": str(zig_cache / "global"),
            "ZIG_LOCAL_CACHE_DIR": str(zig_cache / "local"),
        }
    run(
        tool_compile_command(name, recipe, debug_output, settings),
        verbose=settings.verbose,
        env=tool_env,
    )
    if recipe.get("link") == "dynamic" and Path(settings.tool_cc[0]).name == "zig":
        patch_elf_interpreter(debug_output, settings.dynamic_linker)

    if no_strip:
        shutil.copy2(debug_output, final_output)
    else:
        strip = cross_tool(settings, "strip")
        run(
            [strip, "-s", "--strip-unneeded", "-o", str(final_output), str(debug_output)],
            verbose=settings.verbose,
        )

    package = recipe.get("package")
    artifact = final_output
    if package:
        package_dir = output / package["directory"]
        package_dir.mkdir(parents=True, exist_ok=True)
        packaged_binary = package_dir / output_name
        shutil.copy2(final_output, packaged_binary)
        assets_name = package.get("assets")
        if assets_name:
            assets = TARGET_ROOT / assets_name
            if assets.is_dir():
                packaged_assets = package_dir / assets.name
                if packaged_assets.exists():
                    shutil.rmtree(packaged_assets)
                shutil.copytree(assets, packaged_assets)
        artifact = packaged_binary

    write_receipt(
        output / f"{name}.json",
        kind="target-tool",
        name=name,
        settings=settings,
        source_hash=tool_source_hash(recipe),
        artifact=artifact,
    )
    print(f"Built {artifact} ({artifact.stat().st_size} bytes)")
    return artifact


def check(label: str, ok: bool, detail: str) -> bool:
    marker = "ok" if ok else "missing"
    print(f"[{marker:7}] {label}: {detail}")
    return ok


@requires_source_lease
def cmd_doctor(args: argparse.Namespace) -> int:
    settings = resolve_settings(args, require_kernel=False)
    failures = 0
    print(f"Host: {platform.system()} {platform.machine()} (Python {platform.python_version()})")
    if platform.system() == "Darwin":
        if not check(
            "Apple Command Line Tools",
            command_line_tools_ready(),
            "xcrun clang",
        ):
            failures += 1
        elf_header = Path(os.environ.get("PSVR2_HOST_INCLUDE", str(REPO_ROOT / ".local/inputs/host-include"))) / "elf.h"
        if not check("external host ELF header", elf_header.is_file(),
                     "run ./build.sh sources glibc" if not elf_header.is_file() else str(elf_header)):
            failures += 1
    host_build = discover_host_build_helpers(find_homebrew() if platform.system() == "Darwin" else None)
    for name, label in (("cmake", "CMake >=3.20"), ("ctest", "CTest >=3.20"),
                        ("pkg_config", "pkg-config"), ("libusb", "libusb development files")):
        if not check(label, bool(host_build[name]), host_build[name] or "not found"):
            failures += 1
    git = executable("git")
    if not check("Git snapshot engine", git is not None, git or "git"):
        failures += 1
    make_ok = executable(settings.make) is not None
    if platform.system() == "Darwin":
        make_ok = make_ok and is_gnu_make(settings.make)
    if not check("GNU make", make_ok, settings.make):
        failures += 1
    for helper in ("bc", "perl"):
        path = executable_with_host_paths(helper, settings)
        if not check(f"kernel host helper {helper}", path is not None, path or helper):
            failures += 1
    for tool in ("gcc", "strip", "objcopy"):
        candidate = settings.cross_prefix + tool
        if not check(f"cross {tool}", executable(candidate) is not None, candidate):
            failures += 1
    target_compiler = " ".join(settings.tool_cc)
    if not check(
        "target compiler",
        executable(settings.tool_cc[0]) is not None,
        f"{target_compiler} (glibc {settings.glibc_version})",
    ):
        failures += 1
    if settings.kernel_source:
        source_ok = all(
            (
                (settings.kernel_source / "Makefile").is_file(),
                (settings.kernel_source / "Kconfig").is_file(),
                (settings.kernel_source / "arch" / "arm64").is_dir(),
            )
        )
        if not check("kernel source", source_ok, str(settings.kernel_source)):
            failures += 1
    else:
        check("kernel source", False, "not configured")
        failures += 1
    ready = kernel_ready(settings)
    if not check("kernel build", ready, str(settings.kernel_build)):
        failures += 1
    if settings.sysroot:
        if not check("target sysroot", settings.sysroot.is_dir(), str(settings.sysroot)):
            failures += 1
    else:
        print("[note   ] target sysroot: compiler default (set one for reproducible libc tools)")
    print(f"Configuration: {config_path(args)}")
    return 1 if failures else 0


@requires_source_lease
def cmd_configure(args: argparse.Namespace) -> int:
    data = load_config(args)
    firmware = resolve_firmware_profile(args.firmware)
    firmware_data = data["firmwares"].setdefault(firmware, {})
    if args.kernel_source:
        source = Path(args.kernel_source).expanduser().resolve()
        validate_psvr2_kernel_source(source)
        tree_sha256 = canonical_source_tree_sha256(source)
        tree_owner = catalog_tree_owner(tree_sha256)
        if tree_owner and tree_owner != firmware:
            raise BuildError(
                f"manual source tree belongs to source family {tree_owner}, "
                f"not {firmware}"
            )
        if tree_owner:
            source_verification = "official-tree"
        elif getattr(args, "allow_unverified_source", False):
            source_verification = "unverified"
        else:
            raise BuildError(
                "manual source tree is not in the official identity catalog; "
                "use --allow-unverified-source only for an intentionally "
                "modified PSVR2 source"
            )
        firmware_data["kernel_source"] = str(source)
        for key in (
            "source_snapshot",
            "source_archive_sha256",
            "source_tree_git_sha1",
        ):
            firmware_data.pop(key, None)
        firmware_data["source_tree_sha256"] = tree_sha256
        firmware_data["source_verification"] = source_verification
    if args.kernel_build:
        firmware_data["kernel_build"] = str(Path(args.kernel_build).expanduser().resolve())
    elif "kernel_build" not in firmware_data:
        firmware_data["kernel_build"] = str(DEFAULT_OUTPUT / firmware / "kernel")
    for key in (
        "cross_prefix",
        "tool_cc",
        "glibc_version",
        "make",
        "jobs",
        "dynamic_linker",
    ):
        value = getattr(args, key, None)
        if value is not None:
            data[key] = value
    if args.sysroot:
        data["sysroot"] = str(Path(args.sysroot).expanduser().resolve())
    data["default_firmware"] = firmware
    write_json(config_path(args), data)
    print(f"Saved native build configuration to {config_path(args)}")
    return 0


@requires_source_lease
def cmd_config_show(args: argparse.Namespace) -> int:
    print(json.dumps(load_config(args), indent=2, sort_keys=True))
    return 0


def command_line_tools_ready() -> bool:
    xcrun = executable("xcrun")
    if not xcrun:
        return False
    try:
        subprocess.run(
            [xcrun, "--find", "clang"],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except (OSError, subprocess.CalledProcessError):
        return False
    return True


def wants_install(args: argparse.Namespace, label: str, command: list[str]) -> bool:
    print(f"[missing] {label}: {display_command(command)}")
    if args.check:
        return False
    if args.yes:
        return True
    return confirm(f"Install {label}?")


@requires_source_lease
def cmd_bootstrap(args: argparse.Namespace) -> int:
    if platform.system() != "Darwin":
        print(
            "Native Linux prerequisites: Git, GNU make, GCC/binutils for "
            "aarch64-linux-gnu, Python 3, bc, bison, flex, and OpenSSL headers."
        )
        return 0

    print(f"Checking native tools for macOS {platform.machine()}...")
    if not command_line_tools_ready():
        command = ["/usr/bin/xcode-select", "--install"]
        if wants_install(args, "Apple Command Line Tools", command):
            run(command, verbose=True)
            print(
                "Finish the Apple installer, then rerun this command so the "
                "remaining tools can be checked."
            )
        return 1
    print("[ok     ] Apple Command Line Tools")

    data = load_config(args)
    brew = find_homebrew()
    gmake = discover_gnu_make(data, brew)
    cross_prefix = discover_cross_prefix(data, brew)
    target_compiler = discover_target_compiler(data, brew)
    host_helpers = discover_kernel_host_helpers(brew)
    host_build = discover_host_build_helpers(brew)

    if gmake:
        print(f"[ok     ] GNU make: {gmake}")
    if cross_prefix:
        print(f"[ok     ] Arm GNU AArch64 toolchain: {cross_prefix}gcc")
    if target_compiler:
        print(f"[ok     ] AArch64 Linux compiler: {display_command(target_compiler)}")
    for name, path in host_helpers.items():
        if path:
            print(f"[ok     ] kernel host helper {name}: {path}")

    for name, label in (("cmake", "CMake >=3.20"), ("ctest", "CTest >=3.20"),
                        ("pkg_config", "pkg-config"), ("libusb", "libusb development files")):
        if host_build[name]:
            print(f"[ok     ] {label}: {host_build[name]}")

    missing: list[str] = []
    if not gmake:
        missing.append("GNU make")
    if not cross_prefix:
        missing.append("Arm GNU AArch64 toolchain")
    if not target_compiler:
        missing.append("AArch64 Linux compiler")
    if not host_helpers["bc"]:
        missing.append("bc calculator")
    if not host_helpers["perl"]:
        missing.append("Perl")

    if not host_build["cmake"] or not host_build["ctest"]:
        missing.append("CMake and CTest")
    if not host_build["pkg_config"]:
        missing.append("pkg-config")
    if not host_build["libusb"]:
        missing.append("libusb development files")

    if not missing:
        if brew:
            print(f"[ok     ] Homebrew (available, not required): {brew}")
    elif args.check:
        for label in missing:
            print(f"[missing] {label}")
        if brew:
            print(f"[ok     ] Homebrew installer: {brew}")
        else:
            print("[note   ] Homebrew is not installed; it can install missing tools.")
        return 1
    elif not brew:
        install_command = [
            "/bin/bash",
            "-c",
            '/bin/bash -c "$(/usr/bin/curl -fsSL '
            'https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"',
        ]
        if wants_install(args, "Homebrew", install_command):
            run(install_command, verbose=True)
            brew = find_homebrew()
            if not brew:
                raise BuildError(
                    "Homebrew installation completed but brew was not found in "
                    "/opt/homebrew/bin or /usr/local/bin"
                )
        else:
            print(
                "Homebrew is required to install the remaining architecture-matched "
                "native dependencies."
            )
            return 1
    else:
        if missing:
            print(f"[ok     ] Homebrew installer: {brew}")

    if missing:
        assert brew is not None
        install_specs = (
            ("CMake and CTest", [brew, "install", "cmake"],
             lambda: all(discover_host_build_helpers(brew)[name] for name in ("cmake", "ctest"))),
            ("pkg-config", [brew, "install", "pkgconf"],
             lambda: discover_host_build_helpers(brew)["pkg_config"]),
            ("libusb development files", [brew, "install", "libusb"],
             lambda: discover_host_build_helpers(brew)["libusb"]),
            (
                "GNU make",
                [brew, "install", "make"],
                lambda: discover_gnu_make(data, brew),
            ),
            (
                "Arm GNU AArch64 toolchain",
                [brew, "install", "--cask", "gcc-aarch64-embedded"],
                lambda: discover_cross_prefix(data, brew),
            ),
            (
                "AArch64 Linux compiler",
                [brew, "install", "zig"],
                lambda: discover_target_compiler(data, brew),
            ),
            (
                "bc calculator",
                [brew, "install", "bc"],
                lambda: discover_kernel_host_helpers(brew)["bc"],
            ),
            (
                "Perl",
                [brew, "install", "perl"],
                lambda: discover_kernel_host_helpers(brew)["perl"],
            ),
        )
        skipped: list[str] = []
        for label, command, finder in install_specs:
            if label not in missing:
                continue
            if not wants_install(args, label, command):
                skipped.append(label)
                continue
            print(f"Installing {label}...")
            run(command, verbose=True)
            if not finder():
                raise BuildError(f"{label} installation completed, but it was not found")
        if skipped:
            print("Setup remains incomplete; skipped: " + ", ".join(skipped))
            return 1

        gmake = discover_gnu_make(data, brew)
        cross_prefix = discover_cross_prefix(data, brew)
        target_compiler = discover_target_compiler(data, brew)
        host_helpers = discover_kernel_host_helpers(brew)
        host_build = discover_host_build_helpers(brew)
        if not all(
            (
                gmake,
                cross_prefix,
                target_compiler,
                host_helpers["bc"],
                host_helpers["perl"],
                *host_build.values(),
            )
        ):
            print("One or more required tools remain missing.")
            return 1

    assert gmake is not None
    assert cross_prefix is not None
    assert target_compiler is not None
    if args.check:
        return 0
    data["cross_prefix"] = cross_prefix
    data["tool_cc"] = display_command(target_compiler)
    data["glibc_version"] = "2.28"
    data["make"] = gmake
    host_executables = [
        gmake,
        cross_prefix + "gcc",
        target_compiler[0],
        host_helpers["bc"],
        host_helpers["perl"],
        host_build["cmake"], host_build["ctest"], host_build["pkg_config"],
    ]
    data["host_paths"] = list(
        dict.fromkeys(
            str(Path(item).parent)
            for item in host_executables
            if item and os.sep in item
        )
    )
    data["host_build_tools"] = host_build
    write_json(config_path(args), data)
    print(f"Native macOS toolchains are ready ({platform.machine()}).")
    print(f"Saved toolchain configuration to {config_path(args)}")
    return 0


@requires_source_lease
def cmd_prepare(args: argparse.Namespace) -> int:
    settings = resolve_settings(args, require_kernel=True)
    prepare_kernel(settings, args.defconfig, force_config=args.force_config)
    return 0


@requires_source_lease
def cmd_module(args: argparse.Namespace) -> int:
    if getattr(args, "firmware", None) == "all":
        matrix_args = argparse.Namespace(
            **vars(args),
            names=[args.name],
            group=None,
            firmwares=["all"],
            allow_missing=False,
        )
        return cmd_matrix_modules(matrix_args)
    settings = resolve_settings(args, require_kernel=True)
    build_module(
        args.name,
        settings,
        set(),
        prepare=not args.no_prepare,
        defconfig=args.defconfig,
    )
    return 0


@requires_source_lease
def cmd_modules(args: argparse.Namespace) -> int:
    if getattr(args, "firmware", None) == "all":
        matrix_args = argparse.Namespace(
            **vars(args),
            firmwares=["all"],
            allow_missing=False,
        )
        return cmd_matrix_modules(matrix_args)
    settings = resolve_settings(args, require_kernel=True)
    recipes = manifest()["modules"]
    names = list(args.names)
    if args.group:
        names.extend(
            name for name, recipe in recipes.items() if recipe["category"] == args.group
        )
    if not names:
        names = list(recipes)
    names = list(dict.fromkeys(names))
    explicit_names = bool(args.names)
    supported_names = []
    for name in names:
        supported = recipes[name].get("firmwares")
        if supported and settings.firmware not in supported:
            message = (
                f"Skipping module {name}: supports source families "
                f"{', '.join(supported)}, not {settings.firmware}."
            )
            if explicit_names:
                raise BuildError(message)
            print(message)
            continue
        supported_names.append(name)
    names = supported_names
    if not args.no_prepare:
        prepare_kernel(settings, args.defconfig)
    built: set[str] = set()
    for name in names:
        build_module(
            name,
            settings,
            built,
            prepare=False,
            defconfig=args.defconfig,
        )
    return 0


@requires_source_lease
def cmd_tool(args: argparse.Namespace) -> int:
    settings = resolve_settings(args, require_kernel=False)
    build_tool(
        args.name,
        settings,
        args.no_strip,
        supplied_source=args.source,
        download=args.download,
    )
    return 0


def wii_menu_inputs(project: Path, runtime_root: Path) -> tuple[Path, Path]:
    """Validate the maintained port and local AArch64 graphics SDK inputs."""
    for relative in (
        "CMakeLists.txt", "src/CMakeLists.txt", "cmake/WiiMenuBackend.cmake",
        "src/platform/psvr2/gl_api.h", "src/platform/psvr2/vr_layout.c",
    ):
        if not (project / relative).is_file():
            raise BuildError(f"PSVR2 Wii Menu source is missing: {relative}")
    libraries = tuple(runtime_root / "lib" / name for name in (
        "libEGL.so.1", "libGLESv2.so.2",
    ))
    for library in libraries:
        try:
            with library.open("rb") as stream:
                header = stream.read(20)
        except OSError as exc:
            raise BuildError(f"missing local PSVR2 graphics library: {library}") from exc
        if (len(header) != 20 or header[:6] != b"\x7fELF\x02\x01" or
                struct.unpack_from("<H", header, 18)[0] != 183):
            raise BuildError(f"graphics library is not AArch64 ELF: {library}")
    return libraries


def wii_menu_compiler(settings: Settings) -> list[str]:
    compiler = list(settings.tool_cc)
    if not compiler or not executable_with_host_paths(compiler[0], settings):
        raise BuildError("PSVR2 userspace compiler is unavailable; run ./build.sh doctor")
    if Path(compiler[0]).name == "zig" and compiler[1:] == ["cc"]:
        compiler += ["-target", f"aarch64-linux-gnu.{settings.glibc_version}"]
    if settings.sysroot:
        compiler.append(f"--sysroot={settings.sysroot}")
    return compiler


@requires_source_lease
def cmd_wii_menu(args: argparse.Namespace) -> int:
    settings = resolve_settings(args, require_kernel=False)
    if settings.firmware != "06.00":
        raise BuildError("The Wii Menu display adapter is verified against source family 06.00 only")
    project = Path(args.project).expanduser().resolve()
    runtime_root = (Path(args.runtime_root).expanduser().resolve() if args.runtime_root
                    else REPO_ROOT / ".local/inputs/psvr2-runtime")
    egl, gles = wii_menu_inputs(project, runtime_root)
    cmake = executable_with_host_paths("cmake", settings)
    if not cmake:
        raise BuildError("CMake 3.20 or newer is required to build the maintained Wii Menu project")
    work = settings.output / "work" / "wii-menu"
    work.mkdir(parents=True, exist_ok=True)
    wrapper = work / "psvr2-cc"
    compiler = wii_menu_compiler(settings)
    wrapper_text = "#!/bin/sh\nexec " + shlex.join(compiler) + ' "$@"\n'
    if not wrapper.exists() or wrapper.read_text() != wrapper_text:
        # A compiler/profile change must rerun CMake's ABI checks.
        for cached in (work / "cmake" / "CMakeCache.txt",):
            if cached.exists():
                cached.unlink()
        wrapper.write_text(wrapper_text)
        wrapper.chmod(0o755)
    build = work / "cmake"
    environment = host_path_environment(settings)
    environment.update({
        "ZIG_GLOBAL_CACHE_DIR": str(settings.output / "work/zig-cache/global"),
        "ZIG_LOCAL_CACHE_DIR": str(settings.output / "work/zig-cache/local"),
    })
    command = [
        cmake, "-S", str(project), "-B", str(build),
        "-DCMAKE_SYSTEM_NAME=Linux", "-DCMAKE_SYSTEM_PROCESSOR=aarch64",
        f"-DCMAKE_C_COMPILER={wrapper}", "-DCMAKE_BUILD_TYPE=Release",
        "-DWM_BACKEND=psvr2", "-DWM_BUILD_TOOLS=OFF", "-DBUILD_TESTING=OFF",
        "-DWM_PSVR2_BUILD_ENTRY=ON", "-DWM_PSVR2_SOURCE_FAMILY=0600",
        f"-DWM_PSVR2_TARGET_ROOT={TARGET_ROOT}",
        f"-DWM_PSVR2_EGL_LIBRARY={egl}", f"-DWM_PSVR2_GLES_LIBRARY={gles}",
    ]
    if Path(settings.tool_cc[0]).name != "zig":
        command.append(f"-DCMAKE_EXE_LINKER_FLAGS=-Wl,--dynamic-linker={settings.dynamic_linker}")
    run(command, verbose=settings.verbose, env=environment)
    run([cmake, "--build", str(build), "--target", "wii-menu", "--parallel",
         str(settings.jobs)], verbose=settings.verbose, env=environment)
    debug_binary = build / "wii-menu"
    if Path(settings.tool_cc[0]).name == "zig":
        patch_elf_interpreter(debug_binary, settings.dynamic_linker)
    output = settings.output / "tools" / "wii-menu-folder"
    output.mkdir(parents=True, exist_ok=True)
    binary = output.parent / "wii-menu"
    if args.no_strip:
        shutil.copy2(debug_binary, binary)
    else:
        run([cross_tool(settings, "strip"), "-s", "--strip-unneeded", "-o",
             str(binary), str(debug_binary)], verbose=settings.verbose, env=environment)
    shutil.copy2(binary, output / "wii-menu")
    digest = hashlib.sha256()
    for directory in ("src", "include", "cmake"):
        for path in sorted((project / directory).rglob("*")):
            if path.is_file() and path.suffix in (".c", ".h", ".cmake"):
                digest.update(path.relative_to(project).as_posix().encode() + b"\0")
                digest.update(bytes.fromhex(sha256_file(path)))
    digest.update(bytes.fromhex(sha256_file(project / "CMakeLists.txt")))
    reference = TARGET_ROOT / "tools/open_vrhmd"
    for path in sorted(reference.rglob("*")):
        if path.is_file() and path.suffix in (".c", ".h") and "vendor" not in path.parts:
            digest.update(path.relative_to(TARGET_ROOT).as_posix().encode() + b"\0")
            digest.update(bytes.fromhex(sha256_file(path)))
    write_receipt(
        settings.output / "tools/wii-menu.json", kind="target-tool", name="wii-menu",
        settings=settings, source_hash=digest.hexdigest(), artifact=binary,
        metadata={"package_directory": output.name,
                  "adjacent_assets": "wii-menu.wm",
                  "graphics_sdk_sha256": {egl.name: sha256_file(egl),
                                          gles.name: sha256_file(gles)}},
    )
    print(f"Built {output / 'wii-menu'} ({binary.stat().st_size} bytes)")
    print("Prepare wii-menu.wm on the host, then upload wii-menu first and wii-menu.wm second.")
    return 0


@requires_source_lease
def cmd_tools(args: argparse.Namespace) -> int:
    settings = resolve_settings(args, require_kernel=False)
    recipes = manifest()["tools"]
    names = list(args.names)
    if args.group:
        names.extend(
            name for name, recipe in recipes.items() if recipe["category"] == args.group
        )
    if not names:
        names = [
            name
            for name, recipe in recipes.items()
            if args.download or not recipe.get("external")
        ]
        if not args.download:
            external = [
                name for name, recipe in recipes.items() if recipe.get("external")
            ]
            if external:
                print(
                    "Skipping external-source tools by default: "
                    + ", ".join(external)
                    + ". Build one explicitly or pass --download."
                )
    for name in dict.fromkeys(names):
        recipe = recipes[name]
        build_tool(
            name,
            settings,
            args.no_strip,
            download=args.download and bool(recipe.get("external")),
        )
    return 0


def cmd_list(_: argparse.Namespace) -> int:
    data = manifest()
    print("Kernel modules:")
    for name, recipe in data["modules"].items():
        dependencies = recipe.get("dependencies", [])
        details = []
        if dependencies:
            details.append(f"needs {', '.join(dependencies)}")
        if recipe.get("firmwares"):
            details.append("firmware " + ", ".join(recipe["firmwares"]))
        suffix = f" ({'; '.join(details)})" if details else ""
        print(f"  {name:20} {recipe['category']}{suffix}")
    print("\nAArch64 target tools:")
    for name, recipe in data["tools"].items():
        source = " (pinned external source)" if recipe.get("external") else ""
        print(f"  {name:20} {recipe['category']}{source}")
    print("\nFirmware profiles: " + ", ".join(data["firmwares"]))
    return 0


def safe_member_path(root: Path, name: str) -> Path:
    candidate = (root / name).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError as exc:
        raise BuildError(f"archive entry escapes extraction root: {name}") from exc
    return candidate


def validate_archive_limits(
    *,
    archive: Path,
    members: int,
    expanded_bytes: int,
) -> None:
    if members > MAX_ARCHIVE_MEMBERS:
        raise BuildError(
            f"archive has too many entries ({members} > {MAX_ARCHIVE_MEMBERS}): "
            f"{archive}"
        )
    if expanded_bytes > MAX_ARCHIVE_EXPANDED_BYTES:
        raise BuildError(
            "archive expands beyond the safety limit "
            f"({expanded_bytes} > {MAX_ARCHIVE_EXPANDED_BYTES} bytes): {archive}"
        )


def extract_archive(archive: Path, destination: Path) -> None:
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as bundle:
            entries = bundle.infolist()
            validate_archive_limits(
                archive=archive,
                members=len(entries),
                expanded_bytes=sum(entry.file_size for entry in entries),
            )
            for entry in entries:
                safe_member_path(destination, entry.filename)
                if entry.flag_bits & 0x1:
                    raise BuildError(f"encrypted ZIP entries are unsupported: {entry.filename}")
                entry_mode = (entry.external_attr >> 16) & 0o170000
                if stat.S_ISLNK(entry_mode):
                    raise BuildError(f"ZIP symlinks are unsupported: {entry.filename}")
            bundle.extractall(destination)
        return
    if tarfile.is_tarfile(archive):
        with tarfile.open(archive) as bundle:
            members = bundle.getmembers()
            validate_archive_limits(
                archive=archive,
                members=len(members),
                expanded_bytes=sum(entry.size for entry in members if entry.isfile()),
            )
            for entry in members:
                member_path = safe_member_path(destination, entry.name)
                if entry.issym():
                    link_target = (member_path.parent / entry.linkname).resolve()
                    try:
                        link_target.relative_to(destination.resolve())
                    except ValueError as exc:
                        raise BuildError(
                            f"archive symlink escapes extraction root: {entry.name}"
                        ) from exc
                elif entry.islnk():
                    safe_member_path(destination, entry.linkname)
                elif entry.isdev():
                    raise BuildError(f"unsupported archive entry: {entry.name}")
            bundle.extractall(destination, members=members)
        return
    raise BuildError(f"unsupported archive format: {archive}")


def find_kernel_source(root: Path) -> Path:
    candidates: list[Path] = []
    for makefile in root.rglob("Makefile"):
        directory = makefile.parent
        if (directory / "Kconfig").is_file() and (directory / "arch" / "arm64").is_dir():
            candidates.append(directory)
    if not candidates:
        raise BuildError(f"no ARM64 Linux kernel source tree found under {root}")
    shallowest_depth = min(len(path.relative_to(root).parts) for path in candidates)
    shallowest = [
        path
        for path in candidates
        if len(path.relative_to(root).parts) == shallowest_depth
    ]
    if len(shallowest) != 1:
        paths = ", ".join(str(path.relative_to(root)) for path in sorted(shallowest))
        raise BuildError(f"archive contains multiple candidate kernel trees: {paths}")
    return shallowest[0]


def is_supported_archive(path: Path) -> bool:
    return path.is_file() and (zipfile.is_zipfile(path) or tarfile.is_tarfile(path))


def find_kernel_source_with_nested_archive(root: Path) -> Path:
    try:
        return find_kernel_source(root)
    except BuildError as original_error:
        nested = [
            item
            for item in root.iterdir()
            if not item.name.startswith(("._", ".DS_Store"))
            and is_supported_archive(item)
        ]
        if len(nested) != 1:
            raise original_error
        archive = nested[0]
        print(f"Extracting nested archive {archive.name}...")
        extract_archive(archive, root)
        archive.unlink()
        return find_kernel_source(root)


def validate_psvr2_kernel_source(path: Path) -> None:
    validate_kernel_source(path)
    expected = (
        path / "arch" / "arm64" / "configs" / "sie_release_mt3612_asic_a0_defconfig",
        path / "drivers" / "misc" / "mediatek" / "dprx",
        path / "include" / "soc" / "mediatek" / "mtk_dprx_info.h",
    )
    missing = [str(item) for item in expected if not item.exists()]
    if missing:
        raise BuildError(
            f"{path} is an ARM64 kernel tree but not a recognizable PSVR2 SDK; "
            "missing: " + ", ".join(missing)
        )


def verify_source_identity(
    *,
    firmware: str,
    archive_sha256: str,
    source_tree_git_sha1: str,
    source_tree_sha256: str,
    allow_unverified_source: bool,
) -> str:
    archive_owner = catalog_archive_owner(archive_sha256)
    tree_owner = catalog_tree_owner(source_tree_sha256)
    if archive_owner and archive_owner != firmware:
        raise BuildError(
            f"archive hash belongs to source family {archive_owner}, not {firmware}"
        )
    if tree_owner and tree_owner != firmware:
        raise BuildError(
            f"extracted source tree belongs to source family {tree_owner}, not {firmware}"
        )
    profile = source_catalog()["firmwares"][firmware]
    archive_known = archive_sha256 in profile["archive_sha256"]
    tree_known = (
        source_tree_git_sha1 == profile["source_tree_git_sha1"]
        and source_tree_sha256 == profile["source_tree_sha256"]
    )
    if archive_known and not tree_known:
        raise BuildError(
            f"archive hash matches {firmware}, but its extracted source-tree "
            "identity does not"
        )
    if archive_known:
        return "official-archive"
    if tree_known:
        return "official-tree"
    if not allow_unverified_source:
        raise BuildError(
            f"archive and source-tree hashes are not recognized for {firmware}; "
            "use --allow-unverified-source only for an intentionally modified "
            "PSVR2 kernel source"
        )
    return "unverified"


def preflight_source_archive(
    *,
    firmware: str,
    archive: Path,
    allow_unverified_source: bool,
) -> SourceCandidate:
    if not archive.is_file():
        raise BuildError(f"archive does not exist: {archive}")
    if not is_supported_archive(archive):
        raise BuildError(f"unsupported archive format: {archive}")
    archive_sha256 = sha256_file(archive)
    print(f"Source archive SHA-256: {archive_sha256}")
    archive_owner = catalog_archive_owner(archive_sha256)
    if archive_owner and archive_owner != firmware:
        raise BuildError(
            f"archive hash belongs to source family {archive_owner}, not {firmware}"
        )

    DEFAULT_SDK_ROOT.mkdir(parents=True, exist_ok=True)
    staging = Path(
        tempfile.mkdtemp(prefix=f".{firmware}-preflight-", dir=DEFAULT_SDK_ROOT)
    )
    try:
        print(f"Preflighting {archive} in a temporary import directory...")
        extract_archive(archive, staging)
        staged_source = find_kernel_source_with_nested_archive(staging)
        validate_psvr2_kernel_source(staged_source)
        tree_sha256 = canonical_source_tree_sha256(staged_source)
        tree = store_source_tree(staged_source)
    finally:
        if staging.exists():
            shutil.rmtree(staging)

    verification = verify_source_identity(
        firmware=firmware,
        archive_sha256=archive_sha256,
        source_tree_git_sha1=tree,
        source_tree_sha256=tree_sha256,
        allow_unverified_source=allow_unverified_source,
    )
    print(
        f"Verified source family {firmware}: {verification}, "
        f"tree={tree[:12]}"
    )
    return SourceCandidate(
        firmware=firmware,
        archive=archive,
        archive_sha256=archive_sha256,
        source_tree_git_sha1=tree,
        source_tree_sha256=tree_sha256,
        verification=verification,
    )


def snapshot_tree(snapshot: str) -> str:
    store = sdk_store_path()
    git = executable("git")
    if not git or not (store / "HEAD").is_file():
        raise BuildError("Git snapshot store is unavailable")
    return capture(
        [git, "--git-dir", str(store), "rev-parse", f"{snapshot}^{{tree}}"]
    )


def restore_config_file(
    path: Path,
    *,
    existed: bool,
    original_data: dict[str, Any],
) -> None:
    if existed:
        write_json(path, original_data)
    elif path.exists():
        path.unlink()


def apply_source_candidates(
    args: argparse.Namespace,
    candidates: list[SourceCandidate],
    *,
    compact: bool,
) -> dict[str, str]:
    if not candidates:
        return {}
    path = config_path(args)
    config_existed = path.exists()
    original_data = load_config(args)
    data = copy.deepcopy(original_data)
    profiles = firmware_profiles()
    by_firmware = {candidate.firmware: candidate for candidate in candidates}
    if len(by_firmware) != len(candidates):
        raise BuildError("a source family was supplied more than once")

    for candidate in candidates:
        existing = data["firmwares"].get(candidate.firmware, {})
        existing_snapshot = existing.get("source_snapshot")
        existing_tree = existing.get("source_tree_git_sha1")
        if existing_snapshot and not existing_tree:
            existing_tree = snapshot_tree(existing_snapshot)
        if (
            existing_snapshot
            and existing_tree != candidate.source_tree_git_sha1
            and not getattr(args, "force", False)
        ):
            raise BuildError(
                f"source family {candidate.firmware} is already imported with "
                "different content; pass --force to replace it"
            )

    active_marker_path = sdk_active_path() / ".psvr2-source-snapshot.json"
    active_before = (
        read_json(active_marker_path, {})
        if active_marker_path.is_file()
        else {}
    )
    old_refs = {
        firmware: read_snapshot_ref(firmware)
        for firmware in by_firmware
    }
    updates: dict[str, str] = {}
    result: dict[str, str] = {}
    for firmware in profiles:
        candidate = by_firmware.get(firmware)
        if not candidate:
            continue
        position = profiles.index(firmware)
        parent_snapshot = next(
            (
                data["firmwares"].get(previous, {}).get("source_snapshot")
                for previous in reversed(profiles[:position])
                if data["firmwares"].get(previous, {}).get("source_snapshot")
            ),
            None,
        )
        snapshot = commit_source_tree(
            candidate.source_tree_git_sha1,
            firmware=firmware,
            archive_sha256=candidate.archive_sha256,
            parent_snapshot=parent_snapshot,
        )
        updates[firmware] = snapshot
        result[firmware] = snapshot
        firmware_data = data["firmwares"].setdefault(firmware, {})
        firmware_data["kernel_source"] = str(sdk_active_path())
        firmware_data["kernel_build"] = str(DEFAULT_OUTPUT / firmware / "kernel")
        firmware_data["source_archive_sha256"] = candidate.archive_sha256
        firmware_data["source_tree_git_sha1"] = candidate.source_tree_git_sha1
        firmware_data["source_tree_sha256"] = candidate.source_tree_sha256
        firmware_data["source_verification"] = candidate.verification
        firmware_data["source_snapshot"] = snapshot
        data["default_firmware"] = firmware
    if getattr(args, "cross_prefix", None):
        data["cross_prefix"] = args.cross_prefix

    refs_applied = False
    config_applied = False
    try:
        update_snapshot_refs(updates, expected=old_refs)
        refs_applied = True
        write_json(path, data)
        config_applied = True
        selected = data["default_firmware"]
        activate_source_snapshot(
            selected,
            data["firmwares"][selected]["source_snapshot"],
        )
    except BaseException:
        if refs_applied:
            update_snapshot_refs(
                {firmware: old_refs[firmware] for firmware in updates},
                expected=updates,
            )
        if config_applied or path.exists() != config_existed:
            restore_config_file(
                path,
                existed=config_existed,
                original_data=original_data,
            )
        previous_firmware = active_before.get("firmware")
        previous_snapshot = active_before.get("snapshot")
        if previous_firmware and previous_snapshot:
            activate_source_snapshot(previous_firmware, previous_snapshot)
        elif sdk_active_path().exists():
            shutil.rmtree(sdk_active_path())
        raise

    if compact:
        compact_snapshot_store()
    for firmware, snapshot in result.items():
        print(f"Kernel source snapshot {firmware}: {snapshot}")
    print(f"Active kernel source: {sdk_active_path()}")
    print(f"Saved configuration: {path}")
    return result


def identical_existing_import(
    args: argparse.Namespace,
    *,
    firmware: str,
    archive: Path,
) -> str | None:
    data = load_config(args)
    existing = data["firmwares"].get(firmware, {})
    snapshot = existing.get("source_snapshot")
    if not snapshot:
        return None
    archive_sha256 = sha256_file(archive)
    if existing.get("source_archive_sha256") != archive_sha256:
        return None
    archive_owner = catalog_archive_owner(archive_sha256)
    if archive_owner and archive_owner != firmware:
        raise BuildError(
            f"archive hash belongs to source family {archive_owner}, not {firmware}"
        )
    tree = snapshot_tree(snapshot)
    tree_sha256 = existing.get("source_tree_sha256")
    catalog_profile = source_catalog()["firmwares"][firmware]
    if not tree_sha256 and tree == catalog_profile["source_tree_git_sha1"]:
        tree_sha256 = catalog_profile["source_tree_sha256"]
    if not tree_sha256:
        raise BuildError(
            f"source family {firmware} lacks a canonical tree SHA-256; "
            "reimport its archive to migrate the source metadata"
        )
    verification = verify_source_identity(
        firmware=firmware,
        archive_sha256=archive_sha256,
        source_tree_git_sha1=tree,
        source_tree_sha256=tree_sha256,
        allow_unverified_source=(
            existing.get("source_verification") == "unverified"
            or getattr(args, "allow_unverified_source", False)
        ),
    )
    existing["source_tree_git_sha1"] = tree
    existing["source_tree_sha256"] = tree_sha256
    existing["source_verification"] = verification
    data["default_firmware"] = firmware
    write_json(config_path(args), data)
    activate_source_snapshot(firmware, snapshot)
    print(f"Linux source family {firmware} is already imported ({snapshot[:12]}).")
    return snapshot


def import_source_archive(args: argparse.Namespace, *, compact: bool) -> str:
    firmware = resolve_firmware_profile(args.firmware)
    archive = dragged_path(args.archive)
    if not archive.is_file() or not is_supported_archive(archive):
        raise BuildError(f"unsupported or missing archive: {archive}")
    identical = identical_existing_import(args, firmware=firmware, archive=archive)
    if identical:
        return identical
    candidate = preflight_source_archive(
        firmware=firmware,
        archive=archive,
        allow_unverified_source=getattr(args, "allow_unverified_source", False),
    )
    return apply_source_candidates(
        args,
        [candidate],
        compact=compact,
    )[firmware]


@requires_source_lease
def cmd_sdk_import(args: argparse.Namespace) -> int:
    import_source_archive(args, compact=True)
    return 0


def ensure_macos_host_elf_header(firmware: str) -> None:
    if platform.system() != "Darwin":
        return
    host_include = Path(os.environ.get("PSVR2_HOST_INCLUDE", str(REPO_ROOT / ".local/inputs/host-include"))).expanduser().resolve()
    if (host_include / "elf.h").is_file():
        print(f"Reusing existing host ELF declarations: {host_include / 'elf.h'}")
        return
    cmd_sources(argparse.Namespace(component="glibc", cache=None,
                                   host_include=str(host_include), firmware=firmware))


@requires_source_lease
def cmd_setup(args: argparse.Namespace) -> int:
    bootstrap_args = argparse.Namespace(
        config=args.config,
        check=False,
        yes=args.yes,
    )
    if cmd_bootstrap(bootstrap_args):
        print("Resolve or approve the missing host tools, then rerun setup.")
        return 1

    archive = dragged_path(args.archive)
    if not archive.is_file():
        raise BuildError(f"archive does not exist: {archive}")
    if not is_supported_archive(archive):
        raise BuildError(f"unsupported archive format: {archive}")
    firmware = resolve_firmware_profile(args.firmware)
    print(f"Firmware profile: {firmware}")

    force = args.force
    existing_data = load_config(args)["firmwares"].get(firmware, {})
    existing = existing_data.get("source_snapshot")
    supplied_hash = sha256_file(archive)
    identical = (
        existing
        and existing_data.get("source_archive_sha256") == supplied_hash
    )
    if existing and not identical and not force:
        force = confirm(f"Replace the existing imported source family {firmware}?")
        if not force:
            raise BuildError("setup cancelled without replacing the existing source")

    import_args = argparse.Namespace(
        archive=str(archive),
        firmware=firmware,
        cross_prefix=None,
        force=force,
        allow_unverified_source=getattr(args, "allow_unverified_source", False),
        config=args.config,
    )
    import_source_archive(import_args, compact=True)
    if args.no_prepare:
        print(
            "\nSource and toolchains are configured. Run "
            f"'./build.sh prepare --firmware {firmware}' before building modules."
        )
        return 0

    ensure_macos_host_elf_header(firmware)

    settings_args = argparse.Namespace(
        config=args.config,
        firmware=firmware,
        verbose=args.verbose,
    )
    settings = resolve_settings(settings_args, require_kernel=True)
    prepare_kernel(settings, args.defconfig)
    if cmd_doctor(settings_args):
        raise BuildError("post-setup validation failed; review the checks above")
    print("\nSetup complete. Build all custom modules with:")
    print(f"  ./build.sh modules --firmware {firmware}")
    return 0


def source_pair_map(
    pairs: list[list[str]],
) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for requested_firmware, value in pairs:
        firmware = resolve_firmware_profile(requested_firmware)
        if firmware in result:
            raise BuildError(f"source family {firmware} was supplied more than once")
        archive = dragged_path(value)
        if not is_supported_archive(archive):
            raise BuildError(f"unsupported or missing archive: {archive}")
        result[firmware] = archive
    return result


def import_source_set(args: argparse.Namespace, *, require_all: bool) -> None:
    sources = source_pair_map(args.source)
    configured = load_config(args)["firmwares"]
    if require_all:
        missing = [
            name
            for name in firmware_profiles()
            if name not in sources
            and not configured.get(name, {}).get("source_snapshot")
        ]
        if missing:
            raise BuildError(
                "the resulting configuration would still be missing source "
                "families: " + ", ".join(missing)
            )
    candidates: list[SourceCandidate] = []
    for firmware in firmware_profiles():
        archive = sources.get(firmware)
        if not archive:
            continue
        print(f"\n=== Preflighting Linux source family {firmware} ===")
        candidates.append(
            preflight_source_archive(
                firmware=firmware,
                archive=archive,
                allow_unverified_source=getattr(
                    args,
                    "allow_unverified_source",
                    False,
                ),
            )
        )
    apply_source_candidates(args, candidates, compact=True)


@requires_source_lease
def cmd_sdk_import_set(args: argparse.Namespace) -> int:
    import_source_set(args, require_all=args.require_all)
    return 0


def configured_snapshot(
    data: dict[str, Any],
    firmware: str,
) -> tuple[str, dict[str, Any]]:
    profile = data["firmwares"].get(firmware, {})
    snapshot = profile.get("source_snapshot")
    if not snapshot:
        raise BuildError(
            f"source family {firmware} is not imported; supply its archive first"
        )
    return snapshot, profile


def diff_category(path: str) -> str:
    categories = (
        ("arch/arm64/", "arm64"),
        ("drivers/usb/", "usb"),
        ("drivers/gpu/", "gpu"),
        ("drivers/video/", "display"),
        ("drivers/misc/mediatek/", "mediatek"),
        ("include/", "headers"),
        ("kernel/", "kernel"),
        ("security/", "security"),
    )
    for prefix, name in categories:
        if path.startswith(prefix):
            return name
    if path.endswith(("Kconfig", "defconfig", ".config")):
        return "configuration"
    return "other"


def generate_firmware_diff(
    data: dict[str, Any],
    older: str,
    newer: str,
) -> dict[str, Any]:
    older_snapshot, older_config = configured_snapshot(data, older)
    newer_snapshot, newer_config = configured_snapshot(data, newer)
    store = sdk_store_path()
    git = executable("git")
    if not git or not (store / "HEAD").is_file():
        raise BuildError(f"kernel-source snapshot store is missing: {store}")
    output = DEFAULT_OUTPUT / "firmware-diffs" / f"{older}-to-{newer}"
    output.mkdir(parents=True, exist_ok=True)
    raw = capture(
        [
            git,
            "--git-dir",
            str(store),
            "diff",
            "--no-renames",
            "--name-status",
            older_snapshot,
            newer_snapshot,
        ]
    )
    changes: list[dict[str, str]] = []
    status_counts: dict[str, int] = {}
    category_counts: dict[str, int] = {}
    for line in raw.splitlines():
        if not line:
            continue
        status, path = line.split("\t", 1)
        status = status[0]
        category = diff_category(path)
        status_counts[status] = status_counts.get(status, 0) + 1
        category_counts[category] = category_counts.get(category, 0) + 1
        changes.append({"status": status, "path": path, "category": category})
    report = {
        "schema": 1,
        "from": {
            "firmware": older,
            "snapshot": older_snapshot,
            "archive_sha256": older_config.get("source_archive_sha256"),
            "source_tree_git_sha1": older_config.get("source_tree_git_sha1"),
            "source_tree_sha256": older_config.get("source_tree_sha256"),
        },
        "to": {
            "firmware": newer,
            "snapshot": newer_snapshot,
            "archive_sha256": newer_config.get("source_archive_sha256"),
            "source_tree_git_sha1": newer_config.get("source_tree_git_sha1"),
            "source_tree_sha256": newer_config.get("source_tree_sha256"),
        },
        "changed_files": len(changes),
        "status_counts": status_counts,
        "category_counts": category_counts,
        "changes": changes,
    }
    write_json(output / "report.json", report)
    lines = [
        f"# PSVR2 Linux source diff: {older} to {newer}",
        "",
        f"- Changed files: {len(changes)}",
        f"- Added: {status_counts.get('A', 0)}",
        f"- Modified: {status_counts.get('M', 0)}",
        f"- Deleted: {status_counts.get('D', 0)}",
        "",
        "## Relevant areas",
        "",
    ]
    for category, count in sorted(
        category_counts.items(), key=lambda item: (-item[1], item[0])
    ):
        lines.append(f"- {category}: {count}")
    lines.extend(["", "## Files", ""])
    lines.extend(
        f"- `{change['status']}` `{change['path']}`" for change in changes
    )
    (output / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Diffed {older} -> {newer}: {len(changes)} changed files")
    return report


@requires_source_lease
def cmd_sdk_diff(args: argparse.Namespace) -> int:
    data = load_config(args)
    if args.all:
        configured = [
            firmware
            for firmware in firmware_profiles()
            if data["firmwares"].get(firmware, {}).get("source_snapshot")
        ]
        if len(configured) < 2:
            raise BuildError("at least two imported source families are required")
        pairs = list(zip(configured, configured[1:]))
        expected_directories = {
            f"{older}-to-{newer}" for older, newer in pairs
        }
        diff_root = DEFAULT_OUTPUT / "firmware-diffs"
        if diff_root.is_dir():
            for child in diff_root.iterdir():
                if (
                    child.is_dir()
                    and (child / "report.json").is_file()
                    and child.name not in expected_directories
                ):
                    shutil.rmtree(child)
                    print(f"Removed stale firmware-diff report {child.name}")
    else:
        if len(args.firmwares) != 2:
            raise BuildError("supply two firmware versions or pass --all")
        pairs = [
            (
                resolve_firmware_profile(args.firmwares[0]),
                resolve_firmware_profile(args.firmwares[1]),
            )
        ]
    for older, newer in pairs:
        generate_firmware_diff(data, older, newer)
    return 0


@requires_source_lease
def cmd_sdk_status(args: argparse.Namespace) -> int:
    data = load_config(args)
    print(f"Snapshot store: {sdk_store_path()}")
    print(f"Active checkout: {sdk_active_path()}")
    for firmware, metadata in manifest()["firmwares"].items():
        configured = data["firmwares"].get(firmware, {})
        snapshot = configured.get("source_snapshot")
        archive_hash = configured.get("source_archive_sha256")
        state = snapshot[:12] if snapshot else "not imported"
        hash_text = archive_hash[:12] if archive_hash else "-"
        verification = configured.get("source_verification") or (
            "catalog-migratable" if snapshot else "-"
        )
        print(
            f"  {firmware:5}  {metadata['label']:14}  "
            f"snapshot={state:12} archive={hash_text} identity={verification}"
        )
    return 0


@requires_source_lease
def cmd_sdk_verify(args: argparse.Namespace) -> int:
    data = load_config(args)
    supplied = source_pair_map(args.source) if args.source else {}
    failures: list[str] = []
    warnings: list[str] = []
    store = sdk_store_path()
    git = executable("git")
    has_snapshots = any(
        profile.get("source_snapshot")
        for profile in data["firmwares"].values()
    )
    if has_snapshots and (not git or not (store / "HEAD").is_file()):
        failures.append(f"snapshot store is unavailable: {store}")
    elif has_snapshots:
        try:
            store = ensure_snapshot_store()
            capture([git, "--git-dir", str(store), "fsck", "--full", "--no-dangling"])
            print("[ok] Git object store integrity")
        except BuildError as exc:
            failures.append(f"Git object store integrity: {exc}")
    else:
        print("[note] no snapshot-backed source families are configured")

    configured_count = 0
    snapshot_count = 0
    for firmware in firmware_profiles():
        profile_failure_count = len(failures)
        profile = data["firmwares"].get(firmware, {})
        snapshot = profile.get("source_snapshot")
        if not snapshot:
            manual_source_value = profile.get("kernel_source")
            if not manual_source_value:
                continue
            configured_count += 1
            manual_source = Path(manual_source_value)
            try:
                validate_psvr2_kernel_source(manual_source)
                actual_tree_sha256 = canonical_source_tree_sha256(manual_source)
            except BuildError as exc:
                failures.append(f"{firmware}: manual source: {exc}")
                continue
            recorded_tree_sha256 = profile.get("source_tree_sha256")
            if recorded_tree_sha256 != actual_tree_sha256:
                failures.append(
                    f"{firmware}: manual source tree changed since configuration"
                )
            tree_owner = catalog_tree_owner(actual_tree_sha256)
            if tree_owner and tree_owner != firmware:
                failures.append(
                    f"{firmware}: manual tree belongs to source family {tree_owner}"
                )
            verification = profile.get("source_verification")
            inferred = "official-tree" if tree_owner == firmware else "unverified"
            if verification != inferred:
                failures.append(
                    f"{firmware}: recorded verification {verification!r} does not "
                    f"match inferred state {inferred!r}"
                )
            if inferred == "unverified":
                message = f"{firmware}: intentionally unverified manual source"
                if args.require_official:
                    failures.append(message)
                else:
                    warnings.append(message)
            for kind in ("modules", "tools"):
                receipt_directory = DEFAULT_OUTPUT / firmware / kind
                if not receipt_directory.is_dir():
                    continue
                for receipt_path in receipt_directory.glob("*.json"):
                    receipt = read_json(receipt_path, {})
                    receipt_tree = receipt.get("kernel_source_tree_sha256")
                    if kind == "modules" and receipt_tree != actual_tree_sha256:
                        failures.append(
                            f"{firmware}: stale manual source hash in "
                            f"{receipt_path.name}"
                        )
                    if (
                        kind == "tools"
                        and receipt_tree is not None
                        and receipt_tree != actual_tree_sha256
                    ):
                        failures.append(
                            f"{firmware}: stale manual source hash in "
                            f"{receipt_path.name}"
                        )
                    artifact_name = receipt.get("artifact")
                    expected_artifact_hash = receipt.get("artifact_sha256")
                    if artifact_name and expected_artifact_hash:
                        artifact = receipt_path.parent / artifact_name
                        if not artifact.is_file():
                            failures.append(
                                f"{firmware}: receipt artifact is missing: {artifact}"
                            )
                        elif sha256_file(artifact) != expected_artifact_hash:
                            failures.append(
                                f"{firmware}: artifact hash mismatch: {artifact}"
                            )
            profile_status = "ok" if len(failures) == profile_failure_count else "fail"
            print(
                f"[{profile_status}] {firmware}: "
                f"manual-tree={actual_tree_sha256[:12]} "
                f"identity={inferred}"
            )
            continue
        configured_count += 1
        snapshot_count += 1
        archive_hash = profile.get("source_archive_sha256")
        configured_tree = profile.get("source_tree_git_sha1")
        configured_tree_sha256 = profile.get("source_tree_sha256")
        verification = profile.get("source_verification")
        if (
            not isinstance(archive_hash, str)
            or len(archive_hash) != 64
        ):
            failures.append(f"{firmware}: invalid or missing archive SHA-256")
            continue
        try:
            actual_tree = snapshot_tree(snapshot)
        except BuildError as exc:
            failures.append(f"{firmware}: snapshot cannot be read: {exc}")
            continue
        if configured_tree and configured_tree != actual_tree:
            failures.append(
                f"{firmware}: configured tree {configured_tree} does not match "
                f"snapshot tree {actual_tree}"
            )
        catalog_profile = source_catalog()["firmwares"][firmware]
        if (
            not configured_tree_sha256
            and actual_tree == catalog_profile["source_tree_git_sha1"]
        ):
            configured_tree_sha256 = catalog_profile["source_tree_sha256"]
        if not configured_tree_sha256:
            failures.append(f"{firmware}: canonical source-tree SHA-256 is missing")
            continue
        try:
            inferred = verify_source_identity(
                firmware=firmware,
                archive_sha256=archive_hash,
                source_tree_git_sha1=actual_tree,
                source_tree_sha256=configured_tree_sha256,
                allow_unverified_source=verification == "unverified",
            )
        except BuildError as exc:
            failures.append(f"{firmware}: {exc}")
            inferred = "invalid"
        if verification and verification != inferred:
            failures.append(
                f"{firmware}: recorded verification {verification!r} does not "
                f"match inferred state {inferred!r}"
            )
        if inferred == "unverified":
            message = f"{firmware}: intentionally unverified custom source"
            if args.require_official:
                failures.append(message)
            else:
                warnings.append(message)

        ref_value = read_snapshot_ref(firmware)
        if ref_value != snapshot:
            failures.append(
                f"{firmware}: ref points to {ref_value or 'nothing'}, expected {snapshot}"
            )

        supplied_archive = supplied.get(firmware)
        if supplied_archive:
            supplied_hash = sha256_file(supplied_archive)
            if supplied_hash != archive_hash:
                failures.append(
                    f"{firmware}: supplied archive hashes to {supplied_hash}, "
                    f"configured value is {archive_hash}"
                )

        kernel_build_value = profile.get("kernel_build")
        if kernel_build_value:
            kernel_build = Path(kernel_build_value)
            marker_path = kernel_build / ".psvr2-source-snapshot.json"
            if kernel_build.exists():
                marker = read_json(marker_path, {}) if marker_path.is_file() else {}
                if marker.get("snapshot") != snapshot:
                    failures.append(
                        f"{firmware}: kernel build marker does not match its snapshot"
                    )
                marker_tree_sha256 = marker.get("source_tree_sha256")
                if (
                    marker_tree_sha256 is not None
                    and marker_tree_sha256 != configured_tree_sha256
                ):
                    failures.append(
                        f"{firmware}: kernel build marker has a stale source-tree hash"
                    )

        for kind in ("modules", "tools"):
            receipt_directory = DEFAULT_OUTPUT / firmware / kind
            if not receipt_directory.is_dir():
                continue
            for receipt_path in receipt_directory.glob("*.json"):
                receipt = read_json(receipt_path, {})
                receipt_archive_hash = receipt.get("kernel_source_archive_sha256")
                receipt_snapshot = receipt.get("kernel_source_snapshot")
                receipt_tree_sha256 = receipt.get("kernel_source_tree_sha256")
                if (
                    kind == "modules"
                    or receipt_archive_hash is not None
                ) and receipt_archive_hash != archive_hash:
                    failures.append(
                        f"{firmware}: stale archive hash in {receipt_path.name}"
                    )
                if (
                    kind == "modules"
                    or receipt_snapshot is not None
                ) and receipt_snapshot != snapshot:
                    failures.append(
                        f"{firmware}: stale snapshot in {receipt_path.name}"
                    )
                if (
                    receipt_tree_sha256 is not None
                    and receipt_tree_sha256 != configured_tree_sha256
                ):
                    failures.append(
                        f"{firmware}: stale source-tree hash in {receipt_path.name}"
                    )
                artifact_name = receipt.get("artifact")
                expected_artifact_hash = receipt.get("artifact_sha256")
                if artifact_name and expected_artifact_hash:
                    artifact = receipt_path.parent / artifact_name
                    if not artifact.is_file():
                        failures.append(
                            f"{firmware}: receipt artifact is missing: {artifact}"
                        )
                    elif sha256_file(artifact) != expected_artifact_hash:
                        failures.append(
                            f"{firmware}: artifact hash mismatch: {artifact}"
                        )
        profile_status = "ok" if len(failures) == profile_failure_count else "fail"
        print(
            f"[{profile_status}] {firmware}: snapshot={snapshot[:12]} "
            f"tree={actual_tree[:12]} identity={inferred}"
        )

    active_marker = sdk_active_path() / ".psvr2-source-snapshot.json"
    if active_marker.is_file():
        marker = read_json(active_marker, {})
        active_firmware = marker.get("firmware")
        active_snapshot = marker.get("snapshot")
        configured_snapshot_value = (
            data["firmwares"].get(active_firmware, {}).get("source_snapshot")
            if active_firmware
            else None
        )
        if active_snapshot != configured_snapshot_value:
            failures.append("active source checkout is not a configured snapshot")
        else:
            try:
                validate_kernel_source(sdk_active_path())
            except BuildError as exc:
                failures.append(f"active source checkout: {exc}")
    elif snapshot_count:
        failures.append("active source checkout marker is missing")

    for warning in warnings:
        print(f"[warn] {warning}")
    for failure in failures:
        print(f"[fail] {failure}")
    if failures:
        print(
            f"SDK verification failed: {len(failures)} failure(s), "
            f"{len(warnings)} warning(s)"
        )
        return 1
    print(
        f"SDK verification passed: {configured_count} source family/families, "
        f"{len(warnings)} warning(s)"
    )
    return 0


def matrix_module_names(args: argparse.Namespace) -> list[str]:
    recipes = manifest()["modules"]
    names = list(getattr(args, "names", []))
    group = getattr(args, "group", None)
    if group:
        names.extend(
            name for name, recipe in recipes.items() if recipe["category"] == group
        )
    if not names:
        names = list(recipes)
    unknown = [name for name in names if name not in recipes]
    if unknown:
        raise BuildError("unknown modules: " + ", ".join(unknown))
    return list(dict.fromkeys(names))


def selected_matrix_firmwares(requested: list[str]) -> list[str]:
    if not requested:
        return [DEFAULT_FIRMWARE]
    if any(item.lower() == "all" for item in requested):
        if len(requested) != 1:
            raise BuildError("--firmware all cannot be combined with another firmware")
        return firmware_profiles()
    return list(
        dict.fromkeys(resolve_firmware_profile(item) for item in requested)
    )


@requires_source_lease
def cmd_matrix_modules(args: argparse.Namespace) -> int:
    data = load_config(args)
    profiles = selected_matrix_firmwares(list(args.firmwares))
    missing = [
        firmware
        for firmware in profiles
        if not data["firmwares"].get(firmware, {}).get("source_snapshot")
    ]
    if missing and not args.allow_missing:
        raise BuildError("source families are not imported: " + ", ".join(missing))
    profiles = [firmware for firmware in profiles if firmware not in missing]
    names = matrix_module_names(args)
    report: dict[str, Any] = {
        "schema": 1,
        "firmwares": {},
        "modules": names,
    }
    failed = False
    for firmware in profiles:
        print(f"\n=== Building module matrix for {firmware} ===")
        settings_args = argparse.Namespace(**vars(args))
        settings_args.firmware = firmware
        settings = resolve_settings(settings_args, require_kernel=True)
        firmware_result: dict[str, Any] = {
            "source_snapshot": settings.source_snapshot,
            "archive_sha256": settings.source_archive_sha256,
            "source_tree_git_sha1": settings.source_tree_git_sha1,
            "source_tree_sha256": settings.source_tree_sha256,
            "modules": {},
        }
        report["firmwares"][firmware] = firmware_result
        try:
            if not args.no_prepare:
                prepare_kernel(settings, args.defconfig)
            elif not kernel_ready(settings):
                raise BuildError(
                    f"kernel build tree is not prepared: {settings.kernel_build}"
                )
        except BuildError as exc:
            failed = True
            firmware_result["prepare_error"] = str(exc)
            for name in names:
                firmware_result["modules"][name] = {
                    "status": "not-built",
                    "error": str(exc),
                }
            continue
        built: set[str] = set()
        for name in names:
            supported = manifest()["modules"][name].get("firmwares")
            if supported and firmware not in supported:
                firmware_result["modules"][name] = {
                    "status": "unsupported",
                    "supported_firmwares": supported,
                }
                continue
            try:
                artifact = build_module(
                    name,
                    settings,
                    built,
                    prepare=False,
                    defconfig=args.defconfig,
                )
                firmware_result["modules"][name] = {
                    "status": "ok",
                    "artifact": str(artifact),
                    "sha256": sha256_file(artifact),
                }
            except BuildError as exc:
                failed = True
                firmware_result["modules"][name] = {
                    "status": "failed",
                    "error": str(exc),
                }
    output = DEFAULT_OUTPUT / "matrix"
    output.mkdir(parents=True, exist_ok=True)
    write_json(output / "modules.json", report)
    lines = [
        "# PSVR2 kernel-module compatibility matrix",
        "",
        "| Source family | " + " | ".join(names) + " |",
        "|---|" + "|".join("---" for _ in names) + "|",
    ]
    for firmware, result in report["firmwares"].items():
        cells = [
            result["modules"].get(name, {}).get("status", "not-built")
            for name in names
        ]
        lines.append(f"| {firmware} | " + " | ".join(cells) + " |")
    (output / "modules.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"\nCompatibility matrix: {output / 'modules.md'}")
    return 1 if failed else 0


@requires_source_lease
def cmd_setup_all(args: argparse.Namespace) -> int:
    bootstrap_args = argparse.Namespace(
        config=args.config,
        check=False,
        yes=args.yes,
    )
    if cmd_bootstrap(bootstrap_args):
        print("Resolve or approve the missing host tools, then rerun setup-all.")
        return 1
    import_source_set(args, require_all=True)
    diff_args = argparse.Namespace(
        config=args.config,
        all=True,
        firmwares=[],
    )
    cmd_sdk_diff(diff_args)
    if args.no_build:
        print("\nAll source families are imported and diffed.")
        return 0
    ensure_macos_host_elf_header(DEFAULT_FIRMWARE)
    matrix_args = argparse.Namespace(
        config=args.config,
        firmwares=["all"],
        allow_missing=False,
        names=[],
        group="custom",
        no_prepare=False,
        defconfig=args.defconfig,
        cross_prefix=None,
        tool_cc=None,
        glibc_version=None,
        make=None,
        jobs=args.jobs,
        sysroot=None,
        dynamic_linker=None,
        kernel_source=None,
        kernel_build=None,
        verbose=args.verbose,
    )
    return cmd_matrix_modules(matrix_args)


@requires_source_lease
def cmd_clean(args: argparse.Namespace) -> int:
    settings = resolve_settings(args, require_kernel=False)
    target = settings.output.resolve()
    expected_parent = DEFAULT_OUTPUT.resolve()
    try:
        target.relative_to(expected_parent)
    except ValueError as exc:
        raise BuildError(f"refusing to clean unexpected path: {target}") from exc
    if not args.yes:
        raise BuildError(f"refusing to remove {target} without --yes")
    if target.exists():
        shutil.rmtree(target)
        print(f"Removed generated build output {target}")
    else:
        print(f"Nothing to clean at {target}")
    return 0


def add_config_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--config", help=f"configuration file (default: {DEFAULT_CONFIG})")


def add_firmware_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--firmware",
        default=None,
        help=(
            "target system version or source family (default: 06.00; "
            "module builds also accept 'all')"
        ),
    )


def add_unverified_source_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--allow-unverified-source",
        action="store_true",
        help=(
            "accept an intentionally modified PSVR2 source whose archive and "
            "tree hashes are absent from the project catalog"
        ),
    )


def add_toolchain_arguments(parser: argparse.ArgumentParser, kernel: bool) -> None:
    add_config_argument(parser)
    add_firmware_argument(parser)
    parser.add_argument("--cross-prefix", help="AArch64 Linux tool prefix")
    parser.add_argument("--tool-cc", help="AArch64 Linux userspace compiler command")
    parser.add_argument("--glibc-version", help="minimum target glibc version for Zig")
    parser.add_argument("--make", help="GNU make executable")
    parser.add_argument("--jobs", type=positive_jobs, help="parallel build jobs")
    parser.add_argument("--sysroot", help="AArch64 target sysroot")
    parser.add_argument("--dynamic-linker", help="target ELF interpreter")
    parser.add_argument("-v", "--verbose", action="store_true")
    if kernel:
        parser.add_argument("--kernel-source", help="Linux kernel source directory")
        parser.add_argument("--kernel-build", help="prepared kernel output directory")


def add_module_prepare_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--defconfig",
        default="sie_release_mt3612_asic_a0_defconfig",
        help="kernel defconfig target",
    )
    parser.add_argument(
        "--no-prepare",
        action="store_true",
        help="require an already-prepared kernel build tree",
    )


def unpack_nested_source_archive(root: Path) -> None:
    """Unwrap bounded Sony ZIP/tar nesting, including glibc's extra source tar."""
    for _ in range(4):
        # Once BusyBox's source root is present, its test archives are inputs,
        # not another wrapper layer around the source distribution.
        if any((path.parent / "Config.in").is_file() and
               (path.parent / "include/libbb.h").is_file()
               for path in root.rglob("Makefile")):
            return
        nested = [path for path in root.rglob("*")
                  if path.is_file() and is_supported_archive(path)]
        if not nested:
            return
        if len(nested) != 1:
            raise BuildError("expected one nested Sony source archive")
        archive = nested[0]
        extract_archive(archive, archive.parent)
        archive.unlink()
    if any(path.is_file() and is_supported_archive(path) for path in root.rglob("*")):
        raise BuildError("Sony source archive nesting exceeds four layers")


def public_glibc_elf_header(archive: Path) -> str:
    """Read the one public header without materializing unrelated SDK symlinks."""
    with tempfile.TemporaryDirectory(prefix="psvr2-glibc-header-") as temporary:
        current = archive
        for depth in range(4):
            if zipfile.is_zipfile(current):
                with zipfile.ZipFile(current) as bundle:
                    members = [entry for entry in bundle.infolist() if not entry.is_dir()]
                    validate_archive_limits(archive=current, members=len(members), expanded_bytes=sum(entry.file_size for entry in members))
                    headers = [entry for entry in members if entry.filename.endswith("/elf/elf.h")]
                    nested = [entry for entry in members if entry.filename.endswith((".zip", ".tar", ".tar.bz2", ".tar.gz", ".tar.xz", ".tgz"))]
                    selected = headers if headers else nested
                    if len(selected) != 1:
                        raise BuildError("expected one nested glibc archive or ELF header")
                    if selected[0].file_size > MAX_EXTERNAL_ARCHIVE_BYTES:
                        raise BuildError("nested glibc input exceeds its size limit")
                    data = bundle.read(selected[0])
                    name = selected[0].filename
            else:
                with tarfile.open(current, "r:*") as bundle:
                    members = bundle.getmembers()
                    validate_archive_limits(archive=current, members=len(members), expanded_bytes=sum(entry.size for entry in members))
                    headers = [entry for entry in members if entry.isfile() and entry.name.endswith("/elf/elf.h")]
                    nested = [entry for entry in members if entry.isfile() and entry.name.endswith((".zip", ".tar", ".tar.bz2", ".tar.gz", ".tar.xz", ".tgz"))]
                    selected = headers if headers else nested
                    if len(selected) != 1:
                        raise BuildError("expected one nested glibc archive or ELF header")
                    if selected[0].size > MAX_EXTERNAL_ARCHIVE_BYTES:
                        raise BuildError("nested glibc input exceeds its size limit")
                    stream = bundle.extractfile(selected[0])
                    if stream is None:
                        raise BuildError("cannot read public glibc input")
                    data = stream.read(MAX_EXTERNAL_ARCHIVE_BYTES + 1)
                    name = selected[0].name
            if len(data) > MAX_EXTERNAL_ARCHIVE_BYTES:
                raise BuildError("nested glibc input exceeds its size limit")
            if headers:
                return data.decode("utf-8")
            current = Path(temporary) / (str(depth) + "-" + Path(name).name)
            current.write_bytes(data)
    raise BuildError("glibc source archive nesting exceeds four layers")


def darwin_elf_header(contents: str) -> str:
    """Keep public ELF declarations and license; remove glibc-only host wrappers."""
    if "Elf64_Ehdr" not in contents or "GNU C Library" not in contents:
        raise BuildError("unrecognized glibc ELF header")
    for wrapper in ("#include <features.h>", "__BEGIN_DECLS", "__END_DECLS"):
        contents = contents.replace(wrapper, "")
    return contents


def cmd_sources(args: argparse.Namespace) -> int:
    """Fetch pinned public source archives into an ignored, user-owned cache."""
    if args.component == "kernel":
        firmware = resolve_firmware_profile(args.firmware)
        recipe = source_catalog()["firmwares"][firmware]
        url, expected = recipe["url"], recipe["archive_sha256"][0]
    else:
        recipe = (manifest()["tools"]["busybox"] if args.component == "busybox"
                  else manifest()["sources"]["glibc"])
        url, expected = recipe["source_url"], recipe["source_sha256"]
    if not url.startswith("https://www.playstation.com/"):
        raise BuildError("source URL is outside Sony's public HTTPS source service")
    cache = Path(args.cache).expanduser().resolve() if args.cache else DEFAULT_EXTERNAL_ROOT / "archives"
    archive = cache / Path(url).name
    if not archive.is_file() or sha256_file(archive) != expected:
        download_verified_archive(url, expected, archive)
    else:
        print(f"Verified cached source SHA-256: {expected}")
    print(f"Public source archive: {archive}")
    if args.component == "glibc":
        host_include = (Path(args.host_include).expanduser().resolve() if args.host_include
                        else Path(os.environ.get("PSVR2_HOST_INCLUDE", str(REPO_ROOT / ".local/inputs/host-include"))).expanduser().resolve())
        header = darwin_elf_header(public_glibc_elf_header(archive))
        host_include.mkdir(parents=True, exist_ok=True)
        (host_include / "elf.h").write_text(header)
        print(f"Generated macOS host ELF declarations: {host_include / 'elf.h'}")
    elif args.component == "kernel":
        print(f"Import with: ./build.sh sdk import {shlex.quote(str(archive))} --firmware {firmware}")
    else:
        print(f"Build with: ./build.sh tool busybox --source {shlex.quote(str(archive))}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Build PSVR2 kernel modules and target tools without Docker."
    )
    subcommands = parser.add_subparsers(dest="command", required=True)

    sources = subcommands.add_parser("sources", help="fetch pinned Sony public kernel, BusyBox or glibc sources")
    sources.add_argument("component", choices=("kernel", "busybox", "glibc"))
    sources.add_argument("--firmware", default=DEFAULT_FIRMWARE)
    sources.add_argument("--cache", help="external source archive cache directory")
    sources.add_argument("--host-include", help="external destination for generated macOS ELF header")
    sources.set_defaults(handler=cmd_sources)

    doctor = subcommands.add_parser("doctor", help="diagnose native build prerequisites")
    add_toolchain_arguments(doctor, kernel=True)
    doctor.set_defaults(handler=cmd_doctor)

    bootstrap = subcommands.add_parser(
        "bootstrap", help="diagnose or install native host toolchains"
    )
    add_config_argument(bootstrap)
    bootstrap.add_argument(
        "--check",
        action="store_true",
        help="report missing tools without prompting to install them",
    )
    bootstrap.add_argument(
        "--yes",
        action="store_true",
        help="approve all missing-tool installations (automation only)",
    )
    bootstrap.set_defaults(handler=cmd_bootstrap)

    setup = subcommands.add_parser(
        "setup",
        help="install tools, import an explicit archive path, and prepare Linux",
    )
    add_config_argument(setup)
    setup.add_argument(
        "archive",
        help="path to a user-supplied kernel-source zip or tar archive",
    )
    setup.add_argument(
        "--firmware",
        required=True,
        help="system version or source family matching the supplied archive",
    )
    setup.add_argument(
        "--yes",
        action="store_true",
        help="approve all missing-tool installations (automation only)",
    )
    setup.add_argument(
        "--force",
        action="store_true",
        help="replace an existing local import for this firmware",
    )
    add_unverified_source_argument(setup)
    setup.add_argument(
        "--defconfig", default="sie_release_mt3612_asic_a0_defconfig"
    )
    setup.add_argument(
        "--no-prepare",
        action="store_true",
        help="configure tools and source without preparing the kernel tree",
    )
    setup.add_argument("-v", "--verbose", action="store_true")
    setup.set_defaults(handler=cmd_setup)

    setup_all = subcommands.add_parser(
        "setup-all",
        help="import, diff, and build both maintained firmware source profiles",
    )
    add_config_argument(setup_all)
    setup_all.add_argument(
        "--source",
        action="append",
        nargs=2,
        required=True,
        metavar=("FIRMWARE", "ARCHIVE"),
        help=(
            "explicit source-family/archive pair; repeat for new or replaced "
            "families (already imported families are retained)"
        ),
    )
    setup_all.add_argument(
        "--yes",
        action="store_true",
        help="approve all missing-tool installations (automation only)",
    )
    setup_all.add_argument(
        "--force",
        action="store_true",
        help="replace changed snapshots for already imported source families",
    )
    add_unverified_source_argument(setup_all)
    setup_all.add_argument(
        "--defconfig", default="sie_release_mt3612_asic_a0_defconfig"
    )
    setup_all.add_argument("--jobs", type=positive_jobs)
    setup_all.add_argument(
        "--no-build",
        action="store_true",
        help="import and diff sources without running the module matrix",
    )
    setup_all.add_argument("-v", "--verbose", action="store_true")
    setup_all.set_defaults(handler=cmd_setup_all)

    configure = subcommands.add_parser("configure", help="save local build settings")
    add_config_argument(configure)
    configure.add_argument("--firmware", required=True)
    configure.add_argument("--kernel-source")
    configure.add_argument("--kernel-build")
    configure.add_argument("--cross-prefix")
    configure.add_argument("--tool-cc")
    configure.add_argument("--glibc-version")
    configure.add_argument("--make")
    configure.add_argument("--jobs", type=positive_jobs)
    configure.add_argument("--sysroot")
    configure.add_argument("--dynamic-linker")
    add_unverified_source_argument(configure)
    configure.set_defaults(handler=cmd_configure)

    config = subcommands.add_parser("config", help="show the effective saved configuration")
    add_config_argument(config)
    config.set_defaults(handler=cmd_config_show)

    prepare = subcommands.add_parser("prepare", help="configure and prepare the kernel tree")
    add_toolchain_arguments(prepare, kernel=True)
    prepare.add_argument(
        "--defconfig", default="sie_release_mt3612_asic_a0_defconfig"
    )
    prepare.add_argument("--force-config", action="store_true")
    prepare.set_defaults(handler=cmd_prepare)

    module = subcommands.add_parser("module", help="build one kernel module")
    module.add_argument("name")
    add_toolchain_arguments(module, kernel=True)
    add_module_prepare_arguments(module)
    module.set_defaults(handler=cmd_module)

    modules = subcommands.add_parser("modules", help="build multiple kernel modules")
    modules.add_argument("names", nargs="*")
    modules.add_argument("--group", choices=("custom",))
    add_toolchain_arguments(modules, kernel=True)
    add_module_prepare_arguments(modules)
    modules.set_defaults(handler=cmd_modules)

    tool = subcommands.add_parser("tool", help="build one AArch64 target tool")
    tool.add_argument("name")
    tool.add_argument("--no-strip", action="store_true")
    tool_source = tool.add_mutually_exclusive_group()
    tool_source.add_argument(
        "--source",
        help="explicit local source archive for an external-source tool",
    )
    tool_source.add_argument(
        "--download",
        action="store_true",
        help="download the external tool's pinned official source when uncached",
    )
    add_toolchain_arguments(tool, kernel=False)
    tool.set_defaults(handler=cmd_tool)

    wii_menu = subcommands.add_parser("wii-menu", help="build the maintained PSVR2 Wii Menu port")
    wii_menu.add_argument("--project", required=True, help="local Wii_Menu_C PSVR2 branch checkout")
    wii_menu.add_argument("--runtime-root", help="explicit local runtime root containing lib/libEGL.so.1 and lib/libGLESv2.so.2")
    wii_menu.add_argument("--no-strip", action="store_true")
    add_toolchain_arguments(wii_menu, kernel=False)
    wii_menu.set_defaults(handler=cmd_wii_menu)

    tools = subcommands.add_parser("tools", help="build multiple AArch64 target tools")
    tools.add_argument("names", nargs="*")
    tools.add_argument("--group", choices=("display", "diagnostic", "utility"))
    tools.add_argument("--no-strip", action="store_true")
    tools.add_argument(
        "--download",
        action="store_true",
        help="include external tools and download pinned source when uncached",
    )
    add_toolchain_arguments(tools, kernel=False)
    tools.set_defaults(handler=cmd_tools)

    matrix = subcommands.add_parser(
        "matrix", help="build compatibility matrices across source families"
    )
    matrix_commands = matrix.add_subparsers(dest="matrix_command", required=True)
    matrix_modules = matrix_commands.add_parser(
        "modules", help="build kernel modules across imported source families"
    )
    add_config_argument(matrix_modules)
    matrix_modules.add_argument("names", nargs="*")
    matrix_modules.add_argument("--group", choices=("custom",))
    matrix_modules.add_argument(
        "--firmware",
        dest="firmwares",
        action="append",
        default=[],
        help=(
            "system version or source family; repeat to select multiple, "
            "or use 'all' (default: 06.00)"
        ),
    )
    matrix_modules.add_argument(
        "--allow-missing",
        action="store_true",
        help="skip source families that have not been imported",
    )
    matrix_modules.add_argument("--cross-prefix", help="AArch64 Linux tool prefix")
    matrix_modules.add_argument(
        "--tool-cc", help="AArch64 Linux userspace compiler command"
    )
    matrix_modules.add_argument("--glibc-version")
    matrix_modules.add_argument("--make", help="GNU make executable")
    matrix_modules.add_argument("--jobs", type=positive_jobs)
    matrix_modules.add_argument("--sysroot")
    matrix_modules.add_argument("--dynamic-linker")
    matrix_modules.add_argument("-v", "--verbose", action="store_true")
    add_module_prepare_arguments(matrix_modules)
    matrix_modules.set_defaults(handler=cmd_matrix_modules)

    listing = subcommands.add_parser("list", help="list buildable modules and tools")
    listing.set_defaults(handler=cmd_list)

    sdk = subcommands.add_parser("sdk", help="manage local kernel source")
    sdk_commands = sdk.add_subparsers(dest="sdk_command", required=True)
    sdk_import = sdk_commands.add_parser(
        "import", help="extract a local kernel-source archive"
    )
    sdk_import.add_argument("archive")
    sdk_import.add_argument("--firmware", required=True)
    sdk_import.add_argument("--cross-prefix")
    sdk_import.add_argument("--force", action="store_true")
    add_unverified_source_argument(sdk_import)
    add_config_argument(sdk_import)
    sdk_import.set_defaults(handler=cmd_sdk_import)

    sdk_import_set = sdk_commands.add_parser(
        "import-set", help="import several explicit source-family/archive pairs"
    )
    sdk_import_set.add_argument(
        "--source",
        action="append",
        nargs=2,
        required=True,
        metavar=("FIRMWARE", "ARCHIVE"),
    )
    sdk_import_set.add_argument(
        "--require-all",
        action="store_true",
        help="require the resulting configuration to contain all seven families",
    )
    sdk_import_set.add_argument("--force", action="store_true")
    add_unverified_source_argument(sdk_import_set)
    add_config_argument(sdk_import_set)
    sdk_import_set.set_defaults(handler=cmd_sdk_import_set)

    sdk_diff = sdk_commands.add_parser(
        "diff", help="generate reports between imported source snapshots"
    )
    sdk_diff.add_argument("firmwares", nargs="*")
    sdk_diff.add_argument(
        "--all",
        action="store_true",
        help="diff every adjacent imported source family",
    )
    add_config_argument(sdk_diff)
    sdk_diff.set_defaults(handler=cmd_sdk_diff)

    sdk_status = sdk_commands.add_parser(
        "status", help="show imported source snapshots and hashes"
    )
    add_config_argument(sdk_status)
    sdk_status.set_defaults(handler=cmd_sdk_status)

    sdk_verify = sdk_commands.add_parser(
        "verify",
        help="verify configured source identities, snapshots, outputs, and receipts",
    )
    sdk_verify.add_argument(
        "--source",
        action="append",
        nargs=2,
        default=[],
        metavar=("FIRMWARE", "ARCHIVE"),
        help="also re-hash an explicitly supplied original archive",
    )
    sdk_verify.add_argument(
        "--require-official",
        action="store_true",
        help="treat intentionally unverified custom sources as failures",
    )
    add_config_argument(sdk_verify)
    sdk_verify.set_defaults(handler=cmd_sdk_verify)

    clean = subcommands.add_parser("clean", help="remove one firmware's generated output")
    add_config_argument(clean)
    add_firmware_argument(clean)
    clean.add_argument("--yes", action="store_true", help="confirm generated output removal")
    clean.set_defaults(handler=cmd_clean)
    return parser


def main(argv: list[str] | None = None) -> int:
    try:
        args = build_parser().parse_args(argv)
        return int(args.handler(args))
    except BuildError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"error: operating-system failure: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
