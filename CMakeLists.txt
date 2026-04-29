cmake_minimum_required(VERSION 3.14)

project(ThreadPoolExample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

add_library(threadpool INTERFACE)
target_include_directories(threadpool INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(threadpool_demo main.cpp)
target_link_libraries(threadpool_demo PRIVATE threadpool)

add_executable(threadpool_tests test.cpp)
target_link_libraries(threadpool_tests PRIVATE threadpool)

add_executable(threadpool_coroutine_tests test_coroutine.cpp)
target_compile_features(threadpool_coroutine_tests PRIVATE cxx_std_20)
target_link_libraries(threadpool_coroutine_tests PRIVATE threadpool)

add_executable(threadpool_stress stress_test.cpp)
target_link_libraries(threadpool_stress PRIVATE threadpool)

add_executable(threadpool_benchmark benchmark.cpp)
target_link_libraries(threadpool_benchmark PRIVATE threadpool)

enable_testing()
add_test(NAME threadpool_tests COMMAND threadpool_tests)
add_test(NAME threadpool_coroutine_tests COMMAND threadpool_coroutine_tests)
add_test(NAME threadpool_stress COMMAND threadpool_stress 5000 4 4)
