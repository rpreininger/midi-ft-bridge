#!/bin/bash
#
# midi-ft-bridge Deployment Script
#
# Usage: ./deploy.sh [user@host] [options]
#
# Examples:
#   ./deploy.sh pi@192.168.4.1              # Deploy to Pi Zero AP
#   ./deploy.sh pi@192.168.4.1 --install    # Deploy and install service
#   ./deploy.sh pi@192.168.4.1 --restart    # Deploy and restart service
#

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

REMOTE_DIR="/home/pi/midi-ft-bridge"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build-arm"
BINARY="midi_ft_bridge"
CONFIG_FILE="${PROJECT_DIR}/config.json"
CLIPS_DIR="${PROJECT_DIR}/clips"
SERVICE_FILE="${PROJECT_DIR}/midi-ft-bridge.service"

TARGET=""
INSTALL_SERVICE=false
RESTART_SERVICE=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --install)  INSTALL_SERVICE=true; shift ;;
        --restart)  RESTART_SERVICE=true; shift ;;
        --help|-h)
            echo -e "${BLUE}midi-ft-bridge Deployment Script${NC}"
            echo ""
            echo "Usage: $0 [user@host] [options]"
            echo ""
            echo "Options:"
            echo "  --install    Install and enable systemd service"
            echo "  --restart    Restart the service after deployment"
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
    echo "Usage: $0 user@host [--install] [--restart]"
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
    echo "Build first: cd build-arm && cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64-toolchain.cmake .. && make"
    exit 1
fi

# Test SSH
log_info "Testing SSH to ${TARGET}..."
if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "$TARGET" "echo ok" &>/dev/null; then
    log_error "Cannot connect to ${TARGET}"
    exit 1
fi
log_success "SSH connected"

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

# Deploy clips (mp4 and any other media)
if [[ -d "${CLIPS_DIR}" ]]; then
    CLIP_COUNT=$(find "${CLIPS_DIR}" -maxdepth 1 -type f \( -name "*.mp4" -o -name "*.avi" -o -name "*.mkv" -o -name "*.mov" \) 2>/dev/null | wc -l)
    if [[ "$CLIP_COUNT" -gt 0 ]]; then
        log_info "Deploying ${CLIP_COUNT} video clips..."
        scp "${CLIPS_DIR}"/*.mp4 "${CLIPS_DIR}"/*.avi "${CLIPS_DIR}"/*.mkv "${CLIPS_DIR}"/*.mov "${TARGET}:${REMOTE_DIR}/clips/" 2>/dev/null
        log_success "Clips deployed"
    else
        log_warn "No video clips found in ${CLIPS_DIR}/"
    fi
fi

# Install service
if [[ "$INSTALL_SERVICE" == true ]]; then
    log_info "Installing systemd service..."
    scp "${SERVICE_FILE}" "${TARGET}:/tmp/midi-ft-bridge.service"
    ssh "$TARGET" "sudo mv /tmp/midi-ft-bridge.service /etc/systemd/system/ && \
                   sudo systemctl daemon-reload && \
                   sudo systemctl enable midi-ft-bridge"
    log_success "Service installed"
fi

# Restart
if [[ "$RESTART_SERVICE" == true ]] || [[ "$INSTALL_SERVICE" == true ]]; then
    log_info "Restarting service..."
    ssh "$TARGET" "sudo systemctl restart midi-ft-bridge" || true
    sleep 2
    if ssh "$TARGET" "sudo systemctl is-active --quiet midi-ft-bridge"; then
        log_success "Service running"
    else
        log_warn "Service may not have started. Check: ssh ${TARGET} 'sudo journalctl -u midi-ft-bridge -n 20'"
    fi
fi

echo ""
echo -e "${GREEN}=== Deployment Complete ===${NC}"
echo "Target: ${TARGET}:${REMOTE_DIR}"
echo ""
echo "Run manually:"
echo "  ssh ${TARGET}"
echo "  cd ${REMOTE_DIR}"
echo "  sudo ./midi_ft_bridge --config config.json"
echo ""
PI_HOST=$(echo "$TARGET" | cut -d'@' -f2)
echo "Status page: http://${PI_HOST}:8080"
