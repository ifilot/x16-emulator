cmake_minimum_required(VERSION 3.16)

# Resolve transitive DLL imports instead of maintaining a list tied to a
# particular SDL/FluidSynth release. Use the active MinGW toolchain.
find_program(OBJDUMP objdump REQUIRED)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${OBJDUMP}")
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${ROOT}/build/x16emu.exe" "${ROOT}/build/makecart.exe"
    DIRECTORIES "${MINGW_BIN}"
    RESOLVED_DEPENDENCIES_VAR DLLS
    UNRESOLVED_DEPENDENCIES_VAR MISSING_DLLS
    PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
    POST_EXCLUDE_REGEXES ".*[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\].*"
)
if(MISSING_DLLS)
    message(FATAL_ERROR "Unresolved runtime dependencies: ${MISSING_DLLS}")
endif()
foreach(DLL IN LISTS DLLS)
    message(STATUS "Packaging ${DLL}")
    file(COPY "${DLL}" DESTINATION "${ROOT}/emu_binaries")
endforeach()
