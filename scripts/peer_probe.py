#!/usr/bin/env python3
"""Bounded Bitmessage diagnostics; stdlib only, no keys or message publication.

public: TCP/handshake checks of DNS seeds and a supplied addr-list snapshot.
loopback: reproduce slot starvation using the real relay and local fake peers.
"""
import argparse
import asyncio
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import socket
import struct
import tempfile
import time


def frame(command, payload=b''):
    return bytes.fromhex('e9beb4d9') + command.encode().ljust(12, b'\0') + struct.pack('>I', len(payload)) + hashlib.sha512(payload).digest()[:4] + payload


def version(host, port):
    ip = ipaddress.ip_address(host)
    packed = ip.packed if ip.version == 6 else b'\0' * 10 + b'\xff\xff' + ip.packed
    address = struct.pack('>Q', 1) + packed + struct.pack('>H', port)
    agent = b'/ynotbit-diagnostic:1/'
    return struct.pack('>IQQ', 3, 1, int(time.time())) + address + address + os.urandom(8) + bytes([len(agent)]) + agent + b'\1\1'


async def receive(reader):
    header = await reader.readexactly(24)
    if header[:4] != bytes.fromhex('e9beb4d9'):
        raise ValueError('bad magic')
    size = struct.unpack('>I', header[16:20])[0]
    if size > 1600100:
        raise ValueError('oversized packet')
    payload = await reader.readexactly(size)
    if hashlib.sha512(payload).digest()[:4] != header[20:24]:
        raise ValueError('bad checksum')
    return header[4:16].rstrip(b'\0').decode('ascii'), payload


def varint(data):
    n = data[0]
    if n < 253:
        return n, 1
    size = {253: 2, 254: 4, 255: 8}[n]
    return int.from_bytes(data[1:1 + size], 'big'), 1 + size


def addresses(data):
    count, offset = varint(data)
    if count > 1000 or len(data) != offset + count * 38:
        raise ValueError('invalid addr')
    result = []
    for i in range(count):
        timestamp, stream, services, packed, port = struct.unpack('>QIQ16sH', data[offset+i*38:offset+(i+1)*38])
        ip = ipaddress.IPv6Address(packed)
        ip = ip.ipv4_mapped or ip
        if ip.is_global and stream == 1 and port:
            result.append({'host': str(ip), 'port': port, 'timestamp': timestamp, 'services': services})
    return result


