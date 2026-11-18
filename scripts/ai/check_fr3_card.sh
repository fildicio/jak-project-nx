#!/bin/sh
# Validate every .fr3 on the Switch card: strip the 8-byte uncompressed-size
# prefix, then test the zstd frame with `zstd -t`.
# Prints FAIL lines for any file whose frame is bad.
DIR="${1:-/Volumes/SWITCH SD/switch/jak2/data/out/jak2/fr3}"
fail=0
for f in "$DIR"/*.fr3; do
  if ! tail -c +9 "$f" | zstd -t -q; then
    echo "FAIL: $f"
    fail=$((fail + 1))
  fi
done
echo "checked $(ls "$DIR"/*.fr3 | wc -l) files, $fail failures"
