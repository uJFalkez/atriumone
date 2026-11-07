#!/bin/bash
# Installs the module's services so they start at boot and restart on failure.
#
#   make                                   # first, as the normal user (never as root)
#   sudo deploy/install.sh                 # install + enable (start at the next boot)
#   sudo deploy/install.sh --now           # ... and start them right away
#   sudo deploy/install.sh --uninstall     # stop, disable and remove
#
# Logs:   journalctl -u navd -u missiond -f
# Status: systemctl status navd missiond
set -euo pipefail

PROJ=/home/pi3b/project
UNITS=(navd.service missiond.service)

if [ "$(id -u)" -ne 0 ]; then
    echo "rode com sudo: sudo $0 $*" >&2
    exit 1
fi

if [ "${1:-}" = "--uninstall" ]; then
    systemctl disable --now "${UNITS[@]}" 2>/dev/null || true
    for u in "${UNITS[@]}"; do rm -f "/etc/systemd/system/$u"; done
    systemctl daemon-reload
    echo "serviços removidos"
    exit 0
fi

# Built binaries must exist (build as the normal user, not here as root).
for b in build/navd build/camgrab; do
    if [ ! -x "$PROJ/$b" ]; then
        echo "falta $PROJ/$b: rode 'make' como usuário normal antes" >&2
        exit 1
    fi
done

# Groups the services rely on.
for g in i2c dialout gpio video; do
    if ! id -nG pi3b | tr ' ' '\n' | grep -qx "$g"; then
        echo "aviso: o usuário pi3b não está no grupo '$g' (sudo usermod -aG $g pi3b)" >&2
    fi
done

# Manual instances would hold the I2C bus, the control socket and port 8000.
if pgrep -x navd >/dev/null || pgrep -f 'mission/missiond.py' >/dev/null; then
    echo "aviso: há navd/missiond rodando à mão; pare-os antes de iniciar os serviços" >&2
fi

for u in "${UNITS[@]}"; do
    install -m 0644 "$PROJ/deploy/$u" "/etc/systemd/system/$u"
done
systemctl daemon-reload
systemctl enable "${UNITS[@]}"
echo "serviços instalados e habilitados no boot"

if [ "${1:-}" = "--now" ]; then
    systemctl restart navd.service
    systemctl restart missiond.service
    sleep 3
    systemctl --no-pager --lines=5 status "${UNITS[@]}" || true
fi
