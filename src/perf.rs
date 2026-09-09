use std::io;
use std::mem;

use anyhow::{Context, Result};
use libbpf_rs::{Link, ProgramMut};

use crate::syscall::{
    perf_event_open, PerfEventAttr, PERF_ATTR_FLAG_FREQ, PERF_COUNT_HW_CPU_CYCLES,
    PERF_COUNT_SW_CPU_CLOCK, PERF_TYPE_HARDWARE, PERF_TYPE_SOFTWARE,
};

/// 在每个可能的 CPU 上创建一个按频率触发的 perf event
pub fn init_perf_monitor(freq: u64, sw_event: bool, pid: libc::pid_t) -> Result<Vec<i32>> {
    // 使用可能 CPU 数量而不是当前在线 CPU 数量，与 libbpf 的 CPU 索引保持一致
    let cpu_count = libbpf_rs::num_possible_cpus().context("failed to get possible CPU count")?;
    let mut attr = PerfEventAttr {
        event_type: if sw_event {
            PERF_TYPE_SOFTWARE
        } else {
            PERF_TYPE_HARDWARE
        },
        size: mem::size_of::<PerfEventAttr>() as u32,
        config: if sw_event {
            PERF_COUNT_SW_CPU_CLOCK
        } else {
            PERF_COUNT_HW_CPU_CYCLES
        },
        flags: PERF_ATTR_FLAG_FREQ,
        ..Default::default()
    };

    attr.sample.sample_freq = freq;

    // 为每个 CPU 创建一个 perf event，并返回文件描述符列表
    let mut fds = Vec::with_capacity(cpu_count);
    for cpu in 0..cpu_count {
        let fd = perf_event_open(&attr, pid, cpu as libc::c_int, -1, 0);
        if fd < 0 {
            // 初始化中途失败时关闭已经创建的文件描述符，避免资源泄漏
            let error = io::Error::last_os_error();
            for opened_fd in fds {
                // 初始化部分 CPU 失败时尽力清理已经打开的文件描述符
                unsafe { libc::close(opened_fd) };
            }
            return Err(error).with_context(|| format!("failed to open perf event on CPU {cpu}"));
        }
        fds.push(fd as i32);
    }

    Ok(fds)
}

/// 将 BPF profile 程序附加到所有 perf event，并保留 Link 的生命周期
pub fn attach_perf_events(fds: &[i32], program: &mut ProgramMut) -> Result<Vec<Link>> {
    fds.iter()
        .map(|fd| {
            program
                .attach_perf_event(*fd)
                .context("failed to attach eBPF program to perf event")
        })
        .collect()
}

/// 关闭所有 perf event 文件描述符
pub fn close_perf_events(fds: Vec<i32>) -> Result<()> {
    for fd in fds {
        let result = unsafe { libc::close(fd) };
        if result != 0 {
            return Err(io::Error::last_os_error()).context("failed to close perf event");
        }
    }
    Ok(())
}
