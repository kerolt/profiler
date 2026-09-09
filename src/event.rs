use std::collections::HashMap;
use std::mem;
use std::ptr;
use std::time::{SystemTime, UNIX_EPOCH};

use blazesym::symbolize;
use tracing::warn;

// 这些常量必须与 bpf/profiler.bpf.c 中的定义保持一致
pub const MAX_STACK_DEPTH: usize = 128;
pub const TASK_COMM_LEN: usize = 16;
const ADDR_WIDTH: usize = 16;

/// eBPF 写入 ring buffer、用户态读取的采样事件
///
/// repr(C) 和字段顺序用于保证 Rust 与 C 之间的二进制布局一致
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StacktraceEvent {
    pub pid: u32,
    pub cpu_id: u32,
    pub timestamp: u64,
    pub comm: [u8; TASK_COMM_LEN],
    pub kstack_size: i32,
    pub ustack_size: i32,
    pub kstack: [u64; MAX_STACK_DEPTH],
    pub ustack: [u64; MAX_STACK_DEPTH],
}

/// 采样结果的输出格式
#[derive(Clone, Copy)]
pub enum OutputFormat {
    Standard,
    FoldExtend,
}

/// 事件处理模式：完整符号化输出或只统计样本数量
#[derive(Clone, Copy)]
pub enum ProcessingMode {
    Symbolize,
    RawCount,
}

/// 折叠栈缓存项，保存栈文本和出现次数
struct FoldedStack {
    line: String,
    count: u64,
}

/// 负责校验、符号化、聚合并输出采样事件
pub struct EventHandler {
    symbolizer: symbolize::Symbolizer,
    format: OutputFormat,
    mode: ProcessingMode,
    sample_count: u64,
    boot_time_ns: u64,
    folded_stacks: HashMap<Vec<u8>, FoldedStack>,
}

impl EventHandler {
    /// 创建事件处理器，并记录系统启动时间用于时间戳转换
    pub fn new(format: OutputFormat, mode: ProcessingMode) -> Self {
        Self {
            symbolizer: symbolize::Symbolizer::new(),
            format,
            mode,
            sample_count: 0,
            boot_time_ns: get_boot_time_ns(),
            folded_stacks: HashMap::new(),
        }
    }

    /// 返回已接受的有效样本数
    pub fn sample_count(&self) -> u64 {
        self.sample_count
    }

    /// 处理一条 ring buffer 记录，返回 libbpf ring buffer 回调结果
    pub fn handle(&mut self, data: &[u8]) -> i32 {
        if data.len() != mem::size_of::<StacktraceEvent>() {
            warn!(
                expected = mem::size_of::<StacktraceEvent>(),
                actual = data.len(),
                "invalid event size"
            );
            return 1;
        }

        // 内核产生的 ring buffer 记录不一定满足 Rust 类型的对齐要求，
        // 因此使用非对齐读取避免直接解引用字节切片
        let event = unsafe { ptr::read_unaligned(data.as_ptr() as *const StacktraceEvent) };
        if event.kstack_size <= 0 && event.ustack_size <= 0 {
            return 1;
        }

        self.sample_count += 1;
        if matches!(self.mode, ProcessingMode::RawCount) {
            return 0;
        }

        match self.format {
            OutputFormat::Standard => self.handle_standard(&event),
            OutputFormat::FoldExtend => self.handle_fold_extend(&event),
        }
        0
    }

    /// 输出尚未打印的折叠栈聚合结果
    pub fn flush(&self) {
        if !matches!(self.format, OutputFormat::FoldExtend)
            || matches!(self.mode, ProcessingMode::RawCount)
        {
            return;
        }

        for folded in self.folded_stacks.values() {
            println!("{} {}", folded.line, folded.count);
        }
    }

    /// 输出单个事件的详细信息和两类堆栈
    fn handle_standard(&self, event: &StacktraceEvent) {
        let unix_ns = event.timestamp + self.boot_time_ns;
        println!(
            "[{}.{:09} COMM: {} (pid={}) @ CPU {}]",
            unix_ns / 1_000_000_000,
            unix_ns % 1_000_000_000,
            comm_string(&event.comm),
            event.pid,
            event.cpu_id
        );

        if event.kstack_size > 0 {
            println!("Kernel:");
            show_stack_trace(
                &self.symbolizer,
                stack_slice(&event.kstack, event.kstack_size),
                0,
            );
        } else {
            println!("Kernel: <no stack>");
        }

        if event.ustack_size > 0 {
            println!("Userspace:");
            show_stack_trace(
                &self.symbolizer,
                stack_slice(&event.ustack, event.ustack_size),
                event.pid,
            );
        } else {
            println!("Userspace: <no stack>");
        }
        println!();
    }

