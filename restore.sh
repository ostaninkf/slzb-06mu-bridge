#!/bin/bash
# Возврат штатной SLZB-OS из полного дампа флеша.
# Восстанавливает всё: прошивку, настройки, ключи сети Zigbee, NVS.
set -euo pipefail
cd "$(dirname "$0")"

DUMP="${1:?укажи файл дампа, например ../slzb/flash-dump-20260914-1646.bin}"
PORT="${2:-/dev/ttyACM0}"
ESPTOOL="python3 -m esptool"

[ "$(stat -c%s "$DUMP")" -eq 16777216 ] || { echo "дамп должен быть ровно 16 МБ"; exit 1; }
echo "--- восстанавливаю $DUMP (md5 $(md5sum "$DUMP" | cut -d' ' -f1))"

trap '$ESPTOOL --chip esp32s3 --port "$PORT" --after hard-reset chip-id >/dev/null 2>&1 || true' EXIT

$ESPTOOL --chip esp32s3 --port "$PORT" --baud 921600 --before default-reset --after hard-reset \
         write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x0 "$DUMP"
echo "--- штатная прошивка возвращена"