async def public(args):
    endpoints = {}
    dns = {}
    for host, port in [('bootstrap8080.bitmessage.org', 8080), ('bootstrap8444.bitmessage.org', 8444)]:
        try:
            records = await asyncio.get_running_loop().getaddrinfo(host, port, type=socket.SOCK_STREAM)
            dns[host] = sorted({r[4][0] for r in records})
            for ip in dns[host]:
                endpoints[(ip, port)] = 'dns'
        except OSError as e:
            dns[host] = str(e)
    ages = []
    if args.addresses:
        for line in Path(args.addresses).read_text().splitlines():
            timestamp, stream, services, endpoint = line.split(',', 3)
            host, port = endpoint.rsplit(':', 1)
            host = host.strip('[]')
            if ipaddress.ip_address(host).is_global and int(stream) == 1:
                endpoints.setdefault((host, int(port)), 'cache')
                ages.append(int(time.time()) - int(timestamp))
    semaphore = asyncio.Semaphore(args.concurrency)
    started = time.monotonic()
    async def check(endpoint, source):
        host, port = endpoint
        result = {'host': host, 'port': port, 'source': source}
        writer = None
        async with semaphore:
            begin = time.monotonic()
            try:
                reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), args.timeout)
                result['tcp_seconds'] = round(time.monotonic()-begin, 3)
                writer.write(frame('version', version(host, port)))
                await writer.drain()
                got_version = got_verack = False
                deadline = time.monotonic() + args.timeout
                seen = []
                discovered = []
                while time.monotonic() < deadline:
                    command, data = await asyncio.wait_for(receive(reader), max(.01, deadline-time.monotonic()))
                    seen.append(command)
                    if command == 'version':
                        got_version = True
                        length, offset = varint(data[80:])
                        result['agent'] = data[80+offset:80+offset+length].decode(errors='replace')
                        result['services'] = int.from_bytes(data[4:12], 'big')
                        writer.write(frame('verack'))
                    elif command == 'verack':
                        got_verack = True
                    elif command == 'ping':
                        writer.write(frame('pong'))
                    elif command == 'addr':
                        discovered.extend(addresses(data))
                    elif command == 'error':
                        result['remote_error_hex'] = data.hex()
                    if got_version and got_verack and 'handshake_seconds' not in result:
                        result['handshake_seconds'] = round(time.monotonic()-begin, 3)
                        writer.write(frame('getaddr'))
                        deadline = time.monotonic() + args.hold
                    await writer.drain()
                result['end'] = 'hold_complete'
            except (OSError, ValueError, asyncio.IncompleteReadError, asyncio.TimeoutError) as e:
                result['end'] = type(e).__name__ + ': ' + str(e)
            finally:
                if writer:
                    writer.close()
                    try:
                        await writer.wait_closed()
                    except OSError:
                        pass
                result['elapsed_seconds'] = round(time.monotonic()-begin, 3)
                if 'handshake_seconds' in result and result['elapsed_seconds'] >= result['handshake_seconds'] + args.hold - .1:
                    result['held_for_requested_duration'] = True
                    if result.get('end') == 'TimeoutError: ':
                        result['end'] = 'hold_complete'
                result['commands'] = locals().get('seen', [])
                result['discovered'] = locals().get('discovered', [])
        print(json.dumps(result), flush=True)
        return result
    results = await asyncio.gather(*(check(e, s) for e, s in list(endpoints.items())[:args.limit]))
    summary = {'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'dns': dns, 'cache_count': len(ages), 'cache_older_than_2h': sum(a >= 7200 for a in ages), 'elapsed_seconds': round(time.monotonic()-started, 3), 'tcp_success': sum('tcp_seconds' in r for r in results), 'handshake_success': sum('handshake_seconds' in r for r in results), 'results': results}
    Path(args.output).write_text(json.dumps(summary, indent=2)+'\n')
    print(json.dumps({k:v for k,v in summary.items() if k != 'results'}), flush=True)


async def loopback(args):
    writers, tasks, servers = [], [], []
    counts = {'silent': 0, 'healthy': 0}
    async def accept(reader, writer):
        kind = 'silent' if counts['silent'] < 8 else 'healthy'
        writers.append(writer)
        counts[kind] += 1
        try:
            await receive(reader)
            if kind == 'healthy':
                writer.write(frame('version', version('127.0.0.1', writer.get_extra_info('sockname')[1])) + frame('verack'))
                await writer.drain()
            while True:
                await receive(reader)
        except (OSError, asyncio.IncompleteReadError):
            pass
    def handler(r, w):
        tasks.append(asyncio.create_task(accept(r, w)))
    for i in range(13):
        servers.append(await asyncio.start_server(handler, '127.0.0.1', 0))
    ports = [s.sockets[0].getsockname()[1] for s in servers]
    with tempfile.TemporaryDirectory(prefix='ynotbit-peer-probe-') as root:
        command = [args.binary, '--node', '-D', root, '-b', '-B', '-L', '-i', '-e']
        for port in ports:
            command += ['-P', f'127.0.0.1:{port}']
        with tempfile.TemporaryFile() as log:
            process = await asyncio.create_subprocess_exec(*command, stdout=log, stderr=log)
            rows = []
            start = time.monotonic()
            try:
                deadline = start + 15
                while counts['silent'] < 8 and time.monotonic() < deadline:
                    await asyncio.sleep(.1)
                if counts['silent'] != 8:
                    raise RuntimeError(f'Expected 8 initial peers: {counts}')
                initial_ports = {w.get_extra_info('sockname')[1] for w in writers[:8]}
                if args.reject:
                    reason = b'Too many connections from your IP. Closing connection.'
                    for writer in writers[1:8]:
                        writer.write(frame('error', b'\2\0\0' + bytes([len(reason)]) + reason))
                        await writer.drain()
                # One initial peer completes its handshake. Five untried endpoints
                # are already eligible and will answer immediately when dialed.
                first = writers[0]
                first.write(frame('version', version('127.0.0.1', first.get_extra_info('sockname')[1])) + frame('verack'))
                await first.drain()
                await asyncio.sleep(1)
                for second in range(args.duration):
                    await asyncio.sleep(1)
                    status = Path(root, 'status.json')
                    row = {'seconds': round(time.monotonic()-start, 2), 'accepted': dict(counts), 'status': json.loads(status.read_text()) if status.exists() else None}
                    rows.append(row)
                    if second % 10 == 0:
                        print(json.dumps(row), flush=True)
                    if process.returncode is not None:
                        break
                # Release occupied slots, keeping the first fully established peer alive.
                for writer in writers[1:8]:
                    writer.close()
                for server in servers:
                    if server.sockets[0].getsockname()[1] in initial_ports:
                        server.close()
                for _ in range(10):
                    if process.returncode is not None:
                        break
                    await asyncio.sleep(1)
                    status = Path(root, 'status.json')
                    rows.append({'seconds': round(time.monotonic()-start, 2), 'released': True, 'accepted': dict(counts), 'status': json.loads(status.read_text())})
                print(json.dumps(rows[-1]), flush=True)
            finally:
                if process.returncode is None:
                    process.terminate()
                    await asyncio.wait_for(process.wait(), 5)
                for writer in writers:
                    writer.close()
                for server in servers:
                    server.close()
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
            log.seek(0)
            Path(args.output).write_text(json.dumps({'backend': '--node', 'returncode': process.returncode, 'samples': rows, 'log': log.read().decode(errors='replace')}, indent=2)+'\n')


async def control(args):
    """Run the unmodified relay in a temporary store against measured good peers."""
    survey = json.loads(Path(args.survey).read_text())
    endpoints = []
    hosts = set()
    for r in survey['results']:
        if 'handshake_seconds' in r and r['host'] not in hosts:
            ip = ipaddress.ip_address(r['host'])
            if not ip.is_global:
                continue
            hosts.add(r['host'])
            host = f'[{ip}]' if ip.version == 6 else str(ip)
            endpoints.append(f"{host}:{int(r['port'])}")
    endpoints = endpoints[:8]
    if not endpoints:
        raise ValueError('Survey has no successful peers')
    with tempfile.TemporaryDirectory(prefix='ynotbit-live-control-') as root, tempfile.TemporaryFile() as log:
        command = [args.binary, '--node', '-D', root, '-b', '-B', '-e', '-i']
        for endpoint in endpoints:
            command += ['-P', endpoint]
        process = await asyncio.create_subprocess_exec(*command, stdout=log, stderr=log)
        start = time.monotonic()
        rows = []
        try:
            while time.monotonic() - start <= args.duration:
                status = Path(root, 'status.json')
                row = {'seconds': round(time.monotonic()-start, 3), 'status': json.loads(status.read_text()) if status.exists() else None}
                rows.append(row)
                print(json.dumps(row), flush=True)
                if process.returncode is not None:
                    raise RuntimeError(f'Relay exited: {process.returncode}')
                await asyncio.sleep(1)
        finally:
            if process.returncode is None:
                process.terminate()
                await asyncio.wait_for(process.wait(), 5)
            log.seek(0)
            Path(args.output).write_text(json.dumps({'endpoints': endpoints, 'samples': rows, 'log': log.read().decode(errors='replace')}, indent=2)+'\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    p = sub.add_parser('public')
    p.add_argument('--addresses')
    p.add_argument('--concurrency', type=int, default=12)
    p.add_argument('--timeout', type=float, default=5)
    p.add_argument('--hold', type=float, default=10)
    p.add_argument('--limit', type=int, default=400)
    p.add_argument('--output', required=True)
    p = sub.add_parser('loopback')
    p.add_argument('--binary', required=True)
    p.add_argument('--duration', type=int, default=45)
    p.add_argument('--reject', action='store_true', help='Send fatal errors on seven stalled connections')
    p.add_argument('--output', required=True)
    p = sub.add_parser('control', help='Temporary real relay; receives public objects but publishes no local messages')
    p.add_argument('--binary', required=True)
    p.add_argument('--survey', required=True)
    p.add_argument('--duration', type=int, default=180)
    p.add_argument('--output', required=True)
    args = parser.parse_args()
    if getattr(args, 'concurrency', 1) < 1 or getattr(args, 'timeout', 1) <= 0 or getattr(args, 'duration', 1) < 1 or getattr(args, 'hold', 1) < 0:
        parser.error('concurrency, timeout and duration must be positive; hold must be nonnegative')
    asyncio.run({'public': public, 'loopback': loopback, 'control': control}[args.mode](args))


if __name__ == '__main__':
    main()
