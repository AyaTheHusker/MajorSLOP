"""Entry point: starts the FastAPI server + MUD proxy on a shared asyncio loop.

Usage:
    cd mudproxy-web && python __main__.py
    cd mudproxy-web && python __main__.py --port 8080
    cd mudproxy-web && python __main__.py --slop /path/to/file.slop
"""
import argparse
import asyncio
import logging
import sys
from pathlib import Path

# Add this directory and parent to sys.path
_this_dir = Path(__file__).resolve().parent
_parent_dir = _this_dir.parent
if str(_this_dir) not in sys.path:
    sys.path.insert(0, str(_this_dir))
if str(_parent_dir) not in sys.path:
    sys.path.insert(0, str(_parent_dir))

import uvicorn

from mudproxy.config import Config

from event_bus import EventBus
from slop_loader import SlopLoader
from orchestrator import Orchestrator
from server import create_app

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(name)-20s %(levelname)-7s %(message)s",
)
logger = logging.getLogger("mudproxy-web")


def main():
    parser = argparse.ArgumentParser(description="MudProxy Web Viewer")
    parser.add_argument("--host", default="127.0.0.1", help="Bind address")
    parser.add_argument("--port", type=int, default=8080, help="HTTP port")
    parser.add_argument("--slop", type=str, default=None, help="Path to .slop file")
    parser.add_argument("--dat-dir", type=str, default=None,
                        help="Path to MajorMUD .dat files (default: ~/Megamud)")
    args = parser.parse_args()

    # Load config
    config = Config.load()

    # Resolve dat directory
    dat_dir = Path(args.dat_dir) if args.dat_dir else config.get_dat_dir()

    # Initialize components
    event_bus = EventBus()
    slop = SlopLoader()

    # Load slop file if specified
    if args.slop:
        slop_path = Path(args.slop)
        if slop_path.exists():
            slop.load_file(slop_path)
            logger.info(f"Loaded SLOP: {slop_path}")
        else:
            logger.warning(f"SLOP file not found: {slop_path}")
    else:
        # Auto-load all .slop files from default directory
        slop.load_all()

    # Create orchestrator (wires proxy + parser + event bus)
    orchestrator = Orchestrator(config, event_bus, slop, dat_dir)

    # Create FastAPI app
    app = create_app(orchestrator, event_bus, slop)

    logger.info(f"Starting MudProxy Web on http://{args.host}:{args.port}")

    # Run with uvicorn
    uvicorn.run(
        app,
        host=args.host,
        port=args.port,
        log_level="info",
    )


if __name__ == "__main__":
    main()
