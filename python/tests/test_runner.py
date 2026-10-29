"""Tests for the parallel job runner, with a fake job in place of a backtest."""

import json
import pathlib
import tempfile
import unittest

from runner import Job, finished, plan, run_all, summarize


def fake_work(job):
    """Module level, so worker processes can import it."""
    if job.symbol == "BAD":
        raise ValueError("no such file")
    return {"symbol": job.symbol, "strategy": job.strategy, "latency_us": job.latency_us,
            "shares": 100, "spread_captured": 1.0, "markout_1s": -2.0, "markout_60s": -3.0,
            "pnl": -1.0 if job.symbol != "WIN" else 5.0}


class RunnerTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.out = pathlib.Path(directory.name) / "results.jsonl"

    def lines(self):
        return [json.loads(line) for line in self.out.read_text().splitlines()]

    def test_plan_is_every_combination(self):
        jobs = plan(["AAPL", "MSFT"], ["join"], [10, 1000])
        self.assertEqual([j.key() for j in jobs], [
            ("AAPL", "join", 10), ("AAPL", "join", 1000),
            ("MSFT", "join", 10), ("MSFT", "join", 1000),
        ])

    def test_records_every_job(self):
        run_all(plan(["AAPL", "MSFT"], ["join"], [10]), fake_work, self.out, workers=1)
        self.assertEqual(sorted(r["symbol"] for r in self.lines()), ["AAPL", "MSFT"])

    def test_rerun_skips_finished_jobs(self):
        jobs = plan(["AAPL", "MSFT"], ["join"], [10])
        run_all(jobs, fake_work, self.out, workers=1)
        again = run_all(jobs + [Job("TSLA", "join", 10)], fake_work, self.out, workers=1)

        self.assertEqual([r["symbol"] for r in again], ["TSLA"])
        self.assertEqual(len(self.lines()), 3)

    def test_failed_job_is_recorded_and_retried_next_time(self):
        jobs = plan(["AAPL", "BAD"], ["join"], [10])
        first = run_all(jobs, fake_work, self.out, workers=1)

        self.assertEqual([r.get("error") for r in first], [None, "ValueError: no such file"])
        self.assertEqual(finished(self.out), {("AAPL", "join", 10)})
        second = run_all(jobs, fake_work, self.out, workers=1)
        self.assertEqual([r["symbol"] for r in second], ["BAD"])  # retried, not skipped

    def test_runs_in_worker_processes(self):
        jobs = plan(["AAPL", "MSFT", "TSLA", "BAD"], ["join"], [10])
        records = run_all(jobs, fake_work, self.out, workers=2)

        self.assertEqual(len(records), 4)
        self.assertEqual(sum("error" in r for r in records), 1)
        self.assertEqual(finished(self.out), {(s, "join", 10) for s in ("AAPL", "MSFT", "TSLA")})

    def test_summary_weights_by_shares(self):
        records = [fake_work(Job(s, "join", 10)) for s in ("AAPL", "WIN")]
        text = summarize(records)

        self.assertIn("join, 10 us: 2 symbols, 200 shares filled", text)
        self.assertIn("spread captured +1.000c, markout 1 s -2.000c", text)
        self.assertIn("PnL +2.000c", text)  # ($5 - $1) / 200 shares
        self.assertIn("1 of 2 symbols made money", text)


if __name__ == "__main__":
    unittest.main()
