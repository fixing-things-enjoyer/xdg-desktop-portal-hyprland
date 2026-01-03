#!/usr/bin/env bash
# Mock picker for manual testing
# Selects the first rotated monitor found, or falls back to the first available monitor.

set -euo pipefail

# Find a monitor with transform != 0 (rotated)
ROTATED_MONITOR=$(hyprctl monitors -j | jq -r '.[] | select(.transform != 0) | .name' | head -n1)

if [[ -n "$ROTATED_MONITOR" ]]; then
    # Found a rotated monitor
    echo "[SELECTION]g/screen:${ROTATED_MONITOR}"
    exit 0
fi

# Fallback: First monitor
FIRST_MONITOR=$(hyprctl monitors -j | jq -r '.[0].name')
if [[ -n "$FIRST_MONITOR" ]]; then
    echo "[SELECTION]g/screen:${FIRST_MONITOR}"
    exit 0
fi

# Emergency fallback
echo "[SELECTION]g/screen:HEADLESS-1"
exit 0
