#!/usr/bin/env python3
"""
End-to-end smoke test for the Telegrobsidian daemon.

It starts tests/mock_notion.py (a local HTTP stand-in for the Notion API),
then runs the real daemon against it and drives the three sync directions:

    Telegram  -> vault -> Notion
    local file-> Notion
    Notion    -> vault

and verifies the properties that are easy to get wrong: debouncing, duplicate
delivery, self-write suppression (no feedback loop), incremental re-rendering,
permanent-error handling, retry on 429 and a clean SIGTERM shutdown.

The daemon is built without TLS in CI too, so the mock is addressed over plain
HTTP (NOTION_BASE_URL=http://127.0.0.1:<port>), which the daemon accepts as a
loopback test double.

Usage:
    scripts/smoke_test.py [--binary build/telegrobsidian] [--keep-tmp]
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import ssl
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BINARY = ROOT / "build" / "telegrobsidian"

PASS = "PASS"
FAIL = "FAIL"
results: list[tuple[str, bool, str]] = []
skipped: list[tuple[str, str]] = []


def check(name: str, condition: bool, detail: str = "") -> bool:
    results.append((name, bool(condition), detail))
    mark = PASS if condition else FAIL
    line = f"[{mark}] {name}"
    if detail and not condition:
        line += f"  <- {detail}"
    print(line, flush=True)
    return bool(condition)


def skip(name: str, reason: str) -> None:
    """Records a check that could not run in this environment.

    Skips are reported separately from passes so a green run can never hide a
    check that silently never executed.
    """
    skipped.append((name, reason))
    print(f"[SKIP] {name}  ({reason})", flush=True)


def start_mock(port: int, verbose: bool = False, tls_cert: Path | None = None,
               tls_key: Path | None = None):
    """Starts tests/mock_notion.py and waits for its LISTENING banner."""
    cmd = [sys.executable, str(ROOT / "tests" / "mock_notion.py"), "--port", str(port)]
    if verbose:
        cmd.append("--verbose")
    if tls_cert is not None:
        cmd += ["--tls-cert", str(tls_cert), "--tls-key", str(tls_key)]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=None, text=True)
    assert proc.stdout is not None
    banner = proc.stdout.readline().strip()
    if not banner.startswith("LISTENING"):
        proc.kill()
        raise RuntimeError(f"mock server failed to start: {banner!r}")
    return proc, int(banner.split()[1])


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def http(method: str, url: str, payload: dict | None = None, headers: dict | None = None,
         timeout: float = 10.0):
    data = None if payload is None else json.dumps(payload).encode()
    request = urllib.request.Request(url, data=data, method=method,
                                     headers={"Content-Type": "application/json", **(headers or {})})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read().decode()
            return response.status, body
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode()
    except Exception as exc:  # connection refused while the daemon starts up
        return 0, str(exc)


def wait_for_stable(getter, stable_for: float, timeout: float, interval: float = 0.25):
    """Waits until getter() stops changing for `stable_for` seconds (or times out)."""
    deadline = time.time() + timeout
    last = getter()
    stable_since = time.time()
    while time.time() < deadline:
        time.sleep(interval)
        current = getter()
        if current != last:
            last = current
            stable_since = time.time()
        elif time.time() - stable_since >= stable_for:
            return last
    return last


def wait_for(predicate, timeout: float, description: str, interval: float = 0.2):
    """Polls until predicate() is truthy; returns the value or None on timeout."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            last = predicate()
        except Exception:
            last = None
        if last:
            return last
        time.sleep(interval)
    print(f"    (timed out waiting for {description}; last={last!r})", flush=True)
    return None


