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
                # -n: fail fast if sudo would prompt (misconfigured sudoers)
                # rather than hang; the drop-in below grants NOPASSWD shutdown.
                subprocess.Popen(["sudo", "-n", "shutdown", "now"])
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

echo "[3/6] Granting $RUN_USER NOPASSWD shutdown (sudoers drop-in) ..."
# Self-contained: don't depend on the panel's baseline sudoers (they differ --
# stratopanel lacked this, so its endpoint replied 200 but never powered off).
# Validate before keeping it; a broken sudoers file would lock out sudo.
SUDOERS=/etc/sudoers.d/panel-shutdown
TMP=$(mktemp)
cat > "$TMP" <<EOS
# Installed by panel-shutdown-service.sh — lets the HTTP endpoint power the
# panel off without a password. Only the shutdown binaries, nothing else.
$RUN_USER ALL=(root) NOPASSWD: /sbin/shutdown, /usr/sbin/shutdown
EOS
if visudo -cf "$TMP" >/dev/null 2>&1; then
    install -m 0440 -o root -g root "$TMP" "$SUDOERS"
    echo "    $SUDOERS installed + validated"
else
    echo "    ERROR: generated sudoers failed validation; not installing" >&2
fi
rm -f "$TMP"

echo "[4/6] Writing $UNIT ..."
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

echo "[5/6] Enabling + starting ..."
systemctl daemon-reload
systemctl enable panel-shutdown.service >/dev/null 2>&1
systemctl restart panel-shutdown.service

echo "[6/6] Self-check (/ping + sudo grant) ..."
# Test as the service user, not root, or the check is meaningless.
sudo -u "$RUN_USER" sudo -n /sbin/shutdown --help >/dev/null 2>&1 \
    && echo "    sudo NOPASSWD shutdown as $RUN_USER: OK" \
    || echo "    WARN: $RUN_USER still cannot sudo shutdown"
sleep 1
if command -v curl >/dev/null 2>&1; then
    curl -s -m 3 "http://127.0.0.1:$PORT/ping" && echo "    service up on :$PORT"
else
    python3 -c "import urllib.request,sys; print('   ', urllib.request.urlopen('http://127.0.0.1:$PORT/ping', timeout=3).read().decode().strip())"
fi
echo "Done."
