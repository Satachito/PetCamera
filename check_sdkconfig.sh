#!/usr/bin/env bash
# Verify every option in sdkconfig.defaults actually landed in the generated
# sdkconfig.
#
# The ESP-IDF config system silently ignores unknown symbols in
# sdkconfig.defaults. When a managed component renames its Kconfig options
# between major versions — as esp_hosted did between 2.x and 3.x — a stale entry
# looks correct in the file and does nothing at all on the device. This catches
# that at build time instead of in a boot loop.
set -uo pipefail

DEFAULTS="${1:-sdkconfig.defaults}"
GENERATED="${2:-sdkconfig}"

if [ ! -f "$GENERATED" ]; then
    echo "error: $GENERATED not found — run 'idf.py reconfigure' first" >&2
    exit 2
fi

fails=0
while IFS= read -r line; do
    case "$line" in
        CONFIG_*=*) ;;
        *) continue ;;
    esac

    key=${line%%=*}
    want=${line#*=}

    if [ "$want" = "n" ]; then
        # A disabled bool is written as a comment, or omitted entirely.
        if grep -qx "$key=y" "$GENERATED"; then
            echo "MISMATCH  $key: wanted n, generated y"
            fails=$((fails + 1))
        fi
        continue
    fi

    got=$(grep -m1 "^$key=" "$GENERATED" | cut -d= -f2-)
    if [ -z "$got" ]; then
        echo "IGNORED   $key — not a known symbol (renamed or removed upstream?)"
        fails=$((fails + 1))
    elif [ "$got" != "$want" ]; then
        echo "MISMATCH  $key: wanted $want, generated $got"
        fails=$((fails + 1))
    fi
done < "$DEFAULTS"

if [ "$fails" -eq 0 ]; then
    echo "OK: every option in $DEFAULTS is present in $GENERATED"
else
    echo
    echo "$fails problem(s). An IGNORED symbol means the value never reached the"
    echo "build — look for a renamed Kconfig option in the relevant component."
fi
exit $([ "$fails" -eq 0 ] && echo 0 || echo 1)
