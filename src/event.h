#ifndef EVENT_H_
#define EVENT_H_

#include <sys/sysinfo.h>

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

enum class OutputFormat { Standard, FoldExtend };

class EventHandler {
public:
    EventHandler(OutputFormat fmt) : format(fmt) {
        boot_time_ns = get_boot_time_ns();
    }

    ~EventHandler() = default;

    auto handle(const uint8_t* data, size_t len) -> int;

    void show_stack_trace(const uint64_t* stack, int32_t size, uint32_t pid);

private:
    blaze::Symbolizer symbolizer_;
    OutputFormat format;
    uint64_t boot_time_ns;

    auto get_boot_time_ns() -> uint64_t;

    // 符号化堆栈并返回字符串向量
    auto symbolize_stack_to_vec(const uint64_t* stack, int32_t size,
                                uint32_t pid) -> std::vector<std::string>;

    void handle_standard(const StacktraceEvent* event);

    void handle_fold_extend(const StacktraceEvent* event);
};

#endif /* EVENT_H_ */
