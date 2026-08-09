#!/usr/bin/env python3
# MIT License
#
# Copyright (c) 2026 Michael Ledour
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions :
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.

"""Compare two microbench captures and fail on regressions.

Reads two text files containing lines of the form

    MICROBENCH:<key>:<float-ns-per-call>

(typically the stdout of openxr-api-layer-tests.exe filtered to the
microbench TEST_CASEs), computes per-key ratios pr/main, and exits non-zero
if any ratio exceeds the threshold.

The whole point of this script is to make perf gates *relative* on
GitHub-hosted runners, where absolute thresholds would flake against
shared-Azure CPU variance. As long as both captures come from the same
runner job (same VM, same minutes), a 10 % threshold reliably catches
real regressions while ignoring runner-to-runner drift.

Usage:
    compare_bench.py main.txt pr.txt [--threshold 0.10] [--summary out.md]

Exit codes:
    0  No regression (every ratio <= 1 + threshold).
    1  At least one regression detected.
    2  Input parsing or other tool error.
"""

import argparse
import re
import sys
from pathlib import Path
from typing import Dict


METRIC_RE = re.compile(r"^MICROBENCH:([A-Za-z0-9_]+):([+-]?\d+(?:\.\d+)?)\s*$")


def parse_capture(path: Path) -> Dict[str, float]:
    """Extract MICROBENCH:<key>:<value> lines from a capture file.

    Lines that don't match the sentinel pattern are ignored — the bench
    binary's stdout includes doctest progress output mixed in with the
    metric lines, and we don't want to depend on doctest's exact format.
    Duplicate keys take the LAST value (TEST_CASEs are not expected to
    emit twice, but if they do, the last write wins).
    """
    metrics: Dict[str, float] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = METRIC_RE.match(line)
        if not m:
            continue
        metrics[m.group(1)] = float(m.group(2))
    return metrics


def format_summary(
    main_metrics: Dict[str, float],
    pr_metrics: Dict[str, float],
    threshold: float,
    regressions: list,
) -> str:
    """Return a markdown summary suitable for posting as a PR comment."""
    lines = []
    lines.append("## Microbench: PR vs main")
    lines.append("")
    lines.append(f"Threshold: ratio (pr/main) ≤ {1 + threshold:.2f}.")
    lines.append("")
    lines.append("| Metric | main (ns) | PR (ns) | Δ | Verdict |")
    lines.append("|---|---:|---:|---:|---|")

    all_keys = sorted(set(main_metrics) | set(pr_metrics))
    for key in all_keys:
        main_v = main_metrics.get(key)
        pr_v = pr_metrics.get(key)
        if main_v is None:
            lines.append(f"| `{key}` | — | {pr_v:.1f} | new | ⚠️ no baseline |")
            continue
        if pr_v is None:
            lines.append(f"| `{key}` | {main_v:.1f} | — | removed | ⚠️ |")
            continue
        ratio = pr_v / main_v if main_v > 0 else float("inf")
        delta_pct = (ratio - 1.0) * 100.0
        verdict = "❌ regression" if ratio > 1 + threshold else "✅"
        lines.append(
            f"| `{key}` | {main_v:.1f} | {pr_v:.1f} | {delta_pct:+.1f} % | {verdict} |"
        )

    if regressions:
        lines.append("")
        lines.append("### Regressions")
        for key, main_v, pr_v, ratio in regressions:
            delta_pct = (ratio - 1.0) * 100.0
            lines.append(
                f"- `{key}`: {main_v:.1f} ns → {pr_v:.1f} ns "
                f"({delta_pct:+.1f} %, ratio {ratio:.3f})"
            )

    return "\n".join(lines) + "\n"


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("main", type=Path, help="Capture from main branch.")
    p.add_argument("pr", type=Path, help="Capture from PR branch.")
    p.add_argument(
        "--threshold",
        type=float,
        default=0.10,
        help="Maximum allowed ratio (pr/main) - 1. Default 0.10 (= 10 %% slower).",
    )
    p.add_argument(
        "--summary",
        type=Path,
        default=None,
        help="Optional path to write a markdown summary (for posting as a PR comment).",
    )
    args = p.parse_args()

    if not args.main.is_file():
        print(f"error: main capture not found: {args.main}", file=sys.stderr)
        return 2
    if not args.pr.is_file():
        print(f"error: pr capture not found: {args.pr}", file=sys.stderr)
        return 2

    main_metrics = parse_capture(args.main)
    pr_metrics = parse_capture(args.pr)

    if not main_metrics:
        # Bootstrap case: main predates the microbench, so the baseline
        # capture is empty. We can't compare, but we also don't want to
        # block the PR that's introducing the bench in the first place.
        # Print a warning, dump the PR numbers, and soft-pass (exit 0).
        # The next PR — once main has the bench — will get a real check.
        print(
            f"warning: no MICROBENCH:* lines in {args.main} — "
            "main predates the microbench, no comparison possible.",
            file=sys.stderr,
        )
        if pr_metrics:
            print("\nPR baseline numbers (no comparison):")
            for k in sorted(pr_metrics):
                print(f"  {k:40s}  {pr_metrics[k]:8.1f} ns")
        if args.summary is not None:
            args.summary.write_text(
                "## Microbench\n\n"
                "_main predates the microbench — no comparison possible "
                "for this PR. Future PRs will get a regression check._\n\n"
                + (
                    "| Metric | PR (ns) |\n|---|---:|\n"
                    + "\n".join(
                        f"| `{k}` | {pr_metrics[k]:.1f} |"
                        for k in sorted(pr_metrics)
                    )
                    + "\n"
                    if pr_metrics
                    else ""
                ),
                encoding="utf-8",
            )
        return 0
    if not pr_metrics:
        print(
            f"error: no MICROBENCH:* lines found in {args.pr}",
            file=sys.stderr,
        )
        return 2

    regressions = []
    print(f"{'metric':40s}  {'main (ns)':>12s}  {'pr (ns)':>12s}  {'delta':>8s}")
    print("-" * 80)
    for key in sorted(set(main_metrics) | set(pr_metrics)):
        main_v = main_metrics.get(key)
        pr_v = pr_metrics.get(key)
        if main_v is None or pr_v is None:
            print(f"{key:40s}  {main_v if main_v is not None else '   --':>12}  "
                  f"{pr_v if pr_v is not None else '   --':>12}  {'(missing)':>8s}")
            continue
        ratio = pr_v / main_v if main_v > 0 else float("inf")
        delta_pct = (ratio - 1.0) * 100.0
        marker = " ❌" if ratio > 1 + args.threshold else ""
        print(f"{key:40s}  {main_v:12.1f}  {pr_v:12.1f}  {delta_pct:+7.1f}%{marker}")
        if ratio > 1 + args.threshold:
            regressions.append((key, main_v, pr_v, ratio))

    if args.summary is not None:
        args.summary.write_text(
            format_summary(main_metrics, pr_metrics, args.threshold, regressions),
            encoding="utf-8",
        )
        print(f"\nSummary written to {args.summary}")

    if regressions:
        print(
            f"\n{len(regressions)} regression(s) above {args.threshold * 100:.0f} %.",
            file=sys.stderr,
        )
        return 1

    print(f"\nAll {len(set(main_metrics) | set(pr_metrics))} metric(s) within threshold.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
