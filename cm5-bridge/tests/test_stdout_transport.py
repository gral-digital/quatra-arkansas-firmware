"""StdoutTransport behaviour — no network involved."""

from __future__ import annotations

import io

from quatra_cm5_bridge.transports.stdout import StdoutTransport


def test_publishes_one_line_per_message() -> None:
    buf = io.StringIO()
    t = StdoutTransport(stream=buf)
    t.start(on_command=lambda _line: None)
    assert t.publish('{"a":1}') is True
    assert t.publish('{"b":2}\n') is True  # trailing newline must not double up
    t.stop()
    assert buf.getvalue() == '{"a":1}\n{"b":2}\n'


def test_publish_before_start_fails() -> None:
    t = StdoutTransport(stream=io.StringIO())
    assert t.publish('{"a":1}') is False
