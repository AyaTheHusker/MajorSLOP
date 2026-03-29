#!/usr/bin/env python3
"""Test loop activation via MajorSLOP's web API."""
import requests
import time
import json

BASE = "http://127.0.0.1:8000"

def toggle(offset, value):
    r = requests.post(f"{BASE}/api/mem/toggle", json={"offset": offset, "value": value})
    return r.json()

def stop():
    r = requests.post(f"{BASE}/api/mem/stop")
    return r.json()

def start_loop(file):
    r = requests.post(f"{BASE}/api/mem/loop", json={"file": file})
    return r.json()

def get_state():
    r = requests.get(f"{BASE}/api/state")
    return r.json()

def main():
    print("Getting current state...")
    state = get_state()
    pathing = state.get("pathing", {})
    print(f"Status: {pathing.get('status')}")
    print(f"Mode: {pathing.get('mode')}")
    print(f"Looping: {pathing.get('looping')}")
    print(f"Pathing: {pathing.get('pathing')}")
    print(f"Go: {pathing.get('go')}")
    print(f"Path file: {pathing.get('path_file')}")
    print(f"Step: {pathing.get('step')}/{pathing.get('total_steps')}")
    print(f"Statusbar: {pathing.get('statusbar')}")

    input("\nPress Enter to start FERTLOOP...")

    print("\nStarting loop...")
    result = start_loop("FERTLOOP")
    print(f"Result: {json.dumps(result, indent=2)}")

    for i in range(5):
        time.sleep(2)
        state = get_state()
        p = state.get("pathing", {})
        print(f"\n[+{(i+1)*2}s] Status={p.get('status')} Mode={p.get('mode')} "
              f"Loop={p.get('looping')} Path={p.get('pathing')} Go={p.get('go')} "
              f"Step={p.get('step')}/{p.get('total_steps')}")
        sb = p.get('statusbar', [])
        if sb:
            print(f"  Statusbar: {' | '.join(sb)}")

if __name__ == "__main__":
    main()
