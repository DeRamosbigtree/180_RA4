#!/bin/bash

N="$1"
CONFIG="${2:-config.txt}"
EXEC="${3:-./lab04}"
SESSION="lrp04"

if [ -z "$N" ]; then
    echo "Usage: $0 <n> [config_file] [executable]"
    echo "Example: $0 4000 config.txt ./lab04"
    exit 1
fi

if [ ! -f "$CONFIG" ]; then
    echo "Error: config file '$CONFIG' not found."
    exit 1
fi

if [ ! -x "$EXEC" ]; then
    echo "Error: executable '$EXEC' not found or not executable."
    exit 1
fi

MASTER_PORT=$(awk '$1=="MASTER" {print $3}' "$CONFIG")
SLAVE_COUNT=$(awk '$1=="SLAVE" {count++} END {print count+0}' "$CONFIG")

if [ "$SLAVE_COUNT" -eq 0 ]; then
    echo "Error: no SLAVE entries found in $CONFIG"
    exit 1
fi

# kill old session if it exists
tmux has-session -t "$SESSION" 2>/dev/null
if [ $? -eq 0 ]; then
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
done < <(awk '$1=="SLAVE" {print $2, $4}' "$CONFIG")

sleep 2

MASTER_CMD="$EXEC $N $MASTER_PORT 0 $CONFIG"
tmux split-window -t "$SESSION" "$MASTER_CMD; bash"
tmux select-layout -t "$SESSION" tiled >/dev/null

tmux attach -t "$SESSION"