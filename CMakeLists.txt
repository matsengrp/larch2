cmake_minimum_required(VERSION 3.25)

# --- GCC toolchain discovery --------------------------------------------------
# Pass -DGCC_TOOLCHAIN=/path/to/gcc to resolve all toolchain binaries from one
# prefix (compilers, ar, ranlib).  This must happen before project() so that
# CMake's compiler-detection uses the right tools from the start.
# Handles both standard names (gcc, g++) and suffixed names (gcc-trunk, g++-trunk).
set(GCC_TOOLCHAIN "" CACHE PATH "GCC installation prefix (e.g. /opt/gcc-latest)")
if(GCC_TOOLCHAIN)
    # Find the gcc binary — may be 'gcc' or 'gcc-<suffix>'
    file(GLOB _gcc_bins "${GCC_TOOLCHAIN}/bin/gcc" "${GCC_TOOLCHAIN}/bin/gcc-*")
    list(FILTER _gcc_bins EXCLUDE REGEX "(gcc-ar|gcc-nm|gcc-ranlib)")
    if(NOT _gcc_bins)
        message(FATAL_ERROR "GCC_TOOLCHAIN: no gcc compiler found in ${GCC_TOOLCHAIN}/bin/")
    endif()
    list(SORT _gcc_bins)
    list(GET _gcc_bins 0 _GCC_CC)
    # Derive the suffix (e.g. "" or "-trunk") and apply to all tools
    get_filename_component(_gcc_name "${_GCC_CC}" NAME)
    string(REGEX REPLACE "^gcc" "" _suffix "${_gcc_name}")
    set(CMAKE_C_COMPILER   "${GCC_TOOLCHAIN}/bin/gcc${_suffix}"   CACHE FILEPATH "" FORCE)
    set(CMAKE_CXX_COMPILER "${GCC_TOOLCHAIN}/bin/g++${_suffix}"   CACHE FILEPATH "" FORCE)
endif()

project(larch2 VERSION 2.0.3 LANGUAGES C CXX)

# On FreeBSD the system ranlib is LLVM's, which rejects the --plugin flag that
# gcc-ranlib passes.  CMake's IPO uses CMAKE_<LANG>_COMPILER_RANLIB for static
# libs; reset those to the plain system ranlib so LTO archives build correctly.
# (LTO still works: the linker reads GIMPLE bytecode from archive members at
# link time regardless of the ranlib used.)
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    # GCC's gcc-ranlib wraps the system ranlib with --plugin, but FreeBSD's
    # LLVM ranlib rejects --plugin.  LTO still works: the GCC linker reads
    # GIMPLE bytecode from archive members at link time regardless.  Override
    # the IPO archive-finish rules to use the system ranlib directly.
    find_program(_SYS_RANLIB ranlib PATHS /usr/bin NO_DEFAULT_PATH)
    if(_SYS_RANLIB)
        set(CMAKE_C_ARCHIVE_FINISH_IPO   "\"${_SYS_RANLIB}\" <TARGET>")
        set(CMAKE_CXX_ARCHIVE_FINISH_IPO "\"${_SYS_RANLIB}\" <TARGET>")
    endif()
endif()

# --- Version info from git ---------------------------------------------------
set(LARCH_VERSION ${PROJECT_VERSION})
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE LARCH_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT LARCH_GIT_COMMIT)
    set(LARCH_GIT_COMMIT "unknown")
endif()
configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/larch/version.hpp
    @ONLY
)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-freflection)

# Use mold linker on FreeBSD (GCC specs pass -m elf_x86_64_fbsd which mold
# doesn't recognise; tools/mold-ld/ld is a wrapper that rewrites it).
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    add_link_options("-B${CMAKE_SOURCE_DIR}/tools/mold-ld")
endif()

# Statically link libstdc++ and libgcc so the binary is portable across
# distros without needing the exact GCC runtime version at runtime.
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

