"""Regenera os gráficos dos ensaios a partir dos CSVs publicados."""
import csv
import statistics
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"
ARTICLE = ROOT / "article"
IMAGES = ROOT / "docs" / "figures"
sys.path.insert(0, str(ROOT / "simulation"))
from energy_sim import C_MAH, STANDBY

plt.rcParams.update({"font.size": 8, "axes.spines.top": False,
                     "axes.spines.right": False, "pdf.fonttype": 42})
BLUE = "#214f82"
RED = "#a1442c"


def read_rows(name):
    with (RESULTS / name).open(encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def save(fig, name):
    fig.savefig(ARTICLE / (name + ".pdf"))
    fig.savefig(IMAGES / (name + ".png"), dpi=220)
    plt.close(fig)


def plot_deadline(times):
    fig, ax = plt.subplots(figsize=(3.45, 1.53), layout="constrained")
    xs = [0, *sorted(times), 6.15]
    ys = [0, *range(1, len(times) + 1), len(times)]
    ax.step(xs, ys, where="post", color=BLUE)
    for limit in (4, 6):
        count = sum(t <= limit for t in times)
        ax.vlines(limit, 0, count, color=RED, ls=":", lw=.8)
        ax.plot(limit, count, "o", color=RED, ms=3)
    ax.set(xlim=(0, 6.2), ylim=(0, 20.5), xticks=range(7), yticks=[0, 5, 10, 15, 19],
           xlabel="Time from firmware startup (s)", ylabel="Automatic ACKs")
    ax.grid(axis="y", alpha=.18)
    save(fig, "deadline")


def plot_profiles(times):
    quiet = [int(r["quiet_latency_ms"]) / 1000 for r in read_rows("voice_detections.csv")
             if r["condition"] == "quiet"]
    awake = read_rows("awake_summary.csv")[0]
    stats = [(min(quiet), statistics.median(quiet), max(quiet)),
             tuple(int(awake[k]) / 1000 for k in ("min_ms", "mediana_ms", "max_ms")),
             (min(times), statistics.median(times), max(times))]
    fig, (a, b) = plt.subplots(2, 1, figsize=(3.45, 2.55), layout="constrained")
    a.set_title("(a)  Measured time by processing stage", loc="left", fontsize=8.6, fontweight="bold")
    for row, (low, med, high) in enumerate(stats):
        a.plot([low, high], [row, row], lw=1.3, color=BLUE)
        a.plot(med, row, "o", ms=4, color=BLUE)
        a.annotate(f"{med:.3f}", (high, row), xytext=(4, 0), textcoords="offset points", va="center", fontsize=7)
    a.set(xlim=(-.08, 6.65), ylim=(2.55, -.6), yticks=[0, 1, 2],
          yticklabels=["Speech end to USB", "Awake to BLE ACK", "Wake to BLE ACK"],
          xlabel="Time (s), median and range")
    b.set_title("(b)  Modeled standby lifetime, 2,000 mAh", loc="left", fontsize=8.6, fontweight="bold")
    for row, current in enumerate(STANDBY.values()):
        lifetime = C_MAH / current
        b.plot([10, lifetime], [row, row], color=RED, lw=2)
        b.plot(lifetime, row, "o", ms=4, color=RED)
        b.annotate(f"{lifetime:,.0f} h", (lifetime, row), xytext=(4, 0), textcoords="offset points", va="center", fontsize=7)
    b.set(xscale="log", xlim=(10, 120000), ylim=(2.55, -.6), yticks=[0, 1, 2],
          yticklabels=["Button only", "Voice, 160 MHz", "Voice, 240 MHz"],
          xlabel="Lifetime (h, logarithmic scale)")
    for ax in (a, b):
        ax.grid(axis="x", alpha=.18)
    save(fig, "profiles")


if __name__ == "__main__":
    automatic = [int(r["ack_ms"]) / 1000 for r in read_rows("deepsleep_meas.csv")
                 if r["incluida_na_estatistica"] == "sim"]
    plot_deadline(automatic)
    plot_profiles(automatic)
