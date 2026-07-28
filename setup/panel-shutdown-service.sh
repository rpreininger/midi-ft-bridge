#!/bin/sh
# ====================================================================
#  Panel HTTP shutdown service
#
#  Lets the iOS app (which cannot spawn ssh) shut a panel down over HTTP.
#  A tiny Python listener runs as the `stratojets` user (who has NOPASSWD
#  sudo) and, on an authenticated request, runs `sudo shutdown now`:
#
#      GET http://<panel>:8081/shutdown?token=<TOKEN>   -> 200, powers off
#      GET http://<panel>:8081/ping                     -> 200 "ok"
#
#  The token is a shared secret, also stored in the app's config.json
#  (shutdown_token). On the isolated gig LAN this is enough; the endpoint
#  does nothing but shut down, so its attack surface is one action.
#
#  The Mac build still uses ssh (unchanged); this endpoint is the iOS path
#  but works from any HTTP client, including a browser.
#
#  Run from the Mac (TOKEN is required):
#      ssh root@192.168.10.21 "TOKEN=<token> sh -s" < setup/panel-shutdown-service.sh
# ====================================================================
set -e

: "${TOKEN:?set TOKEN=<hex token> (must match config.json shutdown_token)}"
PORT="${PORT:-8081}"
RUN_USER=stratojets

SVC=/usr/local/sbin/panel-shutdown-service.py
UNIT=/etc/systemd/system/panel-shutdown.service
ENVF=/etc/default/panel-shutdown

echo "[1/5] Writing $SVC ..."
cat > "$SVC" <<'EOS'
#!/usr/bin/env python3
# Minimal, single-purpose HTTP shutdown endpoint. No dependencies beyond
# the stdlib. Only two routes; everything else is 404.
import os, subprocess
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

TOKEN = os.environ.get("SHUTDOWN_TOKEN", "")
PORT  = int(os.environ.get("SHUTDOWN_PORT", "8081"))

class Handler(BaseHTTPRequestHandler):
    def _reply(self, code, body):
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(body.encode())

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/ping":
            self._reply(200, "ok\n")
            return
        if u.path == "/shutdown":
            tok = (parse_qs(u.query).get("token") or [""])[0]
            if TOKEN and tok == TOKEN:
                self._reply(200, "shutting down\n")
                # Detach so the reply flushes before the box goes down.
                subprocess.Popen(["sudo", "shutdown", "now"])
            else:
                self._reply(403, "forbidden\n")
            return
        self._reply(404, "not found\n")

    def log_message(self, *args):
        pass  # keep the journal quiet

if __name__ == "__main__":
    HTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
EOS
chmod 0755 "$SVC"

echo "[2/5] Writing $ENVF (token, mode 0640 root:$RUN_USER) ..."
cat > "$ENVF" <<EOS
SHUTDOWN_TOKEN=$TOKEN
SHUTDOWN_PORT=$PORT
EOS
chown root:"$RUN_USER" "$ENVF"
chmod 0640 "$ENVF"

echo "[3/5] Writing $UNIT ..."
cat > "$UNIT" <<EOS
[Unit]
Description=Panel HTTP shutdown endpoint
After=network.target

[Service]
Type=simple
User=$RUN_USER
EnvironmentFile=$ENVF
ExecStart=/usr/bin/python3 $SVC
Restart=on-failure
# Only capability needed is invoking sudo shutdown; keep the rest minimal.
NoNewPrivileges=no

[Install]
WantedBy=multi-user.target
EOS

echo "[4/5] Enabling + starting ..."
systemctl daemon-reload
systemctl enable panel-shutdown.service >/dev/null 2>&1
systemctl restart panel-shutdown.service

echo "[5/5] Self-check (/ping) ..."
sleep 1
if command -v curl >/dev/null 2>&1; then
    curl -s -m 3 "http://127.0.0.1:$PORT/ping" && echo "    service up on :$PORT"
else
    python3 -c "import urllib.request,sys; print('   ', urllib.request.urlopen('http://127.0.0.1:$PORT/ping', timeout=3).read().decode().strip())"
fi
echo "Done."
