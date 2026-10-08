#!/usr/bin/env python3
import os
import sys
import json
import time
import urllib.parse
import urllib.request
import ssl
from http.server import ThreadingHTTPServer, HTTPServer, SimpleHTTPRequestHandler

from connectors.cdplayer import CDPlayerConnector

PORT = 8080
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
STATIC_DIR = os.path.join(BASE_DIR, "static")

# Load configuration
CONFIG_PATH = os.path.join(BASE_DIR, "config.json")
if not os.path.exists(CONFIG_PATH):
    CONFIG_PATH = os.path.join(BASE_DIR, "config.example.json")

config = {}
if os.path.exists(CONFIG_PATH):
    with open(CONFIG_PATH, "r", encoding="utf-8") as f:
        config = json.load(f)

# Initialize Connectors (Only CD Player)
connectors = {
    "cdplayer": CDPlayerConnector(config.get("cdplayer"))
}

# High-Performance In-Memory Response Cache
class ResponseCache:
    def __init__(self):
        self._cache = {}

    def get(self, key):
        entry = self._cache.get(key)
        if entry:
            val, expiry = entry
            if time.time() < expiry:
                return val
        return None

    def set(self, key, val, ttl_seconds):
        self._cache[key] = (val, time.time() + ttl_seconds)

    def invalidate(self, prefix=None):
        if prefix is None:
            self._cache.clear()
        else:
            keys_to_del = [k for k in self._cache.keys() if k.startswith(prefix)]
            for k in keys_to_del:
                del self._cache[k]

cache = ResponseCache()

class PanelRequestHandler(SimpleHTTPRequestHandler):
    """HTTP Request Handler for Touch Panel Backend and Static Web Server."""
    
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=STATIC_DIR, **kwargs)

    def log_message(self, format, *args):
        # Quiet logging to avoid log spam
        pass

    def send_json(self, data, code=200):
        body = json.dumps(data).encode('utf-8')
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        self.end_headers()
        self.wfile.write(body)

    def end_headers(self):
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        # API Endpoints with TTL Caching (High Performance)
        if path == "/api/connectors":
            self.send_json({"connectors": list(connectors.keys())})
            return

        elif path.startswith("/api/cdplayer"):
            ep = query.get("endpoint", ["status"])[0]
            cache_key = f"cdplayer:{ep}"
            cached = cache.get(cache_key)
            if cached is not None:
                self.send_json(cached)
                return
            res = connectors["cdplayer"].fetch_data(ep)
            cache.set(cache_key, res, ttl_seconds=0.25)
            self.send_json(res)
            return

        # Serve static frontend files
        super().do_GET()

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        
        content_len = int(self.headers.get('Content-Length', 0))
        post_body = {}
        if content_len > 0:
            raw_body = self.rfile.read(content_len).decode('utf-8')
            try:
                post_body = json.loads(raw_body)
            except Exception:
                self.send_json({"error": True, "message": "Invalid JSON payload"}, code=400)
                return

        if path == "/api/cdplayer/action":
            action = post_body.get("action", "play")
            payload = post_body.get("payload", {})
            res = connectors["cdplayer"].post_action(action, payload)
            # Invalidate cache so GET immediately reflects new status
            cache.invalidate()
            self.send_json(res)
            return

        self.send_json({"error": True, "message": "Not Found"}, code=404)

        self.send_json({"error": True, "message": "Unknown POST endpoint"}, code=404)

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

def run_server():
    server_address = ('', PORT)
    httpd = ThreadingHTTPServer(server_address, PanelRequestHandler)
    print(f"=== Touch Panel Server running on port {PORT} ===")
    print(f"=== Static root: {STATIC_DIR} ===")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nServer shutting down.")
        httpd.server_close()

if __name__ == '__main__':
    run_server()
