#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_ROOT}/build"
XDPH_BINARY="${BUILD_DIR}/src/xdg-desktop-portal-hyprland"
MOCK_PICKER="${SCRIPT_DIR}/mock_picker.sh"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

TEST_CONFIG_DIR=$(mktemp -d -t xdph_test_config_XXXXXX)
TEST_OUTPUT_DIR=$(mktemp -d -t xdph_test_output_XXXXXX)

cleanup() {
    echo -e "\n${YELLOW}[*] Cleaning up...${NC}"
    
    if [[ -n "${XDPH_PID:-}" ]]; then
        echo "Killing test portal (PID $XDPH_PID)..."
        kill "$XDPH_PID" 2>/dev/null || true
        wait "$XDPH_PID" 2>/dev/null || true
    fi

    rm -rf "$TEST_CONFIG_DIR"
    echo "Removed temp config: $TEST_CONFIG_DIR"
    echo "Output directory: $TEST_OUTPUT_DIR"

    echo -e "${YELLOW}[*] You may need to restart the system portal manually or let systemd do it.${NC}"
    echo "    systemctl --user restart xdg-desktop-portal-hyprland"
}
trap cleanup EXIT

if [[ ! -x "$XDPH_BINARY" ]]; then
    echo -e "${RED}[ERROR] Binary not found at $XDPH_BINARY${NC}"
    echo "Please build the project first."
    exit 1
fi

if ! command -v hyprctl &>/dev/null; then
    echo -e "${RED}[ERROR] hyprctl not found. Is Hyprland running?${NC}"
    exit 1
fi

echo -e "${YELLOW}[*] Setting up test environment...${NC}"

mkdir -p "${TEST_CONFIG_DIR}/hypr"

cat > "${TEST_CONFIG_DIR}/hypr/xdph.conf" <<EOF
screencopy:custom_picker_binary = $MOCK_PICKER
screencopy:allow_gpu_rotation = true
screencopy:max_fps = 60
EOF

echo "Generated config at ${TEST_CONFIG_DIR}/hypr/xdph.conf"

echo -e "${YELLOW}[*] Stopping existing xdg-desktop-portal-hyprland...${NC}"
killall xdg-desktop-portal-hyprland 2>/dev/null || true
sleep 1

echo -e "${YELLOW}[*] Starting test portal...${NC}"

export XDG_CONFIG_HOME="$TEST_CONFIG_DIR"
export XDPH_LOGLEVEL=trace
# Disable implicit Vulkan layers which might crash the portal in some environments
export VK_LOADER_DISABLE_IMPLICIT_LAYERS=1

# Use stdbuf to ensure we catch logs even if it crashes
stdbuf -oL -eL "$XDPH_BINARY" > "${TEST_OUTPUT_DIR}/xdph.log" 2>&1 &
XDPH_PID=$!

echo "Test portal running with PID $XDPH_PID"
sleep 2

if ! kill -0 "$XDPH_PID" 2>/dev/null; then
    echo -e "${RED}[ERROR] Test portal failed to start.${NC}"
    cat "${TEST_OUTPUT_DIR}/xdph.log"
    exit 1
fi

# Use -c for compact JSON line
ROTATED_MONITOR=$(hyprctl monitors -j | jq -c '.[] | select(.transform != 0)' | head -n1)

if [[ -z "$ROTATED_MONITOR" ]]; then
    echo -e "${YELLOW}[WARN] No rotated monitor found! Testing with primary monitor (expecting NO rotation).${NC}"
    MONITOR_JSON=$(hyprctl monitors -j | jq -c '.[0]')
else
    echo -e "${GREEN}[INFO] Found rotated monitor: $(echo "$ROTATED_MONITOR" | jq -r '.name')${NC}"
    MONITOR_JSON="$ROTATED_MONITOR"
fi

WIDTH=$(echo "$MONITOR_JSON" | jq -r '.width')
HEIGHT=$(echo "$MONITOR_JSON" | jq -r '.height')
TRANSFORM=$(echo "$MONITOR_JSON" | jq -r '.transform')
SCALE=$(echo "$MONITOR_JSON" | jq -r '.scale')

echo "Monitor Physical: ${WIDTH}x${HEIGHT} (Transform: $TRANSFORM, Scale: $SCALE)"

EXPECTED_W=$WIDTH
EXPECTED_H=$HEIGHT

if [[ "$TRANSFORM" == "1" || "$TRANSFORM" == "3" || "$TRANSFORM" == "5" || "$TRANSFORM" == "7" ]]; then
    EXPECTED_W=$HEIGHT
    EXPECTED_H=$WIDTH
    echo "Expecting SWAPPED dimensions: ${EXPECTED_W}x${EXPECTED_H}"
else
    echo "Expecting NORMAL dimensions: ${EXPECTED_W}x${EXPECTED_H}"
fi

echo -e "${YELLOW}[*] Running client to capture frame...${NC}"

cd "$SCRIPT_DIR"
uv run python portal_client.py \
    --output-dir "$TEST_OUTPUT_DIR" \
    --expected-width "$EXPECTED_W" \
    --expected-height "$EXPECTED_H"

CLIENT_EXIT=$?

if [[ $CLIENT_EXIT -eq 0 ]]; then
    echo -e "\n${GREEN}[SUCCESS] Test passed! Frame captured with correct dimensions.${NC}"
    echo "Captured frame: ${TEST_OUTPUT_DIR}/captured_frame.png"
    echo "Log file: ${TEST_OUTPUT_DIR}/xdph.log"
else
    echo -e "\n${RED}[FAIL] Test failed.${NC}"
    echo "Check logs at: ${TEST_OUTPUT_DIR}/xdph.log"
    tail -n 50 "${TEST_OUTPUT_DIR}/xdph.log"
fi

exit $CLIENT_EXIT
