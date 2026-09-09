# Profiler

一个基于 eBPF、perf events 和 Rust 的 Linux 性能分析器。内核态 eBPF 程序保留使用 C 编写，用户态程序使用 Rust、libbpf-rs 和 blazesym Rust API，负责采集、符号化和输出堆栈跟踪。

当前仓库基准脚本 `scripts/benchmark.py` 的实测结果显示：

- 在 `collect-only` 口径（仅采集）下，当前实现开销高于 `perf record`。
- 在 `end-to-end` 口径（采集+后处理）下，当前实现总开销低于 `perf` 工作流。

## 特性

- **采样分析**: 使用硬件或软件性能事件来采样堆栈跟踪。
- **基于 eBPF**: 在内核中进行低开销的堆栈收集。
- **符号化**: 使用 `blazesym` 将内存地址解析为函数名称。
- **PID 过滤**: 分析特定进程或整个系统。
- **输出格式**: 支持标准可读输出和折叠格式（兼容 [FlameGraph](https://github.com/brendangregg/FlameGraph)）。

## 前置条件

在通过 Cargo 构建之前，请确保已安装以下内容：

- **运行依赖**:
  - Linux Kernel 5.8+，并启用 BPF/BTF 支持
  - 运行采样所需的 root 权限或 CAP_BPF、CAP_PERFMON 等能力

- **构建依赖**:
  - [**Rust**](https://www.rust-lang.org/) 和 Cargo
  - **clang**，用于编译 C eBPF 程序
  - **bpftool**，用于从运行中的内核 BTF 生成 `vmlinux.h`
  - libelf、zlib 等 libbpf 系统依赖

## 构建

项目使用 Cargo 构建 Rust 用户态程序，并在 `build.rs` 中调用 `libbpf-cargo` 编译现有的 C eBPF 程序、生成 BPF skeleton 和 Rust 类型绑定。

```sh
# 构建 Debug 版本
cargo build

# 构建 Release 版本
cargo build --release
```

构建过程会通过 `bpftool btf dump` 从 `/sys/kernel/btf/vmlinux` 生成 `vmlinux.h`，因此构建主机需要提供运行中内核的 BTF 信息。

## 使用方法

Profiler 需要 root 权限或相应的 BPF/perf 能力来加载程序。

```sh
sudo ./target/release/profiler [OPTIONS]
```

### 选项

| 选项                | 描述                                               | 默认值       |
| ------------------- | -------------------------------------------------- | ------------ |
| `-f, --freq <N>`    | 采样频率 (Hz)                                      | `10`         |
| `-p, --pid <PID>`   | 按进程 ID 过滤                                     | 监控所有进程 |
| `--filter <MODE>`   | `tgid`、`pgrp`、`session` 或 `cgroup`              | `session`    |
| `--sw-event`        | 使用软件事件 `cpu-clock` 代替硬件周期             | 硬件周期     |
| `-E, --fold-extend` | 输出适用于 FlameGraph 的折叠格式                  | 标准格式     |
| `--no-symbolize`    | 禁用符号化，仅统计样本数量                        | 启用符号化   |
| `-v, --verbose`     | 增加日志详细程度，可重复使用                      | Warning      |

### 示例

**1. 基本全系统分析**

```sh
sudo ./target/release/profiler -f 49
```

**2. 分析特定进程**

```sh
sudo ./target/release/profiler -p 12345 --filter tgid
```

**3. 使用软件事件**

```sh
sudo ./target/release/profiler --sw-event
```

**4. 仅采集样本数量**

```sh
sudo timeout --signal=INT 30s ./target/release/profiler --sw-event --no-symbolize -f 99 -v
```

**5. 生成火焰图 (FlameGraph)**

仓库通过 git submodule 引入 Brendan Gregg 的 [FlameGraph](https://github.com/brendangregg/FlameGraph)，路径为 `third_party/FlameGraph`。

```sh
sudo ./target/release/profiler -f 99 -E > out.folded
./third_party/FlameGraph/flamegraph.pl out.folded > profile.svg
```

## 项目结构

- `src/`: Rust 用户态程序，包括参数解析、perf event、事件处理和符号化。
- `bpf/`: C eBPF 程序，使用 libbpf-cargo 编译。
- `build.rs`: 生成 `vmlinux.h` 并生成 Rust BPF skeleton。
- `third_party/FlameGraph/`: FlameGraph 工具。

## 与 perf 对比采样开销

仓库内提供了脚本：`scripts/benchmark.py`，用于在同一份负载上对比 `profiler` 和 `perf` 的采样器开销。

核心对比指标：

- `user_sec + sys_sec`: 采样器自身 CPU 开销（越低越好）
- `samples`: 采样输出条数（近似采样量）

支持多轮统计（`--runs N`），会额外输出：

- `mean`: 平均值
- `std`: 标准差
- `p95`: 95 分位

脚本会自动给出两组比较结果：

- `collect-only`: `profiler` 对比 `perf record`（采集阶段）
- `end-to-end`: `profiler` 对比 `perf record + perf script/report`（端到端）

默认不开 `--profiler-symbolize` 时，`profiler` 会使用 `--no-symbolize`，只统计样本数量，不做逐样本符号化和文本输出；此时 `collect-only` 可近似用于比较 `profiler` 与 `perf record` 的采集路径开销。

开启 `--profiler-symbolize` 后，`profiler` 会在采集过程中同步完成符号化和文本输出，因此 `collect-only` 中的 `profiler` 不再是纯采集口径，不适合直接对比 `perf record`。这种情况下应主要参考 `end-to-end`，即 `profiler` 对比 `perf record + perf script/report` 的完整流程开销。

### 一组真实 bench 结果（示例）

测试参数：`freq=199`、`duration=20s`、`runs=5`、`workload="yes > /dev/null"`。

- `collect-only`:
  - `profiler cpu_mean = 0.510s`
  - `perf cpu_mean = 0.344s`
  - `cpu_overhead_improvement_vs_perf = -48.26%`
- `end-to-end`:
  - `profiler cpu_mean = 0.510s`
  - `perf cpu_mean = 0.732s`
  - `cpu_overhead_improvement_vs_perf = 30.33%`

解读：仅看采集路径时当前实现不占优；看完整分析流程时当前实现总开销更低。

如需恢复符号化输出，可在脚本中添加 `--profiler-symbolize`。脚本会在该模式下打印提示，提醒 `collect-only` 中的 `profiler` 包含符号化和输出开销。

`--outdir` 表示输出根目录。每次测试会在该目录下新建一个时间戳子目录，例如：

```text
report/20260420_104922/
```

该目录内会包含每轮的 profiler/perf 输出、时间统计、样本统计，以及 `benchmark_config.json` 参数快照。

```sh
sudo ./scripts/benchmark.py \
   --freq 199 \
   --duration 20 \
   --runs 5 \
   --workload "yes > /dev/null" \
   --outdir ./benchmark_out/repeat \
   --profiler-symbolize
```

更多参数可查看：

```sh
./scripts/benchmark.py --help
```

## 参考

1. [eBPF Tutorial by Example 12](https://eunomia.dev/tutorials/12-profile/) - 使用 eBPF 程序 profile 进行性能分析
2. [blazesym](https://github.com/libbpf/blazesym) - 用于符号化的 Rust 库。
3. [libbpf-rs](https://github.com/libbpf/libbpf-rs) - libbpf 的 Rust 封装。
4. [FlameGraph](https://github.com/brendangregg/FlameGraph) - 用于把折叠栈数据生成火焰图 SVG。
4. [libbpf-bootstrap/profile.c](https://github.com/libbpf/libbpf-bootstrap/blob/master/examples/c/profile.c) - libbpf-bootstrap 中的性能分析示例。
