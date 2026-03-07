cmake_minimum_required(VERSION 3.20)
project(ipc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(ipc INTERFACE)
target_include_directories(ipc INTERFACE 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(UNIX AND NOT APPLE)
    target_link_libraries(ipc INTERFACE rt)
endif()

option(IPC_SANITIZERS "Enable ASan + UBSan" OFF)
if(IPC_SANITIZERS)
    target_compile_options(ipc INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(ipc INTERFACE -fsanitize=address,undefined)
endif()

add_compile_options(-Wall -Wpedantic -Wextra -Werror)

option(IPC_BUILD_TESTS "Build Tests" ON) 
if (IPC_BUILD_TESTS) 
    enable_testing()
    add_subdirectory(tests)
endif()

option(IPC_BUILD_BENCH "Build benchmarks" ON)
if (IPC_BUILD_BENCH)
    add_subdirectory(benchmarks)
endif()
