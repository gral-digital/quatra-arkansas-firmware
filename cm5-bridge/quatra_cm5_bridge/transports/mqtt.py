"""MQTT transport.

Uplink: publish each JSON line on ``publish_topic`` (with ``{device_id}``
substitution). Downlink: subscribe to ``subscribe_topic`` and feed each
message payload to the command handler.

The MQTT payload is the raw JSON string the firmware emits/expects —
no envelope, no transformation.
"""

from __future__ import annotations

import logging
import ssl
import threading
from typing import Any

import paho.mqtt.client as mqtt

from ..config import MqttConfig
from .base import CommandHandler, Transport

logger = logging.getLogger(__name__)


class MqttTransport(Transport):
    def __init__(self, cfg: MqttConfig, device_id: str) -> None:
        self._cfg = cfg
        self._device_id = device_id
        self._publish_topic = cfg.publish_topic.format(device_id=device_id)
        self._subscribe_topic = cfg.subscribe_topic.format(device_id=device_id)

        try:
            client_api = mqtt.CallbackAPIVersion.VERSION2
            self._client = mqtt.Client(
                callback_api_version=client_api,
                client_id=cfg.client_id,
            )
        except AttributeError:  # paho-mqtt < 2.0 fallback
            self._client = mqtt.Client(client_id=cfg.client_id)

        if cfg.username:
            self._client.username_pw_set(cfg.username, cfg.password or "")
        if cfg.use_tls:
            self._client.tls_set(cert_reqs=ssl.CERT_REQUIRED)

        self._client.on_connect = self._on_connect
        self._client.on_message = self._on_message

        self._on_command: CommandHandler | None = None
        self._connected = threading.Event()

    # ----- Transport API ---------------------------------------------------

    def start(self, on_command: CommandHandler) -> None:
        self._on_command = on_command
        self._client.connect_async(self._cfg.host, self._cfg.port, keepalive=self._cfg.keepalive_s)
        self._client.loop_start()

    def stop(self, timeout_s: float = 5.0) -> None:
        try:
            self._client.disconnect()
        except Exception:  # noqa: BLE001
            logger.debug("mqtt disconnect error", exc_info=True)
        self._client.loop_stop()

    def publish(self, json_line: str) -> bool:
        info = self._client.publish(self._publish_topic, json_line, qos=self._cfg.qos)
        try:
            info.wait_for_publish(timeout=5.0)
        except Exception:  # noqa: BLE001
            logger.exception("mqtt publish wait failed")
            return False
        return info.rc == mqtt.MQTT_ERR_SUCCESS

    # ----- callbacks -------------------------------------------------------

    def _on_connect(self, client: mqtt.Client, _userdata: Any, _flags: Any, reason_code: Any, _props: Any = None) -> None:  # noqa: D401
        # paho v2 passes a ReasonCode object; v1 passed an int.
        rc_int = int(getattr(reason_code, "value", reason_code))
        if rc_int != 0:
            logger.error("mqtt connect failed rc=%s", rc_int)
            return
        logger.info("mqtt connected to %s:%s", self._cfg.host, self._cfg.port)
        client.subscribe(self._subscribe_topic, qos=self._cfg.qos)
        self._connected.set()

    def _on_message(self, _client: mqtt.Client, _userdata: Any, msg: mqtt.MQTTMessage) -> None:
        if self._on_command is None:
            return
        try:
            payload = msg.payload.decode("utf-8")
        except UnicodeDecodeError:
            logger.warning("mqtt dropped non-utf8 payload on %s", msg.topic)
            return
        try:
            self._on_command(payload)
        except Exception:  # noqa: BLE001
            logger.exception("mqtt command handler raised; continuing")
