import asyncio
import json
import logging
from pathlib import Path
from typing import Callable, Awaitable

SOCKET_PATH = "/tmp/mudproxy_cmd.sock"
logger = logging.getLogger(__name__)


class CommandAPI:
    def __init__(self, inject_fn: Callable[[str], Awaitable[None]],
                 get_state_fn: Callable[[], dict],
                 grind_start_fn: Callable[[], Awaitable[None]] = None,
                 grind_stop_fn: Callable[[], Awaitable[None]] = None):
        self._inject_fn = inject_fn
        self._get_state_fn = get_state_fn
        self._grind_start_fn = grind_start_fn
        self._grind_stop_fn = grind_stop_fn
        self._server = None

    async def start(self) -> None:
        sock = Path(SOCKET_PATH)
        if sock.exists():
            sock.unlink()
        self._server = await asyncio.start_unix_server(
            self._handle_client, path=str(sock)
        )
        sock.chmod(0o660)
        logger.info(f"Command API on {SOCKET_PATH}")

    async def stop(self) -> None:
        if self._server:
            self._server.close()
            await self._server.wait_closed()
        Path(SOCKET_PATH).unlink(missing_ok=True)

    async def _handle_client(self, reader: asyncio.StreamReader,
                             writer: asyncio.StreamWriter) -> None:
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                try:
                    req = json.loads(line.decode())
                except json.JSONDecodeError:
                    writer.write(json.dumps({"ok": False, "error": "bad json"}).encode() + b'\n')
                    await writer.drain()
                    continue

                resp = await self._handle(req)
                writer.write(json.dumps(resp).encode() + b'\n')
                await writer.drain()
        except Exception as e:
            logger.error(f"API error: {e}")
        finally:
            writer.close()

    async def _handle(self, req: dict) -> dict:
        action = req.get("action", "")
        if action == "command":
            text = req.get("text", "")
            await self._inject_fn(text)
            return {"ok": True}
        elif action == "status":
            return {"ok": True, "data": self._get_state_fn()}
        elif action == "grind_start":
            if self._grind_start_fn:
                await self._grind_start_fn()
                return {"ok": True, "msg": "Grind started"}
            return {"ok": False, "error": "grind not available"}
        elif action == "grind_stop":
            if self._grind_stop_fn:
                await self._grind_stop_fn()
                return {"ok": True, "msg": "Grind stopped"}
            return {"ok": False, "error": "grind not available"}
        return {"ok": False, "error": f"unknown: {action}"}
