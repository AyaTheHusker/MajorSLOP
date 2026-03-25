"""FastAPI server — HTTP routes + WebSocket endpoint."""
import asyncio
import json
import logging
import subprocess
from pathlib import Path

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import Response, HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from event_bus import EventBus
from slop_loader import SlopLoader
from orchestrator import Orchestrator

logger = logging.getLogger(__name__)

# Known locations to scan for MDB files
_MDB_SEARCH_PATHS = [
    Path.home() / ".wine" / "drive_c" / "Program Files (x86)" / "MMUD Explorer",
    Path.home() / ".wine" / "drive_c" / "Program Files" / "MMUD Explorer",
    Path.home() / "Downloads",
    Path.home() / "Megamud",
    Path.home(),
]

# Known locations for SLOP files
_SLOP_SEARCH_PATHS = [
    Path.home() / ".local" / "share" / "mudproxy" / "slop",
    Path.home() / ".cache" / "mudproxy" / "slop",
    Path.home() / "Downloads",
    Path.home(),
]

STATIC_DIR = Path(__file__).parent / "static"


def create_app(orchestrator: Orchestrator, event_bus: EventBus, slop: SlopLoader) -> FastAPI:
    app = FastAPI(title="MudProxy Web")

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

    @app.get("/", response_class=HTMLResponse)
    async def index():
        index_path = STATIC_DIR / "index.html"
        return index_path.read_text()

    # ── WebSocket ──

    @app.websocket("/ws")
    async def websocket_endpoint(ws: WebSocket):
        await event_bus.connect(ws)
        # Send current state snapshot
        try:
            state = orchestrator.get_state()
            await ws.send_json({"type": "state_sync", **state})
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
            if text:
                await orch.inject_command(text)
        elif cmd == "ghost":
            ghost_name = data.get("name", "")
            at_cmd = data.get("at_cmd", "")
            if ghost_name and at_cmd:
                await orch.ghost_command(ghost_name, at_cmd)
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
        })

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

    return app
