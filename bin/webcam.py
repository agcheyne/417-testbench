#! /usr/bin/env python3
#
# Logitech B910 webcam as an MJPEG stream over HTTP. GStreamer reads the camera's own MJPEG
# (no re-encoding); one camera reader is shared by every viewer.
#
#   webcam.py [--port 8080] [--bind 0.0.0.0] [--device /dev/video0] [--size 1280x720] [--fps 15]
#
#   http://HOST:PORT/              page with the live picture
#   http://HOST:PORT/stream.mjpg   the bare stream (VLC, ffplay, <img src=...>)
#   http://HOST:PORT/snapshot.jpg  the latest frame
#
# Ctrl-C stops it. From outside the lab, tunnel it:  ssh -L 8080:localhost:8080 nptest@HOST

import argparse, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PAGE = b"""<!doctype html><html><head><meta charset="utf-8"><title>417 webcam</title>
<style>body{margin:0;background:#111;color:#ccc;font:14px sans-serif;text-align:center}
img{max-width:100vw;max-height:95vh}</style></head>
<body><img src="stream.mjpg"><div><a style="color:#8af" href="snapshot.jpg">snapshot</a></div></body></html>"""


class Camera:
    """Runs gst-launch, splits its stdout into JPEG frames, keeps the latest one."""

    def __init__(self, device, width, height, fps):
        self.cmd = ["gst-launch-1.0", "-q", "v4l2src", f"device={device}", "!",
                    f"image/jpeg,width={width},height={height},framerate={fps}/1", "!", "fdsink", "fd=1"]
        self.frame, self.count = None, 0
        self.cond = threading.Condition()
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while True:
            proc = subprocess.Popen(self.cmd, stdout=subprocess.PIPE)
            buf = b""
            while True:
                chunk = proc.stdout.read1(65536)
                if not chunk:
                    break
                buf += chunk
                while True:   # a frame runs from SOI (FFD8) to EOI (FFD9)
                    a = buf.find(b"\xff\xd8")
                    b = buf.find(b"\xff\xd9", a + 2) if a >= 0 else -1
                    if b < 0:
                        buf = buf[a:] if a >= 0 else b""
                        break
                    with self.cond:
                        self.frame, self.count = buf[a:b + 2], self.count + 1
                        self.cond.notify_all()
                    buf = buf[b + 2:]
            proc.wait()
            print(f"webcam: gst-launch exited ({proc.returncode}), restarting in 2 s", file=sys.stderr)
            time.sleep(2)

    def wait(self, last, timeout=5):
        """Block until a frame newer than `last` arrives -> (frame, count)."""
        with self.cond:
            self.cond.wait_for(lambda: self.count != last, timeout)
            return self.frame, self.count


def handler(cam):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

        def do_GET(self):
            path = self.path.split("?")[0]
            if path in ("/", "/index.html"):
                self.reply(PAGE, "text/html")
            elif path == "/snapshot.jpg":
                frame, _ = cam.wait(0 if cam.frame is None else -1)
                if frame is None:
                    self.send_error(503, "no frame from camera")
                else:
                    self.reply(frame, "image/jpeg")
            elif path == "/stream.mjpg":
                self.stream()
            else:
                self.send_error(404)

        def reply(self, body, ctype):
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(body)

        def stream(self):
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            last = None
            try:
                while True:
                    frame, last = cam.wait(last)
                    if frame is None:
                        continue
                    self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n"
                                     % len(frame) + frame + b"\r\n")
            except (BrokenPipeError, ConnectionResetError):
                pass
    return Handler


def main():
    ap = argparse.ArgumentParser(description="Serve the webcam as MJPEG over HTTP")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--device", default="/dev/video0")
    ap.add_argument("--size", default="1280x720", help="WxH the camera does in MJPEG")
    ap.add_argument("--fps", type=int, default=15)
    a = ap.parse_args()
    w, h = a.size.split("x")
    cam = Camera(a.device, w, h, a.fps)
    srv = ThreadingHTTPServer((a.bind, a.port), handler(cam))
    srv.daemon_threads = True
    print(f"webcam: {a.device} {a.size}@{a.fps} on http://{a.bind}:{a.port}/", file=sys.stderr)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
