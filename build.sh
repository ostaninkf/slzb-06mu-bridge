#!/bin/bash
# Сборка в docker с ESP-IDF 5.4 — локальный тулчейн не нужен.
set -euo pipefail
cd "$(dirname "$0")"
exec docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp \
  espressif/idf:release-v5.4 \
  bash -c '. $IDF_PATH/export.sh >/dev/null 2>&1 && idf.py "${@:-build}"' -- "$@"
