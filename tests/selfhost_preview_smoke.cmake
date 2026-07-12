if(NOT DEFINED PREVIEW_EXE OR NOT DEFINED INPUT_SOURCE)
    message(FATAL_ERROR "PREVIEW_EXE and INPUT_SOURCE are required")
endif()
# read_line expects the stdin payload to be the path, not the source contents.
# Generate it here so CMake's path uses forward slashes on Windows too.
set(stdin_file "${CMAKE_CURRENT_BINARY_DIR}/selfhost_preview_stdin.txt")
file(WRITE "${stdin_file}" "${INPUT_SOURCE}\n")
execute_process(
    COMMAND "${PREVIEW_EXE}"
    INPUT_FILE "${stdin_file}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    TIMEOUT 60)
# The fixture computes twice(21), proving the generated code actually ran.
if(NOT rc EQUAL 42)
    message(FATAL_ERROR "self-host preview returned ${rc}, expected 42\nstdout:\n${out}\nstderr:\n${err}")
endif()
if(NOT out MATCHES "ember self-host preview")
    message(FATAL_ERROR "preview banner missing\nstdout:\n${out}")
endif()
