#!/usr/bin/env python3
"""Plot jitter_bench CSV output: time-series and histogram of cycle-to-cycle jitter."""

import argparse
import csv
import math
import sys
from pathlib import Path


def percentile(sorted_data: list[float], p: float) -> float:
    idx = int(p / 100.0 * (len(sorted_data) - 1))
    return sorted_data[idx]


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Plot jitter_bench CSV output",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Example:\n  python3 plot_jitter.py jitter.csv\n  python3 plot_jitter.py jitter.csv -o report.png",
    )
    parser.add_argument("csv_file", nargs="?", default="jitter.csv",
                        help="CSV file produced by jitter_bench (default: jitter.csv)")
    parser.add_argument("--output", "-o", metavar="FILE",
                        help="Save plot to file (PNG/SVG/PDF) instead of displaying interactively")
    parser.add_argument("--bins", type=int, default=200,
                        help="Histogram bin count (default: 200)")
    args = parser.parse_args()

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        sys.exit("matplotlib not found — install with: pip install matplotlib")

    csv_path = Path(args.csv_file)
    if not csv_path.exists():
        sys.exit(f"File not found: {csv_path}")

    elapsed_ms: list[float] = []
    jitter_ns: list[int] = []

    with open(csv_path) as f:
        for row in csv.DictReader(f):
            elapsed_ms.append(float(row["elapsed_ms"]))
            jitter_ns.append(int(row["jitter_ns"]))

    if not jitter_ns:
        sys.exit("CSV is empty")

    jitter_us = [v / 1000.0 for v in jitter_ns]
    sorted_us = sorted(jitter_us)
    n = len(jitter_us)

    mean_us = sum(jitter_us) / n
    stddev_us = math.sqrt(sum((v - mean_us) ** 2 for v in jitter_us) / n)
    p50 = percentile(sorted_us, 50.0)
    p95 = percentile(sorted_us, 95.0)
    p99 = percentile(sorted_us, 99.0)
    p999 = percentile(sorted_us, 99.9)

    print(f"Samples : {n:,}")
    print(f"  Min   : {sorted_us[0]:.3f} µs")
    print(f"  Max   : {sorted_us[-1]:.3f} µs")
    print(f"  Mean  : {mean_us:.3f} µs")
    print(f"  StdDev: {stddev_us:.3f} µs")
    print(f"  P50   : {p50:.3f} µs")
    print(f"  P95   : {p95:.3f} µs")
    print(f"  P99   : {p99:.3f} µs")
    print(f"  P99.9 : {p999:.3f} µs")

    elapsed_s = [t / 1000.0 for t in elapsed_ms]

    fig, (ax_ts, ax_hist) = plt.subplots(2, 1, figsize=(14, 8))
    fig.suptitle(
        f"GameLoop jitter — {csv_path.name}  |  {n:,} samples  |  "
        f"P99={p99:.2f} µs  P99.9={p999:.2f} µs",
        fontsize=11,
    )

    # Time series
    ax_ts.plot(elapsed_s, jitter_us, lw=0.3, color="steelblue", alpha=0.6)
    ax_ts.axhline(p99, color="darkorange", lw=1.2, ls="--", label=f"P99 = {p99:.2f} µs")
    ax_ts.axhline(p999, color="crimson", lw=1.2, ls="--", label=f"P99.9 = {p999:.2f} µs")
    ax_ts.set_xlabel("Elapsed (s)")
    ax_ts.set_ylabel("Jitter (µs)")
    ax_ts.set_title("Jitter over time")
    ax_ts.legend(loc="upper right")
    ax_ts.grid(True, alpha=0.25)

    # Histogram — clip x-axis at 1.5× P99.9 so rare spikes don't crush the main distribution
    clip = max(p999 * 1.5, sorted_us[-1] if n < 1000 else p999 * 2)
    clipped = [v for v in jitter_us if v <= clip]
    n_outliers = n - len(clipped)

    ax_hist.hist(clipped, bins=args.bins, color="steelblue", alpha=0.75, edgecolor="none")
    ax_hist.axvline(p99, color="darkorange", lw=1.5, ls="--", label=f"P99 = {p99:.2f} µs")
    ax_hist.axvline(p999, color="crimson", lw=1.5, ls="--", label=f"P99.9 = {p999:.2f} µs")
    title = "Jitter distribution"
    if n_outliers:
        title += f"  ({n_outliers} outlier(s) clipped from histogram)"
    ax_hist.set_title(title)
    ax_hist.set_xlabel("Jitter (µs)")
    ax_hist.set_ylabel("Count")
    ax_hist.legend(loc="upper right")
    ax_hist.grid(True, alpha=0.25, axis="y")

    plt.tight_layout()

    if args.output:
        plt.savefig(args.output, dpi=150, bbox_inches="tight")
        print(f"\nPlot saved: {args.output}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
