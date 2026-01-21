#include "perf.h"

#include <bpf/libbpf.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdint>
#include <vector>
#include "utils.h"

auto perf_event_open(perf_event_attr* hw_event, pid_t pid, int cpu,
                     int group_fd, unsigned long flags) -> int64_t {
    return syscall(SYS_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

auto init_perf_monitor(uint64_t freq, bool sw_event, uint32_t pid = -1)
    -> Result<std::vector<uint32_t>, libbpf_errno> {
    int cpus = libbpf_num_possible_cpus();
    if (cpus < 0) {
        return Err(libbpf_errno::LIBBPF_ERRNO__INTERNAL);
    }

    perf_event_attr attr = {
        .type = sw_event ? PERF_TYPE_SOFTWARE : PERF_TYPE_HARDWARE,  // 采样类型
        .size = sizeof(perf_event_attr),
        .config = (sw_event ? static_cast<uint64_t>(PERF_COUNT_SW_CPU_CLOCK)
                            : static_cast<uint64_t>(PERF_COUNT_HW_CPU_CYCLES)),
        .sample_freq = freq,  // 设置频率，每秒 freq 次采样
        .freq = 1,            // 标志位：设为 1 表示使用 sample_freq
    };

    std::vector<uint32_t> fds;
    for (int cpu = 0; cpu < cpus; ++cpu) {
        int64_t fd = perf_event_open(&attr, pid, cpu, -1, 0);
        if (fd < 0) {
            return Err(libbpf_errno::LIBBPF_ERRNO__INTERNAL);
        }
        fds.push_back(fd);
    }

    return fds;
}

auto attach_perf_events(const std::vector<uint32_t>& fds, bpf_program* prog)
    -> std::vector<Result<bpf_link*, libbpf_errno>> {
    std::vector<Result<bpf_link*, libbpf_errno>> links;
    for (const auto& fd : fds) {
        auto link = bpf_program__attach_perf_event(prog, fd);
        if (libbpf_get_error(link)) {
            links.push_back(Err(libbpf_errno::LIBBPF_ERRNO__INTERNAL));
        }

        links.push_back(link);
    }
    return links;
}

auto close_perf_events(const std::vector<uint32_t>& fds)
    -> Result<int, libbpf_errno> {
    for (const auto& fd : fds) {
        if (close(fd) != 0) {
            return Err(libbpf_errno::LIBBPF_ERRNO__INTERNAL);
        }
    }
    return 0;
}