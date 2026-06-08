"""Loopback transport — prints uplink JSON, never produces commands.

Useful for bench bring-up: wire the bridge to the ESP32 and watch
messages stream on stdout without needing a real cloud endpoint.
"""

from __future__ import annotations

import sys
import threading
from typing import TextIO

from .base import CommandHandler, Transport


class StdoutTransport(Transport):
    def __init__(self, stream: TextIO | None = None) -> None:
        self._stream = stream if stream is not None else sys.stdout
        self._lock = threading.Lock()
        self._started = False

    def start(self, on_command: CommandHandler) -> None:
        self._started = True

    def stop(self, timeout_s: float = 5.0) -> None:
        self._started = False

    def publish(self, json_line: str) -> bool:
        if not self._started:
            return False
        with self._lock:
            self._stream.write(json_line.rstrip("\n") + "\n")
            self._stream.flush()
        return True
