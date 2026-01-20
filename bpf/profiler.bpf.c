#include "vmlinux.h"

#include <bpf/bpf_helpers.h>

char LICENSE[] SEC("license") = "Dual BSD/GPL";

#ifndef TASK_COMM_LEN
#define TASK_COMM_LEN 16
#endif

#ifndef MAX_STACK_DEPTH
#define MAX_STACK_DEPTH 128
#endif

typedef __u64 stack_trace_t[MAX_STACK_DEPTH];

struct stack_trace_event {
    __u32 pid;
    __u32 cpu_id;
    __u64 timestamp;

    char comm[TASK_COMM_LEN];

    __s32 kstack_sz;
    __s32 ustack_sz;
    stack_trace_t kstack;
    stack_trace_t ustack;
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);
} events SEC(".maps");

SEC("perf_event")
int profile(void* ctx) {
    struct stack_trace_event* event = bpf_ringbuf_reserve(&events, sizeof(struct stack_trace_event), 0);
    if (!event) {
        return 1;
    }

    int pid = bpf_get_current_pid_tgid() >> 32;
    int cpu_id = bpf_get_smp_processor_id();

    event->pid = pid;
    event->cpu_id = cpu_id;
    event->timestamp = bpf_ktime_get_ns();

    // 获取当前进程名
    if (bpf_get_current_comm(event->comm, sizeof(event->comm))) {
        event->comm[0] = 0;
    }

    // 获取内核栈、用户栈信息
    event->kstack_sz = bpf_get_stack(ctx, event->kstack, sizeof(event->kstack), 0);
    event->ustack_sz = bpf_get_stack(ctx, event->ustack, sizeof(event->ustack), BPF_F_USER_STACK);

    bpf_ringbuf_submit(event, 0);

    return 0;
}