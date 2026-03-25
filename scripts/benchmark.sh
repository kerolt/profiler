#!/usr/bin/env bash

set -euo pipefail

# -------------------- 默认参数 --------------------
PROFILER_BIN="./build/Release/profiler"
FREQ=99
DURATION=15
RUNS=1
WORKLOAD_CMD="sha1sum /dev/zero"
WORKDIR=""
OUTDIR="./benchmark_out"
USE_SW_EVENT=0
PROFILER_NO_SYMBOLIZE=1
LAST_PID=0

TIME_FMT='elapsed_sec=%e\nuser_sec=%U\nsys_sec=%S\ncpu_pct=%P\nmax_rss_kb=%M'

die() {
	echo "$*" >&2
	exit 1
}

require_cmd() {
	command -v "$1" >/dev/null 2>&1 || die "缺少命令: $1"
}

usage() {
	cat <<'EOF'
用法:
  sudo ./scripts/benchmark.sh [选项]

说明:
  同一负载下自动输出两组对比：
  1) collect-only: profiler vs perf record
  2) end-to-end: profiler vs (perf record + perf script/report)

选项:
  --profiler-bin <path>   profiler 可执行文件路径 (默认: ./build/Release/profiler)
  --freq <hz>             采样频率 (默认: 99)
  --duration <sec>        每次采样时长，单位秒 (默认: 15)
  --runs <N>              重复运行轮数并输出汇总 (默认: 1)
  --workload <cmd>        被测负载命令
  --workdir <dir>         负载运行目录
  --outdir <dir>          输出目录
  --sw-event              profiler 使用软件事件
  --profiler-symbolize    启用符号化输出（默认关闭，用于低开销基准）
  -h, --help              显示帮助

示例:
  sudo ./scripts/benchmark.sh --freq 199 --duration 20 --runs 5 \
    --workload "yes > /dev/null" --outdir ./benchmark_out/repeat
EOF
}

parse_args() {
	while [[ $# -gt 0 ]]; do
		case "$1" in
		--profiler-bin)
			PROFILER_BIN="$2"
			shift 2
			;;
		--freq)
			FREQ="$2"
			shift 2
			;;
		--duration)
			DURATION="$2"
			shift 2
			;;
		--runs)
			RUNS="$2"
			shift 2
			;;
		--workload)
			WORKLOAD_CMD="$2"
			shift 2
			;;
		--workdir)
			WORKDIR="$2"
			shift 2
			;;
		--outdir)
			OUTDIR="$2"
			shift 2
			;;
		--sw-event)
			USE_SW_EVENT=1
			shift
			;;
		--profiler-symbolize)
			PROFILER_NO_SYMBOLIZE=0
			shift
			;;
		-h | --help)
			usage
			exit 0
			;;
		*)
			die "未知参数: $1"
			;;
		esac
	done
}

run_with_time() {
	local time_file="$1"
	shift
	/usr/bin/time -f "$TIME_FMT" -o "$time_file" "$@" || true
}

metric() {
	local file="$1"
	local key="$2"
	awk -F'=' -v k="$key" '$1==k {print $2}' "$file" | tail -n1
}

non_empty_lines() {
	local file="$1"
	[[ -f "$file" ]] || {
		echo 0
		return
	}
	grep -cve '^\s*$' "$file" || true
}

stats3() {
	local file="$1"
	awk '
        {
            a[++n]=$1;
            sum+=$1;
            sumsq+=$1*$1;
        }
        END {
            if (n==0) { print "0 0 0"; exit; }
            mean=sum/n;
            var=(sumsq/n)-(mean*mean);
            if (var<0) var=0;
            std=sqrt(var);
            asort(a);
            idx=int((95*n+99)/100);
            if (idx<1) idx=1;
            if (idx>n) idx=n;
            printf "%.6f %.6f %.6f\n", mean, std, a[idx];
        }
    ' "$file"
}

# -------------------- workload 与采样执行 --------------------
# 启动 workload，并把 PID 写入全局变量 LAST_PID
start_workload() {
	local log_file="$1"
	local wd="${WORKDIR:-$PWD}"

	(
		cd "$wd"
		exec bash -lc "exec $WORKLOAD_CMD"
	) >"$log_file" 2>&1 &

	local pid=$!
	sleep 0.3
	kill -0 "$pid" 2>/dev/null || die "负载未正常启动，请检查命令: $WORKLOAD_CMD"
	LAST_PID="$pid"
}

