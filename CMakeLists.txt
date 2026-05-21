cmake_minimum_required(VERSION 3.14)

project(ThreadPoolExample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

add_library(threadpool INTERFACE)
target_include_directories(threadpool INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

add_executable(threadpool_demo examples/main.cpp)
target_link_libraries(threadpool_demo PRIVATE threadpool)

add_executable(threadpool_tests tests/test.cpp)
target_link_libraries(threadpool_tests PRIVATE threadpool)

add_executable(threadpool_coroutine_tests tests/test_coroutine.cpp)
target_compile_features(threadpool_coroutine_tests PRIVATE cxx_std_20)
target_link_libraries(threadpool_coroutine_tests PRIVATE threadpool)

add_executable(threadpool_stress tests/stress_test.cpp)
target_link_libraries(threadpool_stress PRIVATE threadpool)

add_executable(threadpool_benchmark tests/benchmark.cpp)
target_link_libraries(threadpool_benchmark PRIVATE threadpool)

add_executable(threadpool_mode_compare tests/mode_compare.cpp)
target_compile_features(threadpool_mode_compare PRIVATE cxx_std_20)
target_link_libraries(threadpool_mode_compare PRIVATE threadpool)

add_executable(threadpool_long_run tests/long_run_stress.cpp)
target_compile_features(threadpool_long_run PRIVATE cxx_std_20)
target_link_libraries(threadpool_long_run PRIVATE threadpool)

enable_testing()
add_test(NAME threadpool_tests COMMAND threadpool_tests)
add_test(NAME threadpool_coroutine_tests COMMAND threadpool_coroutine_tests)
add_test(NAME threadpool_stress COMMAND threadpool_stress 5000 4 4)

install(TARGETS threadpool EXPORT threadpool-targets)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT threadpool-targets DESTINATION lib/cmake/threadpool)
