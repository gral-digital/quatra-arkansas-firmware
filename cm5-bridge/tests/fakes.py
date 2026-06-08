"""In-memory fakes used across the test suite."""

from __future__ import annotations

import threading
import time
from collections import deque
from typing import Callable

from quatra_cm5_bridge.transports.base import CommandHandler, Transport


class FakeSerial:
    """A minimal `serial.Serial` look-alike backed by two byte buffers.

    Tests write 'incoming from ESP32' bytes with `inject()`; the
    UartLink reader thread will read them via `read()`. Anything the
    bridge writes via `write()` is captured for assertions.
    """

    def __init__(self, read_timeout_s: float = 0.05) -> None:
        self.is_open = True
        self._read_timeout = read_timeout_s
        self._rx = bytearray()
        self._rx_event = threading.Event()
        self._tx = bytearray()
        self._lock = threading.Lock()

    def inject(self, data: bytes) -> None:
        with self._lock:
            self._rx.extend(data)
            self._rx_event.set()

    def written(self) -> bytes:
        with self._lock:
            return bytes(self._tx)

    # ----- pyserial surface -----

    def read(self, size: int = 1) -> bytes:
        deadline = time.monotonic() + self._read_timeout
        while True:
            with self._lock:
                if self._rx:
                    chunk = bytes(self._rx[:size])
                    del self._rx[:size]
                    if not self._rx:
                        self._rx_event.clear()
                    return chunk
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return b""
            self._rx_event.wait(min(remaining, 0.05))

    def write(self, data: bytes) -> int:
        with self._lock:
            self._tx.extend(data)
        return len(data)

    def close(self) -> None:
        self.is_open = False
        self._rx_event.set()

    def reset_input_buffer(self) -> None:
        with self._lock:
            self._rx.clear()


class FakeTransport(Transport):
    """A transport that captures publishes and lets the test inject commands."""

    def __init__(self) -> None:
        self.published: list[str] = []
        self._on_command: CommandHandler | None = None
        self._started = False
        self._fail_next = False
        self._publish_event = threading.Event()
        self._lock = threading.Lock()

    def start(self, on_command: CommandHandler) -> None:
        self._on_command = on_command
        self._started = True

    def stop(self, timeout_s: float = 5.0) -> None:
        self._started = False

    def publish(self, json_line: str) -> bool:
        if not self._started:
            return False
        with self._lock:
            if self._fail_next:
                self._fail_next = False
                return False
            self.published.append(json_line)
            self._publish_event.set()
        return True

    def inject_command(self, line: str) -> None:
        if self._on_command is None:
            raise RuntimeError("transport not started")
        self._on_command(line)

    def fail_next_publish(self) -> None:
        with self._lock:
            self._fail_next = True

    def wait_for_publish(self, count: int, timeout_s: float = 2.0) -> bool:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            with self._lock:
                if len(self.published) >= count:
                    return True
            self._publish_event.wait(0.05)
            self._publish_event.clear()
        return False
