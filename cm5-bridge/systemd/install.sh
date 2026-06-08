#!/usr/bin/env bash
# Install the Quatra CM5 bridge as a systemd service on a Raspberry CM5
# running Raspberry Pi OS Bookworm (or any Debian 12-derived image).
#
# Run as root from inside the cm5-bridge/ folder of the repository:
#     sudo ./systemd/install.sh
#
# Idempotent: re-running upgrades the venv and reloads the unit.

set -euo pipefail

SERVICE_USER="${SERVICE_USER:-quatra}"
INSTALL_DIR="${INSTALL_DIR:-/opt/quatra-cm5-bridge}"
CONFIG_DIR="${CONFIG_DIR:-/etc/quatra}"
VENV_DIR="$INSTALL_DIR/.venv"

if [[ $EUID -ne 0 ]]; then
  echo "error: must run as root" >&2
  exit 1
fi

if ! id "$SERVICE_USER" >/dev/null 2>&1; then
  useradd --system --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
fi
usermod -aG dialout "$SERVICE_USER"

mkdir -p "$INSTALL_DIR" "$CONFIG_DIR"

cp -a "$(dirname "$0")/.."/{quatra_cm5_bridge,requirements.txt,config.example.toml,README.md} "$INSTALL_DIR/"

if [[ ! -d "$VENV_DIR" ]]; then
  python3 -m venv "$VENV_DIR"
fi
"$VENV_DIR/bin/pip" install --upgrade pip
"$VENV_DIR/bin/pip" install -r "$INSTALL_DIR/requirements.txt"

if [[ ! -f "$CONFIG_DIR/bridge.toml" ]]; then
  cp "$INSTALL_DIR/config.example.toml" "$CONFIG_DIR/bridge.toml"
  echo "Installed example config to $CONFIG_DIR/bridge.toml — EDIT IT before starting."
fi

chown -R "$SERVICE_USER:$SERVICE_USER" "$INSTALL_DIR" "$CONFIG_DIR"

install -m 0644 "$(dirname "$0")/quatra-cm5-bridge.service" /etc/systemd/system/

systemctl daemon-reload
echo
echo "Installed. Next steps:"
echo "  1. Edit $CONFIG_DIR/bridge.toml (transport, device_id, credentials)."
echo "  2. systemctl enable --now quatra-cm5-bridge.service"
echo "  3. journalctl -u quatra-cm5-bridge -f"
