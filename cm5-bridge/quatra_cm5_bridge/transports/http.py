"""HTTPS transport.

Upstream: POST each JSON line as the request body to
``{base_url}{publish_path}``. Downstream: long-poll
``{base_url}{poll_path}`` and feed each JSON line returned by the
server to the command handler.

The polling endpoint is expected to return either:

* an empty body when no commands are queued, OR
* a JSON array of objects, one per pending command, OR
* a single JSON object (treated as a one-element array), OR
* a newline-delimited list of JSON objects (NDJSON).

This matches what most simple webapp backends already expose
(REST + queue table) without forcing a specific framework.
"""

from __future__ import annotations

import json
import logging
import threading
import time
from typing import Any

import requests

from ..config import HttpConfig
from .base import CommandHandler, Transport

logger = logging.getLogger(__name__)


class HttpTransport(Transport):
    def __init__(self, cfg: HttpConfig, device_id: str) -> None:
        if not cfg.base_url:
            raise ValueError("[http].base_url is required for the http transport")
        self._cfg = cfg
        self._device_id = device_id
        self._session = requests.Session()
        if cfg.auth_token:
            self._session.headers["Authorization"] = f"Bearer {cfg.auth_token}"
        self._session.headers["Content-Type"] = "application/json"
        self._session.headers["X-Device-Id"] = device_id

        self._stop_event = threading.Event()
        self._poll_thread: threading.Thread | None = None
        self._on_command: CommandHandler | None = None

    # ----- Transport API ---------------------------------------------------

    def start(self, on_command: CommandHandler) -> None:
        self._on_command = on_command
        self._poll_thread = threading.Thread(
            target=self._poll_loop, name="quatra-http-poll", daemon=True
        )
        self._poll_thread.start()

    def stop(self, timeout_s: float = 5.0) -> None:
        self._stop_event.set()
        if self._poll_thread is not None:
            self._poll_thread.join(timeout=timeout_s)
        self._session.close()

    def publish(self, json_line: str) -> bool:
        url = self._cfg.base_url.rstrip("/") + self._cfg.publish_path
        try:
            resp = self._session.post(
                url,
                data=json_line.encode("utf-8"),
                timeout=self._cfg.request_timeout_s,
                verify=self._cfg.verify_tls,
            )
        except requests.RequestException:
            logger.exception("http publish failed: %s", url)
            return False
        if resp.status_code >= 300:
            logger.warning(
                "http publish non-2xx: %s %s body=%s",
                resp.status_code, url, resp.text[:200],
            )
            return False
        return True

    # ----- polling ---------------------------------------------------------

    def _poll_loop(self) -> None:
        url = self._cfg.base_url.rstrip("/") + self._cfg.poll_path
        while not self._stop_event.is_set():
            try:
                resp = self._session.get(
                    url,
                    params={"device_id": self._device_id},
                    timeout=self._cfg.request_timeout_s,
                    verify=self._cfg.verify_tls,
                )
            except requests.RequestException:
                logger.exception("http poll failed: %s", url)
                self._stop_event.wait(self._cfg.poll_interval_s)
                continue

            if resp.status_code == 204 or not resp.content:
                self._stop_event.wait(self._cfg.poll_interval_s)
                continue
            if resp.status_code >= 300:
                logger.warning("http poll non-2xx: %s %s", resp.status_code, url)
                self._stop_event.wait(self._cfg.poll_interval_s)
                continue

            self._dispatch(resp.text)
            self._stop_event.wait(self._cfg.poll_interval_s)

    def _dispatch(self, body: str) -> None:
        body = body.strip()
        if not body:
            return
        # Try JSON first (array or single object); fall back to NDJSON.
        try:
            parsed: Any = json.loads(body)
        except json.JSONDecodeError:
            for line in body.splitlines():
                line = line.strip()
                if line:
                    self._dispatch_one(line)
            return

        if isinstance(parsed, list):
            for item in parsed:
                self._dispatch_one(json.dumps(item, separators=(",", ":")))
        elif isinstance(parsed, dict):
            self._dispatch_one(json.dumps(parsed, separators=(",", ":")))
        else:
            logger.warning("http poll: unexpected JSON top-level type %r", type(parsed).__name__)

    def _dispatch_one(self, json_line: str) -> None:
        if self._on_command is None:
            return
        try:
            self._on_command(json_line)
        except Exception:  # noqa: BLE001
            logger.exception("http command handler raised; continuing")
