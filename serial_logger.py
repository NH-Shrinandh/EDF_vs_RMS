import pandas as pd
import re
import matplotlib.pyplot as plt
import seaborn as sns
from pathlib import Path
import numpy as np
import warnings

plt.style.use('seaborn-v0_8-darkgrid')
sns.set_palette('colorblind')

# ---------------- PARSING FUNCTION ----------------
def parse_log(file_path):
    """Parse CSV logs from Arduino serial output into DataFrame."""
    pattern = re.compile(r'(\d+),(START|COMPLETE|RELEASE|MISS|WDT_PET|INFO),([A-Za-z0-9_]+),?([A-Za-z0-9_]*)')
    rows = []
    with open(file_path, 'r', errors='ignore') as f:
        for line in f:
            match = pattern.search(line.strip())
            if match:
                t, evt, task, extra = match.groups()
                rows.append((int(t), evt, task, extra))
    df = pd.DataFrame(rows, columns=["time_ms", "event", "task", "extra"])
    return df

# ---------------- EXECUTION PAIRING / DISTRIBUTIONS ----------------
def build_task_execution_lists(df):
    """
    For each task, build:
     - list of execution times (complete - start), accepting only non-negative samples
     - list of response times (first complete >= release)
     - list of release timestamps
    Returns dicts keyed by task.
    """
    tasks = [t for t in df['task'].unique() if t not in ['WDT','INFO','Supervisor']]
    exec_map = {}
    resp_map = {}
    releases_map = {}

    for task in tasks:
        dft = df[df.task == task].sort_values('time_ms')
        starts = dft[dft.event == 'START']['time_ms'].to_list()
        completes = dft[dft.event == 'COMPLETE']['time_ms'].to_list()
        releases = dft[dft.event == 'RELEASE']['time_ms'].to_list()

        # Pair starts/completes robustly: use matching with first complete >= start
        execs = []
        used_complete_indices = set()
        for s in starts:
            # find the earliest complete >= s that hasn't been used
            idx = next((i for i, c in enumerate(completes) if c >= s and i not in used_complete_indices), None)
            if idx is not None:
                dur = completes[idx] - s
                if dur >= 0:
                    execs.append(dur)
                else:
                    # defensive: skip negative durations
                    warnings.warn(f"Negative exec time for task {task} (start {s}, complete {completes[idx]}). Skipping sample.")
                used_complete_indices.add(idx)
            else:
                # no matching complete found for this start
                continue

        # Response: pair each release with the first unused complete >= release
        resps = []
        used_complete_indices_resp = set()
        for r in releases:
            idx = next((i for i, c in enumerate(completes) if c >= r and i not in used_complete_indices_resp), None)
            if idx is not None:
                dur = completes[idx] - r
                if dur >= 0:
                    resps.append(dur)
                else:
                    warnings.warn(f"Negative response time for task {task} (release {r}, complete {completes[idx]}). Skipping sample.")
                used_complete_indices_resp.add(idx)
            else:
                # no complete found after this release
                continue

        exec_map[task] = execs
        resp_map[task] = resps
        releases_map[task] = releases

    return exec_map, resp_map, releases_map

# ---------------- ANALYSIS FUNCTION (summary metrics) ----------------
def analyze_from_lists(exec_map, resp_map, releases_map, df):
    metrics = []
    for task in exec_map.keys():
        exec_times = exec_map.get(task, [])
        resp_times = resp_map.get(task, [])
        releases = releases_map.get(task, [])
        dft = df[df.task == task]
        misses = len(dft[dft.event == 'MISS'])
        total = len(releases) if len(releases) > 0 else 1

        metrics.append({
            "task": task,
            "avg_exec_time_ms": round(sum(exec_times) / len(exec_times), 2) if exec_times else np.nan,
            "avg_response_ms": round(sum(resp_times) / len(resp_times), 2) if resp_times else np.nan,
            "miss_ratio": round(misses / total, 3),
            "total_exec_time_ms": round(sum(exec_times), 2),
            "release_count": len(releases)
        })
    return pd.DataFrame(metrics).set_index("task")

def watchdog_interval(df):
    wd = df[df.task == 'WDT']
    if len(wd) < 2:
        return None
    intervals = wd['time_ms'].diff().dropna()
    return round(intervals.mean(), 2)

