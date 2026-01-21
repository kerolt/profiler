#ifndef UTILS_H_
#define UTILS_H_

#include <expected>
#include <optional>
#include <string>

template <typename T>
using Option = std::optional<T>;

template <typename... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

template <typename... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

template <typename E = std::string>
using Err = std::unexpected<E>;

template <typename T, typename E = std::string>
using Result = std::expected<T, E>;

#endif /* UTILS_H_ */
