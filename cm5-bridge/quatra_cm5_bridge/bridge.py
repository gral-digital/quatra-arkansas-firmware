"""Bridge orchestrator.

Wires together a `UartLink` and a `Transport`:

* lines from UART → validated as JSON → forwarded to `transport.publish`
* commands from transport → validated as JSON → written to UART

Validation is just `json.loads(...)`. The bridge never inspects the
schema (that belongs to the webapp and to the firmware). Malformed
lines are logged and dropped.

A bounded outbound queue absorbs transient cloud outages without
blocking the UART reader; once full, the oldest message is dropped
and a counter increments — visible in the periodic stats log.
"""

from __future__ import annotations

import contextlib
import json
import logging
import queue
import signal
import threading
import time
from dataclasses import dataclass, field
from typing import Callable

from .config import BridgeConfig
from .transports import Transport
from .uart_link import UartLink

logger = logging.getLogger(__name__)

_UPLINK_QUEUE_MAX = 256
_STATS_INTERVAL_S = 60.0


@dataclass
class BridgeStats:
    uart_lines_in: int = 0
    uart_lines_out: int = 0
    invalid_json_up: int = 0
    invalid_json_down: int = 0
    transport_publish_ok: int = 0
    transport_publish_fail: int = 0
    queue_drops: int = 0
    started_at: float = field(default_factory=time.monotonic)

    def snapshot(self) -> dict[str, int | float]:
        return {
            "uptime_s": round(time.monotonic() - self.started_at, 1),
            "uart_in": self.uart_lines_in,
            "uart_out": self.uart_lines_out,
            "invalid_json_up": self.invalid_json_up,
            "invalid_json_down": self.invalid_json_down,
            "pub_ok": self.transport_publish_ok,
            "pub_fail": self.transport_publish_fail,
            "q_drops": self.queue_drops,
        }


class Bridge:
    def __init__(
        self,
        cfg: BridgeConfig,
        uart: UartLink,
        transport: Transport,
        *,
        uplink_queue_max: int = _UPLINK_QUEUE_MAX,
    ) -> None:
        self._cfg = cfg
        self._uart = uart
        self._transport = transport
        self._uplink: queue.Queue[str] = queue.Queue(maxsize=uplink_queue_max)
        self._stop_event = threading.Event()
        self._publisher: threading.Thread | None = None
        self._stats_thread: threading.Thread | None = None
        self.stats = BridgeStats()

    # ----- lifecycle -------------------------------------------------------

    def start(self) -> None:
        # Wire UART → uplink queue first so we don't miss anything emitted
        # at boot if the transport is slow to connect.
        self._uart._on_line = self._handle_uart_line  # type: ignore[attr-defined]
        self._uart.start()
        self._transport.start(self._handle_command)
        self._publisher = threading.Thread(
            target=self._publish_loop, name="quatra-bridge-pub", daemon=True
        )
        self._publisher.start()
        self._stats_thread = threading.Thread(
            target=self._stats_loop, name="quatra-bridge-stats", daemon=True
        )
        self._stats_thread.start()
        logger.info(
            "bridge started: device_id=%s transport=%s uart=%s",
            self._cfg.device_id, self._cfg.transport, self._cfg.uart.port,
        )

    def stop(self, timeout_s: float = 5.0) -> None:
        logger.info("bridge stopping...")
        self._stop_event.set()
        with contextlib.suppress(Exception):
            self._transport.stop(timeout_s=timeout_s)
        with contextlib.suppress(Exception):
            self._uart.stop(timeout_s=timeout_s)
        # publisher will exit on the sentinel below
        with contextlib.suppress(queue.Full):
            self._uplink.put_nowait("")  # sentinel
        if self._publisher is not None:
            self._publisher.join(timeout=timeout_s)
        logger.info("bridge stopped. stats=%s", self.stats.snapshot())

    def run_forever(self) -> None:
        """Convenience for CLI usage: install SIGINT/SIGTERM handlers and block."""
        self.start()
        signal.signal(signal.SIGINT, lambda *_: self._stop_event.set())
        signal.signal(signal.SIGTERM, lambda *_: self._stop_event.set())
        try:
            while not self._stop_event.is_set():
                self._stop_event.wait(1.0)
        finally:
            self.stop()

    # ----- callbacks -------------------------------------------------------

    def _handle_uart_line(self, line: str) -> None:
        self.stats.uart_lines_in += 1
        if not _is_valid_json(line):
            self.stats.invalid_json_up += 1
            logger.warning("uart→cloud: invalid JSON dropped: %s", line[:120])
            return
        try:
            self._uplink.put_nowait(line)
        except queue.Full:
            # drop oldest, push newest — favour fresh data over backlog
            try:
                _ = self._uplink.get_nowait()
                self.stats.queue_drops += 1
            except queue.Empty:
                pass
            with contextlib.suppress(queue.Full):
                self._uplink.put_nowait(line)

    def _handle_command(self, line: str) -> None:
        if not _is_valid_json(line):
            self.stats.invalid_json_down += 1
            logger.warning("cloud→uart: invalid JSON dropped: %s", line[:120])
            return
        if self._uart.send_line(line):
            self.stats.uart_lines_out += 1
        else:
            logger.warning("cloud→uart: send failed (port not open)")

    # ----- workers ---------------------------------------------------------

    def _publish_loop(self) -> None:
        while not self._stop_event.is_set():
            try:
                line = self._uplink.get(timeout=0.5)
            except queue.Empty:
                continue
            if line == "":
                break
            if self._transport.publish(line):
                self.stats.transport_publish_ok += 1
            else:
                self.stats.transport_publish_fail += 1

    def _stats_loop(self) -> None:
        while not self._stop_event.wait(_STATS_INTERVAL_S):
            logger.info("stats: %s", self.stats.snapshot())


def _is_valid_json(line: str) -> bool:
    try:
        json.loads(line)
    except (json.JSONDecodeError, ValueError):
        return False
    return True


def build_transport(cfg: BridgeConfig) -> Transport:
    """Factory for the configured transport — imports lazily."""
    if cfg.transport == "stdout":
        from .transports.stdout import StdoutTransport
        return StdoutTransport()
    if cfg.transport == "http":
        from .transports.http import HttpTransport
        return HttpTransport(cfg.http, device_id=cfg.device_id)
    if cfg.transport == "mqtt":
        from .transports.mqtt import MqttTransport
        return MqttTransport(cfg.mqtt, device_id=cfg.device_id)
    raise ValueError(f"unknown transport {cfg.transport!r}")
