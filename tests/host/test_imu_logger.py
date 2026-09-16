"""Host checks for durable uploads, retries, CSV validation and plots (no hardware)."""
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('imu_capture', ROOT / 'tools/imu_logger/imu_capture.py')
logger = importlib.util.module_from_spec(spec)
spec.loader.exec_module(logger)


def fixture():
    meta = dict(id='abcdef01-000001', label='walk_01', samples=3, read_errors=0,
                accel_range_g=8, gyro_range_dps=512,
                missed_samples=0, clipped_samples=0, buffer_full=False, end_cue_ok=True)
    header = 'seq,t_us,sensor_counter,' + ','.join(logger.RAW + logger.AXES) + '\n'
    rows = ['0,0,16777215,0,0,4096,0,0,0,0,0,1,0,0,0',
            '1,10000,0,4096,0,4096,6400,0,0,1,0,1,100,0,0',
            '2,20000,1,0,0,4096,0,0,0,0,0,1,0,0,0']
    return ('# ' + json.dumps(meta) + '\n' + header + '\n'.join(rows) + '\n').encode()


class LoggerTests(unittest.TestCase):
    def test_wider_range_and_old_record_compatibility(self):
        import csv
        old_meta, old_rows = logger.parse_record(fixture())
        new_meta = dict(old_meta, accel_range_g=16, gyro_range_dps=1024)
        stream = io.StringIO()
        stream.write('# ' + json.dumps(new_meta) + '\n')
        writer = csv.DictWriter(stream, fieldnames=list(old_rows[0]))
        writer.writeheader()
        for row in old_rows:
            writer.writerow({k: v * 2 if k in logger.AXES else v for k, v in row.items()})
        data = stream.getvalue().encode()
        meta, rows = logger.parse_record(data)
        self.assertEqual(rows[1]['gx_dps'], 200)
        self.assertEqual(rows[1]['ax_g'], 2)
        with self.assertRaises(ValueError):
            logger.parse_record(data.replace(b'"gyro_range_dps": 1024', b'"gyro_range_dps": 512'))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'wide.csv'
            path.write_bytes(data)
            logger.plot_record(path)
            self.assertIn('+/-16g, +/-1024dps', path.with_suffix('.svg').read_text())

    def test_device_error_includes_response_body(self):
        error = HTTPError('http://device/start', 400, 'Bad Request', {},
                          io.BytesIO(b'IPv4 or mapped IPv4 client required'))
        with patch.object(logger, 'build_opener') as opener:
            opener.return_value.open.side_effect = error
            with self.assertRaisesRegex(ValueError, 'HTTP 400.*mapped IPv4'):
                logger.device_request('192.168.137.64', '/start', {'label': 'shake_01', 'port': 8765})

    def test_features_and_plot(self):
        meta, rows = logger.parse_record(fixture())
        summary, delta, norm = logger.summarize(meta, rows)
        self.assertEqual(delta[1:], [1000, 1000])
        self.assertEqual(norm, [0, 100, 0])
        self.assertEqual(summary['effective_hz'], 100)
        self.assertEqual(summary['sensor_hz_estimate'], 100)  # counter wrap
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.csv'
            path.write_bytes(fixture())
            logger.plot_record(path)
            self.assertIn('750 mg / 90 dps', path.with_suffix('.svg').read_text())
            self.assertTrue(path.with_suffix('.summary.json').is_file())

    def test_reject_bad_record(self):
        for data in (fixture()[:-1], fixture().replace(b'walk_01', b'../bad'),
                     fixture().replace(b'1,10000,0,', b'1,0,0,'),
                     fixture().replace(b'"samples": 3', b'"samples": 4'),
                     fixture().replace(b',100,0,0\n', b',101,0,0\n')):
            with self.assertRaises(ValueError):
                logger.parse_record(data)

    def test_durable_idempotent_upload(self):
        with tempfile.TemporaryDirectory() as directory:
            with logger.Receiver(('127.0.0.1', 0), directory, '127.0.0.1') as server:
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                try:
                    url = f'http://127.0.0.1:{server.server_port}/records/abcdef01-000001'
                    for _ in range(2):  # lost response followed by retry: one file
                        with urlopen(Request(url, data=fixture()), timeout=3) as response:
                            self.assertEqual(response.read(), b'abcdef01-000001')
                        self.assertEqual(len(list(Path(directory).glob('*.csv'))), 1)
                        self.assertEqual(next(Path(directory).glob('*.csv')).read_bytes(), fixture())
                    changed = fixture().replace(b'"read_errors": 0', b'"read_errors": 1')
                    with self.assertRaises(HTTPError) as conflict:
                        urlopen(Request(url, data=changed), timeout=3)
                    self.assertEqual(conflict.exception.code, 409)
                    self.assertEqual(next(Path(directory).glob('*.csv')).read_bytes(), fixture())
                    with self.assertRaises(HTTPError):
                        urlopen(Request(url, data=fixture()[:-8]), timeout=3)
                finally:
                    server.shutdown()
                    thread.join()


if __name__ == '__main__':
    unittest.main()
