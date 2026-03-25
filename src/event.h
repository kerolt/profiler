#ifndef EVENT_H_
#define EVENT_H_

#include <sys/sysinfo.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "blaze.h"

#include <blazesym.h>

constexpr size_t MAX_STACK_DEPTH = 128;
constexpr size_t TASK_COMM_LEN = 16;
constexpr int ADDR_WIDTH = 16;

struct StacktraceEvent {
    uint32_t pid;
    uint32_t cpu_id;
    uint64_t timestamp;
    char comm[TASK_COMM_LEN];
    int32_t kstack_size;
    int32_t ustack_size;
    uint64_t kstack[MAX_STACK_DEPTH];
    uint64_t ustack[MAX_STACK_DEPTH];
};

enum class OutputFormat : uint8_t { Standard, FoldExtend };
enum class ProcessingMode : uint8_t { Symbolize, RawCount };

class EventHandler {
public:
    EventHandler(OutputFormat fmt,
                 ProcessingMode mode = ProcessingMode::Symbolize)
        : format(fmt), mode_(mode) {
        boot_time_ns = get_boot_time_ns();
    }

    ~EventHandler() = default;

    auto handle(const uint8_t* data, size_t len) -> int;
    [[nodiscard]] auto sample_count() const -> uint64_t {
        return sample_count_;
    }

    void show_stack_trace(const uint64_t* stack, uint32_t size, uint32_t pid);

private:
    blaze::Symbolizer symbolizer_;
    OutputFormat format;
    ProcessingMode mode_;
    uint64_t sample_count_{0};
    uint64_t boot_time_ns;

    static auto get_boot_time_ns() -> uint64_t;

    // 符号化堆栈并返回字符串向量
    auto symbolize_stack_to_vec(const uint64_t* stack, uint32_t stack_sz,
                                uint32_t pid) -> std::vector<std::string>;

    void handle_standard(const StacktraceEvent* event);

    void handle_fold_extend(const StacktraceEvent* event);
};

#endif /* EVENT_H_ */
