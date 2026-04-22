#!/usr/bin/env python3
"""Benchmark profiler against perf under the same workload."""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import TextIO


TIME_FMT = "elapsed_sec=%e\nuser_sec=%U\nsys_sec=%S\ncpu_pct=%P\nmax_rss_kb=%M"
GRACE_SEC = 1.0
LIST_FILES = {
    "prof_cpu": ".profiler_cpu_total.list",
    "prof_samples": ".profiler_samples.list",
    "perf_cpu": ".perf_collect_cpu_total.list",
    "perf_e2e_cpu": ".perf_e2e_cpu_total.list",
    "perf_samples": ".perf_samples.list",
}


def positive_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"必须是整数: {value}") from exc
    if parsed < 1:
        raise argparse.ArgumentTypeError(f"必须 >= 1: {value}")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "同一负载下自动输出 collect-only 和 end-to-end 两组开销对比："
            "profiler vs perf record，以及 profiler vs perf record + perf script/report。"
        ),
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--profiler-bin", default=Path("./build/Release/profiler"), type=Path, help="profiler 可执行文件路径")
    parser.add_argument("--freq", default=99, type=positive_int, metavar="hz", help="采样频率")
    parser.add_argument("--duration", default=15, type=positive_int, metavar="sec", help="每次采样时长，单位秒")
    parser.add_argument("--runs", default=1, type=positive_int, metavar="N", help="重复运行轮数")
    parser.add_argument("--workload", default="sha1sum /dev/zero", metavar="cmd", help="被测负载命令")
    parser.add_argument("--workdir", default=Path.cwd(), type=Path, metavar="dir", help="负载运行目录")
    parser.add_argument("--outdir", default=Path("./report"), type=Path, metavar="dir", help="输出根目录；每次测试会在该目录下创建时间戳子目录")
    parser.add_argument("--sw-event", action="store_true", help="profiler 使用软件事件")
    parser.add_argument("--profiler-symbolize", action="store_true", help="启用符号化输出；默认关闭，用于低开销基准")
    parser.add_argument(
        "--profiler-filter",
        choices=("tgid", "pgrp", "session", "cgroup"),
        default="session",
        help="profiler 的 --pid 过滤范围；session/pgrp 可覆盖 stress-ng 这类子进程负载",
    )
    return parser.parse_args()


def require_cmd(name: str) -> None:
    if shutil.which(name) is None:
        raise SystemExit(f"缺少命令: {name}")


def validate(args: argparse.Namespace) -> None:
    if os.geteuid() != 0:
        raise SystemExit(f"请使用 root 运行，例如: sudo {sys.argv[0]} ...")
    for cmd in ("timeout", "perf", "/usr/bin/time", "bash"):
        require_cmd(cmd)
    if not args.profiler_bin.is_file() or not os.access(args.profiler_bin, os.X_OK):
        raise SystemExit(f"profiler 不存在或不可执行: {args.profiler_bin}")
    if not args.workdir.is_dir():
        raise SystemExit(f"负载运行目录不存在: {args.workdir}")


def unique_run_dir(root: Path, tag: str) -> Path:
    root = root.resolve()
    path = root / tag
    suffix = 2
    while path.exists():
        path = root / f"{tag}_{suffix:02d}"
        suffix += 1
    path.mkdir(parents=True)
    return path


def write_config(args: argparse.Namespace, tag: str) -> None:
    data = {
        "timestamp": tag,
        "profiler_bin": str(args.profiler_bin),
        "freq": args.freq,
        "duration": args.duration,
        "runs": args.runs,
        "workload": args.workload,
        "workdir": str(args.workdir),
        "outdir": str(args.outdir),
        "sw_event": args.sw_event,
        "profiler_symbolize": args.profiler_symbolize,
        "profiler_filter": args.profiler_filter,
    }
    (args.outdir / "benchmark_config.json").write_text(
        json.dumps(data, ensure_ascii=False, indent=2) + "\n"
    )


