#!/usr/bin/env python3
# This program source code file is part of Cably Desktop, based on KiCad,
# a free EDA CAD application.
#
# Copyright (C) 2026 Cably
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

"""
Mock of the two cloud services the F4 bridge talks to, for cably/tests/bridge.sh.
One process plays BOTH Supabase (auth + PostgREST) and the Cably engine, on one
127.0.0.1 port, so the CLI is pointed at http://127.0.0.1:<port> for both.

    mock_cloud.py --apikey <expected publishable key> --log <requests.jsonl> [--port-file <file>]

Every request is appended to the log as one JSON line {method, path, headers:[names...],
authorization, apikey, body} so the shell test can assert the exact calls.

Tokens:  "good-token"     valid bearer
         "expired-token"  bearer -> 401 (the bridge must refresh)
         "refresh-1"      refresh token -> new session with "good-token"
"""
import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import TCPServer
from urllib.parse import urlparse, parse_qs

FIXTURE_PROJECT = {
    "project_name": "Blinking LED",
    "schematic": {"nodes": [{"id": "led1", "type": "led"}], "edges": []},
    "pcb": {"tracks": [], "vias": []},
}
FIXTURE_ROW = {
    "data": {"schema": "cably-project-session-v2", "project": FIXTURE_PROJECT, "chat": []}
}
PROJECT_LIST = [
    {"id": "p1", "name": "Blinking LED", "updated_at": "2026-09-03T10:00:00.000Z"},
    {"id": "p0", "name": "Older Amp", "updated_at": "2026-09-01T10:00:00.000Z"},
]
KICAD_PCB = "(kicad_pcb (version 20240108) (generator \"mock\")\n  (general (thickness 1.6))\n)\n"
KICAD_SCH = "(kicad_sch (version 20231120) (generator \"mock\")\n  (uuid \"00000000-0000-0000-0000-000000000001\")\n)\n"

ARGS = None


class LoopbackServer(HTTPServer):
    def server_bind(self):
        # HTTPServer.server_bind() calls socket.getfqdn(), a reverse-DNS lookup that hangs
        # for tens of seconds when DNS is slow/offline (measured 2026-09-03). We never
        # need the name, so bind like a plain TCPServer.
        TCPServer.server_bind(self)
        self.server_name = "127.0.0.1"
        self.server_port = self.server_address[1]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quiet
        pass

    # -- helpers ---------------------------------------------------------------
    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n).decode("utf-8") if n else ""

    def _record(self, body):
        with open(ARGS.log, "a", encoding="utf-8") as f:
            f.write(json.dumps({
                "method": self.command,
                "path": self.path,
                "headers": sorted({k.lower() for k in self.headers.keys()}),
                "authorization": self.headers.get("Authorization", ""),
                "apikey": self.headers.get("apikey", ""),
                "content_type": self.headers.get("Content-Type", ""),
                "body": body,
            }) + "\n")

    def _send(self, status, obj):
        data = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _bearer(self):
        a = self.headers.get("Authorization", "")
        return a[7:] if a.startswith("Bearer ") else ""

    def _supabase_guard(self):
        if self.headers.get("apikey", "") != ARGS.apikey:
            self._send(401, {"message": "No API key found in request"})
            return False
        return True

    # -- routes ----------------------------------------------------------------
    def do_GET(self):
        self._record("")
        u = urlparse(self.path)
        if u.path == "/auth/v1/user":
            if not self._supabase_guard():
                return
            if self._bearer() == "good-token":
                return self._send(200, {"id": "u1", "email": "dev@example.com"})
            return self._send(401, {"message": "invalid JWT"})
        if u.path == "/rest/v1/projects":
            if not self._supabase_guard():
                return
            if self._bearer() != "good-token":
                return self._send(401, {"message": "JWT expired"})
            q = parse_qs(u.query)
            if "id" in q:
                if q["id"][0] == "eq.p1" and q.get("select") == ["data"]:
                    return self._send(200, [FIXTURE_ROW])
                return self._send(200, [])
            if q.get("select") == ["id,name,updated_at"] and q.get("order") == ["updated_at.desc"]:
                return self._send(200, PROJECT_LIST)
            return self._send(400, {"message": "unexpected query: " + u.query})
        self._send(404, {"message": "not found"})

    def do_POST(self):
        body = self._body()
        self._record(body)
        u = urlparse(self.path)
        if u.path == "/auth/v1/token":
            if not self._supabase_guard():
                return
            if parse_qs(u.query).get("grant_type") != ["refresh_token"]:
                return self._send(400, {"error": "unsupported_grant_type"})
            try:
                j = json.loads(body)
            except ValueError:
                return self._send(400, {"error": "bad json"})
            if j.get("refresh_token") != "refresh-1":
                return self._send(400, {"error": "invalid_grant"})
            return self._send(200, {
                "access_token": "good-token", "refresh_token": "refresh-2",
                "token_type": "bearer", "expires_in": 3600, "expires_at": 4102444800,
                "user": {"id": "u1", "email": "dev@example.com"},
            })
        if u.path == "/v1/export":
            if self._bearer() != "good-token":
                return self._send(401, {"error": {"code": "unauthorized", "message": "Sign in"}})
            try:
                j = json.loads(body)
            except ValueError:
                return self._send(400, {"error": {"code": "bad_request", "message": "bad json"}})
            if j != {"apiVersion": 1, "project": FIXTURE_PROJECT}:
                return self._send(400, {"error": {"code": "bad_request", "message": "body is not the contract"}})
            return self._send(200, {
                "kicadPcb": KICAD_PCB, "kicadSch": KICAD_SCH,
                "schematic": {"source": "generated", "unmapped": [], "unresolvedPins": 0},
                "engineVersion": "mock-1", "timings": {"wallMs": 3},
            })
        self._send(404, {"message": "not found"})


def main():
    global ARGS
    p = argparse.ArgumentParser()
    p.add_argument("--apikey", required=True)
    p.add_argument("--log", required=True)
    p.add_argument("--port-file", default=None)
    ARGS = p.parse_args()
    srv = LoopbackServer(("127.0.0.1", 0), Handler)
    port = srv.server_address[1]
    if ARGS.port_file:
        with open(ARGS.port_file, "w") as f:
            f.write(str(port))
    print(port, flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
