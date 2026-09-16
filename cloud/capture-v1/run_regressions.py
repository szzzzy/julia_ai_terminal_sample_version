"""Run the existing cloud regression suite locally using its legacy WebSocket API."""
import argparse
from pathlib import Path
import sys
import unittest
import websockets
from websockets.legacy.client import connect
from websockets.legacy.server import serve

p=argparse.ArgumentParser()
p.add_argument('cloud_root',type=Path)
a=p.parse_args()
# The inspected production adapter uses create_protocol/process_request from the
# legacy API. New websockets packages still ship it under an explicit namespace.
websockets.serve=serve
websockets.connect=connect
sys.path[:0]=[str(a.cloud_root/'tests'),str(a.cloud_root/'server'),str(a.cloud_root/'engine')]
suite=unittest.TestLoader().loadTestsFromNames([
    'test_state_sync','test_voice_state_wait','test_control_protocol','test_protocol_fields'])
result=unittest.TextTestRunner(verbosity=2).run(suite)
raise SystemExit(not result.wasSuccessful())
