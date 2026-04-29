#pragma once

#include <type_traits>

namespace threadpool {
namespace detail {

#if (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L) || __cplusplus >= 201703L
template <typename F, typename... Args>
using invoke_result_t = std::invoke_result_t<F, Args...>;
#else
template <typename F, typename... Args>
using invoke_result_t = typename std::result_of<F(Args...)>::type;
#endif

} // namespace detail
} // namespace threadpool
