#!/bin/bash
# SwitchRecomp - Copyright 2026 MZ Labs
# Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
# runs every test program with one runtime and saves its last frame as TAG.ppm in the test's folder
# usage: tests/regress/regrun.sh path/to/runtime_linux_x11 TAG [tests folder, default tests/regress/programs]
# each test gets a fresh Xvfb, a shared one kept dying after a few runs
set -u
RT=$(realpath "${1:?runtime required}")
TAG=${2:?tag required}
ROOT=${3:-$(dirname "$0")/programs}
[[ "$TAG" =~ ^[a-zA-Z0-9_-]+$ ]] || { echo "invalid tag: $TAG" >&2; exit 1; }
[ -f "$RT" ] && [ -x "$RT" ] || { echo "no runtime at $1" >&2; exit 1; }

shopt -s nullglob
samples=0
failures=0
for d in "$ROOT"/*/; do
  b=$(basename "$d")
  [ -f "$d/${b}_code.so" ] || continue
  samples=$((samples+1))
  (
    cd "$d" &&
    rm -f -- "$TAG.ppm" &&
    cp -- "$RT" "$b" &&
    rm -rf -- shader_cache &&
    timeout -k 2s 40s xvfb-run -a -s '-screen 0 1920x1080x24' "./$b" --seconds=3 --shot="$TAG"
  ) >"$d/$TAG.out" 2>&1
  rc=$?
  frames=$(grep -E '^frames=[1-9][0-9]* hash=' "$d/$TAG.out" | tail -1)
  printf '%-24s exit=%s %s\n' "$b" "$rc" "$frames"
  printf '%s\n' "$rc" > "$d/$TAG.exit"
  if [ "$rc" -ne 0 ] || [ -z "$frames" ] || [ ! -s "$d/$TAG.ppm" ] ||
     grep -qE '^FATAL:|^\*\*\* Crash' "$d/$TAG.out"; then
    echo "$b: no successful capture; see $d/$TAG.out" >&2
    failures=$((failures+1))
  fi
done
printf '%s samples, %s failed captures\n' "$samples" "$failures"
[ "$samples" -gt 0 ] && [ "$failures" -eq 0 ]
