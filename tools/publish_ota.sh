#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
remote="${OTA_REMOTE:-hamed@192.168.1.138}"
remote_dir="${OTA_REMOTE_DIR:-/home/hamed/cyd-printer-display/gateway/firmware}"
version="$(sed -n 's/^#define CYD_FIRMWARE_VERSION "\([^"]*\)"/\1/p' \
  "$project_dir/include/firmware_version.h")"

if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Invalid CYD_FIRMWARE_VERSION: $version" >&2
  exit 1
fi

cd "$project_dir"
pio run

version_file="$(mktemp)"
trap 'rm -f "$version_file"' EXIT
printf '%s\n' "$version" >"$version_file"

ssh "$remote" "mkdir -p '$remote_dir'"
rsync -az .pio/build/cyd/firmware.bin "$remote:$remote_dir/firmware.bin.new"
rsync -az "$version_file" "$remote:$remote_dir/version.txt.new"
ssh "$remote" \
  "mv '$remote_dir/firmware.bin.new' '$remote_dir/firmware.bin' && \
   mv '$remote_dir/version.txt.new' '$remote_dir/version.txt'"

echo "Published CYD firmware v$version to $remote"
