cmake_minimum_required(VERSION 3.22)

project(AutoLootNativeProbe LANGUAGES CXX)

set(RE_UE4SS_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/RE-UE4SS-c838a8ac")
if(NOT EXISTS "${RE_UE4SS_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Exact RE-UE4SS checkout not found: ${RE_UE4SS_SOURCE_DIR}")
endif()

add_subdirectory("${RE_UE4SS_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/re-ue4ss")

add_library(AutoLootNativeProbe SHARED src/main.cpp)
target_compile_features(AutoLootNativeProbe PRIVATE cxx_std_23)
target_link_libraries(AutoLootNativeProbe PUBLIC UE4SS)

add_executable(AutoLootOrchestrationSelfTest tests/orchestration_selftest.cpp src/orchestration.hpp)
target_compile_features(AutoLootOrchestrationSelfTest PRIVATE cxx_std_23)
enable_testing()
add_test(NAME AutoLootOrchestrationSelfTest COMMAND AutoLootOrchestrationSelfTest)

set_target_properties(AutoLootNativeProbe PROPERTIES
    OUTPUT_NAME "main"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/bin/AutoLootNativeProbe"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/bin/AutoLootNativeProbe"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/lib/AutoLootNativeProbe"
)
