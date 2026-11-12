cmake_minimum_required(VERSION 3.20)
project(course_work_parallel_computing CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(search_server
    src/main.cpp
    src/thread_pool.cpp
    src/inverted_index.cpp
    src/dataset_indexer.cpp
)

target_include_directories(search_server PRIVATE src)
target_link_libraries(search_server PRIVATE Threads::Threads)