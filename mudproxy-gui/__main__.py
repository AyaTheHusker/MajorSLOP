"""MudProxy GUI — cross-platform server controller.

Usage:
    cd mudproxy-gui && python __main__.py
"""

import signal
import sys
from pathlib import Path

# Ensure mudproxy package is importable
_project_dir = Path(__file__).resolve().parent.parent
_web_dir = _project_dir / "mudproxy-web"
for p in [str(_project_dir), str(_web_dir)]:
    if p not in sys.path:
        sys.path.insert(0, p)

from PySide6.QtWidgets import QApplication

from main_window import MainWindow


def main():
    app = QApplication(sys.argv)
    app.setApplicationName("MajorSLOP!")

    window = MainWindow()
    window.show()

    # Allow Ctrl+C to kill the app
    signal.signal(signal.SIGINT, lambda *_: window.close())

    sys.exit(app.exec())


if __name__ == "__main__":
    main()
