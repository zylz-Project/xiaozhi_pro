#!/usr/bin/env python3
"""Run the host unittest suite with bounded process-level parallelism."""

import argparse
from collections import Counter
from concurrent.futures import ProcessPoolExecutor, as_completed
from contextlib import redirect_stderr, redirect_stdout
import io
import multiprocessing
import os
from pathlib import Path
import sys
import time
import traceback
import unittest


ROOT = Path(__file__).resolve().parents[1]
TEST_DIR = ROOT / "scripts/tests"


def run_suite(suite: unittest.TestSuite, verbosity: int) -> tuple[Counter[str], bool, str]:
    output = io.StringIO()
    with redirect_stdout(output), redirect_stderr(output):
        result = unittest.TextTestRunner(stream=output, verbosity=verbosity).run(suite)
    counts = Counter(tests=result.testsRun, failures=len(result.failures), errors=len(result.errors),
                     skipped=len(result.skipped), expected_failures=len(result.expectedFailures),
                     unexpected_successes=len(result.unexpectedSuccesses))
    return counts, result.wasSuccessful(), output.getvalue()


def run_parallel(suites: list[unittest.TestSuite], jobs: int, verbose: bool) -> int:
    start = time.monotonic()
    totals: Counter[str] = Counter()
    print(f"Running {sum(suite.countTestCases() for suite in suites)} host tests "
          f"with {jobs} workers", flush=True)

    # Keep each discovered module together so unittest still shares its fixtures.
    # Queue large suites early to overlap them with independent tests.
    suites.sort(key=lambda suite: suite.countTestCases(), reverse=True)
    context = multiprocessing.get_context("spawn")
    with ProcessPoolExecutor(max_workers=jobs, mp_context=context) as executor:
        pending = [executor.submit(run_suite, suite, 2 if verbose else 1) for suite in suites]
        try:
            for finished, future in enumerate(as_completed(pending), start=1):
                try:
                    counts, successful, output = future.result()
                except Exception:
                    totals["errors"] += 1
                    print("Host test worker failed", file=sys.stderr)
                    traceback.print_exc()
                    continue
                totals.update(counts)
                if verbose or not successful:
                    print(output, end="", flush=True)
                elif finished % 10 == 0:
                    print(f"Finished {finished}/{len(suites)} test suites", flush=True)
        except KeyboardInterrupt:
            executor.shutdown(cancel_futures=True)
            return 130

    print(f"Ran {totals['tests']} tests in {time.monotonic() - start:.2f}s")
    details = ", ".join(f"{key}={totals[key]}" for key in (
        "failures", "errors", "skipped", "expected_failures", "unexpected_successes"
    ) if totals[key])
    successful = not any(totals[key] for key in ("failures", "errors", "unexpected_successes"))
    status = "OK" if successful else "FAILED"
    print(f"{status} ({details})" if details else status)
    return int(not successful)


def main() -> int:
    available_cpus = (len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity")
                      else os.cpu_count() or 1)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-j", "--jobs", type=int, default=min(6, available_cpus),
                        help="maximum worker processes (default: %(default)s, 1 runs serially)")
    parser.add_argument("-p", "--pattern", default="test*.py",
                        help="unittest discovery file pattern (default: %(default)s)")
    parser.add_argument("-v", "--verbose", action="store_true", help="show individual test results")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")

    os.chdir(ROOT)
    loader = unittest.TestLoader()
    tests = loader.discover(str(TEST_DIR), pattern=args.pattern)
    if not tests.countTestCases():
        print(f"No host tests matched {args.pattern!r}", file=sys.stderr)
        return 1
    suites = [suite for suite in tests if suite.countTestCases()]
    jobs = min(args.jobs, len(suites))
    if jobs == 1 or loader.errors:
        # Keep unittest's normal diagnostics for import and discovery failures.
        result = unittest.TextTestRunner(verbosity=2 if args.verbose else 1).run(tests)
        return int(not result.wasSuccessful())
    return run_parallel(suites, jobs, args.verbose)


if __name__ == "__main__":
    sys.exit(main())
