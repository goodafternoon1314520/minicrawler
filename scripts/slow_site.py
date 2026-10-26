#!/usr/bin/env python3
"""本地慢速站点: / 链接到 /p/1../p/N，每个响应延迟 DELAY 秒。
用法: slow_site.py [N=100] [DELAY=0.2] [PORT=8000]"""
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

N = int(sys.argv[1]) if len(sys.argv) > 1 else 100
DELAY = float(sys.argv[2]) if len(sys.argv) > 2 else 0.2
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8000


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        time.sleep(DELAY)
        status = 200
        if self.path == "/":
            links = "".join(f'<a href="/p/{i}">p{i}</a>\n' for i in range(1, N + 1))
            body = f"<html><body>{links}</body></html>"
        elif self.path.startswith("/p/"):
            body = f"<html><body>page {self.path}</body></html>"
        else:
            status, body = 404, "not found"
        data = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()