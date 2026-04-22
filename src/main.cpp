#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <cstdint>

#include <bpf/libbpf.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <CLI/CLI.hpp>

#include "common.h"
#include "event.h"
#include "perf.h"

auto handle_event_wrapper(void* ctx, void* data, size_t data_sz) -> int {
    auto* handler = static_cast<EventHandler*>(ctx);
    return handler->handle(static_cast<const uint8_t*>(data), data_sz);
}

volatile bool exiting = false;
void sig_handler(int sig) { exiting = true; }

struct Args {
    uint64_t freq = 10;
    uint8_t verbosity = 0;
    bool sw_event = false;
    int32_t pid = -1;
    std::string filter = "session";
    bool fold_extend = false;
    bool no_symbolize = false;
};

auto main(int argc, const char* argv[]) -> int {
    try {
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
        app.add_option("--filter",
                       args.filter,
                       "Filter scope for --pid: tgid, pgrp, session, cgroup")
            ->default_val("session");

        // Output format
        app.add_flag(
            "-E,--fold-extend",
            args.fold_extend,
            "Output in extended folded format (timestamp_ns comm pid tid "
            "cpu stack1;stack2;...)");

        app.add_flag("--no-symbolize",
                     args.no_symbolize,
                     "Disable userspace symbolization and per-sample output. "
                     "Useful for low-overhead collection benchmark.");

        // 解析命令行参数
        CLI11_PARSE(app, argc, argv);

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

        FilterMode filter_mode = parse_filter_mode(args.filter);
        uint64_t target_id = resolve_target_id(args.pid, filter_mode);
        ProfilerSkel obj{target_id, filter_mode};
        if (!obj) {
            spdlog::error("Failed to open and load BPF object");
            return 1;
        }

        int32_t perf_pid = target_id == 0 ? args.pid : -1;
        auto perf_fds = init_perf_monitor(freq, args.sw_event, perf_pid);
        if (!perf_fds) {
            spdlog::error("Failed to initialize perf monitor");
            return 1;
        }

        // 挂载
        attach_perf_events(perf_fds.value(), obj->progs.profile);

        EventHandler event_handler(args.fold_extend ? OutputFormat::FoldExtend
                                                    : OutputFormat::Standard,
                                   args.no_symbolize
                                       ? ProcessingMode::RawCount
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
        event_handler.flush();

        auto r = close_perf_events(perf_fds.value());
        if (!r) {
            spdlog::error("Failed to close perf events, error message is: {}",
                          static_cast<int>(r.error()));
            return 1;
        }

        if (args.no_symbolize) {
            spdlog::info("samples={}", event_handler.sample_count());
        }

        return 0;
    } catch (const CLI::ValidationError& e) {
        spdlog::error("Command line argument validation error: {}", e.what());
        return 1;
    } catch (const std::format_error& e) {
        spdlog::error("Format string error: {}", e.what());
        return 1;
    } catch (const std::exception& e) {
        spdlog::error("Unexpected standard exception: {}", e.what());
        return 1;
    } catch (...) {
        spdlog::error("Unknown non-standard exception occurred");
        return 1;
    }
}
