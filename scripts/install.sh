#!/usr/bin/env bash
#
# Installs the Canon GM2080 print bridge:
#   1. builds and starts the Linux container that holds Canon's driver
#   2. adds a driverless macOS print queue pointing at it
#   3. installs a launchd agent so the bridge comes back after a reboot
#
# Safe to re-run; every step is idempotent.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="${PROJECT_DIR}/docker/docker-compose.yml"

MAC_QUEUE="${MAC_QUEUE:-Canon_GM2080}"
BRIDGE_PORT="${BRIDGE_PORT:-6631}"
BRIDGE_QUEUE="${BRIDGE_QUEUE:-GM2080}"
BRIDGE_URI="ipp://127.0.0.1:${BRIDGE_PORT}/printers/${BRIDGE_QUEUE}"

AGENT_LABEL="com.local.canon-gm2080-bridge"
AGENT_PLIST="${HOME}/Library/LaunchAgents/${AGENT_LABEL}.plist"
SUPPORT_DIR="${HOME}/Library/Application Support/canon-gm2080-bridge"
LAUNCHER="${SUPPORT_DIR}/start-bridge.sh"
LOG_DIR="${HOME}/Library/Logs"

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
info() { printf '    %s\n' "$*"; }
die()  { printf '\n\033[31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- preflight
say "Preflight"

command -v docker >/dev/null 2>&1 || die "docker not found. Install Docker Desktop first."
docker info >/dev/null 2>&1 || die "Docker daemon is not running. Start Docker Desktop and re-run."
info "docker: $(docker --version)"

[[ -f "$COMPOSE_FILE" ]] || die "compose file missing: $COMPOSE_FILE"

ENV_FILE="${PROJECT_DIR}/docker/.env"
ENV_EXAMPLE="${PROJECT_DIR}/docker/.env.example"

if [[ ! -f "$ENV_FILE" ]]; then
    [[ -f "$ENV_EXAMPLE" ]] || die "missing both docker/.env and docker/.env.example"
    cp "$ENV_EXAMPLE" "$ENV_FILE"
    die "Created docker/.env from the example. Set PRINTER_IP in it, then re-run this script."
fi

# shellcheck disable=SC1090
set -a; source "$ENV_FILE"; set +a
PRINTER_IP="${PRINTER_IP:-}"
PRINTER_PORT="${PRINTER_PORT:-9100}"

[[ -n "$PRINTER_IP" ]] || die "PRINTER_IP is not set in docker/.env"
[[ "$PRINTER_IP" != "192.168.1.50" ]] || \
    die "docker/.env still holds the example address. Set PRINTER_IP to your printer's real IP."

info "printer: ${PRINTER_IP}:${PRINTER_PORT}"

if nc -z -G 5 "$PRINTER_IP" "$PRINTER_PORT" 2>/dev/null; then
    info "printer is reachable on tcp/${PRINTER_PORT}"
else
    info "WARNING: cannot reach ${PRINTER_IP}:${PRINTER_PORT} - is it on and on this network?"
    info "         Install will continue; printing will fail until it is."
fi

# ------------------------------------------------------------------ bridge
say "Building and starting the bridge container"
docker compose -f "$COMPOSE_FILE" up -d --build

info "waiting for the bridge to answer Get-Printer-Attributes ..."

# A plain HTTP 200 on the queue page is NOT enough: `lpadmin -m everywhere`
# builds its PPD from a full Get-Printer-Attributes response, and the
# scheduler serves the web page slightly before it will answer that.  Testing
# the weaker condition makes the install race and fail intermittently.
GPA_TEST="$(mktemp -t gm2080gpa)"
cat > "$GPA_TEST" <<'GPA'
{
  OPERATION Get-Printer-Attributes
  GROUP operation-attributes-tag
  ATTR charset attributes-charset utf-8
  ATTR language attributes-natural-language en
  ATTR uri printer-uri $uri
}
GPA

bridge_ready() {
    ipptool -q "$BRIDGE_URI" "$GPA_TEST" >/dev/null 2>&1
}

ready=""
for _ in $(seq 1 45); do
    if bridge_ready; then ready=yes; break; fi
    sleep 2
done
rm -f "$GPA_TEST"

[[ -n "$ready" ]] || {
    docker compose -f "$COMPOSE_FILE" logs --tail 30
    die "bridge did not come up - see the container logs above"
}
info "bridge is serving ${BRIDGE_URI}"

# -------------------------------------------------------------- mac queue
say "Adding the macOS print queue '${MAC_QUEUE}'"

# Admin users are in _lpadmin and can usually do this without sudo; fall back
# if this machine is configured otherwise.
add_queue() {
    lpadmin -p "$MAC_QUEUE" \
            -v "$BRIDGE_URI" \
            -m everywhere \
            -D "Canon GM2080 (via bridge)" \
            -L "${PRINTER_IP} via local bridge" \
            -E \
            -o printer-is-shared=false
}

# Retry rather than escalating straight to sudo: the usual cause of a failure
# here is the bridge still settling, not missing privileges.
queue_ok=""
for attempt in 1 2 3; do
    if err="$(add_queue 2>&1)"; then queue_ok=yes; break; fi
    info "attempt ${attempt} failed: ${err:-unknown error}"
    sleep 3
done

if [[ -z "$queue_ok" ]]; then
    info "retrying with sudo (you may be prompted for your password)"
    if ! sudo lpadmin -p "$MAC_QUEUE" \
                      -v "$BRIDGE_URI" \
                      -m everywhere \
                      -D "Canon GM2080 (via bridge)" \
                      -L "${PRINTER_IP} via local bridge" \
                      -E \
                      -o printer-is-shared=false
    then
        die "could not create the macOS queue. Last error: ${err:-unknown}"
    fi
fi

cupsenable "$MAC_QUEUE" 2>/dev/null || sudo cupsenable "$MAC_QUEUE" 2>/dev/null || true
cupsaccept "$MAC_QUEUE" 2>/dev/null || sudo cupsaccept "$MAC_QUEUE" 2>/dev/null || true
info "queue created: $(lpstat -v "$MAC_QUEUE" 2>/dev/null || echo "$MAC_QUEUE")"

# --------------------------------------------------------------- launchd
say "Installing the launchd agent"

# The agent CANNOT live in (or read from) this project directory: launchd
# spawns it without TCC rights to ~/Documents, so anything there fails with
# "Operation not permitted" (exit 126).  ~/Library/Application Support is not
# TCC-protected, so the launcher goes there - and it deliberately only calls
# `docker start`, which needs no access to the compose file at all.
mkdir -p "$SUPPORT_DIR" "$LOG_DIR" "$(dirname "$AGENT_PLIST")"

cat > "$LAUNCHER" <<'LAUNCHER_EOF'
#!/usr/bin/env bash
# Auto-generated by install.sh - do not edit; re-run the installer instead.
#
# Starts the already-created bridge container at login.  Docker's own
# `restart: unless-stopped` policy usually handles this; this agent is the
# backstop for when the container was stopped manually or the policy did not
# fire.
set -euo pipefail

find_docker() {
    for c in /usr/local/bin/docker /opt/homebrew/bin/docker \
             /Applications/Docker.app/Contents/Resources/bin/docker; do
        [[ -x "$c" ]] && { echo "$c"; return 0; }
    done
    command -v docker 2>/dev/null && return 0
    return 1
}

DOCKER="$(find_docker)" || { echo "docker not found" >&2; exit 1; }

# Docker Desktop can take a while to come up after login.
for _ in $(seq 1 60); do
    "$DOCKER" info >/dev/null 2>&1 && break
    sleep 10
done
"$DOCKER" info >/dev/null 2>&1 || { echo "Docker never became ready" >&2; exit 1; }

if [[ "$("$DOCKER" inspect -f '{{.State.Running}}' canon-gm2080-bridge 2>/dev/null)" == "true" ]]; then
    echo "bridge already running"
    exit 0
fi

echo "starting bridge container ..."
"$DOCKER" start canon-gm2080-bridge
LAUNCHER_EOF

chmod +x "$LAUNCHER"

cat > "$AGENT_PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>${AGENT_LABEL}</string>

    <key>ProgramArguments</key>
    <array>
        <string>/bin/bash</string>
        <string>${LAUNCHER}</string>
    </array>

    <key>RunAtLoad</key>
    <true/>

    <!-- The launcher waits for Docker Desktop itself, so one run at login is
         enough; KeepAlive would just respawn it in a loop. -->
    <key>KeepAlive</key>
    <false/>

    <key>StandardOutPath</key>
    <string>${LOG_DIR}/${AGENT_LABEL}.log</string>
    <key>StandardErrorPath</key>
    <string>${LOG_DIR}/${AGENT_LABEL}.log</string>
</dict>
</plist>
PLIST

launchctl bootout "gui/$(id -u)/${AGENT_LABEL}" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$AGENT_PLIST"
info "launcher: ${LAUNCHER}"
info "agent:    ${AGENT_PLIST}"
info "log:      ${LOG_DIR}/${AGENT_LABEL}.log"

# Prove it actually runs, rather than discovering exit 126 at next reboot.
launchctl kickstart -k "gui/$(id -u)/${AGENT_LABEL}" 2>/dev/null || true
sleep 3
agent_rc="$(launchctl print "gui/$(id -u)/${AGENT_LABEL}" 2>/dev/null | awk '/last exit code/ {print $NF}')"
if [[ "$agent_rc" == "0" || -z "$agent_rc" ]]; then
    info "agent self-test: OK"
else
    info "WARNING: agent exited with code ${agent_rc}; see the log above"
fi

# ----------------------------------------------------------------- done
say "Done"
cat <<SUMMARY
    macOS queue : ${MAC_QUEUE}
    bridge      : ${BRIDGE_URI}
    printer     : ${PRINTER_IP}:${PRINTER_PORT}

    Test it with:
        lp -d ${MAC_QUEUE} /System/Library/Documentation/Acknowledgements.rtf

    Watch the bridge with:
        docker compose -f ${COMPOSE_FILE} logs -f
SUMMARY
