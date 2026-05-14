#!/usr/bin/env bash
# Build and run larch2 inside Docker.
#
# Usage:
#   ./docker-build.sh              # configure + build
#   ./docker-build.sh test         # configure + build + run tests
#   ./docker-build.sh shell        # drop into a shell inside the container
#
# The source tree is mounted read-write at /src.  Build artifacts go to
# /src/build-docker (gitignored via the build*/ pattern).

set -euo pipefail

IMAGE="larch2-dev"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Build the Docker image if it doesn't exist yet (or pass --rebuild)
if [[ "${1:-}" == "--rebuild" ]]; then
    shift
    docker build -t "$IMAGE" "$SCRIPT_DIR"
elif ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "==> Building Docker image '$IMAGE' (this takes a while the first time)..."
    docker build -t "$IMAGE" "$SCRIPT_DIR"
fi

run_docker() {
    docker run --rm -it \
        -v "$SCRIPT_DIR":/src \
        -w /src \
        "$IMAGE" \
        "$@"
}

CMD="${1:-build}"

case "$CMD" in
    build)
        run_docker bash -c '
            cmake -B build-docker -DGCC_TOOLCHAIN=/opt/gcc-trunk \
                  -DCMAKE_BUILD_TYPE=Debug \
            && cmake --build build-docker -j"$(nproc)"
        '
        ;;
    test)
        run_docker bash -c '
            cmake -B build-docker -DGCC_TOOLCHAIN=/opt/gcc-trunk \
                  -DCMAKE_BUILD_TYPE=Debug \
            && cmake --build build-docker -j"$(nproc)" \
            && ctest --test-dir build-docker --output-on-failure
        '
        ;;
    shell)
        run_docker bash
        ;;
    *)
        echo "Usage: $0 [--rebuild] [build|test|shell]" >&2
        exit 1
        ;;
esac