add_library(larch src/protobuf.cpp src/protobuf_encode.cpp src/pickle_reader.cpp)
target_include_directories(larch PUBLIC include ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(larch PUBLIC ZLIB::ZLIB Threads::Threads)

# --- Optional Vulkan compute --------------------------------------------------

option(ENABLE_VULKAN "Enable Vulkan compute for neural network inference" OFF)

if(ENABLE_VULKAN)
    find_package(Vulkan REQUIRED)
    include(cmake/CompileShaders.cmake)

    compile_shaders(
        SHADERS shaders/double_it.comp
                shaders/fivemer_forward.comp
                shaders/cnn_r_branch.comp
                shaders/cnn_s_branch.comp
        OUTPUT_DIR ${CMAKE_BINARY_DIR}/generated/larch
        TARGET larch_shaders
    )

    add_library(larch_vulkan src/vulkan_compute.cpp src/nn_inference.cpp)
    target_include_directories(larch_vulkan PUBLIC include ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(larch_vulkan PUBLIC Vulkan::Vulkan larch)
    add_dependencies(larch_vulkan larch_shaders)
endif()

# --- Executable ---------------------------------------------------------------

add_executable(larch2 tools/larch2.cpp)
target_link_libraries(larch2 PRIVATE larch)

add_executable(dagutil tools/dagutil.cpp)
target_link_libraries(dagutil PRIVATE larch)

add_executable(larch2-bcr tools/larch2-bcr.cpp)
target_link_libraries(larch2-bcr PRIVATE larch)

set_target_properties(larch2 dagutil larch2-bcr PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# --- Tests --------------------------------------------------------------------

enable_testing()

# Header-only tests (no library link needed beyond include path)
foreach(test_name chain_test dag_test hash_chain_test extend_dag_test
                  overlay_dag_test bigint_test weight_counter_test
                  kmer_encoder_test likelihood_test)
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

# dagutil ML sampling CLI smoke tests.
set(DAGUTIL_ML_SAMPLE_OUT "${CMAKE_BINARY_DIR}/dagutil_ml_sample.pb.gz")
add_test(NAME dagutil_ml_sample
         COMMAND dagutil
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --dag-pb data/test_5_trees/tree_1.pb.gz
                 --force-no-vcf
                 --sample
                 --sample-uniformly
                 --sample-method ml
                 --model-dir data/bcr
                 --model-name ThriftyHumV0.2-45
                 --validate
                 --seed 1
                 -o ${DAGUTIL_ML_SAMPLE_OUT}
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(dagutil_ml_sample
                     PROPERTIES
                     FIXTURES_SETUP dagutil_ml_sample_fixture
                     PASS_REGULAR_EXPRESSION
                     "sampled_tree: ML_NLL=948\\.401399")
add_test(NAME dagutil_thrifty_sample
         COMMAND dagutil
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --force-no-vcf
                 --sample
                 --sample-method thrifty
                 --model-dir data/bcr
                 --model-name ThriftyHumV0.2-45
                 --seed 1
                 -o ${CMAKE_BINARY_DIR}/dagutil_thrifty_sample.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME dagutil_ml_missing_model_fails
         COMMAND dagutil
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --force-no-vcf
                 --sample
                 --sample-method ml
                 -o ${CMAKE_BINARY_DIR}/dagutil_missing_model.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(dagutil_ml_missing_model_fails PROPERTIES WILL_FAIL TRUE)
add_test(NAME dagutil_sample_method_without_sample_fails
         COMMAND dagutil
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --force-no-vcf
                 --trim
                 --sample-method ml
                 -o ${CMAKE_BINARY_DIR}/dagutil_ignored_sample_method.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(dagutil_sample_method_without_sample_fails
                     PROPERTIES WILL_FAIL TRUE)
add_test(NAME dagutil_rf_sample_method_fails
         COMMAND dagutil
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --force-no-vcf
                 --trim
                 --rf data/test_5_trees/tree_0.pb.gz
                 --sample
                 --sample-method parsimony
                 -o ${CMAKE_BINARY_DIR}/dagutil_rf_sample_method.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(dagutil_rf_sample_method_fails PROPERTIES WILL_FAIL TRUE)
add_test(NAME dagutil_ml_sample_validate
         COMMAND dagutil
                 --dag-pb ${DAGUTIL_ML_SAMPLE_OUT}
                 --force-no-vcf
                 --validate
                 --dag-info
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(dagutil_ml_sample_validate
                     PROPERTIES
                     FIXTURES_REQUIRED dagutil_ml_sample_fixture
                     PASS_REGULAR_EXPRESSION "tree_count: 1")

# dagutil ML edge-penalty CLI smoke test: write ML penalties to edge_weight,
# reload them, and sample a zero-penalty min-ML tree via edge-weight sampling.
set(DAGUTIL_EDGE_ML_SCORED_OUT
    "${CMAKE_BINARY_DIR}/dagutil_edge_ml_scored.pb.gz")
set(DAGUTIL_EDGE_ML_SAMPLE_OUT
    "${CMAKE_BINARY_DIR}/dagutil_edge_ml_sample.pb.gz")
add_test(NAME dagutil_edge_ml_score
         COMMAND dagutil
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --dag-pb data/test_5_trees/tree_1.pb.gz
                 --force-no-vcf
                 --edge-ml
                 --model-dir data/bcr
                 --model-name ThriftyHumV0.2-45
                 --seed 1
                 -o ${DAGUTIL_EDGE_ML_SCORED_OUT}
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(dagutil_edge_ml_score
                     PROPERTIES
                     FIXTURES_SETUP dagutil_edge_ml_fixture
                     PASS_REGULAR_EXPRESSION
                     "edge_ml: global_min=.*nonzero_penalty=[1-9]")
add_test(NAME dagutil_edge_ml_missing_model_fails
         COMMAND dagutil
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --force-no-vcf
                 --edge-ml
                 -o ${CMAKE_BINARY_DIR}/dagutil_edge_ml_missing_model.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(dagutil_edge_ml_missing_model_fails
                     PROPERTIES WILL_FAIL TRUE)
add_test(NAME dagutil_edge_ml_with_sample_fails
         COMMAND dagutil
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --force-no-vcf
                 --edge-ml
                 --model-dir data/bcr
                 --model-name ThriftyHumV0.2-45
                 --sample
                 --sample-method edge-weight
                 -o ${CMAKE_BINARY_DIR}/dagutil_edge_ml_with_sample.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(dagutil_edge_ml_with_sample_fails
                     PROPERTIES WILL_FAIL TRUE)
add_test(NAME dagutil_edge_ml_sample
         COMMAND dagutil
                 --dag-pb ${DAGUTIL_EDGE_ML_SCORED_OUT}
                 --force-no-vcf
                 --sample
                 --sample-uniformly
                 --sample-method edge-weight
                 --seed 1
                 -o ${DAGUTIL_EDGE_ML_SAMPLE_OUT}
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(dagutil_edge_ml_sample
                     PROPERTIES
                     FIXTURES_REQUIRED dagutil_edge_ml_fixture
                     FIXTURES_SETUP dagutil_edge_ml_sample_fixture
                     PASS_REGULAR_EXPRESSION "edge_weight_min=0.000000")
add_test(NAME dagutil_edge_ml_sample_validate
         COMMAND dagutil
                 --dag-pb ${DAGUTIL_EDGE_ML_SAMPLE_OUT}
                 --force-no-vcf
                 --validate
                 --dag-info
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(dagutil_edge_ml_sample_validate
                     PROPERTIES
                     FIXTURES_REQUIRED dagutil_edge_ml_sample_fixture
                     PASS_REGULAR_EXPRESSION "tree_count: 1")

# dagutil stored edge_weight CLI smoke test: write edge penalties, reload them,
# and sample a min-edge-weight tree.
set(DAGUTIL_EDGE_WEIGHT_SCORED_OUT
    "${CMAKE_BINARY_DIR}/dagutil_edge_weight_scored.pb.gz")
set(DAGUTIL_EDGE_WEIGHT_SAMPLE_OUT
    "${CMAKE_BINARY_DIR}/dagutil_edge_weight_sample.pb.gz")
add_test(NAME dagutil_edge_weight_score
         COMMAND dagutil
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --dag-pb data/test_5_trees/tree_1.pb.gz
                 --force-no-vcf
                 --edge-parsimony
                 -o ${DAGUTIL_EDGE_WEIGHT_SCORED_OUT}
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(dagutil_edge_weight_score
                     PROPERTIES
                     FIXTURES_SETUP dagutil_edge_weight_fixture
                     PASS_REGULAR_EXPRESSION
                     "edge_parsimony: global_min=.*nonzero_penalty=[1-9]")
add_test(NAME dagutil_edge_parsimony_with_sample_fails
         COMMAND dagutil
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --force-no-vcf
                 --edge-parsimony
                 --sample
                 --sample-method edge-weight
                 -o ${CMAKE_BINARY_DIR}/dagutil_edge_parsimony_with_sample.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(dagutil_edge_parsimony_with_sample_fails
                     PROPERTIES WILL_FAIL TRUE)
add_test(NAME dagutil_edge_parsimony_with_trim_fails
         COMMAND dagutil
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --force-no-vcf
                 --edge-parsimony
                 --trim
                 -o ${CMAKE_BINARY_DIR}/dagutil_edge_parsimony_with_trim.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(dagutil_edge_parsimony_with_trim_fails
                     PROPERTIES WILL_FAIL TRUE)
add_test(NAME dagutil_edge_weight_sample
         COMMAND dagutil
                 --dag-pb ${DAGUTIL_EDGE_WEIGHT_SCORED_OUT}
                 --force-no-vcf
                 --sample
                 --sample-uniformly
                 --sample-method edge-weight
                 --seed 1
                 -o ${DAGUTIL_EDGE_WEIGHT_SAMPLE_OUT}
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(dagutil_edge_weight_sample
                     PROPERTIES
                     FIXTURES_REQUIRED dagutil_edge_weight_fixture
                     FIXTURES_SETUP dagutil_edge_weight_sample_fixture
                     PASS_REGULAR_EXPRESSION "edge_weight_min=0.000000")
add_test(NAME dagutil_edge_weight_sample_validate
         COMMAND dagutil
                 --dag-pb ${DAGUTIL_EDGE_WEIGHT_SAMPLE_OUT}
                 --force-no-vcf
                 --validate
                 --dag-info
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(dagutil_edge_weight_sample_validate
                     PROPERTIES
                     FIXTURES_REQUIRED dagutil_edge_weight_sample_fixture
                     PASS_REGULAR_EXPRESSION "tree_count: 1")

add_test(NAME larch2_edge_weight_sample
         COMMAND larch2
                 --dag-pb ${DAGUTIL_EDGE_WEIGHT_SCORED_OUT}
                 --sample-method edge-weight
                 --sample-uniformly
                 --seed 1
                 -n 1
                 --max-moves 1
                 -o ${CMAKE_BINARY_DIR}/larch2_edge_weight_sample.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(larch2_edge_weight_sample
                     PROPERTIES FIXTURES_REQUIRED dagutil_edge_weight_fixture)
add_test(NAME larch2_edge_weight_patience_reports_objective
         COMMAND larch2
                 --dag-pb ${DAGUTIL_EDGE_WEIGHT_SCORED_OUT}
                 --sample-method edge-weight
                 --seed 1
                 -n 2
                 --max-moves 0
                 --patience 1
                 -o ${CMAKE_BINARY_DIR}/larch2_edge_weight_patience.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(larch2_edge_weight_patience_reports_objective
                     PROPERTIES
                     FIXTURES_REQUIRED dagutil_edge_weight_fixture
                     PASS_REGULAR_EXPRESSION
                     "Early stopping: no edge_weight improvement")

# larch2 ML sampling CLI smoke tests.
add_test(NAME larch2_ml_score_ua_edge_sample
         COMMAND larch2
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --sample-method ml
                 --sample-uniformly
                 --score-ua-edge-ml
                 --model-dir data/bcr
                 --model-name ThriftyHumV0.2-45
                 --seed 1
                 -n 1
                 --max-moves 1
                 -o ${CMAKE_BINARY_DIR}/larch2_ml_score_ua_edge_sample.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(larch2_ml_score_ua_edge_sample
                     PROPERTIES PASS_REGULAR_EXPRESSION "ML NLL")
add_test(NAME larch2_ml_patience_reports_objective
         COMMAND larch2
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --sample-method ml
                 --model-dir data/bcr
                 --model-name ThriftyHumV0.2-45
                 --seed 1
                 -n 2
                 --max-moves 0
                 --move-coeff-ml 1.0
                 --patience 1
                 -o ${CMAKE_BINARY_DIR}/larch2_ml_patience.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(larch2_ml_patience_reports_objective
                     PROPERTIES PASS_REGULAR_EXPRESSION
                     "Early stopping: no ML NLL improvement")
add_test(NAME larch2_ml_drift_fails
         COMMAND bash test/expect_fail_regex.sh
                 "objective-aware drift"
                 $<TARGET_FILE:larch2>
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --sample-method ml
                 --model-dir data/bcr
                 --model-name ThriftyHumV0.2-45
                 --drift 1
                 -n 0
                 -o ${CMAKE_BINARY_DIR}/larch2_ml_drift.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(NAME larch2_unknown_sample_method_fails
         COMMAND larch2
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --sample-method definitely-not-a-method
                 -o ${CMAKE_BINARY_DIR}/larch2_unknown_sample_method.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(larch2_unknown_sample_method_fails
                     PROPERTIES WILL_FAIL TRUE)
add_test(NAME larch2_trim_ml_sample_method_fails
         COMMAND larch2
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --trim
                 --sample-method ml
                 --model-dir data/bcr
                 --model-name ThriftyHumV0.2-45
                 -o ${CMAKE_BINARY_DIR}/larch2_trim_ml_sample_method.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(larch2_trim_ml_sample_method_fails
                     PROPERTIES WILL_FAIL TRUE)
add_test(NAME larch2_diverse_ml_sample_method_fails
         COMMAND larch2
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --diverse-sample 2
                 --sample-method ml
                 --model-dir data/bcr
                 --model-name ThriftyHumV0.2-45
                 -o ${CMAKE_BINARY_DIR}/larch2_diverse_ml_sample_method.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(larch2_diverse_ml_sample_method_fails
                     PROPERTIES WILL_FAIL TRUE)
add_test(NAME larch2_partial_model_args_fail
         COMMAND larch2
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --model-dir data/bcr
                 -o ${CMAKE_BINARY_DIR}/larch2_partial_model_args.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(larch2_partial_model_args_fail PROPERTIES WILL_FAIL TRUE)
add_test(NAME larch2_ua_flag_without_ml_warns
         COMMAND larch2
                 --dag-pb data/test_5_trees/tree_0.pb.gz
                 --optimizer random
                 --score-ua-edge-ml
                 -n 0
                 -o ${CMAKE_BINARY_DIR}/larch2_ua_flag_without_ml.pb.gz
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(larch2_ua_flag_without_ml_warns
                     PROPERTIES PASS_REGULAR_EXPRESSION
                     "warning: --ignore-ua-edge-ml/--score-ua-edge-ml has no effect")

# Tests that need liblarch (protobuf + zlib + threads) and run from source dir
foreach(test_name merge_test merge_consistency_test subtree_weight_test
                  weight_accum_test optimize_test native_optimize_test
                  spr_pipeline_test trim_clade_edges_test
                  rotaA_diagnostic_test
                  pth_loader_test rs_fivemer_model_test
                  nn_cnn_test nn_s5f_test nn_ml_spr_test
                  nn_blended_spr_test)
    add_executable(${test_name} test/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE larch)
    add_test(NAME ${test_name} COMMAND ${test_name}
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()

# Vulkan compute tests
if(ENABLE_VULKAN)
    add_executable(vulkan_compute_test test/vulkan_compute_test.cpp)
    target_link_libraries(vulkan_compute_test PRIVATE larch_vulkan)
    add_dependencies(vulkan_compute_test larch_shaders)
    add_test(NAME vulkan_compute_test COMMAND vulkan_compute_test)

    add_executable(nn_inference_test test/nn_inference_test.cpp)
    target_link_libraries(nn_inference_test PRIVATE larch_vulkan)
    add_dependencies(nn_inference_test larch_shaders)
    add_test(NAME nn_inference_test COMMAND nn_inference_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    set_tests_properties(vulkan_compute_test nn_inference_test
                         PROPERTIES SKIP_RETURN_CODE 77)
endif()
