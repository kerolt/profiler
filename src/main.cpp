#include <sys/resource.h>
#include <cerrno>
#include <csignal>
#include <cstdint>

#include <print>

#include <bpf/libbpf.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <CLI/CLI.hpp>

#include "event.h"
#include "perf.h"
#include "profiler.skel.h"

static auto handle_event_wrapper(void* ctx, void* data, size_t data_sz) -> int {
    auto* handler = static_cast<EventHandler*>(ctx);
    return handler->handle(static_cast<const uint8_t*>(data), data_sz);
}

static volatile bool exiting = false;
static void sig_handler(int sig) { exiting = true; }

/* ----------------------- 简单封装 profiler_bpf 对象 ----------------------- */
struct ProfilerSkel {
    profiler_bpf* obj{nullptr};

    ProfilerSkel() {
        obj = profiler_bpf::open_and_load();
        if (obj == nullptr) {
            spdlog::error("Failed to open and load BPF object");
        }
    }

    ~ProfilerSkel() {
        if (obj != nullptr) {
            profiler_bpf__destroy(obj);
        }
    }

    // 重载布尔运算符，方便检查对象是否成功创建
    operator bool() const { return obj != nullptr; }

    // 重载 -> 运算符，方便访问内部的 profiler_bpf 指针
    auto operator->() const -> profiler_bpf* { return obj; }
};

/* -------------------- RingBuffer 简单封装 ------------------- */
struct RingBuffer {
    ring_buffer* rb;

    RingBuffer(int map_fd, ring_buffer_sample_fn sample_cb, void* ctx,
               const struct ring_buffer_opts* opts) {
        rb = ring_buffer__new(map_fd, sample_cb, ctx, opts);
    }

    ~RingBuffer() {
        if (rb != nullptr) {
            ring_buffer__free(rb);
        }
    }

    operator bool() const { return rb != nullptr; }

    [[nodiscard]]
    auto poll(int timeout_ms) const -> int {
        return ring_buffer__poll(rb, timeout_ms);
    }
};

/* -------------------------- 命令行参数结构体 -------------------------- */
struct Args {
    uint64_t freq = 10;
    uint8_t verbosity = 0;
    bool sw_event = false;
    int32_t pid = -1;
    bool fold_extend = false;
    bool no_symbolize = false;
};

/* ---------------------------- main函数 ----------------------------- */
auto main(int argc, const char* argv[]) -> int {
    CLI::App app{"A simple profiler using eBPF"};
    Args args;

    // Frequency
    app.add_option("-f,--freq", args.freq, "Sampling frequency")
        ->default_val(10);

    // Verbosity (Count 模式)
    app.add_flag("-v,--verbose",
                 args.verbosity,
                 "Increase verbosity (can be supplied multiple times)");

    // Software event
    app.add_flag("--sw-event",
                 args.sw_event,
                 "Use software event for triggering stack trace capture.\n"
                 "This can be useful for compatibility reasons if hardware "
                 "event is not available.");

    // PID Filter
    app.add_option("-p,--pid", args.pid, "Filter by PID (optional)");

    // Output format
    app.add_flag("-E,--fold-extend",
                 args.fold_extend,
                 "Output in extended folded format (timestamp_ns comm pid tid "
                 "cpu stack1;stack2;...)");

    app.add_flag("--no-symbolize",
                 args.no_symbolize,
                 "Disable userspace symbolization and per-sample output. "
                 "Useful for low-overhead collection benchmark.");

    // 解析命令行参数
    CLI11_PARSE(app, argc, argv);  // NOLINT

    auto freq = args.freq < 1 ? 1 : args.freq;

    // 映射日志级别逻辑
    using Level = spdlog::level::level_enum;
    Level level;
    if (args.verbosity == 0) {
        level = Level::warn;
    } else if (args.verbosity == 1) {
        level = Level::info;
    } else if (args.verbosity == 2) {
        level = Level::debug;
    } else {
        level = Level::trace;
    }

    auto console = spdlog::stdout_color_mt("console");
    spdlog::set_default_logger(console);
    spdlog::set_level(level);

    // 提高内存锁定限制
    rlimit rl = {.rlim_cur = RLIM_INFINITY, .rlim_max = RLIM_INFINITY};
    setrlimit(RLIMIT_MEMLOCK, &rl);

    ProfilerSkel obj;
    if (!obj) {
        spdlog::error("Fialed to open and load BPF object");
        return 1;
    }

    auto perf_fds = init_perf_monitor(freq, args.sw_event, args.pid);
    if (!perf_fds) {
        spdlog::error("Failed to initialize perf monitor");
        return 1;
    }

    // 挂载
    attach_perf_events(perf_fds.value(), obj->progs.profile);

    EventHandler event_handler(
        args.fold_extend ? OutputFormat::FoldExtend : OutputFormat::Standard,
        args.no_symbolize ? ProcessingMode::RawCount
                          : ProcessingMode::Symbolize);
    RingBuffer rb{bpf_map__fd(obj->maps.events),
                  handle_event_wrapper,
                  &event_handler,
                  nullptr};

    if (!rb) {
        spdlog::error("Failed to create ring buffer");
        return 1;
    }

    signal(SIGINT, sig_handler);
    while (!exiting) {
        int err = rb.poll(100);
        if (err == -EINTR) {
            // Interrupted by signal, continue to check exiting flag
            continue;
        }
        if (err < 0) {
            spdlog::error("Error polling ring buffer: {}", err);
            break;
        }
    }

    auto r = close_perf_events(perf_fds.value());
    if (!r) {
        spdlog::error("Failed to close perf events, error message is: {}",
                      static_cast<int>(r.error()));
        return 1;
    }

    if (args.no_symbolize) {
        std::println("samples={}", event_handler.sample_count());
    }

    return 0;
}