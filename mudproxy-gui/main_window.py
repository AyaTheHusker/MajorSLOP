"""Main GUI window for MudProxy server controller."""

import logging
import webbrowser
from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtGui import QAction, QActionGroup, QCloseEvent, QFont
from PySide6.QtWidgets import (
    QCheckBox, QFileDialog, QGroupBox, QHBoxLayout, QLabel, QLineEdit,
    QMainWindow, QMessageBox, QPlainTextEdit, QPushButton, QSpinBox,
    QVBoxLayout, QWidget,
)

from mudproxy.config import Config
from mudproxy.paths import default_slop_dir, default_megamud_dir
# default_slop_dir used by _find_default_slop()

from log_handler import QtLogHandler
from server_thread import ServerThread
from tray_icon import TrayIcon

logger = logging.getLogger("mudproxy-gui")


def _find_default_slop() -> str:
    """Find the first .slop file in the default slop directory."""
    slop_dir = default_slop_dir()
    if slop_dir.exists():
        for p in sorted(slop_dir.glob("*.slop"), key=lambda x: x.stat().st_size, reverse=True):
            return str(p)
    return ""


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("MajorSLOP!")
        self.setMinimumSize(720, 560)
        self.resize(800, 620)
        self.force_quit = False

        self._config = Config.load()
        self._server: ServerThread | None = None

        self._build_ui()
        self._build_menu()
        self._setup_logging()
        self._setup_tray()
        self._scan_on_startup()

    # ── UI ───────────────────────────────────────────────────────────

    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)

        # -- Server Settings --
        settings = QGroupBox("Server Settings")
        settings_layout = QHBoxLayout(settings)

        settings_layout.addWidget(QLabel("Proxy Port:"))
        self._proxy_port = QSpinBox()
        self._proxy_port.setRange(1, 65535)
        self._proxy_port.setValue(self._config.proxy_port)
        settings_layout.addWidget(self._proxy_port)

        settings_layout.addSpacing(16)

        settings_layout.addWidget(QLabel("Web Port:"))
        self._web_port = QSpinBox()
        self._web_port.setRange(1, 65535)
        self._web_port.setValue(self._config.web_port)
        settings_layout.addWidget(self._web_port)

        settings_layout.addSpacing(16)

        settings_layout.addWidget(QLabel("Slop:"))
        saved_slop = self._config.path_slop_dir  # reuse for slop file path from config
        self._slop_edit = QLineEdit(saved_slop if saved_slop else _find_default_slop())
        self._slop_edit.setMinimumWidth(180)
        settings_layout.addWidget(self._slop_edit, stretch=1)
        browse_btn = QPushButton("...")
        browse_btn.setFixedWidth(30)
        browse_btn.clicked.connect(self._browse_slop)
        settings_layout.addWidget(browse_btn)

        root.addWidget(settings)

        # -- Paths --
        paths_box = QGroupBox("Paths (blank = OS default)")
        paths_layout = QVBoxLayout(paths_box)

        self._path_edits = {}
        path_fields = [
            ("megamud",  "MegaMud Dir",  self._config.path_megamud,    default_megamud_dir(),  True),
            ("mdb_file", "MME Export",   self._config.path_mdb_file,   "",                     False),
        ]

        for key, label, current, default, is_dir in path_fields:
            row = QHBoxLayout()
            lbl = QLabel(f"{label}:")
            lbl.setFixedWidth(100)
            row.addWidget(lbl)

            edit = QLineEdit(current)
            edit.setPlaceholderText(str(default))
            row.addWidget(edit, stretch=1)
            self._path_edits[key] = edit

            btn = QPushButton("...")
            btn.setFixedWidth(30)
            if is_dir:
                btn.clicked.connect(lambda _, e=edit: self._browse_dir(e))
            else:
                btn.clicked.connect(lambda _, e=edit: self._browse_file(e))
            row.addWidget(btn)

            paths_layout.addLayout(row)

        root.addWidget(paths_box)

        # -- Buttons --
        btn_row = QHBoxLayout()
        self._start_btn = QPushButton("Start")
        self._start_btn.clicked.connect(self._start_server)
        btn_row.addWidget(self._start_btn)

        self._stop_btn = QPushButton("Stop")
        self._stop_btn.setEnabled(False)
        self._stop_btn.clicked.connect(self._stop_server)
        btn_row.addWidget(self._stop_btn)

        self._open_btn = QPushButton("Open in Browser")
        self._open_btn.setEnabled(False)
        self._open_btn.clicked.connect(self._open_browser)
        btn_row.addWidget(self._open_btn)

        self._auto_browser = QCheckBox("Auto-open browser")
        self._auto_browser.setChecked(False)
        btn_row.addWidget(self._auto_browser)

        self._status_label = QLabel("Stopped")
        self._status_label.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        btn_row.addWidget(self._status_label, stretch=1)
        root.addLayout(btn_row)

        # -- Console log --
        self._console = QPlainTextEdit()
        self._console.setReadOnly(True)
        self._console.setFont(QFont("Monospace", 9))
        self._console.setStyleSheet(
            "QPlainTextEdit { background: #0d0d0d; color: #c8c8c8; }"
        )
        self._console.setMaximumBlockCount(5000)
        root.addWidget(self._console, stretch=1)

    def _setup_logging(self):
        self._log_handler = QtLogHandler(logging.DEBUG)
        self._log_handler.bridge.log_message.connect(self._console.appendPlainText)
        logging.getLogger().addHandler(self._log_handler)
        logging.getLogger().setLevel(logging.INFO)

    def _setup_tray(self):
        self._tray = TrayIcon(self)
        self._tray.show()

    def _build_menu(self):
        menu_bar = self.menuBar()

        # ── Proxy menu ──
        proxy_menu = menu_bar.addMenu("&Proxy")

        # Pro mode submenu
        pro_menu = proxy_menu.addMenu("Pro (Location)")
        self._pro_group = QActionGroup(self)
        self._pro_group.setExclusive(True)
        for label, mode in [
            ("Silent Pro (ParaMUD)", "silent"),
            ("Loud Pro (ParaMUD)", "loud"),
            ("No Pro (Legacy MMUD)", "off"),
        ]:
            action = QAction(label, self, checkable=True)
            action.setData(mode)
            if mode == "silent":
                action.setChecked(True)
            self._pro_group.addAction(action)
            pro_menu.addAction(action)
        self._pro_group.triggered.connect(self._on_pro_mode_changed)

        proxy_menu.addSeparator()

        # Ambient filter toggle
        self._ambient_action = QAction("Filter Ambient Messages", self, checkable=True)
        self._ambient_action.setChecked(True)
        self._ambient_action.triggered.connect(self._on_ambient_filter_changed)
        proxy_menu.addAction(self._ambient_action)

        # ── Help menu ──
        help_menu = menu_bar.addMenu("&Help")

        setup_action = QAction("Setup Guide", self)
        setup_action.triggered.connect(self._show_setup_guide)
        help_menu.addAction(setup_action)

        about_action = QAction("About MajorSLOP!", self)
        about_action.triggered.connect(self._show_about)
        help_menu.addAction(about_action)

    def _show_setup_guide(self):
        QMessageBox.information(self, "MajorSLOP! - Setup Guide",
            "<h2>Quick Start</h2>"
            "<ol>"
            "<li><b>Get a .slop image pack</b><br>"
            "Download from <a href='https://huggingface.co/Bellgaffer/MajorSLOP'>"
            "huggingface.co/Bellgaffer/MajorSLOP</a><br>"
            "Place it in the Slop Dir (or browse to it above)</li>"
            "<li><b>Click Start</b> in this window</li>"
            "<li><b>Configure MegaMud</b><br>"
            "Setup &rarr; Comm Parameters:<br>"
            "&nbsp;&nbsp;Host: <code>127.0.0.1</code><br>"
            "&nbsp;&nbsp;Port: <code>9999</code> (must match Proxy Port above)</li>"
            "<li><b>Open the browser UI</b><br>"
            "Click 'Open in Browser' or go to <code>http://127.0.0.1:8000</code></li>"
            "</ol>"
            "<h3>How it works</h3>"
            "<p>MajorSLOP sits between MegaMud and the BBS. MegaMud connects to "
            "MajorSLOP's proxy port (9999), and MajorSLOP connects to the real BBS. "
            "It reads the game data flowing through and shows room images, entity "
            "portraits, 3D effects, and more in the web UI.</p>"
            "<h3>Ports</h3>"
            "<p><b>Proxy Port (9999)</b> — MegaMud connects here<br>"
            "<b>Web Port (8000)</b> — Browser UI at http://127.0.0.1:8000</p>"
            "<h3>Paths</h3>"
            "<p><b>Slop Dir</b> — Where .slop image packs live<br>"
            "<b>MegaMud Dir</b> — MegaMud install folder (for reading game databases)<br>"
            "<b>Gamedata Dir</b> — Exported MDB game data (auto-populated)</p>"
            "<h3>Troubleshooting</h3>"
            "<p><b>Port in use?</b> Change the port number above.<br>"
            "<b>No images?</b> Make sure a .slop file is selected.<br>"
            "<b>MegaMud won't connect?</b> Verify host is 127.0.0.1 and port matches.</p>"
        )

    def _show_about(self):
        QMessageBox.about(self, "About MajorSLOP!",
            "<h2>MajorSLOP!</h2>"
            "<p>A visual proxy for MajorMUD that adds room images, entity portraits, "
            "3D depth effects, and a browser-based interface to your MUD session.</p>"
            "<p><a href='https://huggingface.co/Bellgaffer/MajorSLOP'>"
            "huggingface.co/Bellgaffer/MajorSLOP</a></p>"
        )

    def _scan_on_startup(self):
        """Scan all configured paths on app startup and report findings."""
        self._scan_mdb()
        self._scan_slop()
        self._scan_megamud()

    def _scan_mdb(self):
        """Load MDB if one is configured."""
        mdb = self._config.path_mdb_file
        if not mdb:
            logger.info("MME Export: not configured")
            return
        if not Path(mdb).exists():
            logger.warning(f"MME Export: file not found — {mdb}")
            return
        try:
            import sys as _sys
            scripts_dir = Path(__file__).resolve().parent.parent / "scripts"
            if str(scripts_dir) not in _sys.path:
                _sys.path.insert(0, str(scripts_dir))
            if getattr(_sys, '_MEIPASS', None):
                meipass_scripts = Path(_sys._MEIPASS) / "scripts"
                if str(meipass_scripts) not in _sys.path:
                    _sys.path.insert(0, str(meipass_scripts))

            from export_gamedata import run_export
            gamedata_dir = self._config.get_gamedata_dir()
            mdb_name = Path(mdb).name
            logger.info(f"MME Export: loading {mdb_name}...")
            counts = run_export(mdb, str(gamedata_dir))
            total = sum(counts.values())
            parts = ', '.join(f'{v} {k}' for k, v in counts.items())
            logger.info(f"MME Export: {total} records ({parts})")
        except Exception as e:
            logger.error(f"MME Export: failed — {e}")

    def _scan_slop(self):
        """Report slop file status with asset count."""
        slop = self._slop_edit.text().strip()
        if not slop:
            logger.info("Slop: not configured")
            return
        if not Path(slop).exists():
            logger.warning(f"Slop: file not found — {slop}")
            return
        size_mb = Path(slop).stat().st_size / 1024 / 1024
        # Quick-read the header to get entry count
        import struct
        try:
            with open(slop, "rb") as f:
                magic = f.read(4)
                if magic == b"SLOP":
                    _version, entry_count = struct.unpack("<II", f.read(8))
                    # Count asset types (0=npc, 1=item, 3=room, 4=depth, 5=prompt, 6=metadata)
                    monsters = items = rooms = depths = 0
                    for _ in range(entry_count):
                        key_len = struct.unpack("<H", f.read(2))[0]
                        f.read(key_len)  # skip key
                        asset_type = struct.unpack("<B", f.read(1))[0]
                        f.read(12)  # skip offset (8) + size (4)
                        if asset_type == 0:    # npc/monster thumb
                            monsters += 1
                        elif asset_type == 1:  # item thumb
                            items += 1
                        elif asset_type == 3:  # room image
                            rooms += 1
                        elif asset_type == 4:  # depth map
                            depths += 1
                    logger.info(f"Slop: {Path(slop).name} — {entry_count} assets ({size_mb:.0f} MB)")
                    logger.info(f"  {rooms} rooms, {monsters} monsters, {items} items, {depths} depth maps")
                else:
                    logger.warning(f"Slop: {Path(slop).name} — not a valid SLOP file")
        except Exception as e:
            logger.info(f"Slop: {Path(slop).name} ({size_mb:.0f} MB)")
            logger.warning(f"  Could not read index: {e}")

    def _scan_megamud(self):
        """Report MegaMud directory contents."""
        mega = self._config.path_megamud
        mega_dir = Path(mega) if mega else default_megamud_dir()
        if not mega_dir.exists():
            logger.info(f"MegaMud Dir: not found — {mega_dir}")
            return
        # Scan recursively for .md database files
        md_files = list(mega_dir.rglob("*.md")) + list(mega_dir.rglob("*.MD"))
        dat_files = list(mega_dir.rglob("*.dat")) + list(mega_dir.rglob("*.DAT"))
        # Filter to known useful .md files
        useful_names = {"Classes", "Items", "Monsters", "Paths", "Rooms",
                        "Spells", "Races", "Messages", "Macros", "Players"}
        useful_md = [f for f in md_files if f.stem in useful_names]
        if useful_md:
            names = sorted(set(f.stem for f in useful_md))
            logger.info(f"MegaMud Dir: {mega_dir}")
            logger.info(f"  Found {len(useful_md)} database files: {', '.join(names)}")
            if dat_files:
                logger.info(f"  Found {len(dat_files)} .dat files")
        elif dat_files:
            logger.info(f"MegaMud Dir: {mega_dir} ({len(dat_files)} .dat files)")
        else:
            logger.info(f"MegaMud Dir: {mega_dir} (no game data found)")

    # ── Proxy settings ────────────────────────────────────────────────

    def _get_orchestrator(self):
        """Get the orchestrator from the running server thread."""
        if self._server and hasattr(self._server, 'orchestrator'):
            return self._server.orchestrator
        return None

    def _on_pro_mode_changed(self, action: QAction):
        mode = action.data()
        orch = self._get_orchestrator()
        if orch:
            orch.pro_mode = mode
            # Broadcast to web clients too
            import asyncio
            try:
                loop = asyncio.get_event_loop()
                if loop.is_running():
                    asyncio.run_coroutine_threadsafe(
                        orch.bus.broadcast("pro_mode", {"mode": mode}), loop)
            except Exception:
                pass
            logger.info(f"Pro mode: {mode}")
        else:
            logger.info(f"Pro mode set to {mode} (will apply when server starts)")

    def _on_ambient_filter_changed(self, checked: bool):
        orch = self._get_orchestrator()
        if orch:
            orch.ambient_filter_enabled = checked
            import asyncio
            try:
                loop = asyncio.get_event_loop()
                if loop.is_running():
                    asyncio.run_coroutine_threadsafe(
                        orch.bus.broadcast("ambient_filter", {"enabled": checked}), loop)
            except Exception:
                pass
            logger.info(f"Ambient filter: {'on' if checked else 'off'}")

    def _on_web_pro_mode(self, mode: str):
        """Web UI changed pro mode — update GUI radio buttons."""
        for action in self._pro_group.actions():
            if action.data() == mode:
                action.setChecked(True)
                break
        logger.info(f"Pro mode (from web): {mode}")

    def _on_web_ambient_filter(self, enabled: bool):
        """Web UI changed ambient filter — update GUI checkbox."""
        self._ambient_action.setChecked(enabled)
        logger.info(f"Ambient filter (from web): {'on' if enabled else 'off'}")

    def sync_proxy_settings(self):
        """Sync GUI proxy menu state from the orchestrator (call after server starts)."""
        orch = self._get_orchestrator()
        if not orch:
            return
        # Sync pro mode
        for action in self._pro_group.actions():
            if action.data() == orch.pro_mode:
                action.setChecked(True)
                break
        # Sync ambient filter
        self._ambient_action.setChecked(orch.ambient_filter_enabled)

    # ── Actions ──────────────────────────────────────────────────────

    def _browse_slop(self):
        path, _ = QFileDialog.getOpenFileName(
            self, "Select .slop file",
            str(default_slop_dir()),
            "Slop files (*.slop);;All files (*)",
        )
        if path:
            self._slop_edit.setText(path)
            self._scan_slop()

    def _browse_dir(self, edit: QLineEdit):
        path = QFileDialog.getExistingDirectory(
            self, "Select Directory",
            edit.text() or edit.placeholderText(),
        )
        if path:
            edit.setText(path)
            # Re-scan if megamud dir changed
            if edit is self._path_edits.get("megamud"):
                self._config.path_megamud = path
                self._scan_megamud()

    def _browse_file(self, edit: QLineEdit):
        path, _ = QFileDialog.getOpenFileName(
            self, "Select MME Export File",
            edit.text() or edit.placeholderText(),
            "MDB files (*.mdb);;All files (*)",
        )
        if path:
            edit.setText(path)
            # Re-scan if mdb changed
            if edit is self._path_edits.get("mdb_file"):
                self._config.path_mdb_file = path
                self._scan_mdb()

    def _save_paths(self):
        """Save path edits back to config."""
        self._config.path_megamud = self._path_edits["megamud"].text().strip()
        self._config.path_mdb_file = self._path_edits["mdb_file"].text().strip()

    def _start_server(self):
        # Save config
        self._config.proxy_port = self._proxy_port.value()
        self._config.web_port = self._web_port.value()
        self._save_paths()
        self._config.save()

        slop = self._slop_edit.text().strip() or None
        dat_dir = self._config.get_dat_dir()

        mdb = self._config.path_mdb_file or None

        self._server = ServerThread(
            web_host="0.0.0.0",
            web_port=self._web_port.value(),
            proxy_port=self._proxy_port.value(),
            slop_path=slop,
            dat_dir=str(dat_dir),
            mdb_file=mdb,
        )
        self._server.started_ok.connect(self._on_started)
        self._server.stopped.connect(self._on_stopped)
        self._server.error.connect(self._on_error)
        self._server.start()

        self._start_btn.setEnabled(False)
        self._stop_btn.setEnabled(True)
        self._proxy_port.setEnabled(False)
        self._web_port.setEnabled(False)
        self._slop_edit.setEnabled(False)
        self._status_label.setText("Starting...")

    def _stop_server(self):
        if self._server:
            self._server.stop()
        self._status_label.setText("Stopping...")

    def _on_started(self):
        self._open_btn.setEnabled(True)
        self._status_label.setText(
            f"Running  -  http://127.0.0.1:{self._web_port.value()}"
        )
        self.sync_proxy_settings()
        # Wire bidirectional sync: web UI changes → update GUI menu
        orch = self._get_orchestrator()
        if orch:
            self._server.pro_mode_changed.connect(self._on_web_pro_mode)
            self._server.ambient_filter_changed.connect(self._on_web_ambient_filter)
            orch.on_pro_mode_changed = lambda mode: self._server.pro_mode_changed.emit(mode)
            orch.on_ambient_filter_changed = lambda en: self._server.ambient_filter_changed.emit(en)
        if self._auto_browser.isChecked():
            self._open_browser()

    def _on_stopped(self):
        self._start_btn.setEnabled(True)
        self._stop_btn.setEnabled(False)
        self._open_btn.setEnabled(False)
        self._proxy_port.setEnabled(True)
        self._web_port.setEnabled(True)
        self._slop_edit.setEnabled(True)
        self._status_label.setText("Stopped")
        self._server = None

    def _open_browser(self):
        url = f"http://127.0.0.1:{self._web_port.value()}"
        webbrowser.open(url)

    def _on_error(self, msg: str):
        self._status_label.setText(f"Error: {msg[:60]}")
        logger.error(f"Server error: {msg}")

    # ── Window events ────────────────────────────────────────────────

    def closeEvent(self, event: QCloseEvent):
        # Save paths before quitting
        self._save_paths()
        self._config.save()
        # Stop server
        if self._server:
            self._server.stop()
            self._server.wait(5000)
        logging.getLogger().removeHandler(self._log_handler)
        from PySide6.QtWidgets import QApplication
        QApplication.instance().quit()
        event.accept()
