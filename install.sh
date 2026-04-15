#!/bin/bash
# SlothDB installer — one-liner install for Linux and macOS
# Usage: curl -fsSL https://raw.githubusercontent.com/SouravRoy-ETL/slothdb/main/install.sh | bash

set -e

REPO="SouravRoy-ETL/slothdb"
VERSION="v0.1.1"
INSTALL_DIR="/usr/local/bin"

# Detect OS and architecture
OS=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=$(uname -m)

if [ "$OS" = "darwin" ]; then
    ASSET="slothdb-macos/slothdb"
    PLATFORM="macOS"
elif [ "$OS" = "linux" ]; then
    ASSET="slothdb-linux-x64/slothdb"
    PLATFORM="Linux"
else
    echo "Unsupported OS: $OS"
    exit 1
fi

echo "Installing SlothDB ${VERSION} for ${PLATFORM}..."

# Download from GitHub releases
DOWNLOAD_URL="https://github.com/${REPO}/releases/download/${VERSION}/slothdb"

TMP_FILE=$(mktemp)
if command -v curl &> /dev/null; then
    curl -fsSL "$DOWNLOAD_URL" -o "$TMP_FILE"
elif command -v wget &> /dev/null; then
    wget -q "$DOWNLOAD_URL" -O "$TMP_FILE"
else
    echo "Error: curl or wget required"
    exit 1
fi

# Install
chmod +x "$TMP_FILE"
if [ -w "$INSTALL_DIR" ]; then
    mv "$TMP_FILE" "$INSTALL_DIR/slothdb"
else
    echo "Need sudo to install to $INSTALL_DIR"
    sudo mv "$TMP_FILE" "$INSTALL_DIR/slothdb"
fi

echo ""
echo "SlothDB installed successfully!"
echo ""
echo "Usage:"
echo "  slothdb                        # in-memory database"
echo "  slothdb mydata.slothdb         # persistent database"
echo "  slothdb -c \"SELECT 42\"         # run a query"
echo ""
echo "Try it:"
echo "  slothdb"
echo "  slothdb> SELECT 'Hello World!' AS greeting;"
