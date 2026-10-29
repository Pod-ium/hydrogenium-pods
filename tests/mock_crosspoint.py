#!/usr/bin/env python3
"""Minimal stand-in for CrossPoint's file-transfer server (same routes and
replies as src/network/CrossPointWebServer.cpp). Files land in ROOT."""
import json, os, sys, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = sys.argv[2] if len(sys.argv) > 2 else "/tmp/x3sd"
os.makedirs(ROOT, exist_ok=True)

class H(BaseHTTPRequestHandler):
    def reply(self, code, body, ctype="text/plain"):
        b = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path == "/api/status":
            self.reply(200, json.dumps({"version": "mock", "ip": "127.0.0.1", "device": "X3"}), "application/json")
        else:
            self.reply(404, "Not found")

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        body = self.rfile.read(int(self.headers["Content-Length"]))
        if u.path == "/upload":
            ctype = self.headers["Content-Type"]
            boundary = ctype.split("boundary=")[1].encode()
            parts = body.split(b"--" + boundary)
            assert parts[-1].strip() == b"--", "missing closing boundary"
            part = parts[1]
            hdr, data = part.split(b"\r\n\r\n", 1)
            assert data.endswith(b"\r\n")
            data = data[:-2]
            name = hdr.split(b'filename="')[1].split(b'"')[0].decode()
            if "/" in name or "\\" in name or name in (".", ".."):
                return self.reply(400, "Invalid file name")
            folder = q.get("path", ["/"])[0]
            dest = os.path.join(ROOT, folder.strip("/"), name)
            if os.path.exists(dest):
                return self.reply(400, "File already exists: " + name)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            open(dest, "wb").write(data)
            return self.reply(200, "File uploaded successfully: " + name)
        if u.path == "/delete":
            f = urllib.parse.parse_qs(body.decode())
            p = f.get("path", [""])[0]
            dest = os.path.join(ROOT, p.strip("/"))
            if not os.path.exists(dest):
                return self.reply(400, "Failed to delete some items: " + p + " (not found); ")
            os.remove(dest)
            return self.reply(200, "All items deleted successfully")
        self.reply(404, "Not found")

    def log_message(self, fmt, *a):
        sys.stderr.write("mock: " + fmt % a + "\n")

ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 8765), H).serve_forever()
