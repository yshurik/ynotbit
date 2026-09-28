"""Black-box relay admission, recovery and persistence tests; loopback only."""
import asyncio
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from peer_probe import frame, receive, version

BINARY = sys.argv.pop(1)
BACKEND = os.environ.get('YNOTBIT_TEST_NODE_FLAG', '--node')


class Connectivity(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.servers, self.writers, self.tasks = [], [], []
        self.accepted = 0
        self.closed = 0
        self.process = None
        self.log = tempfile.TemporaryFile()

    async def asyncTearDown(self):
        if self.process and self.process.returncode is None:
            self.process.terminate()
            await asyncio.wait_for(self.process.wait(), 5)
        for w in self.writers:
            w.close()
        for s in self.servers:
            s.close()
            await s.wait_closed()
        for task in self.tasks:
            task.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)
        self.log.close()
        self.tmp.cleanup()

    async def listeners(self, count, mode='healthy'):
        async def serve(reader, writer):
            self.accepted += 1
            ordinal = self.accepted
            self.writers.append(writer)
            try:
                command, _ = await receive(reader)
                self.assertEqual(command, 'version')
                if mode == 'fatal':
                    reason = b'Server full'
                    writer.write(frame('error', b'\2\0\0' + bytes([len(reason)]) + reason))
                elif mode == 'malformed':
                    writer.write(frame('error', b'\xfd'))
                elif mode == 'healthy' or (mode == 'starve' and (ordinal == 1 or ordinal > 8)):
                    port = writer.get_extra_info('sockname')[1]
                    writer.write(frame('version', version('127.0.0.1', port)) + frame('verack'))
                await writer.drain()
                while True:
                    command, _ = await receive(reader)
                    if command == 'ping':
                        writer.write(frame('pong'))
                        await writer.drain()
            except (OSError, asyncio.IncompleteReadError):
                self.closed += 1
        def handler(r, w):
            self.tasks.append(asyncio.create_task(serve(r, w)))
        result = []
        for _ in range(count):
            s = await asyncio.start_server(handler, '127.0.0.1', 0)
            self.servers.append(s)
            result.append(s.sockets[0].getsockname()[1])
        return result

    async def start(self, ports=(), explicit=True):
        cmd = [BINARY, BACKEND, '-D', str(self.root), '-b', '-B', '-L', '-i']
        if explicit:
            cmd.append('-e')
        for port in ports:
            cmd += ['-P', f'127.0.0.1:{port}']
        self.process = await asyncio.create_subprocess_exec(*cmd, stdout=self.log, stderr=self.log)
        self.started = time.monotonic()

    def status(self):
        if self.process.returncode is not None:
            self.log.seek(0)
            self.fail(f'relay exited {self.process.returncode}: {self.log.read().decode(errors="replace")}')
        p = self.root / 'status.json'
        try:
            return json.loads(p.read_text()) if p.exists() else {}
        except PermissionError:
            # Windows refuses opens while the relay atomically replaces the
            # file; the next poll reads the new one.
            return {}

    async def until(self, predicate, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.status()
            if predicate():
                return
            await asyncio.sleep(.05)
        self.fail(f'condition not met in {seconds}s; accepted={self.accepted}, closed={self.closed}, status={self.status()}')

    async def test_healthy_start_and_cap(self):
        await self.start(await self.listeners(20))
        await self.until(lambda: self.status().get('peers', 0) >= 5, 5)
        await self.until(lambda: self.status().get('peers') == 8, 5)
        for _ in range(20):
            self.assertLessEqual(self.status()['peers'], 8)
            await asyncio.sleep(.1)

    async def test_stalled_setups_do_not_starve_healthy_candidates(self):
        await self.start(await self.listeners(13, 'starve'))
        await self.until(lambda: self.status().get('peers', 0) >= 5, 25)
        self.assertLessEqual(self.status()['pending_outgoing'], 16)
        self.assertLessEqual(self.status()['established_outgoing'], 8)

    async def test_silent_handshakes_expire_without_crash(self):
        await self.start(await self.listeners(8, 'silent'))
        await self.until(lambda: self.accepted == 8, 10)
        await self.until(lambda: self.closed == 8, 24)
        self.assertEqual(self.status()['peers'], 0)

    async def test_fatal_rejection_releases_socket(self):
        await self.start(await self.listeners(1, 'fatal'))
        await self.until(lambda: self.closed == 1, 3)
        await self.until(lambda: self.status().get('rejections') == 1, 2)

    async def test_malformed_error_releases_socket(self):
        await self.start(await self.listeners(1, 'malformed'))
        await self.until(lambda: self.closed == 1, 3)

    async def test_125_minute_cache_is_still_dialed(self):
        ports = await self.listeners(5)
        if BACKEND == '--node':
            (self.root / 'addr-list.txt').write_text(''.join(f'{int(time.time())-7500},1,1,127.0.0.1:{p}\n' for p in ports))
        else:
            (self.root / 'qt-peers.json').write_text(json.dumps([f'127.0.0.1:{p}' for p in ports]))
        await self.start(explicit=False)
        await self.until(lambda: self.status().get('peers') == 5, 5)

    async def test_verified_history_survives_24_hours(self):
        ports = await self.listeners(5)
        yesterday = int(time.time()) - 86400
        if BACKEND == '--node':
            (self.root / 'peer-state-v1.txt').write_text(''.join(
                f'{yesterday},1,1,127.0.0.1:{p},{yesterday},{yesterday},0,0\n' for p in ports))
        else:
            (self.root / 'qt-peer-state-v1.json').write_text(json.dumps([
                {'endpoint': f'127.0.0.1:{p}', 'advertised': yesterday,
                 'success': yesterday, 'attempt': yesterday, 'retry': 0, 'failures': 0}
                for p in ports]))
        await self.start(explicit=False)
        await self.until(lambda: self.status().get('peers') == 5, 5)

    async def test_churn_replenishes_outgoing_target(self):
        await self.start(await self.listeners(16))
        await self.until(lambda: self.status().get('peers') == 8, 10)
        # Stop three established endpoints, keeping the others available.
        for w in self.writers[:3]:
            w.close()
        await asyncio.sleep(1.2)
        await self.until(lambda: self.status().get('peers') == 8, 30)


if __name__ == '__main__':
    unittest.main()
