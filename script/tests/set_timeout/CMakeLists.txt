cmake_minimum_required(VERSION 3.20)
project(SetTimeoutRegression LANGUAGES CXX)

# Link the actual utility implementation without pulling in the application.
# Section garbage collection discards unrelated utilities and their dependencies.
if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "This standalone regression runner requires Linux and GCC or Clang")
endif ()
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
get_filename_component(THRONE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_executable(set_timeout set_timeout.cpp "${THRONE_ROOT}/src/global/Utils.cpp")
target_compile_features(set_timeout PRIVATE cxx_std_20)
target_include_directories(set_timeout PRIVATE "${THRONE_ROOT}")
target_compile_options(set_timeout PRIVATE -ffunction-sections -fdata-sections)
target_link_options(set_timeout PRIVATE -Wl,--gc-sections)
target_link_libraries(set_timeout PRIVATE Qt6::Widgets Qt6::Network)

enable_testing()
add_test(NAME set_timeout COMMAND set_timeout)
set_tests_properties(set_timeout PROPERTIES TIMEOUT 30)
