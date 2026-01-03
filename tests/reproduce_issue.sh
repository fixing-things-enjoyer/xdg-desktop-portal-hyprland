#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
OUTPUT_DIR="${SCRIPT_DIR}/output"
XDPH_BINARY="${PROJECT_ROOT}/build/src/xdg-desktop-portal-hyprland"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

cleanup() {
    echo -e "\n${YELLOW}[*] Cleaning up...${NC}"
    [[ -n "${HYPRLAND_PID:-}" ]] && kill "$HYPRLAND_PID" 2>/dev/null || true
    [[ -n "${XDPH_PID:-}" ]] && kill "$XDPH_PID" 2>/dev/null || true
    [[ -n "${DBUS_SESSION_BUS_PID:-}" ]] && kill "$DBUS_SESSION_BUS_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

check_dependencies() {
    local missing=()
    
    command -v Hyprland >/dev/null 2>&1 || missing+=("Hyprland")
    command -v uv >/dev/null 2>&1 || missing+=("uv (Python package manager)")
    command -v gst-launch-1.0 >/dev/null 2>&1 || missing+=("gstreamer (gst-launch-1.0)")
    command -v dbus-daemon >/dev/null 2>&1 || missing+=("dbus-daemon")
    
    if [[ ${#missing[@]} -gt 0 ]]; then
        echo -e "${RED}[ERROR] Missing dependencies: ${missing[*]}${NC}"
        exit 2
    fi
    
    if [[ ! -x "$XDPH_BINARY" ]]; then
        echo -e "${RED}[ERROR] XDPH binary not found at $XDPH_BINARY${NC}"
        echo "Build the project first: meson setup build && ninja -C build"
        exit 2
    fi
    
    echo -e "${YELLOW}[*] Syncing Python dependencies...${NC}"
    (cd "$SCRIPT_DIR" && uv sync --quiet)
}

start_dbus_session() {
    echo -e "${YELLOW}[*] Starting D-Bus session...${NC}"
    eval "$(dbus-launch --sh-syntax)"
    export DBUS_SESSION_BUS_ADDRESS
    export DBUS_SESSION_BUS_PID
    echo "    D-Bus address: $DBUS_SESSION_BUS_ADDRESS"
}

start_nested_hyprland() {
    echo -e "${YELLOW}[*] Starting nested Hyprland with 90° rotation...${NC}"
    
    export WLR_BACKENDS=headless
    export WLR_RENDERER=pixman
    export HYPRLAND_INSTANCE_SIGNATURE="xdph_test_$$"
    
    Hyprland -c "${SCRIPT_DIR}/hyprland_rotated.conf" &
    HYPRLAND_PID=$!
    
    sleep 2
    
    if ! kill -0 "$HYPRLAND_PID" 2>/dev/null; then
        echo -e "${RED}[ERROR] Hyprland failed to start${NC}"
        exit 2
    fi
    
    export WAYLAND_DISPLAY="${XDG_RUNTIME_DIR}/hypr/${HYPRLAND_INSTANCE_SIGNATURE}/.socket.wayland"
    
    if [[ ! -S "$WAYLAND_DISPLAY" ]]; then
        for socket in "${XDG_RUNTIME_DIR}"/wayland-*; do
            if [[ -S "$socket" ]]; then
                export WAYLAND_DISPLAY="$socket"
                break
            fi
        done
    fi
    
    echo "    Hyprland PID: $HYPRLAND_PID"
    echo "    WAYLAND_DISPLAY: $WAYLAND_DISPLAY"
}

start_portal() {
    echo -e "${YELLOW}[*] Starting xdg-desktop-portal-hyprland...${NC}"
    
    export XDPH_LOGLEVEL=trace
    
    mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/hypr"
    cat > "${XDG_CONFIG_HOME:-$HOME/.config}/hypr/xdph.conf" <<EOF
screencopy:custom_picker_binary = ${SCRIPT_DIR}/auto_picker.sh
screencopy:allow_gpu_rotation = true
EOF
    
    "$XDPH_BINARY" > "${OUTPUT_DIR}/xdph.log" 2>&1 &
    XDPH_PID=$!
    
    sleep 1
    
    if ! kill -0 "$XDPH_PID" 2>/dev/null; then
        echo -e "${RED}[ERROR] XDPH failed to start${NC}"
        cat "${OUTPUT_DIR}/xdph.log"
        exit 2
    fi
    
    echo "    XDPH PID: $XDPH_PID"
}

run_test() {
    echo -e "${YELLOW}[*] Running portal client test...${NC}"
    
    cd "$SCRIPT_DIR"
    uv run python portal_client.py \
        --output-dir "$OUTPUT_DIR" \
        --expected-width 1080 \
        --expected-height 1920
    
    return $?
}

main() {
    echo "=============================================="
    echo " XDPH Rotation Test"
    echo " Monitor: 1920x1080 with transform=1 (90°)"
    echo " Expected output: 1080x1920 (portrait)"
    echo "=============================================="
    echo
    
    mkdir -p "$OUTPUT_DIR"
    
    check_dependencies
    start_dbus_session
    start_nested_hyprland
    start_portal
    
    echo
    if run_test; then
        echo -e "\n${GREEN}=============================================="
        echo " TEST PASSED"
        echo "==============================================${NC}"
        exit 0
    else
        EXIT_CODE=$?
        echo -e "\n${RED}=============================================="
        echo " TEST FAILED (exit code: $EXIT_CODE)"
        echo "==============================================${NC}"
        
        if [[ -f "${OUTPUT_DIR}/xdph.log" ]]; then
            echo -e "\n${YELLOW}[*] XDPH Log (last 50 lines):${NC}"
            tail -50 "${OUTPUT_DIR}/xdph.log"
        fi
        
        exit $EXIT_CODE
    fi
}

main "$@"
