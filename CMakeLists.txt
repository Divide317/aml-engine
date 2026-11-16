cmake_minimum_required(VERSION 3.20)
project(aml_engine VERSION 0.1.0 LANGUAGES CXX)

# C++20 exactly, with no GNU extensions, so the code stays portable to clang/MSVC.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for editors and clang-tidy

# Single-config generators (Ninja, Makefiles) default to no optimisation at all.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(AML_BUILD_TESTS "Build the unit tests" ON)
option(AML_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# Flags every target in this project is compiled with. An INTERFACE target holds
# no code; linking to it just passes these options on.
add_library(aml_options INTERFACE)
target_compile_options(aml_options INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Werror)
if(AML_SANITIZE)
  target_compile_options(aml_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(aml_options INTERFACE -fsanitize=address,undefined)
endif()

# The engine. Header-only for now; it becomes a STATIC library when the first
# file lands in src/ (CSV loader, roadmap step 1).
add_library(aml INTERFACE)
target_include_directories(aml INTERFACE include)
target_link_libraries(aml INTERFACE aml_options)

if(AML_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
