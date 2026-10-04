#!/usr/bin/env bash
# Install (or update) the stats service. Run it ON the machine you want to monitor; it asks for sudo.
#
#   bash install.sh                      # port 8250, LAN detected automatically
#   LAN=10.0.0.0/24 PORT=9000 bash install.sh
#
# It installs bc250_stats.py as a systemd service and, if ufw is active, opens the port to your LAN only.
set -euo pipefail
cd "$(dirname "$0")"
PORT="${PORT:-8250}"

# The subnet of the interface that holds the default route, e.g. 192.168.1.0/24
detect_lan() {
  python3 - <<'PY'
import ipaddress, re, subprocess
try:
    route = subprocess.run(["ip", "-4", "-o", "route", "get", "1.1.1.1"], capture_output=True, text=True).stdout
    src = re.search(r"src (\S+)", route).group(1)
    dev = re.search(r"dev (\S+)", route).group(1)
    addr = subprocess.run(["ip", "-4", "-o", "addr", "show", "dev", dev], capture_output=True, text=True).stdout
    prefix = re.search(re.escape(src) + r"/(\d+)", addr).group(1)
    print(ipaddress.ip_interface(f"{src}/{prefix}").network)
except Exception:
    pass
PY
}
LAN="${LAN:-$(detect_lan)}"

sudo install -Dm755 bc250_stats.py /opt/bc250-stats/bc250_stats.py
sed "s/--port [0-9]*/--port ${PORT}/" bc250-stats.service | sudo tee /etc/systemd/system/bc250-stats.service > /dev/null
sudo systemctl daemon-reload
sudo systemctl enable bc250-stats
sudo systemctl restart bc250-stats          # also picks up a newly installed version

if command -v ufw > /dev/null && sudo ufw status | grep -q "Status: active"; then
  if [ -n "$LAN" ]; then
    sudo ufw allow from "$LAN" to any port "$PORT" proto tcp comment 'bc250-stats'
    echo "ufw: port $PORT opened to $LAN only"
  else
    echo "ufw is active but your LAN could not be detected: run  sudo ufw allow from <your-subnet> to any port $PORT proto tcp"
  fi
else
  echo "No active ufw found. Make sure port $PORT is reachable from the device (firewalld: sudo firewall-cmd --add-port=$PORT/tcp --permanent)."
fi

sleep 1
curl -s "localhost:$PORT/health" > /dev/null && echo "bc250-stats is up on :$PORT   ->   http://$(hostname -I 2>/dev/null | awk '{print $1}'):$PORT/stats"
