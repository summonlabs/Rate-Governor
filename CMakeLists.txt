cmake_minimum_required(VERSION 3.20)

project(RateGovernor
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral C++20 runtime for generation-bound enforcement of explicit network rate envelopes"
  HOMEPAGE_URL "https://github.com/summonlabs/Rate-Governor"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(RATE_GOVERNOR_BUILD_TESTS "Build the Rate Governor test and validation suite" ON)
option(RATE_GOVERNOR_BUILD_TOOLS "Build the Rate Governor command line tools" ON)
option(RATE_GOVERNOR_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(RATE_GOVERNOR_ENABLE_ANALYZE "Run the MSVC static analyzer over first-party code" OFF)
option(RATE_GOVERNOR_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# ---------------------------------------------------------------------------
# Warning and hardening profile (strict on first-party targets only).
# ---------------------------------------------------------------------------
function(rate_governor_strict_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc /GR /bigobj)
    if(RATE_GOVERNOR_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(RATE_GOVERNOR_ENABLE_ASAN)
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
      target_link_options(${target} PRIVATE /INCREMENTAL:NO)
    endif()
    if(RATE_GOVERNOR_ENABLE_ANALYZE)
      target_compile_options(${target} PRIVATE /analyze /analyze:WX-)
    endif()
    target_compile_definitions(${target} PRIVATE
      _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(RATE_GOVERNOR_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(RATE_GOVERNOR_ENABLE_ASAN)
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(rate_governor STATIC
  src/ids.cpp
  src/reason.cpp
  src/codec.cpp
  src/bucket.cpp
  src/entities.cpp
  src/backend.cpp
  src/plan.cpp
  src/record_codec.cpp
  src/journal.cpp
  src/engine.cpp
  src/engine_authority.cpp
  src/engine_lifecycle.cpp
  src/engine_runtime.cpp
  src/recovery.cpp
  src/synthetic_backend.cpp
  src/wire.cpp
  src/ipc.cpp
  src/worker_session.cpp
  src/remote_backend.cpp
)
add_library(RateGovernor::rate_governor ALIAS rate_governor)

target_include_directories(rate_governor PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

set_target_properties(rate_governor PROPERTIES
  OUTPUT_NAME rate_governor
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

rate_governor_strict_options(rate_governor)

find_package(Threads REQUIRED)
target_link_libraries(rate_governor PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(rate_governor PUBLIC ws2_32)
endif()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(RATE_GOVERNOR_BUILD_TOOLS)
  add_executable(rgctl tools/rgctl.cpp)
  target_link_libraries(rgctl PRIVATE rate_governor)
  rate_governor_strict_options(rgctl)

  add_executable(rg_backend_worker tools/rg_backend_worker.cpp)
  target_link_libraries(rg_backend_worker PRIVATE rate_governor)
  rate_governor_strict_options(rg_backend_worker)

  add_executable(rg_bench tools/rg_bench.cpp)
  target_link_libraries(rg_bench PRIVATE rate_governor)
  rate_governor_strict_options(rg_bench)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS rate_governor
  EXPORT RateGovernorTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(EXPORT RateGovernorTargets
  FILE RateGovernorTargets.cmake
  NAMESPACE RateGovernor::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RateGovernor")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RateGovernorConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/RateGovernorConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RateGovernor")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/RateGovernorConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/RateGovernorConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/RateGovernorConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RateGovernor")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(RATE_GOVERNOR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "Rate Governor ${PROJECT_VERSION} | build type: ${CMAKE_BUILD_TYPE} | tests: ${RATE_GOVERNOR_BUILD_TESTS} | tools: ${RATE_GOVERNOR_BUILD_TOOLS}")
