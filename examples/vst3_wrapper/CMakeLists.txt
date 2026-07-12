# The SDK's `sdk` target contains the common VST implementation, but (like
# Steinberg's again-simple sample) deliberately omits SingleComponentEffect.
# Keep the SDK root local to this sibling subdirectory as variables set inside
# add_subdirectory(thirdparty/vst3sdk) do not propagate back to us.  Defining
# public_sdk_SOURCE_DIR also lets smtg_add_vst3plugin add the platform entry TU
# (dllmain.cpp on Windows), which supplies moduleHandle/InitDll/ExitDll.
set(EMBER_VST3_SDK_ROOT "${CMAKE_SOURCE_DIR}/thirdparty/vst3sdk")
set(public_sdk_SOURCE_DIR "${EMBER_VST3_SDK_ROOT}/public.sdk")

set(EMBER_VST3_SOURCES
    vst3_ember_processor.h
    vst3_ember_processor.cpp
    vst3_ember_factory.cpp
    vst3_ember_entry.cpp
    ${public_sdk_SOURCE_DIR}/source/vst/vstsinglecomponenteffect.h
    ${public_sdk_SOURCE_DIR}/source/vst/vstsinglecomponenteffect.cpp
)

smtg_add_vst3plugin(ember_gain ${EMBER_VST3_SOURCES})

# SDK 3.8 only assigns the Windows bundle's architecture-specific output
# directories by iterating CMAKE_CONFIGURATION_TYPES.  That list is empty for
# single-config generators (Ninja/Makefiles), otherwise the PE DLL lands at
# VST3/ember_gain.vst3 while moduleinfotool looks inside the package and fails.
if(SMTG_WIN AND SMTG_CREATE_BUNDLE_FOR_WINDOWS AND NOT CMAKE_CONFIGURATION_TYPES)
    get_target_property(EMBER_VST3_WIN_ARCH ember_gain SMTG_WIN_ARCHITECTURE_NAME)
    set_target_properties(ember_gain PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY
            "${CMAKE_BINARY_DIR}/VST3/${CMAKE_BUILD_TYPE}/ember_gain.vst3/Contents/${EMBER_VST3_WIN_ARCH}")
endif()

target_compile_features(ember_gain PRIVATE cxx_std_17)
# VST3 3.8 checks _MSC_VER for Windows' aligned allocator, but MinGW also
# lacks std::aligned_alloc. Redirect those two SDK calls to CRT equivalents.
if(MINGW)
    target_compile_definitions(sdk PRIVATE
        "aligned_alloc(numBytes,alignment)=_aligned_malloc(numBytes,alignment)"
        "aligned_free(addr,alignment)=_aligned_free(addr)")
endif()
target_compile_definitions(ember_gain PRIVATE
    EMBER_VST3_DEFAULT_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/gain_vst.ember"
    SMTG_PLATFORM_64=$<IF:$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>,1,0>
    $<$<PLATFORM_ID:Windows>:UNICODE>
    $<$<PLATFORM_ID:Windows>:_UNICODE>)
target_include_directories(ember_gain PRIVATE
    ${EMBER_VST3_SDK_ROOT}
    ${public_sdk_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extensions/audio)
target_link_libraries(ember_gain PRIVATE
    sdk
    ember
    ember_frontend
    ember_ext_audio)

smtg_target_add_plugin_resources(ember_gain RESOURCES gain_vst.ember)
set_target_properties(ember_gain PROPERTIES OUTPUT_NAME "ember_gain")
