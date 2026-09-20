cmake_minimum_required(VERSION 3.24)
if(NOT COMMAND o3de_gem_setup)
    project(SilPOMCore LANGUAGES CXX)
    enable_testing()
    add_executable(silpom_core_tests Tests/HeightfieldTests.cpp)
    target_compile_features(silpom_core_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(silpom_core_tests PRIVATE /W4 /fp:precise)
    endif()
    add_test(NAME SilPOM.Core COMMAND silpom_core_tests)
    add_executable(silpom_face_tests Tests/PlanarFaceTests.cpp)
    target_compile_features(silpom_face_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.PlanarFaces COMMAND silpom_face_tests)
    add_executable(silpom_normal_tests Tests/FaceNormalsTests.cpp)
    target_compile_features(silpom_normal_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.FaceNormals COMMAND silpom_normal_tests)
    add_executable(silpom_blended_artifact_tests Tests/BlendedFaceArtifactTests.cpp)
    target_compile_features(silpom_blended_artifact_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.BlendedArtifacts COMMAND silpom_blended_artifact_tests)
    add_executable(silpom_relief_shadow_tests Tests/ReliefShadowTests.cpp)
    target_compile_features(silpom_relief_shadow_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.ReliefShadows COMMAND silpom_relief_shadow_tests)
    set(SILPOM_DXC "" CACHE FILEPATH "DXC executable for optional Windows GPU conformance tests")
    option(SILPOM_DXR_TESTS "Build optional procedural ray tracing conformance test" OFF)
    if(WIN32 AND SILPOM_DXC)
        add_executable(silpom_gpu_tests Tests/GpuTests.cpp)
        target_compile_features(silpom_gpu_tests PRIVATE cxx_std_17)
        target_compile_definitions(silpom_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(silpom_gpu_tests PRIVATE d3d12 dxgi dxguid)
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/compute.dxil
            COMMAND "${SILPOM_DXC}" -T cs_6_3 -E Main -Fo ${CMAKE_CURRENT_BINARY_DIR}/compute.dxil ${CMAKE_CURRENT_SOURCE_DIR}/Tests/HeightfieldGpu.hlsl
            DEPENDS Tests/HeightfieldGpu.hlsl Assets/Shaders/SilPOM/Heightfield.azsli VERBATIM)
        add_custom_target(silpom_gpu_shaders DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/compute.dxil)
        add_dependencies(silpom_gpu_tests silpom_gpu_shaders)
        add_test(NAME SilPOM.GPU COMMAND silpom_gpu_tests ${CMAKE_CURRENT_BINARY_DIR}/compute.dxil)
        if(SILPOM_DXR_TESTS)
            target_compile_definitions(silpom_gpu_tests PRIVATE SILPOM_DXR_TESTS=1)
            add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/rays.dxil
                COMMAND "${SILPOM_DXC}" -T lib_6_3 -D SILPOM_RT=1 -Fo ${CMAKE_CURRENT_BINARY_DIR}/rays.dxil ${CMAKE_CURRENT_SOURCE_DIR}/Tests/HeightfieldGpu.hlsl
                DEPENDS Tests/HeightfieldGpu.hlsl Assets/Shaders/SilPOM/Heightfield.azsli VERBATIM)
            add_custom_target(silpom_rt_shader DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/rays.dxil)
            add_dependencies(silpom_gpu_tests silpom_rt_shader)
            add_test(NAME SilPOM.DXR COMMAND silpom_gpu_tests ${CMAKE_CURRENT_BINARY_DIR}/compute.dxil ${CMAKE_CURRENT_BINARY_DIR}/rays.dxil)
        endif()
    endif()
    return()
endif()

o3de_gem_setup("SilPOM")
ly_add_target(NAME ${gem_name}.Static STATIC NAMESPACE Gem
    FILES_CMAKE silpom_files.cmake
    INCLUDE_DIRECTORIES PUBLIC Include PRIVATE Source
    BUILD_DEPENDENCIES PUBLIC AZ::AzCore AZ::AzFramework Gem::Atom_RPI.Public Gem::Atom_Feature_Common.Public)
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
