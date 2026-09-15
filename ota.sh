#!/bin/bash
# Обновление прошивки по сети (мост уже с этой прошивкой).
#
# Таймаут на заливку — десять минут: на канале с большим RTT мегабайтный образ
# идёт десятки секунд, а прежние 60 с обрывали приём на середине (15.09.2026).
#
# Мост перезагружается сразу после приёма образа и ответить на запрос обычно
# не успевает — оборванное соединение здесь норма, а не ошибка. Поэтому итог
# проверяется по /status: аптайм после обновления должен обнулиться.
set -uo pipefail
cd "$(dirname "$0")"

HOST="${1:?укажи адрес моста: ./ota.sh 10.0.0.5}"
BIN="build/slzb-bridge.bin"
[ -f "$BIN" ] || { echo "сначала собери: ./build.sh"; exit 1; }

BEFORE=$(curl -s --max-time 5 "http://$HOST/status" | sed -n 's/.*"uptime":\([0-9]*\).*/\1/p')
echo "--- заливаю $(stat -c%s "$BIN") байт на $HOST (аптайм сейчас ${BEFORE:-?} с)"
curl -sS --max-time 600 -X POST --data-binary "@$BIN" "http://$HOST/update" >/dev/null 2>&1

for i in $(seq 1 20); do
    sleep 3
    S=$(curl -s --max-time 5 "http://$HOST/status" 2>/dev/null)
    AFTER=$(echo "$S" | sed -n 's/.*"uptime":\([0-9]*\).*/\1/p')
    if [ -n "$AFTER" ] && { [ -z "$BEFORE" ] || [ "$AFTER" -lt "$BEFORE" ]; }; then
        echo "--- мост перезагрузился и поднялся, аптайм $AFTER с"
        echo "$S"
        exit 0
    fi
done
echo "--- мост не ответил за минуту. Проверь адрес: при смене MAC DHCP выдаёт другой IP."
exit 1
