# Telegrobsidian

A single-binary C++20 daemon that keeps an Obsidian vault, a Telegram bot and a
Notion database in sync:

```
   Telegram message ──▶ vault/telegram_inbox/tg_<chat>_<id>.md ──▶ new Notion page
   vault/**/*.md     ─────────────────────────────────────────────▶ new Notion page
                          (files under notion_sync/ are pull-only)
   Notion page       ──▶ vault/notion_sync/<page-id>.md  (updated in place)
```

* **No build-time cloud dependencies** – `httplib.h` and `nlohmann/json.hpp` are
  vendored in `include/`, so `cmake -B build && cmake --build build` works offline.
* **No credentials in source** – everything is environment driven, with an
  `.env`-style file supported for systemd.
* **Tested end to end** – `scripts/smoke_test.py` drives the real binary against
  a mock Notion/Telegram API and checks the sync, loop-protection, retry and
  shutdown behaviour (63 assertions, no network, no credentials).

---

## Contents

1. [Requirements](#requirements)
2. [Build](#build)
3. [Configuration](#configuration)
4. [Running](#running)
5. [Telegram setup](#telegram-setup)
6. [Notion setup](#notion-setup)
7. [systemd deployment](#systemd-deployment)
8. [Operations](#operations)
9. [How the sync works](#how-the-sync-works)
10. [Testing](#testing)
11. [Troubleshooting](#troubleshooting)
12. [Not synced / limitations](#not-synced--limitations)

---

## Requirements

| | |
|---|---|
| OS | Linux (uses `inotify`; any kernel ≥ 2.6.25) |
| Compiler | GCC ≥ 11 or Clang ≥ 14 (needs C++20 threads/`std::jthread`) |
| Build | CMake ≥ 3.20, `pthread` |
| TLS | OpenSSL ≥ 3 for Notion and for talking to the Telegram API |
| Headers | vendored: `include/httplib.h` (cpp-httplib v0.59.0) and `include/nlohmann/json.hpp` (nlohmann/json v3.12.0) |
| Python | Only for the test suite (`python3`, stdlib only) |

The Notion API and `api.telegram.org` are HTTPS only, so OpenSSL is required in
practice. A build without TLS still runs, but logs that the Notion integration
is disabled and only serves the webhook endpoint.

## Build

```bash
# Debian/Ubuntu
sudo apt update && sudo apt install -y build-essential cmake libssl-dev python3

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
sudo cmake --install build            # optional: /usr/local/bin/telegrobsidian
```

Without CMake:

```bash
g++ -std=c++20 -O2 -Wall -Wextra -Iinclude main.cpp -o telegrobsidian \
    -DCPPHTTPLIB_OPENSSL_SUPPORT -lssl -lcrypto -lpthread
```

The two single-header dependencies are committed so the daemon builds on a VPS
without fetching anything (this repository was developed on a machine where
`raw.githubusercontent.com` was unreachable). To refresh them:

```bash
sha256sum include/httplib.h include/nlohmann/json.hpp
# dc1e4de3e0ef3a18…  include/httplib.h        (cpp-httplib v0.59.0)
# aaf127c04cb31c40…  include/nlohmann/json.hpp (nlohmann/json v3.12.0)

curl -fsSL -o include/httplib.h \
     https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.59.0/httplib.h
curl -fsSL -o include/nlohmann/json.hpp \
     https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp
```

`CPPHTTPLIB_OPENSSL_SUPPORT` is only a compile definition; the daemon always
enables certificate verification and hostname checks for HTTPS requests.

Verify the build:

```bash
./build/telegrobsidian --version        # version + TLS backend
./build/telegrobsidian --help
./build/telegrobsidian --self-test      # after configuring the environment
```

## Configuration

Everything is read from the environment; `--env-file <path>` (or the systemd
`EnvironmentFile=`) loads `KEY=VALUE` lines first, without overriding variables
that are already set. See [`.env.example`](.env.example) for the full annotated
list.

Minimum for Telegram + local-file capture:

```bash
export OBSIDIAN_VAULT_PATH=/var/lib/obsidian/vault
export TELEGRAM_WEBHOOK_SECRET="$(head -c 32 /dev/urandom | base64 | tr -d '/+=')"
export WEBHOOK_PORT=8080
```

Add Notion:

```bash
export NOTION_API_KEY=ntn_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
export NOTION_DATABASE_ID=1f2e3d4c5b6a79880a1b2c3d4e5f6071
export NOTION_PUSH_MIRROR_FILES=false      # keep mirrors pull-only
```

Useful knobs:

| Variable | Default | Meaning |
|---|---|---|
| `NOTION_POLL_INTERVAL_SEC` | `60` | How often Notion is queried (`5`–`86400`). |
| `DEBOUNCE_MS` | `300` | A file is considered settled after this much quiet. |
| `QUIESCE_MS` | `200` | Do not read a file whose mtime is younger than this. |
| `DEDUP_COOLDOWN_MS` | `5000` | Lifetime of the "we wrote this file" markers. |
| `WATCHER_RESCAN_SEC` | `120` | Re-scan interval for new directories. |
| `MAX_PUSH_ATTEMPTS` | `3` | Attempts for `429`/`5xx` answers. |
| `NOTION_CA_CERT_PATH` | *(empty)* | Extra PEM bundle to trust (private CA / TLS-inspection proxy). |
| `NOTION_TLS_VERIFY` | `true` | Certificate verification. Turning it off logs a warning; prefer `NOTION_CA_CERT_PATH`. |
| `USE_SYSTEM_CA_BUNDLE` | `true` | Point `SSL_CERT_FILE` at the system bundle when unset. |
| `PUSH_EXISTING_ON_STARTUP` | `false` | Upload files that already exist on a first run. |
| `LOG_LEVEL` | `info` | `trace`, `debug`, `info`, `warn`, `error`. |

`--self-test` validates the configuration against the live APIs and explains
the usual failures (wrong database id, integration without access, placeholder
tokens, missing CA bundle).

## Running

```bash
set -a; . /etc/telegrobsidian/env; set +a
./build/telegrobsidian                 # foreground, logs to stdout
./build/telegrobsidian --once          # catch up, one Notion poll, then exit
./build/telegrobsidian --self-test     # check credentials, then exit
```

Exit codes: `0` success, `1` fatal error (bad configuration, port in use, or an
unexpected failure in one of the worker threads).

Signals: `SIGINT`/`SIGTERM` stop accepting work, drain the queue (bounded by
`DRAIN_TIMEOUT_SEC`), save state and exit `0`.

## Telegram setup

1. Create a bot with [@BotFather](https://t.me/BotFather) and note the token.
2. Terminate TLS in front of the daemon (nginx/caddy) – Telegram refuses plain
   HTTP URLs. Example nginx location:

   ```nginx
   location /telegram-webhook {
       proxy_pass http://127.0.0.1:8080;
       proxy_set_header Host $host;
       client_max_body_size 2m;
   }
   ```

3. Register the webhook (either let the daemon do it by setting
   `TELEGRAM_PUBLIC_URL`, or run the helper):

   ```bash
   TELEGRAM_BOT_TOKEN=123:ABC TELEGRAM_PUBLIC_URL=https://bot.example.com \
   TELEGRAM_WEBHOOK_SECRET=$(openssl rand -hex 24) \
       scripts/register-telegram-webhook.sh
   ```

   The script also supports `--info` and `--delete`.

4. Set `TELEGRAM_WEBHOOK_SECRET` **on both sides**: it becomes the
   `X-Telegram-Bot-Api-Secret-Token` header, and the daemon answers `403` to any
   webhook call that does not present it.

Messages with text or a caption are stored as `telegram_inbox/tg_<chat>_<id>.md`
with frontmatter (`source`, `chat_id`, `message_id`, `date`, `author`) and
pushed to Notion as a page titled with the first line. Other update types
(stickers, joins, …) are acknowledged with `200` and ignored so Telegram does
not retry them. Re-deliveries of the same update do not create a second page.

## Notion setup

1. Create an internal integration at <https://www.notion.so/my-integrations>
   and copy its token (`secret_…` or `ntn_…`).
2. Create (or pick) a database with a **title** property. The daemon reads the
   schema and uses whichever property is the title, so `Name` is a convention,
   not a requirement.
3. Share the database with the integration: open the database → `•••` →
   `Connections` → add your integration. Without this the API answers `404`.
4. Put the token in `NOTION_API_KEY` and the 32 hex characters from the database
   URL in `NOTION_DATABASE_ID` (dashes are optional).

Notion API versions are pinned per request with `NOTION_VERSION`, default
`2022-06-28`. Notion's `2025-09-03` version splits a database into *data
sources* and moves the query endpoint to `/v1/data_sources/<id>/query`; this
daemon logs an explicit hint if the API rejects a query with a `data_source`
error. Pin `NOTION_VERSION` to the version your integration was created with.

## systemd deployment

```bash
sudo useradd --system --home /var/lib/obsidian --create-home obsidian
sudo -u obsidian mkdir -p /var/lib/obsidian/vault/notion_sync

sudo install -d -m 750 /etc/telegrobsidian
sudo install -m 600 .env.example /etc/telegrobsidian/env
sudoedit /etc/telegrobsidian/env                 # fill in paths and tokens

sudo install -m 644 deploy/obsidian-sync.service /etc/systemd/system/obsidian-sync.service
sudo systemctl daemon-reload
sudo systemctl enable --now obsidian-sync
systemctl status obsidian-sync
sudo journalctl -u obsidian-sync -f
```

The unit runs unprivileged with `ProtectSystem=strict`, an empty capability set
and `ReadWritePaths=/var/lib/obsidian`; adjust that path if your vault lives
elsewhere. Secrets stay in `/etc/telegrobsidian/env` (mode `600`) instead of the
unit file, where `systemctl cat` would expose them.

## Operations

* **Health/counters** – `GET /healthz` (or `curl 127.0.0.1:8080/healthz`)
  returns queue depth, cursor, cache size and counters:

  ```json
  {"ok":true,"version":"1.0.0","notion_enabled":true,"notion_cursor":"2026-06-01T09:12:33.480Z",
   "cached_pages":42,"queue_depth":0,"watched_directories":5,"stats":{"files_written":17,
   "events_suppressed":12,"pushes_succeeded":9,"pushes_failed":0,"inotify_overflows":0,...}}
  ```

  `events_suppressed` counts the daemon's own writes that the watcher
  recognised and dropped – it should grow together with `files_written`, never
  with `pushes_*`.
* **State** – `<vault>/.telegrobsidian/state.json` (override with `STATE_FILE`)
  holds the Notion cursor, the last rendered `last_edited_time` per page and the
  content fingerprint of every file that was pushed. It is written with
  `fsync`-free atomic rename; a corrupt file is ignored and rebuilt rather than
  trusted.
* **Logs** – `LOG_JSON=true` emits one JSON object per line for log shippers.
  `LOG_LEVEL=trace` adds per-event lines (`queued local change`, `suppressed
  self-induced event`, `coalesced event`, watch registrations).
* **Backups** – the state file is the only mutable state; losing it costs one
  full re-walk of the database (already-mirrored pages are rewritten only if
  their content changed).

## How the sync works

**Threads.** `http` (cpp-httplib server) → `sync-worker` (the only writer for
Telegram/local pushes) → `file-watcher` (recursive inotify) → `notion-poller`
(incremental pull, renders inline so its cursor is trustworthy) → the main
thread supervises, handles signals and saves state every 10 s. Every thread
body is wrapped so that an unexpected exception is logged and turns into a
clean shutdown instead of `std::terminate`.

**Loop protection.** A write by the daemon raises inotify events too. Each write
is recorded together with its timestamp; when an event arrives for that path the
observed mtime is compared with the recorded write time:

* `mtime <= write time + 50 ms` → the daemon's own echo, dropped (counted in
  `events_suppressed`);
* `mtime >` that → somebody else wrote afterwards, so it is a real change.

Markers expire after `DEDUP_COOLDOWN_MS` and are never consulted again, so a
crash cannot permanently mute a file. Pushes additionally carry a content
fingerprint: an unchanged file (a `touch`, an editor save without edits, a
Telegram re-delivery) never creates a second page.

**Debouncing without losing edits.** Every event bumps a per-path version. The
watcher hands a path to the worker once it has been quiet for `DEBOUNCE_MS`
*and* its version moved past the last scheduled one. A change that lands while
the previous task is still running therefore schedules another task; the worker
waits for `QUIESCE_MS` of mtime stability before reading, so half-written files
are never pushed.

**Startup catch-up.** The daemon watches the vault *before* it reads it, so
every later change raises an event. Files that changed while it was stopped are
queued and pushed at startup; files whose fingerprint is already recorded are
skipped, and directories that appear already populated (a `git checkout`, a
`cp -r`) have their markdown files queued as well, because their creation events
predate the watch. On a **first run** (no state file) the existing files are
registered as already synced instead of being uploaded - otherwise pointing the
daemon at a 2000-note vault would create 2000 pages. Set
`PUSH_EXISTING_ON_STARTUP=true` (or run `--once`) to opt into that bulk upload.

**Incremental pull.** Each poll queries with
`last_edited_time > cursor - 1s` (sorted descending) and renders only pages
whose `last_edited_time` differs from the recorded one. The cursor is committed
only after a complete walk; a failed walk is simply repeated and costs one
query, because rendered pages are skipped by version. Mirror files are written
through the same atomic writer as everything else and are never pushed back
unless `NOTION_PUSH_MIRROR_FILES=true`.

**Failure handling.** `429`/`5xx` are retried with exponential backoff up to
`MAX_PUSH_ATTEMPTS`. Connection errors are *not* retried automatically: the
request may have reached Notion, and a blind retry would duplicate a page (the
next edit of that file pushes again instead). A `4xx` is permanent, is logged
with a concrete hint (401 → token, 404 → sharing/database id, 400 → title
property) and is not retried for identical content. Pages whose blocks cannot be
fetched are retried on the next poll and skipped after 3 attempts with an
explicit error.

## Testing

```bash
ctest --test-dir build --output-on-failure          # or:
python3 scripts/smoke_test.py --binary build/telegrobsidian [--keep-tmp]
```

The suite starts `tests/mock_notion.py` (a stdlib HTTP stand-in for the Notion
and Telegram APIs, including `setWebhook`/`getMe` and injectable `400`/`429`
answers), runs the real daemon against it over loopback HTTP and asserts:

* Telegram → vault file → Notion page, including frontmatter and the title;
* startup catch-up (a change made while the daemon was down), files created
  together with their directory, first-run seeding and the `--once`/opt-in bulk
  upload;
* duplicate webhook deliveries and unchanged files never duplicate pages;
* the Notion → vault mirror, its markdown rendering (headings, annotations,
  links, lists, to-dos, code fences, quotes, unsupported blocks) and in-place
  updates;
* rapid edits collapse into exactly one push carrying the final content;
* mirror-directory files are not pushed, self-writes are suppressed
  (`events_suppressed > 0`) and no sync loop appears over several poll cycles;
* unchanged pages are neither re-fetched nor rewritten;
* `400` is reported without a retry, `429` is retried, the daemon survives both;
* webhook hardening (secret check, empty body, non-text updates);
* `--self-test`, `/healthz`, the state file and a clean `SIGTERM` shutdown.

For a TLS-enabled binary the suite adds a `TLS:` scenario: it mints a
self-signed certificate, serves the mock over HTTPS and asserts that the daemon
*rejects* the untrusted certificate, reports the failure instead of swallowing
it, and accepts the same endpoint once `NOTION_TLS_VERIFY=false` is set. That is
the only way to exercise the OpenSSL client without contacting the internet, and
it is skipped (and reported as skipped) for a binary built without TLS.

CI (`.github/workflows/ci.yml`) builds with `-Werror` for GCC and Clang, runs
the smoke test – TLS scenario included – on the OpenSSL build, and compiles the
no-TLS variant so the plain-HTTP path cannot rot.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `Notion sync disabled (set NOTION_API_KEY …)` | Keys missing, or the build lacks TLS. |
| `Notion query failed with HTTP 404` | Database id wrong, or the database is not shared with the integration. |
| `HTTP 401` | Token revoked/wrong type; create an internal integration token. |
| `HTTP 400 … property` | The title property could not be matched; check the database schema. |
| `HTTP 400 … data_source` | Newer API version required; see the note in [Notion setup](#notion-setup). |
| `certificate verification failed` | No CA bundle: install `ca-certificates` or set `SSL_CERT_FILE`. |
| Telegram `last_error_message` | The public HTTPS URL is unreachable, the certificate is invalid, or the daemon is not listening. |
| Nothing syncs from the vault | Check `LOG_LEVEL=trace`; a watch only exists for directories that were visible at scan time (`WATCHER_RESCAN_SEC`). |
| `inotify queue overflowed` | Huge burst of changes; the daemon re-scans, but consider raising `fs.inotify.max_user_watches`. |
| Port already in use | Another instance is running, or change `WEBHOOK_PORT`. |

## Not synced / limitations

* **Changes made while the daemon is stopped** are caught up at startup (one
  `stat`+hash per file). A file edited in place *while* the daemon runs is
  picked up from its inotify event; a change to a file in a directory created
  after the initial scan is picked up by the new-directory scan. Neither path
  requires a restart.
* **Deletions are not propagated** in either direction (a local `rm` and a
  Notion trash are logged, never mirrored destructively).
* **Only the title property is written** on push; other properties are Notion's
  to own. Page bodies are pushed as paragraphs (Notion blocks can only be
  appended once a page exists – full block-level sync would need a second
  update call).
* **Page bodies larger than 100 paragraphs** are truncated when pushed
  (a Notion request limit), and the truncation is logged.
* **Markdown↔blocks is lossy by design**: pushed content is plain text, pulled
  content is rendered from blocks (nested blocks up to 3 levels).
* Local edits to files under `notion_sync/` are ignored while
  `NOTION_PUSH_MIRROR_FILES=false` (they would fight with the puller).
* Attachments are referenced by URL, never downloaded.
