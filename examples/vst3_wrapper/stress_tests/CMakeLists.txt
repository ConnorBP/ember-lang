add_executable(vst3_stress_tests vst3_stress_tests.cpp)
target_link_libraries(vst3_stress_tests PRIVATE ember_vst3_processor sdk_hosting)
target_compile_features(vst3_stress_tests PRIVATE cxx_std_17)
if(WIN32)
    target_link_libraries(vst3_stress_tests PRIVATE psapi)
endif()

