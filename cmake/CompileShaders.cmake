# CompileShaders.cmake — compile GLSL compute shaders to SPIR-V and embed as C++.
#
# Usage:
#   compile_shaders(
#     SHADERS shaders/foo.comp shaders/bar.comp
#     OUTPUT_DIR ${CMAKE_BINARY_DIR}/generated/larch
#     TARGET larch_shaders
#   )
#
# For each .comp file, produces a .spv binary via glslc, then generates a C++
# header with the SPIR-V bytecode as a constexpr uint32_t array.
# All arrays are collected into a single header: <OUTPUT_DIR>/shader_bytecode.hpp

function(compile_shaders)
    cmake_parse_arguments(CS "" "OUTPUT_DIR;TARGET" "SHADERS" ${ARGN})

    find_program(GLSLC glslc REQUIRED)

    set(_spv_files "")
    set(_shader_decls "")

    foreach(shader ${CS_SHADERS})
        get_filename_component(name ${shader} NAME_WE)
        set(spv_file "${CS_OUTPUT_DIR}/${name}.spv")
        set(abs_shader "${CMAKE_SOURCE_DIR}/${shader}")

        add_custom_command(
            OUTPUT ${spv_file}
            COMMAND ${GLSLC} -fshader-stage=compute -o ${spv_file} ${abs_shader}
            DEPENDS ${abs_shader}
            COMMENT "Compiling shader ${shader} -> ${name}.spv"
            VERBATIM
        )
        list(APPEND _spv_files ${spv_file})

        # Generate the C++ embedding command.  This runs after the .spv is
        # built and produces a .hpp with the bytecode as a uint32_t array.
        set(hpp_file "${CS_OUTPUT_DIR}/${name}_spv.hpp")
        add_custom_command(
            OUTPUT ${hpp_file}
            COMMAND ${CMAKE_COMMAND}
                -DSPV_FILE=${spv_file}
                -DHPP_FILE=${hpp_file}
                -DNAME=${name}
                -P ${CMAKE_SOURCE_DIR}/cmake/EmbedSpirv.cmake
            DEPENDS ${spv_file}
            COMMENT "Embedding ${name}.spv as C++ header"
            VERBATIM
        )
        list(APPEND _spv_files ${hpp_file})
    endforeach()

    add_custom_target(${CS_TARGET} DEPENDS ${_spv_files})
endfunction()
