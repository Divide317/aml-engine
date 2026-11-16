find_package(Catch2 3 REQUIRED)

add_executable(aml_tests
  test_types.cpp
)
# Catch2WithMain supplies main(), so test files contain only TEST_CASEs.
target_link_libraries(aml_tests PRIVATE aml Catch2::Catch2WithMain)

# Register every TEST_CASE with CTest individually.
include(Catch)
catch_discover_tests(aml_tests)
