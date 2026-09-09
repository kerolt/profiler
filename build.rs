use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use libbpf_cargo::SkeletonBuilder;

// C 语言 eBPF 程序的源文件路径
const BPF_SOURCE: &str = "bpf/profiler.bpf.c";

/// 从当前运行内核的 BTF 信息生成 CO-RE 所需的 vmlinux.h
fn generate_vmlinux_header(path: &Path) {
    // libbpf-cargo 编译 BPF 程序时需要能够找到内核类型定义
    let output = Command::new("bpftool")
        .args([
            "btf",
            "dump",
            "file",
            "/sys/kernel/btf/vmlinux",
            "format",
            "c",
        ])
        .output()
        .expect("failed to execute bpftool");

    // bpftool 失败通常表示系统缺少 BTF、bpftool 或访问权限不足
    if !output.status.success() {
        panic!(
            "bpftool failed to generate vmlinux.h: {}",
            String::from_utf8_lossy(&output.stderr)
        );
    }

    fs::write(path, output.stdout).expect("failed to write generated vmlinux.h");
}

fn main() {
    // OUT_DIR 由 Cargo 提供，用于存放生成的临时文件，避免污染源码目录
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR must be set"));
    let vmlinux = out_dir.join("vmlinux.h");
    generate_vmlinux_header(&vmlinux);

    let skeleton = out_dir.join("profiler.skel.rs");
    SkeletonBuilder::new()
        .source(BPF_SOURCE)
        .clang_args(["-I", out_dir.to_str().expect("OUT_DIR is not valid UTF-8")])
        .build_and_generate(&skeleton)
        .expect("failed to compile eBPF program");

    println!("cargo:rerun-if-changed={BPF_SOURCE}");
}