# 结束 workload，避免污染后续轮次
stop_workload() {
	local pid="$1"
	if kill -0 "$pid" 2>/dev/null; then
		kill -TERM "$pid" 2>/dev/null || true
		sleep 0.2
		kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true
	fi
	wait "$pid" 2>/dev/null || true
}

run_profiler() {
	local tag="$1"
	local pid="$2"
	local out_file="$OUTDIR/profiler_${tag}.out"
	local time_file="$OUTDIR/profiler_${tag}.time"

	local cmd=("$PROFILER_BIN" "-p" "$pid" "-f" "$FREQ" "-E")
	[[ "$USE_SW_EVENT" -eq 1 ]] && cmd+=("--sw-event")
	[[ "$PROFILER_NO_SYMBOLIZE" -eq 1 ]] && cmd+=("--no-symbolize")

	run_with_time "$time_file" timeout --signal=INT "${DURATION}s" "${cmd[@]}" >"$out_file" 2>&1

	# 低开销模式下优先解析 samples=N；兼容符号化模式时退化为非空行计数
	local samples
	samples="$(awk -F'=' '/^samples=[0-9]+$/ {print $2}' "$out_file" | tail -n1)"
	[[ -z "$samples" ]] && samples="$(non_empty_lines "$out_file")"
	echo "${samples:-0}" >"$OUTDIR/profiler_${tag}.samples"
}

perf_samples() {
	local tag="$1"
	local report_file="$OUTDIR/perf_${tag}.report"
	local script_file="$OUTDIR/perf_${tag}.script"

	local s
	s="$({
		awk 'match($0, /[Ss]amples:[[:space:]]*([0-9][0-9,]*)/, m) { gsub(/,/, "", m[1]); print m[1]; exit }' "$report_file"
	} 2>/dev/null || true)"

	if [[ -n "$s" ]]; then
		echo "$s"
	else
		awk '/^[^[:space:]#]/ {c++} END {print c+0}' "$script_file" 2>/dev/null || echo 0
	fi
}

run_perf() {
	local tag="$1"
	local pid="$2"
	local data_file="$OUTDIR/perf_${tag}.data"
	local script_file="$OUTDIR/perf_${tag}.script"
	local report_file="$OUTDIR/perf_${tag}.report"

	run_with_time "$OUTDIR/perf_${tag}.time" \
		timeout --signal=INT "${DURATION}s" perf record -F "$FREQ" -g -p "$pid" -o "$data_file" >/dev/null 2>&1

	# perf 后处理单独计时，用于 end-to-end 口径
	run_with_time "$OUTDIR/perf_post_${tag}.time" bash -c "
        perf script -i '$data_file' >'$script_file' 2>/dev/null || true
        perf report --stdio --stats -i '$data_file' >'$report_file' 2>/dev/null || true
    "

	perf_samples "$tag" >"$OUTDIR/perf_${tag}.samples"
}

# -------------------- 输出与统计 --------------------
print_collect_summary() {
	local tag="$1"

	local p_user p_sys p_elapsed p_cpu p_rss p_samples
	local f_user f_sys f_elapsed f_cpu f_rss f_samples

	p_user="$(metric "$OUTDIR/profiler_${tag}.time" user_sec)"
	p_sys="$(metric "$OUTDIR/profiler_${tag}.time" sys_sec)"
	p_elapsed="$(metric "$OUTDIR/profiler_${tag}.time" elapsed_sec)"
	p_cpu="$(metric "$OUTDIR/profiler_${tag}.time" cpu_pct)"
	p_rss="$(metric "$OUTDIR/profiler_${tag}.time" max_rss_kb)"
	p_samples="$(cat "$OUTDIR/profiler_${tag}.samples")"

	f_user="$(metric "$OUTDIR/perf_${tag}.time" user_sec)"
	f_sys="$(metric "$OUTDIR/perf_${tag}.time" sys_sec)"
	f_elapsed="$(metric "$OUTDIR/perf_${tag}.time" elapsed_sec)"
	f_cpu="$(metric "$OUTDIR/perf_${tag}.time" cpu_pct)"
	f_rss="$(metric "$OUTDIR/perf_${tag}.time" max_rss_kb)"
	f_samples="$(cat "$OUTDIR/perf_${tag}.samples")"

	echo
	echo "=== 采样开销对比 (${tag}) ==="
	printf '%-12s %-10s %-10s %-10s %-8s %-12s\n' "tool" "user(s)" "sys(s)" "elapsed" "cpu" "samples"
	printf '%-12s %-10s %-10s %-10s %-8s %-12s\n' "profiler" "$p_user" "$p_sys" "$p_elapsed" "$p_cpu" "$p_samples"
	printf '%-12s %-10s %-10s %-10s %-8s %-12s\n' "perf" "$f_user" "$f_sys" "$f_elapsed" "$f_cpu" "$f_samples"
	printf '%-12s %-10s %-10s\n' "max_rss(kb)" "profiler" "$p_rss"
	printf '%-12s %-10s %-10s\n' "" "perf" "$f_rss"
}

