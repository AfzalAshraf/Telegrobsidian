#!/usr/bin/env bash
# Register (or remove) the Telegram webhook for the Telegrobsidian daemon.
#
# Telegram only delivers updates to an HTTPS URL with a valid certificate, so
# put the daemon behind a TLS-terminating reverse proxy (nginx/caddy) or run it
# with a certificate of your own. The daemon itself speaks plain HTTP.
#
# Usage:
#   TELEGRAM_BOT_TOKEN=... TELEGRAM_PUBLIC_URL=https://bot.example.com \
#     scripts/register-telegram-webhook.sh
#
#   scripts/register-telegram-webhook.sh --info      # show current state
#   scripts/register-telegram-webhook.sh --delete    # remove the webhook
#
# Environment (or --env-file <path>):
#   TELEGRAM_BOT_TOKEN       required
#   TELEGRAM_PUBLIC_URL      required unless --info/--delete; base URL of the daemon
#   TELEGRAM_WEBHOOK_SECRET  optional but recommended (X-Telegram-Bot-Api-Secret-Token)
#   TELEGRAM_API_BASE        default https://api.telegram.org

set -euo pipefail

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

ACTION="register"
ENV_FILE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --info)   ACTION="info"; shift ;;
        --delete) ACTION="delete"; shift ;;
        --env-file) ENV_FILE="${2:-}"; [[ -n "$ENV_FILE" ]] || die "--env-file needs a path"; shift 2 ;;
        -h|--help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

if [[ -n "$ENV_FILE" ]]; then
    [[ -r "$ENV_FILE" ]] || die "cannot read $ENV_FILE"
    # shellcheck disable=SC1090
    set -a; . "$ENV_FILE"; set +a
fi

command -v curl >/dev/null || die "curl is required"

: "${TELEGRAM_BOT_TOKEN:?set TELEGRAM_BOT_TOKEN (from @BotFather)}"
API_BASE="${TELEGRAM_API_BASE:-https://api.telegram.org}"
API="$API_BASE/bot$TELEGRAM_BOT_TOKEN"

case "$ACTION" in
    register)
        : "${TELEGRAM_PUBLIC_URL:?set TELEGRAM_PUBLIC_URL, e.g. https://bot.example.com}"
        [[ "$TELEGRAM_PUBLIC_URL" == https://* ]] \
            || die "Telegram requires an https:// URL (got $TELEGRAM_PUBLIC_URL)"
        [[ "$TELEGRAM_PUBLIC_URL" == */ ]] && TELEGRAM_PUBLIC_URL="${TELEGRAM_PUBLIC_URL%/}"
        url="$TELEGRAM_PUBLIC_URL/telegram-webhook"

        args=(--data-urlencode "url=$url" --data-urlencode 'allowed_updates=["message","edited_message","channel_post","edited_channel_post"]')
        if [[ -n "${TELEGRAM_WEBHOOK_SECRET:-}" ]]; then
            args+=(--data-urlencode "secret_token=$TELEGRAM_WEBHOOK_SECRET")
        else
            printf 'warning: TELEGRAM_WEBHOOK_SECRET is not set; anyone who learns the URL can post fake updates\n' >&2
        fi

        printf 'registering webhook: %s\n' "$url"
        curl --silent --show-error --fail-with-body -X POST "$API/setWebhook" "${args[@]}"
        printf '\n'
        ;;

    delete)
        printf 'removing webhook\n'
        curl --silent --show-error --fail-with-body -X POST "$API/deleteWebhook" \
             --data-urlencode 'drop_pending_updates=false'
        printf '\n'
        ;;

    info)
        ;;
esac

printf '\ncurrent webhook state:\n'
curl --silent --show-error --fail-with-body "$API/getWebhookInfo" | {
    if command -v jq >/dev/null; then jq .; else cat; fi
}
printf '\n'

printf 'bot identity:\n'
curl --silent --show-error --fail-with-body "$API/getMe" | {
    if command -v jq >/dev/null; then jq '{ok, id: .result.id, username: .result.username}'; else cat; fi
}
printf '\n'

cat <<'EOF'
notes:
  * last_error_message in the webhook state usually means the proxy/certificate
    is not reachable from the internet yet, or the daemon is not listening.
  * Telegram retries failed deliveries, so a temporary failure is not fatal.
  * test delivery: send a message to the bot and watch `journalctl -u obsidian-sync -f`.
EOF
