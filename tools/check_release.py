#!/usr/bin/env python3
"""Check the Git file set before sharing this source repository."""

import argparse
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
TOP_LEVEL = {
    ".gitattributes", ".gitignore", "README.md", "LICENSE", "NOTICE.md",
    "build.sh", "docs", "psvr2_krw_c", "target", "tests", "tools",
}
TEXT_SUFFIXES = {".c", ".h", ".inc", ".S", ".py", ".sh", ".md",
                 ".json", ".txt", ".toml", ".patch", ".cmake", ".config"}
TEXT_NAMES = {"Makefile", "CMakeLists.txt", "LICENSE", ".gitignore",
              ".gitattributes", ".clang-format"}
MODULES = {"stage1", "stage3_serial", "rmmod_helper"}
FIRMWARES = {"01.10", "06.00"}
CONTENT_CHECKS = (
    ("host home path", re.compile(r"/(?:Users|home)/[A-Za-z0-9_.-]+/")),
    ("Windows home path", re.compile(r"[A-Za-z]:\\Users\\[A-Za-z0-9_.-]+\\")),
    ("private key", re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----")),
    ("GitHub credential", re.compile(r"\bgh[pousr]_[A-Za-z0-9]{20,}\b")),
    ("GitHub credential", re.compile(r"\bgithub_pat_[A-Za-z0-9_]{30,}\b")),
    ("cloud credential", re.compile(r"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b")),
)


def file_errors(name, data, mode="100644"):
    path = PurePosixPath(name)
    errors = []
    if not path.parts or path.parts[0] not in TOP_LEVEL:
        errors.append("unexpected repository root")
    if mode not in {"100644", "100755"}:
        errors.append("only regular source files are allowed")
    if path.name not in TEXT_NAMES and path.suffix not in TEXT_SUFFIXES:
        errors.append("unexpected file type")
    if len(data) > 1024 * 1024:
        errors.append("source file exceeds the review size limit")
    if b"\x00" in data:
        errors.append("binary content")
    try:
        contents = data.decode("utf-8")
    except UnicodeDecodeError:
        return errors + ["not UTF-8 source text"]
    for description, pattern in CONTENT_CHECKS:
        if pattern.search(contents):
            errors.append(description)
    if path.parts[:3] == ("target", "psvr2", "modules"):
        if len(path.parts) > 3 and path.parts[3] not in MODULES and name != "target/psvr2/modules/README.md":
            errors.append("module is outside the maintained set")
    return errors


def git_output(*arguments):
    return subprocess.check_output(["git", "-C", str(ROOT), *arguments])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--working-tree", action="store_true",
                        help="include non-ignored new files and inspect working contents")
    options = parser.parse_args()
    failures = []
    staged = {}
    for record in git_output("ls-files", "--stage", "-z").split(b"\x00"):
        if not record:
            continue
        metadata, name = record.split(b"\t", 1)
        mode, object_id, stage = metadata.decode("ascii").split()
        decoded_name = name.decode("utf-8")
        if stage != "0":
            failures.append((decoded_name, ["unresolved Git conflict"]))
        staged[decoded_name] = (mode, object_id)
    names = set(staged)
    if options.working_tree:
        names.update(name.decode("utf-8") for name in
                     git_output("ls-files", "--others", "--exclude-standard", "-z")
                     .split(b"\x00") if name)
    if not names:
        print("No source files were selected.", file=sys.stderr)
        return 1
    manifest = None
    for name in sorted(names):
        mode, object_id = staged.get(name, ("100644", None))
        if options.working_tree:
            path = ROOT / name
            if path.is_symlink() or not path.is_file():
                failures.append((name, ["not a regular source file"]))
                continue
            data = path.read_bytes()
        else:
            data = git_output("cat-file", "blob", object_id)
        errors = file_errors(name, data, mode)
        if errors:
            failures.append((name, errors))
        if name == "target/psvr2/build-manifest.json":
            try:
                manifest = json.loads(data)
            except (UnicodeDecodeError, ValueError):
                failures.append((name, ["invalid build manifest"]))
    if manifest is None:
        failures.append(("build manifest", ["missing manifest"]))
    else:
        if set(manifest.get("firmwares", {})) != FIRMWARES:
            failures.append(("build manifest", ["unexpected firmware profiles"]))
        if set(manifest.get("modules", {})) != MODULES:
            failures.append(("build manifest", ["unexpected module recipes"]))
    for name, errors in failures:
        print(f"{name}: {', '.join(errors)}", file=sys.stderr)
    if failures:
        return 1
    print(f"Source check passed: {len(names)} regular text files.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
