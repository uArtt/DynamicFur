cmake_minimum_required(VERSION 3.24)

project(
    DynamicFurV2
    VERSION 0.4.7
    DESCRIPTION "Modular runtime fur shells for Skyrim"
    LANGUAGES CXX
)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "" FORCE)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DYNAMICFUR_BUILD_PLUGIN "Build the SKSE plugin and runtime modules" ON)
option(DYNAMICFUR_BUILD_TESTS "Build renderer-independent unit tests" ON)
option(DYNAMICFUR_DEPLOY "Enable the explicit DynamicFurDeploy target" OFF)
set(DYNAMICFUR_DEPLOY_ROOT "" CACHE PATH "Explicit mod root used only by DynamicFurDeploy")

# The Portuguese MSVC toolchain can emit /showIncludes text in an encoding
# Ninja does not recognize, yielding empty compiler dependency records. Keep
# every project translation unit explicitly dependent on every local header so
# an incremental build can never mix incompatible module layouts.
file(GLOB_RECURSE DYNAMICFUR_LOCAL_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/DynamicFur/*.h"
)
list(APPEND DYNAMICFUR_LOCAL_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include/PCH.h")

function(dynamicfur_track_local_headers)
    set_property(SOURCE ${ARGN} APPEND PROPERTY
        OBJECT_DEPENDS "${DYNAMICFUR_LOCAL_HEADERS}"
    )
endfunction()

function(dynamicfur_target_defaults target visibility)
    target_compile_features(${target} ${visibility} cxx_std_23)
    target_compile_definitions(${target} ${visibility} NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(${target} ${visibility}
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /Zc:preprocessor /utf-8>
    )
endfunction()

file(GLOB_RECURSE DYNAMICFUR_CORE_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/DynamicFur/Core/*.h"
)
file(GLOB_RECURSE DYNAMICFUR_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/*.cpp"
)
dynamicfur_track_local_headers(${DYNAMICFUR_CORE_SOURCES})

add_library(DynamicFurCore STATIC
    ${DYNAMICFUR_CORE_HEADERS}
    ${DYNAMICFUR_CORE_SOURCES}
)
add_library(DynamicFur::Core ALIAS DynamicFurCore)
dynamicfur_target_defaults(DynamicFurCore PUBLIC)
target_include_directories(DynamicFurCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

file(GLOB_RECURSE DYNAMICFUR_GENERATOR_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/DynamicFur/Generator/*.h"
)
file(GLOB_RECURSE DYNAMICFUR_GENERATOR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Generator/*.cpp"
)
dynamicfur_track_local_headers(${DYNAMICFUR_GENERATOR_SOURCES})
add_library(DynamicFurGenerator STATIC
    ${DYNAMICFUR_GENERATOR_HEADERS}
    ${DYNAMICFUR_GENERATOR_SOURCES}
)
add_library(DynamicFur::Generator ALIAS DynamicFurGenerator)
dynamicfur_target_defaults(DynamicFurGenerator PUBLIC)
target_include_directories(DynamicFurGenerator PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(DynamicFurGenerator PUBLIC DynamicFur::Core)

if(DYNAMICFUR_BUILD_TESTS)
    enable_testing()

    add_executable(DynamicFurCoreTests "tests/CoreTests.cpp")
    dynamicfur_track_local_headers("tests/CoreTests.cpp")
    dynamicfur_target_defaults(DynamicFurCoreTests PRIVATE)
    target_link_libraries(DynamicFurCoreTests PRIVATE DynamicFur::Core)
    add_test(NAME DynamicFur.Core COMMAND DynamicFurCoreTests)

	add_executable(DynamicFurGeneratorTests "tests/GeneratorTests.cpp")
	dynamicfur_track_local_headers("tests/GeneratorTests.cpp")
	dynamicfur_target_defaults(DynamicFurGeneratorTests PRIVATE)
	target_link_libraries(DynamicFurGeneratorTests PRIVATE DynamicFur::Generator)
	add_test(NAME DynamicFur.Generator COMMAND DynamicFurGeneratorTests)
endif()

if(DYNAMICFUR_BUILD_PLUGIN)
    set(BUILD_TESTS OFF CACHE BOOL "Build CommonLibSSE-NG tests" FORCE)
    set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
    set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
    set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)

    add_subdirectory("lib/commonlibsse-ng" commonlibsse)
    include("${CMAKE_CURRENT_SOURCE_DIR}/lib/commonlibsse-ng/cmake/CommonLibSSE.cmake")

    get_target_property(COMMONLIBSSE_INCLUDES CommonLibSSE INTERFACE_INCLUDE_DIRECTORIES)
    if(COMMONLIBSSE_INCLUDES)
        set_target_properties(CommonLibSSE PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${COMMONLIBSSE_INCLUDES}"
        )
    endif()

    file(GLOB_RECURSE DYNAMICFUR_GAME_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/DynamicFur/Game/*.h"
    )
    file(GLOB_RECURSE DYNAMICFUR_GAME_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Game/*.cpp"
    )
    dynamicfur_track_local_headers(${DYNAMICFUR_GAME_SOURCES})
    add_library(DynamicFurGame STATIC
        ${DYNAMICFUR_GAME_HEADERS}
        ${DYNAMICFUR_GAME_SOURCES}
    )
    add_library(DynamicFur::Game ALIAS DynamicFurGame)
    dynamicfur_target_defaults(DynamicFurGame PRIVATE)
    target_precompile_headers(DynamicFurGame PRIVATE "include/PCH.h")
    target_include_directories(DynamicFurGame PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(DynamicFurGame PUBLIC DynamicFur::Core CommonLibSSE)

    file(GLOB_RECURSE DYNAMICFUR_RENDER_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/DynamicFur/Render/*.h"
    )
    file(GLOB_RECURSE DYNAMICFUR_RENDER_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/Render/*.cpp"
    )
    dynamicfur_track_local_headers(${DYNAMICFUR_RENDER_SOURCES})
    add_library(DynamicFurRender STATIC
        ${DYNAMICFUR_RENDER_HEADERS}
        ${DYNAMICFUR_RENDER_SOURCES}
    )
    add_library(DynamicFur::Render ALIAS DynamicFurRender)
    dynamicfur_target_defaults(DynamicFurRender PRIVATE)
    target_precompile_headers(DynamicFurRender PRIVATE "include/PCH.h")
    target_include_directories(DynamicFurRender PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(DynamicFurRender PUBLIC
        DynamicFur::Core
        DynamicFur::Generator
        CommonLibSSE
        d3d11
        dxgi
    )

    if(DYNAMICFUR_BUILD_TESTS)
        add_executable(DynamicFurRenderTests "tests/RenderTests.cpp")
        dynamicfur_track_local_headers("tests/RenderTests.cpp")
        dynamicfur_target_defaults(DynamicFurRenderTests PRIVATE)
        target_link_libraries(DynamicFurRenderTests PRIVATE DynamicFur::Render)
        add_test(NAME DynamicFur.Render COMMAND DynamicFurRenderTests)
    endif()

    set(DYNAMICFUR_PLUGIN_HEADERS
        "include/PCH.h"
        "include/DynamicFur/Controller.h"
        "include/DynamicFur/Log.h"
    )
    set(DYNAMICFUR_PLUGIN_SOURCES
        "src/Controller.cpp"
        "src/Log.cpp"
        "src/Main.cpp"
    )
    dynamicfur_track_local_headers(${DYNAMICFUR_PLUGIN_SOURCES})

    add_commonlibsse_plugin(DynamicFurV2
        NAME "Dynamic Fur V2"
        AUTHOR "Art"
        VERSION ${PROJECT_VERSION}
        SOURCES ${DYNAMICFUR_PLUGIN_HEADERS} ${DYNAMICFUR_PLUGIN_SOURCES}
    )
    dynamicfur_target_defaults(DynamicFurV2 PRIVATE)
    target_compile_definitions(DynamicFurV2 PRIVATE
        DYNAMICFUR_PROJECT_NAME="DynamicFurV2"
        DYNAMICFUR_VERSION_STRING="${PROJECT_VERSION}"
    )
    target_precompile_headers(DynamicFurV2 PRIVATE "include/PCH.h")
    target_include_directories(DynamicFurV2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(DynamicFurV2 PRIVATE
        DynamicFur::Core
        DynamicFur::Game
        DynamicFur::Render
        CommonLibSSE
        d3d11
        dxgi
    )

    if(DYNAMICFUR_DEPLOY)
        if(NOT DYNAMICFUR_DEPLOY_ROOT)
            message(FATAL_ERROR "DYNAMICFUR_DEPLOY_ROOT is required when DYNAMICFUR_DEPLOY=ON")
        endif()

        set(DYNAMICFUR_PLUGIN_DIR "${DYNAMICFUR_DEPLOY_ROOT}/SKSE/Plugins")
        add_custom_target(DynamicFurDeploy
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${DYNAMICFUR_PLUGIN_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:DynamicFurV2>"
                "${DYNAMICFUR_PLUGIN_DIR}/$<TARGET_FILE_NAME:DynamicFurV2>"
            DEPENDS DynamicFurV2
            COMMENT "Deploying DynamicFurV2 to the explicitly configured mod root"
            VERBATIM
        )
    endif()
endif()
