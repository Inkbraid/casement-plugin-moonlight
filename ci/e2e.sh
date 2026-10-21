#!/usr/bin/env bash
# Runs the end-to-end tests against a real Sunshine host on this machine:
#
#   ci/e2e.sh [build-dir]
#
# It installs the pinned Sunshine release (and Xvfb, PulseAudio) when they
# are missing, then starts, all private to this run:
#
#   - Xvfb on display :31, its screen filled with one known colour, so the
#     tests can check the streamed picture;
#   - PulseAudio with a 440 Hz tone, which Sunshine captures and streams;
#   - Sunshine with the software encoder, its config, state and log in a
#     temporary folder, on its default ports (47984-48010, web UI 47990).
#
# The tests (label "e2e") run the plugin under Wine through tools/sim. The
# processes started here are stopped by their PIDs at the end.
set -euo pipefail

BUILD=$(realpath "${1:-build}")
SUNSHINE_VERSION=2026.516.143833
SUNSHINE_DEB_SHA256=6df8900f23c9c056252eea51639507b8239a1d1241308ab8923cb402b0ca653b
DISPLAY_NUMBER=31
COLOUR=3366cc
WORK=$(mktemp -d)
PIDS=()

log() { echo "e2e: $*" >&2; }

cleanup() {
    local status=$?
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
    for pid in "${PIDS[@]}"; do
        for _ in $(seq 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
        kill -9 "$pid" 2>/dev/null || true
    done
    # The logs stay with the build, for CI artifacts and a look afterwards.
    mkdir -p "$BUILD/e2e-logs"
    cp "$WORK"/*.log "$WORK"/*.out "$BUILD/e2e-logs/" 2>/dev/null || true
    if [ "$status" -ne 0 ] && [ -f "$WORK/sunshine.log" ]; then
        log "Sunshine's log, last lines:"
        grep -v -E "^(x|y|width|height|keyAction|keyCode|modifiers|flags|action|button|scrollAmt1|deltaX|deltaY) +\[" \
            "$WORK/sunshine.log" | tail -n 60 >&2 || true
    fi
    rm -rf "$WORK"
    exit "$status"
}
trap cleanup EXIT

sudo_() { if [ "$(id -u)" = 0 ]; then "$@"; else sudo "$@"; fi; }

install_packages() {
    local missing=()
    command -v Xvfb >/dev/null || missing+=(xvfb)
    command -v xsetroot >/dev/null || missing+=(x11-xserver-utils)
    command -v pulseaudio >/dev/null || missing+=(pulseaudio)
    command -v pactl >/dev/null || missing+=(pulseaudio-utils)
    if ! sunshine --version 2>/dev/null | grep -q "$SUNSHINE_VERSION"; then
        curl -sSLf --retry 3 -o "$WORK/sunshine.deb" \
            "https://github.com/LizardByte/Sunshine/releases/download/v$SUNSHINE_VERSION/sunshine-ubuntu-24.04-amd64.deb"
        echo "$SUNSHINE_DEB_SHA256  $WORK/sunshine.deb" | sha256sum -c --quiet -
        missing+=("$WORK/sunshine.deb")
    fi
    if [ "${#missing[@]}" -gt 0 ]; then
        log "installing ${missing[*]}"
        # Other jobs on the machine may use apt too.
        sudo_ flock /tmp/apt.lock apt-get install -y --no-install-recommends "${missing[@]}" >"$WORK/apt.log" 2>&1 ||
            { cat "$WORK/apt.log" >&2; exit 1; }
    fi
}

wait_for() {  # seconds description command...
    local seconds=$1 what=$2
    shift 2
    for _ in $(seq $((seconds * 10))); do
        if "$@" >/dev/null 2>&1; then return 0; fi
        sleep 0.1
    done
    log "$what didn't come up"
    return 1
}

install_packages

# The screen. -noreset keeps the root colour when xsetroot disconnects.
Xvfb ":$DISPLAY_NUMBER" -screen 0 1280x720x24 -nolisten tcp -noreset >"$WORK/xvfb.log" 2>&1 &
PIDS+=($!)
wait_for 10 "Xvfb" xsetroot -display ":$DISPLAY_NUMBER" -solid "#$COLOUR"

# Sound: Sunshine streams what plays on its own sink; the tone follows the
# default sink there. The plugin plays into e2e_out instead (PULSE_SINK),
# so its output doesn't loop back into the stream.
mkdir -p "$WORK/pulse"
export PULSE_SERVER="unix:$WORK/pulse/native"
XDG_RUNTIME_DIR="$WORK/pulse" HOME="$WORK" pulseaudio --daemonize=no --exit-idle-time=-1 --disallow-exit -n \
    --use-pid-file=no --system=no \
    --load="module-native-protocol-unix auth-anonymous=1 socket=$WORK/pulse/native" \
    --load="module-null-sink sink_name=e2e_out" --load="module-always-sink" >"$WORK/pulse.log" 2>&1 &
PIDS+=($!)
wait_for 10 "PulseAudio" pactl info
pactl load-module module-sine frequency=440 >/dev/null
export PULSE_SINK=e2e_out

# Sunshine, with everything in $WORK.
PASSWORD=$(head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n')
cat >"$WORK/apps.json" <<EOF
{ "env": {}, "apps": [ { "name": "Desktop", "image-path": "desktop.png" } ] }
EOF
cat >"$WORK/sunshine.conf" <<EOF
sunshine_name = casement-e2e
min_log_level = debug
log_path = $WORK/sunshine.log
file_state = $WORK/state.json
credentials_file = $WORK/credentials.json
file_apps = $WORK/apps.json
pkey = $WORK/key.pem
cert = $WORK/cert.pem
encoder = software
sw_preset = ultrafast
hevc_mode = 2
capture = x11
system_tray = disabled
controller = disabled
upnp = disabled
notify_pre_releases = disabled
origin_web_ui_allowed = pc
EOF
HOME="$WORK" sunshine "$WORK/sunshine.conf" --creds casement "$PASSWORD" >"$WORK/creds.log" 2>&1
DISPLAY=":$DISPLAY_NUMBER" HOME="$WORK" sunshine "$WORK/sunshine.conf" >"$WORK/sunshine.out" 2>&1 &
PIDS+=($!)
wait_for 60 "Sunshine" curl -sSf --noproxy '*' "http://127.0.0.1:47989/serverinfo?uniqueid=0&uuid=0"
codecs=$(curl -sSf --noproxy '*' "http://127.0.0.1:47989/serverinfo?uniqueid=0&uuid=0" |
    sed -n 's/.*<ServerCodecModeSupport>\([0-9]*\)<.*/\1/p')
codecs=${codecs:-0}
log "Sunshine $SUNSHINE_VERSION is up (codecs $codecs)"

windows_path() { echo "Z:${1//\//\\}"; }
mkdir -p "$WORK/plugin-data"
export MOONLIGHT_E2E_HOST=127.0.0.1
export MOONLIGHT_E2E_WEB_USER=casement
export MOONLIGHT_E2E_WEB_PASSWORD=$PASSWORD
export MOONLIGHT_E2E_COLOUR=$COLOUR
export MOONLIGHT_E2E_SUNSHINE_LOG
MOONLIGHT_E2E_SUNSHINE_LOG=$(windows_path "$WORK/sunshine.log")
export MOONLIGHT_E2E_DATA
MOONLIGHT_E2E_DATA=$(windows_path "$WORK/plugin-data")
export MOONLIGHT_E2E_HEVC=$(( (codecs & 0x100) != 0 ))
export MOONLIGHT_E2E_AUDIO=1
export CASEMENT_E2E=1
export WINEDEBUG=${WINEDEBUG:--all}

ctest --test-dir "$BUILD" -L e2e --output-on-failure
