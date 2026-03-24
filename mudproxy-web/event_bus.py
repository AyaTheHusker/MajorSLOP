"""Async event bus — broadcasts game events to connected WebSocket clients."""
import asyncio
import json
import logging
from typing import Any

from fastapi import WebSocket

logger = logging.getLogger(__name__)


class EventBus:
    def __init__(self):
        self._clients: set[WebSocket] = set()
        self._lock = asyncio.Lock()

    async def connect(self, ws: WebSocket):
        await ws.accept()
        async with self._lock:
            self._clients.add(ws)
        logger.info(f"WebSocket client connected ({len(self._clients)} total)")

    async def disconnect(self, ws: WebSocket):
        async with self._lock:
            self._clients.discard(ws)
        logger.info(f"WebSocket client disconnected ({len(self._clients)} total)")

    async def broadcast(self, event_type: str, data: dict[str, Any] | None = None):
        """Broadcast a JSON event to all connected clients."""
        msg = json.dumps({"type": event_type, **(data or {})})
        dead = set()
        clients = list(self._clients)
        for ws in clients:
            try:
                await asyncio.wait_for(ws.send_text(msg), timeout=2.0)
            except Exception:
                dead.add(ws)
        self._clients -= dead

    def broadcast_sync(self, event_type: str, data: dict[str, Any] | None = None, loop: asyncio.AbstractEventLoop | None = None):
        """Fire-and-forget broadcast from sync context."""
        _loop = loop or asyncio.get_event_loop()
        _loop.call_soon_threadsafe(
            asyncio.ensure_future,
            self.broadcast(event_type, data),
        )

    @property
    def client_count(self) -> int:
        return len(self._clients)
