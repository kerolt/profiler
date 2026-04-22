#include "vmlinux.h"

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>

char LICENSE[] SEC("license") = "Dual BSD/GPL";

#ifndef TASK_COMM_LEN
#define TASK_COMM_LEN 16
#endif

#ifndef MAX_STACK_DEPTH
#define MAX_STACK_DEPTH 128
#endif

typedef __u64 stack_trace_t[MAX_STACK_DEPTH];

/**
 * filter_mode 定义了四种过滤模式：
 * 1. 线程组 ID（tgid）
 * 2. 进程组 ID（pgrp）
 * 3. 会话 ID（session）
 * 4. cgroup ID（cgroup）
 */
enum filter_mode {
    FILTER_TGID = 0,
    FILTER_PGRP = 1,
    FILTER_SESSION = 2,
    FILTER_CGROUP = 3,
};

/**
 * target_id 和 target_filter 通过 BPF ROData 传入，
 * 分别表示过滤的目标 ID 和过滤模式
 */
const volatile __u64 target_id = 0;
const volatile __u32 target_filter = FILTER_SESSION;

struct stack_trace_event {
    __u32 pid;
    __u32 cpu_id;
    __u64 timestamp;

    char comm[TASK_COMM_LEN];

    __u32 kstack_sz;
    __u32 ustack_sz;
    stack_trace_t kstack;
    stack_trace_t ustack;
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);
} events SEC(".maps");

/**
 * 获取当前进程的指定类型的 ID（如 pgrp、session 等）。如果无法获取，则返回 0。
 */
static __always_inline int current_pid_nr(enum pid_type type) {
    struct task_struct* task = bpf_get_current_task_btf();
    struct signal_struct* signal = BPF_CORE_READ(task, signal);
    if (!signal) {
        return 0;
    }

    struct pid* pid = BPF_CORE_READ(signal, pids[type]);
    if (!pid) {
        return 0;
    }

    unsigned int level = BPF_CORE_READ(pid, level);
    int nr = 0;
    bpf_core_read(&nr, sizeof(nr), &pid->numbers[level].nr);
    return nr;
}

/**
 * 根据 target_filter 的设置，判断当前进程是否匹配监视的目标 ID。
 */
static __always_inline bool match_target(__u32 tgid) {
    if (target_id == 0) {
        return true;
    }

    if (target_filter == FILTER_TGID) {
        return tgid == target_id;
    }
    if (target_filter == FILTER_PGRP) {
        return current_pid_nr(PIDTYPE_PGID) == target_id;
    }
    if (target_filter == FILTER_SESSION) {
        return current_pid_nr(PIDTYPE_SID) == target_id;
    }
    if (target_filter == FILTER_CGROUP) {
        return bpf_get_current_cgroup_id() == target_id;
    }

    return false;
}

SEC("perf_event")
int profile(void* ctx) {
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 tgid = pid_tgid >> 32;

    if (!match_target(tgid)) {
        return 0;
    }

    struct stack_trace_event* event =
        bpf_ringbuf_reserve(&events, sizeof(struct stack_trace_event), 0);
    if (!event) {
        return 1;
    }

    __u32 cpu_id = bpf_get_smp_processor_id();

    event->pid = tgid;
    event->cpu_id = cpu_id;
    event->timestamp = bpf_ktime_get_ns();

    // 获取当前进程名
    if (bpf_get_current_comm(event->comm, sizeof(event->comm))) {
        event->comm[0] = 0;
    }

    // 获取内核栈、用户栈信息
    event->kstack_sz =
        bpf_get_stack(ctx, event->kstack, sizeof(event->kstack), 0);
    event->ustack_sz = bpf_get_stack(
        ctx, event->ustack, sizeof(event->ustack), BPF_F_USER_STACK);

    bpf_ringbuf_submit(event, 0);

    return 0;
}
