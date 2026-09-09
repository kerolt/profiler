use std::mem::MaybeUninit;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use anyhow::{Context, Result};
use clap::{ArgAction, Parser, ValueEnum};
use libbpf_rs::skel::{OpenSkel, SkelBuilder};
use nix::unistd::{getpgid, getsid, Pid};
use tracing::{info, warn};
use tracing_subscriber::filter::LevelFilter;

mod event;
mod perf;
mod syscall;

mod profiler {
    include!(concat!(env!("OUT_DIR"), "/profiler.skel.rs"));
}

use event::{EventHandler, OutputFormat, ProcessingMode};
use profiler::*;

// 收到 SIGINT 后设置该标志，主循环会在下一次轮询结束后退出
static EXITING: AtomicBool = AtomicBool::new(false);

/// BPF 程序支持的进程过滤范围
#[derive(Clone, Copy, Debug, ValueEnum)]
#[value(rename_all = "lowercase")]
enum FilterMode {
    Tgid,
    #[value(alias = "pgid")]
    Pgrp,
    #[value(alias = "sid")]
    Session,
    Cgroup,
}

impl FilterMode {
    /// 将命令行枚举转换为 eBPF 程序使用的枚举值
    fn as_bpf_value(self) -> u32 {
        match self {
            Self::Tgid => 0,
            Self::Pgrp => 1,
            Self::Session => 2,
            Self::Cgroup => 3,
        }
    }
}

/// 命令行参数集合
#[derive(Debug, Parser)]
#[command(name = "profiler", about = "A simple profiler using eBPF")]
struct Args {
    /// 采样频率，单位为 Hz
    #[arg(short = 'f', long, default_value_t = 10)]
    freq: u64,

    /// 增加日志详细程度，可重复使用
    #[arg(short = 'v', long = "verbose", action = ArgAction::Count)]
    verbosity: u8,

    /// 使用软件 cpu-clock 事件代替硬件周期事件
    #[arg(long)]
    sw_event: bool,

    /// 按进程 ID 进行过滤
    #[arg(short = 'p', long)]
    pid: Option<libc::pid_t>,

    /// 指定 --pid 的过滤范围
    #[arg(long, value_enum, default_value_t = FilterMode::Session)]
    filter: FilterMode,

    /// 输出扩展折叠栈格式
    #[arg(short = 'E', long)]
    fold_extend: bool,

    /// 禁用符号化和逐样本输出
    #[arg(long)]
    no_symbolize: bool,
}

/// 初始化 BPF、perf event 和 ring buffer，然后持续处理采样事件
fn main() -> Result<()> {
    let args = Args::parse();
    init_logging(args.verbosity);
    install_signal_handler()?;

    // 采样频率至少为 1，过滤目标 ID 只在指定 PID 时解析
    let freq = args.freq.max(1);
    let target_id = resolve_target_id(args.pid, args.filter)?;
    // tgid 以外的过滤由 eBPF 完成，因此 perf event 需要覆盖所有进程
    let perf_pid = if target_id == 0 {
        args.pid.unwrap_or(-1)
    } else {
        -1
    };

    raise_memlock_limit();

    // 打开 skeleton 后先写入只读全局变量，再加载 BPF 程序
    let mut open_object = MaybeUninit::uninit();
    let skel_builder = ProfilerSkelBuilder::default();
    let open_skel = skel_builder
        .open(&mut open_object)
        .context("failed to open BPF object")?;
    open_skel.maps.rodata_data.target_id = target_id;
    open_skel.maps.rodata_data.target_filter = args.filter.as_bpf_value();
    let mut skel = open_skel.load().context("failed to load BPF object")?;

    // 每个可能的 CPU 创建一个 perf event，并将 BPF profile 程序附加到这些事件
    let perf_fds = perf::init_perf_monitor(freq, args.sw_event, perf_pid)?;
    let _links = perf::attach_perf_events(&perf_fds, &mut skel.progs.profile)?;

    let format = if args.fold_extend {
        OutputFormat::FoldExtend
    } else {
        OutputFormat::Standard
    };
    let mode = if args.no_symbolize {
        ProcessingMode::RawCount
    } else {
        ProcessingMode::Symbolize
    };
    let mut handler = EventHandler::new(format, mode);

    // RingBuffer 的回调借用 handler，因此将轮询过程限制在独立作用域内
    {
        let mut builder = libbpf_rs::RingBufferBuilder::new();
        builder
            .add(&skel.maps.events, |data| handler.handle(data))
            .context("failed to add ring buffer callback")?;
        let ring_buffer = builder.build().context("failed to build ring buffer")?;

        // 使用短超时让循环能够及时检查退出标志，同时持续消费 ring buffer
        while !EXITING.load(Ordering::Relaxed) {
            let result = ring_buffer.poll(Duration::from_millis(100));
            if let Err(error) = result {
                if !EXITING.load(Ordering::Relaxed) {
                    return Err(error).context("failed to poll ring buffer");
                }
                break;
            }
        }
    }

    handler.flush();
    perf::close_perf_events(perf_fds)?;
    if args.no_symbolize {
        info!(samples = handler.sample_count(), "sampling finished");
    }
    Ok(())
}

