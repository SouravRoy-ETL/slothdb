#!/bin/bash
# Build .deb package for Debian/Ubuntu
# Usage: ./build-deb.sh

set -e

VERSION="0.1.2"
PKG_NAME="slothdb"
PKG_DIR="${PKG_NAME}_${VERSION}_amd64"

echo "Building SlothDB ${VERSION} .deb package..."

# Build binary
cmake -B build -DSLOTHDB_BUILD_SHELL=ON -DSLOTHDB_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Create package structure
rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR/DEBIAN"
mkdir -p "$PKG_DIR/usr/local/bin"
mkdir -p "$PKG_DIR/usr/share/doc/slothdb"

# Copy binary
cp build/src/slothdb "$PKG_DIR/usr/local/bin/"
strip "$PKG_DIR/usr/local/bin/slothdb"

# Copy docs
cp README.md "$PKG_DIR/usr/share/doc/slothdb/"
cp LICENSE "$PKG_DIR/usr/share/doc/slothdb/"

# Create control file
cat > "$PKG_DIR/DEBIAN/control" << EOF
Package: slothdb
Version: ${VERSION}
Section: database
Priority: optional
Architecture: amd64
Depends: libc6 (>= 2.31)
Maintainer: Sourav Roy <souravroy7864@gmail.com>
Description: An embedded analytical database engine
 SlothDB is a production-grade in-process OLAP database written in C++20.
 Zero dependencies. GPU accelerated. Query CSV, Parquet, JSON, Excel directly.
 .
 Features: 130+ SQL features, window functions, QUALIFY, CTEs,
 compression, parallel execution, stable extension API.
Homepage: https://github.com/SouravRoy-ETL/slothdb
EOF

# Build .deb
dpkg-deb --build "$PKG_DIR"

echo ""
echo "Package built: ${PKG_DIR}.deb"
echo "Install with: sudo dpkg -i ${PKG_DIR}.deb"
