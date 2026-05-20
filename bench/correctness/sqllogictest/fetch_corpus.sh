#!/usr/bin/env bash
# Fetch Apache DataFusion's sqllogictest corpus into test_files/.
# Uses a sparse clone to avoid pulling the entire DataFusion repo.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$HERE/test_files"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
git clone --depth 1 --filter=blob:none --sparse https://github.com/apache/datafusion "$TMP" 2>&1 | tail -2
( cd "$TMP" && git sparse-checkout set datafusion/sqllogictest/test_files )
mkdir -p "$DEST"
cp "$TMP/datafusion/sqllogictest/test_files/"*.slt "$DEST/"
n=$(ls "$DEST"/*.slt 2>/dev/null | wc -l)
echo "fetched $n .slt files into $DEST"
