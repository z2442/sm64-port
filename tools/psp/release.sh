#!/bin/sh
# Builds the "no PC needed" release of one version (or all of them): the normal build first, then the ROM-free package.
# It only calls the existing build and tools/psp/make_release.py; the normal build is not changed in any way.
#
#   sh tools/psp/release.sh us          (us, jp, eu, sh or all)
#
# In Docker, the same way the normal build is run:
#   docker run --rm -v "${PWD}:/sm64" -w /sm64 sm64psp sh tools/psp/release.sh us
#
# Result: dist/psp_release/<version>/PSP/GAME/SM64_<VERSION>/   (copy the PSP folder to the Memory Stick)
#
# Environment variables:
#   JOBS=8        parallel jobs of the normal build
#   FAST=1        skip the (slow) check that no ROM data is left in the template; --verify always runs
#   SKIP_BUILD=1  do not rebuild; use the existing build/<version>_psp (for repeating just the packaging)
set -e

if [ -z "$1" ]; then
    echo "usage: sh tools/psp/release.sh us|jp|eu|sh|all" >&2
    exit 2
fi
if [ "$1" = "all" ]; then
    versions="us jp eu sh"
else
    versions="$1"
fi

for v in $versions; do
    case "$v" in
        us|jp|eu|sh) ;;
        *) echo "unknown version: $v (use us, jp, eu, sh or all)" >&2; exit 2 ;;
    esac
done

for v in $versions; do
    echo "=== $v ==="
    if [ -z "$SKIP_BUILD" ]; then
        make TARGET_PSP=1 pbp VERSION="$v" -j"${JOBS:-8}"
    fi
    if [ -n "$FAST" ]; then
        python3 tools/psp/make_release.py --version "$v" --verify --pack
    else
        python3 tools/psp/make_release.py --version "$v" --verify --leak-check --pack
    fi
done
echo "Done. Packages are in dist/psp_release/<version>/PSP/GAME/"
