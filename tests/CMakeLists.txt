add_executable(rg_tests
  main.cpp
  test_support.cpp
  test_math.cpp
  test_codec_wire.cpp
  test_plan.cpp
  test_lifecycle.cpp
  test_races.cpp
  test_journal.cpp
  test_recovery.cpp
  test_concurrency.cpp
  test_randomized.cpp
  test_multiprocess.cpp
)

target_link_libraries(rg_tests PRIVATE rate_governor)
rate_governor_strict_options(rg_tests)

target_compile_definitions(rg_tests PRIVATE
  RG_WORKER_EXECUTABLE="$<TARGET_FILE:rg_backend_worker>"
  RG_CTL_EXECUTABLE="$<TARGET_FILE:rgctl>")

# No TIMEOUT property is set anywhere: tests run to completion.
add_test(NAME rate_governor_suite COMMAND rg_tests)
