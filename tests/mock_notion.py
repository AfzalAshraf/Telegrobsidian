#!/usr/bin/env python3
"""
Mock Notion + Telegram API used by the end-to-end smoke test.

It implements just enough of the Notion API for the daemon to run a complete
sync cycle against a local HTTP endpoint:

    GET  /v1/databases/<db_id>              database schema (title property)
    POST /v1/databases/<db_id>/query        filtered + paged database query
    GET  /v1/blocks/<block_id>/children     page/block children
    POST /v1/pages                          page creation
    GET  /bot<token>/getMe                  Telegram bot check
    POST /bot<token>/setWebhook             Telegram webhook registration

Test-only control endpoints (never part of the real APIs):

    GET  /_test/state                       counters + created pages + queries
    POST /_test/notion_page                 create/update a Notion page
    POST /_test/fail_next                   {"count": 1, "status": 429}
    POST /_test/reset

Usage: mock_notion.py [--port 0] [--state-out <file>]
It prints "LISTENING <port>" on stdout once the socket is bound.
"""

from __future__ import annotations

import argparse
import json
import re
import ssl
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

ISO_FMT = "%Y-%m-%dT%H:%M:%S"


def iso_now(offset_seconds: float = 0.0) -> str:
    t = time.time() + offset_seconds
    ms = int((t - int(t)) * 1000)
    return time.strftime(ISO_FMT, time.gmtime(t)) + f".{ms:03d}Z"


def rich_text(text: str, href: str | None = None) -> dict:
    part = {"type": "text", "plain_text": text, "text": {"content": text}}
    if href:
        part["text"]["link"] = {"url": href}
        part["href"] = href
    return part


def paragraph_block(text: str) -> dict:
    return {
        "object": "block",
        "id": f"block-{abs(hash(text)) % 10**8}",
        "type": "paragraph",
        "has_children": False,
        "paragraph": {"rich_text": [rich_text(text)]},
    }


class MockState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.pages: dict[str, dict] = {}
        self.blocks: dict[str, list[dict]] = {}
        self.created: list[dict] = []
        self.queries: list[dict] = []
        self.block_requests: list[str] = []
        self.pages_created_requests: list[dict] = []
        self.getme_count = 0
        self.auth_failures = 0
        self.webhook_registrations: list[dict] = []
        self.fail_next: list[int] = []
        self.clock = 0.0

    def tick(self) -> str:
        """Strictly increasing Notion-style timestamp."""
        self.clock = max(self.clock + 0.002, time.time())
        ms = int((self.clock - int(self.clock)) * 1000)
        return time.strftime(ISO_FMT, time.gmtime(self.clock)) + f".{ms:03d}Z"


STATE = MockState()

DB_ID = "12345678123412341234123456789012"
TITLE_PROPERTY = "Name"


