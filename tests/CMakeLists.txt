find_package(Catch2 3 REQUIRED)

add_executable(aml_tests
  test_types.cpp
  test_parse.cpp
)
# Catch2WithMain supplies main(), so test files contain only TEST_CASEs.
target_link_libraries(aml_tests PRIVATE aml aml_options Catch2::Catch2WithMain)

# Tests may include internal headers from src/ (e.g. parse.hpp) and the
# generated fixtures/tiny_expected.hpp.
target_include_directories(aml_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

# Absolute path to the fixture files, so tests run from any working directory.
target_compile_definitions(aml_tests PRIVATE AML_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# Register every TEST_CASE with CTest individually.
include(Catch)
catch_discover_tests(aml_tests)
