"""Black-box relay admission, recovery and persistence tests; loopback only."""
import asyncio
import json
import os
from pathlib import Path
import shutil
import subprocess
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

    def handler(self, mode='healthy'):
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
        def handle(r, w):
            self.tasks.append(asyncio.create_task(serve(r, w)))
        return handle

    async def listeners(self, count, mode='healthy'):
        result = []
        for _ in range(count):
            s = await asyncio.start_server(self.handler(mode), '127.0.0.1', 0)
            self.servers.append(s)
            result.append(s.sockets[0].getsockname()[1])
        return result

    async def serve_on(self, port, mode='healthy'):
        """A listener on a port chosen earlier: a peer that is back."""
        s = await asyncio.start_server(self.handler(mode), '127.0.0.1', port)
        self.servers.append(s)
        return s

    async def dead_port(self):
        """A port nobody listens on, yet."""
        s = await asyncio.start_server(lambda r, w: None, '127.0.0.1', 0)
        port = s.sockets[0].getsockname()[1]
        s.close()
        await s.wait_closed()
        return port

    def saved(self, port):
        """What the node saved of one endpoint in peer-state-v1.txt, once it has."""
        try:
            text = (self.root / 'peer-state-v1.txt').read_text()
        except OSError:
            return None
        for line in text.splitlines():
            f = line.split(',')
            if len(f) >= 8 and f[3] == f'127.0.0.1:{port}':
                return {'success': int(f[4]), 'attempt': int(f[5]), 'retry': int(f[6]),
                        'failures': int(f[7])}
        return None

    async def start(self, ports=(), explicit=True, extra=(), listen=None):
        cmd = [BINARY, BACKEND, '-D', str(self.root), '-b', '-B', '-L']
        cmd += ['-a', f'127.0.0.1:{listen}'] if listen else ['-i']
        if explicit:
            cmd.append('-e')
        for port in ports:
            cmd += ['-P', f'127.0.0.1:{port}']
        cmd += list(extra)
        self.process = await asyncio.create_subprocess_exec(*cmd, stdout=self.log, stderr=self.log)
        self.started = time.monotonic()

    async def inbound(self, port):
        """A peer that dials the node and completes the handshake; False if it is not let in."""
        try:
            reader, writer = await asyncio.wait_for(asyncio.open_connection('127.0.0.1', port), 3)
        except (OSError, asyncio.TimeoutError):
            return False
        self.writers.append(writer)
        try:
            writer.write(frame('version', version('127.0.0.1', 1)))
            await writer.drain()
            got = set()
            while not {'version', 'verack'} <= got:
                command, _ = await asyncio.wait_for(receive(reader), 4)
                got.add(command)
            writer.write(frame('verack'))
            await writer.drain()
        except (OSError, asyncio.TimeoutError, asyncio.IncompleteReadError):
            return False

        async def read():
            try:
                while True:
                    await receive(reader)
            except (OSError, asyncio.IncompleteReadError, ValueError):
                pass
        self.tasks.append(asyncio.create_task(read()))
        return True

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
        await self.until(lambda: self.status().get('peers') == 5, 15)

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
        await self.until(lambda: self.status().get('peers') == 5, 15)

    # A peer the operator named is a choice, not a guess. It may be a node that is only
    # sometimes up (an app that is quit and started again), so it is dialed again in
    # seconds for as long as it takes; the hour a stale public address waits is for
    # addresses most of which are dead. Before, a quiet app cost the responder minutes.
    async def test_explicit_peer_that_comes_up_late_is_dialed_within_seconds(self):
        port = await self.dead_port()
        await self.start([port])
        await self.until(lambda: self.status().get('attempts', 0) >= 1, 5)
        await asyncio.sleep(1)
        await self.serve_on(port)
        await self.until(lambda: self.status().get('peers') == 1, 20)

    async def test_explicit_peer_that_went_away_is_dialed_again_within_seconds(self):
        (port,) = await self.listeners(1)
        await self.start([port])
        await self.until(lambda: self.status().get('peers') == 1, 10)
        # Connections first: since Python 3.12 wait_closed() waits for them.
        for w in self.writers:
            w.close()
        self.servers[0].close()
        await self.servers[0].wait_closed()
        await self.until(lambda: self.status().get('peers') == 0, 10)
        await asyncio.sleep(1)
        await self.serve_on(port)
        await self.until(lambda: self.status().get('peers') == 1, 20)

    async def test_explicit_peer_ignores_a_backoff_saved_by_an_earlier_run(self):
        (port,) = await self.listeners(1)
        now = int(time.time())
        # Six failures and an hour to wait, as an app that was quit all night leaves it.
        (self.root / 'peer-state-v1.txt').write_text(
            f'{now - 100},1,1,127.0.0.1:{port},{now - 100},{now - 10},{now + 3600},6\n')
        await self.start([port])
        await self.until(lambda: self.status().get('peers') == 1, 10)

    # An address that connected within 28 days is worth coming back to: when it fails it may
    # be our own network that was down, so it waits minutes, not the hour a dead one does.
    async def test_peer_that_worked_is_retried_within_minutes(self):
        if BACKEND != '--node':
            self.skipTest('the Qt relay keeps no such schedule')
        port = await self.dead_port()
        now = int(time.time())
        (self.root / 'peer-state-v1.txt').write_text(
            f'{now - 3600},1,1,127.0.0.1:{port},{now - 3600},{now - 3600},0,5\n')
        await self.start(explicit=False)
        await self.until(lambda: (self.saved(port) or {}).get('failures') == 6, 15)
        self.assertLessEqual(self.saved(port)['retry'] - int(time.time()), 305)

    async def test_peer_that_never_worked_keeps_the_public_schedule(self):
        if BACKEND != '--node':
            self.skipTest('the Qt relay keeps no such schedule')
        port = await self.dead_port()
        now = int(time.time())
        (self.root / 'peer-state-v1.txt').write_text(
            f'{now - 3600},1,1,127.0.0.1:{port},0,{now - 3600},0,5\n')
        await self.start(explicit=False)
        await self.until(lambda: (self.saved(port) or {}).get('failures') == 6, 15)
        self.assertGreater(self.saved(port)['retry'] - int(time.time()), 1800)

    # A node that wants to be well connected (a service, not a desktop app) asks for more
    # slots; the default stays at the eight a desktop needs.
    async def test_outgoing_slots_are_an_option(self):
        if BACKEND != '--node':
            self.skipTest('the Qt relay has no such option')
        await self.start(await self.listeners(20), extra=['-o', '12'])
        await self.until(lambda: self.status().get('peers') == 12, 10)
        for _ in range(20):
            self.assertLessEqual(self.status()['peers'], 12)
            await asyncio.sleep(.1)

    async def inbound_peers(self, count, extra=()):
        s = await asyncio.start_server(lambda r, w: None, '127.0.0.1', 0)
        port = s.sockets[0].getsockname()[1]
        s.close()
        await s.wait_closed()
        await self.start(listen=port, extra=extra)
        await self.until(lambda: 'listen_port' in self.status() and self.status()['listen_port'], 10)
        results = await asyncio.gather(*[self.inbound(port) for _ in range(count)])
        return sum(results)

    async def test_incoming_slots_default_to_eight(self):
        if BACKEND != '--node':
            self.skipTest('the Qt relay has no such option')
        self.assertEqual(await self.inbound_peers(12), 8)
        await self.until(lambda: self.status().get('established_incoming') == 8, 10)

    async def test_incoming_slots_are_an_option(self):
        if BACKEND != '--node':
            self.skipTest('the Qt relay has no such option')
        self.assertEqual(await self.inbound_peers(12, extra=['-n', '12']), 12)
        await self.until(lambda: self.status().get('established_incoming') == 12, 10)

    # The system probes a peer that went quiet (a laptop that slept, a router that forgot
    # the flow), on the sockets the node dials and on those it accepts. Linux only:
    # `ss -o` shows the timer.
    async def test_peer_sockets_are_kept_alive(self):
        if sys.platform != 'linux' or not shutil.which('ss') or BACKEND != '--node':
            self.skipTest('needs the notbit node and ss, as on Linux')

        def timers(*filters):
            return subprocess.run(['ss', '-tnoH', 'state', 'established', *filters],
                                  capture_output=True, text=True).stdout
        (port,) = await self.listeners(1)
        s = await asyncio.start_server(lambda r, w: None, '127.0.0.1', 0)
        listen = s.sockets[0].getsockname()[1]
        s.close()
        await s.wait_closed()
        await self.start([port], listen=listen)
        await self.until(lambda: self.status().get('peers') == 1, 10)
        self.assertTrue(await self.inbound(listen))

        async def keepalive(filter, what):
            # ss shows one timer at a time: the retransmit one while data is in flight, the
            # keepalive one once the socket is quiet.
            for _ in range(100):
                if 'keepalive' in timers(filter):
                    return
                await asyncio.sleep(.1)
            self.fail(f'no keepalive timer on {what}: {timers(filter)!r}')
        await keepalive(f'( dport = :{port} )', 'a socket the node dialed')
        await keepalive(f'( sport = :{listen} )', 'a socket the node accepted')

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
