#!/usr/bin/env python3
"""Compare perftest-vk CSV files (GPUs, compilers, configs) as a Markdown table, optionally a PNG chart.

    python scripts/compare.py dxc.csv slang.csv
    python scripts/compare.py "3070 DXC=nv_dxc.csv" "3070 Slang=nv_slang.csv" --filter "working set" --markdown out.md
    python scripts/compare.py a.csv b.csv --metric median --plot chart.png      (needs matplotlib)

Each column shows the time; columns after the first also show the speed relative to the first file
(>1.00x = faster than the first). Labels default to "device / compiler / config" from the CSV, or to the file name.
"""
import argparse
import csv
import os
import sys

def load(path):
    rows, meta = {}, {}
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            name = r.get("test", "")
            if not name:
                continue
            rows[name] = r
            if not meta and r.get("device"):
                meta = {k: r.get(k, "") for k in ("device", "driver", "compiler", "config")}
    return rows, meta


def value(row, metric):
    if row is None or not row.get("status", "").startswith("ok"):
        return None
    key = {"total": "total_ms", "median": "median_ms", "min": "min_ms"}[metric]
    v = row.get(key) or (row.get("total_ms") if metric == "total" else None)
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def short_device(name):
    for junk in ("NVIDIA GeForce ", "AMD Radeon(TM) ", "AMD ", "Intel(R) ", " Laptop GPU", " Graphics"):
        name = name.replace(junk, "")
    return name.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help="CSV files, optionally as LABEL=path")
    ap.add_argument("--metric", choices=["total", "median", "min"], default="total",
                    help="total = sum over the benchmark frames (default, the original's metric); median/min = per dispatch")
    ap.add_argument("--filter", default="", help="only tests whose name contains this text")
    ap.add_argument("--suite", default="", help="only this suite (original, bindless)")
    ap.add_argument("--markdown", help="write the table to this file instead of stdout")
    ap.add_argument("--plot", help="also write a horizontal bar chart (PNG) of the relative speed")
    args = ap.parse_args()

    runs = []
    for spec in args.files:
        label, _, path = spec.rpartition("=") if "=" in spec and not os.path.exists(spec) else ("", "", spec)
        rows, meta = load(path)

        if not label:
            label = " / ".join(x for x in (short_device(meta.get("device", "")), meta.get("compiler", "").split(" ")[0],
                                           meta.get("config", "") if meta.get("config") not in ("", "default") else "") if x)
            label = label or os.path.splitext(os.path.basename(path))[0]

        runs.append((label, rows))

    names = []

    for _, rows in runs:
        for n, r in rows.items():
            if n in names:
                continue
            if args.filter and args.filter not in n:
                continue
            suite = r.get("suite") or "original"
            if args.suite and suite != args.suite:
                continue
            names.append(n)

    if not names:
        print("no tests match", file=sys.stderr)
        return 1

    unit = "ms (sum over frames)" if args.metric == "total" else f"ms ({args.metric} per dispatch)"
    lines = [f"Metric: {unit}. Relative speed vs the first column: >1.00x = faster.", ""]
    header = "| Test | " + " | ".join(label for label, _ in runs) + " |"
    lines += [header, "|---|" + "---|" * len(runs)]
    chart = []

    for n in names:
        base = value(runs[0][1].get(n), args.metric)
        cells = []

        for i, (_, rows) in enumerate(runs):
            v = value(rows.get(n), args.metric)
            if v is None:
                cells.append("-")
            elif i == 0 or not base:
                cells.append(f"{v:.3f}")
            else:
                cells.append(f"{v:.3f} ({base / v:.2f}x)")
                chart.append((n, i, base / v))

        cell_name = n.replace("|", "\\|")      # Markdown table cells cannot contain a bare pipe
        lines.append(f"| {cell_name} | " + " | ".join(cells) + " |")
    text = "\n".join(lines) + "\n"

    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"wrote {args.markdown} ({len(names)} tests)")
    else:
        sys.stdout.write(text)

    if args.plot:
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
        except ImportError:
            print("--plot needs matplotlib (pip install matplotlib)", file=sys.stderr)
            return 1

        if len(runs) < 2:
            print("--plot needs at least two files", file=sys.stderr)
            return 1

        fig_h = max(3.0, 0.28 * len(names) * (len(runs) - 1) + 1.0)
        fig, ax = plt.subplots(figsize=(10, fig_h))
        bar_h = 0.8 / (len(runs) - 1)

        for i in range(1, len(runs)):
            ys, xs = [], []
            for row, n in enumerate(names):
                r = next((c for c in chart if c[0] == n and c[1] == i), None)
                if r:
                    ys.append(row + (i - 1) * bar_h)
                    xs.append(r[2])
            ax.barh(ys, xs, height=bar_h, label=f"{runs[i][0]} vs {runs[0][0]}")
        ax.axvline(1.0, color="black", linewidth=0.8)

        ax.set_yticks([row + 0.4 - bar_h / 2 for row in range(len(names))])
        ax.set_yticklabels(names, fontsize=7)
        ax.invert_yaxis()
        ax.set_xlabel("relative speed (>1 = faster than the first file)")
        ax.legend(fontsize=8)

        fig.tight_layout()
        fig.savefig(args.plot, dpi=120)
        
        print(f"wrote {args.plot}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
