#!/usr/bin/env python3
"""本地慢速站点: / 链接到 /p/1../p/N，每个响应延迟 DELAY 秒。
用法: slow_site.py [N=100] [DELAY=0.2] [PORT=8000]
环境变量:
  ROBOTS_FILE=路径    把这个文件作为 /robots.txt 提供（默认 404）
  ROBOTS_STATUS=500   /robots.txt 一律返回这个状态码（模拟服务器出错）
  LOG=1               每个请求打印一行 "<unix时间> <路径>"，用来统计请求速率"""
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

N = int(sys.argv[1]) if len(sys.argv) > 1 else 100
DELAY = float(sys.argv[2]) if len(sys.argv) > 2 else 0.2
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8000
ROBOTS = open(os.environ["ROBOTS_FILE"], "rb").read() if os.environ.get("ROBOTS_FILE") else None
ROBOTS_STATUS = int(os.environ["ROBOTS_STATUS"]) if os.environ.get("ROBOTS_STATUS") else None
LOG = os.environ.get("LOG") == "1"


class Handler(BaseHTTPRequestHandler):
    def reply(self, status, data, ctype="text/html"):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if LOG:
            print(f"{time.time():.3f} {self.path}", flush=True)
        if self.path == "/robots.txt":
            if ROBOTS_STATUS is not None:
                return self.reply(ROBOTS_STATUS, b"oops", "text/plain")
            if ROBOTS is not None:
                return self.reply(200, ROBOTS, "text/plain")
            return self.reply(404, b"not found")

        time.sleep(DELAY)
        if self.path == "/":
            links = "".join(f'<a href="/p/{i}">p{i}</a>\n' for i in range(1, N + 1))
            self.reply(200, f"<html><body>{links}</body></html>".encode())
        elif self.path.startswith("/p/"):
            self.reply(200, f"<html><body>page {self.path}</body></html>".encode())
        else:
            self.reply(404, b"not found")

    def log_message(self, *args):
        pass


ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()