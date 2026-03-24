# EmbedSpirv.cmake — reads a .spv binary and writes a C++ header with the
# bytecode as a constexpr uint32_t array.
#
# Variables (passed via -D):
#   SPV_FILE  — path to the .spv binary
#   HPP_FILE  — path to the output .hpp file
#   NAME      — C++ identifier stem (e.g. "double_it" -> larch::shaders::double_it_spv)

file(READ "${SPV_FILE}" spv_hex HEX)
string(LENGTH "${spv_hex}" hex_len)
math(EXPR n_bytes "${hex_len} / 2")
math(EXPR n_words "${n_bytes} / 4")

# Convert hex pairs to 0xAABBCCDD uint32 literals (little-endian byte order
# matches SPIR-V's native word order).
set(words "")
set(i 0)
while(i LESS hex_len)
    # Read 8 hex chars (4 bytes = 1 uint32, little-endian).
    string(SUBSTRING "${spv_hex}" ${i} 2 b0)
    math(EXPR i1 "${i} + 2")
    string(SUBSTRING "${spv_hex}" ${i1} 2 b1)
    math(EXPR i2 "${i} + 4")
    string(SUBSTRING "${spv_hex}" ${i2} 2 b2)
    math(EXPR i3 "${i} + 6")
    string(SUBSTRING "${spv_hex}" ${i3} 2 b3)

    # SPIR-V is stored in native byte order; file(READ HEX) gives sequential
    # bytes.  On little-endian, bytes b0 b1 b2 b3 form uint32 0xb3b2b1b0.
    list(APPEND words "0x${b3}${b2}${b1}${b0}")

    math(EXPR i "${i} + 8")
endwhile()

list(JOIN words ", " words_str)

# Break into lines of 8 words.
set(lines "")
list(LENGTH words total)
set(idx 0)
while(idx LESS total)
    math(EXPR end "${idx} + 8")
    if(end GREATER total)
        set(end ${total})
    endif()
    math(EXPR count "${end} - ${idx}")
    list(SUBLIST words ${idx} ${count} line_words)
    list(JOIN line_words ", " line_str)
    list(APPEND lines "    ${line_str},")
    set(idx ${end})
endwhile()
list(JOIN lines "\n" body)

file(WRITE "${HPP_FILE}" "#pragma once\n")
file(APPEND "${HPP_FILE}" "// Auto-generated from ${NAME}.spv -- do not edit.\n")
file(APPEND "${HPP_FILE}" "#include <cstdint>\n")
file(APPEND "${HPP_FILE}" "#include <span>\n")
file(APPEND "${HPP_FILE}" "namespace larch::shaders {\n")
file(APPEND "${HPP_FILE}" "inline constexpr std::uint32_t ${NAME}_spv[] = {\n")
file(APPEND "${HPP_FILE}" "${body}\n")
file(APPEND "${HPP_FILE}" "};\n")
file(APPEND "${HPP_FILE}" "inline constexpr std::span<const std::uint32_t> ${NAME}{${NAME}_spv};\n")
file(APPEND "${HPP_FILE}" "} // namespace larch::shaders\n")
