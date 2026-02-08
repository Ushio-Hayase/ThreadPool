cmake_minimum_required(VERSION 3.20)
project(thread_pool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

add_subdirectory(src)

enable_testing()

add_subdirectory(tests)
