#!/bin/sh
# Přeloží výstup backendu skutečnými assemblery všech cílů.
# Použití: tests/assemble.sh BACKEND soubor.lir ...
# Potřebuje clang (aarch64, riscv64, Apple) a mingw-w64 binutils.
set -u
be=$1; shift
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
status=0
for t in arm64 arm64_apple amd64_apple rv64 amd64_win amd64_sysv; do
  case $t in
    arm64) as="clang --target=aarch64-linux-gnu -c";;
    arm64_apple) as="clang --target=arm64-apple-macos13 -c";;
    amd64_apple) as="clang --target=x86_64-apple-macos13 -c";;
    rv64) as="clang --target=riscv64-linux-gnu -march=rv64gc -c";;
    amd64_win) as="x86_64-w64-mingw32-as";;
    amd64_sysv) as="as --64";;
  esac
  gen=0; ok=0; refused=0; bad=0
  for f in "$@"; do
    if ! $be -t $t -o $OUT/x.s $f 2>/dev/null; then refused=$((refused+1)); continue; fi
    gen=$((gen+1))
    if $as -o $OUT/x.o $OUT/x.s 2>$OUT/as.err; then ok=$((ok+1)); else bad=$((bad+1)); status=1; echo "  $t: $f: $(head -c 200 $OUT/as.err)"; fi
  done
  echo "$t: generated=$gen assembled=$ok failed=$bad backend-refused=$refused"
done
exit $status
