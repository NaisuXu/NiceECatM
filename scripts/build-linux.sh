#!/usr/bin/env sh
# Copyright (C) 2026 Naisu Xu
# SPDX-License-Identifier: GPL-3.0-only

set -eu
cd "$(dirname "$0")/.."
clean=false
case "${1:-}" in
    '') ;;
    --clean) clean=true; shift ;;
    --help)
        printf 'Usage: %s [--clean]\nSet NICEECATM_JOBS to choose the parallel build count.\n' "$0"
        exit 0
        ;;
    *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2 ;;
esac
if [ "$#" -ne 0 ]; then
    printf 'Usage: %s [--clean]\n' "$0" >&2
    exit 2
fi
jobs="${NICEECATM_JOBS:-4}"
case "$jobs" in
    *[!0-9]*|0) printf 'NICEECATM_JOBS must be a positive integer.\n' >&2; exit 2 ;;
esac
if [ "$clean" = true ]; then
    # Refresh configuration and rebuild using the local third_party sources.
    cmake --fresh --preset linux-release
    cmake --build --preset linux-release --clean-first --parallel "$jobs"
else
    cmake --preset linux-release
    cmake --build --preset linux-release --parallel "$jobs"
fi
ctest --preset linux-release
# Preserve user scripts, ESI descriptions and preferences in the install tree.
cmake --install build/linux-release --prefix "dist/linux-$(uname -m)" --strip
