#ifndef PERF_H_
#define PERF_H_

#include <vector>

#include <bpf/libbpf.h>

#include "utils.h"

auto perf_event_open(perf_event_attr* hw_event, pid_t pid, int cpu,
                     int group_fd, unsigned long flags) -> int64_t;

auto init_perf_monitor(uint64_t freq, bool sw_event, uint32_t pid)
    -> Result<std::vector<uint32_t>, libbpf_errno>;

auto attach_perf_events(const std::vector<uint32_t>& fds, bpf_program* prog)
    -> std::vector<Result<bpf_link*, libbpf_errno>>;

auto close_perf_events(const std::vector<uint32_t>& fds)
    -> Result<int, libbpf_errno>;

#endif /* PERF_H_ */
