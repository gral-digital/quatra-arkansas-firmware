"""Transport interface and shared types.

A transport publishes JSON messages going **up** (ESP32 → webapp)
and delivers commands coming **down** (webapp → ESP32). Each
implementation lives in its own module and is selected by the
`[bridge].transport` config key.
"""

from __future__ import annotations

import abc
from typing import Callable

# A command line is the raw JSON string we will write back to UART.
CommandHandler = Callable[[str], None]


class Transport(abc.ABC):
    """Base class for all cloud transports."""

    @abc.abstractmethod
    def start(self, on_command: CommandHandler) -> None:
        """Begin shuttling traffic. ``on_command`` is invoked per inbound JSON line."""

    @abc.abstractmethod
    def stop(self, timeout_s: float = 5.0) -> None:
        """Stop background workers and release resources."""

    @abc.abstractmethod
    def publish(self, json_line: str) -> bool:
        """Publish one JSON message upward. Returns False on transport failure."""
