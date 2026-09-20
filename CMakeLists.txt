cmake_minimum_required(VERSION 3.24)
if(NOT COMMAND o3de_gem_setup)
    project(SilPOMCore LANGUAGES CXX)
    enable_testing()
    add_executable(silpom_core_tests Tests/HeightfieldTests.cpp)
    target_compile_features(silpom_core_tests PRIVATE cxx_std_17)
    add_executable(silpom_curved_tests Tests/Curved/CurvedTests.cpp)
    target_compile_features(silpom_curved_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(silpom_core_tests PRIVATE /W4 /fp:precise)
        target_compile_options(silpom_curved_tests PRIVATE /W4 /fp:precise)
    endif()
    add_test(NAME SilPOM.Core COMMAND silpom_core_tests)
    add_executable(silpom_preview_sampling_tests Tests/PreviewSamplingTests.cpp)
    target_compile_features(silpom_preview_sampling_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.PreviewSampling COMMAND silpom_preview_sampling_tests)
    add_test(NAME SilPOM.CurvedCPU COMMAND silpom_curved_tests)
    set(SILPOM_PYTHON "" CACHE FILEPATH "Python interpreter for optional metadata decoder tests")
    if(SILPOM_PYTHON)
        add_test(NAME SilPOM.Metadata COMMAND "${SILPOM_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/Tests/test_roundtrip_metadata.py")
    endif()
    set(SILPOM_BLENDER "" CACHE FILEPATH "Blender executable for optional authoring and add-on tests (validated with 5.0.1)")
    if(SILPOM_BLENDER)
        add_test(NAME SilPOM.BlenderAuthoring COMMAND "${SILPOM_BLENDER}" --background --factory-startup --python-exit-code 1
            --python "${CMAKE_CURRENT_SOURCE_DIR}/Tests/blender_authoring_tests.py" -- --output "${CMAKE_CURRENT_BINARY_DIR}/blender-authoring-tests")
        add_test(NAME SilPOM.BlenderAddon COMMAND "${SILPOM_BLENDER}" --background --factory-startup --python-exit-code 1
            --python "${CMAKE_CURRENT_SOURCE_DIR}/Tests/blender_addon_tests.py" -- --output "${CMAKE_CURRENT_BINARY_DIR}/blender-addon-tests")
        set_tests_properties(SilPOM.BlenderAddon PROPERTIES
            ENVIRONMENT "BLENDER_USER_RESOURCES=${CMAKE_CURRENT_BINARY_DIR}/blender-addon-tests/blender-user")
    endif()
    set(SILPOM_DXC "" CACHE FILEPATH "DXC executable for optional Windows GPU conformance tests")
    if(WIN32 AND SILPOM_DXC)
        add_executable(silpom_gpu_tests Tests/GpuTests.cpp)
        target_compile_features(silpom_gpu_tests PRIVATE cxx_std_17)
        target_compile_definitions(silpom_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(silpom_gpu_tests PRIVATE d3d12 dxgi dxguid)
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/compute.dxil ${CMAKE_CURRENT_BINARY_DIR}/rays.dxil ${CMAKE_CURRENT_BINARY_DIR}/curved.dxil ${CMAKE_CURRENT_BINARY_DIR}/curved-rational.dxil
            COMMAND "${SILPOM_DXC}" -T cs_6_3 -E Main -Fo ${CMAKE_CURRENT_BINARY_DIR}/compute.dxil ${CMAKE_CURRENT_SOURCE_DIR}/Tests/HeightfieldGpu.hlsl
            COMMAND "${SILPOM_DXC}" -T lib_6_3 -D SILPOM_RT=1 -Fo ${CMAKE_CURRENT_BINARY_DIR}/rays.dxil ${CMAKE_CURRENT_SOURCE_DIR}/Tests/HeightfieldGpu.hlsl
            COMMAND "${SILPOM_DXC}" -T cs_6_3 -E Main -Fo ${CMAKE_CURRENT_BINARY_DIR}/curved.dxil ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Curved/CurvedGpu.hlsl
            COMMAND "${SILPOM_DXC}" -T cs_6_3 -E RationalFallback -Fo ${CMAKE_CURRENT_BINARY_DIR}/curved-rational.dxil ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Curved/CurvedGpu.hlsl
            DEPENDS Tests/HeightfieldGpu.hlsl Assets/Shaders/SilPOM/Heightfield.azsli Tests/Curved/CurvedGpu.hlsl
                Assets/Shaders/SilPOM/Curved/Intersection.azsli Assets/Shaders/SilPOM/Curved/CurvedGpuInterval.hlsli
                Assets/Shaders/SilPOM/Curved/CurvedGpuRational.hlsli Assets/Shaders/SilPOM/Curved/CurvedBoundedDyadic.hlsli
                Tests/Curved/CurvedGpuInterval.hlsli Tests/Curved/CurvedGpuRational.hlsli Tests/Curved/CurvedBoundedDyadic.hlsli VERBATIM)
        add_custom_target(silpom_gpu_shaders DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/compute.dxil ${CMAKE_CURRENT_BINARY_DIR}/rays.dxil ${CMAKE_CURRENT_BINARY_DIR}/curved.dxil ${CMAKE_CURRENT_BINARY_DIR}/curved-rational.dxil)
        add_dependencies(silpom_gpu_tests silpom_gpu_shaders)
        add_test(NAME SilPOM.GPU COMMAND silpom_gpu_tests ${CMAKE_CURRENT_BINARY_DIR}/compute.dxil ${CMAKE_CURRENT_BINARY_DIR}/rays.dxil ${CMAKE_CURRENT_BINARY_DIR}/curved.dxil ${CMAKE_CURRENT_BINARY_DIR}/curved-rational.dxil)
    endif()
    return()
endif()

o3de_gem_setup("SilPOM")
ly_add_target(NAME ${gem_name}.Static STATIC NAMESPACE Gem
    FILES_CMAKE silpom_files.cmake
    INCLUDE_DIRECTORIES PUBLIC Include PRIVATE Source
    BUILD_DEPENDENCIES PUBLIC AZ::AzCore AZ::AzFramework Gem::Atom_RPI.Public Gem::Atom_Feature_Common.Public)
if(MSVC)
    target_compile_options(${gem_name}.Static PRIVATE /EHsc)
else()
    target_compile_options(${gem_name}.Static PRIVATE -fexceptions)
endif()
ly_add_target(NAME ${gem_name} ${PAL_TRAIT_MONOLITHIC_DRIVEN_MODULE_TYPE} NAMESPACE Gem
    FILES_CMAKE silpom_shared_files.cmake BUILD_DEPENDENCIES PRIVATE Gem::${gem_name}.Static)
if(PAL_TRAIT_BUILD_HOST_TOOLS)
    ly_add_target(NAME ${gem_name}.Builder GEM_MODULE NAMESPACE Gem FILES_CMAKE silpom_builder_files.cmake
        INCLUDE_DIRECTORIES PRIVATE Include Source
        BUILD_DEPENDENCIES PRIVATE Gem::${gem_name}.Static AZ::AssetBuilderSDK AZ::AzToolsFramework AZ::SceneCore 3rdParty::OpenSSL)
    ly_add_target(NAME ${gem_name}.Editor GEM_MODULE NAMESPACE Gem FILES_CMAKE silpom_editor_files.cmake
        INCLUDE_DIRECTORIES PRIVATE Include Source
        BUILD_DEPENDENCIES PRIVATE Gem::${gem_name}.Static AZ::AzToolsFramework Gem::AtomLyIntegration_CommonFeatures.Public)
    ly_create_alias(NAME ${gem_name}.Tools NAMESPACE Gem TARGETS Gem::${gem_name}.Editor)
    ly_create_alias(NAME ${gem_name}.Builders NAMESPACE Gem TARGETS Gem::${gem_name}.Editor Gem::${gem_name}.Builder)
endif()
ly_create_alias(NAME ${gem_name}.Clients NAMESPACE Gem TARGETS Gem::${gem_name})
ly_create_alias(NAME ${gem_name}.Servers NAMESPACE Gem TARGETS Gem::${gem_name})
ly_create_alias(NAME ${gem_name}.Unified NAMESPACE Gem TARGETS Gem::${gem_name})
o3de_add_variant_dependencies_for_gem_dependencies(GEM_NAME ${gem_name} VARIANTS Builders Tools Clients Servers Unified)
