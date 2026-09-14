#!/bin/bash
# Первая заливка своей прошивки по USB (мост в режиме загрузчика или обычном —
# ESP32-S3 шьётся через встроенный USB-JTAG без перемычек).
#
# Перед заливкой снимается свежий дамп всей флеш-памяти: это единственный
# способ вернуть штатную SLZB-OS вместе с настройками и ключами сети Zigbee.
set -euo pipefail
cd "$(dirname "$0")"

PORT="${1:-/dev/ttyACM0}"
ESPTOOL="python3 -m esptool"
STAMP=$(date +%Y%m%d-%H%M)
BACKUP="backup/flash-$STAMP.bin"

mkdir -p backup

# Что бы дальше ни случилось — мост должен выйти из загрузчика.
trap '$ESPTOOL --chip esp32s3 --port "$PORT" --after hard-reset chip-id >/dev/null 2>&1 || true' EXIT

echo "--- снимаю резервную копию флеша (16 МБ, это несколько минут)"
$ESPTOOL --chip esp32s3 --port "$PORT" --baud 921600 --before default-reset --after no-reset \
         read-flash 0 0x1000000 "$BACKUP"
md5sum "$BACKUP"
ls -l "$BACKUP"

echo "--- заливаю свою прошивку"
$ESPTOOL --chip esp32s3 --port "$PORT" --baud 921600 --before default-reset --after hard-reset \
         write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
         0x0      build/bootloader/bootloader.bin \
         0x8000   build/partition_table/partition-table.bin \
         0xf000   build/ota_data_initial.bin \
         0x20000  build/slzb-bridge.bin

echo "--- готово. Резервная копия: $BACKUP"
echo "    Возврат на штатную SLZB-OS:  ./restore.sh $BACKUP $PORT"
