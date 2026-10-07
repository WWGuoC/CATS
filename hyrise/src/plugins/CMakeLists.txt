function(add_plugin)
    cmake_parse_arguments(
        PARSED_ARGS
        ""
        "NAME"
        "SRCS;DEPS"
        ${ARGN}
    )
    if(NOT PARSED_ARGS_NAME)
        message(FATAL_ERROR "You must provide a name for the plugin")
    endif(NOT PARSED_ARGS_NAME)

    add_library(${PARSED_ARGS_NAME} SHARED ${PARSED_ARGS_SRCS})

    # We tried to link both libraries directly to hyrise, which resulted in segmentation faults when the
    # `ENABLE_COVERAGE` compiler flag is set.
    target_link_libraries_system(
        ${PARSED_ARGS_NAME}

        tbb
        ${BOOST_LIBS}
    )

    foreach(dep ${PARSED_ARGS_DEPS})
        target_link_libraries(${PARSED_ARGS_NAME} PRIVATE ${dep})
    endforeach(dep)

    # Link libraries that are required by all plugins and that require system linking.
    target_link_libraries_system(${PARSED_ARGS_NAME} nlohmann_json::nlohmann_json sparse_map)

    # Prevent the linker under macOS from complaining about undefined methods.
    if (APPLE)
        target_link_libraries(${PARSED_ARGS_NAME} PRIVATE "-undefined dynamic_lookup")
    endif()
endfunction(add_plugin)

add_plugin(NAME hyriseMvccDeletePlugin SRCS mvcc_delete_plugin.cpp mvcc_delete_plugin.hpp DEPS gtest magic_enum)
add_plugin(NAME hyriseSecondTestPlugin SRCS second_test_plugin.cpp second_test_plugin.hpp)
add_plugin(NAME hyriseTestNonInstantiablePlugin SRCS non_instantiable_plugin.cpp)
add_plugin(NAME hyriseTestPlugin SRCS test_plugin.cpp test_plugin.hpp DEPS magic_enum sqlparser)
add_plugin(NAME hyriseUccDiscoveryPlugin SRCS ucc_discovery_plugin.cpp ucc_discovery_plugin.hpp DEPS compact_vector magic_enum sqlparser)
add_plugin(NAME hyrisePlacementPlugin SRCS placement_plugin.cpp placement_plugin.hpp
        DEPS hyrise sqlparser nlohmann_json::nlohmann_json magic_enum benchmark::benchmark)

# [EDBT v4 Phase W] Weisgut-style static whole-column placement baseline.
add_plugin(NAME hyriseWeisgutStaticColumnPlugin
        SRCS weisgut_static_column_plugin.cpp weisgut_static_column_plugin.hpp
        DEPS hyrise sqlparser)

add_plugin(NAME hyriseSemanticPlacementPlugin
        SRCS semantic_placement_plugin.cpp semantic_placement_plugin.hpp
               plan_analyzer.cpp plan_analyzer.hpp
               policy_mapper.cpp policy_mapper.hpp
        DEPS hyrise sqlparser)

# [IMPL-3] (plan v3 §4, human-approved 2026-08-31, D28): E9A weight-perturbation
# arms. Configure with e.g. cmake -DHYRISE_PLACEMENT_W_ATYPE=0.3 and rebuild the
# plugin target to produce a variant .so; without any define the frozen v2 default
# weights (0.50/0.25/0.15/0.10) apply. The effective vector is printed at plugin
# startup ("- [IMPL-3] P(s) weights: ...") for per-run provenance.
foreach(_w ATYPE FREQ VOLUME IMMUTABLE)
    if(DEFINED HYRISE_PLACEMENT_W_${_w})
        target_compile_definitions(hyriseSemanticPlacementPlugin PRIVATE
                HYRISE_PLACEMENT_W_${_w}=${HYRISE_PLACEMENT_W_${_w}})
        message(STATUS "hyriseSemanticPlacementPlugin: HYRISE_PLACEMENT_W_${_w}=${HYRISE_PLACEMENT_W_${_w}}")
    endif()
endforeach()

# We define TEST_PLUGIN_DIR to always load plugins from the correct directory for testing purposes.
add_definitions(-DTEST_PLUGIN_DIR="${CMAKE_BINARY_DIR}/lib/")