/// 根据 -v 的重复次数配置 tracing 日志级别
fn init_logging(verbosity: u8) {
    let level = match verbosity {
        0 => LevelFilter::WARN,
        1 => LevelFilter::INFO,
        2 => LevelFilter::DEBUG,
        _ => LevelFilter::TRACE,
    };
    tracing_subscriber::fmt()
        .with_max_level(level)
        .with_ansi(true)
        .init();
}

/// 安装 SIGINT 处理器，使 Ctrl+C 能够优雅地结束采样
fn install_signal_handler() -> Result<()> {
    // SAFETY: 处理器只执行原子写操作，该操作可以在信号上下文中安全执行
    let handler = handle_signal as *const () as libc::sighandler_t;
    let previous = unsafe { libc::signal(libc::SIGINT, handler) };
    if previous == libc::SIG_ERR {
        return Err(std::io::Error::last_os_error()).context("failed to install SIGINT handler");
    }
    Ok(())
}

/// 信号处理器只执行原子写操作，避免在信号上下文中调用非安全函数
extern "C" fn handle_signal(_: libc::c_int) {
    EXITING.store(true, Ordering::Relaxed);
}

/// 尝试提高 BPF 所需的内存锁定上限
fn raise_memlock_limit() {
    let limit = libc::rlimit {
        rlim_cur: libc::RLIM_INFINITY,
        rlim_max: libc::RLIM_INFINITY,
    };
    // 某些系统会因进程缺少能力而拒绝修改上限；如果仍然需要，
    // libbpf 会在加载 BPF 时给出具体错误
    if unsafe { libc::setrlimit(libc::RLIMIT_MEMLOCK, &limit) } != 0 {
        warn!(error = ?std::io::Error::last_os_error(), "failed to raise RLIMIT_MEMLOCK");
    }
}

/// 将用户提供的 PID 转换为 eBPF 过滤所需的目标 ID
fn resolve_target_id(pid: Option<libc::pid_t>, mode: FilterMode) -> Result<u64> {
    let Some(pid) = pid else {
        return Ok(0);
    };
    if pid <= 0 {
        return Ok(0);
    }

    match mode {
        FilterMode::Tgid => Ok(pid as u64),
        FilterMode::Pgrp => Ok(getpgid(Some(Pid::from_raw(pid)))?.as_raw() as u64),
        FilterMode::Session => Ok(getsid(Some(Pid::from_raw(pid)))?.as_raw() as u64),
        FilterMode::Cgroup => cgroup_id_for_pid(pid),
    }
}

/// 读取目标进程所在 cgroup v2 目录的 inode 作为 cgroup ID
fn cgroup_id_for_pid(pid: libc::pid_t) -> Result<u64> {
    let content = std::fs::read_to_string(format!("/proc/{pid}/cgroup"))?;
    let relative_path = content
        .lines()
        .find_map(|line| line.strip_prefix("0::"))
        .context("failed to find cgroup v2 entry")?;
    let metadata = std::fs::metadata(format!("/sys/fs/cgroup{relative_path}"))?;
    Ok(std::os::unix::fs::MetadataExt::ino(&metadata))
}
