"""End-to-end Bridge tests using FakeSerial + FakeTransport."""

from __future__ import annotations

import time

import pytest

from quatra_cm5_bridge.bridge import Bridge, _is_valid_json
from quatra_cm5_bridge.config import BridgeConfig, UartConfig
from quatra_cm5_bridge.uart_link import UartLink

from .fakes import FakeSerial, FakeTransport


def _make_bridge(uplink_queue_max: int = 16) -> tuple[Bridge, FakeSerial, FakeTransport]:
    ser = FakeSerial()
    uart = UartLink(serial_factory=lambda: ser, on_line=lambda _l: None)
    transport = FakeTransport()
    cfg = BridgeConfig(
        device_id="test-01",
        transport="stdout",
        uart=UartConfig(port="fake"),
    )
    bridge = Bridge(cfg, uart, transport, uplink_queue_max=uplink_queue_max)
    return bridge, ser, transport


@pytest.mark.timeout(5)
def test_uart_to_cloud_happy_path() -> None:
    bridge, ser, transport = _make_bridge()
    bridge.start()
    try:
        time.sleep(0.1)  # let UART open
        ser.inject(b'{"ts":1,"sensors":[10,20,30]}\n')
        ser.inject(b'{"ts":2,"state":"IRRIG"}\n')
        assert transport.wait_for_publish(2)
    finally:
        bridge.stop()
    assert transport.published == [
        '{"ts":1,"sensors":[10,20,30]}',
        '{"ts":2,"state":"IRRIG"}',
    ]
    assert bridge.stats.uart_lines_in == 2
    assert bridge.stats.transport_publish_ok == 2
    assert bridge.stats.invalid_json_up == 0


@pytest.mark.timeout(5)
def test_uart_invalid_json_is_dropped() -> None:
    bridge, ser, transport = _make_bridge()
    bridge.start()
    try:
        time.sleep(0.1)
        ser.inject(b'not json\n')
        ser.inject(b'{"ok":true}\n')
        assert transport.wait_for_publish(1)
        time.sleep(0.1)
    finally:
        bridge.stop()
    assert transport.published == ['{"ok":true}']
    assert bridge.stats.invalid_json_up == 1


@pytest.mark.timeout(5)
def test_cloud_to_uart_happy_path() -> None:
    bridge, ser, transport = _make_bridge()
    bridge.start()
    try:
        time.sleep(0.1)
        transport.inject_command('{"cmd":"set_mode","mode":"AUTO"}')
        deadline = time.monotonic() + 1.0
        while not ser.written() and time.monotonic() < deadline:
            time.sleep(0.02)
    finally:
        bridge.stop()
    assert ser.written() == b'{"cmd":"set_mode","mode":"AUTO"}\n'
    assert bridge.stats.uart_lines_out == 1


@pytest.mark.timeout(5)
def test_cloud_invalid_json_does_not_reach_uart() -> None:
    bridge, ser, transport = _make_bridge()
    bridge.start()
    try:
        time.sleep(0.1)
        transport.inject_command("nope")
        time.sleep(0.1)
    finally:
        bridge.stop()
    assert ser.written() == b""
    assert bridge.stats.invalid_json_down == 1


@pytest.mark.timeout(5)
def test_publish_failure_is_counted_but_not_fatal() -> None:
    bridge, ser, transport = _make_bridge()
    bridge.start()
    try:
        time.sleep(0.1)
        transport.fail_next_publish()
        ser.inject(b'{"a":1}\n')
        # Subsequent publishes should still go through.
        ser.inject(b'{"a":2}\n')
        assert transport.wait_for_publish(1)
    finally:
        bridge.stop()
    assert transport.published == ['{"a":2}']
    assert bridge.stats.transport_publish_ok == 1
    assert bridge.stats.transport_publish_fail == 1


@pytest.mark.timeout(5)
def test_queue_overflow_drops_oldest() -> None:
    # Queue size 2 → push 5, transport down so nothing drains.
    bridge, ser, transport = _make_bridge(uplink_queue_max=2)
    # Start UART only; transport.publish will return False because we
    # never call transport.start(). Wait — the Bridge.start() does call
    # transport.start(), so to simulate backpressure we monkey-patch
    # publish to block on an Event until the test releases it.
    import threading
    release = threading.Event()

    original_publish = transport.publish

    def slow_publish(line: str) -> bool:
        release.wait(timeout=1.0)
        return original_publish(line)

    transport.publish = slow_publish  # type: ignore[method-assign]

    bridge.start()
    try:
        time.sleep(0.1)
        for i in range(5):
            ser.inject(f'{{"i":{i}}}\n'.encode())
        time.sleep(0.3)
        release.set()
        # Wait for at least the most recent to make it out
        time.sleep(0.3)
    finally:
        bridge.stop()

    assert bridge.stats.uart_lines_in == 5
    assert bridge.stats.queue_drops >= 1
    # Most recent (i=4) must be among what was published.
    assert any('"i":4' in line for line in transport.published)


def test_is_valid_json_helper() -> None:
    assert _is_valid_json('{"a":1}')
    assert _is_valid_json('[]')
    assert _is_valid_json('null')
    assert not _is_valid_json('')
    assert not _is_valid_json('{')
    assert not _is_valid_json('not json')
