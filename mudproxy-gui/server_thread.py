"""QThread that runs the mudproxy-web uvicorn server."""

import sys
import logging
from pathlib import Path

from PySide6.QtCore import QThread, Signal

logger = logging.getLogger("mudproxy-gui.server")

# Paths for imports
_GUI_DIR = Path(__file__).resolve().parent
_PROJECT_DIR = _GUI_DIR.parent
_WEB_DIR = _PROJECT_DIR / "mudproxy-web"
_SCRIPTS_DIR = _PROJECT_DIR / "scripts"


class ServerThread(QThread):
    """Runs uvicorn in a background thread with start/stop control."""

    started_ok = Signal()
    stopped = Signal()
    error = Signal(str)
    pro_mode_changed = Signal(str)       # emitted when web UI changes pro mode
    ambient_filter_changed = Signal(bool) # emitted when web UI changes ambient filter

    def __init__(self, web_host: str, web_port: int, proxy_port: int,
                 slop_path: str | None = None, dat_dir: str | None = None,
                 mdb_file: str | None = None):
        super().__init__()
        self.web_host = web_host
        self.web_port = web_port
        self.proxy_port = proxy_port
        self.slop_path = slop_path
        self.dat_dir = dat_dir
        self.mdb_file = mdb_file
        self._server = None

    def run(self):
        """Thread entry — sets up and runs the uvicorn server."""
        import asyncio
        import io
        import uvicorn

        # PyInstaller --windowed sets stdout/stderr to None; uvicorn needs them
        if sys.stdout is None:
            sys.stdout = io.StringIO()
        if sys.stderr is None:
            sys.stderr = io.StringIO()

        # Ensure mudproxy-web and mudproxy packages are importable
        for p in [str(_WEB_DIR), str(_PROJECT_DIR)]:
            if p not in sys.path:
                sys.path.insert(0, p)

        try:
            from mudproxy.config import Config
            from event_bus import EventBus
            from slop_loader import SlopLoader
            from orchestrator import Orchestrator
            from server import create_app

            config = Config.load()
            config.proxy_port = self.proxy_port

            dat_dir = Path(self.dat_dir) if self.dat_dir else config.get_dat_dir()

            event_bus = EventBus()
            slop = SlopLoader()

            if self.slop_path:
                sp = Path(self.slop_path)
                if sp.exists():
                    slop.load_file(sp)
                    logger.info(f"Loaded SLOP: {sp}")
                else:
                    logger.warning(f"SLOP file not found: {sp}")
            else:
                slop.load_all()

            orchestrator = Orchestrator(config, event_bus, slop, dat_dir)
            self.orchestrator = orchestrator  # expose for GUI access
            app = create_app(orchestrator, event_bus, slop)

            uv_config = uvicorn.Config(
                app,
                host=self.web_host,
                port=self.web_port,
                log_level="info",
            )
            self._server = uvicorn.Server(uv_config)

            logger.info(f"Starting server on http://{self.web_host}:{self.web_port}")
            self.started_ok.emit()

            # Run the server (blocks until shutdown)
            self._server.run()

        except Exception as exc:
            logger.exception("Server crashed")
            self.error.emit(str(exc))
        finally:
            self._server = None
            self.stopped.emit()

    def stop(self):
        """Signal the uvicorn server to shut down gracefully."""
        if self._server:
            self._server.should_exit = True
            logger.info("Shutdown requested")
