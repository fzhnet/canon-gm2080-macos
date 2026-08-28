#!/usr/bin/env bash
#
# Boots CUPS inside the bridge container and (re)creates the GM2080 queue.
# Everything is driven by environment variables so the image stays generic;
# docker-compose.yml supplies the real values.
set -euo pipefail

# No default: docker-compose.yml requires PRINTER_IP to be set in docker/.env,
# so an unset value here means the container was started some other way.
PRINTER_IP="${PRINTER_IP:?PRINTER_IP is not set - see docker/.env.example}"
PRINTER_PORT="${PRINTER_PORT:-9100}"
QUEUE_NAME="${QUEUE_NAME:-GM2080}"
QUEUE_INFO="${QUEUE_INFO:-Canon GM2080 (bridge)}"
PPD_FILE="${PPD_FILE:-/usr/share/ppd/canongm2080.ppd}"
DEVICE_URI="${DEVICE_URI:-socket://${PRINTER_IP}:${PRINTER_PORT}}"

log() { printf '[bridge] %s\n' "$*"; }

if [[ ! -r "$PPD_FILE" ]]; then
    log "FATAL: PPD not found: $PPD_FILE"
    log "Available Canon PPDs:"
    ls /usr/share/ppd/ | grep -i canon | sed 's/^/  /' || true
    exit 1
fi

# Reachability is advisory only.  The printer sits behind a VPN that may come
# up after the container does, and CUPS will retry the job either way.
log "Checking ${PRINTER_IP}:${PRINTER_PORT} ..."
if nc -z -w5 "$PRINTER_IP" "$PRINTER_PORT" 2>/dev/null; then
    log "  reachable"
else
    log "  NOT reachable - is the VPN up? Queue will still be created; jobs will retry."
fi

# CUPS needs these to exist and be writable across container restarts.
mkdir -p /var/spool/cups /var/cache/cups /var/log/cups /var/run/cups
chown -R root:lp /var/spool/cups /var/cache/cups /var/run/cups

log "Starting cupsd ..."
cupsd -f &
CUPSD_PID=$!

shutdown() {
    log "Stopping cupsd ..."
    kill -TERM "$CUPSD_PID" 2>/dev/null || true
    wait "$CUPSD_PID" 2>/dev/null || true
    exit 0
}
trap shutdown TERM INT

# Wait for the scheduler to actually accept IPP before touching lpadmin.
#
# Do NOT test this with `lpstat -r`: it exits 0 even when the scheduler is
# down (it just prints "scheduler is not running"), which silently hides a
# cupsd.conf syntax error until lpadmin fails with a useless message.
scheduler_up() {
    lpstat -r 2>/dev/null | grep -q 'is running'
}

for _ in $(seq 1 30); do
    scheduler_up && break
    # If cupsd has already died there is no point waiting out the timeout.
    kill -0 "$CUPSD_PID" 2>/dev/null || break
    sleep 1
done

if ! scheduler_up; then
    log "FATAL: cupsd did not come up. Last errors:"
    tail -n 20 /var/log/cups/error_log 2>/dev/null | sed 's/^/  /'
    exit 1
fi

log "Configuring queue '${QUEUE_NAME}' -> ${DEVICE_URI}"
lpadmin -p "$QUEUE_NAME" \
        -v "$DEVICE_URI" \
        -P "$PPD_FILE" \
        -D "$QUEUE_INFO" \
        -L "via ${PRINTER_IP}" \
        -E \
        -o printer-is-shared=true \
        -o printer-error-policy=retry-job

cupsenable "$QUEUE_NAME"     >/dev/null 2>&1 || true
cupsaccept "$QUEUE_NAME"     >/dev/null 2>&1 || true
lpadmin   -d "$QUEUE_NAME"   >/dev/null 2>&1 || true

log "Queue ready:"
lpstat -v "$QUEUE_NAME" 2>&1 | sed 's/^/  /'
log "Add on macOS:  ipp://127.0.0.1:6631/printers/${QUEUE_NAME}"

wait "$CUPSD_PID"
