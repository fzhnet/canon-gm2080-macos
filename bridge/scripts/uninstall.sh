#!/usr/bin/env bash
#
# Removes everything install.sh created: the launchd agent, the macOS queue,
# and the bridge container.  The built image is kept unless --purge is given.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="${PROJECT_DIR}/docker/docker-compose.yml"

MAC_QUEUE="${MAC_QUEUE:-Canon_GM2080}"
AGENT_LABEL="com.local.canon-gm2080-bridge"
AGENT_PLIST="${HOME}/Library/LaunchAgents/${AGENT_LABEL}.plist"
SUPPORT_DIR="${HOME}/Library/Application Support/canon-gm2080-bridge"

PURGE=""
[[ "${1:-}" == "--purge" ]] && PURGE=yes

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
info() { printf '    %s\n' "$*"; }

say "Removing the launchd agent"
launchctl bootout "gui/$(id -u)/${AGENT_LABEL}" 2>/dev/null && info "agent unloaded" || info "agent was not loaded"
rm -f "$AGENT_PLIST" && info "plist removed"
rm -rf "$SUPPORT_DIR" && info "launcher removed"

say "Removing the macOS queue '${MAC_QUEUE}'"
if lpstat -p "$MAC_QUEUE" >/dev/null 2>&1; then
    lpadmin -x "$MAC_QUEUE" 2>/dev/null || sudo lpadmin -x "$MAC_QUEUE" 2>/dev/null || info "could not remove queue"
    info "queue removed"
else
    info "queue was not present"
fi

say "Stopping the bridge container"
if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    if [[ -n "$PURGE" ]]; then
        docker compose -f "$COMPOSE_FILE" down --rmi local
        info "container and image removed"
    else
        docker compose -f "$COMPOSE_FILE" down
        info "container removed (image kept; re-run with --purge to delete it)"
    fi
else
    info "Docker is not running - skipping"
fi

say "Done"
