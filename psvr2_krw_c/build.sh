#!/usr/bin/env bash

set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
profile="release"
run_tests=1
clean_build=0
jobs=""
cmake_args=()

usage() {
    cat <<'EOF'
Usage: ./build.sh [profile] [options] [-- CMake options]

Build psvr2_krw_c using one of its CMake presets.

Profiles:
  release             Optimized build (default)
  debug               Debug build
  sanitize            Debug build with AddressSanitizer and UBSan
  ci                  Debug build with warnings treated as errors

Options:
  -c, --clean         Remove the selected build directory before configuring
  -j, --jobs N        Use N parallel build and test jobs
      --no-tests      Build without running the test suite
      --no-tools      Do not build the standalone helper tools
      --tools         Build the standalone helper tools (default)
  -h, --help          Show this help

Arguments after "--" are passed to CMake during configuration:
  ./build.sh debug -- -DBUILD_TESTING=OFF
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 2
}

while (($# > 0)); do
    case "$1" in
        release|debug|sanitize|ci)
            profile="$1"
            shift
            ;;
        -c|--clean)
            clean_build=1
            shift
            ;;
        -j|--jobs)
            (($# >= 2)) || die "$1 requires a positive integer"
            [[ "$2" =~ ^[1-9][0-9]*$ ]] ||
                die "$1 requires a positive integer"
            jobs="$2"
            shift 2
            ;;
        --no-tests)
            run_tests=0
            shift
            ;;
        --no-tools)
            cmake_args+=("-DPSVR2_BUILD_TOOLS=OFF")
            shift
            ;;
        --tools)
            cmake_args+=("-DPSVR2_BUILD_TOOLS=ON")
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            cmake_args+=("$@")
            break
            ;;
        -*)
            die "unknown option: $1"
            ;;
        *)
            die "unknown profile or argument: $1"
            ;;
    esac
done

command -v cmake >/dev/null 2>&1 || die "CMake 3.20 or newer is required"
command -v ctest >/dev/null 2>&1 || die "CTest is required"

build_dir="${script_dir}/.local/build-${profile}"

if ((clean_build)); then
    case "$build_dir" in
        "${script_dir}"/.local/build-release|\
        "${script_dir}"/.local/build-debug|\
        "${script_dir}"/.local/build-sanitize|\
        "${script_dir}"/.local/build-ci)
            cmake -E remove_directory "$build_dir"
            ;;
        *)
            die "refusing to clean unexpected path: $build_dir"
            ;;
    esac
fi

printf 'Configuring %s build in %s\n' "$profile" "$build_dir"
cmake --preset "$profile" -S "$script_dir" "${cmake_args[@]}"

build_command=(cmake --build "$build_dir")
if [[ -n "$jobs" ]]; then
    build_command+=(--parallel "$jobs")
fi
"${build_command[@]}"

if ((run_tests)); then
    test_command=(
        ctest
        --test-dir "$build_dir"
        --output-on-failure
    )
    if [[ -n "$jobs" ]]; then
        test_command+=(--parallel "$jobs")
    fi
    "${test_command[@]}"
fi

printf 'Build complete: %s\n' "$build_dir"
