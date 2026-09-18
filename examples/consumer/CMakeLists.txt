cmake_minimum_required(VERSION 3.20)

# An independent downstream consumer. It is deliberately NOT part of the Rate
# Governor build: it is configured separately against an installed prefix, so it
# proves that the exported package is usable on its own.
project(RateGovernorConsumer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(RateGovernor 1.0 CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE RateGovernor::rate_governor)

if(MSVC)
  target_compile_options(consumer PRIVATE /W4 /WX /permissive-)
else()
  target_compile_options(consumer PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME consumer_envelope_smoke COMMAND consumer)