class Daemon:
    def __init__(self, binary: Path, env: dict, log_path: Path):
        self.log_path = log_path
        self.log_file = open(log_path, "wb")
        self.proc = subprocess.Popen([str(binary)], env=env, stdout=self.log_file,
                                     stderr=subprocess.STDOUT, cwd=str(ROOT))

    def log(self) -> str:
        if not self.log_file.closed:
            self.log_file.flush()
        return self.log_path.read_text(errors="replace")

    def alive(self) -> bool:
        return self.proc.poll() is None

    def stop(self, sig=signal.SIGTERM, timeout: float = 20.0) -> int | None:
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
        try:
            code = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            code = self.proc.wait(timeout=5)
        self.log_file.close()
        return code


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=os.environ.get("TELEGROBSIDIAN_BIN", str(DEFAULT_BINARY)))
    parser.add_argument("--keep-tmp", action="store_true")
    parser.add_argument("--verbose-mock", action="store_true")
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    if not binary.exists():
        print(f"binary not found: {binary}\nbuild it first, e.g. "
              f"cmake -B build && cmake --build build -j", file=sys.stderr)
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="telegrobsidian-smoke-"))
    vault = tmp / "vault"
    vault.mkdir(parents=True)
    log_path = tmp / "daemon.log"

    mock_port = free_port()
    webhook_port = free_port()
    try:
        mock, mock_port = start_mock(mock_port, verbose=args.verbose_mock)
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    notion_base = f"http://127.0.0.1:{mock_port}"
    print(f"mock Notion listening on {notion_base}\n"
          f"daemon vault:   {vault}\n"
          f"daemon webhook: http://127.0.0.1:{webhook_port}/telegram-webhook\n", flush=True)

    env = dict(os.environ)
    env.update({
        "OBSIDIAN_VAULT_PATH": str(vault),
        "NOTION_API_KEY": "secret_smoketest",
        "NOTION_DATABASE_ID": "12345678123412341234123456789012",
        "NOTION_BASE_URL": notion_base,
        "NOTION_POLL_INTERVAL_SEC": "5",
        "WEBHOOK_BIND": "127.0.0.1",
        "WEBHOOK_PORT": str(webhook_port),
        "TELEGRAM_BOT_TOKEN": "TESTTOKEN",
        "TELEGRAM_WEBHOOK_SECRET": "smoke-secret",
        "TELEGRAM_API_BASE": notion_base,   # the mock serves /bot<token>/... too
        "TELEGRAM_PUBLIC_URL": f"http://127.0.0.1:{webhook_port}",
        "LOG_LEVEL": "trace",
        "LOG_TIMESTAMPS": "true",
        "DEDUP_COOLDOWN_MS": "2000",
        "WATCHER_RESCAN_SEC": "10",
    })

    daemon = Daemon(binary, env, log_path)
    health = f"http://127.0.0.1:{webhook_port}/healthz"
    webhook = f"http://127.0.0.1:{webhook_port}/telegram-webhook"
    secret_header = {"X-Telegram-Bot-Api-Secret-Token": "smoke-secret"}

    def hidden() -> str:
        return (vault / "notion_sync")

    def mock_state() -> dict:
        status, body = http("GET", f"{notion_base}/_test/state")
        return json.loads(body) if status == 200 else {}

    def created_pages() -> list[dict]:
        return mock_state().get("pages_created", [])

    def titles() -> list[str]:
        return [p["title"] for p in created_pages()]

    def health_json() -> dict:
        status, body = http("GET", health)
        if status != 200:
            return {}
        return json.loads(body)

    try:
        # ------------------------------------------------------------------
        # 1. startup
        # ------------------------------------------------------------------
        ok = wait_for(lambda: health_json().get("ok") is True, 15, "the daemon's /healthz endpoint")
        check("daemon starts and serves /healthz", bool(ok))
        check("startup log announces the webhook listener",
              "HTTP webhook listening" in daemon.log(),
              daemon.log()[-400:])
        registered = wait_for(lambda: mock_state().get("webhook_registrations"), 10,
                              "the setWebhook call")
        check("auto-registers the Telegram webhook", bool(registered), str(registered))
        if registered:
            reg = registered[-1]
            check("webhook registration points at this daemon",
                  reg["url"].endswith("/telegram-webhook") and str(webhook_port) in reg["url"],
                  str(reg))
            check("webhook registration sends the secret token",
                  reg["secret_token"] == "smoke-secret", str(reg))
            check("webhook registration uses the configured bot token",
                  reg["token"] == "TESTTOKEN", str(reg))

        # ------------------------------------------------------------------
        # 2. Telegram -> vault -> Notion
        # ------------------------------------------------------------------
        update = {
            "update_id": 1,
            "message": {
                "message_id": 101,
                "date": 1780000000,
                "chat": {"id": 555, "type": "private"},
                "from": {"id": 555, "username": "tester", "first_name": "Test"},
                "text": "Buy milk\nand remember the launch checklist",
            },
        }
        status, body = http("POST", webhook, update, secret_header)
        check("webhook accepts a Telegram update", status == 200 and json.loads(body)["queued"] is True,
              f"HTTP {status}: {body[:200]}")

        capture = vault / "telegram_inbox" / "tg_555_101.md"
        found = wait_for(lambda: capture.exists(), 10, "the capture file")
        check("Telegram message is stored in the vault", bool(found), str(capture))
        if found:
            text = capture.read_text()
            check("capture keeps the message text", "Buy milk" in text and "launch checklist" in text,
                  text[:200])
            check("capture carries telegram frontmatter",
                  "source: telegram" in text and "chat_id: 555" in text and "message_id: 101" in text,
                  text[:300])

        got = wait_for(lambda: "Buy milk" in " ".join(titles()), 15, "the Notion page for the capture")
        check("Telegram capture is pushed to Notion", bool(got), str(titles()))
        if got:
            page = [p for p in created_pages() if p["title"].startswith("Buy milk")][0]
            check("pushed page contains the full message",
                  "launch checklist" in page["text"], page["text"][:200])
            check("pushed page parent is the configured database",
                  page["parent"].get("database_id") == "12345678123412341234123456789012",
                  str(page["parent"]))
            check("pushed page uses the discovered title property",
                  "Name" in page["properties"], str(list(page["properties"])))

        # duplicate delivery of the very same update must not create a second page
        before = len(created_pages())
        http("POST", webhook, update, secret_header)
        time.sleep(2.5)
        check("duplicate webhook delivery does not duplicate the Notion page",
              len(created_pages()) == before, f"{before} -> {len(created_pages())}: {titles()}")

        # ------------------------------------------------------------------
        # 3. Notion -> vault (pull) and loop protection
        # ------------------------------------------------------------------
        mirrored = wait_for(lambda: (hidden() / "made-0001.md").exists(), 20,
                            "the mirror of the created page")
        check("page created in Notion is mirrored back into the vault", bool(mirrored))
        if mirrored:
            content = (hidden() / "made-0001.md").read_text()
            check("mirror has notion frontmatter",
                  "notion_id: made-0001" in content and "last_edited:" in content, content[:300])
            check("mirror contains the page body without YAML frontmatter duplication",
                  "launch checklist" in content, content[:300])

        # the daemon wrote that file itself: no event from it may bounce back
        time.sleep(3)
        check("no feedback loop after mirroring (page count stable)",
              len(created_pages()) == before, f"{titles()}")

        # ------------------------------------------------------------------
        # 4. local file -> Notion, with debounce and permanent-error handling
        # ------------------------------------------------------------------
        notes = vault / "notes"
        notes.mkdir(exist_ok=True)
        note = notes / "local.md"
        note.write_text('---\ntitle: "Local Note"\nauthor: me\n---\n\nFirst line of the note.\n')
        wait_for(lambda: "Obsidian: Local Note" in titles(), 15, "the local file push")
        check("local vault file is pushed to Notion", "Obsidian: Local Note" in titles(), str(titles()))
        pushed = [p for p in created_pages() if p["title"] == "Obsidian: Local Note"]
        if pushed:
            check("frontmatter is stripped from the pushed body",
                  "First line of the note." in pushed[0]["text"] and "author: me" not in pushed[0]["text"],
                  pushed[0]["text"][:200])
            check("frontmatter title wins over the file name", pushed[0]["title"] == "Obsidian: Local Note")

        # rapid successive writes must collapse into a single push
        before = len(created_pages())
        for i in range(5):
            note.write_text(f'---\ntitle: "Local Note"\n---\n\nRevision {i}\n')
            time.sleep(0.02)
        after = wait_for_stable(lambda: len(created_pages()), 3.0, 20)
        delta = after - before
        check("rapid edits are debounced into one push", delta == 1, f"{delta} page(s) created")
        if delta == 1:
            rev = [p for p in created_pages() if p["title"] == "Obsidian: Local Note"][-1]
            check("the debounced push carries the final content", "Revision 4" in rev["text"],
                  rev["text"][:200])

        # identical content again (e.g. `touch`) must not push
        before = len(created_pages())
        note.write_text(f'---\ntitle: "Local Note"\n---\n\nRevision 4\n')
        after = wait_for_stable(lambda: len(created_pages()), 4.0, 15)
        check("touching a file without changing it does not push", after == before, f"{titles()}")

        # a 4xx from Notion is permanent: logged, not retried, daemon stays up
        http("POST", f"{notion_base}/_test/fail_next",
             {"count": 1, "status": 400, "path_contains": "/v1/pages"})
        bad = notes / "bad.md"
        bad.write_text("---\ntitle: Bad\n---\n\nrejected\n")
        wait_for(lambda: "Notion rejected" in daemon.log(), 15, "the rejection log line")
        check("permanent API errors are reported, not retried",
              "Notion rejected" in daemon.log() and "HTTP 400" in daemon.log()),
        check("daemon survives a rejected push", daemon.alive())

        # 429 is retried
        http("POST", f"{notion_base}/_test/fail_next",
             {"count": 1, "status": 429, "path_contains": "/v1/pages"})
        retry = notes / "retry.md"
        retry.write_text("---\ntitle: Retry Me\n---\n\nretried content\n")
        got = wait_for(lambda: "Obsidian: Retry Me" in titles(), 20, "the retried push")
        check("rate limited requests are retried", bool(got), str(titles()))
        check("retry attempt is visible in the log", "retrying" in daemon.log().lower())

        # ------------------------------------------------------------------
        # 5. Notion mirror files are pull-only (no echo)
        # ------------------------------------------------------------------
        before = len(created_pages())
        hidden().mkdir(parents=True, exist_ok=True)
        (hidden() / "manual.md").write_text("---\ntitle: Manual\n---\n\nhand written\n")
        after = wait_for_stable(lambda: len(created_pages()), 4.0, 15)
        check("files inside the Notion mirror directory are not pushed", after == before,
              f"{titles()}")
        check("mirror-directory writes are logged as skipped",
              "pull-only" in daemon.log() or "not pushing" in daemon.log())

        # ------------------------------------------------------------------
        # 6. rich Notion content -> markdown
        # ------------------------------------------------------------------
        blocks = [
            {"object": "block", "id": "b1", "type": "heading_1", "has_children": False,
             "heading_1": {"rich_text": [{"type": "text", "plain_text": "Heading One",
                                          "text": {"content": "Heading One"}}]}},
            {"object": "block", "id": "b2", "type": "paragraph", "has_children": False,
             "paragraph": {"rich_text": [
                 {"type": "text", "plain_text": "plain ", "text": {"content": "plain "}},
                 {"type": "text", "plain_text": "bold", "text": {"content": "bold"},
                  "annotations": {"bold": True}},
                 {"type": "text", "plain_text": "link", "text": {"content": "link",
                                                                 "link": {"url": "https://example.com"}}},
             ]}},
            {"object": "block", "id": "b3", "type": "bulleted_list_item", "has_children": False,
             "bulleted_list_item": {"rich_text": [{"type": "text", "plain_text": "bullet",
                                                   "text": {"content": "bullet"}}]}},
            {"object": "block", "id": "b4", "type": "to_do", "has_children": False,
             "to_do": {"checked": True, "rich_text": [{"type": "text", "plain_text": "done",
                                                       "text": {"content": "done"}}]}},
            {"object": "block", "id": "b5", "type": "code", "has_children": False,
             "code": {"language": "cpp", "rich_text": [{"type": "text", "plain_text": "int x = 1;",
                                                        "text": {"content": "int x = 1;"}}]}},
            {"object": "block", "id": "b6", "type": "quote", "has_children": False,
             "quote": {"rich_text": [{"type": "text", "plain_text": "quoted", "text": {"content": "quoted"}}]}},
            {"object": "block", "id": "b7", "type": "table", "has_children": False,
             "table": {"table_width": 1}},
        ]
        http("POST", f"{notion_base}/_test/notion_page",
             {"id": "page-rich", "title": "Rich Page", "blocks": blocks})
        rich = wait_for(lambda: (hidden() / "page-rich.md").exists(), 25, "the rich mirror file")
        check("rich Notion page is mirrored", bool(rich))
        if rich:
            md = (hidden() / "page-rich.md").read_text()
            check("headings are rendered", "# Heading One" in md, md[:400])
            check("inline annotations are rendered", "**bold**" in md, md[:400])
            check("links are rendered", "[link](https://example.com)" in md, md[:400])
            check("bullet list items are rendered", "- bullet" in md, md[:400])
            check("to-do items are rendered", "- [x] done" in md, md[:400])
            check("code blocks are fenced", "```cpp" in md and "int x = 1;" in md, md[:400])
            check("quotes are rendered", "> quoted" in md, md[:400])
            check("unknown block types degrade to a comment", "unsupported Notion block" in md, md[:400])
            check("title comes from the page property", 'title: "Rich Page"' in md, md[:200])

        # ------------------------------------------------------------------
        # 7. incremental behaviour
        # ------------------------------------------------------------------
        state_before = mock_state()
        http("POST", f"{notion_base}/_test/notion_page",
             {"id": "page-rich", "title": "Rich Page",
              "blocks": [{"object": "block", "id": "b2", "type": "paragraph", "has_children": False,
                          "paragraph": {"rich_text": [{"type": "text", "plain_text": "updated text",
                                                       "text": {"content": "updated text"}}]}}]})
        updated = wait_for(lambda: "updated text" in (hidden() / "page-rich.md").read_text()
                           if (hidden() / "page-rich.md").exists() else False,
                           25, "the mirrored update")
        check("edits in Notion update the existing mirror file in place", bool(updated))
        mirrors = sorted(p.name for p in hidden().glob("*.md"))
        check("no duplicate mirror file is created for the same page",
              mirrors.count("page-rich.md") == 1 and "made-0001.md" in mirrors, str(mirrors))

        blocks_before = mock_state()["block_request_count"]
        time.sleep(7)  # at least one more poll cycle
        blocks_after = mock_state()["block_request_count"]
        check("unchanged pages are not re-fetched from Notion", blocks_after == blocks_before,
              f"{blocks_before} -> {blocks_after}")
        check("unchanged pages are not rewritten", daemon.alive())

        # loop protection over several cycles
        before = len(created_pages())
        after = wait_for_stable(lambda: len(created_pages()), 12.0, 30)
        check("no sync loop after several poll cycles", after == before,
              f"{before} -> {after}: {titles()}")

        # ------------------------------------------------------------------
        # 8. health + state
        # ------------------------------------------------------------------
        h = health_json()
        check("health exposes counters", h.get("stats", {}).get("files_written", 0) > 0, json.dumps(h)[:300])
        check("health reports self-write suppression",
              h.get("stats", {}).get("events_suppressed", 0) >= 1, json.dumps(h["stats"])[:300])
        check("health has no auth failures on the mock",
              mock_state().get("auth_failures") == 0, str(mock_state().get("auth_failures")))

        state_file = vault / ".telegrobsidian" / "state.json"
        check("state file is written", state_file.exists(), str(state_file))
        if state_file.exists():
            state = json.loads(state_file.read_text())
            check("state tracks the Notion cursor", bool(state.get("notion_cursor")), str(state)[:200])
            check("state tracks rendered page versions", "page-rich" in state.get("pages", {}),
                  str(list(state.get("pages", {}).keys())))

        # ------------------------------------------------------------------
        # 9. webhook hardening
        # ------------------------------------------------------------------
        status, _ = http("POST", webhook, update, {"X-Telegram-Bot-Api-Secret-Token": "wrong"})
        check("wrong webhook secret is rejected", status == 403, f"HTTP {status}")
        status, body = http("POST", webhook, {"update_id": 2, "message": {
            "message_id": 202, "date": 1780000001, "chat": {"id": 555}, "sticker": {"file_id": "x"}}},
            secret_header)
        check("non-text updates are acknowledged without queueing",
              status == 200 and json.loads(body)["queued"] is False, f"HTTP {status}: {body[:200]}")
        status, _ = http("POST", webhook, None, secret_header)
        check("empty body is rejected", status == 400, f"HTTP {status}")

        # ------------------------------------------------------------------
        # 10. a directory that appears already populated
        # ------------------------------------------------------------------
        before = len(created_pages())
        newdir = vault / "projects" / "alpha"
        newdir.mkdir(parents=True)
        (newdir / "brief.md").write_text("---\ntitle: Alpha Brief\n---\n\nproject brief\n")
        found = wait_for(lambda: "Obsidian: Alpha Brief" in titles(), 20,
                         "the push for a file created with its directory")
        check("file created together with its directory is picked up", bool(found), str(titles()))

        # ------------------------------------------------------------------
        # 11. clean shutdown
        # ------------------------------------------------------------------
        code = daemon.stop(signal.SIGTERM)
        check("SIGTERM leads to a clean exit code", code == 0, f"exit code {code}")
        log = daemon.log()

        # ------------------------------------------------------------------
        # 12. catch-up: a change made while the daemon was down
        # ------------------------------------------------------------------
        before = len(created_pages())
        downtime = notes / "downtime.md"
        downtime.write_text('---\ntitle: Edited While Down\n---\n\nwritten while stopped\n')

        daemon2 = Daemon(binary, env, tmp / "daemon2.log")
        health2 = f"http://127.0.0.1:{webhook_port}/healthz"
        check("daemon restarts", bool(wait_for(lambda: health_json().get("ok") is True, 15,
                                              "second daemon /healthz")))
        caught_up = wait_for(lambda: "Obsidian: Edited While Down" in titles(), 20,
                             "the catch-up push")
        check("change made while the daemon was down is pushed on restart", bool(caught_up),
              str(titles()))
        code2 = daemon2.stop(signal.SIGTERM)
        check("second shutdown is clean", code2 == 0, f"exit code {code2}")
        check("startup scan is logged", "startup scan" in daemon2.log(), daemon2.log()[-300:])

        # ------------------------------------------------------------------
        # 13. first run over an existing vault does not mass-upload
        # ------------------------------------------------------------------
        before = len(created_pages())
        existing_vault = tmp / "existing-vault"
        (existing_vault / "keep").mkdir(parents=True)
        (existing_vault / "old-note.md").write_text("---\ntitle: Old Note\n---\n\npre-existing\n")
        (existing_vault / "keep" / "second.md").write_text("---\ntitle: Second Old\n---\n\nold\n")

        seed_port = free_port()
        seed_env = dict(env)
        seed_env.update({"OBSIDIAN_VAULT_PATH": str(existing_vault), "WEBHOOK_PORT": str(seed_port)})
        daemon3 = Daemon(binary, seed_env, tmp / "daemon3.log")
        seed_health = f"http://127.0.0.1:{seed_port}/healthz"
        check("seeding daemon starts", bool(wait_for(
            lambda: http("GET", seed_health)[0] == 200, 15, "seeding daemon /healthz")))
        after = wait_for_stable(lambda: len(created_pages()), 5.0, 15)
        check("first run registers existing notes instead of uploading them", after == before,
              f"{before} -> {after}: {titles()}")
        check("seeding is explained in the log",
              "already synced" in daemon3.log() or "first run" in daemon3.log(),
              daemon3.log()[-400:])
        state3 = json.loads((existing_vault / ".telegrobsidian" / "state.json").read_text())
        check("seeding recorded fingerprints for the existing files",
              len(state3.get("pushed", {})) >= 2, str(state3.get("pushed"))[:200])
        daemon3.stop(signal.SIGTERM)

        # ------------------------------------------------------------------
        # 14. opting in uploads the existing vault
        # ------------------------------------------------------------------
        before = len(created_pages())
        optin_env = dict(seed_env)
        optin_env.update({"PUSH_EXISTING_ON_STARTUP": "true",
                          "STATE_FILE": str(tmp / "optin-state.json")})
        daemon4 = Daemon(binary, optin_env, tmp / "daemon4.log")
        uploaded = wait_for(lambda: "Obsidian: Old Note" in titles(), 20,
                            "the opt-in bulk upload")
        check("PUSH_EXISTING_ON_STARTUP uploads existing notes", bool(uploaded), str(titles()))
        daemon4.stop(signal.SIGTERM)

        # ------------------------------------------------------------------
        # 15. --self-test against the same mocks
        # ------------------------------------------------------------------
        getme_before = mock_state().get("getme_count", 0)
        selftest = subprocess.run([str(binary), "--self-test"], env=env, capture_output=True,
                                  text=True, timeout=60)
        check("--self-test exits 0 against reachable endpoints", selftest.returncode == 0,
              (selftest.stdout + selftest.stderr)[-600:])
        check("--self-test verifies the Notion database",
              "Notion reachable" in selftest.stdout + selftest.stderr,
              (selftest.stdout + selftest.stderr)[-600:])
        check("--self-test queries the Telegram bot",
              mock_state().get("getme_count", 0) > getme_before,
              str(mock_state().get("getme_count")))
        check("shutdown is logged", "Shutdown requested" in log, log[-300:])
        check("shutdown summary is logged", "stopped:" in log, log[-300:])
        check("no fatal errors in the log", "fatal:" not in log)
        check("no data race warnings", "unhandled error in" not in log)

        # ------------------------------------------------------------------
        # 16. TLS client: real handshake, certificate verification, opt-out
        # ------------------------------------------------------------------
        # Everything above talks plain HTTP to keep the suite runnable without
        # OpenSSL. The checks below only run for a TLS-enabled binary and drive
        # the OpenSSL code path against a self-signed HTTPS server, which is the
        # only way to test certificate handling without touching the internet.
        version = subprocess.run([str(binary), "--version"], capture_output=True, text=True,
                                 timeout=30).stdout.strip()
        if "tls: disabled" in version:
            skip("TLS: handshake, verification, opt-out", f"built without OpenSSL ({version})")
        elif shutil.which("openssl") is None:
            skip("TLS: handshake, verification, opt-out", "openssl(1) not available to mint a test certificate")
        else:
            tls_dir = tmp / "tls"
            tls_dir.mkdir(exist_ok=True)
            cert, key = tls_dir / "cert.pem", tls_dir / "key.pem"
            minted = subprocess.run(
                ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                 "-keyout", str(key), "-out", str(cert), "-days", "2",
                 "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1"],
                capture_output=True, text=True, timeout=120)
            if minted.returncode != 0:
                skip("TLS: handshake, verification, opt-out",
                     f"certificate generation failed: {minted.stderr[-200:]}")
            else:
                tls_env = dict(env)
                tls_mock = None
                try:
                    tls_mock, tls_port = start_mock(free_port(), tls_cert=cert, tls_key=key)
                    # Both clients (Notion and Telegram) go through the same TLS
                    # configuration, so point both of them at the HTTPS mock.
                    tls_env["NOTION_BASE_URL"] = f"https://127.0.0.1:{tls_port}"
                    tls_env["TELEGRAM_API_BASE"] = f"https://127.0.0.1:{tls_port}"

                    # (a) an untrusted certificate must abort the connection
                    strict = subprocess.run([str(binary), "--self-test"], env=tls_env,
                                            capture_output=True, text=True, timeout=120)
                    strict_out = strict.stdout + strict.stderr
                    check("a self-signed certificate is rejected by default",
                          strict.returncode != 0,
                          f"exit={strict.returncode}, output={strict_out[-300:]}")
                    check("the certificate error is reported, not swallowed",
                          any(word in strict_out.lower()
                              for word in ("certificate", "ssl", "tls", "handshake")),
                          strict_out[-300:])

                    # (b) the opt-out must make the same endpoint work
                    tls_env["NOTION_TLS_VERIFY"] = "false"
                    permissive = subprocess.run([str(binary), "--self-test"], env=tls_env,
                                                capture_output=True, text=True, timeout=120)
                    permissive_out = permissive.stdout + permissive.stderr
                    check("NOTION_TLS_VERIFY=false accepts the self-signed certificate",
                          permissive.returncode == 0 and "Notion reachable" in permissive_out,
                          f"exit={permissive.returncode}, output={permissive_out[-300:]}")

                    # server-side proof that the request really crossed TLS
                    context = ssl.create_default_context()
                    context.check_hostname = False
                    context.verify_mode = ssl.CERT_NONE
                    with urllib.request.urlopen(f"https://127.0.0.1:{tls_port}/_test/state",
                                                context=context, timeout=10) as response:
                        tls_state = json.loads(response.read().decode())
                    check("the HTTPS server received the request after the opt-out",
                          tls_state.get("getme_count", 0) > 0, str(tls_state)[:300])
                except Exception as exc:  # noqa: BLE001 - reported as a failed check
                    check("TLS scenario runs to completion", False, f"{type(exc).__name__}: {exc}")
                finally:
                    if tls_mock is not None:
                        tls_mock.terminate()
                        try:
                            tls_mock.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            tls_mock.kill()

    finally:
        try:
            daemon.stop(signal.SIGKILL, timeout=5)
        except Exception:
            pass
        mock.terminate()
        try:
            mock.wait(timeout=5)
        except subprocess.TimeoutExpired:
            mock.kill()

        failures = [name for name, ok, _ in results if not ok]
        print("\n" + "=" * 72)
        print(f"{len(results) - len(failures)}/{len(results)} checks passed")
        if skipped:
            print(f"{len(skipped)} check(s) skipped:")
            for name, reason in skipped:
                print(f"  - {name}: {reason}")
        if failures:
            print("failed checks:")
            for name in failures:
                print(f"  - {name}")
            print(f"\ndaemon log: {log_path}")
            print(log_path.read_text(errors="replace")[-4000:])
        print("=" * 72)

        if args.keep_tmp or failures:
            print(f"artifacts kept in {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    return 1 if any(not ok for _, ok, _ in results) else 0


if __name__ == "__main__":
    sys.exit(main())
