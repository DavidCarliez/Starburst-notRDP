#!/usr/bin/env python3
"""notRDP live viewer server (operator side).

One listening port serves the agent's reverse connection; a second serves
the browser viewer:

  agent connects to :8124 and sends  magic "NRDP" ver(1) w(4) h(4) scale(1)
  then frames:  msg(1)=0x01 len(4) jpeg  (all big-endian)
  it receives input messages: msg(1)=0x02 x(2) y(2) action(2) key(2)

Browser API on :8123:
  GET  /        viewer page
  GET  /dims    {"w":..,"h":..,"connected":..}
  GET  /stream  multipart/x-mixed-replace MJPEG stream
  POST /input   body {"x":..,"y":..,"action":..,"key":..}

stdlib only.  usage: python3 viewer_server.py [agent_port] [--http N]
"""

import argparse
import json
import socket
import struct
import threading
import time

MAGIC = b"NRDP"

_lock = threading.Lock()
_state = {
    "jpeg": None,
    "seq": 0,
    "w": 0,
    "h": 0,
    "scale": 1,
    "agent": None,
    "connected": False,
}
_new_frame = threading.Event()


def agent_thread(port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(4)
    print(f"[*] agent listener on 0.0.0.0:{port}")
    while True:
        conn, addr = srv.accept()
        print(f"[+] agent connected from {addr}")
        try:
            conn.settimeout(30)
            handle_agent(conn, addr)
        except Exception as e:
            print(f"[-] agent {addr}: {e}")
        finally:
            try:
                conn.close()
            except OSError:
                pass
            with _lock:
                _state["agent"] = None
                _state["connected"] = False
                _state["jpeg"] = None
            _new_frame.set()
            print(f"[-] agent {addr} disconnected")


def handle_agent(conn, addr):
    hdr = b""
    while len(hdr) < 14:
        chunk = conn.recv(14 - len(hdr))
        if not chunk:
            raise ConnectionError("handshake incomplete")
        hdr += chunk
    if hdr[:4] != MAGIC:
        raise ConnectionError("bad magic")
    ver = hdr[4]
    w, h = struct.unpack(">II", hdr[5:13])
    scale = hdr[13]
    if ver != 1:
        raise ConnectionError(f"unsupported protocol version {ver}")
    if not scale:
        raise ConnectionError("invalid zero scale")
    print(f"[+] handshake v{ver} screen {w}x{h} scale={scale}")
    with _lock:
        _state["agent"] = conn
        _state["connected"] = True
        _state["jpeg"] = None
        _state["w"], _state["h"] = w, h
        _state["scale"] = scale
    buf = b""
    while True:
        chunk = conn.recv(65536)
        if not chunk:
            raise ConnectionError("agent closed")
        buf += chunk
        while True:
            if len(buf) < 5:
                break
            msg_type = buf[0]
            (length,) = struct.unpack(">I", buf[1:5])
            if msg_type == 0x01:  # frame
                if len(buf) < 5 + length:
                    break
                jpeg = buf[5:5 + length]
                buf = buf[5 + length:]
                with _lock:
                    _state["jpeg"] = jpeg
                    _state["seq"] += 1
                _new_frame.set()
            else:
                buf = b""


VIEWER_HTML = """<!doctype html>
<html><head><title>notRDP live</title>
<style>
  html,body{margin:0;background:#111;color:#ddd;font-family:monospace}
  img{display:block;cursor:crosshair;max-width:100vw;max-height:100vh;outline:none}
  #stat{position:fixed;top:0;left:0;background:#000a;padding:2px 6px;z-index:9;pointer-events:none}
</style></head>
<body>
<div id="stat">connecting...</div>
<img id="screen" src="/stream" tabindex="0">
<script>
const img = document.getElementById('screen');
const stat = document.getElementById('stat');
async function updateStatus(){
  const d = await fetch('/dims').then(r=>r.json());
  stat.textContent = d.connected ? `live ${d.w}x${d.h} / ${d.scale}` : 'waiting for agent';
}
updateStatus();
setInterval(updateStatus, 1000);

function toScreen(ev){
  const r = img.getBoundingClientRect();
  const sx = img.naturalWidth / r.width, sy = img.naturalHeight / r.height;
  return {x: Math.round((ev.clientX - r.left) * sx),
          y: Math.round((ev.clientY - r.top) * sy)};
}
let lastPoint = {x: 0, y: 0};
let moveTimer = null;
let pendingMove = null;
function sendInput(x, y, action, key){
  fetch('/input', {method:'POST',
    body: JSON.stringify({x, y, action, key: key||0})});
}
function remember(ev){
  lastPoint = toScreen(ev);
  return lastPoint;
}
function sendMove(point, buttons){
  if(moveTimer){ pendingMove = {point, buttons}; return; }
  sendInput(point.x, point.y, 0, buttons & 3);
  moveTimer = setTimeout(() => {
    moveTimer = null;
    if(pendingMove){
      const next = pendingMove; pendingMove = null;
      sendMove(next.point, next.buttons);
    }
  }, 30);
}
img.addEventListener('mousedown', e => {
  e.preventDefault(); img.focus({preventScroll:true});
  const p = remember(e); sendInput(p.x, p.y, e.button === 2 ? 6 : 4, 0);
});
img.addEventListener('mouseup', e => {
  const p = remember(e); sendInput(p.x, p.y, e.button === 2 ? 7 : 5, 0);
});
img.addEventListener('mousemove', e => sendMove(remember(e), e.buttons));
img.addEventListener('dblclick', e => {
  const p = remember(e); sendInput(p.x, p.y, 3, 0);
});
img.addEventListener('wheel', e => {
  e.preventDefault();
  const p = remember(e);
  sendInput(p.x, p.y, 8, -Math.sign(e.deltaY) * 120);
}, {passive:false});
img.addEventListener('contextmenu', e => e.preventDefault());
window.addEventListener('keydown', e => {
  if(document.activeElement === img){
    e.preventDefault(); sendInput(lastPoint.x, lastPoint.y, 11, e.keyCode);
  }
});
window.addEventListener('keyup', e => {
  if(document.activeElement === img){
    e.preventDefault(); sendInput(lastPoint.x, lastPoint.y, 12, e.keyCode);
  }
});
img.onerror = () => { stat.textContent = 'agent offline'; };
img.onload = () => { stat.textContent = 'live'; };
</script></body></html>
"""


class HttpHandler(threading.Thread):
    """Minimal threaded HTTP server: /, /dims, /stream, /input."""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port

    def run(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("0.0.0.0", self.port))
        srv.listen(16)
        print(f"[*] viewer http on http://0.0.0.0:{self.port}/")
        while True:
            c, _addr = srv.accept()
            threading.Thread(target=self.handle, args=(c, _addr),
                             daemon=True).start()

    def handle(self, c, addr=None):
        try:
            c.settimeout(10)
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = c.recv(4096)
                if not chunk:
                    return
                data += chunk
            head, _, rest = data.partition(b"\r\n\r\n")
            lines = head.decode("latin1").split("\r\n")
            method, path = lines[0].split(" ")[0:2]
            cl = 0
            for line in lines[1:]:
                if line.lower().startswith("content-length:"):
                    cl = int(line.split(":")[1].strip())
            while len(rest) < cl:
                chunk = c.recv(4096)
                if not chunk:
                    break
                rest += chunk

            if path.startswith("/stream"):
                self.serve_stream(c)
            elif path.startswith("/dims"):
                with _lock:
                    body = json.dumps({
                        "w": _state["w"], "h": _state["h"],
                        "scale": _state["scale"],
                        "connected": _state["connected"],
                    })
                self.reply(c, "application/json", body.encode())
            elif method == "POST" and path.startswith("/input"):
                try:
                    j = json.loads(rest.decode("latin1") or "{}")
                    self.forward_input(j)
                    self.reply(c, "application/json", b'{"ok":true}')
                except Exception as e:
                    self.reply(c, "application/json",
                               json.dumps({"ok": False,
                                           "e": str(e)}).encode())
            else:
                self.reply(c, "text/html", VIEWER_HTML.encode())
        except Exception:
            pass
        finally:
            try:
                c.close()
            except OSError:
                pass

    def reply(self, c, ctype, body):
        hdr = (f"HTTP/1.1 200 OK\r\nContent-Type: {ctype}\r\n"
               f"Content-Length: {len(body)}\r\nConnection: close\r\n"
               f"\r\n").encode()
        c.sendall(hdr + body)

    def serve_stream(self, c):
        c.sendall(
            b"HTTP/1.0 200 OK\r\nContent-Type: multipart/x-mixed-replace; "
            b"boundary=frame\r\nCache-Control: no-cache\r\n\r\n")
        last = -1
        boundary = b"--frame\r\nContent-Type: image/jpeg\r\n\r\n"
        try:
            while True:
                with _lock:
                    jpeg = _state["jpeg"]
                    seq = _state["seq"]
                if jpeg is None or seq == last:
                    _new_frame.wait(0.25)
                    _new_frame.clear()
                    continue
                last = seq
                c.sendall(boundary + jpeg + b"\r\n")
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def forward_input(self, j):
        x = int(j.get("x", 0))
        y = int(j.get("y", 0))
        action = int(j.get("action", 0))
        key = int(j.get("key", 0))
        with _lock:
            agent = _state.get("agent")
            if not _state["connected"] or agent is None:
                raise ConnectionError("no agent connected")
            scale = _state["scale"]
            x = max(-32768, min(32767, x * scale))
            y = max(-32768, min(32767, y * scale))
            action = max(-32768, min(32767, action))
            key = max(-32768, min(32767, key))
            agent.sendall(struct.pack(">Bhhhh", 2, x, y, action, key))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", type=int, default=8124,
                    help="agent reverse-connection port (default 8124)")
    ap.add_argument("--http", type=int, default=8123,
                    help="browser viewer port (default 8123)")
    args = ap.parse_args()
    threading.Thread(target=agent_thread, args=(args.port,),
                     daemon=True).start()
    HttpHandler(args.http).start()
    print("[*] open the viewer at http://<this-host>:%d/" % args.http)
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
