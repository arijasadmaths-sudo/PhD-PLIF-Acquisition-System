#!/usr/bin/env python3
"""Compile and exercise the camera receiver with simulated SDK buffers."""
import csv
import os
from pathlib import Path
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build'
BINARY = BUILD / 'mock_camera'


def compile_receiver():
    BUILD.mkdir(exist_ok=True)
    command = ['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-pthread', '-DLIBTIFF_AVAILABLE', '-I' + str(ROOT/'tests/sdk'),
               str(ROOT/'server.c'), str(ROOT/'tests/mock_sdk.c'), '-o', str(BINARY)]
    subprocess.run(command, check=True, timeout=60)


class RunningReceiver:
    def __init__(self, temporary, extra=(), environment=None):
        self.root = Path(temporary)
        self.output = self.root / 'images'
        self.log_path = self.root / 'stdout.txt'
        self.sdk_log = self.root / 'sdk.txt'
        self.log = self.log_path.open('w')
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0))
            self.port = s.getsockname()[1]
        env = dict(os.environ)
        for key in list(env):
            if key.startswith('MOCK_'):
                env.pop(key)
        env['MOCK_LOG'] = str(self.sdk_log)
        env.update(environment or {})
        self.process = subprocess.Popen([str(BINARY), '--port', str(self.port), '--output', str(self.output), *extra],
                                        stdout=self.log, stderr=subprocess.STDOUT, env=env)

    def wait_ready(self):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if 'Listening on' in self.log_path.read_text():
                return
            if self.process.poll() is not None:
                raise AssertionError(self.log_path.read_text())
            time.sleep(0.02)
        raise AssertionError('Receiver did not become ready:\n' + self.log_path.read_text())

    def connect(self):
        return socket.create_connection(('127.0.0.1', self.port), timeout=3)

    def files(self):
        return sorted(self.output.glob('session-*/img_*.tif'))

    def stop(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGINT)
            try:
                self.process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise AssertionError('Receiver did not stop promptly')
        self.log.close()


def read_reply(connection):
    data = bytearray()
    while not data.endswith(b'\n'):
        part = connection.recv(1)
        if not part:
            raise AssertionError('Receiver disconnected without a complete reply')
        data.extend(part)
        if len(data) > 4096:
            raise AssertionError('Reply too long')
    return bytes(data)


def wait_count(receiver, number):
    deadline = time.monotonic() + 3
    while len(receiver.files()) != number and time.monotonic() < deadline:
        time.sleep(0.03)
    if len(receiver.files()) != number:
        raise AssertionError(receiver.log_path.read_text())


class CameraTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compile_receiver()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.receivers = []

    def tearDown(self):
        for receiver in self.receivers:
            receiver.stop()
        self.temp.cleanup()

    def start(self, extra=(), env=None, ready=True):
        receiver = RunningReceiver(self.temp.name, extra, env)
        self.receivers.append(receiver)
        if ready:
            receiver.wait_ready()
        return receiver

    def check_images(self, receiver, channels=3):
        for path in receiver.files():
            header, payload = path.read_bytes().split(b'\n', 1)
            self.assertEqual(header, f'MOCK_NOT_TIFF 8 6 {channels}'.encode())
            self.assertEqual(len(payload), 48*channels)
            self.assertEqual(len(set(payload)), 1, 'Snapshot was modified during the write')
            self.assertNotEqual(payload[0], 0xee, 'Saved an SDK buffer after release')
        for manifest in receiver.output.glob('session-*/frames.csv'):
            with manifest.open() as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), len(receiver.files()))
            for row in rows:
                image = manifest.parent / row['file']
                self.assertTrue(image.exists())
                self.assertGreater(int(row['received_frame_sequence']), 0)

    def test_repeated_fragmented_and_coalesced_requests(self):
        receiver = self.start()
        with receiver.connect() as s:
            s.sendall(b'st')
            time.sleep(0.05)
            self.assertEqual(receiver.files(), [])
            s.sendall(b'ore\nstore\nstore\r\n')
            for _ in range(3):
                self.assertTrue(read_reply(s).startswith(b'OK '))
        wait_count(receiver, 3)
        self.check_images(receiver)
        receiver.stop()
        calls = receiver.sdk_log.read_text().splitlines()
        self.assertEqual(calls.count('continuous_start'), 1)
        self.assertNotIn('ERROR_buffer_still_owned', calls)
        self.assertLess(calls.index('abort'), calls.index('free_transfer'))
        self.assertLess(calls.index('free_transfer'), calls.index('close'))

    def test_legacy_nul_protocol_and_reconnection(self):
        receiver = self.start()
        with receiver.connect() as s:
            s.sendall(b'store\0store\0')
            wait_count(receiver, 2)
            s.settimeout(0.2)
            with self.assertRaises(socket.timeout):
                s.recv(1)
        with receiver.connect() as s:
            s.sendall(b'store\n')
            self.assertTrue(read_reply(s).startswith(b'OK '))
        wait_count(receiver, 3)
        self.check_images(receiver)

    def test_unknown_oversized_and_incomplete_requests(self):
        receiver = self.start()
        with receiver.connect() as s:
            s.sendall(b'wrong\n' + b'x'*200 + b'store\n')
            self.assertTrue(read_reply(s).startswith(b'ERR '))
            self.assertTrue(read_reply(s).startswith(b'ERR '))
            s.sendall(b'store')
        time.sleep(0.2)
        self.assertEqual(receiver.files(), [])
        with receiver.connect() as s:
            s.sendall(b'\n\0store\n')
            self.assertTrue(read_reply(s).startswith(b'OK '))
        wait_count(receiver, 1)

    def test_write_failure_leaves_no_partial_image(self):
        receiver = self.start(env={'MOCK_WRITE_FAIL':'1'})
        with receiver.connect() as s:
            s.sendall(b'store\n')
            self.assertIn(b'TIFF write failed', read_reply(s))
        self.assertEqual(receiver.files(), [])
        self.assertEqual(list(receiver.output.glob('session-*/.*partial.tif')), [])

    def test_stale_frame_is_rejected(self):
        receiver = self.start(env={'MOCK_STALE':'1'})
        time.sleep(2.3)
        with receiver.connect() as s:
            s.sendall(b'store\n')
            self.assertIn(b'No recent complete camera frame', read_reply(s))
        self.assertEqual(receiver.files(), [])

    def test_idle_connected_client_does_not_block_shutdown(self):
        receiver = self.start()
        with receiver.connect():
            receiver.stop()
        self.assertEqual(receiver.process.returncode, 0)

    def test_start_failure_and_bad_feature_return_failure(self):
        for extra, env in [([], {'MOCK_START_FAIL':'1'}), (['--set','BadFeature=1'], {})]:
            with self.subTest(extra=extra, env=env):
                receiver = self.start(extra, env, ready=False)
                self.assertNotEqual(receiver.process.wait(timeout=4), 0)
                self.assertEqual(receiver.files(), [])
                receiver.stop()

    def test_incomplete_frames_are_not_saved(self):
        receiver = self.start(env={'MOCK_INCOMPLETE':'1'}, ready=False)
        self.assertNotEqual(receiver.process.wait(timeout=8), 0)
        self.assertEqual(receiver.files(), [])
        self.assertNotIn('ERROR_buffer_still_owned', receiver.sdk_log.read_text())

    def test_once_monochrome(self):
        receiver = self.start(['--once'], {'MOCK_MONO':'1'}, ready=False)
        self.assertEqual(receiver.process.wait(timeout=4), 0, receiver.log_path.read_text())
        self.assertEqual(len(receiver.files()), 1)
        self.check_images(receiver, channels=1)

    def test_python_request_tool(self):
        receiver = self.start()
        run = subprocess.run([sys.executable, str(ROOT/'request_frame.py'), '--port', str(receiver.port),
                              '--count','2','--interval','0.1'], capture_output=True, text=True, timeout=5)
        self.assertEqual(run.returncode, 0, run.stderr)
        wait_count(receiver, 2)
        self.check_images(receiver)

    def test_bad_cli(self):
        for arguments in [('--port','-1'), ('--port','garbage'), ('--camera-ip','bad'), ('--set','broken')]:
            run = subprocess.run([str(BINARY), *arguments], capture_output=True, timeout=3)
            self.assertNotEqual(run.returncode, 0)

    def test_sdk_build_wrapper_preserves_original(self):
        parent = Path(self.temp.name)/'examples3'
        source = parent/'genicam_c_demo'
        source.mkdir(parents=True)
        (source/'genicam_c_demo.c').write_text('/* Original vendor example placeholder. */\n')
        (source/'genicam_c_demo').write_text('Stale binary must not be reused\n')
        include = shlex.quote(str(ROOT/'tests/sdk'))
        mock = shlex.quote(str(ROOT/'tests/mock_sdk.c'))
        (source/'Makefile').write_text('genicam_c_demo: genicam_c_demo.c\n\tgcc -std=c11 -pthread -DLIBTIFF_AVAILABLE -I'+include+' genicam_c_demo.c '+mock+' -o genicam_c_demo\n')
        run = subprocess.run([sys.executable, str(ROOT/'build_camera.py'), '--sdk-example', str(source)],
                             capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
        self.assertIn('Original vendor', (source/'genicam_c_demo.c').read_text())
        run = subprocess.run([str(ROOT/'build/plif_camera'), '--help'], capture_output=True, text=True, timeout=3)
        self.assertEqual(run.returncode, 0)
        self.assertIn('--camera-ip', run.stdout)
        second = subprocess.run([sys.executable, str(ROOT/'build_camera.py'), '--sdk-example', str(source)],
                                capture_output=True, text=True, timeout=3)
        self.assertNotEqual(second.returncode, 0, 'Existing SDK work directory was overwritten')


if __name__ == '__main__':
    unittest.main(verbosity=2)
