#!/bin/bash
set -e

SRC_DIR="/tmp/article-publisher"
BUILD_PATH="/tmp/article_publisher"
OUT_PATH="/usr/local/bin/article_publisher"

echo "[*] Checking gsutil installation..."
if ! command -v gsutil &>/dev/null; then
  echo "[!] gsutil not found, installing Google Cloud SDK..."
  # Add the Cloud SDK distribution URI as a package source
  echo "deb [signed-by=/usr/share/keyrings/cloud.google.gpg] https://packages.cloud.google.com/apt cloud-sdk main" | sudo tee -a /etc/apt/sources.list.d/google-cloud-sdk.list

  # Import the Google Cloud public key
  curl https://packages.cloud.google.com/apt/doc/apt-key.gpg | sudo apt-key --keyring /usr/share/keyrings/cloud.google.gpg add -

  # Update and install the Cloud SDK
  sudo apt-get update && sudo apt-get install -y google-cloud-sdk

  echo "[+] Google Cloud SDK installed"
else
  echo "[+] gsutil is already installed"
fi

echo "[*] Verifying GCS authentication..."
# Check if service account key exists
if [ ! -f /etc/google-cloud-keys/grabbiel-media-key.json ]; then
  echo "[!] GCS service account key not found at /etc/google-cloud-keys/grabbiel-media-key.json"
  echo "[!] GCS integration may not work properly"
else
  echo "[+] GCS authentication key found"
fi

echo "[*] Compiling to $BUILD_PATH..."
g++ -std=c++17 -O2 -o "$BUILD_PATH" \
  "$SRC_DIR/article_publisher.cpp" \
  "$SRC_DIR/https_server.cpp" \
  -lsqlite3 -pthread

echo "[*] Moving binary to $OUT_PATH..."
sudo mv "$BUILD_PATH" "$OUT_PATH"
sudo chmod +x "$OUT_PATH"

echo "[*] Creating required directories..."
sudo mkdir -p /var/lib/article-content
sudo chmod 755 /var/lib/article-content

echo "[+] Build complete and deployed to $OUT_PATH"