print_e2e_summary() {
	local tag="$1"

	local p_user p_sys p_elapsed p_samples
	local f_user f_sys f_elapsed fp_user fp_sys fp_elapsed f_samples

	p_user="$(metric "$OUTDIR/profiler_${tag}.time" user_sec)"
	p_sys="$(metric "$OUTDIR/profiler_${tag}.time" sys_sec)"
	p_elapsed="$(metric "$OUTDIR/profiler_${tag}.time" elapsed_sec)"
	p_samples="$(cat "$OUTDIR/profiler_${tag}.samples")"

	f_user="$(metric "$OUTDIR/perf_${tag}.time" user_sec)"
	f_sys="$(metric "$OUTDIR/perf_${tag}.time" sys_sec)"
	f_elapsed="$(metric "$OUTDIR/perf_${tag}.time" elapsed_sec)"
	fp_user="$(metric "$OUTDIR/perf_post_${tag}.time" user_sec)"
	fp_sys="$(metric "$OUTDIR/perf_post_${tag}.time" sys_sec)"
	fp_elapsed="$(metric "$OUTDIR/perf_post_${tag}.time" elapsed_sec)"
	f_samples="$(cat "$OUTDIR/perf_${tag}.samples")"

	local p_total_cpu f_total_cpu f_total_elapsed
	p_total_cpu="$(awk -v u="${p_user:-0}" -v s="${p_sys:-0}" 'BEGIN{printf "%.6f", u+s}')"
	f_total_cpu="$(awk -v u="${f_user:-0}" -v s="${f_sys:-0}" -v pu="${fp_user:-0}" -v ps="${fp_sys:-0}" 'BEGIN{printf "%.6f", u+s+pu+ps}')"
	f_total_elapsed="$(awk -v a="${f_elapsed:-0}" -v b="${fp_elapsed:-0}" 'BEGIN{printf "%.2f", a+b}')"

	echo
	echo "=== 端到端开销对比 (${tag}) ==="
	printf '%-12s %-14s %-14s %-12s\n' "tool" "cpu_total(s)" "elapsed(s)" "samples"
	printf '%-12s %-14s %-14s %-12s\n' "profiler" "$p_total_cpu" "$p_elapsed" "$p_samples"
	printf '%-12s %-14s %-14s %-12s\n' "perf" "$f_total_cpu" "$f_total_elapsed" "$f_samples"
}

append_round_stats() {
	local tag="$1"

	local p_user p_sys p_samples
	local f_user f_sys f_samples fp_user fp_sys

	p_user="$(metric "$OUTDIR/profiler_${tag}.time" user_sec)"
	p_sys="$(metric "$OUTDIR/profiler_${tag}.time" sys_sec)"
	p_samples="$(cat "$OUTDIR/profiler_${tag}.samples")"

	f_user="$(metric "$OUTDIR/perf_${tag}.time" user_sec)"
	f_sys="$(metric "$OUTDIR/perf_${tag}.time" sys_sec)"
	f_samples="$(cat "$OUTDIR/perf_${tag}.samples")"

	fp_user="$(metric "$OUTDIR/perf_post_${tag}.time" user_sec)"
	fp_sys="$(metric "$OUTDIR/perf_post_${tag}.time" sys_sec)"

	awk -v u="${p_user:-0}" -v s="${p_sys:-0}" 'BEGIN{printf "%.6f\n", u+s}' >>"$OUTDIR/.profiler_cpu_total.list"
	echo "${p_samples:-0}" >>"$OUTDIR/.profiler_samples.list"

	awk -v u="${f_user:-0}" -v s="${f_sys:-0}" 'BEGIN{printf "%.6f\n", u+s}' >>"$OUTDIR/.perf_collect_cpu_total.list"
	awk -v u="${f_user:-0}" -v s="${f_sys:-0}" -v pu="${fp_user:-0}" -v ps="${fp_sys:-0}" 'BEGIN{printf "%.6f\n", u+s+pu+ps}' >>"$OUTDIR/.perf_e2e_cpu_total.list"
	echo "${f_samples:-0}" >>"$OUTDIR/.perf_samples.list"
}

