#!/usr/bin/env bash
# Remove the stats service. Run it ON the monitored machine; it asks for sudo.
#   PORT=9000 bash uninstall.sh      # only if you installed on a non-default port
set -euo pipefail
PORT="${PORT:-8250}"

sudo systemctl disable --now bc250-stats 2>/dev/null || true
sudo rm -f /etc/systemd/system/bc250-stats.service
sudo rm -rf /opt/bc250-stats
sudo systemctl daemon-reload

if command -v ufw > /dev/null; then
  # deletes every rule that carries the bc250-stats comment
  for n in $(sudo ufw status numbered | grep -n "bc250-stats" | sed -E 's/^[0-9]+:\[ *([0-9]+)\].*/\1/' | sort -rn); do
    yes | sudo ufw delete "$n" > /dev/null || true
  done
fi
echo "bc250-stats removed."
