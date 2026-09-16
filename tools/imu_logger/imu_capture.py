#!/usr/bin/env python3
"""One-action IMU recorder. Python standard library only; outputs CSV + SVG + JSON.

Run: python tools/imu_logger/imu_capture.py --device 192.168.1.50
Plot an existing recording: python .../imu_capture.py --plot path/to/record.csv
"""
import argparse
import csv
import html
import io
import json
import math
import os
from pathlib import Path
import re
import socket
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import HTTPError, URLError
from urllib.request import Request, ProxyHandler, build_opener

MAX_BYTES = 256 * 1024
ID = re.compile(r"[0-9a-f]{8}-[0-9]{6,10}\Z")
LABEL = re.compile(r"[A-Za-z0-9_-]{1,48}\Z")
AXES = ("ax_g", "ay_g", "az_g", "gx_dps", "gy_dps", "gz_dps")
RAW = ("ax_raw", "ay_raw", "az_raw", "gx_raw", "gy_raw", "gz_raw")


def parse_record(data):
    """Reject malformed/truncated data before acknowledging durable receipt."""
    if not 0 < len(data) <= MAX_BYTES or not data.endswith(b"\n"):
        raise ValueError("invalid record size or incomplete final line")
    stream = io.StringIO(data.decode("utf-8"))
    first = stream.readline()
    if not first.startswith("# "):
        raise ValueError("missing metadata")
    meta = json.loads(first[2:])
    if not isinstance(meta, dict):
        raise ValueError('metadata must be an object')
    accel_range, gyro_range = meta.get('accel_range_g'), meta.get('gyro_range_dps')
    if accel_range not in (8, 16) or gyro_range not in (512, 1024):
        raise ValueError('missing or unsupported sensor range metadata')
    divisors = (32768 / accel_range,) * 3 + (32768 / gyro_range,) * 3
    if not ID.fullmatch(meta.get("id", "")) or not LABEL.fullmatch(meta.get("label", "")):
        raise ValueError("invalid record id or label")
    reader = csv.DictReader(stream)
    if reader.fieldnames != ["seq", "t_us", "sensor_counter", *RAW, *AXES]:
        raise ValueError("unexpected CSV columns")
    rows = []
    for index, row in enumerate(reader):
        values = {k: int(row[k]) for k in ("seq", "t_us", "sensor_counter", *RAW)}
        values.update({k: float(row[k]) for k in AXES})
        if values["seq"] != index or not 0 <= values["t_us"] < 8_000_000:
            raise ValueError("invalid sequence/time")
        if rows and values["t_us"] <= rows[-1]["t_us"]:
            raise ValueError("timestamps must increase")
        if not 0 <= values["sensor_counter"] <= 0xFFFFFF:
            raise ValueError("invalid sensor counter")
        if rows and values["sensor_counter"] == rows[-1]["sensor_counter"]:
            raise ValueError("duplicate sensor sample")
        if any(not -32768 <= values[k] <= 32767 for k in RAW):
            raise ValueError("raw sample out of range")
        if any(not math.isfinite(values[k]) for k in AXES):
            raise ValueError("non-finite sample")
        for raw, scaled, divisor in zip(RAW, AXES, divisors):
            if abs(values[raw] / divisor - values[scaled]) > 0.000001:
                raise ValueError("raw/scaled conversion mismatch")
        rows.append(values)
        if len(rows) > 1200:
            raise ValueError("too many samples")
    if meta.get('samples') != len(rows):
        raise ValueError('sample count does not match metadata')
    return meta, rows


def summarize(meta, rows):
    delta = [math.nan] + [sum(abs(b[k] - a[k]) for k in AXES[:3]) * 1000
                         for a, b in zip(rows, rows[1:])] if rows else []
    norm = [math.sqrt(sum(r[k] ** 2 for k in AXES[3:])) for r in rows]
    gaps = [(b["t_us"] - a["t_us"]) / 1000 for a, b in zip(rows, rows[1:])]
    elapsed = (rows[-1]["t_us"] - rows[0]["t_us"]) / 1e6 if len(rows) > 1 else 0
    sensor_steps = sum((b["sensor_counter"] - a["sensor_counter"]) & 0xFFFFFF
                       for a, b in zip(rows, rows[1:]))
    warnings = []
    for key in ("read_errors", "missed_samples", "clipped_samples", "buffer_full"):
        if meta.get(key):
            warnings.append(f"{key}={meta[key]}")
    if not meta.get("end_cue_ok", True):
        warnings.append("end cue failed")
    if len(rows) < 2:
        warnings.append("insufficient samples")
    elif (len(rows) - 1) / elapsed < 50:
        warnings.append("effective sample rate below 50 Hz")
    if rows and (rows[0]["t_us"] > 100_000 or rows[-1]["t_us"] < 7_900_000):
        warnings.append("record does not cover the full 8-second window")
    result = dict(meta, samples=len(rows),
                  clipped_by_axis={k: sum(abs(r[k]) >= 32760 for r in rows) for k in RAW},
                  clipped_fraction=sum(any(abs(r[k]) >= 32760 for k in RAW) for r in rows) / len(rows) if rows else None,
                  effective_hz=(len(rows) - 1) / elapsed if elapsed else None,
                  sensor_hz_estimate=sensor_steps / elapsed if elapsed else None,
                  max_interval_ms=max(gaps) if gaps else None,
                  gyro_peak_dps=max(norm) if norm else None,
                  accel_delta_peak_mg=max(delta[1:]) if len(delta) > 1 else None,
                  warnings=warnings)
    return result, delta, norm


