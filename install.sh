#!/bin/sh
set -e

# bonfire-pomodoro installer
REPO="GabrielBaiano/bonfire-pomodoro"
TARGET_DIR="${HOME}/.local/bin"

if [ "$(id -u)" -eq 0 ]; then
    TARGET_DIR="/usr/local/bin"
fi

mkdir -p "${TARGET_DIR}"

OS="$(uname -s)"
ARCH="$(uname -m)"

if [ "$OS" != "Linux" ]; then
    echo "bonfire-pomodoro currently targets Linux terminals. Detected OS: $OS"
    exit 1
fi

echo "Installing bonfire-pomodoro..."

INSTALLED=0
if [ "$ARCH" = "x86_64" ]; then
    TMP_DIR=$(mktemp -d)
    TAR_URL="https://github.com/${REPO}/releases/download/v1.0.0/bonfire-v1.0.0-linux-x86_64.tar.gz"
    if curl -sSL -f "$TAR_URL" -o "${TMP_DIR}/bonfire.tar.gz" 2>/dev/null; then
        tar -xzf "${TMP_DIR}/bonfire.tar.gz" -C "${TMP_DIR}"
        install -m 755 "${TMP_DIR}/bonfire" "${TARGET_DIR}/bonfire"
        ln -sf bonfire "${TARGET_DIR}/fireplace"
        rm -rf "${TMP_DIR}"
        INSTALLED=1
    fi
fi

if [ "$INSTALLED" -eq 0 ]; then
    if ! command -v make >/dev/null 2>&1 || ! command -v gcc >/dev/null 2>&1; then
        echo "Error: Pre-compiled binary unavailable for $ARCH and 'make'/'gcc' not found."
        echo "Please install build-essential / base-devel and try again."
        exit 1
    fi

    TMP_DIR=$(mktemp -d)
    echo "Compiling from source..."
    curl -sSL "https://github.com/${REPO}/archive/refs/tags/v1.0.0.tar.gz" | tar -xz -C "${TMP_DIR}"
    SRC_DIR=$(find "${TMP_DIR}" -maxdepth 1 -type d -name "bonfire-pomodoro*" | head -n 1)
    make -C "${SRC_DIR}"
    install -m 755 "${SRC_DIR}/bonfire" "${TARGET_DIR}/bonfire"
    ln -sf bonfire "${TARGET_DIR}/fireplace"
    rm -rf "${TMP_DIR}"
fi

echo "bonfire-pomodoro installed successfully to ${TARGET_DIR}/bonfire"

case ":$PATH:" in
    *":${TARGET_DIR}:"*) ;;
    *)
        echo ""
        echo "Note: ${TARGET_DIR} is not in your PATH."
        echo "Add it by adding this to your ~/.bashrc or ~/.zshrc:"
        echo "    export PATH=\"${TARGET_DIR}:\$PATH\""
        ;;
esac

echo ""
echo "Run 'bonfire' to begin your focus session."
