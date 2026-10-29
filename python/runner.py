"""Runs many backtests in parallel: one job per (symbol, strategy, latency).

    PYTHONPATH=build-release:python python3 python/runner.py ~/data/itch/12302019 results.jsonl \\
        --strategies join,imbalance --latencies 10

Each job reads only its own symbol's file (made by tools/itch_split), so jobs
are independent: they can run in any order, on any core, or on any machine
that has the file.

- Each finished job is appended to a JSON Lines file at once. Re-running
  skips jobs already recorded, so an interrupted run resumes where it stopped
  and running twice does no extra work.
- A job that raises is recorded with its error and doesn't stop the others.
  Failed jobs aren't counted as done, so the next run retries them.
"""

from __future__ import annotations

import argparse
import functools
import json
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import asdict, dataclass
from pathlib import Path

REBATE = 20  # $0.0020 per share filled


@dataclass(frozen=True)
class Job:
    symbol: str
    strategy: str
    latency_us: int

    def key(self) -> tuple:
        return (self.symbol, self.strategy, self.latency_us)


def plan(symbols, strategies, latencies) -> list[Job]:
    return [Job(sym, strat, lat) for sym in symbols for strat in strategies for lat in latencies]


def finished(path: Path) -> set[tuple]:
    """Keys of jobs that already succeeded in `path`."""
    if not path.exists():
        return set()
    keys = set()
    for line in path.read_text().splitlines():
        record = json.loads(line)
        if "error" not in record:
            keys.add((record["symbol"], record["strategy"], record["latency_us"]))
    return keys


def run_job(data_dir: str, job: Job) -> dict:
    """One market-making backtest on one symbol's split file."""
    from itchpy import PassiveSim
    from market_making import STRATEGIES, make_markets

    started = time.perf_counter()
    sim = PassiveSim(Path(data_dir) / f"{job.symbol}.itch", job.symbol, latency_ns=job.latency_us * 1_000)
    r = make_markets(sim, STRATEGIES[job.strategy](size=100, max_position=500), rebate_per_share=REBATE)
    return {
        **asdict(job),
        "fills": len(r.fills),
        "shares": r.shares_filled,
        "spread_captured": r.spread_captured,
        "markout_1s": r.markout_1s,
        "markout_60s": r.markout_60s,
        "rebates": r.rebates,
        "pnl": r.pnl,
        "max_position": r.max_position,
        "rejected": r.rejected,
        "clean": not (sim.malformed or sim.book_errors or sim.truncated),
        "seconds": round(time.perf_counter() - started, 2),
    }


def attempt(work, job: Job) -> dict:
    """Runs one job, turning an exception into an error record."""
    try:
        return work(job)
    except Exception as e:  # noqa: BLE001 - one bad job mustn't stop the rest
        return {**asdict(job), "error": f"{type(e).__name__}: {e}"}


def run_all(jobs: list[Job], work, out_path: Path, workers: int) -> list[dict]:
    """Runs the jobs not yet recorded in `out_path`, appending each result as
    it finishes. `work` must be picklable (a module-level function, or a
    functools.partial of one) when `workers` > 1."""
    done = finished(out_path)
    pending = [j for j in jobs if j.key() not in done]
    records = []
    with out_path.open("a") as out:
        def record(r):
            out.write(json.dumps(r) + "\n")
            out.flush()  # on disk now, so a crash loses at most the jobs still running
            records.append(r)

        if workers <= 1:
            for job in pending:
                record(attempt(work, job))
        else:
            with ProcessPoolExecutor(max_workers=workers) as pool:
                futures = {pool.submit(attempt, work, job): job for job in pending}
                for future in as_completed(futures):
                    try:
                        record(future.result())
                    except Exception as e:  # the worker process itself died
                        record({**asdict(futures[future]), "error": f"{type(e).__name__}: {e}"})
    return records


def summarize(records: list[dict]) -> str:
    """Totals per (strategy, latency), weighted by shares filled."""
    groups: dict[tuple, list[dict]] = {}
    for r in records:
        if "error" not in r:
            groups.setdefault((r["strategy"], r["latency_us"]), []).append(r)

    lines = []
    for (strategy, latency), rs in sorted(groups.items()):
        shares = sum(r["shares"] for r in rs)
        per_share = lambda field: 100 * sum(r[field] for r in rs) / shares if shares else 0.0  # noqa: E731
        winners = sum(1 for r in rs if r["pnl"] > 0)
        lines.append(f"{strategy}, {latency} us: {len(rs)} symbols, {shares:,} shares filled")
        lines.append(f"  per share: spread captured {per_share('spread_captured'):+.3f}c, "
                     f"markout 1 s {per_share('markout_1s'):+.3f}c, "
                     f"markout 60 s {per_share('markout_60s'):+.3f}c, PnL {per_share('pnl'):+.3f}c")
        lines.append(f"  total PnL ${sum(r['pnl'] for r in rs):,.2f}; "
                     f"{winners} of {len(rs)} symbols made money")
        ranked = sorted(rs, key=lambda r: r["pnl"])
        lines.append("  worst: " + ", ".join(f"{r['symbol']} ${r['pnl']:,.0f}" for r in ranked[:5]))
        lines.append("  best:  " + ", ".join(f"{r['symbol']} ${r['pnl']:,.0f}" for r in ranked[::-1][:5]))
    failed = [r for r in records if "error" in r]
    if failed:
        lines.append(f"{len(failed)} failed: " + ", ".join(f"{r['symbol']} ({r['error']})" for r in failed[:5]))
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("data_dir", help="folder of <SYMBOL>.itch files from itch_split")
    parser.add_argument("out", type=Path, help="JSON Lines results file (appended to)")
    parser.add_argument("--symbols", help="comma-separated; default: every .itch file in data_dir")
    parser.add_argument("--strategies", default="join")
    parser.add_argument("--latencies", default="10", help="comma-separated microseconds")
    parser.add_argument("--workers", type=int, default=os.cpu_count())
    args = parser.parse_args()

    symbols = args.symbols.split(",") if args.symbols else sorted(
        p.stem for p in Path(args.data_dir).glob("*.itch"))
    jobs = plan(symbols, args.strategies.split(","), [int(x) for x in args.latencies.split(",")])
    started = time.perf_counter()
    new = run_all(jobs, functools.partial(run_job, args.data_dir), args.out, args.workers)
    print(f"{len(new)} jobs run ({len(jobs) - len(new)} already done) in "
          f"{time.perf_counter() - started:.1f} s with {args.workers} workers\n")

    every = [json.loads(line) for line in args.out.read_text().splitlines()]
    latest = {(r["symbol"], r["strategy"], r["latency_us"]): r for r in every}  # last attempt wins
    print(summarize(list(latest.values())))
    return 0 if all("error" not in r for r in latest.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
