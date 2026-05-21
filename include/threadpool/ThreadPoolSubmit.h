#pragma once

#include <future>
#include <memory>
#include <tuple>
#include <utility>

namespace threadpool {
namespace detail {

#if !((defined(_MSVC_LANG) && _MSVC_LANG >= 201703L) || __cplusplus >= 201703L)
template <typename ReturnType, typename F, typename Tuple, std::size_t... I>
ReturnType apply_impl(F &&f, Tuple &&tuple, std::index_sequence<I...>)
{
    return std::forward<F>(f)(std::get<I>(std::forward<Tuple>(tuple))...);
}
#endif

} // namespace detail

template <typename F, typename... Args>
auto ThreadPool::submit(F &&f, Args &&...args) -> std::future<detail::invoke_result_t<F, Args...>>
{
    using ReturnType = detail::invoke_result_t<F, Args...>;
    auto packaged = std::make_shared<std::packaged_task<ReturnType()>>(
        [func = std::forward<F>(f), tuple_args = std::make_tuple(std::forward<Args>(args)...)]() mutable -> ReturnType {
#if (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L) || __cplusplus >= 201703L
            return std::apply(std::move(func), std::move(tuple_args));
#else
            return detail::apply_impl<ReturnType>(
                std::move(func),
                std::move(tuple_args),
                std::index_sequence_for<Args...>{}
            );
#endif
        }
    );

    auto future = packaged->get_future();

    runtime_.submit_task([packaged] {
        (*packaged)();
    });

    return future;
}

template <typename F, typename... Args>
auto ThreadPool::submit_with_stop(StopToken token, F &&f, Args &&...args)
    -> std::future<detail::invoke_result_t<F, StopToken, Args...>>
{
    return submit(
        std::forward<F>(f),
        std::move(token),
        std::forward<Args>(args)...
    );
}

} // namespace threadpool
