#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# One-shot build + install for a server (Debian/Ubuntu, x86_64 or arm64).
#
#   scripts/install.sh                      # build, test, install the binary
#   scripts/install.sh --service            # ... and install the systemd unit
#   scripts/install.sh --prefix ~/.local    # unprivileged install
#
# It never overwrites an existing environment file: secrets live there.
# ---------------------------------------------------------------------------
set -euo pipefail

PREFIX=/usr/local
VAULT=/var/lib/obsidian
ENV_FILE=/etc/telegrobsidian/env
PORT=8080
SERVICE_USER=obsidian
BUILD_TYPE=Release
JOBS=""
WITH_SERVICE=0
RUN_TESTS=1
REPO_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="$REPO_DIR/build"

usage() {
    sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

Options:
  --prefix DIR      install prefix                       (default /usr/local)
  --vault DIR       vault + state directory              (default /var/lib/obsidian)
  --env-file FILE   environment file for the service     (default /etc/telegrobsidian/env)
  --port N          webhook port written to the env file (default 8080)
  --user NAME       service account                      (default obsidian)
  --jobs N          parallel build jobs                  (default: all cores)
  --debug           build without optimisation
  --no-tests        skip the smoke test
  --service         install and enable the systemd unit
  --help            this text
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)    PREFIX="${2:?}"; shift 2 ;;
        --vault)     VAULT="${2:?}"; shift 2 ;;
        --env-file)  ENV_FILE="${2:?}"; shift 2 ;;
        --port)      PORT="${2:?}"; shift 2 ;;
        --user)      SERVICE_USER="${2:?}"; shift 2 ;;
        --jobs)      JOBS="${2:?}"; shift 2 ;;
        --debug)     BUILD_TYPE=Debug; shift ;;
        --no-tests)  RUN_TESTS=0; shift ;;
        --service)   WITH_SERVICE=1; shift ;;
        --help|-h)   usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# --- privileges ------------------------------------------------------------
# Only the install/enable steps need root; the build runs as the caller.
if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
elif command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
else
    SUDO=""
fi
as_root() { if [ -n "$SUDO" ]; then "$SUDO" "$@"; else "$@"; fi; }

# --- preflight -------------------------------------------------------------
say "Checking the toolchain"
missing=()
command -v cmake >/dev/null 2>&1 || missing+=("cmake")
CXX_BIN="$(command -v c++ || command -v g++ || true)"
[ -n "$CXX_BIN" ] || missing+=("g++")
# Ask the compiler the same question CMake will: can we include OpenSSL and link
# it? This works for every layout (multiarch, custom prefixes, LibreSSL, ...).
if [ -n "$CXX_BIN" ] && ! printf '#include <openssl/ssl.h>\nint main(){return 0;}\n' \
        | "$CXX_BIN" -x c++ - -o /dev/null -lssl -lcrypto >/dev/null 2>&1; then
    missing+=("libssl-dev")
