#!/bin/bash

N="$1"
CONFIG="${2:-config.txt}"
EXEC="${3:-lab04}"
USER_NAME="wederamos"

if [ -z "$N" ]; then
    echo "Usage: $0 <n> [config_file] [executable]"
    exit 1
fi

if [ ! -f "$CONFIG" ]; then
    echo "Config file not found: $CONFIG"
    exit 1
fi

if [ ! -f "$EXEC" ]; then
    echo "Executable not found: $EXEC"
    exit 1
fi

MASTER_IP=$(awk '$1=="MASTER" {print $2}' "$CONFIG")
MASTER_PORT=$(awk '$1=="MASTER" {print $3}' "$CONFIG")

if [ -z "$MASTER_IP" ] || [ -z "$MASTER_PORT" ]; then
    echo "No valid MASTER entry found in $CONFIG"
    exit 1
fi

echo "Master: $MASTER_IP:$MASTER_PORT"
echo "Launching slaves..."

awk '$1=="SLAVE" {print $2, $3, $4}' "$CONFIG" | while read -r SLAVE_ID SLAVE_IP SLAVE_PORT
do
    [ -z "$SLAVE_ID" ] && continue
    echo "Starting slave $SLAVE_ID on $SLAVE_IP:$SLAVE_PORT"

    ssh -n -o StrictHostKeyChecking=accept-new "$USER_NAME@$SLAVE_IP" \
        bash -s -- "$N" "$SLAVE_PORT" "$SLAVE_ID" <<'EOF'
cd ~ || exit 1
pkill -u wederamos -x lab04 2>/dev/null || true
chmod +x ./lab04 || exit 1
nohup ./lab04 "$1" "$2" 1 config.txt "$3" > "slave_$3.log" 2>&1 < /dev/null &
EOF
done

sleep 3

echo "Starting master on $MASTER_IP:$MASTER_PORT"

ssh -n -o StrictHostKeyChecking=accept-new "$USER_NAME@$MASTER_IP" \
    bash -s -- "$N" "$MASTER_PORT" <<'EOF'
cd ~ || exit 1
pkill -u wederamos -x lab04 2>/dev/null || true
chmod +x ./lab04 || exit 1
nohup ./lab04 "$1" "$2" 0 config.txt > master.log 2>&1 < /dev/null &
EOF

echo "Done."