#!/usr/bin/env bash
# =============================================================================
# WSL2 (Ubuntu/Debian) Emscripten + Host Build Toolchain Setup for pokemonweb
# Installs:
#   - build-essential, git, python3, pkg-config, libpng-dev, zlib1g-dev,
#     libsdl2-dev, clang, lld, cmake, binutils-arm-none-eabi
#   - emsdk (https://github.com/emscripten-core/emsdk.git) into ~/.emsdk
#   - Optional: engine/tools native utilities (--build-tools) and gcc-arm-none-eabi
# Usage:
#   bash tools/setup_wsl_toolchain.sh [options]
# Options:
#   --check            Audit installed toolchain without modifying the system
#   --with-gba-gcc     Also install gcc-arm-none-eabi for upstream GBA ROM builds
#   --build-tools      Also compile native host tools in engine/tools/
#   --emsdk-dir DIR    Install location for emsdk (default: $HOME/.emsdk)
#   --emsdk-ver VER    Emscripten SDK version tag (default: latest)
#   -h, --help         Show this help message
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ENGINE_DIR="$REPO_ROOT/engine"

EMSDK_DIR="${EMSDK_DIR:-$HOME/.emsdk}"
EMSDK_VERSION="${EMSDK_VERSION:-latest}"
CHECK_ONLY=0
WITH_GBA_GCC=0
BUILD_TOOLS=0

usage() {
    sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --check)
            CHECK_ONLY=1
            shift
            ;;
        --with-gba-gcc)
            WITH_GBA_GCC=1
            shift
            ;;
        --build-tools)
            BUILD_TOOLS=1
            shift
            ;;
        --emsdk-dir)
            EMSDK_DIR="${2:?--emsdk-dir requires a path}"
            shift 2
            ;;
        --emsdk-ver)
            EMSDK_VERSION="${2:?--emsdk-ver requires a version}"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "[setup_wsl_toolchain] Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

log()  { printf '[setup_wsl_toolchain] %s\n' "$*"; }
warn() { printf '[setup_wsl_toolchain] WARN: %s\n' "$*" >&2; }
err()  { printf '[setup_wsl_toolchain] ERROR: %s\n' "$*" >&2; }

if [[ "$(uname -s)" != "Linux" ]]; then
    err "This script must run inside WSL2 Ubuntu/Debian Linux (detected: $(uname -s))."
    exit 1
fi

if ! grep -qiE 'microsoft|wsl' /proc/version 2>/dev/null; then
    warn "Not running on a Microsoft/WSL kernel (/proc/version); proceeding as standard Linux."
fi

APT_PACKAGES=(
    build-essential
    git
    python3
    python3-pip
    curl
    ca-certificates
    xz-utils
    bzip2
    unzip
    pkg-config
    libpng-dev
    zlib1g-dev
    libsdl2-dev
    clang
    lld
    cmake
    binutils-arm-none-eabi
)

if [[ "$WITH_GBA_GCC" -eq 1 ]]; then
    APT_PACKAGES+=(gcc-arm-none-eabi libnewlib-arm-none-eabi)
fi

check_apt_package() {
    local pkg="$1"
    dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q 'install ok installed'
}

audit_toolchain() {
    local missing=0

    log "Auditing apt packages..."
    if command -v dpkg-query >/dev/null 2>&1; then
        for pkg in "${APT_PACKAGES[@]}"; do
            if check_apt_package "$pkg"; then
                printf '  [OK] apt: %s\n' "$pkg"
            else
                printf '  [MISSING] apt: %s\n' "$pkg"
                missing=1
            fi
        done
    else
        warn "dpkg-query not found; checking executables directly."
    fi

    if [[ -f "$EMSDK_DIR/emsdk_env.sh" ]]; then
        # shellcheck source=/dev/null
        EMSDK_QUIET=1 source "$EMSDK_DIR/emsdk_env.sh"
        printf '  [OK] emsdk_env.sh: %s\n' "$EMSDK_DIR/emsdk_env.sh"
    else
        printf '  [MISSING] emsdk: %s\n' "$EMSDK_DIR/emsdk_env.sh"
        missing=1
    fi

    for cmd in gcc g++ make python3 git pkg-config arm-none-eabi-as emcc em++ emar wasm-ld; do
        if command -v "$cmd" >/dev/null 2>&1; then
            local ver
            ver="$("$cmd" --version 2>/dev/null | head -n 1 || echo "installed")"
            printf '  [OK] %-18s -> %s (%s)\n' "$cmd" "$(command -v "$cmd")" "$ver"
        else
            printf '  [MISSING] %-18s\n' "$cmd"
            missing=1
        fi
    done

    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libpng 2>/dev/null; then
        printf '  [OK] pkg-config libpng -> %s\n' "$(pkg-config --modversion libpng)"
    else
        printf '  [MISSING] pkg-config libpng\n'
        missing=1
    fi

    if command -v sdl2-config >/dev/null 2>&1; then
        printf '  [OK] sdl2-config        -> %s\n' "$(sdl2-config --version)"
    else
        printf '  [MISSING] sdl2-config\n'
        missing=1
    fi

    return "$missing"
}