    /// 将用户态和内核态堆栈转换为折叠格式，并合并重复栈
    fn handle_fold_extend(&mut self, event: &StacktraceEvent) {
        let key = folded_key(event);
        if let Some(folded) = self.folded_stacks.get_mut(&key) {
            folded.count += 1;
            return;
        }

        let mut frames = vec![format!("{}-{}", comm_string(&event.comm), event.pid)];
        if event.ustack_size > 0 {
            let user_frames = symbolize_stack_to_vec(
                &self.symbolizer,
                stack_slice(&event.ustack, event.ustack_size),
                event.pid,
            );
            frames.extend(user_frames.into_iter().rev());
        }
        if event.kstack_size > 0 {
            let kernel_frames = symbolize_stack_to_vec(
                &self.symbolizer,
                stack_slice(&event.kstack, event.kstack_size),
                0,
            );
            frames.extend(
                kernel_frames
                    .into_iter()
                    .rev()
                    .map(|frame| format!("{frame}_[k]")),
            );
        }

        self.folded_stacks.insert(
            key,
            FoldedStack {
                line: frames.join(";"),
                count: 1,
            },
        );
    }
}

/// 计算 Unix 时间起点到系统启动时刻的纳秒数
fn get_boot_time_ns() -> u64 {
    let now_ns = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("system time is before the Unix epoch")
        .as_nanos() as u64;

    let mut info = unsafe { mem::zeroed::<libc::sysinfo>() };
    let uptime_ns = if unsafe { libc::sysinfo(&mut info) } == 0 {
        (info.uptime as u64).saturating_mul(1_000_000_000)
    } else {
        0
    };
    now_ns.saturating_sub(uptime_ns)
}

/// 将内核提供的定长进程名转换为字符串并去除末尾零字节
fn comm_string(comm: &[u8; TASK_COMM_LEN]) -> String {
    let end = comm
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(comm.len());
    String::from_utf8_lossy(&comm[..end]).into_owned()
}

/// 根据字节数截取有效栈帧，并限制在数组容量内
fn stack_slice(stack: &[u64; MAX_STACK_DEPTH], size: i32) -> &[u64] {
    if size <= 0 {
        return &[];
    }
    let frame_count = (size as usize / mem::size_of::<u64>()).min(MAX_STACK_DEPTH);
    &stack[..frame_count]
}

/// 使用进程信息和原始栈地址生成折叠栈缓存键
fn folded_key(event: &StacktraceEvent) -> Vec<u8> {
    let kstack_bytes = stack_byte_count(event.kstack_size);
    let ustack_bytes = stack_byte_count(event.ustack_size);
    let mut key = Vec::with_capacity(
        mem::size_of::<u32>()
            + TASK_COMM_LEN
            + mem::size_of::<i32>() * 2
            + kstack_bytes
            + ustack_bytes,
    );
    key.extend_from_slice(&event.pid.to_ne_bytes());
    key.extend_from_slice(&event.comm);
    key.extend_from_slice(&event.kstack_size.to_ne_bytes());
    key.extend_from_slice(&event.ustack_size.to_ne_bytes());
    key.extend_from_slice(bytes_of_u64_slice(&event.kstack, kstack_bytes));
    key.extend_from_slice(bytes_of_u64_slice(&event.ustack, ustack_bytes));
    key
}

/// 将内核返回的栈字节数转换为安全的数组范围
fn stack_byte_count(size: i32) -> usize {
    if size > 0 {
        (size as usize).min(MAX_STACK_DEPTH * mem::size_of::<u64>())
    } else {
        0
    }
}

/// 将栈地址数组按原始字节形式读取，用于生成缓存键
fn bytes_of_u64_slice(values: &[u64; MAX_STACK_DEPTH], bytes: usize) -> &[u8] {
    // SAFETY: 读取范围由调用方限制在数组大小以内，这里只读取已有对象的字节表示
    unsafe { std::slice::from_raw_parts(values.as_ptr() as *const u8, bytes) }
}

/// 符号化堆栈，并提取折叠格式所需的函数名
fn symbolize_stack_to_vec(
    symbolizer: &symbolize::Symbolizer,
    stack: &[u64],
    pid: u32,
) -> Vec<String> {
    let addresses: Vec<blazesym::Addr> = stack
        .iter()
        .map(|address| *address as blazesym::Addr)
        .collect();
    let source = symbolize_source(pid);
    let symbols = match symbolizer.symbolize(&source, symbolize::Input::AbsAddr(&addresses)) {
        Ok(symbols) => symbols,
        Err(_) => {
            return stack
                .iter()
                .map(|address| format!("{address:#x}"))
                .collect()
        }
    };

    stack
        .iter()
        .zip(symbols)
        .map(|(address, symbol)| match symbol {
            symbolize::Symbolized::Sym(sym) => sym.name.to_string(),
            symbolize::Symbolized::Unknown(_) => format!("{address:#x}"),
        })
        .collect()
}

