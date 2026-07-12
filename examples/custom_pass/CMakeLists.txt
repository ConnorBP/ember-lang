# Copyable in-tree custom-pass extension target.
#
# From Ember's root CMakeLists.txt, use:
#   add_subdirectory(examples/custom_pass)
#   target_link_libraries(your_ember_host PRIVATE ember_custom_pass_examples)
#
# The host must still call
# ember::examples::custom_pass::register_passes(reg); linking is not discovery.
cmake_minimum_required(VERSION 3.16)

add_library(ember_custom_pass_examples STATIC
    minimal_pass.cpp
    nop_injection_pass.cpp
    block_merge_pass.cpp
)

target_include_directories(ember_custom_pass_examples PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(ember_custom_pass_examples PUBLIC ember_frontend)
target_compile_features(ember_custom_pass_examples PUBLIC cxx_std_17)
