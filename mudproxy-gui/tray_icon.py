"""System tray icon with show/hide and quit actions."""

from PySide6.QtGui import QAction, QIcon
from PySide6.QtWidgets import QMenu, QSystemTrayIcon


class TrayIcon(QSystemTrayIcon):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setIcon(QIcon.fromTheme("network-server"))
        self.setToolTip("MajorSLOP!")

        menu = QMenu()
        self._show_action = QAction("Show", menu)
        self._show_action.triggered.connect(self._toggle_window)
        menu.addAction(self._show_action)

        menu.addSeparator()

        quit_action = QAction("Quit", menu)
        quit_action.triggered.connect(self._quit)
        menu.addAction(quit_action)

        self.setContextMenu(menu)
        self.activated.connect(self._on_activated)

    def _toggle_window(self):
        w = self.parent()
        if w.isVisible():
            w.hide()
            self._show_action.setText("Show")
        else:
            w.show()
            w.raise_()
            w.activateWindow()
            self._show_action.setText("Hide")

    def _on_activated(self, reason):
        if reason == QSystemTrayIcon.ActivationReason.Trigger:
            self._toggle_window()

    def _quit(self):
        w = self.parent()
        if hasattr(w, "force_quit"):
            w.force_quit = True
        w.close()
