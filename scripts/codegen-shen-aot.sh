#!/usr/bin/env bash
# Generate bootstrap KL (defuns only; datatype effects dropped) from .shen
# files via a fresh kernel boot. Analog of shen-rust scripts/codegen-shen-aot.sh.
#
# Usage: scripts/codegen-shen-aot.sh <out-kl-dir> <in1.shen> [in2.shen ...]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [ $# -lt 2 ]; then
  echo "Usage: scripts/codegen-shen-aot.sh <out-kl-dir> <in1.shen> [in2.shen ...]" >&2
  exit 2
fi

OUT_DIR="$1"
shift

if [ ! -x "$ROOT/bin/shen-c" ]; then
  echo "codegen-shen-aot: missing $ROOT/bin/shen-c" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

for input in "$@"; do
  if [ ! -f "$input" ]; then
    echo "codegen-shen-aot: missing $input" >&2
    exit 1
  fi
  abs="$(cd "$(dirname "$input")" && pwd)/$(basename "$input")"
  dir="$(dirname "$abs")"
  base="$(basename "$abs")"
  kl_name="${base%.shen}.kl"
  (
    cd "$dir"
    env SHEN_C_HOME="$ROOT" "$ROOT/bin/shen-c" eval -q -e "(bootstrap \"$base\")"
  )
  if [ ! -f "$dir/$kl_name" ]; then
    echo "codegen-shen-aot: bootstrap did not write $dir/$kl_name" >&2
    exit 1
  fi
  mv "$dir/$kl_name" "$OUT_DIR/$kl_name"
  echo "codegen-shen-aot: $input -> $OUT_DIR/$kl_name"
done