def plot_record(path):
    """Standalone SVG: six raw-axis traces and two current detector features."""
    path = Path(path)
    meta, rows = parse_record(path.read_bytes())
    summary, delta, norm = summarize(meta, rows)
    title = html.escape(f"{meta['label']} / {meta['id']} / {len(rows)} samples")
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="1100" height="1340" viewBox="0 0 1100 1340">',
           '<rect width="1100" height="1340" fill="white"/>',
           '<g font-family="Arial,sans-serif" font-size="12" fill="#223044">',
           f'<text x="80" y="28" font-size="20">{title}</text>']
    rate = summary["effective_hz"]
    subtitle = f"Effective rate: {rate:.2f} Hz" if rate is not None else "No valid time series"
    svg.append(f'<text x="80" y="50">{subtitle}; ODR code 6, +/-{meta["accel_range_g"]}g, +/-{meta["gyro_range_dps"]}dps, LPF off</text>')
    svg.append('<text x="80" y="69">Reference lines: 750 mg / 90 dps. Adjacent-sample accel delta depends on sample interval.</text>')
    if summary["warnings"]:
        svg.append(f'<text x="80" y="89" fill="#b22222">{html.escape("WARNING: " + "; ".join(summary["warnings"]))}</text>')
    series = [(name, [r[name] for r in rows], None) for name in AXES]
    series += [("accel_delta_mg (reference only)", delta, 750), ("gyro_norm_dps", norm, 90)]
    times = [r["t_us"] / 1e6 for r in rows]
    for i, (name, values, threshold) in enumerate(series):
        top, height, left, width = 125 + i * 150, 100, 80, 980
        finite = [v for v in values if math.isfinite(v)] + [0]
        if threshold is not None:
            finite.append(threshold)
        lo, hi = min(finite), max(finite)
        margin = max((hi - lo) * 0.1, 0.01)
        lo -= margin
        hi += margin
        def y(value):
            return top + height * (hi - value) / (hi - lo)
        svg.append(f'<text x="80" y="{top-12}">{html.escape(name)}</text>')
        for t in range(9):
            x = left + width * t / 8
            svg.append(f'<path d="M{x},{top} v{height}" stroke="#e5e7eb"/>')
            svg.append(f'<text x="{x}" y="{top+height+17}" text-anchor="middle">{t}s</text>')
        for value in (lo, (lo + hi) / 2, hi):
            svg.append(f'<path d="M{left},{y(value):.2f} h{width}" stroke="#e5e7eb"/>')
            svg.append(f'<text x="72" y="{y(value)+4:.2f}" text-anchor="end">{value:.2f}</text>')
        if threshold is not None:
            svg.append(f'<path d="M{left},{y(threshold):.2f} h{width}" stroke="#d94841" stroke-dasharray="6 4"/>')
        # Separate segments at non-finite values; the first acceleration delta is undefined.
        points = " ".join(f'{left+width*t/8:.2f},{y(v):.2f}'
                          for t, v in zip(times, values) if math.isfinite(v))
        svg.append(f'<polyline points="{points}" fill="none" stroke="#1264a3" stroke-width="1.3"/>')
    svg.append('</g></svg>')
    path.with_suffix('.svg').write_text('\n'.join(svg), encoding='utf-8')
    path.with_suffix('.summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    return summary


class Receiver(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, out, device_ip):
        self.out = Path(out)
        self.out.mkdir(parents=True, exist_ok=True)
        self.device_ip = device_ip
        self.save_lock = threading.Lock()
        self.received = {}
        super().__init__(address, ReceiveHandler)


class ReceiveHandler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        self.connection.settimeout(15)
        if self.client_address[0] != self.server.device_ip:
            self.send_error(403)
            return
        try:
            record_id = self.path.removeprefix('/records/')
            if not self.path.startswith('/records/') or not ID.fullmatch(record_id):
                raise ValueError('invalid path')
            size = int(self.headers.get('Content-Length', '0'))
            if not 0 < size <= MAX_BYTES:
                raise ValueError('invalid length')
            data = self.rfile.read(size)
            if len(data) != size:
                raise ValueError('truncated upload')
            meta, _ = parse_record(data)
            if meta['id'] != record_id:
                raise ValueError('id mismatch')
            path = self.server.out / f"{record_id}_{meta['label']}.csv"
            with self.server.save_lock:
                if path.exists():
                    if path.read_bytes() != data:
                        self.send_error(409, 'same id with different data')
                        return
                else:
                    temp = None
                    try:
                        with tempfile.NamedTemporaryFile(dir=self.server.out, delete=False) as f:
                            temp = Path(f.name)
                            f.write(data)
                            f.flush()
                            os.fsync(f.fileno())
                        os.replace(temp, path)
                    finally:
                        if temp is not None and temp.exists():
                            temp.unlink()
                self.server.received[record_id] = path
            # Only durable receipt is acknowledged. Plotting never controls data retention.
            ack = record_id.encode('ascii')
            self.send_response(200)
            self.send_header('Content-Length', str(len(ack)))
            self.end_headers()
            self.wfile.write(ack)
        except (ValueError, KeyError, TypeError, OSError) as exc:
            self.send_error(400, str(exc))


def device_request(ip, path, payload=None):
    data = json.dumps(payload).encode() if payload is not None else None
    request = Request(f'http://{ip}:8080{path}', data=data,
                      headers={'Content-Type': 'application/json'})
    # LAN control must reach the device directly, not an environment HTTP proxy.
    try:
        with build_opener(ProxyHandler({})).open(request, timeout=5) as response:
            return json.load(response)
    except HTTPError as exc:
        detail = exc.read(1024).decode('utf-8', errors='replace').strip()
        raise ValueError(f'HTTP {exc.code} from device: {detail or exc.reason}') from exc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', help='device IPv4 shown in boot Wi-Fi log')
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--out', type=Path, default=Path('imu_records'))
    parser.add_argument('--plot', type=Path, help='replot a saved CSV, no device needed')
    args = parser.parse_args()
    if args.plot:
        print(json.dumps(plot_record(args.plot), indent=2))
        return
    if not args.device or not 1024 <= args.port <= 65535:
        parser.error('--device and a port between 1024 and 65535 are required')
    ip = socket.gethostbyname(args.device)
    with Receiver(('0.0.0.0', args.port), args.out, ip) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        print(f'Receiver :{args.port}; device {ip}:8080; output {args.out.resolve()}')
        print('Type action label (ASCII letters/digits/_/-), or :retry / :status / :quit.')
        print('One beep -> prepare 3s -> record 8s -> two beeps -> upload. Keep battery power on.')
        plotted = set()
        try:
            while True:
                label = input('Action> ').strip()
                if label == ':quit':
                    break
                try:
                    if label == ':status':
                        print(device_request(ip, '/status'))
                        continue
                    if label == ':retry':
                        state = device_request(ip, '/retry', {})
                    elif LABEL.fullmatch(label):
                        state = device_request(ip, '/start', {'label': label, 'port': args.port})
                    else:
                        print('Use an ASCII label, e.g. walk_01, shake_01, tap_01.')
                        continue
                    record_id, last = state['id'], None
                    deadline = time.monotonic() + 45
                    while time.monotonic() < deadline:
                        with server.save_lock:
                            saved = server.received.get(record_id)
                        if saved and record_id not in plotted:
                            summary = plot_record(saved)
                            plotted.add(record_id)
                            print(f'Saved: {saved}\nPlot: {saved.with_suffix(".svg")}')
                            print(f'Samples={summary["samples"]}, effective_hz={summary["effective_hz"]}, warnings={summary["warnings"]}')
                        if state['stage'] != last:
                            print(f'{record_id}: {state["stage"]}')
                            last = state['stage']
                        if state['stage'] in ('saved', 'retry', 'error'):
                            break
                        time.sleep(0.5)
                        state = device_request(ip, '/status')
                    else:
                        print('Timed out waiting; use :status. Do not restart the device.')
                except (HTTPError, URLError, TimeoutError, OSError, ValueError) as exc:
                    print(f'Error: {exc}. Use :status / :retry; RAM survives only while powered.')
        except (EOFError, KeyboardInterrupt):
            pass
        finally:
            server.shutdown()


if __name__ == '__main__':
    main()
