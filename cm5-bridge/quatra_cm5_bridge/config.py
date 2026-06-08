"""Configuration loader for the bridge.

Reads a TOML file (preferred) or falls back to environment variables
when no file is supplied. Validates the minimal set of fields and
returns a frozen `BridgeConfig` dataclass.

Example config.toml is shipped alongside this package; see the
project README for the full schema.
"""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Mapping

if sys.version_info >= (3, 11):
    import tomllib
else:  # pragma: no cover - exercised on 3.10 CI only
    import tomli as tomllib


@dataclass(frozen=True)
class UartConfig:
    port: str = "/dev/ttyAMA0"
    baudrate: int = 115200
    bytesize: int = 8
    parity: str = "N"
    stopbits: int = 1
    read_timeout_s: float = 1.0
    reconnect_delay_s: float = 2.0


@dataclass(frozen=True)
class HttpConfig:
    base_url: str = ""
    publish_path: str = "/messages"
    poll_path: str = "/commands"
    poll_interval_s: float = 2.0
    request_timeout_s: float = 10.0
    auth_token: str | None = None
    verify_tls: bool = True


@dataclass(frozen=True)
class MqttConfig:
    host: str = "localhost"
    port: int = 1883
    username: str | None = None
    password: str | None = None
    client_id: str = "quatra-cm5-bridge"
    publish_topic: str = "quatra/device/{device_id}/up"
    subscribe_topic: str = "quatra/device/{device_id}/cmd"
    qos: int = 1
    use_tls: bool = False
    keepalive_s: int = 60


@dataclass(frozen=True)
class BridgeConfig:
    device_id: str
    transport: str  # "http" | "mqtt" | "stdout"
    log_level: str = "INFO"
    uart: UartConfig = field(default_factory=UartConfig)
    http: HttpConfig = field(default_factory=HttpConfig)
    mqtt: MqttConfig = field(default_factory=MqttConfig)


_VALID_TRANSPORTS = {"http", "mqtt", "stdout"}


def _section(raw: Mapping[str, Any], name: str) -> Mapping[str, Any]:
    value = raw.get(name, {})
    if not isinstance(value, Mapping):
        raise ValueError(f"config section [{name}] must be a table")
    return value


def _build_uart(raw: Mapping[str, Any]) -> UartConfig:
    return UartConfig(**{k: v for k, v in raw.items() if k in UartConfig.__annotations__})


def _build_http(raw: Mapping[str, Any]) -> HttpConfig:
    return HttpConfig(**{k: v for k, v in raw.items() if k in HttpConfig.__annotations__})


def _build_mqtt(raw: Mapping[str, Any]) -> MqttConfig:
    return MqttConfig(**{k: v for k, v in raw.items() if k in MqttConfig.__annotations__})


def load_config(path: str | os.PathLike[str] | None) -> BridgeConfig:
    """Load configuration from a TOML file (or env if path is None)."""
    if path is None:
        return _from_env()
    data = tomllib.loads(Path(path).read_text(encoding="utf-8"))
    return _from_mapping(data)


def _from_mapping(data: Mapping[str, Any]) -> BridgeConfig:
    bridge = _section(data, "bridge")
    device_id = bridge.get("device_id")
    transport = bridge.get("transport", "stdout")
    if not device_id:
        raise ValueError("[bridge].device_id is required")
    if transport not in _VALID_TRANSPORTS:
        raise ValueError(
            f"[bridge].transport must be one of {sorted(_VALID_TRANSPORTS)}, got {transport!r}"
        )
    return BridgeConfig(
        device_id=str(device_id),
        transport=str(transport),
        log_level=str(bridge.get("log_level", "INFO")),
        uart=_build_uart(_section(data, "uart")),
        http=_build_http(_section(data, "http")),
        mqtt=_build_mqtt(_section(data, "mqtt")),
    )


def _from_env() -> BridgeConfig:
    device_id = os.environ.get("QUATRA_DEVICE_ID")
    if not device_id:
        raise ValueError(
            "no config file supplied and QUATRA_DEVICE_ID env var is not set"
        )
    transport = os.environ.get("QUATRA_TRANSPORT", "stdout")
    if transport not in _VALID_TRANSPORTS:
        raise ValueError(f"QUATRA_TRANSPORT must be one of {sorted(_VALID_TRANSPORTS)}")
    return BridgeConfig(
        device_id=device_id,
        transport=transport,
        log_level=os.environ.get("QUATRA_LOG_LEVEL", "INFO"),
        uart=UartConfig(
            port=os.environ.get("QUATRA_UART_PORT", "/dev/ttyAMA0"),
            baudrate=int(os.environ.get("QUATRA_UART_BAUD", "115200")),
        ),
        http=HttpConfig(
            base_url=os.environ.get("QUATRA_HTTP_BASE_URL", ""),
            auth_token=os.environ.get("QUATRA_HTTP_TOKEN"),
        ),
        mqtt=MqttConfig(
            host=os.environ.get("QUATRA_MQTT_HOST", "localhost"),
            port=int(os.environ.get("QUATRA_MQTT_PORT", "1883")),
            username=os.environ.get("QUATRA_MQTT_USER"),
            password=os.environ.get("QUATRA_MQTT_PASS"),
        ),
    )
