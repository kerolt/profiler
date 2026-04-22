#ifndef COMMON_H_
#define COMMON_H_

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <stdexcept>
#include <string>

#include <spdlog/spdlog.h>
#include <CLI/CLI.hpp>

#include "profiler.skel.h"

enum class FilterMode : uint8_t {
    Tgid = 0,
    Pgrp = 1,
    Session = 2,
    Cgroup = 3,
};

inline auto parse_filter_mode(const std::string& value) -> FilterMode {
    if (value == "tgid") {
        return FilterMode::Tgid;
    }
    if (value == "pgrp" || value == "pgid") {
        return FilterMode::Pgrp;
    }
    if (value == "session" || value == "sid") {
        return FilterMode::Session;
    }
    if (value == "cgroup") {
        return FilterMode::Cgroup;
    }
    throw CLI::ValidationError(
        "--filter must be one of: tgid, pgrp, session, cgroup");
}

inline auto cgroup_path_for_pid(pid_t pid) -> std::string {
    std::ifstream file{std::format("/proc/{}/cgroup", pid)};
    std::string line;
    while (std::getline(file, line)) {
        auto pos = line.find("::");
        if (pos != std::string::npos) {
            return "/sys/fs/cgroup" + line.substr(pos + 2);
        }
    }
    throw std::runtime_error("failed to read cgroup v2 path for target pid");
}

inline auto cgroup_id_for_pid(pid_t pid) -> uint64_t {
    struct stat st{};
    auto path = cgroup_path_for_pid(pid);
    if (stat(path.c_str(), &st) != 0) {
        throw std::runtime_error(
            std::format("failed to stat target cgroup: {}", path));
    }
    return st.st_ino;
}

inline auto resolve_target_id(pid_t pid, FilterMode mode) -> uint64_t {
    if (pid <= 0) {
        return 0;
    }

    switch (mode) {
        case FilterMode::Tgid:
            return static_cast<uint64_t>(pid);
        case FilterMode::Pgrp: {
            pid_t pgid = getpgid(pid);
            if (pgid < 0) {
                throw std::runtime_error(
                    "failed to resolve target process group");
            }
            return static_cast<uint64_t>(pgid);
        }
        case FilterMode::Session: {
            pid_t sid = getsid(pid);
            if (sid < 0) {
                throw std::runtime_error("failed to resolve target session");
            }
            return static_cast<uint64_t>(sid);
        }
        case FilterMode::Cgroup:
            return cgroup_id_for_pid(pid);
    }

    return 0;
}

/**
 * ProfilerSkel 对 libbpf 中 profiler_bpf 对象的简单封装
 */
struct ProfilerSkel {
    profiler_bpf* obj{nullptr};

    ProfilerSkel(uint64_t target_id, FilterMode filter_mode) {
        obj = profiler_bpf::open();
        if (obj == nullptr) {
            spdlog::error("Failed to open BPF object");
            return;
        }

        obj->rodata->target_id = target_id;
        obj->rodata->target_filter = static_cast<uint32_t>(filter_mode);
        if (profiler_bpf::load(obj) != 0) {
            spdlog::error("Failed to load BPF object");
            profiler_bpf__destroy(obj);
            obj = nullptr;
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

/**
 * RingBuffer 对 libbpf 中 ring_buffer 的简单封装
 */
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

#endif /* COMMON_H_ */
