set(EMBER_VST3_SOURCES
    vst3_ember_processor.h
    vst3_ember_processor.cpp
    vst3_ember_factory.cpp
    vst3_ember_entry.cpp
)

smtg_add_vst3plugin(ember_gain ${EMBER_VST3_SOURCES})
target_compile_features(ember_gain PRIVATE cxx_std_17)
# VST3 3.8 checks _MSC_VER for Windows' aligned allocator, but MinGW also
# lacks std::aligned_alloc. Redirect those two SDK calls to CRT equivalents.
if(MINGW)
    target_compile_definitions(sdk PRIVATE
        "aligned_alloc(numBytes,alignment)=_aligned_malloc(numBytes,alignment)"
        "aligned_free(addr,alignment)=_aligned_free(addr)")
endif()
target_compile_definitions(ember_gain PRIVATE
    EMBER_VST3_DEFAULT_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/gain_vst.ember")
target_include_directories(ember_gain PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extensions/audio)
target_link_libraries(ember_gain PRIVATE
    sdk
    ember
    ember_frontend
    ember_ext_audio)

smtg_target_add_plugin_resources(ember_gain RESOURCES gain_vst.ember)
set_target_properties(ember_gain PROPERTIES OUTPUT_NAME "ember_gain")