if [[ "$CHECK_ONLY" -eq 1 ]]; then
    log "Running in --check mode (no changes will be made)."
    if audit_toolchain; then
        log "All required WSL2 WASM toolchain components are installed."
        exit 0
    else
        err "One or more toolchain components are missing. Run: bash tools/setup_wsl_toolchain.sh"
        exit 1
    fi
fi

# Determine missing apt packages before requesting sudo
MISSING_PKGS=()
if command -v dpkg-query >/dev/null 2>&1; then
    for pkg in "${APT_PACKAGES[@]}"; do
        if ! check_apt_package "$pkg"; then
            MISSING_PKGS+=("$pkg")
        fi
    done
else
    err "apt/dpkg-query required (Ubuntu/Debian WSL2 distro expected)."
    exit 1
fi

if [[ ${#MISSING_PKGS[@]} -gt 0 ]]; then
    log "Missing apt packages (${#MISSING_PKGS[@]}): ${MISSING_PKGS[*]}"
    if [[ "${EUID:-$(id -u)}" -eq 0 ]]; then
        apt-get update
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${MISSING_PKGS[@]}"
    elif sudo -n true 2>/dev/null; then
        sudo apt-get update
        sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${MISSING_PKGS[@]}"
    elif [[ -t 0 ]]; then
        log "Requesting sudo password to install required Ubuntu packages..."
        sudo apt-get update
        sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${MISSING_PKGS[@]}"
    else
        err "sudo requires a password and no interactive TTY is attached."
        err "Run manually inside WSL2:"
        printf '  sudo apt-get update && sudo apt-get install -y %s\n' "${MISSING_PKGS[*]}" >&2
        exit 3
    fi
else
    log "All required apt packages are already installed."
fi

# Install or update emsdk inside native Linux filesystem ($HOME/.emsdk) for fast caching
if [[ ! -x "$EMSDK_DIR/emsdk" ]]; then
    log "Cloning emsdk into $EMSDK_DIR..."
    mkdir -p "$(dirname -- "$EMSDK_DIR")"
    git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
else
    log "Found existing emsdk in $EMSDK_DIR."
fi

pushd "$EMSDK_DIR" >/dev/null
if [[ ! -f "$EMSDK_DIR/emsdk_env.sh" ]] || ! grep -q "$EMSDK_VERSION" "$EMSDK_DIR/.emscripten" 2>/dev/null; then
    log "Installing and activating emsdk ($EMSDK_VERSION)..."
    ./emsdk install "$EMSDK_VERSION"
    ./emsdk activate "$EMSDK_VERSION"
else
    log "Ensuring emsdk ($EMSDK_VERSION) is activated..."
    ./emsdk activate "$EMSDK_VERSION" >/dev/null
fi
popd >/dev/null

# shellcheck source=/dev/null
source "$EMSDK_DIR/emsdk_env.sh"

# Add shell persistence helper note
SHELL_RC="$HOME/.bashrc"
if [[ -f "$SHELL_RC" ]] && ! grep -q 'emsdk_env.sh' "$SHELL_RC" 2>/dev/null; then
    log "Tip: append 'EMSDK_QUIET=1 source \"$EMSDK_DIR/emsdk_env.sh\"' to ~/.bashrc if you want emcc on interactive shells."
fi

if [[ "$BUILD_TOOLS" -eq 1 ]]; then
    if [[ -d "$ENGINE_DIR/tools" ]]; then
        log "Compiling native host asset/script tools in $ENGINE_DIR..."
        make -C "$ENGINE_DIR" -f make_tools.mk tools -j"$(nproc)"
    else
        warn "Engine directory not found at $ENGINE_DIR; skipping --build-tools."
    fi
fi

log "Final toolchain audit:"
audit_toolchain
log "WSL2 WASM toolchain setup complete."
