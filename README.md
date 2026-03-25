# Profiler C++

一个基于 eBPF、perf events 和 C++ 的简易 Linux 性能分析器。该工具可以捕获堆栈跟踪（内核态和用户态），并使用 [blazesym](https://github.com/libbpf/blazesym) 提供符号化支持。

## 特性

- **采样分析**: 使用硬件或软件性能事件来采样堆栈跟踪。
- **基于 eBPF**: 在内核中进行低开销的堆栈收集。
- **符号化**: 使用 `blazesym` 将内存地址解析为函数名称。
- **PID 过滤**: 分析特定进程或整个系统。
- **输出格式**: 支持标准可读输出和折叠格式（兼容 [FlameGraph](https://github.com/brendangregg/FlameGraph)）。

## 前置条件

在通过 CMake 和 Conan 构建之前，请确保已安装以下内容：

- **运行依赖**:
  - Linux Kernel 5.8+ (已启用 BPF/BTF 支持)

- **构建依赖**:
  - **C++23**。本人开发环境为 Fedora 43，使用的编译器版本如下：
    - gcc: 15.2.1
    - clang: 21.1.7
  - **libbpf-devel**: version 1.6.1
  - **bpftool**: version 7.6.0
  - [**CMake**](https://cmake.org/) (>= 3.19)
  - [**Conan**](https://conan.io/) (包管理器)
  - [**Rust**](https://www.rust-lang.org/) (构建 `blazesym` 需要)

## 构建

1. **安装依赖**

   使用 Conan 安装 C++ 依赖：

   ```sh
   cd profiler

   # 初始化并更新 git 子模块 (blazesym)
   git submodule update --init --recursive

   # 如果是首次安装 Conan，先设置默认配置
   # conan profile detect

   # 安装依赖 (Release 模式)
   conan install . -s build_type=Release --output-folder=. --build=missing
   # 或者 Debug 模式
   # conan install . -s build_type=Debug --output-folder=. --build=missing
   ```

2. **使用 CMake 配置**

   使用 Conan 生成的预设 (presets)：

   ```sh
   # Release
   cmake --preset release
   # 或者 Debug
   # cmake --preset debug
   ```

3. **构建 Blazesym (C API)**

   这一步编译 Rust 库可能需要一点时间：

   ```sh
   cmake --build build/Release --target _cargo-build_blazesym_c
   ```

4. **构建 Profiler**

   ```sh
   cmake --build build/Release --target profiler
   ```

## 使用方法

Profiler 需要 `root` 权限 (CAP_PERFMON / CAP_SYS_ADMIN) 来加载 BPF 程序。

```sh
sudo ./build/Release/profiler [OPTIONS]
```

### 选项

| 选项                | 描述                                                      | 默认值               |
| ------------------- | --------------------------------------------------------- | -------------------- |
| `-f, --freq <N>`    | 采样频率 (Hz)                                             | `10`                 |
| `-p, --pid <PID>`   | 按进程 ID 过滤 (可选)                                     | 监控所有进程         |
| `--sw-event`        | 使用软件事件 (cpu-clock) 代替硬件周期。在虚拟机中很有用。 | 硬件周期 (HW Cycles) |
| `-E, --fold-extend` | 以适用于火焰图的扩展折叠格式输出                          | 标准格式             |
| `-v, --verbose`     | 增加日志详细程度                                          | Warning              |

### 示例

**1. 基本全系统分析**
以 49 Hz 采样：

```sh
sudo ./build/Release/profiler -f 49
```

**2. 分析特定进程**
分析 PID 12345：

```sh
sudo ./build/Release/profiler -p 12345
```

**3. 使用软件事件 (例如在虚拟机中)**
如果硬件计数器不可用：

```sh
sudo ./build/Release/profiler --sw-event
```

**4. 生成火焰图 (FlameGraph)**

你可以将输出直接通过管道传输给 `flamegraph.pl` (来自 Brendan Gregg 的工具集)：

```sh
# 生成数据
sudo ./build/Release/profiler -f 99 -E > out.folded
# (按 Ctrl+C 停止)

# 生成 SVG
./FlameGraph/flamegraph.pl out.folded > profile.svg
```

## 项目结构

- `src/`: 用户态代理的 C++ 源代码。
- `bpf/`: eBPF C 代码 (内核侧)。
- `cmake/`: CMake 辅助模块。
- `third_party/`: 外部依赖 (blazesym)。

## 与 perf 对比采样开销

仓库内提供了脚本：`scripts/benchmark.sh`，用于在同一份负载上对比 `profiler` 和 `perf` 的采样器开销。

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

默认情况下，脚本会让 `profiler` 使用 `--no-symbolize`（仅采集计数，不做逐样本符号化/输出），用于更公平地比较采集路径开销。
如需恢复符号化输出，可在脚本中添加 `--profiler-symbolize`。

```sh
sudo ./scripts/benchmark.sh \
   --freq 199 \
   --duration 20 \
   --runs 5 \
   --workload "yes > /dev/null" \
   --outdir ./benchmark_out/repeat \
   --profiler-symbolize
```

更多参数可查看：

```sh
./scripts/benchmark.sh --help
```

## 参考

1. [eBPF Tutorial by Example 12](https://eunomia.dev/tutorials/12-profile/) - 使用 eBPF 程序 profile 进行性能分析
2. [blazesym](https://github.com/libbpf/blazesym) - 用于符号化的 Rust 库和 C API。
3. [libbpf-bootstrap/profile.c](https://github.com/libbpf/libbpf-bootstrap/blob/master/examples/c/profile.c) - libbpf-bootstrap 中的性能分析示例。
4. [libbpf-bootstrap/tols/cmake](https://github.com/libbpf/libbpf-bootstrap/tree/master/tools/cmake) - libbpf-bootstrap 的 CMake 构建工具。