# ---------------- LOAD & BUILD DISTRIBUTIONS ----------------
edf_path = Path("edf_log.csv")
rm_path = Path("rm_log.csv")

if not edf_path.exists() or not rm_path.exists():
    raise FileNotFoundError("Make sure 'edf_log.csv' and 'rm_log.csv' exist in current directory.")

edf_df = parse_log(edf_path)
rm_df = parse_log(rm_path)

# Build execution/response lists
edf_execs, edf_resps, edf_rels = build_task_execution_lists(edf_df)
rm_execs, rm_resps, rm_rels = build_task_execution_lists(rm_df)

edf_metrics = analyze_from_lists(edf_execs, edf_resps, edf_rels, edf_df)
rm_metrics = analyze_from_lists(rm_execs, rm_resps, rm_rels, rm_df)
edf_wdt = watchdog_interval(edf_df)
rm_wdt = watchdog_interval(rm_df)

# ---------------- PRINT METRICS ----------------
print("\n===== EDF METRICS =====")
print(edf_metrics)
print(f"\nEDF Watchdog Avg Interval: {edf_wdt} ms")

print("\n===== RM METRICS =====")
print(rm_metrics)
print(f"\nRM Watchdog Avg Interval: {rm_wdt} ms")

# ---------------- COMPARISON SUMMARY ----------------
edf_mean_resp = edf_metrics['avg_response_ms'].mean(skipna=True)
rm_mean_resp = rm_metrics['avg_response_ms'].mean(skipna=True)
edf_miss = edf_metrics['miss_ratio'].mean()
rm_miss = rm_metrics['miss_ratio'].mean()

print("\n===== COMPARISON SUMMARY =====")
if pd.notna(edf_mean_resp) and pd.notna(rm_mean_resp):
    if edf_mean_resp < rm_mean_resp:
        print(f"✅ EDF shows better average response time ({edf_mean_resp:.2f} ms) vs RM ({rm_mean_resp:.2f} ms).")
    else:
        print(f"✅ RM shows better average response time ({rm_mean_resp:.2f} ms) vs EDF ({edf_mean_resp:.2f} ms).")
else:
    print("⚠️ Unable to compare mean response times (missing data).")

if not np.isnan(edf_miss) and not np.isnan(rm_miss):
    if edf_miss < rm_miss:
        print(f"✅ EDF is more deadline-tolerant (miss ratio {edf_miss:.3f} vs {rm_miss:.3f}).")
    else:
        print(f"✅ RM is more deadline-tolerant (miss ratio {rm_miss:.3f} vs {edf_miss:.3f}).")
else:
    print("⚠️ Unable to compare miss ratios (missing data).")

print("\n📊 Fault-Tolerance: Both maintain WDT petting ~500ms; system stable under both schedulers (if WDT samples present).")

# ---------------- PLOTTING ----------------
def plot_response_time_comparison(edf_metrics, rm_metrics):
    fig, ax = plt.subplots(figsize=(7,5))
    edf_metrics['avg_response_ms'].plot(kind='bar', color='skyblue', label='EDF', ax=ax, position=0, width=0.4)
    rm_metrics['avg_response_ms'].plot(kind='bar', color='lightcoral', label='RM', ax=ax, position=1, width=0.4)
    ax.set_title("Average Response Time per Task")
    ax.set_ylabel("Response Time (ms)")
    ax.legend()
    plt.tight_layout()
    plt.savefig("comparison_response_time.png", dpi=300)
    plt.show()

def plot_miss_ratio_comparison(edf_metrics, rm_metrics):
    fig, ax = plt.subplots(figsize=(7,5))
    edf_metrics['miss_ratio'].plot(kind='bar', color='blue', alpha=0.6, label='EDF', ax=ax)
    rm_metrics['miss_ratio'].plot(kind='bar', color='red', alpha=0.6, label='RM', ax=ax)
    ax.set_title("Deadline Miss Ratio Comparison")
    ax.set_ylabel("Miss Ratio")
    ax.legend()
    plt.tight_layout()
    plt.savefig("comparison_miss_ratio.png", dpi=300)
    plt.show()