/// 符号化并打印标准格式中的单个堆栈
fn show_stack_trace(symbolizer: &symbolize::Symbolizer, stack: &[u64], pid: u32) {
    let addresses: Vec<blazesym::Addr> = stack
        .iter()
        .map(|address| *address as blazesym::Addr)
        .collect();
    let source = symbolize_source(pid);
    let symbols = match symbolizer.symbolize(&source, symbolize::Input::AbsAddr(&addresses)) {
        Ok(symbols) => symbols,
        Err(error) => {
            eprintln!("  Failed to symbolize stack trace. err: {error:#}");
            return;
        }
    };

    for (input_addr, symbol) in addresses.iter().zip(symbols) {
        match symbol {
            symbolize::Symbolized::Sym(sym) => {
                print_frame(*input_addr, &sym);
                for inline in &sym.inlined {
                    print_inline_frame(inline);
                }
            }
            symbolize::Symbolized::Unknown(_) => {
                println!("0x{input_addr:0width$x}: <no-symbol>", width = ADDR_WIDTH);
            }
        }
    }
}

/// 根据 PID 选择用户进程或内核符号源，PID 0 表示内核
fn symbolize_source(pid: u32) -> symbolize::source::Source<'static> {
    if pid == 0 {
        symbolize::source::Source::from(symbolize::source::Kernel::default())
    } else {
        symbolize::source::Source::from(symbolize::source::Process::new(pid.into()))
    }
}

/// 打印普通符号帧，包括地址、符号偏移和源码位置
fn print_frame(input_addr: blazesym::Addr, symbol: &symbolize::Sym) {
    println!(
        "0x{input_addr:0width$x}: {} @ {:#x} + {:#x}{}",
        symbol.name,
        symbol.addr,
        symbol.offset,
        format_code_info(symbol.code_info.as_deref()),
        width = ADDR_WIDTH
    );
}

/// 打印符号化结果中的内联函数帧
fn print_inline_frame(frame: &symbolize::InlinedFn) {
    println!(
        "{:>width$} {}{} [inlined]",
        "",
        frame.name,
        format_code_info(frame.code_info.as_ref()),
        width = ADDR_WIDTH
    );
}

/// 将 blazesym 提供的源码目录、文件、行列号格式化为文本
fn format_code_info(code_info: Option<&symbolize::CodeInfo>) -> String {
    let Some(code_info) = code_info else {
        return String::new();
    };
    let path = code_info.to_path().display().to_string();
    match (code_info.line, code_info.column) {
        (Some(line), Some(column)) => format!(" {path}:{line}:{column}"),
        (Some(line), None) => format!(" {path}:{line}"),
        (None, _) => format!(" {path}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn event_bytes(event: &StacktraceEvent) -> &[u8] {
        // SAFETY: event 已初始化且采用 repr(C)，切片范围正好覆盖整个事件
        unsafe {
            std::slice::from_raw_parts(
                event as *const StacktraceEvent as *const u8,
                mem::size_of::<StacktraceEvent>(),
            )
        }
    }

    #[test]
    fn bpf_event_layout_is_stable() {
        assert_eq!(mem::size_of::<StacktraceEvent>(), 2088);
    }

    #[test]
    fn raw_mode_counts_valid_samples() {
        let mut event = StacktraceEvent {
            pid: 123,
            cpu_id: 2,
            timestamp: 1,
            comm: [0; TASK_COMM_LEN],
            kstack_size: 8,
            ustack_size: 0,
            kstack: [0; MAX_STACK_DEPTH],
            ustack: [0; MAX_STACK_DEPTH],
        };
        event.comm[..4].copy_from_slice(b"test");

        let mut handler = EventHandler::new(OutputFormat::Standard, ProcessingMode::RawCount);
        assert_eq!(handler.handle(event_bytes(&event)), 0);
        assert_eq!(handler.sample_count(), 1);
    }

    #[test]
    fn invalid_event_size_is_rejected() {
        let mut handler = EventHandler::new(OutputFormat::Standard, ProcessingMode::RawCount);
        assert_eq!(handler.handle(&[0; 4]), 1);
        assert_eq!(handler.sample_count(), 0);
    }
}
