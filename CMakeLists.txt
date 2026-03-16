cmake_minimum_required(VERSION 3.25)

project(larch2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-freflection)

# Use mold linker on FreeBSD (GCC specs pass -m elf_x86_64_fbsd which mold
# doesn't recognise; tools/mold-ld/ld is a wrapper that rewrites it).
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    add_link_options("-B${CMAKE_SOURCE_DIR}/tools/mold-ld")
endif()

# Statically link libstdc++ and libgcc
add_link_options(-static-libstdc++ -static-libgcc)

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

# --- Sanitizers ---------------------------------------------------------------

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN are mutually exclusive")
endif()

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

# --- LTO for Release ----------------------------------------------------------

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# --- Library ------------------------------------------------------------------

add_library(larch src/protobuf.cpp src/protobuf_encode.cpp)
target_include_directories(larch PUBLIC include)
target_link_libraries(larch PUBLIC ZLIB::ZLIB Threads::Threads)

# --- Executable ---------------------------------------------------------------

add_executable(larch2 tools/larch2.cpp)
target_link_libraries(larch2 PRIVATE larch)

# --- Tests --------------------------------------------------------------------

enable_testing()

# Header-only tests (no library link needed beyond include path)
foreach(test_name chain_test dag_test hash_chain_test extend_dag_test
                  overlay_dag_test bigint_test weight_counter_test)
    add_executable(${test_name} test/${test_name}.cpp)
    target_include_directories(${test_name} PRIVATE include)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Tests that need Threads::Threads only
foreach(test_name rf_distance_test thread_pool_test validate_dag_test)
    add_executable(${test_name} test/${test_name}.cpp)
    target_include_directories(${test_name} PRIVATE include)
    target_link_libraries(${test_name} PRIVATE Threads::Threads)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Tests that need liblarch (protobuf + zlib + threads) and run from source dir
foreach(test_name merge_test subtree_weight_test weight_accum_test
                  optimize_test native_optimize_test spr_pipeline_test)
    add_executable(${test_name} test/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE larch)
    add_test(NAME ${test_name} COMMAND ${test_name}
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()
