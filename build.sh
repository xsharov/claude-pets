#!/usr/bin/env bash
set -euo pipefail

configuration="Release"
version=""
clean=0
universal=0

usage() {
    cat <<'EOF'
Usage: ./build.sh [--configuration Release|Debug|RelWithDebInfo] [--version 1.2.3] [--clean] [--universal]

Builds build/<configuration>/ClaudePets.app on macOS.
  --configuration  CMake build type, Release by default.
  --version        Version embedded into the app; the latest v* tag by default.
  --clean          Remove the build directory first.
  --universal      Build for both arm64 and x86_64.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    -c | --configuration)
        configuration="${2:?--configuration needs a value}"
        shift 2
        ;;
    --version)
        version="${2:?--version needs a value}"
        shift 2
        ;;
    --clean)
        clean=1
        shift
        ;;
    --universal)
        universal=1
        shift
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        echo "Unknown option: $1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

case "$configuration" in
Release | Debug | RelWithDebInfo) ;;
*)
    echo "Configuration must be Release, Debug or RelWithDebInfo, got '$configuration'." >&2
    exit 2
    ;;
esac

if [[ -n "$version" && ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Version must look like 1.2.3, got '$version'." >&2
    exit 2
fi

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "build.sh builds the macOS app; use build.ps1 on Windows." >&2
    exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
    echo "CMake was not found. Install it with 'brew install cmake' or from cmake.org." >&2
    exit 1
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$root/build/$(printf '%s' "$configuration" | tr '[:upper:]' '[:lower:]')"

if [[ $clean -eq 1 ]]; then
    rm -rf "$build_dir"
fi

generator=()
if command -v ninja >/dev/null 2>&1; then
    generator=(-G Ninja)
fi

architectures=()
if [[ $universal -eq 1 ]]; then
    architectures=("-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64")
fi

cmake -S "$root" -B "$build_dir" ${generator[@]+"${generator[@]}"} -DCMAKE_BUILD_TYPE="$configuration" -DCLAUDE_PETS_VERSION="$version" ${architectures[@]+"${architectures[@]}"}
cmake --build "$build_dir" --parallel

echo "Built $build_dir/ClaudePets.app"
