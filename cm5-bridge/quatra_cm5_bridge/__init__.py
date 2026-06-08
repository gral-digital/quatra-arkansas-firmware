"""Quatra CM5 ⇄ webapp bridge.

A small daemon that runs on the Raspberry CM5 and shuttles JSON
messages between the ESP32-S3 (over UART) and the webapp (over a
pluggable transport: HTTPS, MQTT, or stdout for debugging).

The wire format on the UART side is the JSON protocol defined in
PRD §8 and implemented by the firmware library at:
    https://github.com/gral-digital/quatra-arkansas-firmware

This bridge is transport-agnostic: it does not interpret the JSON,
it only validates that each line is well-formed JSON and routes it
to/from the configured transport.
"""

__version__ = "1.2.0"
