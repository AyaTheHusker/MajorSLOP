#!/bin/bash
# raise-megamud.sh — Find hidden MegaMUD Wine window and raise it
# Usage:
#   ./raise-megamud.sh          # one-shot: raise it now
#   ./raise-megamud.sh --watch  # background: auto-raise whenever it hides

POLL_INTERVAL=2  # seconds between checks in watch mode

raise_megamud() {
    # MegaMUD's Wine window shows up as "paramud" in xdotool
    local wids
    wids=$(xdotool search --name "paramud" 2>/dev/null)

    if [ -z "$wids" ]; then
        return 1  # no window found at all
    fi

    local raised=0
    for wid in $wids; do
        # Check if window is mapped (visible) via xprop
        local map_state
        map_state=$(xprop -id "$wid" WM_STATE 2>/dev/null | grep "window state")

        # Check if it's behind other windows or unmapped
        # windowmap ensures it's mapped, windowactivate brings it forward
        xdotool windowmap "$wid" 2>/dev/null
        xdotool windowactivate "$wid" 2>/dev/null
        raised=1
    done

    [ "$raised" -eq 1 ] && return 0 || return 1
}

if [ "$1" = "--watch" ]; then
    echo "Watching for hidden MegaMUD window (checking every ${POLL_INTERVAL}s)..."
    echo "Press Ctrl+C to stop."

    last_visible=true
    while true; do
        wids=$(xdotool search --name "paramud" 2>/dev/null)
        if [ -n "$wids" ]; then
            for wid in $wids; do
                # Check if the window is in the active client list (wmctrl)
                if ! wmctrl -l 2>/dev/null | grep -q "$(printf '0x%08x' "$wid")"; then
                    # Window exists but not in window list — it's hidden
                    echo "[$(date +%H:%M:%S)] MegaMUD window hidden — raising (wid=$wid)"
                    xdotool windowmap "$wid" 2>/dev/null
                    xdotool windowactivate "$wid" 2>/dev/null
                    last_visible=false
                else
                    if [ "$last_visible" = false ]; then
                        echo "[$(date +%H:%M:%S)] MegaMUD window restored."
                        last_visible=true
                    fi
                fi
            done
        fi
        sleep "$POLL_INTERVAL"
    done
else
    if raise_megamud; then
        echo "MegaMUD window raised."
    else
        echo "No MegaMUD window found. Is megamud.exe running?"
        exit 1
    fi
fi
