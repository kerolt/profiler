//! 实现把从内核/用户态采集到的栈回溯事件（StacktraceEvent）解码、符号化（symbolize）并以两种可读格式输出的逻辑。
//!
//! 常见的应用场景是：结合 eBPF
//! 在内核中捕获栈回溯数据，然后在用户态把这些原始地址转成函数名、源文件位置等并打印/汇总，用于性能分析/采样分析（profiling）。

#include "event.h"

#include <cstddef>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <print>
#include <ranges>

#include "blaze.h"
#include "utils.h"

using AddrInfo = std::tuple<uint64_t, uint64_t, size_t>;

static void print_frame(const char* name, Option<AddrInfo> addr_info,
                        const blaze_symbolize_code_info* code_info) {
    std::string code_str;
    if (code_info != nullptr) {
        // path
        if ((code_info->dir != nullptr) && (code_info->file != nullptr)) {
            code_str = std::format(" {}/{})", code_info->dir, code_info->file);
        } else if (code_info->file != nullptr) {
            code_str = code_info->file;
        }

        // line and column
        if (code_info->line > 0) {
            code_str += std::format(":{}", code_info->line);
            if (code_info->column > 0) {
                code_str += std::format(":{}", code_info->column);
            }
        }
    }

    if (addr_info.has_value()) {
        auto [input_addr, addr, offset] = *addr_info;
        std::println("0x{:0>{}}: {} @ {:#x} + {:#x}{}",
                     input_addr,
                     ADDR_WIDTH,
                     (name != nullptr) ? name : "<unknown>",
                     addr,
                     offset,
                     code_str);
    } else {
        std::println("{:>{}} {}{} [inlined]", "", ADDR_WIDTH, name, code_str);
    }
}

auto EventHandler::get_boot_time_ns() -> uint64_t {
    auto now = std::chrono::system_clock::now();
    uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          now.time_since_epoch())
                          .count();

    struct sysinfo info;
    if (::sysinfo(&info) != 0) {
        return now_ns;
    }
    uint64_t uptime_ns = static_cast<uint64_t>(info.uptime) * 1'000'000'000ULL;
    return now_ns - uptime_ns;
}

auto EventHandler::symbolize_stack_to_vec(const uint64_t* stack,
                                          uint32_t stack_sz, uint32_t pid)
    -> std::vector<std::string> {
    if (stack_sz <= 0) {
        return {};
    }

    blaze::Source src = blaze::get_symbolize_source(pid);
    size_t count = stack_sz / sizeof(uint64_t);
    auto result = symbolizer_.symbolize(
        src, blaze::Input{.addrs_ = stack, .cnt_ = count});

    if (!result) {
        return {};
    }

    const auto* syms = result->syms_;
    std::vector<std::string> vec;

    if (syms == nullptr) {
        for (size_t i = 0; i < count; ++i) {
            vec.push_back(std::format("0x{:x}", stack[i]));
        }
        return vec;
    }

    for (size_t i = 0; i < syms->cnt; ++i) {
        if (syms->syms[i].name != nullptr) {
            vec.emplace_back(syms->syms[i].name);
        } else {
            vec.push_back(std::format("0x{:x}", stack[i]));
        }
    }

    return vec;
}

auto EventHandler::handle(const uint8_t* data, size_t len) -> int {
    if (len != sizeof(StacktraceEvent)) {
        std::println("Data length mismatch: expected {}, got {}",
                     sizeof(StacktraceEvent),
                     len);
        return 1;
    }

    const auto* const event = reinterpret_cast<const StacktraceEvent*>(data);

    if (event->kstack_size <= 0 && event->ustack_size <= 0) {
        return 1;
    }

    sample_count_++;

    if (mode_ == ProcessingMode::RawCount) {
        return 0;
    }

    if (format == OutputFormat::Standard) {
        handle_standard(event);
    } else {
        handle_fold_extend(event);
    }

    return 0;
}

void EventHandler::handle_standard(const StacktraceEvent* event) {
    uint64_t unix_ns = event->timestamp + boot_time_ns;
    std::println("[{}.{:09} COMM: {} (pid={}) @ CPU {}]",
                 unix_ns / 1'000'000'000,
                 unix_ns % 1'000'000'000,
                 event->comm,
                 event->pid,
                 event->cpu_id);

    // 打印内核栈
    if (event->kstack_size > 0) {
        std::println("Kernel:");
        show_stack_trace(event->kstack, event->kstack_size, 0);
    } else {
        std::println("Kernel: <no stack>");
    }

    // 打印用户栈
    if (event->ustack_size > 0) {
        std::println("Userspace:");
        show_stack_trace(event->ustack, event->ustack_size, event->pid);
    } else {
        std::println("Userspace: <no stack>");
    }

    std::println();
}

void EventHandler::handle_fold_extend(const StacktraceEvent* event) {
    std::vector<std::string> stack_frames;

    // 为了让火焰图能够按进程聚合，将 "comm-pid" 作为栈底
    stack_frames.push_back(std::format("{}-{}", event->comm, event->pid));

    // 处理用户态
    if (event->ustack_size > 0) {
        auto user_frames = symbolize_stack_to_vec(
            event->ustack, event->ustack_size, event->pid);
        for (const auto& frame : user_frames | std::views::reverse) {
            stack_frames.push_back(frame);
        }
    }

    // 处理内核态
    if (event->kstack_size > 0) {
        auto kern_frames =
            symbolize_stack_to_vec(event->kstack, event->kstack_size, 0);
        for (const auto& frame : kern_frames | std::views::reverse) {
            stack_frames.push_back(frame + "_[k]");
        }
    }

    auto temp = stack_frames | std::views::join_with(';');
    // 输出格式：stack;frames 1
    // FlameGraph 工具期望每行以空格和数字结尾
    std::println("{} 1", temp | std::ranges::to<std::string>());
}

void EventHandler::show_stack_trace(const uint64_t* stack, uint32_t size,
                                    uint32_t pid) {
    blaze::Source src = blaze::get_symbolize_source(pid);
    size_t count = static_cast<size_t>(size) / sizeof(uint64_t);  // 栈帧数量
    auto result = symbolizer_.symbolize(
        src, blaze::Input{.addrs_ = stack, .cnt_ = count});

    if (!result) {
        std::println(stderr,
                     "  Failed to symbolize stack trace. err: {}",
                     result.error());
        return;
    }

    const auto* syms = result->syms_;
    for (size_t i = 0; i < count; ++i) {
        if (i < syms->cnt && (syms->syms[i].name != nullptr)) {
            print_frame(
                syms->syms[i].name,
                Option<AddrInfo>(std::make_tuple(
                    stack[i], syms->syms[i].addr, syms->syms[i].offset)),
                &syms->syms[i].code_info);

            // 打印内联函数信息
            for (size_t j = 0; j < syms->syms[i].inlined_cnt; ++j) {
                print_frame(syms->syms[i].inlined[j].name,
                            std::nullopt,
                            &syms->syms[i].inlined[j].code_info);
            }
        } else {
            std::println("{:>0{}}: <no-symbol>", stack[i], ADDR_WIDTH);
        }
    }
}
