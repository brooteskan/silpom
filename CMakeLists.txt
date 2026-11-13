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
    add_executable(silpom_decal_paint_tests Tests/DecalPaintTests.cpp)
    target_compile_features(silpom_decal_paint_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.DecalPaint COMMAND silpom_decal_paint_tests)
    add_executable(silpom_height_precision_tests Tests/HeightPrecisionTests.cpp)
    target_compile_features(silpom_height_precision_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.HeightPrecision COMMAND silpom_height_precision_tests)
    add_executable(silpom_visibility_tests Tests/VisibilityTests.cpp)
    target_compile_features(silpom_visibility_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(silpom_visibility_tests PRIVATE /W4 /fp:precise)
    endif()
    add_test(NAME SilPOM.Visibility COMMAND silpom_visibility_tests)
    add_executable(silpom_material_tests Tests/MaterialCodecTests.cpp)
    target_compile_features(silpom_material_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.MaterialCodec COMMAND silpom_material_tests)
    add_executable(silpom_relief_lod_tests Tests/ReliefLodTests.cpp)
    target_compile_features(silpom_relief_lod_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.ReliefLod COMMAND silpom_relief_lod_tests)
    if(WIN32)
        add_library(silpom_level_one SHARED Tools/level_one_evaluator.cpp)
        add_library(silpom_welded_teacher SHARED Tools/welded_tower_teacher.cpp)
        add_library(silpom_visibility SHARED Tools/visibility_evaluator.cpp)
        add_library(silpom_appearance_reference SHARED Tools/appearance_reference.cpp)
        foreach(tool IN ITEMS silpom_level_one silpom_welded_teacher silpom_visibility silpom_appearance_reference)
            target_compile_features(${tool} PRIVATE cxx_std_17)
            target_include_directories(${tool} PRIVATE Include)
            if(MSVC)
                target_compile_options(${tool} PRIVATE /W4 /fp:precise)
            endif()
        endforeach()
        set(SILPOM_PYTHON "" CACHE FILEPATH "Python with numpy/Pillow for Level 1 evaluation tests")
        if(SILPOM_PYTHON)
            add_test(NAME SilPOM.AppearanceReference COMMAND "${SILPOM_PYTHON}"
                ${CMAKE_CURRENT_SOURCE_DIR}/Tests/AppearanceReferenceTests.py $<TARGET_FILE_DIR:silpom_appearance_reference>)
            add_test(NAME SilPOM.LevelOne COMMAND "${SILPOM_PYTHON}"
                ${CMAKE_CURRENT_SOURCE_DIR}/Tests/LevelOneTests.py $<TARGET_FILE_DIR:silpom_level_one>)
            add_test(NAME SilPOM.LevelTwo COMMAND "${SILPOM_PYTHON}"
                ${CMAKE_CURRENT_SOURCE_DIR}/Tests/LevelTwoTests.py $<TARGET_FILE_DIR:silpom_level_one>)
        endif()
    endif()
    set(SILPOM_TORCH_PATH "" CACHE PATH "Optional isolated PyTorch directory for learned-material training/filtering tests")
    if(SILPOM_PYTHON AND SILPOM_TORCH_PATH)
        add_test(NAME SilPOM.MaterialFiltering COMMAND "${CMAKE_COMMAND}" -E env "PYTHONPATH=${SILPOM_TORCH_PATH}"
            "${SILPOM_PYTHON}" ${CMAKE_CURRENT_SOURCE_DIR}/Tests/MaterialFilteringTests.py)
    endif()
    set(SILPOM_DXC "" CACHE FILEPATH "DXC executable for optional Windows GPU conformance tests")
    option(SILPOM_DXR_TESTS "Build optional procedural ray tracing conformance test" OFF)
    if(WIN32 AND SILPOM_DXC)
        add_executable(silpom_appearance_gpu_tests Tests/AppearanceGpuTests.cpp)
        target_compile_features(silpom_appearance_gpu_tests PRIVATE cxx_std_17)
        target_compile_definitions(silpom_appearance_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(silpom_appearance_gpu_tests PRIVATE d3d12 dxgi dxguid)
        foreach(profile IN ITEMS 6_2 6_7)
            set(appearance_shader ${CMAKE_CURRENT_BINARY_DIR}/appearance-${profile}.dxil)
            add_custom_command(OUTPUT ${appearance_shader}
                COMMAND "${SILPOM_DXC}" -T cs_${profile} -E Main -Fo ${appearance_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/AppearanceGpu.hlsl
                DEPENDS Tests/AppearanceGpu.hlsl Assets/Shaders/SilPOM/AppearanceDecode.azsli Assets/Shaders/SilPOM/ExperimentalDecode.azsli VERBATIM)
            add_custom_target(silpom_appearance_shader_${profile} DEPENDS ${appearance_shader})
            add_dependencies(silpom_appearance_gpu_tests silpom_appearance_shader_${profile})
            add_test(NAME SilPOM.Appearance.GPU.SM${profile} COMMAND silpom_appearance_gpu_tests ${appearance_shader})
        endforeach()
        if(SILPOM_PYTHON)
            add_test(NAME SilPOM.VisibilityOnly COMMAND "${SILPOM_PYTHON}"
                ${CMAKE_CURRENT_SOURCE_DIR}/Tests/VisibilityOnlyTests.py $<TARGET_FILE_DIR:silpom_visibility> "${SILPOM_DXC}")
        endif()
        add_executable(silpom_material_gpu_tests Tests/MaterialCodecGpuTests.cpp)
        target_compile_features(silpom_material_gpu_tests PRIVATE cxx_std_17)
        target_compile_definitions(silpom_material_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(silpom_material_gpu_tests PRIVATE d3d12 dxgi dxguid)
        foreach(profile IN ITEMS 6_2 6_7)
            set(material_shader ${CMAKE_CURRENT_BINARY_DIR}/material-${profile}.dxil)
            add_custom_command(OUTPUT ${material_shader}
                COMMAND "${SILPOM_DXC}" -T cs_${profile} -E Main -Fo ${material_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/MaterialCodecGpu.hlsl
                DEPENDS Tests/MaterialCodecGpu.hlsl Assets/Shaders/SilPOM/MaterialDecode.azsli VERBATIM)
            add_custom_target(silpom_material_shader_${profile} DEPENDS ${material_shader})
            add_dependencies(silpom_material_gpu_tests silpom_material_shader_${profile})
            add_test(NAME SilPOM.MaterialCodec.GPU.SM${profile} COMMAND silpom_material_gpu_tests ${material_shader})
        endforeach()
        add_executable(silpom_visibility_gpu_tests Tests/VisibilityGpuTests.cpp)
        target_compile_features(silpom_visibility_gpu_tests PRIVATE cxx_std_17)
        target_compile_definitions(silpom_visibility_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(silpom_visibility_gpu_tests PRIVATE d3d12 dxgi dxguid)
        foreach(profile IN ITEMS 6_2 6_7)
            set(visibility_shader ${CMAKE_CURRENT_BINARY_DIR}/visibility-${profile}.dxil)
            add_custom_command(OUTPUT ${visibility_shader}
                COMMAND "${SILPOM_DXC}" -T cs_${profile} -E Main -Fo ${visibility_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/VisibilityGpu.hlsl
                DEPENDS Tests/VisibilityGpu.hlsl Assets/Shaders/SilPOM/LevelThree.azsli
                    Assets/Shaders/SilPOM/VisibilityInputs.azsli Assets/Shaders/SilPOM/VisibilityClassificationThresholds.azsli
                    Assets/Shaders/SilPOM/NeuralVisibilityWeights.azsli Assets/Shaders/SilPOM/ReliefShadow.azsli
                    Assets/Shaders/SilPOM/ValidationHeight.azsli Assets/Shaders/SilPOM/BlendedFace.azsli
                    Assets/Shaders/SilPOM/PlanarFace.azsli Assets/Shaders/SilPOM/Heightfield.azsli VERBATIM)
            add_custom_target(silpom_visibility_shader_${profile} DEPENDS ${visibility_shader})
            add_dependencies(silpom_visibility_gpu_tests silpom_visibility_shader_${profile})
            add_test(NAME SilPOM.Visibility.GPU.SM${profile} COMMAND silpom_visibility_gpu_tests ${visibility_shader})
            set(visibility_only_shader ${CMAKE_CURRENT_BINARY_DIR}/visibility-only-${profile}.dxil)
            add_custom_command(OUTPUT ${visibility_only_shader}
                COMMAND "${SILPOM_DXC}" -T cs_${profile} -D SILPOM_NEURAL_VISIBILITY_ONLY=1 -E Main
                    -Fo ${visibility_only_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/VisibilityGpu.hlsl
                DEPENDS Tests/VisibilityGpu.hlsl Assets/Shaders/SilPOM/NeuralVisibility.azsli
                    Assets/Shaders/SilPOM/VisibilityInputs.azsli Assets/Shaders/SilPOM/VisibilityClassificationThresholds.azsli
                    Assets/Shaders/SilPOM/NeuralVisibilityWeights.azsli Assets/Shaders/SilPOM/ReliefShadow.azsli
                    Assets/Shaders/SilPOM/ValidationHeight.azsli Assets/Shaders/SilPOM/BlendedFace.azsli
                    Assets/Shaders/SilPOM/PlanarFace.azsli Assets/Shaders/SilPOM/Heightfield.azsli VERBATIM)
            add_custom_target(silpom_visibility_only_shader_${profile} DEPENDS ${visibility_only_shader})
            add_dependencies(silpom_visibility_gpu_tests silpom_visibility_only_shader_${profile})
            add_test(NAME SilPOM.VisibilityOnly.GPU.SM${profile}
                COMMAND silpom_visibility_gpu_tests ${visibility_only_shader} only)
        endforeach()
        add_executable(silpom_level_one_gpu_tests Tests/LevelOneGpuTests.cpp)
        target_compile_features(silpom_level_one_gpu_tests PRIVATE cxx_std_17)
        target_compile_definitions(silpom_level_one_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(silpom_level_one_gpu_tests PRIVATE d3d12 dxgi dxguid)
        foreach(profile IN ITEMS 6_2 6_7)
            set(level_one_shader ${CMAKE_CURRENT_BINARY_DIR}/level-one-${profile}.dxil)
            add_custom_command(OUTPUT ${level_one_shader}
                COMMAND "${SILPOM_DXC}" -T cs_${profile} -E Main -Fo ${level_one_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/LevelOneGpu.hlsl
                DEPENDS Tests/LevelOneGpu.hlsl Assets/Shaders/SilPOM/EvaluationHooks.azsli
                    Assets/Shaders/SilPOM/BlendedFace.azsli Assets/Shaders/SilPOM/LevelTwo.azsli
                    Assets/Shaders/SilPOM/NeuralTowerLevelTwoWeights.azsli
                    Assets/Shaders/SilPOM/NeuralOpenShaftTowerWeights.azsli
                    Assets/Shaders/SilPOM/NeuralWeldedTowerWeights.azsli VERBATIM)
            add_custom_target(silpom_level_one_shader_${profile} DEPENDS ${level_one_shader})
            add_dependencies(silpom_level_one_gpu_tests silpom_level_one_shader_${profile})
            add_test(NAME SilPOM.LevelOne.GPU.SM${profile}
                COMMAND silpom_level_one_gpu_tests ${level_one_shader})
            set(level_two_shader ${CMAKE_CURRENT_BINARY_DIR}/level-two-${profile}.dxil)
            add_custom_command(OUTPUT ${level_two_shader}
                COMMAND "${SILPOM_DXC}" -T cs_${profile} -D SILPOM_LEVEL_TWO_TEST=1 -E Main
                    -Fo ${level_two_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/LevelOneGpu.hlsl
                DEPENDS Tests/LevelOneGpu.hlsl Assets/Shaders/SilPOM/LevelTwo.azsli
                    Assets/Shaders/SilPOM/NeuralTowerLevelTwoWeights.azsli
                    Assets/Shaders/SilPOM/NeuralOpenShaftTowerWeights.azsli
                    Assets/Shaders/SilPOM/NeuralWeldedTowerWeights.azsli
                    Assets/Shaders/SilPOM/BlendedFace.azsli VERBATIM)
            add_custom_target(silpom_level_two_shader_${profile} DEPENDS ${level_two_shader})
            add_dependencies(silpom_level_one_gpu_tests silpom_level_two_shader_${profile})
            add_test(NAME SilPOM.LevelTwo.GPU.SM${profile}
                COMMAND silpom_level_one_gpu_tests ${level_two_shader} 2)
        endforeach()
        add_executable(silpom_validation_gather_gpu_tests Tests/ValidationGatherGpuTests.cpp)
        target_compile_features(silpom_validation_gather_gpu_tests PRIVATE cxx_std_17)
        target_compile_definitions(silpom_validation_gather_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(silpom_validation_gather_gpu_tests PRIVATE d3d12 dxgi dxguid)
        foreach(profile IN ITEMS 6_2 6_7)
            set(gather_shader ${CMAKE_CURRENT_BINARY_DIR}/validation-gather-${profile}.dxil)
            add_custom_command(OUTPUT ${gather_shader}
                COMMAND "${SILPOM_DXC}" -T cs_${profile} -E Main -Fo ${gather_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ValidationGatherGpu.hlsl
                DEPENDS Tests/ValidationGatherGpu.hlsl Assets/Shaders/SilPOM/ValidationHeight.azsli
                    Assets/Shaders/SilPOM/BlendedFace.azsli Assets/Shaders/SilPOM/Heightfield.azsli VERBATIM)
            add_custom_target(silpom_gather_shader_${profile} DEPENDS ${gather_shader})
            add_dependencies(silpom_validation_gather_gpu_tests silpom_gather_shader_${profile})
            add_test(NAME SilPOM.ValidationGather.SM${profile}
                COMMAND silpom_validation_gather_gpu_tests ${gather_shader})
        endforeach()
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
