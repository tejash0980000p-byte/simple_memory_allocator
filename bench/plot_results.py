"""Draw comparison charts from results.csv (created by `make bench`).
Needs: pip install matplotlib   ->  python3 bench/plot_results.py
"""
import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

rows = list(csv.DictReader(open("results.csv")))
workloads = list(dict.fromkeys(r["workload"] for r in rows))
allocs = list(dict.fromkeys(r["allocator"] for r in rows))
colors = {"first-fit": "#3b82f6", "next-fit": "#f59e0b", "best-fit": "#10b981",
          "worst-fit": "#ef4444", "libc malloc": "#6b7280"}

def get(w, a, col):
    return next(float(r[col]) for r in rows if r["workload"] == w and r["allocator"] == a)

fig, ax = plt.subplots(1, 2, figsize=(13, 4.8))
width = 0.16
for i, a in enumerate(allocs):
    xs = [j + (i - 2) * width for j in range(len(workloads))]
    ax[0].bar(xs, [get(w, a, "time_ms") for w in workloads], width, label=a, color=colors.get(a))
    if a != "libc malloc":
        ax[1].bar(xs, [get(w, a, "ext_frag_pct") for w in workloads], width, label=a, color=colors.get(a))
for k, (title, yl) in enumerate([("Speed (lower is better)", "time (ms, log scale)"),
                                 ("External fragmentation at end (lower is better)", "fragmentation %")]):
    ax[k].set_xticks(range(len(workloads)))
    ax[k].set_xticklabels(workloads)
    ax[k].set_title(title)
    ax[k].set_ylabel(yl)
    ax[k].legend()
ax[0].set_yscale("log")
plt.tight_layout()
plt.savefig("comparison.png", dpi=150)
print("Saved comparison.png")