fi
if [ ${#missing[@]} -gt 0 ]; then
    die "missing: ${missing[*]}
    sudo apt update && sudo apt install -y build-essential cmake libssl-dev python3"
fi
echo "cmake $(cmake --version | head -1 | awk '{print $3}'), $("$CXX_BIN" --version | head -1)"
echo "arch: $(uname -m), repo: $REPO_DIR"

[ -f "$REPO_DIR/main.cpp" ] || die "this script must live in <repo>/scripts/"

# --- build -----------------------------------------------------------------
say "Configuring ($BUILD_TYPE)"
cmake -S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" >/dev/null

say "Building"
if [ -n "$JOBS" ]; then
    cmake --build "$BUILD_DIR" --parallel "$JOBS"
else
    cmake --build "$BUILD_DIR" --parallel
fi
BINARY="$BUILD_DIR/telegrobsidian"
[ -x "$BINARY" ] || die "the build did not produce $BINARY"
"$BINARY" --version

# --- tests -----------------------------------------------------------------
if [ "$RUN_TESTS" -eq 1 ]; then
    if command -v python3 >/dev/null 2>&1; then
        say "Running the smoke test (no credentials, no internet)"
        python3 "$REPO_DIR/scripts/smoke_test.py" --binary "$BINARY" || die "the smoke test failed"
    else
        warn "python3 not found: skipping the smoke test"
    fi
fi

# --- install ---------------------------------------------------------------
say "Installing to $PREFIX"
as_root cmake --install "$BUILD_DIR" --prefix "$PREFIX"

say "Preparing the vault and the environment file"
as_root mkdir -p "$VAULT" "$(dirname "$ENV_FILE")"
if [ -e "$ENV_FILE" ]; then
    echo "keeping the existing $ENV_FILE (secrets are not touched)"
else
    as_root install -m 600 "$REPO_DIR/.env.example" "$ENV_FILE"
    # Point the sample at the paths chosen above.
    as_root sed -i "s|^OBSIDIAN_VAULT_PATH=.*|OBSIDIAN_VAULT_PATH=$VAULT/vault|" "$ENV_FILE"
    as_root sed -i "s|^WEBHOOK_PORT=.*|WEBHOOK_PORT=$PORT|" "$ENV_FILE"
    echo "created $ENV_FILE (mode 600) - fill in the tokens before starting"
fi

# --- systemd ---------------------------------------------------------------
if [ "$WITH_SERVICE" -eq 1 ]; then
    command -v systemctl >/dev/null 2>&1 || die "--service needs systemd"
    [ "$(id -u)" -eq 0 ] || [ -n "$SUDO" ] || die "--service needs root (or sudo)"
    say "Installing the systemd unit"
    as_root chmod 600 "$ENV_FILE"   # never world-readable: it holds the tokens
    if ! id "$SERVICE_USER" >/dev/null 2>&1; then
        as_root useradd --system --home "$VAULT" --shell /usr/sbin/nologin "$SERVICE_USER"
        id "$SERVICE_USER" >/dev/null 2>&1 || die "could not create the system account $SERVICE_USER
    create it yourself, or pass --user <existing-account>"
        echo "created the system account $SERVICE_USER"
    fi
    as_root chown -R "$SERVICE_USER" "$VAULT"
    unit_tmp=$(mktemp)
    sed -e "s|^User=.*|User=$SERVICE_USER|" \
        -e "s|^Group=.*|Group=$SERVICE_USER|" \
        -e "s|^ExecStart=.*|ExecStart=$PREFIX/bin/telegrobsidian|" \
        -e "s|^WorkingDirectory=.*|WorkingDirectory=$VAULT|" \
        -e "s|^ReadWritePaths=.*|ReadWritePaths=$VAULT|" \
        -e "s|^EnvironmentFile=.*|EnvironmentFile=$ENV_FILE|" \
        "$REPO_DIR/deploy/obsidian-sync.service" > "$unit_tmp"
    # The shipped unit assumes the default layout; say so instead of failing
    # later with a permission error from inside systemd.
    case "$VAULT" in
        /var/lib/obsidian*) ;;
        /home/*) warn "--vault is under /home but the unit sets ProtectHome=true:
         move the vault (recommended) or drop ProtectHome in
         /etc/systemd/system/obsidian-sync.service" ;;
        *) warn "custom --vault: check ProtectHome/ReadWritePaths in the installed unit" ;;
    esac
    as_root test -w /etc/systemd/system || die "cannot write /etc/systemd/system (run with sudo)"
    as_root install -m 644 "$unit_tmp" /etc/systemd/system/obsidian-sync.service
    rm -f "$unit_tmp"
    as_root systemctl daemon-reload
    as_root systemctl enable obsidian-sync >/dev/null 2>&1 || true
    echo "unit installed and enabled (start it with: sudo systemctl start obsidian-sync)"
fi

cat <<EOF

$(printf '\033[1mNext steps\033[0m')
 1. Fill in the secrets:            sudoedit $ENV_FILE
 2. Validate them against the live APIs:
      set -a; . $ENV_FILE; set +a
      $PREFIX/bin/telegrobsidian --self-test
 3. Start it:
      sudo systemctl start obsidian-sync && journalctl -u obsidian-sync -f
    or in the foreground:  $PREFIX/bin/telegrobsidian
 4. Expose the webhook over HTTPS and register it (Telegram needs TLS):
      sudo $REPO_DIR/scripts/register-telegram-webhook.sh --env-file $ENV_FILE
EOF
