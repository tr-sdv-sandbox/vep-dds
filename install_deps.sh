#!/bin/bash
#
# install_deps.sh - Install dependencies for VDR ecosystem PoC
#
# Tested on: Ubuntu 24.04
#

set -e

echo "=== VDR Ecosystem Dependencies Installer ==="
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
    exit 1
}

# Check if running on Ubuntu
if ! grep -q "Ubuntu" /etc/os-release 2>/dev/null; then
    warn "This script is designed for Ubuntu. Other distros may require modifications."
fi

# Check Ubuntu version
UBUNTU_VERSION=$(grep VERSION_ID /etc/os-release | cut -d'"' -f2)
info "Detected Ubuntu version: $UBUNTU_VERSION"

if [[ "$UBUNTU_VERSION" != "24.04" ]]; then
    warn "Script tested on Ubuntu 24.04. Your version ($UBUNTU_VERSION) may have different package versions."
fi

# Update package list
info "Updating package list..."
sudo apt-get update

# Core build tools
info "Installing core build tools..."
sudo apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    git

# Cyclone DDS
info "Installing Cyclone DDS..."
sudo apt-get install -y \
    cyclonedds-dev \
    cyclonedds-tools \
    libcycloneddsidl0t64 \
    libddsc0t64

# Google logging
info "Installing Google logging (glog)..."
sudo apt-get install -y \
    libgoogle-glog-dev

# YAML parsing
info "Installing yaml-cpp..."
sudo apt-get install -y \
    libyaml-cpp-dev

# Google Test for unit testing
info "Installing GoogleTest..."
sudo apt-get install -y \
    libgtest-dev

# Optional: nlohmann-json for JSON encoding (simulated MQTT payload)
info "Installing nlohmann-json..."
sudo apt-get install -y \
    nlohmann-json3-dev

# Verify installations
echo ""
info "Verifying installations..."

verify_package() {
    if dpkg -l | grep -q "^ii  $1 "; then
        echo -e "  ${GREEN}✓${NC} $1"
        return 0
    else
        echo -e "  ${RED}✗${NC} $1"
        return 1
    fi
}

echo ""
echo "Installed packages:"
verify_package "cyclonedds-dev"
verify_package "cyclonedds-tools"
verify_package "libgoogle-glog-dev"
verify_package "libyaml-cpp-dev"
verify_package "libgtest-dev"
verify_package "nlohmann-json3-dev"
verify_package "cmake"
verify_package "build-essential"

# Check Cyclone DDS version
echo ""
info "Cyclone DDS version:"
apt-cache show cyclonedds-dev | grep "^Version:" | head -1

# Check idlc (IDL compiler) is available
echo ""
info "Checking IDL compiler (idlc)..."
if command -v idlc &> /dev/null; then
    echo -e "  ${GREEN}✓${NC} idlc found at: $(which idlc)"
    idlc --version 2>/dev/null || true
else
    error "idlc not found. Cyclone DDS tools may not be installed correctly."
fi

# Check ddsperf (performance tool) is available
info "Checking DDS performance tool..."
if command -v ddsperf &> /dev/null; then
    echo -e "  ${GREEN}✓${NC} ddsperf found at: $(which ddsperf)"
else
    warn "ddsperf not found. Performance testing tool not available."
fi

# Print pkg-config info for Cyclone DDS
echo ""
info "Cyclone DDS pkg-config info:"
if pkg-config --exists cyclonedds; then
    echo "  Include path: $(pkg-config --cflags cyclonedds)"
    echo "  Library path: $(pkg-config --libs cyclonedds)"
else
    warn "pkg-config cannot find cyclonedds. CMake may need manual configuration."
fi

echo ""
echo "=== Installation Complete ==="
echo ""
echo "Next steps:"
echo "  1. Review SPECIFICATION.md for architecture details"
echo "  2. Run 'cmake -B build && cmake --build build' to build"
echo "  3. Test DDS with: ddsperf pub  (terminal 1)"
echo "                    ddsperf sub  (terminal 2)"
echo ""
