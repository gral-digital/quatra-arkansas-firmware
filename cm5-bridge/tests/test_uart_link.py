"""UartLink framing and reconnect behaviour."""

from __future__ import annotations

import threading
import time

import pytest

from quatra_cm5_bridge.uart_link import UartLink

from .fakes import FakeSerial


def _drain_lines(link: UartLink, lines: list[str], expected: int, timeout_s: float = 1.0) -> None:
    deadline = time.monotonic() + timeout_s
    while len(lines) < expected and time.monotonic() < deadline:
        time.sleep(0.02)


@pytest.mark.timeout(5)
def test_reads_single_line() -> None:
    ser = FakeSerial()
    received: list[str] = []
    link = UartLink(serial_factory=lambda: ser, on_line=received.append)
    link.start()
    try:
        ser.inject(b'{"ts":1,"x":42}\n')
        _drain_lines(link, received, 1)
    finally:
        link.stop()
    assert received == ['{"ts":1,"x":42}']


@pytest.mark.timeout(5)
def test_reads_multiple_lines_in_one_chunk() -> None:
    ser = FakeSerial()
    received: list[str] = []
    link = UartLink(serial_factory=lambda: ser, on_line=received.append)
    link.start()
    try:
        ser.inject(b'{"a":1}\n{"b":2}\n{"c":3}\n')
        _drain_lines(link, received, 3)
    finally:
        link.stop()
    assert received == ['{"a":1}', '{"b":2}', '{"c":3}']


@pytest.mark.timeout(5)
def test_assembles_split_line() -> None:
    ser = FakeSerial()
    received: list[str] = []
    link = UartLink(serial_factory=lambda: ser, on_line=received.append)
    link.start()
    try:
        ser.inject(b'{"hello":')
        time.sleep(0.1)
        assert received == []
        ser.inject(b'"world"}\n')
        _drain_lines(link, received, 1)
    finally:
        link.stop()
    assert received == ['{"hello":"world"}']


@pytest.mark.timeout(5)
def test_handles_crlf_endings() -> None:
    ser = FakeSerial()
    received: list[str] = []
    link = UartLink(serial_factory=lambda: ser, on_line=received.append)
    link.start()
    try:
        ser.inject(b'{"a":1}\r\n{"b":2}\r\n')
        _drain_lines(link, received, 2)
    finally:
        link.stop()
    assert received == ['{"a":1}', '{"b":2}']


@pytest.mark.timeout(5)
def test_drops_oversized_line() -> None:
    ser = FakeSerial()
    received: list[str] = []
    link = UartLink(
        serial_factory=lambda: ser,
        on_line=received.append,
        max_line_bytes=64,
    )
    link.start()
    try:
        garbage = b"x" * 200  # no newline → exceeds max
        ser.inject(garbage)
        time.sleep(0.2)
        # then a real line comes after the buffer is cleared
        ser.inject(b'{"ok":true}\n')
        _drain_lines(link, received, 1)
    finally:
        link.stop()
    assert received == ['{"ok":true}']


@pytest.mark.timeout(5)
def test_send_line_writes_with_newline() -> None:
    ser = FakeSerial()
    link = UartLink(serial_factory=lambda: ser, on_line=lambda _l: None)
    link.start()
    try:
        # wait for port to open
        time.sleep(0.1)
        assert link.send_line('{"cmd":"ping"}') is True
    finally:
        link.stop()
    assert ser.written() == b'{"cmd":"ping"}\n'


@pytest.mark.timeout(5)
def test_reconnects_after_factory_failure() -> None:
    """First open fails; second succeeds; reader must come back online."""
    attempts: list[FakeSerial] = []
    failures = {"count": 1}
    real_serial = FakeSerial()

    def factory() -> FakeSerial:
        if failures["count"] > 0:
            failures["count"] -= 1
            raise OSError("port busy")
        attempts.append(real_serial)
        return real_serial

    received: list[str] = []
    link = UartLink(
        serial_factory=factory,
        on_line=received.append,
        reconnect_delay_s=0.05,
    )
    link.start()
    try:
        # wait until factory was actually called the second time
        deadline = time.monotonic() + 2.0
        while not attempts and time.monotonic() < deadline:
            time.sleep(0.02)
        assert attempts, "factory was never retried"
        real_serial.inject(b'{"recovered":true}\n')
        _drain_lines(link, received, 1)
    finally:
        link.stop()
    assert received == ['{"recovered":true}']


@pytest.mark.timeout(5)
def test_callback_exception_does_not_kill_reader() -> None:
    ser = FakeSerial()
    received: list[str] = []

    def cb(line: str) -> None:
        if line == 'BOOM':
            raise RuntimeError("intentional")
        received.append(line)

    link = UartLink(serial_factory=lambda: ser, on_line=cb)
    link.start()
    try:
        ser.inject(b'BOOM\n{"after":1}\n')
        _drain_lines(link, received, 1)
    finally:
        link.stop()
    assert received == ['{"after":1}']
