cmake_minimum_required(VERSION 3.16)
project(hermitdb CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Warnings apply to our targets only — third-party test deps (Catch2) build
# with their own flags so -Werror can't be broken by code we don't own.
add_library(hermit_warnings INTERFACE)
target_compile_options(hermit_warnings INTERFACE -Wall -Wextra -Werror)

# CP5 runs one reactor thread per --threads.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

option(HERMIT_TSAN "Build with ThreadSanitizer (CP5 CI job)" OFF)
if(HERMIT_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()

add_library(hermit_core
  src/util/config.cpp
  src/protocol/resp_parser.cpp
  src/protocol/resp_writer.cpp
  src/core/db.cpp
  src/core/expiry.cpp
  src/core/commands.cpp
  src/core/shard.cpp
  src/persist/wal.cpp
  src/persist/snapshot.cpp
)
target_include_directories(hermit_core PUBLIC src)
target_link_libraries(hermit_core PUBLIC hermit_warnings Threads::Threads)

add_library(hermit_net
  src/net/listener.cpp
  src/net/connection.cpp
  src/net/event_loop.cpp
)
target_link_libraries(hermit_net PUBLIC hermit_core)

add_executable(hermitdb src/main.cpp)
target_link_libraries(hermitdb PRIVATE hermit_net)

enable_testing()
add_subdirectory(tests)
