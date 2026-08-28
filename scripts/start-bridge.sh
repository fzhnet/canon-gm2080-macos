#!/usr/bin/env bash
#
# Brings the bridge container up.  Run by the launchd agent at login, and
# safe to run by hand at any time (compose up -d is idempotent).
#
# launchd starts agents with a minimal PATH and, at login, usually *before*
# Docker Desktop's daemon is accepting connections - hence the explicit
# binary lookup and the wait loop.
set -euo pipefail

COMPOSE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../docker" && pwd)"

find_docker() {
    for candidate in \
        /usr/local/bin/docker \
        /opt/homebrew/bin/docker \
        /Applications/Docker.app/Contents/Resources/bin/docker
    do
        [[ -x "$candidate" ]] && { echo "$candidate"; return 0; }
    done
    command -v docker 2>/dev/null && return 0
    return 1
}

DOCKER="$(find_docker)" || {
    echo "[start-bridge] FATAL: docker binary not found" >&2
    exit 1
}

# Docker Desktop can take a while after login. Wait up to 10 minutes.
echo "[start-bridge] waiting for Docker daemon ..."
for _ in $(seq 1 60); do
    if "$DOCKER" info >/dev/null 2>&1; then
        echo "[start-bridge] Docker is up"
        break
    fi
    sleep 10
done

if ! "$DOCKER" info >/dev/null 2>&1; then
    echo "[start-bridge] FATAL: Docker daemon never became ready" >&2
    exit 1
fi

echo "[start-bridge] starting bridge container ..."
"$DOCKER" compose -f "${COMPOSE_DIR}/docker-compose.yml" up -d

echo "[start-bridge] done"
"$DOCKER" ps --filter name=canon-gm2080-bridge --format '  {{.Names}}  {{.Status}}'
