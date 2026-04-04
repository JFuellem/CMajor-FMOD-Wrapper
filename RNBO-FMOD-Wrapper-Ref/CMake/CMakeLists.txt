#CMAKE_HOST_SYSTEM_NAME

cmake_minimum_required(VERSION 3.10.0)

#--------------------------------------------------------------------------------
# Basic Compiler and Project Setup
#--------------------------------------------------------------------------------

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS -O3)

get_filename_component(PARENT_DIR ${CMAKE_CURRENT_SOURCE_DIR} DIRECTORY)

# Determine plugin name from description.json
file(READ ${PARENT_DIR}/SrcExportDir/description.json JSON_DESCRIPTION)
string(STRIP "${JSON_DESCRIPTION}" JSON_DESCRIPTION)
string(REGEX MATCH "\"rnboobjname\" *: *\"([^\"]*)\"" RNBO_OBJ_MATCH "${JSON_DESCRIPTION}")
if(CMAKE_MATCH_1)
    if("${CMAKE_MATCH_1}" STREQUAL "rnbomatic")
        set(PLUGIN_NAME "PLS_SET_CODEGEN_CLASS_IN_RNBO")
        message(WARNING "Default rnboobjname 'rnbomatic' detected. Please set a custom class name when exporting from RNBO.")
    else()
        set(PLUGIN_NAME "${CMAKE_MATCH_1}")
    endif()
endif()

# meta.rnboversion: used to pick patcher factory API. RNBO 1.4.0+ exports use
# PatcherFactoryFunctionPtr NameFactoryFunction() / NameFactoryFunction()();
# older exports used NameFactoryFunction(PlatformInterface*) / ...Platform::get()().
# See https://cycling74.com/releases/rnbo/1.4.0 (C++ export / lifecycle fixes in 1.4.x).
string(REGEX MATCH "\"rnboversion\" *: *\"([^\"]*)\"" _RNBO_VER_MATCH "${JSON_DESCRIPTION}")
set(RNBO_EXPORT_VERSION "${CMAKE_MATCH_1}")
if(NOT RNBO_EXPORT_VERSION)
    set(RNBO_EXPORT_VERSION "1.4.0")
    message(STATUS "description.json: rnboversion missing; assuming ${RNBO_EXPORT_VERSION} (modern patcher factory)")
endif()
message(STATUS "RNBO export meta.rnboversion: ${RNBO_EXPORT_VERSION}")

set(_RNBO_LEGACY_FACTORY_DEFAULT OFF)
if(RNBO_EXPORT_VERSION VERSION_LESS "1.4.0")
    set(_RNBO_LEGACY_FACTORY_DEFAULT ON)
endif()
option(RNBO_LEGACY_PATCHER_FACTORY "Pre-1.4 RNBO export: PatcherFactory(PlatformInterface*) + Platform::get (requires matching old RNBO headers in SrcExportDir/rnbo)" ${_RNBO_LEGACY_FACTORY_DEFAULT})
if(RNBO_LEGACY_PATCHER_FACTORY)
    message(STATUS "Patcher factory mode: legacy (export < 1.4.0)")
else()
    message(STATUS "Patcher factory mode: modern (export >= 1.4.0)")
endif()

if(RNBO_LEGACY_PATCHER_FACTORY)
    set(RNBO_FMOD_FACTORY_EXTERN "extern \"C\" RNBO::PatcherFactoryFunctionPtr ${PLUGIN_NAME}FactoryFunction(RNBO::PlatformInterface* platformInterface);")
    set(RNBO_FMOD_FACTORY_GETDSP_ALLOC "    RNBO::UniquePtr<RNBO::PatcherInterface> patcherInterfacePtr(${PLUGIN_NAME}FactoryFunction(RNBO::Platform::get())());")
else()
    set(RNBO_FMOD_FACTORY_EXTERN "extern \"C\" RNBO::PatcherFactoryFunctionPtr ${PLUGIN_NAME}FactoryFunction();")
    set(RNBO_FMOD_FACTORY_GETDSP_ALLOC "    RNBO::UniquePtr<RNBO::PatcherInterface> patcherInterfacePtr(${PLUGIN_NAME}FactoryFunction()());")
endif()

set(_RNBO_WRAPPER_DEFS RNBO_NO_PATCHERFACTORY)
if(RNBO_LEGACY_PATCHER_FACTORY)
    list(APPEND _RNBO_WRAPPER_DEFS RNBO_LEGACY_PATCHER_FACTORY=1)
endif()

project(${PLUGIN_NAME})
message(STATUS "Building Project: ${PROJECT_NAME} for System: ${CMAKE_SYSTEM_NAME}")

#--------------------------------------------------------------------------------
# Source File Collections
#--------------------------------------------------------------------------------

