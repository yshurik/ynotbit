#!/usr/bin/env python3
"""Compare fresh and 125-minute-old caches using five healthy loopback peers."""
import argparse
import asyncio
import json
from pathlib import Path
import tempfile
import time

from peer_probe import frame, receive, version


async def run(binary):
    results = []
    for age in (0, 7500):
        accepted, tasks, writers = [], [], []

        async def peer(reader, writer):
            accepted.append(1)
            writers.append(writer)
            try:
                await receive(reader)
                port = writer.get_extra_info('sockname')[1]
                writer.write(frame('version', version('127.0.0.1', port)) + frame('verack'))
                await writer.drain()
                while True:
                    await receive(reader)
            except (OSError, asyncio.IncompleteReadError):
                pass

        def handler(reader, writer):
            tasks.append(asyncio.create_task(peer(reader, writer)))

        servers = [await asyncio.start_server(handler, '127.0.0.1', 0) for _ in range(5)]
        with tempfile.TemporaryDirectory() as root, tempfile.TemporaryFile() as log:
            Path(root, 'addr-list.txt').write_text(''.join(
                f'{int(time.time())-age},1,1,127.0.0.1:{s.sockets[0].getsockname()[1]}\n'
                for s in servers))
            process = await asyncio.create_subprocess_exec(
                binary, '--node', '-D', root, '-b', '-B', '-L', '-i', stdout=log, stderr=log)
            try:
                await asyncio.sleep(5)
                results.append({'cache_age_seconds': age, 'accepted': len(accepted),
                                'status': json.loads(Path(root, 'status.json').read_text())})
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
    return results


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    results = asyncio.run(run(args.binary))
    Path(args.output).write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2))
