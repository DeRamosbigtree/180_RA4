#!/bin/bash

N="$1"
CONFIG="${2:-config.txt}"
EXEC="${3:-./lab04_ca}"
SESSION="lrp04ca"

if [ -z "$N" ]; then
    echo "Usage: $0 <n> [config_file] [executable]"
    exit 1
fi

if [ ! -f "$CONFIG" ]; then
    echo "Config file not found: $CONFIG"
    exit 1
fi

if [ ! -x "$EXEC" ]; then
    echo "Executable not found or not executable: $EXEC"
    exit 1
fi

MASTER_PORT=$(awk '$1=="MASTER" {print $3}' "$CONFIG")
SLAVES=$(awk '$1=="SLAVE" {print $2, $4}' "$CONFIG")

if [ -z "$MASTER_PORT" ]; then
    echo "No MASTER entry found in $CONFIG"
    exit 1
fi

if [ -z "$SLAVES" ]; then
    echo "No SLAVE entries found in $CONFIG"
    exit 1
fi

# kill leftover processes
pkill -x lab04_ca 2>/dev/null
sleep 1

# kill old tmux session only if it exists
if tmux has-session -t "$SESSION" 2>/dev/null; then
    tmux kill-session -t "$SESSION"
fi

FIRST=1

while read -r SLAVE_ID SLAVE_PORT
do
    CMD="$EXEC $N $SLAVE_PORT 1 $CONFIG $SLAVE_ID"

    if [ $FIRST -eq 1 ]; then
        tmux new-session -d -s "$SESSION" "$CMD; bash"
        FIRST=0
    else
        tmux split-window -t "$SESSION" "$CMD; bash"
        tmux select-layout -t "$SESSION" tiled >/dev/null
    fi
done <<< "$SLAVES"

sleep 2

MASTER_CMD="$EXEC $N $MASTER_PORT 0 $CONFIG"
tmux split-window -t "$SESSION" "$MASTER_CMD; bash"
tmux select-layout -t "$SESSION" tiled >/dev/null

tmux attach -t "$SESSION"