# Generated file from the FMOD wrapper template (placeholders set above)
configure_file(../RNBOWrapper/RNBO_FMOD.cpp.in ${CMAKE_BINARY_DIR}/RNBOWrapper/generated/RNBO_FMOD.cpp)
file(GLOB GENERATED_FILES "${CMAKE_BINARY_DIR}/RNBOWrapper/generated/*")

# All other source files
file(GLOB RNBOSRC_FILES "${PARENT_DIR}/SrcExportDir/*.cpp")

# --- Memory Leak Fix Integration ---
option(APPLY_LEAKFIX "Apply memory leak fixes to generated source code (requires Python)" ON)
if(APPLY_LEAKFIX)
    include(${CMAKE_CURRENT_SOURCE_DIR}/LeakFixer.cmake)
    rnbo_apply_leak_fix_to_list("${RNBOSRC_FILES}")
endif()
# -----------------------------------

file(GLOB WRAPPER_HPP_FILES "../RNBOWrapper/RNBOWrapper.hpp") # For Xcode project

# Create a library of object files containing the plugin-specific sources.
# This doesn't produce a final library file, just a collection of .o files
# that we can use to build our final static or shared libraries later.
add_library(oLib OBJECT
    ${RNBOSRC_FILES}
    ${GENERATED_FILES}
)

# For Xcode, add header files to the project for easy editing.
if(CMAKE_GENERATOR STREQUAL "Xcode")
    target_sources(oLib PRIVATE ${WRAPPER_HPP_FILES})
endif()

#--------------------------------------------------------------------------------
# Platform-Specific Library Configuration
#--------------------------------------------------------------------------------

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    # --- EMSCRIPTEN BUILD ---
    # For Emscripten, we build all common code into a single static library
    # called "rnbo_base" to avoid duplicate symbols when linking multiple plugins.
    message(STATUS "Configuring for Emscripten build")

    add_library(rnbo_base STATIC
        "${PARENT_DIR}/SrcExportDir/rnbo/RNBO.cpp"
        "${PARENT_DIR}/RNBOWrapper/RNBOWrapper.cpp"
    )
    set_target_properties(rnbo_base PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "../BuildProducts")
    target_compile_definitions(rnbo_base PRIVATE ${_RNBO_WRAPPER_DEFS})

    # The base library needs all the include paths
    target_include_directories(rnbo_base PRIVATE
        ./inc
        ${PARENT_DIR}/SrcExportDir
        ${PARENT_DIR}/SrcExportDir/rnbo
        ${PARENT_DIR}/SrcExportDir/rnbo/common
        ${PARENT_DIR}/RNBOWrapper
        ${PARENT_DIR}/decoder
    )

    # Create the final plugin-specific static library from our object files.
    add_library(${PROJECT_NAME} STATIC $<TARGET_OBJECTS:oLib>)
    set_target_properties(${PROJECT_NAME} PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "../BuildProducts")

else()
    # --- ALL OTHER BUILDS (macOS, Windows, Linux, etc.) ---
    # For other platforms, we compile all sources directly into the object library.
    message(STATUS "Configuring for non-Emscripten build")
    target_sources(oLib PRIVATE
        "${PARENT_DIR}/SrcExportDir/rnbo/RNBO.cpp"
        "${PARENT_DIR}/RNBOWrapper/RNBOWrapper.cpp"
    )

    # Create the final shared library for the plugin.
    add_library(${PROJECT_NAME} SHARED $<TARGET_OBJECTS:oLib>)

    if(APPLE)
        add_library(${PROJECT_NAME}_bundle MODULE $<TARGET_OBJECTS:oLib>)
        set_target_properties(${PROJECT_NAME}_bundle PROPERTIES
            BUNDLE TRUE
            OUTPUT_NAME "${PROJECT_NAME}"
            LIBRARY_OUTPUT_DIRECTORY "../BuildProducts"
        )
    endif()

    if(APPLE OR UNIX)
        set_target_properties(${PROJECT_NAME} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ../BuildProducts)
    else()
        set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ../BuildProducts)
    endif()
endif()

#--------------------------------------------------------------------------------
# Common Include Directories for oLib
# (Needed by all platforms to compile the plugin-specific sources)
#--------------------------------------------------------------------------------
target_include_directories(oLib PRIVATE
    ./inc
    ../SrcExportDir/rnbo
    ../SrcExportDir/rnbo/common
    ../SrcExportDir
    ../RNBOWrapper
    ../decoder
    ${CMAKE_BINARY_DIR}/RNBOWrapper/generated
)

target_compile_definitions(oLib PRIVATE ${_RNBO_WRAPPER_DEFS})
set_target_properties(oLib PROPERTIES POSITION_INDEPENDENT_CODE ON)




