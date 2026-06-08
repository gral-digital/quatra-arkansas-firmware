"""Config loader tests."""

from __future__ import annotations

import textwrap
from pathlib import Path

import pytest

from quatra_cm5_bridge.config import load_config


def _write(tmp_path: Path, body: str) -> Path:
    p = tmp_path / "bridge.toml"
    p.write_text(textwrap.dedent(body))
    return p


def test_minimal_toml(tmp_path: Path) -> None:
    cfg = load_config(_write(tmp_path, """
        [bridge]
        device_id = "demo-01"
        transport = "stdout"
    """))
    assert cfg.device_id == "demo-01"
    assert cfg.transport == "stdout"
    assert cfg.uart.baudrate == 115200  # default
    assert cfg.log_level == "INFO"


def test_full_toml(tmp_path: Path) -> None:
    cfg = load_config(_write(tmp_path, """
        [bridge]
        device_id = "demo-01"
        transport = "http"
        log_level = "DEBUG"

        [uart]
        port = "/dev/ttyUSB0"
        baudrate = 230400
        reconnect_delay_s = 0.5

        [http]
        base_url = "https://api.example.com"
        publish_path = "/msg"
        poll_path = "/cmd"
        poll_interval_s = 0.25
        auth_token = "tok"
        verify_tls = false

        [mqtt]
        host = "broker.local"
        port = 8883
    """))
    assert cfg.transport == "http"
    assert cfg.uart.port == "/dev/ttyUSB0"
    assert cfg.uart.baudrate == 230400
    assert cfg.http.base_url == "https://api.example.com"
    assert cfg.http.auth_token == "tok"
    assert cfg.http.verify_tls is False
    assert cfg.mqtt.host == "broker.local"


def test_missing_device_id_rejected(tmp_path: Path) -> None:
    with pytest.raises(ValueError, match="device_id"):
        load_config(_write(tmp_path, """
            [bridge]
            transport = "stdout"
        """))


def test_unknown_transport_rejected(tmp_path: Path) -> None:
    with pytest.raises(ValueError, match="transport"):
        load_config(_write(tmp_path, """
            [bridge]
            device_id = "x"
            transport = "carrier_pigeon"
        """))


def test_unknown_keys_silently_ignored(tmp_path: Path) -> None:
    cfg = load_config(_write(tmp_path, """
        [bridge]
        device_id = "x"
        transport = "stdout"

        [uart]
        port = "/dev/ttyAMA0"
        future_knob = 42
    """))
    assert cfg.uart.port == "/dev/ttyAMA0"
