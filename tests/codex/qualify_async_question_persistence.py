#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
"""Exercise the installed app-server with disposable history and a local provider."""

import argparse
import datetime
import http.server
import itertools
import json
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import time


class Provider(http.server.BaseHTTPRequestHandler):
    identifiers = itertools.count(1)

    def log_message(self, *_args):
        return

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        if b"Wait for steering fixture" in body:
            self.server.steering_started.set()
            assert self.server.steering_release.wait(10)
        identifier = str(next(self.identifiers))
        events = [
            {"type": "response.created", "response": {"id": "fixture-response-" + identifier}},
            {"type": "response.output_item.done", "item": {
                "type": "message", "role": "assistant", "id": "fixture-final-" + identifier,
                "content": [{"type": "output_text", "text": "Fixture completed."}],
            }},
            {"type": "response.completed", "response": {"id": "fixture-response-" + identifier, "usage": {
                "input_tokens": 0, "output_tokens": 0, "total_tokens": 0,
            }}},
        ]
        data = "".join("event: " + event["type"] + "\ndata: " + json.dumps(event) + "\n\n" for event in events).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        return


class Client:
    def __init__(self, executable, root, stderr):
        env = {key: value for key, value in os.environ.items() if key not in {"OPENAI_API_KEY", "CODEX_API_KEY"}}
        env["CODEX_HOME"] = str(root / "home")
        self.process = subprocess.Popen(
            [executable, "app-server", "--listen", "stdio://"], cwd=root, env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr, text=True,
        )
        self.messages = queue.Queue()
        self.identifier = 0
        self.pending = []
        threading.Thread(target=self.read, daemon=True).start()
        try:
            self.call("initialize", {"clientInfo": {"name": "async-question-fixture", "version": "1"},
                                     "capabilities": {"experimentalApi": True}})
            self.send({"method": "initialized", "params": {}})
        except Exception:
            self.stop()
            raise
        return

    def read(self):
        for line in self.process.stdout:
            try:
                self.messages.put(json.loads(line))
            except ValueError:
                pass
        return

    def send(self, message):
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()
        return

    def wait(self, predicate):
        result = next((message for message in self.pending if predicate(message)), None)
        if result is not None:
            self.pending.remove(result)
        deadline = time.monotonic() + 30
        while result is None and time.monotonic() < deadline:
            try:
                message = self.messages.get(timeout=max(0.01, deadline - time.monotonic()))
                if predicate(message):
                    result = message
                else:
                    self.pending.append(message)
            except queue.Empty:
                pass
        if result is None:
            raise TimeoutError("The disposable app-server did not produce the expected event")
        return result

    def call(self, method, params):
        self.identifier += 1
        identifier = self.identifier
        self.send({"id": identifier, "method": method, "params": params})
        message = self.wait(lambda value: value.get("id") == identifier)
        if "error" in message:
            raise RuntimeError(message["error"])
        return message["result"]

    def stop(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        return


def qualify(executable, root, stderr):
    fixture = json.loads((Path(__file__).parents[1] / "fixtures/async-questions.json").read_text())
    entry = next(case for case in fixture["cases"] if case["id"] == "two-independent-choices")
    home = root / "home"
    home.mkdir()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    server.steering_started = threading.Event()
    server.steering_release = threading.Event()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    (home / "config.toml").write_text(
        'model = "mock-model"\nmodel_provider = "fixture"\napproval_policy = "never"\n'
        '[model_providers.fixture]\nname = "Offline fixture"\n'
        f'base_url = "http://127.0.0.1:{server.server_port}/v1"\nwire_api = "responses"\n'
        'request_max_retries = 0\nstream_max_retries = 0\n'
    )
    client = None
    try:
        client = Client(executable, root, stderr)
        thread = client.call("thread/start", {"cwd": str(root), "model": "mock-model", "modelProvider": "fixture",
                                             "approvalPolicy": "never", "baseInstructions": "Offline persistence fixture."})["thread"]["id"]
        turn = client.call("turn/start", {"threadId": thread, "input": [{"type": "text", "text": "Persistence fixture"}]})["turn"]["id"]
        client.wait(lambda message: message.get("method") == "turn/completed")
        client.stop()
        paths = list((home / "sessions").rglob("*" + thread + "*.jsonl"))
        assert len(paths) == 1
        path = paths[0]
        events = [json.loads(line) for line in path.read_text().splitlines()]
        # Seed only disposable history; runtime question-tool availability is unrelated to persistence.
        now = datetime.datetime.now(datetime.timezone.utc)
        item = {"type": "AgentMessage", "id": "question", "phase": "final_answer", "delivery": "async",
                "questions": entry["questions"], "content": [{"type": "Text", "text": entry["text"]}]}
        event = {"timestamp": now.isoformat(), "ordinal": max(value.get("ordinal", 0) for value in events) + 1,
                 "type": "event_msg", "payload": {"type": "item_completed", "thread_id": thread, "turn_id": turn,
                 "item": item, "started_at_ms": int(now.timestamp() * 1000), "completed_at_ms": int(now.timestamp() * 1000)}}
        with path.open("a") as output:
            output.write(json.dumps(event) + "\n")
        client = Client(executable, root, stderr)
        client.call("thread/resume", {"threadId": thread, "path": str(path)})
        restored = client.call("thread/turns/list", {"threadId": thread, "itemsView": "full", "sortDirection": "asc", "limit": 80})
        assert any(item.get("questions") == entry["questions"] for turn in restored["data"] for item in turn.get("items", []))
        client.call("turn/start", {"threadId": thread, "clientUserMessageId": "codexui-persistence-fixture",
                                  "input": [{"type": "text", "text": entry["wire"]}]})
        client.wait(lambda message: message.get("method") == "turn/completed")
        client.stop()
        client = Client(executable, root, stderr)
        restored = client.call("thread/turns/list", {"threadId": thread, "itemsView": "full", "sortDirection": "asc", "limit": 80})
        items = [item for turn in restored["data"] for item in turn.get("items", [])]
        assert any(item.get("questions") == entry["questions"] for item in items)
        assert any(item.get("type") == "userMessage" and item.get("clientId") == "codexui-persistence-fixture"
                   and item["content"][0]["text"] == entry["wire"] for item in items)
        client.call("thread/resume", {"threadId": thread})
        active = client.call("turn/start", {"threadId": thread, "input": [{"type": "text", "text": "Wait for steering fixture"}]})["turn"]["id"]
        assert server.steering_started.wait(10)
        client.call("turn/steer", {"threadId": thread, "expectedTurnId": active, "clientUserMessageId": "codexui-steering-fixture",
                                  "input": [{"type": "text", "text": entry["wire"]}]})
        server.steering_release.set()
        client.wait(lambda message: message.get("method") == "item/completed"
                    and message.get("params", {}).get("item", {}).get("clientId") == "codexui-steering-fixture")
        client.wait(lambda message: message.get("method") == "turn/completed" and message["params"]["turn"]["id"] == active)
        client.stop()
        client = Client(executable, root, stderr)
        restored = client.call("thread/turns/list", {"threadId": thread, "itemsView": "full", "sortDirection": "asc", "limit": 80})
        assert any(item.get("clientId") == "codexui-steering-fixture" and item["content"][0]["text"] == entry["wire"]
                   for turn in restored["data"] for item in turn.get("items", []))
    finally:
        server.steering_release.set()
        if client is not None:
            client.stop()
        server.shutdown()
        server.server_close()
    print("PASS: installed app-server restart preserves seeded questions and both turn-start and steering replies")
    return


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codex", required=True)
    parser.add_argument("--build-dir", required=True, type=Path)
    args = parser.parse_args()
    args.build_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="app-server-", dir=args.build_dir) as directory:
        with (args.build_dir / "app-server-stderr.log").open("w") as stderr:
            qualify(args.codex, Path(directory), stderr)