print_aggregate() {
	local base_tag="$1"
	local mode="$2"
	local perf_cpu_file="$3"

	local p_cpu_mean p_cpu_std p_cpu_p95
	local p_smp_mean p_smp_std p_smp_p95
	local f_cpu_mean f_cpu_std f_cpu_p95
	local f_smp_mean f_smp_std f_smp_p95

	read -r p_cpu_mean p_cpu_std p_cpu_p95 < <(stats3 "$OUTDIR/.profiler_cpu_total.list")
	read -r p_smp_mean p_smp_std p_smp_p95 < <(stats3 "$OUTDIR/.profiler_samples.list")
	read -r f_cpu_mean f_cpu_std f_cpu_p95 < <(stats3 "$perf_cpu_file")
	read -r f_smp_mean f_smp_std f_smp_p95 < <(stats3 "$OUTDIR/.perf_samples.list")

	local improve
	improve="$(awk -v p="$p_cpu_mean" -v f="$f_cpu_mean" 'BEGIN{if (f<=0) print "0.00"; else printf "%.2f", ((f-p)/f)*100}')"

	echo
	echo "=== 多轮统计汇总-${mode} (${base_tag}, runs=${RUNS}) ==="
	printf '%-12s %-12s %-12s %-12s %-14s %-14s %-14s\n' "tool" "cpu_mean(s)" "cpu_std(s)" "cpu_p95(s)" "samples_mean" "samples_std" "samples_p95"
	printf '%-12s %-12s %-12s %-12s %-14s %-14s %-14s\n' "profiler" "$p_cpu_mean" "$p_cpu_std" "$p_cpu_p95" "$p_smp_mean" "$p_smp_std" "$p_smp_p95"
	printf '%-12s %-12s %-12s %-12s %-14s %-14s %-14s\n' "perf" "$f_cpu_mean" "$f_cpu_std" "$f_cpu_p95" "$f_smp_mean" "$f_smp_std" "$f_smp_p95"
	echo "cpu_overhead_improvement_vs_perf(%): $improve"
}

# -------------------- 主流程 --------------------
main() {
	parse_args "$@"

	[[ "$EUID" -eq 0 ]] || die "请使用 root 运行，例如: sudo $0 ..."
	require_cmd timeout
	require_cmd perf
	require_cmd /usr/bin/time

	[[ -x "$PROFILER_BIN" ]] || die "profiler 不存在或不可执行: $PROFILER_BIN"
	[[ "$RUNS" =~ ^[0-9]+$ ]] && [[ "$RUNS" -ge 1 ]] || die "--runs 必须是 >=1 的整数，当前值: $RUNS"

	mkdir -p "$OUTDIR"
	OUTDIR="$(cd "$OUTDIR" && pwd)"

	: >"$OUTDIR/.profiler_cpu_total.list"
	: >"$OUTDIR/.profiler_samples.list"
	: >"$OUTDIR/.perf_collect_cpu_total.list"
	: >"$OUTDIR/.perf_e2e_cpu_total.list"
	: >"$OUTDIR/.perf_samples.list"

	local base_tag i tag pid
	base_tag="$(date +%Y%m%d_%H%M%S)"

	for ((i = 1; i <= RUNS; i++)); do
		tag="${base_tag}_r$(printf '%02d' "$i")"

		echo "[${tag}] [1/3] 启动负载: $WORKLOAD_CMD"
		start_workload "$OUTDIR/workload_${tag}.log"
		pid="$LAST_PID"

		echo "[${tag}] [2/3] 对 workload(pid=$pid) 运行 profiler"
		run_profiler "$tag" "$pid"
		stop_workload "$pid"

		echo "[${tag}] [3/3] 重新启动同样负载并运行 perf"
		start_workload "$OUTDIR/workload_${tag}.perf.log"
		pid="$LAST_PID"
		run_perf "$tag" "$pid"
		stop_workload "$pid"

		print_collect_summary "$tag"
		print_e2e_summary "$tag"
		append_round_stats "$tag"
	done

	if [[ "$RUNS" -gt 1 ]]; then
		print_aggregate "$base_tag" "collect-only" "$OUTDIR/.perf_collect_cpu_total.list"
		print_aggregate "$base_tag" "end-to-end" "$OUTDIR/.perf_e2e_cpu_total.list"
	fi
}

main "$@"
