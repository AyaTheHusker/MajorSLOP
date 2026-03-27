"""Thread-safe logging handler that emits Qt signals for GUI display."""

import logging
from PySide6.QtCore import QObject, Signal


class _LogBridge(QObject):
    """QObject that carries the log signal (must be a QObject subclass)."""
    log_message = Signal(str)


class QtLogHandler(logging.Handler):
    """Logging handler that forwards records to a Qt signal.

    Usage:
        handler = QtLogHandler()
        handler.bridge.log_message.connect(some_text_widget.appendPlainText)
        logging.getLogger().addHandler(handler)
    """

    def __init__(self, level=logging.NOTSET):
        super().__init__(level)
        self.bridge = _LogBridge()
        self.setFormatter(logging.Formatter(
            "%(asctime)s %(name)-20s %(levelname)-7s %(message)s"
        ))

    def emit(self, record: logging.LogRecord):
        try:
            msg = self.format(record)
            self.bridge.log_message.emit(msg)
        except Exception:
            self.handleError(record)
