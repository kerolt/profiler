#ifndef BLAZE_H_
#define BLAZE_H_

#include <cstdint>
#include <print>
#include <variant>

#include <blazesym.h>

#include "utils.h"

namespace blaze {

struct CodeInfo {
    const blaze_symbolize_code_info* info_;

    CodeInfo(const blaze_symbolize_code_info* info) : info_(info) {}
};

struct Syms {
    const blaze_syms* syms_;

    Syms(const blaze_syms* syms) : syms_(syms) {}

    ~Syms() {
        if (syms_) {
            blaze_syms_free(syms_);
        }
    }

    // 禁止拷贝，防止双重释放
    Syms(const Syms&) = delete;
    Syms& operator=(const Syms&) = delete;
};

using Source =
    std::variant<blaze_symbolize_src_process, blaze_symbolize_src_kernel>;

struct Input {
    const uint64_t* addrs_;
    size_t cnt_;
};

struct Symbolizer {
    blaze_symbolizer* symbolizer_;

    Symbolizer() { symbolizer_ = blaze_symbolizer_new(); }

    ~Symbolizer() { blaze_symbolizer_free(symbolizer_); }

    [[nodiscard]]
    auto symbolize(Source src, const Input& input) -> Result<Syms> {
        if (!input.addrs_ || input.cnt_ == 0) {
            return Err<>{"Empty input addresses"};
        }

        const blaze_syms* syms = nullptr;

        std::visit(
            Overloaded{
                [&](blaze_symbolize_src_kernel& kern_src) {
                    syms = blaze_symbolize_kernel_abs_addrs(
                        symbolizer_, &kern_src, input.addrs_, input.cnt_);
                },
                [&](blaze_symbolize_src_process& proc_src) {
                    syms = blaze_symbolize_process_abs_addrs(
                        symbolizer_, &proc_src, input.addrs_, input.cnt_);
                },
            },
            src);
        return syms
                   ? Result<Syms>{syms}
                   : Err<>{std::format("Symbolization failed, errno is: {}",
                                       static_cast<int16_t>(blaze_err_last()))};
    }
};

inline auto get_symbolize_source(uint32_t pid) -> blaze::Source {
    if (pid == 0) {
        blaze_symbolize_src_kernel src{
            .type_size = sizeof(src),
        };
        return blaze::Source{src};
    } else {
        blaze_symbolize_src_process src{
            .type_size = sizeof(src),
            .pid = pid,
        };
        return blaze::Source{src};
    }
}

}  // namespace blaze

#endif /* BLAZE_H_ */
