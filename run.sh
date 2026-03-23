#!/bin/bash
# Kill any existing mudproxy python processes
for pid in $(pgrep -f "python3.12 -m mudproxy"); do
    kill "$pid" 2>/dev/null
done
sleep 0.5

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Use bundled portable Python
PYTHON="$SCRIPT_DIR/python/bin/python3.12"

if [ ! -x "$PYTHON" ]; then
    echo "Error: Bundled Python not found at $PYTHON"
    echo "Run setup.sh first to download the portable Python environment."
    exit 1
fi

# Delete stale bytecache
rm -rf "$SCRIPT_DIR/mudproxy/__pycache__"

exec "$PYTHON" -m mudproxy "$@"