def kill_group(proc: subprocess.Popen[str], first: int = signal.SIGINT) -> None:
    if proc.poll() is not None:
        return
    for sig in (first, signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(proc.pid, sig)
        except ProcessLookupError:
            return
        try:
            proc.wait(timeout=GRACE_SEC)
            return
        except subprocess.TimeoutExpired:
            continue
    proc.wait()


def run_cmd(
    cmd: list[str],
    *,
    stdout: TextIO | int | None = None,
    stderr: TextIO | int | None = None,
) -> subprocess.CompletedProcess[str]:
    proc = subprocess.Popen(
        cmd,
        stdout=stdout,
        stderr=stderr,
        text=True,
        start_new_session=True,
    )
    try:
        code = proc.wait()
    except KeyboardInterrupt:
        kill_group(proc)
        raise
    return subprocess.CompletedProcess(cmd, code)


def timed(
    time_file: Path,
    cmd: list[str],
    *,
    stdout: TextIO | int | None = None,
    stderr: TextIO | int | None = None,
) -> subprocess.CompletedProcess[str]:
    return run_cmd(
        ["/usr/bin/time", "-f", TIME_FMT, "-o", str(time_file), *cmd],
        stdout=stdout,
        stderr=stderr,
    )


def start_workload(args: argparse.Namespace, log_file: Path) -> subprocess.Popen[str]:
    log = log_file.open("w")
    try:
        proc = subprocess.Popen(
            ["bash", "-lc", f"exec {args.workload}"],
            cwd=args.workdir,
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )
    except Exception:
        log.close()
        raise
    log.close()

    try:
        time.sleep(0.3)
        if proc.poll() is not None:
            raise SystemExit(f"负载未正常启动，请检查命令: {args.workload}")
        return proc
    except KeyboardInterrupt:
        kill_group(proc)
        raise


def stop_workload(proc: subprocess.Popen[str]) -> None:
    kill_group(proc, signal.SIGTERM)


def time_metric(path: Path, key: str) -> str:
    if not path.exists():
        return "0"
    for line in path.read_text(errors="replace").splitlines():
        name, sep, value = line.partition("=")
        if sep and name == key:
            return value
    return "0"


def as_float(path: Path, key: str) -> float:
    try:
        return float(time_metric(path, key))
    except ValueError:
        return 0.0


def read_int(path: Path) -> int:
    try:
        return int(path.read_text().strip() or "0")
    except (FileNotFoundError, ValueError):
        return 0


def folded_samples(path: Path) -> int:
    total = 0
    for line in path.read_text(errors="replace").splitlines() if path.exists() else []:
        if line.startswith("samples="):
            continue
        match = re.search(r"\s([0-9]+)$", line)
        if match:
            total += int(match.group(1))
    return total


def profiler_samples(path: Path, symbolized: bool) -> int:
    text = path.read_text(errors="replace") if path.exists() else ""
    match = re.findall(r"^samples=(\d+)$", text, re.MULTILINE)
    if match:
        return int(match[-1])
    if symbolized:
        return folded_samples(path)
    return sum(1 for line in text.splitlines() if line.strip())


def perf_samples(report_file: Path, script_file: Path) -> int:
    if report_file.exists():
        match = re.search(
            r"[Ss]amples:\s*([0-9][0-9,]*)",
            report_file.read_text(errors="replace"),
        )
        if match:
            return int(match.group(1).replace(",", ""))
    if not script_file.exists():
        return 0
    return sum(
        1
        for line in script_file.read_text(errors="replace").splitlines()
        if line and not line[0].isspace() and not line.startswith("#")
    )


def proc_stat_ids(pid: int) -> tuple[int, int] | None:
    try:
        text = Path(f"/proc/{pid}/stat").read_text(errors="replace")
    except FileNotFoundError:
        return None
    end = text.rfind(")")
    if end < 0:
        return None
    fields = text[end + 2 :].split()
    return (int(fields[2]), int(fields[3])) if len(fields) >= 4 else None


def proc_cgroup_path(pid: int) -> str | None:
    try:
        lines = Path(f"/proc/{pid}/cgroup").read_text(errors="replace").splitlines()
    except FileNotFoundError:
        return None
    for line in lines:
        if "::" in line:
            return line.partition("::")[2]
    return None


def target_pids(pid: int, filter_mode: str) -> list[int]:
    if filter_mode == "tgid":
        return [pid]

    if filter_mode in {"pgrp", "session"}:
        ids = proc_stat_ids(pid)
        if ids is None:
            return [pid]
        wanted = ids[0 if filter_mode == "pgrp" else 1]
        index = 0 if filter_mode == "pgrp" else 1
        pids = [
            int(proc.name)
            for proc in Path("/proc").iterdir()
            if proc.name.isdigit()
            and (current := proc_stat_ids(int(proc.name))) is not None
            and current[index] == wanted
        ]
        return sorted(pids) or [pid]

    wanted_cgroup = proc_cgroup_path(pid)
    if wanted_cgroup is None:
        return [pid]
    pids = [
        int(proc.name)
        for proc in Path("/proc").iterdir()
        if proc.name.isdigit() and proc_cgroup_path(int(proc.name)) == wanted_cgroup
    ]
    return sorted(pids) or [pid]


def run_profiler(args: argparse.Namespace, tag: str, pid: int) -> None:
    out_file = args.outdir / f"profiler_{tag}.out"
    cmd = [
        "timeout",
        "--signal=INT",
        f"{args.duration}s",
        str(args.profiler_bin),
        "-p",
        str(pid),
        "--filter",
        args.profiler_filter,
        "-f",
        str(args.freq),
        "-E",
    ]
    if args.sw_event:
        cmd.append("--sw-event")
    if not args.profiler_symbolize:
        cmd.append("--no-symbolize")

    with out_file.open("w") as out:
        timed(args.outdir / f"profiler_{tag}.time", cmd, stdout=out, stderr=subprocess.STDOUT)
    (args.outdir / f"profiler_{tag}.samples").write_text(
        f"{profiler_samples(out_file, args.profiler_symbolize)}\n"
    )


def run_perf(args: argparse.Namespace, tag: str, pid: int) -> None:
    data_file = args.outdir / f"perf_{tag}.data"
    script_file = args.outdir / f"perf_{tag}.script"
    report_file = args.outdir / f"perf_{tag}.report"
    pids = ",".join(str(value) for value in target_pids(pid, args.profiler_filter))

    timed(
        args.outdir / f"perf_{tag}.time",
        [
            "timeout",
            "--signal=INT",
            f"{args.duration}s",
            "perf",
            "record",
            "-F",
            str(args.freq),
            "-g",
            "-p",
            pids,
            "-o",
            str(data_file),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    timed(
        args.outdir / f"perf_post_{tag}.time",
        [sys.executable, __file__, "--perf-post", str(data_file), str(script_file), str(report_file)],
    )
    (args.outdir / f"perf_{tag}.samples").write_text(
        f"{perf_samples(report_file, script_file)}\n"
    )


def perf_post_main(argv: list[str]) -> None:
    data_file, script_file, report_file = map(Path, argv)
    with script_file.open("w") as out:
        run_cmd(["perf", "script", "-i", str(data_file)], stdout=out, stderr=subprocess.DEVNULL)
    with report_file.open("w") as out:
        run_cmd(
            ["perf", "report", "--stdio", "--stats", "-i", str(data_file)],
            stdout=out,
            stderr=subprocess.DEVNULL,
        )


def symbolize_note() -> None:
    print(
        "注意: 已开启 --profiler-symbolize，profiler 此项包含采集、符号化和文本输出；"
        "不应视为与 perf record 的纯采集开销对比。"
    )


def cpu_total(outdir: Path, *names: str) -> float:
    return sum(
        as_float(outdir / name, "user_sec") + as_float(outdir / name, "sys_sec")
        for name in names
    )


def sample_count(outdir: Path, tool: str, tag: str) -> int:
    return read_int(outdir / f"{tool}_{tag}.samples")


def print_round(args: argparse.Namespace, tag: str) -> None:
    outdir = args.outdir
    rows = [
        ("profiler", f"profiler_{tag}.time", sample_count(outdir, "profiler", tag)),
        ("perf", f"perf_{tag}.time", sample_count(outdir, "perf", tag)),
    ]

    print(f"\n=== 采样开销对比 ({tag}) ===")
    if args.profiler_symbolize:
        symbolize_note()
    print(f"{'tool':<12} {'user(s)':<10} {'sys(s)':<10} {'elapsed':<10} {'cpu':<8} {'samples':<12}")
    for tool, time_name, samples in rows:
        time_file = outdir / time_name
        print(
            f"{tool:<12} {time_metric(time_file, 'user_sec'):<10} "
            f"{time_metric(time_file, 'sys_sec'):<10} {time_metric(time_file, 'elapsed_sec'):<10} "
            f"{time_metric(time_file, 'cpu_pct'):<8} {samples:<12}"
        )
    print(f"{'max_rss(kb)':<12} {'profiler':<10} {time_metric(outdir / f'profiler_{tag}.time', 'max_rss_kb'):<10}")
    print(f"{'':<12} {'perf':<10} {time_metric(outdir / f'perf_{tag}.time', 'max_rss_kb'):<10}")

    perf_elapsed = as_float(outdir / f"perf_{tag}.time", "elapsed_sec") + as_float(
        outdir / f"perf_post_{tag}.time", "elapsed_sec"
    )
    print(f"\n=== 端到端开销对比 ({tag}) ===")
    print(f"{'tool':<12} {'cpu_total(s)':<14} {'elapsed(s)':<14} {'samples':<12}")
    print(
        f"{'profiler':<12} {cpu_total(outdir, f'profiler_{tag}.time'):<14.6f} "
        f"{time_metric(outdir / f'profiler_{tag}.time', 'elapsed_sec'):<14} "
        f"{sample_count(outdir, 'profiler', tag):<12}"
    )
    print(
        f"{'perf':<12} {cpu_total(outdir, f'perf_{tag}.time', f'perf_post_{tag}.time'):<14.6f} "
        f"{perf_elapsed:<14.2f} {sample_count(outdir, 'perf', tag):<12}"
    )


def append_stats(args: argparse.Namespace, tag: str) -> None:
    values = {
        "prof_cpu": cpu_total(args.outdir, f"profiler_{tag}.time"),
        "prof_samples": sample_count(args.outdir, "profiler", tag),
        "perf_cpu": cpu_total(args.outdir, f"perf_{tag}.time"),
        "perf_e2e_cpu": cpu_total(args.outdir, f"perf_{tag}.time", f"perf_post_{tag}.time"),
        "perf_samples": sample_count(args.outdir, "perf", tag),
    }
    for key, value in values.items():
        with (args.outdir / LIST_FILES[key]).open("a") as out:
            out.write(f"{value:.6f}\n")


def stats(path: Path) -> tuple[float, float, float]:
    values = [float(line) for line in path.read_text().splitlines() if line.strip()]
    if not values:
        return 0.0, 0.0, 0.0
    mean = sum(values) / len(values)
    variance = sum(value * value for value in values) / len(values) - mean * mean
    p95 = sorted(values)[max(0, min(len(values) - 1, math.ceil(0.95 * len(values)) - 1))]
    return mean, math.sqrt(max(variance, 0.0)), p95


def print_aggregate(args: argparse.Namespace, runs: int, mode: str, perf_cpu_key: str) -> None:
    p_cpu = stats(args.outdir / LIST_FILES["prof_cpu"])
    p_samples = stats(args.outdir / LIST_FILES["prof_samples"])
    f_cpu = stats(args.outdir / LIST_FILES[perf_cpu_key])
    f_samples = stats(args.outdir / LIST_FILES["perf_samples"])
    improve = 0.0 if f_cpu[0] <= 0 else (f_cpu[0] - p_cpu[0]) / f_cpu[0] * 100

    print(f"\n=== 多轮统计汇总-{mode} (runs={runs}) ===")
    if mode == "collect-only" and args.profiler_symbolize:
        symbolize_note()
    print(
        f"{'tool':<12} {'cpu_mean(s)':<12} {'cpu_std(s)':<12} {'cpu_p95(s)':<12} "
        f"{'samples_mean':<14} {'samples_std':<14} {'samples_p95':<14}"
    )
    for tool, cpu, samples in (("profiler", p_cpu, p_samples), ("perf", f_cpu, f_samples)):
        print(
            f"{tool:<12} {cpu[0]:<12.6f} {cpu[1]:<12.6f} {cpu[2]:<12.6f} "
            f"{samples[0]:<14.6f} {samples[1]:<14.6f} {samples[2]:<14.6f}"
        )
    print(f"cpu_overhead_improvement_vs_perf(%): {improve:.2f}")


def run_one(args: argparse.Namespace, tag: str) -> None:
    print(f"[{tag}] [1/3] 启动负载: {args.workload}")
    workload = start_workload(args, args.outdir / f"workload_{tag}.log")
    try:
        print(f"[{tag}] [2/3] 对 workload(pid={workload.pid}) 运行 profiler")
        run_profiler(args, tag, workload.pid)
    finally:
        stop_workload(workload)

    print(f"[{tag}] [3/3] 重新启动同样负载并运行 perf")
    workload = start_workload(args, args.outdir / f"workload_{tag}.perf.log")
    try:
        run_perf(args, tag, workload.pid)
    finally:
        stop_workload(workload)

    print_round(args, tag)
    append_stats(args, tag)


def main() -> None:
    args = parse_args()
    validate(args)

    base_tag = datetime.now().strftime("%Y%m%d_%H%M%S")
    args.outdir = unique_run_dir(args.outdir, base_tag)
    write_config(args, base_tag)
    print(f"输出目录: {args.outdir}")
    if args.profiler_symbolize:
        symbolize_note()
    for name in LIST_FILES.values():
        (args.outdir / name).write_text("")

    completed = 0
    interrupted = False
    try:
        for index in range(1, args.runs + 1):
            run_one(args, f"{base_tag}_r{index:02d}")
            completed += 1
    except KeyboardInterrupt:
        interrupted = True
        print("\n收到 Ctrl-C，已停止当前子进程并保留已生成报告。")

    if completed > 1:
        print_aggregate(args, completed, "collect-only", "perf_cpu")
        print_aggregate(args, completed, "end-to-end", "perf_e2e_cpu")
    if interrupted:
        raise SystemExit(130)


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "--perf-post":
        perf_post_main(sys.argv[2:])
    else:
        main()
