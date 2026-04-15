#!/bin/bash
#
# midi-ft-bridge Deployment Script
#
# Usage: ./deploy.sh [user@host] [options]
#
# Examples:
#   ./deploy.sh stratojets@192.168.10.1              # Deploy binary + config
#   ./deploy.sh stratojets@192.168.10.1 --install     # Deploy and install all services
#   ./deploy.sh stratojets@192.168.10.1 --restart     # Deploy and restart services
#   ./deploy.sh stratojets@192.168.10.1 --setup       # Full first-time setup (packages, venv, services)
#

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

REMOTE_DIR="/home/stratojets/midi-ft-bridge"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build-aarch64"
BINARY="midi_ft_bridge"
CONFIG_FILE="${PROJECT_DIR}/config.json"
CLIPS_DIR="${PROJECT_DIR}/clips"
SERVICE_FILE="${PROJECT_DIR}/midi-ft-bridge.service"
BT_BRIDGE_SERVICE="${PROJECT_DIR}/bt-bridge.service"
BT_BRIDGE_PY="${PROJECT_DIR}/bt_bridge.py"

TARGET=""
INSTALL_SERVICE=false
RESTART_SERVICE=false
FULL_SETUP=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --install)  INSTALL_SERVICE=true; shift ;;
        --restart)  RESTART_SERVICE=true; shift ;;
        --setup)    FULL_SETUP=true; INSTALL_SERVICE=true; shift ;;
        --help|-h)
            echo -e "${BLUE}midi-ft-bridge Deployment Script${NC}"
            echo ""
            echo "Usage: $0 [user@host] [options]"
            echo ""
            echo "Options:"
            echo "  --install    Install and enable systemd services (midi-ft-bridge + bt-bridge)"
            echo "  --restart    Restart services after deployment"
            echo "  --setup      Full first-time setup (apt packages, Python venv, services)"
            echo "  --help       Show this help"
            exit 0
            ;;
        *)
            if [[ -z "$TARGET" ]]; then TARGET="$1"; fi
            shift
            ;;
    esac
done

if [[ -z "$TARGET" ]]; then
    echo "Usage: $0 user@host [--install] [--restart] [--setup]"
    exit 1
fi