def page_object(page_id: str, title: str, created: str, edited: str, archived: bool = False) -> dict:
    return {
        "object": "page",
        "id": page_id,
        "created_time": created,
        "last_edited_time": edited,
        "archived": archived,
        "in_trash": archived,
        "url": f"https://www.notion.so/{page_id.replace('-', '')}",
        "parent": {"type": "database_id", "database_id": DB_ID},
        "properties": {
            TITLE_PROPERTY: {
                "id": "title",
                "type": "title",
                "name": TITLE_PROPERTY,
                "title": [rich_text(title)],
            }
        },
    }


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "mock-notion/1.0"

    # ---------------------------------------------------------------- helpers
    def log_message(self, fmt: str, *args) -> None:  # keep the test output clean
        if self.server.verbose:  # type: ignore[attr-defined]
            sys.stderr.write("[mock] " + fmt % args + "\n")

    def _send(self, status: int, payload: dict | str) -> None:
        body = payload if isinstance(payload, str) else json.dumps(payload)
        raw = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def _body(self) -> dict:
        length = int(self.headers.get("Content-Length", 0))
        self.raw_body = b""
        if length <= 0:
            return {}
        self.raw_body = self.rfile.read(length)
        try:
            return json.loads(self.raw_body)
        except json.JSONDecodeError:
            return {}

    def _authorized(self) -> bool:
        with STATE.lock:
            for index, failure in enumerate(STATE.fail_next):
                if failure.get("path_contains") and failure["path_contains"] not in self.path:
                    continue
                STATE.fail_next.pop(index)
                self._send(failure["status"], {"object": "error", "status": failure["status"],
                                               "code": "injected_failure",
                                               "message": "injected failure for testing"})
                return False
        auth = self.headers.get("Authorization", "")
        version = self.headers.get("Notion-Version", "")
        if not auth.startswith("Bearer ") or not auth[len("Bearer "):]:
            with STATE.lock:
                STATE.auth_failures += 1
            self._send(401, {"object": "error", "status": 401, "code": "unauthorized",
                             "message": "missing bearer token"})
            return False
        if not version:
            with STATE.lock:
                STATE.auth_failures += 1
            self._send(400, {"object": "error", "status": 400, "code": "invalid_request",
                             "message": "missing Notion-Version header"})
            return False
        return True

    # -------------------------------------------------------------------- GET
    def do_GET(self) -> None:  # noqa: N802
        path = urlparse(self.path).path

        if path == "/_test/state":
            self._send(200, self._state())
            return

        m = re.fullmatch(r"/bot([^/]+)/getMe", path)
        if m:
            with STATE.lock:
                STATE.getme_count += 1
            if m.group(1) != "TESTTOKEN":
                self._send(401, {"ok": False, "description": "Unauthorized"})
                return
            self._send(200, {"ok": True, "result": {"id": 42, "username": "telegrobsidian_test_bot"}})
            return

        if not self._authorized():
            return

        m = re.fullmatch(r"/v1/databases/([0-9a-fA-F-]+)", path)
        if m:
            self._send(200, {
                "object": "database",
                "id": m.group(1),
                "title": [rich_text("Mock Database")],
                "properties": {
                    TITLE_PROPERTY: {"id": "title", "name": TITLE_PROPERTY, "type": "title",
                                     "title": {}},
                },
            })
            return

        m = re.fullmatch(r"/v1/blocks/([^/]+)/children", path)
        if m:
            block_id = m.group(1)
            with STATE.lock:
                STATE.block_requests.append(block_id)
                blocks = STATE.blocks.get(block_id, [])
                if block_id not in STATE.blocks and block_id in STATE.pages:
                    blocks = [paragraph_block("body of " + block_id)]
            self._send(200, {"object": "list", "results": blocks, "has_more": False,
                             "next_cursor": None})
            return

        self._send(404, {"object": "error", "code": "not_found", "message": f"no route for {path}"})

    # ------------------------------------------------------------------- POST
    def do_POST(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        body = self._body()

        if path == "/_test/notion_page":
            with STATE.lock:
                page_id = body.get("id") or f"page-{len(STATE.pages) + 1:04d}"
                now = STATE.tick()
                created = STATE.pages.get(page_id, {}).get("created_time", now)
                page = page_object(page_id, body.get("title", "Untitled"), created, now,
                                   bool(body.get("archived", False)))
                STATE.pages[page_id] = page
                blocks = body.get("blocks")
                if blocks is None:
                    blocks = [paragraph_block(body.get("text", ""))]
                STATE.blocks[page_id] = blocks
            self._send(200, {"ok": True, "id": page_id, "last_edited_time": page["last_edited_time"]})
            return

        if path == "/_test/fail_next":
            with STATE.lock:
                for _ in range(int(body.get("count", 1))):
                    STATE.fail_next.append({"status": int(body.get("status", 429)),
                                            "path_contains": body.get("path_contains", "")})
            self._send(200, {"ok": True})
            return

        if path == "/_test/reset":
            with STATE.lock:
                STATE.pages.clear()
                STATE.blocks.clear()
                STATE.created.clear()
                STATE.queries.clear()
                STATE.block_requests.clear()
                STATE.pages_created_requests.clear()
                STATE.auth_failures = 0
            self._send(200, {"ok": True})
            return

        m = re.fullmatch(r"/bot([^/]+)/setWebhook", path)
        if m:
            from urllib.parse import parse_qs
            form = parse_qs(self.raw_body.decode()) if self.raw_body else {}
            with STATE.lock:
                STATE.webhook_registrations.append({
                    "token": m.group(1),
                    "url": (form.get("url") or [""])[0],
                    "secret_token": (form.get("secret_token") or [""])[0],
                    "notion_version": self.headers.get("Notion-Version", ""),
                })
            self._send(200, {"ok": True, "result": True, "description": "Webhook was set"})
            return

        if not self._authorized():
            return

        m = re.fullmatch(r"/v1/databases/([0-9a-fA-F-]+)/query", path)
        if m:
            self._query(body)
            return

        if path == "/v1/pages":
            self._create_page(body)
            return

        self._send(404, {"object": "error", "code": "not_found", "message": f"no route for {path}"})

    # ---------------------------------------------------------------- handlers
    def _query(self, body: dict) -> None:
        after = (((body.get("filter") or {}).get("last_edited_time")) or {}).get("after")
        direction = "descending"
        for sort in body.get("sorts") or []:
            if sort.get("timestamp") == "last_edited_time":
                direction = sort.get("direction", "descending")

        with STATE.lock:
            STATE.queries.append({"after": after, "sorts": body.get("sorts"),
                                  "start_cursor": body.get("start_cursor")})
            pages = [p for p in STATE.pages.values() if not after or p["last_edited_time"] > after]
            pages.sort(key=lambda p: p["last_edited_time"], reverse=(direction == "descending"))
            page_size = int(body.get("page_size", 100))
            start = int(body.get("start_cursor") or 0)
            window = pages[start:start + page_size]
            next_cursor = str(start + page_size) if start + page_size < len(pages) else None

        self._send(200, {
            "object": "list",
            "results": window,
            "has_more": next_cursor is not None,
            "next_cursor": next_cursor,
            "type": "page",
        })

    def _create_page(self, body: dict) -> None:
        properties = body.get("properties") or {}
        title = ""
        for value in properties.values():
            for part in (value or {}).get("title") or []:
                title += (part.get("text") or {}).get("content", "") or part.get("plain_text", "")
        chunks = []
        for child in body.get("children") or []:
            rich = ((child.get("paragraph") or {}).get("rich_text")) or []
            chunks.extend((part.get("text") or {}).get("content", "") for part in rich)
        text = "\n\n".join(chunks)

        with STATE.lock:
            STATE.pages_created_requests.append({"title": title, "text": text,
                                                 "parent": body.get("parent")})
            page_id = f"made-{len(STATE.created) + 1:04d}"
            created = STATE.tick()
            page = page_object(page_id, title, created, created)
            # Realistic behaviour: pages created through the API show up in later
            # queries, which is exactly the situation that can cause sync loops.
            STATE.pages[page_id] = page
            STATE.blocks[page_id] = [paragraph_block(text)] if text else []
            STATE.created.append({"id": page_id, "title": title, "text": text,
                                  "parent": body.get("parent"),
                                  "properties": properties})

        self._send(200, page)

    def _state(self) -> dict:
        with STATE.lock:
            return {
                "pages_total": len(STATE.pages),
                "pages_created": list(STATE.created),
                "pages_created_count": len(STATE.created),
                "create_requests": list(STATE.pages_created_requests),
                "queries": list(STATE.queries),
                "query_count": len(STATE.queries),
                "block_requests": list(STATE.block_requests),
                "block_request_count": len(STATE.block_requests),
                "webhook_registrations": list(STATE.webhook_registrations),
                "getme_count": STATE.getme_count,
                "setwebhook_count": len(STATE.webhook_registrations),
                "auth_failures": STATE.auth_failures,
                "page_ids": sorted(STATE.pages),
            }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--tls-cert", help="serve HTTPS with this certificate (PEM)")
    parser.add_argument("--tls-key", help="private key belonging to --tls-cert")
    args = parser.parse_args()

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.daemon_threads = True
    server.verbose = args.verbose  # type: ignore[attr-defined]
    if args.tls_cert:
        # Used by the smoke test to prove that the daemon's TLS client code
        # performs a real handshake and rejects untrusted certificates.
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(args.tls_cert, args.tls_key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
    print(f"LISTENING {server.server_address[1]}", flush=True)
    try:
        server.serve_forever(poll_interval=0.1)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
