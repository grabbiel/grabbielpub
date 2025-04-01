#!/bin/bash

set -e

SRC_DIR="/tmp/article-publisher"
OUT_PATH="/usr/local/bin/article_publisher"

echo "[*] Building article_publisher..."
g++ -std=c++17 -O2 -o "$OUT_PATH" \
  "$SRC_DIR/article_publisher.cpp" \
  "$SRC_DIR/https_server.cpp" \
  -lsqlite3
chmod +x "$OUT_PATH"
echo "[+] Build complete: $OUT_PATH"
