#!/bin/bash

USERNAME="wederamos"
OVERQUEEN="10.0.9.20"   # adjust if needed
CONFIG="config.txt"

# Generate SSH key if not existing
if [ ! -f ~/.ssh/id_ed25519 ]; then
    echo "Generating SSH key pair..."
    ssh-keygen -t ed25519 -N "" -f ~/.ssh/id_ed25519
else
    echo "SSH key already exists."
fi

echo ""
echo "Copying key to Overqueen..."
ssh-copy-id -o StrictHostKeyChecking=no "$USERNAME@$OVERQUEEN"

if [ ! -f "$CONFIG" ]; then
    echo "Config file not found. Skipping slaves."
    exit 0
fi

echo ""
echo "Copying key to each slave..."

# Read SLAVE lines: ID IP PORT
awk '$1=="SLAVE" {print $3}' "$CONFIG" | while read -r SLAVE_IP
do
    echo ""
    echo "Connecting to $SLAVE_IP"
    ssh-copy-id -o StrictHostKeyChecking=no "$USERNAME@$SLAVE_IP"
done

echo ""
echo "Done. SSH passwordless login should now work."