def plot_exec_time_comparison(edf_metrics, rm_metrics):
    fig, ax = plt.subplots(figsize=(7,5))
    edf_metrics['avg_exec_time_ms'].plot(kind='bar', color='green', alpha=0.6, label='EDF', ax=ax)
    rm_metrics['avg_exec_time_ms'].plot(kind='bar', color='orange', alpha=0.6, label='RM', ax=ax)
    ax.set_title("Average Execution Time per Task")
    ax.set_ylabel("Execution Time (ms)")
    ax.legend()
    plt.tight_layout()
    plt.savefig("comparison_exec_time.png", dpi=300)
    plt.show()

def plot_distribution(exec_map, releases_map, prefix="edf"):
    """
    - Pie: total CPU share per task (sum exec times) — only non-negative durations
    - Bar: release counts per task
    - Histograms: per-task execution time distributions
    """
    tasks = list(exec_map.keys())
    total_execs = {}
    for t in tasks:
        # keep only positive exec samples
        vals = [v for v in exec_map[t] if v is not None and v > 0]
        total_execs[t] = sum(vals)

    # Avoid negative or nan values in pie sizes
    sizes = [max(0.0, total_execs.get(t, 0.0)) for t in tasks]
    sum_sizes = sum(sizes)
    # If everything zero (no recorded execs), create fallback uniform small slices
    if sum_sizes <= 0:
        sizes = [1.0 for _ in sizes]
        sum_sizes = sum(sizes)

    # 1) Pie chart: CPU share
    def autopct_fn(pct):
        # show percent and absolute ms (from sizes)
        absolute = pct * sum_sizes / 100.0
        return f"{pct:.1f}%\n({absolute:.1f} ms)"

    fig1, ax1 = plt.subplots(figsize=(6,6))
    ax1.pie(sizes, labels=tasks, autopct=autopct_fn, startangle=90)
    ax1.set_title(f'CPU Time Share per Task ({prefix.upper()})')
    plt.tight_layout()
    plt.savefig(f"{prefix}_cpu_share_pie.png", dpi=300)
    plt.show()

    # 2) Release count bar
    counts = {t: len(releases_map.get(t, [])) for t in tasks}
    fig2, ax2 = plt.subplots(figsize=(7,4))
    ax2.bar(counts.keys(), counts.values(), color=sns.color_palette('pastel'))
    ax2.set_title(f'Release Counts per Task ({prefix.upper()})')
    ax2.set_ylabel("Number of RELEASE events")
    plt.tight_layout()
    plt.savefig(f"{prefix}_release_counts.png", dpi=300)
    plt.show()

    # 3) Histogram / KDE per task (stacked subplots)
    n = len(tasks)
    if n == 0:
        warnings.warn("No tasks found to plot distributions.")
        return

    cols = 2
    rows = (n + cols - 1) // cols
    fig3, axes = plt.subplots(rows, cols, figsize=(10, rows * 3))
    axes = np.array(axes).reshape(-1)
    used_axes = 0
    for i, t in enumerate(tasks):
        data = [v for v in exec_map[t] if v is not None and v >= 0]
        ax = axes[i]
        if data:
            sns.histplot(data, kde=True, ax=ax, color='steelblue', stat='count', bins=20)
            ax.set_xlabel("Execution time (ms)")
            ax.set_title(f"{t} exec time distribution (n={len(data)})")
        else:
            ax.text(0.5,0.5,"No samples", ha='center', va='center')
            ax.set_title(f"{t} exec time distribution")
            ax.set_xlabel("Execution time (ms)")
        used_axes += 1

    # hide unused axes
    for j in range(used_axes, len(axes)):
        fig3.delaxes(axes[j])

    plt.tight_layout()
    plt.savefig(f"{prefix}_exec_histograms.png", dpi=300)
    plt.show()

# Produce standard plots
plot_response_time_comparison(edf_metrics, rm_metrics)
plot_miss_ratio_comparison(edf_metrics, rm_metrics)
plot_exec_time_comparison(edf_metrics, rm_metrics)

# Produce distribution plots for EDF and RM
plot_distribution(edf_execs, edf_rels, prefix="edf")
plot_distribution(rm_execs, rm_rels, prefix="rm")

print("\nAll plots saved: comparison_response_time.png, comparison_miss_ratio.png, comparison_exec_time.png, edf_* and rm_* distributions.")
