#!/bin/bash
set -e

SRC_DIR="/tmp/article-publisher"
BUILD_PATH="/tmp/article_publisher"
OUT_PATH="/usr/local/bin/article_publisher"

echo "[*] Compiling to $BUILD_PATH..."
g++ -std=c++17 -O2 -o "$BUILD_PATH" \
  "$SRC_DIR/article_publisher.cpp" \
  "$SRC_DIR/https_server.cpp" \
  -lsqlite3

echo "[*] Moving binary to $OUT_PATH..."
sudo mv "$BUILD_PATH" "$OUT_PATH"
sudo chmod +x "$OUT_PATH"

echo "[+] Build complete and deployed to $OUT_PATH"
