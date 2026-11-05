"""Measure real in-place dataset loads, without calling a model API."""
import argparse
import asyncio
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from backend.config import load_config
from backend.datasets import descriptor
from backend.graph_worker import GraphWorker


async def main(args):
    results = []
    for path in args.descriptors:
        for run in range(1, args.repeats + 1):
            worker = GraphWorker(load_config())
            try:
                started = time.perf_counter()
                dataset = descriptor(path)
                await worker.load(dataset)
                elapsed = time.perf_counter() - started
                row = dict(dataset=dataset['id'], run=run, wall_seconds=round(elapsed, 3),
                           **{k: worker.info[k] for k in ('vertices', 'edges', 'total_weight')})
                results.append(row)
                print(json.dumps(row), flush=True)
            finally:
                await worker.stop()
    if args.output:
        Path(args.output).write_text(json.dumps(results, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('descriptors', nargs='+')
    parser.add_argument('--repeats', type=int, default=2)
    parser.add_argument('--output')
    asyncio.run(main(parser.parse_args()))
