# Profiler C++

一个基于 eBPF、perf events 和 C++ 的简易 Linux 性能分析器。该工具可以捕获堆栈跟踪（内核态和用户态），并使用 [blazesym](https://github.com/libbpf/blazesym) 提供符号化支持。

## 特性

- **采样分析**: 使用硬件或软件性能事件来采样堆栈跟踪。
- **基于 eBPF**: 在内核中进行低开销的堆栈收集。
- **符号化**: 使用 `blazesym` 将内存地址解析为函数名称。
- **PID 过滤**: 分析特定进程或整个系统。
- **输出格式**: 支持标准可读输出和折叠格式（兼容 [FlameGraph](https://github.com/brendangregg/FlameGraph)）。

## 前置条件

当前 Pixi 环境支持 Linux x86_64。请先准备以下宿主机依赖：

- **运行依赖**：Linux Kernel 5.8+，已启用 BPF/BTF 支持。
- **构建依赖**：
  - [**Pixi**](https://pixi.sh/latest/installation/)：管理 C++ 库、构建工具和任务。
  - **GCC/G++ 15 或 16**：支持 C++23，预设使用 `/usr/bin/gcc` 和 `/usr/bin/g++`。
  - **Clang**：需要包含 BPF 后端，用于编译 `bpf/profiler.bpf.c`。
  - **libbpf、libelf、zlib 开发包和 bpftool**：由系统包管理器提供，例如 Fedora 的 `libbpf-devel`、`elfutils-libelf-devel`、`zlib-devel`、`bpftool`。
  - **Git、Rust 和 Cargo**：用于获取 Corrosion 和构建 `blazesym`。

Pixi 安装 CMake（>= 3.28）、Ninja、CLI11、spdlog 及其传递依赖。`pixi.lock` 固定这些包的具体版本和构建；系统编译器、内核以及 Rust 工具链仍由宿主机提供。环境中的 C++ 运行库使用 GCC 16 系列，兼容上述系统编译器。

## 构建

1. **准备依赖**

   ```sh
   cd profiler
   git submodule update --init --recursive
   pixi install --locked
   ```

2. **构建 Release 或 Debug**

   ```sh
   # 默认使用 Release
   pixi run --locked build

   # 或者构建 Debug
   pixi run --locked build-debug
   ```

   两个任务都使用 Ninja 生成器，按顺序配置 CMake、构建 blazesym C API，再编译和链接 profiler，分别输出到 `build/Release/` 和 `build/Debug/`。

   Debug 预设保留原有静态检查开关；系统安装了 clang-tidy 时，现有 `-fix` 配置可能修改源码，请注意检查工作区差异。

3. **单独配置和验证**

   ```sh
   pixi run --locked configure
   pixi run --locked configure-debug

   # 构建 Release 并检查 --help，无需 root 权限
   pixi run --locked smoke
   ```

   CMake 预设通过 `CONDA_PREFIX` 查找 Pixi 环境中的库，因此配置和构建应在 `pixi run` 或 `pixi shell` 中执行。

   配置成功后，`build/compile_commands.json` 会自动链接到最近配置的构建目录，`.clangd` 无需调整。

### 切换已有构建目录

如果已有 Conan 构建目录，先移除仅由 Conan 自动生成的 `CMakeUserPresets.json`；若其中包含自己的配置，请保留自定义部分并移除 Conan 的 `include`。

预设统一使用 Ninja。从 Conan 或 Unix Makefiles 构建切换时，需要重新生成 CMake 缓存，避免沿用旧工具链、依赖路径或生成器。Corrosion 的 FetchContent 子构建有独立缓存，需要先一并切换：

```sh
# 仅重置已有子构建的配置，保留下载的 Corrosion 源码。
for mode in Release Debug; do
  subbuild="build/$mode/_deps/corrosion-subbuild"
  if [ -f "$subbuild/CMakeLists.txt" ]; then
    pixi run --locked cmake --fresh -S "$subbuild" -B "$subbuild" -G Ninja
  fi
done

pixi run --locked cmake --fresh --preset release
pixi run --locked cmake --fresh --preset debug
```

这只会重建对应目录的 CMake 配置，不需要删除整个 `build/` 目录。运行 profiler 时仍需要保留 `.pixi/` 环境，供动态链接器加载其中的 C++ 库。

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
