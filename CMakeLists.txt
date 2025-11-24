cmake_minimum_required(VERSION 3.13)
project(zephyr_audio_toolkit C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
