"""FastAPI server — HTTP routes + WebSocket endpoint."""
import asyncio
import hashlib
import json
import logging
import re
import subprocess
import time
from pathlib import Path

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, UploadFile, File, Form, Request
from fastapi.responses import Response, HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from event_bus import EventBus
from slop_loader import SlopLoader
from orchestrator import Orchestrator
from mudproxy.paths import mdb_search_paths, slop_search_paths

logger = logging.getLogger(__name__)

_MDB_SEARCH_PATHS = mdb_search_paths()
_SLOP_SEARCH_PATHS = slop_search_paths()

def _get_static_dir() -> Path:
    """Resolve static dir — works both normally and inside PyInstaller bundle."""
    import sys
    if getattr(sys, '_MEIPASS', None):
        return Path(sys._MEIPASS) / "static"
    return Path(__file__).parent / "static"

STATIC_DIR = _get_static_dir()


def create_app(orchestrator: Orchestrator, event_bus: EventBus, slop: SlopLoader) -> FastAPI:
    app = FastAPI(title="MajorSLOP!")

    @app.on_event("startup")
    async def startup():
        await orchestrator.start()

    # Serve static files (no cache during dev)
    app.mount("/static", StaticFiles(directory=str(STATIC_DIR)), name="static")

    @app.middleware("http")
    async def no_cache_static(request, call_next):
        response = await call_next(request)
        if request.url.path.startswith("/static/"):
            response.headers["Cache-Control"] = "no-cache, no-store, must-revalidate"
            response.headers["Pragma"] = "no-cache"
            if "ETag" in response.headers:
                del response.headers["ETag"]
        return response

    def _cache_bust_html(html: str) -> str:
        """Replace ?v=N query strings on /static/ URLs with content hashes."""
        def _hash_asset(m):
            url_path = m.group(1)       # e.g. /static/js/nav_panel.js
            rel = url_path.split("/static/", 1)[-1]  # js/nav_panel.js
            full = STATIC_DIR / rel
            if full.exists():
                h = hashlib.md5(full.read_bytes()).hexdigest()[:10]
                return f'{url_path}?h={h}'
            return m.group(0)
        return re.sub(r'(/static/[^\s"\'?]+)(?:\?[^\s"\']*)?', _hash_asset, html)

    @app.get("/", response_class=HTMLResponse)
    async def index():
        index_path = STATIC_DIR / "index.html"
        html = index_path.read_text()
        return _cache_bust_html(html)

    # ── WebSocket ──

    @app.websocket("/ws")
    async def websocket_endpoint(ws: WebSocket):
        await event_bus.connect(ws)
        # Send current state snapshot
        try:
            state = orchestrator.get_state()
            await ws.send_json({"type": "state_sync", **state})
            # Re-populate any missing data (portrait, inventory, etc.)
            orchestrator.on_client_connect()
        except Exception:
            pass
        try:
            while True:
                data = await ws.receive_json()
                await _handle_ws_command(data, orchestrator)
        except WebSocketDisconnect:
            pass
        except Exception as e:
            logger.error(f"WebSocket error: {e}")
        finally:
            await event_bus.disconnect(ws)

    async def _handle_ws_command(data: dict, orch: Orchestrator):
        """Handle commands from the browser."""
        cmd = data.get("command")
        if cmd == "connect":
            success = await orch.connect()
            await event_bus.broadcast("connection", {
                "status": "connected" if success else "failed"
            })
        elif cmd == "disconnect":
            await orch.disconnect()
        elif cmd == "inject":
            text = data.get("text", "")
            if text is not None:
                await orch.inject_command(text)
        elif cmd == "raw_input":
            # Raw bytes from terminal (arrow keys, escape sequences, etc.)
            raw = data.get("data", "")
            if raw:
                await orch.inject_raw(raw.encode('latin-1', errors='replace'))
        elif cmd == "ghost":
            ghost_name = data.get("name", "")
            at_cmd = data.get("at_cmd", "")
            if ghost_name and at_cmd:
                await orch.ghost_command(ghost_name, at_cmd)
        elif cmd == "silent_pro":
            await orch.proxy.inject_silent_pro()
        elif cmd == "set_pro_mode":
            mode = data.get("mode", "silent")
            if mode in ("silent", "loud", "off"):
                orch.pro_mode = mode
                await event_bus.broadcast("pro_mode", {"mode": mode})
                if hasattr(orch, 'on_pro_mode_changed') and orch.on_pro_mode_changed:
                    orch.on_pro_mode_changed(mode)
        elif cmd == "toggle_ambient_filter":
            orch.ambient_filter_enabled = not orch.ambient_filter_enabled
            await event_bus.broadcast("ambient_filter", {
                "enabled": orch.ambient_filter_enabled
            })
            if hasattr(orch, 'on_ambient_filter_changed') and orch.on_ambient_filter_changed:
                orch.on_ambient_filter_changed(orch.ambient_filter_enabled)
        elif cmd == "get_state":
            # Re-send full state
            state = orch.get_state()
            await event_bus.broadcast("state_sync", state)

    # ── Asset API ──

    @app.get("/api/asset/{key:path}")
    async def get_asset(key: str):
        """Serve a slop asset (image/depth/prompt) by key."""
        # Strip trailing /prompt suffix (handled by separate route)
        if key.endswith("/prompt"):
            return await get_asset_prompt(key[:-7])

        data = slop.get_asset(key)
        if data is None:
            logger.warning(f"Asset 404: {key!r}")
            return Response(status_code=404)

        # Determine content type
        if key.endswith("_depth"):
            content_type = "image/png"
        else:
            content_type = "image/webp"

        return Response(
            content=data,
            media_type=content_type,
            headers={
                "Cache-Control": "no-store",
            },
        )

    @app.get("/api/asset-prompt/{key:path}")
    async def get_asset_prompt(key: str):
        """Get the prompt text that generated an asset."""
        prompt = slop.get_prompt(key)
        if prompt is None:
            return Response(status_code=404)
        return JSONResponse({"key": key, "prompt": prompt})

    @app.get("/api/entity/{name}")
    async def get_entity(name: str):
        """Get entity info from gamedata."""
        monster = orchestrator.gamedata.get_monster_stats(name)
        if monster:
            drops = orchestrator.gamedata.get_monster_drops(name)
            return JSONResponse({"type": "monster", "stats": monster, "drops": drops})
        item = orchestrator.gamedata.get_item(name)
        if item:
            return JSONResponse({"type": "item", "data": item})
        return Response(status_code=404)

    @app.get("/api/slop/stats")
    async def slop_stats():
        return JSONResponse(slop.get_stats())

    @app.get("/api/slop/files")
    async def slop_files():
        """List available .slop files."""
        slop_dir = slop._slop_dir
        files = []
        if slop_dir.exists():
            for p in sorted(slop_dir.glob("*.slop")):
                files.append({
                    "name": p.name,
                    "path": str(p),
                    "size_mb": round(p.stat().st_size / 1024 / 1024, 1),
                    "loaded": p in slop._loaded_files,
                })
        return JSONResponse({"files": files, "stats": slop.get_stats()})

    @app.post("/api/slop/load")
    async def slop_load(body: dict):
        """Load a specific .slop file by name."""
        from pathlib import Path as P
        name = body.get("name", "")
        slop_path = slop._slop_dir / name
        if not slop_path.exists() or not name.endswith(".slop"):
            return Response(status_code=404)
        count = slop.load_file(slop_path)
        return JSONResponse({"loaded": name, "entries": count, "stats": slop.get_stats()})

    @app.post("/api/slop/quality")
    async def slop_quality(body: dict):
        """Toggle between hi-res and lo-res assets."""
        hires = body.get("hires", True)
        slop.prefer_hires = bool(hires)
        alt_count = len(slop._alt_index)
        logger.info(f"Asset quality: {'hi-res' if hires else 'lo-res'} ({alt_count} assets have variants)")
        return JSONResponse({"prefer_hires": slop.prefer_hires, "alt_assets": alt_count})

    @app.post("/api/slop/reload")
    async def slop_reload():
        """Reload the currently loaded slop files (does NOT load new ones)."""
        # Clear and reload only the files that were originally loaded
        loaded = list(slop._loaded_files)
        slop._index.clear()
        slop._alt_index.clear()
        slop._prompt_index.clear()
        slop._loaded_files.clear()
        for p in loaded:
            if p.exists():
                slop.load_file(p)
        return JSONResponse({"total_assets": len(slop._index), "stats": slop.get_stats()})

    @app.get("/api/debug/room")
    async def debug_room():
        """Debug: show current room state."""
        state = orchestrator.get_state()
        return JSONResponse({
            "room_name": state.get("room_name"),
            "exits": state.get("exits"),
            "room_image_key": state.get("room_image_key"),
            "depth_key": state.get("depth_key"),
            "entities": [{"name": e["name"], "key": e.get("key")} for e in state.get("entities", [])],
            "items": [{"name": i["name"], "key": i.get("key")} for i in state.get("items", [])],
        })

    @app.get("/api/status")
    async def status():
        return JSONResponse({
            "connected": orchestrator.proxy.connected,
            "ws_clients": event_bus.client_count,
            "slop": slop.get_stats(),
            "mem_attached": orchestrator.mem_reader.attached,
        })

    # ── MegaMUD Memory API ──

    @app.get("/api/mem/state")
    async def mem_full_state():
        """Full MegaMUD process memory state dump."""
        state = orchestrator.get_mem_state()
        if state is None:
            return JSONResponse({"error": "not attached", "attached": False}, status_code=503)
        return JSONResponse({"attached": True, **state})

    @app.get("/api/mem/hp")
    async def mem_hp():
        if not orchestrator.mem_reader.attached:
            return JSONResponse({"error": "not attached"}, status_code=503)
        return JSONResponse(orchestrator.mem_reader.get_hp_mana())

    @app.get("/api/mem/stats")
    async def mem_stats():
        if not orchestrator.mem_reader.attached:
            return JSONResponse({"error": "not attached"}, status_code=503)
        return JSONResponse(orchestrator.mem_reader.get_stats())

    @app.get("/api/mem/flags")
    async def mem_flags():
        if not orchestrator.mem_reader.attached:
            return JSONResponse({"error": "not attached"}, status_code=503)
        return JSONResponse(orchestrator.mem_reader.get_flags())

    @app.get("/api/mem/room")
    async def mem_room():
        if not orchestrator.mem_reader.attached:
            return JSONResponse({"error": "not attached"}, status_code=503)
        return JSONResponse(orchestrator.mem_reader.get_room())

    @app.get("/api/mem/party")
    async def mem_party():
        if not orchestrator.mem_reader.attached:
            return JSONResponse({"error": "not attached"}, status_code=503)
        return JSONResponse(orchestrator.mem_reader.get_party())

    @app.get("/api/mem/player")
    async def mem_player():
        if not orchestrator.mem_reader.attached:
            return JSONResponse({"error": "not attached"}, status_code=503)
        return JSONResponse(orchestrator.mem_reader.get_player())

    @app.get("/api/mem/exp")
    async def mem_exp():
        if not orchestrator.mem_reader.attached:
            return JSONResponse({"error": "not attached"}, status_code=503)
        return JSONResponse(orchestrator.mem_reader.get_experience())

    @app.post("/api/mem/attach")
    async def mem_attach():
        """Manually trigger attach attempt with optional player name."""
        name = orchestrator._char_name or None
        ok = orchestrator.mem_reader.attach(name)
        return JSONResponse({"attached": ok})

    @app.post("/api/mem/goto")
    async def mem_goto(request: Request):
        """Navigate MegaMUD to a room. Body: {"room": "Bank of Godfrey", "run": false}"""
        body = await request.json()
        room = body.get("room", "")
        if not room:
            return JSONResponse({"ok": False, "error": "missing 'room' field"}, status_code=400)
        run = body.get("run", False)
        result = orchestrator.mem_reader.goto_room(room, run=run)
        return JSONResponse(result)

    @app.post("/api/mem/loop")
    async def mem_loop(request: Request):
        """Start a grind loop. Body: {"file": "DCCWLOOP"} — looks up .mp file for checksums."""
        body = await request.json()
        mp_name = body.get("file", "")
        if not mp_name:
            return JSONResponse({"ok": False, "error": "missing 'file' field"}, status_code=400)
        if not orchestrator.mem_reader.attached:
            return JSONResponse({"ok": False, "error": "not attached"}, status_code=503)

        # Find and parse the .mp file
        import os, platform
        if platform.system() == "Windows":
            mm_dir = Path("C:/MegaMUD")
        else:
            mm_dir = Path(os.path.expanduser("~/.wine/drive_c/MegaMUD"))

        mp_file = mp_name if mp_name.lower().endswith(".mp") else mp_name + ".mp"
        mp_path = None
        for d in [mm_dir / "Chars" / "All", mm_dir / "Default"]:
            candidate = d / mp_file
            if candidate.exists():
                mp_path = candidate
                break
        if not mp_path:
            return JSONResponse({"ok": False, "error": f".mp file not found: {mp_file}"}, status_code=404)

        try:
            lines = mp_path.read_text().splitlines()
            if len(lines) < 3:
                return JSONResponse({"ok": False, "error": "invalid .mp file"}, status_code=400)
            parts = lines[2].split(":")
            from_checksum = int(parts[0], 16)
            to_checksum = int(parts[1], 16)
            total_steps = int(parts[2])
        except (ValueError, IndexError) as e:
            return JSONResponse({"ok": False, "error": f"failed to parse .mp: {e}"}, status_code=400)

        result = orchestrator.mem_reader.start_loop(mp_file, from_checksum, to_checksum, total_steps)
        return JSONResponse(result)

    @app.post("/api/mem/stop")
    async def mem_stop():
        """Stop pathing/looping (mimics stop button)."""
        result = orchestrator.mem_reader.stop_pathing()
        return JSONResponse(result)

    @app.get("/api/mem/loop-monitor")
    async def mem_loop_monitor():
        """Raw loop/pathing flags for debugging. Poll this to watch MegaMUD's native loop behavior."""
        mr = orchestrator.mem_reader
        if not mr.attached:
            return JSONResponse({"error": "not attached"}, status_code=503)
        data = mr.get_loop_debug()
        if data is None:
            return JSONResponse({"error": "read failed"}, status_code=503)
        data["_ts"] = time.time()
        return JSONResponse(data)

    @app.get("/api/mem/read-range")
    async def mem_read_range(offset: str = "0x5640", size: int = 256):
        """Read raw memory range as hex + i32 dump. For reverse engineering."""
        mr = orchestrator.mem_reader
        if not mr.attached:
            return JSONResponse({"error": "not attached"}, status_code=503)
        try:
            off = int(offset, 16) if offset.startswith("0x") else int(offset)
        except ValueError:
            return JSONResponse({"error": "bad offset"}, status_code=400)
        size = min(size, 1024)
        data = mr._read_bytes(off, size)
        if data is None:
            return JSONResponse({"error": "read failed"}, status_code=503)
        # Return as i32 values with offsets
        import struct
        i32s = {}
        for i in range(0, len(data) - 3, 4):
            val = struct.unpack_from('<i', data, i)[0]
            if val != 0:  # only show non-zero
                i32s[f"0x{off + i:04X}"] = val
        return JSONResponse({
            "offset": f"0x{off:04X}",
            "size": size,
            "hex": data.hex(),
            "nonzero_i32s": i32s,
        })

    @app.post("/api/mem/exp-reset")
    async def mem_exp_reset():
        """Reset MegaMUD's exp meter (BM_CLICK on Player Statistics button 1721)."""
        mr = orchestrator.mem_reader
        if not mr.attached:
            return JSONResponse({"ok": False, "error": "not attached"})
        ok = mr.reset_exp_meter()
        return JSONResponse({"ok": ok})

    @app.post("/api/mem/toggle")
    async def mem_toggle(request: Request):
        """Toggle a MegaMUD automation flag. Body: {"offset": "0x4D00", "value": 1}
        If value omitted, reads current and flips it."""
        body = await request.json()
        offset_str = body.get("offset", "")
        try:
            offset = int(offset_str, 16) if isinstance(offset_str, str) else int(offset_str)
        except (ValueError, TypeError):
            return JSONResponse({"ok": False, "error": "bad offset"})
        mr = orchestrator.mem_reader
        if not mr.attached:
            return JSONResponse({"ok": False, "error": "not attached"})
        if "value" in body:
            val = int(body["value"])
        else:
            cur = mr._read_i32(offset) or 0
            val = 0 if cur else 1
        mr._write_i32(offset, val)
        return JSONResponse({"ok": True, "offset": f"0x{offset:X}", "value": val})

    @app.get("/api/mem/rooms")
    async def mem_rooms(q: str = ""):
        """Search available rooms. ?q=pier returns matching rooms."""
        from mudproxy.mem_reader import MegaMUDMemReader
        rooms = MegaMUDMemReader._room_checksums
        if q:
            lower = q.lower()
            matches = {name: cksum for name, cksum in rooms.items() if lower in name.lower()}
        else:
            matches = dict(list(rooms.items())[:50])
        return JSONResponse({"count": len(matches), "rooms": {n: f"0x{c:08X}" for n, c in matches.items()}})

    # ── Gamedata / MDB API ──

    @app.get("/api/gamedata/status")
    async def gamedata_status():
        """Return gamedata load status and counts."""
        gd = orchestrator.gamedata
        counts = {}
        if gd.is_loaded:
            counts = {
                "monsters": len(gd._monsters),
                "items": len(gd._items),
                "rooms": len(gd._rooms),
                "spells": len(gd._spells),
                "shops": len(gd._shops),
                "classes": len(gd._classes),
                "races": len(gd._races),
                "lairs": len(gd._lairs),
            }
        # Check what MDB was last used (store path in gamedata dir)
        mdb_info_path = gd._data_dir / "_mdb_source.json"
        mdb_source = None
        if mdb_info_path.exists():
            try:
                mdb_source = json.loads(mdb_info_path.read_text())
            except Exception:
                pass
        return JSONResponse({
            "loaded": gd.is_loaded,
            "counts": counts,
            "mdb_source": mdb_source,
            "data_dir": str(gd._data_dir),
        })

    @app.get("/api/gamedata/scan-mdb")
    async def scan_mdb_files():
        """Scan known locations for .mdb files."""
        found = []
        seen = set()
        for search_dir in _MDB_SEARCH_PATHS:
            if not search_dir.exists():
                continue
            # Don't recurse too deep
            for pattern in ["*.mdb", "**/*.mdb"]:
                try:
                    for p in search_dir.glob(pattern):
                        real = p.resolve()
                        if real in seen:
                            continue
                        seen.add(real)
                        found.append({
                            "path": str(real),
                            "name": p.name,
                            "size_mb": round(p.stat().st_size / 1024 / 1024, 1),
                            "dir": str(p.parent),
                        })
                except (PermissionError, OSError):
                    continue
                if pattern == "*.mdb":
                    continue  # always do shallow first
                # limit deep scan
                if len(found) > 50:
                    break
        return JSONResponse({"files": found})

    @app.post("/api/gamedata/load-mdb")
    async def load_mdb(body: dict):
        """Load an MDB file: run export_gamedata.py then reload GameData."""
        mdb_path = body.get("path", "")
        if not mdb_path or not Path(mdb_path).exists():
            return JSONResponse({"error": "MDB file not found"}, status_code=404)
        if not mdb_path.endswith(".mdb"):
            return JSONResponse({"error": "Not an .mdb file"}, status_code=400)

        # Find export script
        export_script = Path(__file__).resolve().parent.parent / "scripts" / "export_gamedata.py"
        if not export_script.exists():
            return JSONResponse({"error": "export_gamedata.py not found"}, status_code=500)

        # Run export in a thread to avoid blocking
        gd = orchestrator.gamedata
        outdir = str(gd._data_dir)

        def _run_export():
            import sys as _sys
            python = _sys.executable
            result = subprocess.run(
                [python, str(export_script), mdb_path, "--outdir", outdir],
                capture_output=True, text=True, timeout=120,
            )
            return result

        try:
            loop = asyncio.get_event_loop()
            result = await loop.run_in_executor(None, _run_export)
        except subprocess.TimeoutExpired:
            return JSONResponse({"error": "Export timed out"}, status_code=500)
        except Exception as e:
            return JSONResponse({"error": str(e)}, status_code=500)

        if result.returncode != 0:
            return JSONResponse({
                "error": "Export failed",
                "stderr": result.stderr[:2000],
                "stdout": result.stdout[:2000],
            }, status_code=500)

        # Save source info
        mdb_info_path = gd._data_dir / "_mdb_source.json"
        try:
            mdb_info_path.write_text(json.dumps({
                "path": mdb_path,
                "name": Path(mdb_path).name,
            }))
        except Exception:
            pass

        # Reload gamedata
        gd.load()

        counts = {
            "monsters": len(gd._monsters),
            "items": len(gd._items),
            "rooms": len(gd._rooms),
            "spells": len(gd._spells),
            "shops": len(gd._shops),
        }
        return JSONResponse({
            "ok": True,
            "counts": counts,
            "output": result.stdout[-2000:],
        })

    # ── Enhanced SLOP API ──

    @app.get("/api/slop/scan")
    async def scan_slop_files():
        """Scan known locations for .slop files."""
        found = []
        seen = set()
        for search_dir in _SLOP_SEARCH_PATHS:
            if not search_dir.exists():
                continue
            try:
                for p in search_dir.glob("*.slop"):
                    real = p.resolve()
                    if real in seen:
                        continue
                    seen.add(real)
                    found.append({
                        "path": str(real),
                        "name": p.name,
                        "size_mb": round(p.stat().st_size / 1024 / 1024, 1),
                        "dir": str(p.parent),
                        "loaded": real in [lf.resolve() for lf in slop._loaded_files],
                    })
            except (PermissionError, OSError):
                continue
        return JSONResponse({"files": found})

    @app.post("/api/slop/load-path")
    async def slop_load_path(body: dict):
        """Load a .slop file by absolute path."""
        slop_path = body.get("path", "")
        if not slop_path or not Path(slop_path).exists():
            return JSONResponse({"error": "SLOP file not found"}, status_code=404)
        if not slop_path.endswith(".slop"):
            return JSONResponse({"error": "Not a .slop file"}, status_code=400)
        count = slop.load_file(Path(slop_path))
        # Notify connected clients about new assets
        await event_bus.broadcast("slop_update", slop.get_stats())
        return JSONResponse({
            "ok": True,
            "loaded": Path(slop_path).name,
            "entries": count,
            "stats": slop.get_stats(),
        })

    # ── Character Portrait API ──

    def _portrait_dir() -> Path:
        d = orchestrator.gamedata._data_dir / "portraits"
        d.mkdir(parents=True, exist_ok=True)
        return d

    @app.post("/api/portrait/upload")
    async def upload_portrait(file: UploadFile = File(...), char_name: str = Form(...)):
        """Upload a character portrait image. Saved as <char_name>.webp."""
        import re
        safe_name = re.sub(r'[^a-zA-Z0-9_-]', '_', char_name.strip().lower())
        if not safe_name:
            return JSONResponse({"error": "Invalid character name"}, status_code=400)

        data = await file.read()
        if len(data) > 10 * 1024 * 1024:  # 10MB limit
            return JSONResponse({"error": "File too large (10MB max)"}, status_code=400)

        # Save as-is (browser can handle any image format)
        # Detect format from content
        ext = "png"
        if data[:4] == b'RIFF':
            ext = "webp"
        elif data[:3] == b'\xff\xd8\xff':
            ext = "jpg"
        elif data[:8] == b'\x89PNG\r\n\x1a\n':
            ext = "png"

        dest = _portrait_dir() / f"{safe_name}.{ext}"
        # Remove any old portraits for this character
        for old in _portrait_dir().glob(f"{safe_name}.*"):
            old.unlink()
        dest.write_bytes(data)
        logger.info(f"Portrait saved: {dest}")
        return JSONResponse({"ok": True, "path": str(dest)})

    @app.get("/api/portrait/{char_name}")
    async def get_portrait(char_name: str):
        """Serve a character portrait by name."""
        import re
        safe_name = re.sub(r'[^a-zA-Z0-9_-]', '_', char_name.strip().lower())
        d = _portrait_dir()
        for ext in ("webp", "png", "jpg", "jpeg", "gif"):
            p = d / f"{safe_name}.{ext}"
            if p.exists():
                ct = {"webp": "image/webp", "png": "image/png", "jpg": "image/jpeg",
                      "jpeg": "image/jpeg", "gif": "image/gif"}
                return Response(content=p.read_bytes(), media_type=ct.get(ext, "image/png"),
                                headers={"Cache-Control": "no-cache"})
        return Response(status_code=404)

    # ── MegaMud Navigation API ──

    # Cache for parsed megamud nav data
    _nav_cache = {"data": None, "mtimes": {}}

    def _megamud_source_files() -> dict[str, Path]:
        """Find MegaMud source files (Rooms.md, .mp loops/paths)."""
        import os, platform
        # Under Wine: C:\MegaMUD; on Linux natively: ~/.wine/drive_c/MegaMUD
        if platform.system() == "Windows":
            mm_dir = Path("C:/MegaMUD")
        else:
            mm_dir = Path(os.path.expanduser("~/.wine/drive_c/MegaMUD"))
        sources = {}
        for rooms_file in [mm_dir / "Default" / "Rooms.md", mm_dir / "Chars" / "All" / "Rooms.md"]:
            if rooms_file.exists():
                sources[str(rooms_file)] = rooms_file
        for d in [mm_dir / "Default", mm_dir / "Chars" / "All"]:
            if d.exists():
                for mp in d.glob("*.mp"):
                    sources[str(mp)] = mp
        return sources

    def _check_nav_stale() -> bool:
        """Check if any MegaMud source files changed since last parse."""
        sources = _megamud_source_files()
        if not sources:
            return False
        if _nav_cache["data"] is None:
            return True
        for path_str, path in sources.items():
            try:
                mtime = path.stat().st_mtime
            except OSError:
                continue
            if path_str not in _nav_cache["mtimes"] or _nav_cache["mtimes"][path_str] != mtime:
                return True
        # Also check if files were removed
        if set(_nav_cache["mtimes"].keys()) != set(sources.keys()):
            return True
        return False

    def _rebuild_nav():
        """Re-parse MegaMud files and update cache."""
        import sys, os, platform
        sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
        from parse_megamud_rooms import parse_rooms_md, scan_mp_files

        if platform.system() == "Windows":
            mm_dir = Path("C:/MegaMUD")
        else:
            mm_dir = Path(os.path.expanduser("~/.wine/drive_c/MegaMUD"))

        # Parse rooms
        all_rooms = parse_rooms_md(mm_dir / "Default" / "Rooms.md")
        char_rooms = parse_rooms_md(mm_dir / "Chars" / "All" / "Rooms.md")
        by_code = {r["code"]: r for r in all_rooms}
        for r in char_rooms:
            by_code[r["code"]] = r
        all_rooms = list(by_code.values())

        categories = {}
        hidden = []
        for r in all_rooms:
            if r["flags"] & 0x10:
                hidden.append(r)
                continue
            cat = r["category"] or "Uncategorized"
            if cat not in categories:
                categories[cat] = []
            categories[cat].append({"code": r["code"], "name": r["name"], "flags": r["flags"]})
        for cat in categories:
            categories[cat].sort(key=lambda x: x["name"])

        # Parse loops and paths from Default and Chars/All
        default_loops, default_paths = scan_mp_files(mm_dir / "Default")
        char_loops, char_paths = scan_mp_files(mm_dir / "Chars" / "All")
        loop_by_file = {l["file"]: l for l in default_loops}
        for l in char_loops:
            loop_by_file[l["file"]] = l
        all_loops = sorted(loop_by_file.values(), key=lambda x: x.get("start_category", "") + x.get("name", ""))

        loop_categories = {}
        for l in all_loops:
            cat = l.get("start_category") or "Uncategorized"
            if cat not in loop_categories:
                loop_categories[cat] = []
            loop_categories[cat].append(l)

        path_by_file = {p["file"]: p for p in default_paths}
        for p in char_paths:
            path_by_file[p["file"]] = p
        all_paths = sorted(path_by_file.values(), key=lambda x: x.get("start_category", "") + x.get("name", ""))
        path_categories = {}
        for p in all_paths:
            cat = p.get("start_category") or "Uncategorized"
            if cat not in path_categories:
                path_categories[cat] = []
            path_categories[cat].append(p)

        data = {
            "rooms": dict(sorted(categories.items())),
            "hidden_rooms": [{"code": r["code"], "category": r["category"], "name": r["name"]} for r in hidden],
            "loops": dict(sorted(loop_categories.items())),
            "paths": dict(sorted(path_categories.items())),
            "stats": {
                "total_rooms": len(all_rooms),
                "visible_rooms": sum(len(v) for v in categories.values()),
                "hidden_rooms": len(hidden),
                "categories": len(categories),
                "loops": len(all_loops),
                "paths": len(all_paths),
            },
        }

        # Update cache with current mtimes
        sources = _megamud_source_files()
        mtimes = {}
        for path_str, path in sources.items():
            try:
                mtimes[path_str] = path.stat().st_mtime
            except OSError:
                pass
        _nav_cache["data"] = data
        _nav_cache["mtimes"] = mtimes

        logger.info(f"MegaMud nav rebuilt: {data['stats']['visible_rooms']} rooms, "
                     f"{data['stats']['loops']} loops, {data['stats']['paths']} paths")
        return data

    @app.get("/api/megamud/nav")
    async def megamud_nav():
        """Serve parsed MegaMud rooms, loops, and paths. Auto-rebuilds when source files change."""
        try:
            if _check_nav_stale():
                _rebuild_nav()
            if _nav_cache["data"] is None:
                return JSONResponse({"error": "No MegaMud files found"}, status_code=404)
            return JSONResponse(_nav_cache["data"])
        except Exception as e:
            logger.exception("Failed to load MegaMud nav data")
            return JSONResponse({"error": str(e)}, status_code=500)

    return app
