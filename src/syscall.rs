use std::mem;

/// perf_event_attr 中根据事件类型复用的 sample_period/sample_freq 字段
#[repr(C)]
pub union Sample {
    pub sample_period: u64,
    pub sample_freq: u64,
}

/// perf_event_attr 中复用的唤醒阈值字段
#[repr(C)]
pub union Wakeup {
    pub wakeup_events: u32,
    pub wakeup_watermark: u32,
}

/// perf_event_attr 中复用的断点地址及扩展配置字段
#[repr(C)]
pub union BreakpointAddress {
    pub bp_addr: u64,
    pub kprobe_func: u64,
    pub uprobe_path: u64,
    pub config1: u64,
}

/// perf_event_attr 中复用的断点长度及扩展配置字段
#[repr(C)]
pub union BreakpointLength {
    pub bp_len: u64,
    pub kprobe_addr: u64,
    pub probe_offset: u64,
    pub config2: u64,
}

/// Linux perf_event_open 系统调用使用的 C ABI 属性结构体
#[repr(C)]
pub struct PerfEventAttr {
    pub event_type: u32,
    pub size: u32,
    pub config: u64,
    pub sample: Sample,
    pub sample_type: u64,
    pub read_format: u64,
    pub flags: u64,
    pub wakeup: Wakeup,
    pub bp_type: u32,
    pub bp_addr: BreakpointAddress,
    pub bp_len: BreakpointLength,
    pub branch_sample_type: u64,
    pub sample_regs_user: u64,
    pub sample_stack_user: u32,
    pub clockid: i32,
    pub sample_regs_intr: u64,
    pub aux_watermark: u32,
    pub sample_max_stack: u16,
    pub reserved_2: u16,
    pub aux_sample_size: u32,
    pub reserved_3: u32,
}

impl Default for PerfEventAttr {
    fn default() -> Self {
        // perf_event_attr 是由内核读取的 ABI 结构体，按零值初始化
        unsafe { mem::zeroed() }
    }
}

// 以下常量对应 Linux 内核 include/uapi/linux/perf_event.h 中的定义
pub const PERF_TYPE_HARDWARE: u32 = 0;
pub const PERF_TYPE_SOFTWARE: u32 = 1;
pub const PERF_COUNT_HW_CPU_CYCLES: u64 = 0;
pub const PERF_COUNT_SW_CPU_CLOCK: u64 = 0;
pub const PERF_ATTR_FLAG_FREQ: u64 = 1 << 10;

/// 调用 Linux perf_event_open 系统调用创建 perf event
pub fn perf_event_open(
    attr: &PerfEventAttr,
    pid: libc::pid_t,
    cpu: libc::c_int,
    group_fd: libc::c_int,
    flags: libc::c_ulong,
) -> libc::c_long {
    // SAFETY: 参数符合 Linux perf_event_open 系统调用 ABI，且 attr
    // 在系统调用执行期间保持有效
    unsafe {
        libc::syscall(
            libc::SYS_perf_event_open,
            attr as *const PerfEventAttr,
            pid,
            cpu,
            group_fd,
            flags,
        )
    }
}
