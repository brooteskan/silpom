cmake_minimum_required(VERSION 3.24)
if(NOT COMMAND o3de_gem_setup)
    project(SilPOMCore LANGUAGES CXX)
    enable_testing()
    add_executable(silpom_resolve_region_tests Tests/ResolveRegionTests.cpp)
    target_compile_features(silpom_resolve_region_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.ResolveRegion COMMAND silpom_resolve_region_tests)
    add_executable(silpom_resolve_regions_tests Tests/ResolveRegionsTests.cpp)
    target_compile_features(silpom_resolve_regions_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.ResolveRegions COMMAND silpom_resolve_regions_tests)
    add_executable(silpom_repair_region_tests Tests/RepairRegionTests.cpp)
    target_compile_features(silpom_repair_region_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(silpom_repair_region_tests PRIVATE /W4 /fp:precise)
    endif()
    add_test(NAME SilPOM.RepairRegion COMMAND silpom_repair_region_tests)
    add_executable(silpom_repair_execution_tests Tests/RepairExecutionTests.cpp)
    target_compile_features(silpom_repair_execution_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(silpom_repair_execution_tests PRIVATE /W4 /fp:precise)
    endif()
    add_test(NAME SilPOM.RepairExecution COMMAND silpom_repair_execution_tests)
    add_executable(silpom_gradient_context_tests Tests/GradientContextTests.cpp)
    target_compile_features(silpom_gradient_context_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(silpom_gradient_context_tests PRIVATE /W4 /fp:precise)
    endif()
    add_test(NAME SilPOM.GradientContext COMMAND silpom_gradient_context_tests)
    add_executable(silpom_repair_gradient_context_tests Tests/RepairGradientContextTests.cpp)
    target_compile_features(silpom_repair_gradient_context_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(silpom_repair_gradient_context_tests PRIVATE /W4 /fp:precise)
    endif()
    add_test(NAME SilPOM.RepairGradientContext COMMAND silpom_repair_gradient_context_tests)
    add_executable(silpom_normal_query_policy_tests Tests/NormalQueryPolicyTests.cpp)
    target_compile_features(silpom_normal_query_policy_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(silpom_normal_query_policy_tests PRIVATE /W4 /fp:precise)
    endif()
    add_test(NAME SilPOM.NormalQueryPolicy COMMAND silpom_normal_query_policy_tests)
    add_executable(silpom_face_coefficient_tests Tests/FaceCoefficientTests.cpp)
    target_compile_features(silpom_face_coefficient_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(silpom_face_coefficient_tests PRIVATE /W4 /fp:precise)
    endif()
    set(face_coefficient_fixture ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Reference/FaceCoefficientTowerPacked.bin)
    add_test(NAME SilPOM.FaceCoefficients COMMAND silpom_face_coefficient_tests ${face_coefficient_fixture})
    add_executable(silpom_refinement_height_cache_tests Tests/RefinementHeightCacheTests.cpp)
    target_compile_features(silpom_refinement_height_cache_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(silpom_refinement_height_cache_tests PRIVATE /W4 /fp:precise)
    endif()
    add_test(NAME SilPOM.RefinementHeightCache COMMAND silpom_refinement_height_cache_tests)
    add_executable(silpom_mesh_setting_rules_tests Tests/MeshSettingRulesTests.cpp)
    target_compile_features(silpom_mesh_setting_rules_tests PRIVATE cxx_std_20)
    add_test(NAME SilPOM.MeshSettingRules COMMAND silpom_mesh_setting_rules_tests)
    add_executable(silpom_strict_neural_policy_tests Tests/StrictNeuralPolicyTests.cpp)
    target_compile_features(silpom_strict_neural_policy_tests PRIVATE cxx_std_17)
    add_executable(silpom_neural_receiver_tests Tests/NeuralReceiverTests.cpp)
    target_compile_features(silpom_neural_receiver_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(silpom_strict_neural_policy_tests PRIVATE /W4 /fp:precise)
        target_compile_options(silpom_neural_receiver_tests PRIVATE /W4 /fp:precise)
    endif()
    add_test(NAME SilPOM.StrictNeuralPolicy COMMAND silpom_strict_neural_policy_tests)
    add_test(NAME SilPOM.NeuralReceiver COMMAND silpom_neural_receiver_tests)
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
    add_executable(silpom_receiver_surface_tests Tests/ReceiverSurfaceTests.cpp)
    target_compile_features(silpom_receiver_surface_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.ReceiverSurface COMMAND silpom_receiver_surface_tests)
    add_executable(silpom_receiver_paint_state_tests Tests/ReceiverPaintStateTests.cpp)
    target_compile_features(silpom_receiver_paint_state_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.ReceiverPaintState COMMAND silpom_receiver_paint_state_tests)
    add_executable(silpom_surface_paint_math_tests Tests/SurfacePaintMathTests.cpp)
    target_compile_features(silpom_surface_paint_math_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.SurfacePaintMath COMMAND silpom_surface_paint_math_tests)
    add_executable(silpom_surface_paint_image_tests Tests/SurfacePaintImageTests.cpp)
    target_compile_features(silpom_surface_paint_image_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.SurfacePaintImage COMMAND silpom_surface_paint_image_tests)
    add_executable(silpom_surface_paint_layout_tests Tests/SurfacePaintLayoutTests.cpp)
    target_compile_features(silpom_surface_paint_layout_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.SurfacePaintLayout COMMAND silpom_surface_paint_layout_tests)
    add_executable(silpom_surface_brush_shape_tests Tests/SurfaceBrushShapeTests.cpp)
    target_compile_features(silpom_surface_brush_shape_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.SurfaceBrushShapes COMMAND silpom_surface_brush_shape_tests)
    add_executable(silpom_camera_collision_tests Tests/CameraCollisionMathTests.cpp)
    target_compile_features(silpom_camera_collision_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.CameraCollision COMMAND silpom_camera_collision_tests)
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
    add_executable(silpom_nearest_hit_tests Tests/NearestHitTests.cpp)
    target_compile_features(silpom_nearest_hit_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.NearestHit COMMAND silpom_nearest_hit_tests)
    add_executable(silpom_nearest_hit_equivalence_tests Tests/NearestHitEquivalenceTests.cpp)
    target_compile_features(silpom_nearest_hit_equivalence_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.NearestHit.Equivalence COMMAND silpom_nearest_hit_equivalence_tests)
    add_executable(silpom_msaa_address_tests Tests/MsaaAddressTests.cpp)
    target_compile_features(silpom_msaa_address_tests PRIVATE cxx_std_17)
    target_compile_definitions(silpom_msaa_address_tests PRIVATE SILPOM_MSAA_FAST_ADDRESSING=1)
    add_test(NAME SilPOM.MsaaAddressing COMMAND silpom_msaa_address_tests)
    # Default exact NPOT addressing must not depend on an MSAA-only define.
    add_executable(silpom_address_tests Tests/MsaaAddressTests.cpp)
    target_compile_features(silpom_address_tests PRIVATE cxx_std_17)
    add_test(NAME SilPOM.Addressing COMMAND silpom_address_tests)
    add_executable(silpom_generic_address_tests Tests/MsaaAddressTests.cpp)
    target_compile_features(silpom_generic_address_tests PRIVATE cxx_std_17)
    target_compile_definitions(silpom_generic_address_tests PRIVATE SILPOM_GENERIC_REMAINDER=1)
    add_test(NAME SilPOM.Addressing.Generic COMMAND silpom_generic_address_tests)
    add_executable(silpom_reference_address_tests Tests/MsaaAddressTests.cpp)
    target_compile_features(silpom_reference_address_tests PRIVATE cxx_std_17)
    target_compile_definitions(silpom_reference_address_tests PRIVATE SILPOM_FAST_ADDRESSING=0)
    add_test(NAME SilPOM.Addressing.Reference COMMAND silpom_reference_address_tests)
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
        add_test(NAME SilPOM.ReceiverUvAnalysis COMMAND "${SILPOM_PYTHON}"
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests/test_receiver_uv_analysis.py)
        add_test(NAME SilPOM.NativePaintShader COMMAND "${SILPOM_PYTHON}"
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests/test_native_paint_shader.py)
        add_test(NAME SilPOM.SurfaceTextureAnalysis COMMAND "${SILPOM_PYTHON}"
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests/test_surface_texture_analysis.py)
        add_test(NAME SilPOM.DirectionalEstimator.Models COMMAND "${SILPOM_PYTHON}"
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests/DirectionalEstimatorModelTests.py)
            add_test(NAME SilPOM.DirectionalOcclusion COMMAND "${SILPOM_PYTHON}"
                ${CMAKE_CURRENT_SOURCE_DIR}/Tests/DirectionalOcclusionTests.py)
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
        add_executable(silpom_surface_normal_gpu_tests Tests/SurfacePaintNormalGpuTests.cpp)
        target_compile_features(silpom_surface_normal_gpu_tests PRIVATE cxx_std_17)
        target_compile_definitions(silpom_surface_normal_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(silpom_surface_normal_gpu_tests PRIVATE d3d12 dxgi dxguid)
        foreach(profile IN ITEMS 6_2 6_7)
            set(surface_normal_shader ${CMAKE_CURRENT_BINARY_DIR}/surface-normal-${profile}.dxil)
            add_custom_command(OUTPUT ${surface_normal_shader}
                COMMAND "${SILPOM_DXC}" -T cs_${profile} -E Main -Fo ${surface_normal_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/SurfacePaintNormalGpu.hlsl
                DEPENDS Tests/SurfacePaintNormalGpu.hlsl Assets/Shaders/SilPOM/ReceiverUv/SurfaceNormal.azsli VERBATIM)
            add_custom_target(silpom_surface_normal_shader_${profile} DEPENDS ${surface_normal_shader})
            add_dependencies(silpom_surface_normal_gpu_tests silpom_surface_normal_shader_${profile})
            add_test(NAME SilPOM.SurfaceNormal.GPU.SM${profile} COMMAND silpom_surface_normal_gpu_tests ${surface_normal_shader})
            set(surface_texture_shader ${CMAKE_CURRENT_BINARY_DIR}/surface-texture-${profile}.dxil)
            add_custom_command(OUTPUT ${surface_texture_shader}
                COMMAND "${SILPOM_DXC}" -T ps_${profile} -E MainPS -Fo ${surface_texture_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/SurfaceTextureGpu.hlsl
                DEPENDS Tests/SurfaceTextureGpu.hlsl Assets/Shaders/SilPOM/ReceiverUv/SurfaceTexture.azsli
                    Assets/Shaders/SilPOM/ReceiverUv/SurfaceNormal.azsli VERBATIM)
            add_custom_target(silpom_surface_texture_shader_${profile} DEPENDS ${surface_texture_shader})
            add_dependencies(silpom_surface_normal_gpu_tests silpom_surface_texture_shader_${profile})
        endforeach()
        add_executable(silpom_implicit_proxy_cache_gpu_tests Tests/ImplicitProxyCacheGpuTests.cpp)
        target_compile_features(silpom_implicit_proxy_cache_gpu_tests PRIVATE cxx_std_17)
        target_compile_definitions(silpom_implicit_proxy_cache_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(silpom_implicit_proxy_cache_gpu_tests PRIVATE d3d12 dxgi dxguid)
        foreach(profile IN ITEMS 6_2 6_7)
            set(implicit_proxy_profile_shaders)
            foreach(entry IN ITEMS PrepareCS CompareCS)
                set(implicit_proxy_shader ${CMAKE_CURRENT_BINARY_DIR}/implicit-proxy-cache-${entry}-${profile}.dxil)
                add_custom_command(OUTPUT ${implicit_proxy_shader}
                    COMMAND "${SILPOM_DXC}" -O3 -T cs_${profile} -E ${entry} -Fo ${implicit_proxy_shader}
                        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ImplicitProxyCacheGpu.hlsl
                    DEPENDS Tests/ImplicitProxyCacheGpu.hlsl Tests/Reference/ImplicitProxyCoverageLegacy.hlsl
                        Assets/Shaders/SilPOM/ImplicitProxyCoverage.azsli Assets/Shaders/SilPOM/FrustumDepthBound.azsli
                        Assets/Shaders/SilPOM/HitReuse/SilPOMFaceCacheRead.azsli Assets/Shaders/SilPOM/FaceCoefficients.azsli
                        Assets/Shaders/SilPOM/MeshFace.azsli Assets/Shaders/SilPOM/Quality.azsli VERBATIM)
                list(APPEND implicit_proxy_profile_shaders ${implicit_proxy_shader})
            endforeach()
            set(implicit_proxy_empty_shader ${CMAKE_CURRENT_BINARY_DIR}/implicit-proxy-cache-CompareEmpty-${profile}.dxil)
            add_custom_command(OUTPUT ${implicit_proxy_empty_shader}
                COMMAND "${SILPOM_DXC}" -O3 -T cs_${profile} -E CompareCS -D SILPOM_TEST_EMPTY_TABLE=1
                    -Fo ${implicit_proxy_empty_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ImplicitProxyCacheGpu.hlsl
                DEPENDS Tests/ImplicitProxyCacheGpu.hlsl Tests/Reference/ImplicitProxyCoverageLegacy.hlsl
                    Assets/Shaders/SilPOM/ImplicitProxyCoverage.azsli Assets/Shaders/SilPOM/FrustumDepthBound.azsli
                    Assets/Shaders/SilPOM/HitReuse/SilPOMFaceCacheRead.azsli Assets/Shaders/SilPOM/FaceCoefficients.azsli
                    Assets/Shaders/SilPOM/MeshFace.azsli Assets/Shaders/SilPOM/Quality.azsli VERBATIM)
            list(APPEND implicit_proxy_profile_shaders ${implicit_proxy_empty_shader})
            add_custom_target(silpom_implicit_proxy_cache_shaders_${profile} DEPENDS ${implicit_proxy_profile_shaders})
            add_dependencies(silpom_implicit_proxy_cache_gpu_tests silpom_implicit_proxy_cache_shaders_${profile})
            add_test(NAME SilPOM.ImplicitProxyCache.GPU.SM${profile} COMMAND silpom_implicit_proxy_cache_gpu_tests
                ${CMAKE_CURRENT_BINARY_DIR}/implicit-proxy-cache-PrepareCS-${profile}.dxil
                ${CMAKE_CURRENT_BINARY_DIR}/implicit-proxy-cache-CompareCS-${profile}.dxil)
            add_test(NAME SilPOM.ImplicitProxyCache.GPU.SM${profile}.EmptyTable COMMAND silpom_implicit_proxy_cache_gpu_tests
                ${CMAKE_CURRENT_BINARY_DIR}/implicit-proxy-cache-PrepareCS-${profile}.dxil
                ${implicit_proxy_empty_shader} --empty-table)
        endforeach()
        add_executable(silpom_face_coefficient_gpu_tests Tests/FaceCoefficientGpuTests.cpp)
        target_compile_features(silpom_face_coefficient_gpu_tests PRIVATE cxx_std_17)
        target_compile_definitions(silpom_face_coefficient_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(silpom_face_coefficient_gpu_tests PRIVATE d3d12 dxgi dxguid)
        set(face_coefficient_shader_dependencies Tests/FaceCoefficientGpu.hlsl Tests/FaceCoefficientLegacy.azsli
            Assets/Shaders/SilPOM/Heightfield.azsli Assets/Shaders/SilPOM/PlanarFace.azsli
            Assets/Shaders/SilPOM/BlendedFace.azsli Assets/Shaders/SilPOM/MeshFace.azsli
            Assets/Shaders/SilPOM/FaceCoefficients.azsli Assets/Shaders/SilPOM/GradientContext.azsli
            Assets/Shaders/SilPOM/Quality.azsli Assets/Shaders/SilPOM/EvaluationHooks.azsli)
        foreach(profile IN ITEMS 6_2 6_7)
            set(face_prepare_shader ${CMAKE_CURRENT_BINARY_DIR}/face-coefficient-prepare-${profile}.dxil)
            add_custom_command(OUTPUT ${face_prepare_shader}
                COMMAND "${SILPOM_DXC}" -O3 -T cs_${profile} -E Prepare -Fo ${face_prepare_shader}
                    ${CMAKE_CURRENT_SOURCE_DIR}/Tests/FaceCoefficientGpu.hlsl
                DEPENDS ${face_coefficient_shader_dependencies} VERBATIM)
            set(face_coefficient_profile_shaders ${face_prepare_shader})
            foreach(mode IN ITEMS Cached Missing Disabled)
                string(TOLOWER ${mode} face_coefficient_mode)
                set(face_compare_shader ${CMAKE_CURRENT_BINARY_DIR}/face-coefficient-${face_coefficient_mode}-${profile}.dxil)
                set(face_coefficient_defines)
                if(mode STREQUAL "Missing")
                    set(face_coefficient_defines -D SILPOM_TEST_FORCE_MISSING_CACHE=1)
                elseif(mode STREQUAL "Disabled")
                    set(face_coefficient_defines -D SILPOM_TEST_DISABLE_CACHE=1)
                endif()
                add_custom_command(OUTPUT ${face_compare_shader}
                    COMMAND "${SILPOM_DXC}" -O3 -T cs_${profile} -E Compare ${face_coefficient_defines}
                        -Fo ${face_compare_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/FaceCoefficientGpu.hlsl
                    DEPENDS ${face_coefficient_shader_dependencies} VERBATIM)
                list(APPEND face_coefficient_profile_shaders ${face_compare_shader})
                add_test(NAME SilPOM.FaceCoefficients.GPU.${mode}.SM${profile}
                    COMMAND silpom_face_coefficient_gpu_tests ${face_prepare_shader} ${face_compare_shader} ${face_coefficient_fixture})
            endforeach()
            add_custom_target(silpom_face_coefficient_shaders_${profile} DEPENDS ${face_coefficient_profile_shaders})
            add_dependencies(silpom_face_coefficient_gpu_tests silpom_face_coefficient_shaders_${profile})
        endforeach()
        add_executable(silpom_gradient_context_gpu_tests Tests/GradientContextGpuTests.cpp)
        target_compile_features(silpom_gradient_context_gpu_tests PRIVATE cxx_std_17)
        target_compile_definitions(silpom_gradient_context_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(silpom_gradient_context_gpu_tests PRIVATE d3d12 dxgi dxguid)
        foreach(profile IN ITEMS 6_2 6_7)
            set(gradient_shader ${CMAKE_CURRENT_BINARY_DIR}/gradient-context-${profile}.dxil)
            add_custom_command(OUTPUT ${gradient_shader}
                COMMAND "${SILPOM_DXC}" -T cs_${profile} -E Main -Fo ${gradient_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/GradientContextGpu.hlsl
                DEPENDS Tests/GradientContextGpu.hlsl Assets/Shaders/SilPOM/GradientContext.azsli VERBATIM)
            add_custom_target(silpom_gradient_context_shader_${profile} DEPENDS ${gradient_shader})
            add_dependencies(silpom_gradient_context_gpu_tests silpom_gradient_context_shader_${profile})
            add_test(NAME SilPOM.GradientContext.GPU.SM${profile} COMMAND silpom_gradient_context_gpu_tests ${gradient_shader})
            set(repair_gradient_shader ${CMAKE_CURRENT_BINARY_DIR}/repair-gradient-context-${profile}.dxil)
            add_custom_command(OUTPUT ${repair_gradient_shader}
                COMMAND "${SILPOM_DXC}" -T cs_${profile} -E Main -Fo ${repair_gradient_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/RepairGradientContextGpu.hlsl
                DEPENDS Tests/RepairGradientContextGpu.hlsl Tests/GradientContextGpu.hlsl
                    Assets/Shaders/SilPOM/RepairGradientContext.azsli Assets/Shaders/SilPOM/GradientContext.azsli VERBATIM)
            add_custom_target(silpom_repair_gradient_context_shader_${profile} DEPENDS ${repair_gradient_shader})
            add_dependencies(silpom_gradient_context_gpu_tests silpom_repair_gradient_context_shader_${profile})
            add_test(NAME SilPOM.RepairGradientContext.GPU.SM${profile} COMMAND silpom_gradient_context_gpu_tests ${repair_gradient_shader})
            set(normal_query_shader ${CMAKE_CURRENT_BINARY_DIR}/normal-query-policy-${profile}.dxil)
            add_custom_command(OUTPUT ${normal_query_shader}
                COMMAND "${SILPOM_DXC}" -T cs_${profile} -E Main -Fo ${normal_query_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/NormalQueryPolicyGpu.hlsl
                DEPENDS Tests/NormalQueryPolicyGpu.hlsl Tests/GradientContextGpu.hlsl
                    Assets/Shaders/SilPOM/NormalQueryPolicy.azsli Assets/Shaders/SilPOM/GradientConsumers.azsli
                    Assets/Shaders/SilPOM/GradientContext.azsli VERBATIM)
            add_custom_target(silpom_normal_query_policy_shader_${profile} DEPENDS ${normal_query_shader})
            add_dependencies(silpom_gradient_context_gpu_tests silpom_normal_query_policy_shader_${profile})
            add_test(NAME SilPOM.NormalQueryPolicy.GPU.SM${profile} COMMAND silpom_gradient_context_gpu_tests ${normal_query_shader})
        endforeach()
        add_executable(silpom_appearance_gpu_tests Tests/AppearanceGpuTests.cpp)
        target_compile_features(silpom_appearance_gpu_tests PRIVATE cxx_std_17)
        target_compile_definitions(silpom_appearance_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(silpom_appearance_gpu_tests PRIVATE d3d12 dxgi dxguid)
        foreach(profile IN ITEMS 6_2 6_7)
            set(appearance_shader ${CMAKE_CURRENT_BINARY_DIR}/appearance-${profile}.dxil)
            add_custom_command(OUTPUT ${appearance_shader}
                COMMAND "${SILPOM_DXC}" -T cs_${profile} -E Main -Fo ${appearance_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/AppearanceGpu.hlsl
                DEPENDS Tests/AppearanceGpu.hlsl Assets/Shaders/SilPOM/AppearanceDecode.azsli
                    Assets/Shaders/SilPOM/MaterialDecode.azsli Assets/Shaders/SilPOM/MaterialNormalDecode.azsli
                    Assets/Shaders/SilPOM/ExperimentalDecode.azsli Assets/Shaders/SilPOM/SharedVisibility.azsli VERBATIM)
            add_custom_target(silpom_appearance_shader_${profile} DEPENDS ${appearance_shader})
            add_dependencies(silpom_appearance_gpu_tests silpom_appearance_shader_${profile})
            add_test(NAME SilPOM.Appearance.GPU.SM${profile} COMMAND silpom_appearance_gpu_tests ${appearance_shader})
        endforeach()
        if(SILPOM_PYTHON)
            add_test(NAME SilPOM.VisibilityOnly COMMAND "${SILPOM_PYTHON}"
                ${CMAKE_CURRENT_SOURCE_DIR}/Tests/VisibilityOnlyTests.py $<TARGET_FILE_DIR:silpom_visibility> "${SILPOM_DXC}")
        endif()
        add_executable(silpom_directional_estimator_gpu_tests Tests/DirectionalEstimatorGpuTests.cpp)
        target_compile_features(silpom_directional_estimator_gpu_tests PRIVATE cxx_std_17)
        target_compile_definitions(silpom_directional_estimator_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(silpom_directional_estimator_gpu_tests PRIVATE d3d12 dxgi dxguid)
        foreach(profile IN ITEMS 6_2 6_7)
            set(directional_shader ${CMAKE_CURRENT_BINARY_DIR}/directional-estimator-${profile}.dxil)
            add_custom_command(OUTPUT ${directional_shader}
                COMMAND "${SILPOM_DXC}" -T cs_${profile} -E Main -Fo ${directional_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/DirectionalEstimatorGpu.hlsl
                DEPENDS Tests/DirectionalEstimatorGpu.hlsl Assets/Shaders/SilPOM/DirectionalEstimator.azsli Assets/Shaders/SilPOM/DirectionalEstimatorWeights.azsli Assets/Shaders/SilPOM/AngularEdgeWeights.azsli VERBATIM)
            add_custom_target(silpom_directional_estimator_shader_${profile} DEPENDS ${directional_shader})
            add_dependencies(silpom_directional_estimator_gpu_tests silpom_directional_estimator_shader_${profile})
        endforeach()
        if(SILPOM_PYTHON)
            add_test(NAME SilPOM.DirectionalEstimator.GPU COMMAND "${SILPOM_PYTHON}"
                ${CMAKE_CURRENT_SOURCE_DIR}/Tests/DirectionalEstimatorModelTests.py ${CMAKE_CURRENT_BINARY_DIR})
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
                    Assets/Shaders/SilPOM/PhysicalShadowReach.azsli
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
                    Assets/Shaders/SilPOM/PhysicalShadowReach.azsli Assets/Shaders/SilPOM/NeuralVisibilityPhysicalWeights.azsli
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
                    Assets/Shaders/SilPOM/NearestHit.azsli Assets/Shaders/SilPOM/Heightfield.azsli
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
                    Assets/Shaders/SilPOM/NearestHit.azsli Assets/Shaders/SilPOM/Heightfield.azsli
                    Assets/Shaders/SilPOM/NeuralTowerLevelTwoWeights.azsli
                    Assets/Shaders/SilPOM/NeuralOpenShaftTowerWeights.azsli
                    Assets/Shaders/SilPOM/NeuralWeldedTowerWeights.azsli
                    Assets/Shaders/SilPOM/BlendedFace.azsli VERBATIM)
            add_custom_target(silpom_level_two_shader_${profile} DEPENDS ${level_two_shader})
            add_dependencies(silpom_level_one_gpu_tests silpom_level_two_shader_${profile})
            add_test(NAME SilPOM.LevelTwo.GPU.SM${profile}
                COMMAND silpom_level_one_gpu_tests ${level_two_shader} 2)
        endforeach()
        add_executable(silpom_refinement_height_cache_gpu_tests Tests/RefinementHeightCacheGpuTests.cpp)
        target_compile_features(silpom_refinement_height_cache_gpu_tests PRIVATE cxx_std_17)
        target_compile_definitions(silpom_refinement_height_cache_gpu_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(silpom_refinement_height_cache_gpu_tests PRIVATE d3d12 dxgi dxguid)
        foreach(profile IN ITEMS 6_2 6_7)
            set(refinement_height_profile_shaders)
            foreach(mode IN ITEMS 0 1)
              foreach(coefficients IN ITEMS 0 1)
                set(refinement_height_shader ${CMAKE_CURRENT_BINARY_DIR}/refinement-height-${mode}-coeff${coefficients}-${profile}.dxil)
                add_custom_command(OUTPUT ${refinement_height_shader}
                    COMMAND "${SILPOM_DXC}" -O3 -T cs_${profile} -E Main -D SILPOM_TEST_GATHER=${mode}
                        -D SILPOM_FACE_COEFFICIENT_CACHE=${coefficients}
                        -Fo ${refinement_height_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/RefinementHeightCacheGpu.hlsl
                    DEPENDS Tests/RefinementHeightCacheGpu.hlsl Tests/Reference/RefineFaceUncached.azsli
                        Assets/Shaders/SilPOM/BlendedFace.azsli Assets/Shaders/SilPOM/Heightfield.azsli
                        Assets/Shaders/SilPOM/PlanarFace.azsli Assets/Shaders/SilPOM/MeshFace.azsli
                        Assets/Shaders/SilPOM/FaceCoefficients.azsli Assets/Shaders/SilPOM/ValidationHeight.azsli
                        Assets/Shaders/SilPOM/Quality.azsli Assets/Shaders/SilPOM/EvaluationHooks.azsli VERBATIM)
                list(APPEND refinement_height_profile_shaders ${refinement_height_shader})
                add_test(NAME SilPOM.RefinementHeightCache.GPU.Mode${mode}.Coefficients${coefficients}.SM${profile}
                    COMMAND silpom_refinement_height_cache_gpu_tests ${refinement_height_shader})
              endforeach()
            endforeach()
            add_custom_target(silpom_refinement_height_cache_shader_${profile} DEPENDS ${refinement_height_profile_shaders})
            add_dependencies(silpom_refinement_height_cache_gpu_tests silpom_refinement_height_cache_shader_${profile})
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
        set(deferred_height_shader ${CMAKE_CURRENT_BINARY_DIR}/heightfield-deferred.dxil)
        add_custom_command(OUTPUT ${deferred_height_shader}
            COMMAND "${SILPOM_DXC}" -T cs_6_3 -E Main -Fo ${deferred_height_shader} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/HeightfieldDeferredGpu.hlsl
            DEPENDS Tests/HeightfieldDeferredGpu.hlsl Tests/Reference/HeightfieldTraversalEager.azsli
                Assets/Shaders/SilPOM/Heightfield.azsli VERBATIM)
        add_custom_target(silpom_deferred_height_shader DEPENDS ${deferred_height_shader})
        add_dependencies(silpom_gpu_tests silpom_deferred_height_shader)
        add_test(NAME SilPOM.GPU.DeferredLeaves COMMAND silpom_gpu_tests ${deferred_height_shader})
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
if(WIN32)
    target_link_libraries(${gem_name}.Static PRIVATE Gem::Atom_RHI_DX12.Interface)
endif()
ly_add_target(NAME ${gem_name} ${PAL_TRAIT_MONOLITHIC_DRIVEN_MODULE_TYPE} NAMESPACE Gem
    FILES_CMAKE silpom_shared_files.cmake BUILD_DEPENDENCIES PRIVATE Gem::${gem_name}.Static)
if(PAL_TRAIT_BUILD_HOST_TOOLS)
    ly_add_target(NAME ${gem_name}.Builder GEM_MODULE NAMESPACE Gem FILES_CMAKE silpom_builder_files.cmake
        INCLUDE_DIRECTORIES PRIVATE Include Source
        BUILD_DEPENDENCIES PRIVATE Gem::${gem_name}.Static AZ::AssetBuilderSDK AZ::AzToolsFramework AZ::SceneCore 3rdParty::OpenSSL)
    ly_add_target(NAME ${gem_name}.Editor GEM_MODULE NAMESPACE Gem FILES_CMAKE silpom_editor_files.cmake
        INCLUDE_DIRECTORIES PRIVATE Include Source
        BUILD_DEPENDENCIES PRIVATE Gem::${gem_name}.Static AZ::AzToolsFramework Gem::AtomLyIntegration_CommonFeatures.Public Gem::AtomToolsFramework.Static)
    ly_create_alias(NAME ${gem_name}.Tools NAMESPACE Gem TARGETS Gem::${gem_name}.Editor)
    ly_create_alias(NAME ${gem_name}.Builders NAMESPACE Gem TARGETS Gem::${gem_name}.Editor Gem::${gem_name}.Builder)
endif()
ly_create_alias(NAME ${gem_name}.Clients NAMESPACE Gem TARGETS Gem::${gem_name})
ly_create_alias(NAME ${gem_name}.Servers NAMESPACE Gem TARGETS Gem::${gem_name})
ly_create_alias(NAME ${gem_name}.Unified NAMESPACE Gem TARGETS Gem::${gem_name})
o3de_add_variant_dependencies_for_gem_dependencies(GEM_NAME ${gem_name} VARIANTS Builders Tools Clients Servers Unified)
