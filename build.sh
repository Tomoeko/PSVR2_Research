#!/bin/sh
set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

PYTHON=
for candidate in \
    "$(command -v python3 2>/dev/null || true)" \
    /usr/bin/python3 \
    /opt/homebrew/bin/python3 \
    /usr/local/bin/python3 \
    /opt/local/bin/python3
do
    [ -n "$candidate" ] || continue
    [ -x "$candidate" ] || continue
    if "$candidate" -c 'import sys; raise SystemExit(sys.version_info < (3, 9))' 2>/dev/null; then
        PYTHON=$candidate
        break
    fi
done

if [ -z "$PYTHON" ]; then
    echo "Python 3.9 or newer is required." >&2
    echo "On macOS, install Apple Command Line Tools and rerun ./build.sh setup." >&2
    exit 1
fi

export PYTHONDONTWRITEBYTECODE=1
export PYTHONUNBUFFERED=1
exec "$PYTHON" "$REPO_ROOT/tools/psvr2_build.py" "$@"
