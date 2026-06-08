"""Newline-delimited UART link to the ESP32-S3.

The firmware emits one JSON object per line on UART; we mirror that
on the way back. This module is a thin wrapper around `pyserial`
that:

* reads bytes until ``\\n``, decodes to ``str``, strips, dispatches
* writes a string + ``\\n`` atomically
* survives the serial port disappearing (USB unplug, CM5 reboot of
  the ESP32) by reconnecting in the background

The link does **not** parse or validate JSON — that is the bridge
orchestrator's job. We only care about framing.
"""

from __future__ import annotations

import logging
import threading
import time
from typing import Callable, Protocol

logger = logging.getLogger(__name__)


class SerialPort(Protocol):
    """Minimal subset of `serial.Serial` we use — eases testing."""

    is_open: bool

    def read(self, size: int = 1) -> bytes: ...
    def write(self, data: bytes) -> int: ...
    def close(self) -> None: ...
    def reset_input_buffer(self) -> None: ...


SerialFactory = Callable[[], SerialPort]


class UartLink:
    """Newline-framed reader/writer with auto-reconnect."""

    def __init__(
        self,
        serial_factory: SerialFactory,
        on_line: Callable[[str], None],
        *,
        reconnect_delay_s: float = 2.0,
        max_line_bytes: int = 4096,
    ) -> None:
        self._factory = serial_factory
        self._on_line = on_line
        self._reconnect_delay_s = reconnect_delay_s
        self._max_line_bytes = max_line_bytes

        self._serial: SerialPort | None = None
        self._serial_lock = threading.Lock()
        self._stop_event = threading.Event()
        self._reader_thread: threading.Thread | None = None

    # ----- lifecycle -------------------------------------------------------

    def start(self) -> None:
        if self._reader_thread is not None:
            raise RuntimeError("UartLink already started")
        self._reader_thread = threading.Thread(
            target=self._run, name="quatra-uart-rx", daemon=True
        )
        self._reader_thread.start()

    def stop(self, timeout_s: float = 5.0) -> None:
        self._stop_event.set()
        with self._serial_lock:
            if self._serial is not None:
                try:
                    self._serial.close()
                except Exception:  # noqa: BLE001 - best effort on shutdown
                    logger.debug("error closing serial during stop", exc_info=True)
                self._serial = None
        if self._reader_thread is not None:
            self._reader_thread.join(timeout=timeout_s)
            self._reader_thread = None

    # ----- I/O -------------------------------------------------------------

    def send_line(self, line: str) -> bool:
        """Write ``line + '\\n'`` to the port. Returns False if disconnected."""
        payload = (line.rstrip("\r\n") + "\n").encode("utf-8")
        with self._serial_lock:
            if self._serial is None or not self._serial.is_open:
                logger.warning("uart send_line dropped (port not open): %s", line[:80])
                return False
            try:
                self._serial.write(payload)
                return True
            except Exception:  # noqa: BLE001
                logger.exception("uart write failed; will reconnect")
                self._drop_locked()
                return False

    # ----- internals -------------------------------------------------------

    def _drop_locked(self) -> None:
        if self._serial is not None:
            try:
                self._serial.close()
            except Exception:  # noqa: BLE001
                pass
        self._serial = None

    def _open(self) -> bool:
        try:
            ser = self._factory()
        except Exception:  # noqa: BLE001
            logger.error("uart open failed", exc_info=True)
            return False
        with self._serial_lock:
            self._serial = ser
        logger.info("uart opened")
        return True

    def _run(self) -> None:
        buf = bytearray()
        while not self._stop_event.is_set():
            if self._serial is None:
                if not self._open():
                    self._stop_event.wait(self._reconnect_delay_s)
                    continue
                buf.clear()

            try:
                chunk = self._serial.read(256)  # blocks up to read_timeout
            except Exception:  # noqa: BLE001
                logger.exception("uart read failed; reconnecting")
                with self._serial_lock:
                    self._drop_locked()
                continue

            if not chunk:
                continue

            buf.extend(chunk)
            if len(buf) > self._max_line_bytes:
                logger.warning("uart line overflow (%d bytes); discarding", len(buf))
                buf.clear()
                continue

            while True:
                idx = buf.find(b"\n")
                if idx < 0:
                    break
                line_bytes = bytes(buf[:idx]).rstrip(b"\r")
                del buf[: idx + 1]
                if not line_bytes:
                    continue
                try:
                    line = line_bytes.decode("utf-8")
                except UnicodeDecodeError:
                    logger.warning("uart non-utf8 line dropped (%d bytes)", len(line_bytes))
                    continue
                try:
                    self._on_line(line)
                except Exception:  # noqa: BLE001
                    logger.exception("uart on_line callback raised; continuing")

        with self._serial_lock:
            self._drop_locked()
        logger.info("uart reader stopped")


def default_serial_factory(
    *,
    port: str,
    baudrate: int,
    bytesize: int,
    parity: str,
    stopbits: int,
    read_timeout_s: float,
) -> SerialFactory:
    """Return a factory that opens a real `serial.Serial` with these params."""
    import serial  # imported lazily so tests don't need pyserial installed

    parity_map = {"N": serial.PARITY_NONE, "E": serial.PARITY_EVEN, "O": serial.PARITY_ODD}
    bytesize_map = {5: serial.FIVEBITS, 6: serial.SIXBITS, 7: serial.SEVENBITS, 8: serial.EIGHTBITS}
    stopbits_map = {1: serial.STOPBITS_ONE, 2: serial.STOPBITS_TWO}

    def factory() -> SerialPort:
        ser = serial.Serial(
            port=port,
            baudrate=baudrate,
            bytesize=bytesize_map[bytesize],
            parity=parity_map[parity.upper()],
            stopbits=stopbits_map[stopbits],
            timeout=read_timeout_s,
        )
        ser.reset_input_buffer()
        return ser  # type: ignore[return-value]

    return factory
