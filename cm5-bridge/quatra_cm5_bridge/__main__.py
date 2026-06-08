"""CLI entry point: ``python -m quatra_cm5_bridge --config /etc/quatra/bridge.toml``."""

from __future__ import annotations

import argparse
import logging
import sys

from . import __version__
from .bridge import Bridge, build_transport
from .config import load_config
from .uart_link import UartLink, default_serial_factory


def _build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="quatra-cm5-bridge",
        description="Bridge JSON between the Quatra ESP32-S3 (UART) and the webapp.",
    )
    p.add_argument(
        "--config", "-c",
        help="Path to a TOML config file. If omitted, env vars are read.",
        default=None,
    )
    p.add_argument(
        "--version", "-V",
        action="version",
        version=f"quatra-cm5-bridge {__version__}",
    )
    return p


def main(argv: list[str] | None = None) -> int:
    args = _build_arg_parser().parse_args(argv)
    cfg = load_config(args.config)

    logging.basicConfig(
        level=getattr(logging, cfg.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)-7s %(name)s — %(message)s",
    )

    serial_factory = default_serial_factory(
        port=cfg.uart.port,
        baudrate=cfg.uart.baudrate,
        bytesize=cfg.uart.bytesize,
        parity=cfg.uart.parity,
        stopbits=cfg.uart.stopbits,
        read_timeout_s=cfg.uart.read_timeout_s,
    )

    uart = UartLink(
        serial_factory=serial_factory,
        on_line=lambda _line: None,  # replaced by Bridge.start()
        reconnect_delay_s=cfg.uart.reconnect_delay_s,
    )
    transport = build_transport(cfg)
    bridge = Bridge(cfg, uart, transport)
    bridge.run_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