log_info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[OK]${NC} $1"; }
log_warn()    { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error()   { echo -e "${RED}[ERROR]${NC} $1"; }

# Check binary
BINARY_PATH="${BUILD_DIR}/${BINARY}"
if [[ ! -f "${BINARY_PATH}" ]]; then
    log_error "Binary not found: ${BINARY_PATH}"
    echo "Build first: cd build-aarch64 && cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64-toolchain.cmake .. && make"
    exit 1
fi

# Test SSH
log_info "Testing SSH to ${TARGET}..."
if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "$TARGET" "echo ok" &>/dev/null; then
    log_error "Cannot connect to ${TARGET}"
    log_info "If first time, run: ssh-copy-id ${TARGET}"
    exit 1
fi
log_success "SSH connected"

# Full first-time setup: install packages and create Python venv
if [[ "$FULL_SETUP" == true ]]; then
    log_info "Installing system packages..."
    ssh "$TARGET" "sudo apt update && sudo apt install -y \
        hostapd dnsmasq \
        ffmpeg libasound2-dev libavformat-dev libavcodec-dev libswscale-dev libavutil-dev \
        libdbus-1-dev \
        python3-full python3-venv \
        bluetooth bluez"
    log_success "Packages installed"

    log_info "Creating Python venv for bt_bridge..."
    ssh "$TARGET" "sudo python3 -m venv /opt/bt-bridge-venv && \
                   sudo /opt/bt-bridge-venv/bin/pip install bleak Pillow"
    log_success "Python venv created"

    log_info "Enabling Bluetooth..."
    ssh "$TARGET" "sudo rfkill unblock bluetooth && \
                   sudo systemctl enable bluetooth && \
                   sudo systemctl start bluetooth"
    log_success "Bluetooth enabled"
fi

# Create dirs
log_info "Creating remote directories..."
ssh "$TARGET" "mkdir -p ${REMOTE_DIR}/clips"

# Deploy binary
log_info "Deploying binary..."
scp "${BINARY_PATH}" "${TARGET}:${REMOTE_DIR}/"
ssh "$TARGET" "chmod +x ${REMOTE_DIR}/${BINARY}"
log_success "Binary deployed"

# Deploy config
if [[ -f "${CONFIG_FILE}" ]]; then
    log_info "Deploying config.json..."
    scp "${CONFIG_FILE}" "${TARGET}:${REMOTE_DIR}/"
    log_success "Config deployed"
fi

# Deploy bt_bridge.py
if [[ -f "${BT_BRIDGE_PY}" ]]; then
    log_info "Deploying bt_bridge.py..."
    scp "${BT_BRIDGE_PY}" "${TARGET}:${REMOTE_DIR}/"
    log_success "bt_bridge.py deployed"
fi

# Deploy clips (mp4 and any other media)
if [[ -d "${CLIPS_DIR}" ]]; then
    CLIP_COUNT=$(find "${CLIPS_DIR}" -maxdepth 1 -type f \( -name "*.mp4" -o -name "*.avi" -o -name "*.mkv" -o -name "*.mov" \) 2>/dev/null | wc -l)
    if [[ "$CLIP_COUNT" -gt 0 ]]; then
        log_info "Deploying ${CLIP_COUNT} video clips..."
        scp "${CLIPS_DIR}"/*.mp4 "${TARGET}:${REMOTE_DIR}/clips/" 2>/dev/null || true
        scp "${CLIPS_DIR}"/*.avi "${TARGET}:${REMOTE_DIR}/clips/" 2>/dev/null || true
        scp "${CLIPS_DIR}"/*.mkv "${TARGET}:${REMOTE_DIR}/clips/" 2>/dev/null || true
        scp "${CLIPS_DIR}"/*.mov "${TARGET}:${REMOTE_DIR}/clips/" 2>/dev/null || true
        log_success "Clips deployed"
    else
        log_warn "No video clips found in ${CLIPS_DIR}/"
    fi
fi

# Install services
if [[ "$INSTALL_SERVICE" == true ]]; then
    log_info "Installing midi-ft-bridge service..."
    scp "${SERVICE_FILE}" "${TARGET}:/tmp/midi-ft-bridge.service"
    ssh "$TARGET" "sudo mv /tmp/midi-ft-bridge.service /etc/systemd/system/ && \
                   sudo systemctl daemon-reload && \
                   sudo systemctl enable midi-ft-bridge"
    log_success "midi-ft-bridge service installed"

    if [[ -f "${BT_BRIDGE_SERVICE}" ]]; then
        log_info "Installing bt-bridge service..."
        scp "${BT_BRIDGE_SERVICE}" "${TARGET}:/tmp/bt-bridge.service"
        ssh "$TARGET" "sudo mv /tmp/bt-bridge.service /etc/systemd/system/ && \
                       sudo systemctl daemon-reload && \
                       sudo systemctl enable bt-bridge"
        log_success "bt-bridge service installed"
    fi
fi

# Restart
if [[ "$RESTART_SERVICE" == true ]] || [[ "$INSTALL_SERVICE" == true ]]; then
    log_info "Restarting services..."
    ssh "$TARGET" "sudo systemctl restart midi-ft-bridge" || true
    ssh "$TARGET" "sudo systemctl restart bt-bridge" || true
    sleep 3
    if ssh "$TARGET" "sudo systemctl is-active --quiet midi-ft-bridge"; then
        log_success "midi-ft-bridge running"
    else
        log_warn "midi-ft-bridge may not have started. Check: ssh ${TARGET} 'sudo journalctl -u midi-ft-bridge -n 20'"
    fi
    if ssh "$TARGET" "sudo systemctl is-active --quiet bt-bridge"; then
        log_success "bt-bridge running"
    else
        log_warn "bt-bridge may not have started. Check: ssh ${TARGET} 'sudo journalctl -u bt-bridge -n 20'"
    fi
fi

echo ""
echo -e "${GREEN}=== Deployment Complete ===${NC}"
echo "Target: ${TARGET}:${REMOTE_DIR}"
echo ""
echo "Run manually:"
echo "  ssh ${TARGET}"
echo "  cd ${REMOTE_DIR}"
echo "  sudo ./midi_ft_bridge --test --config config.json"
echo ""
PI_HOST=$(echo "$TARGET" | cut -d'@' -f2)
echo "Status page: http://${PI_HOST}:8080"
