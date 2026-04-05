cmake_minimum_required(VERSION 3.14.0)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(PARENT_DIR ${CMAKE_CURRENT_SOURCE_DIR} DIRECTORY)

# cmaj CLI (see FetchCmaj.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/FetchCmaj.cmake)

# .cmajor source: clipboard, or a single file in SrcExportDir
set(USE_CLIPBOARD OFF CACHE BOOL "Read Cmajor code from the system clipboard")
set(SRC_EXPORT_DIR "${PARENT_DIR}/SrcExportDir")
set(CMAJOR_CODE "")

if(USE_CLIPBOARD)
    message(STATUS "Reading Cmajor code from the clipboard...")
    if(APPLE)
        execute_process(COMMAND pbpaste OUTPUT_VARIABLE CLIPBOARD_CONTENT RESULT_VARIABLE CLIP_RES)
    elseif(WIN32)
        execute_process(COMMAND powershell -command "Get-Clipboard" OUTPUT_VARIABLE CLIPBOARD_CONTENT RESULT_VARIABLE CLIP_RES)
    else()
        # Linux: xclip, else xsel
        execute_process(COMMAND xclip -selection clipboard -o OUTPUT_VARIABLE CLIPBOARD_CONTENT RESULT_VARIABLE CLIP_RES ERROR_QUIET)
        if(NOT CLIP_RES EQUAL 0)
            execute_process(COMMAND xsel --clipboard --output OUTPUT_VARIABLE CLIPBOARD_CONTENT RESULT_VARIABLE CLIP_RES ERROR_QUIET)
        endif()
    endif()

    if(NOT CLIP_RES EQUAL 0 OR NOT CLIPBOARD_CONTENT)
        message(FATAL_ERROR "Failed to read from clipboard or clipboard is empty.")
    endif()

    set(CMAJOR_CODE "${CLIPBOARD_CONTENT}")
endif()

if(CMAJOR_CODE)
    # Extract the processor name to name the file
    string(REGEX MATCH "processor[ \t]+([a-zA-Z0-9_]+)" PROCESSOR_MATCH "${CMAJOR_CODE}")
    if(CMAKE_MATCH_1)
        set(PROCESSOR_NAME "${CMAKE_MATCH_1}")
    else()
        set(PROCESSOR_NAME "GeneratedProcessor")
    endif()

    file(MAKE_DIRECTORY "${SRC_EXPORT_DIR}")
    set(CMAJOR_FILE "${SRC_EXPORT_DIR}/${PROCESSOR_NAME}.cmajor")
    file(WRITE "${CMAJOR_FILE}" "${CMAJOR_CODE}")
else()
    # Fallback to searching in SrcExportDir if no explicit code was provided
    if(EXISTS "${SRC_EXPORT_DIR}")
        file(GLOB CMAJOR_FILES "${SRC_EXPORT_DIR}/*.cmajor")
        list(LENGTH CMAJOR_FILES NUM_CMAJOR)
        if(NUM_CMAJOR EQUAL 1)
            list(GET CMAJOR_FILES 0 CMAJOR_FILE)
        elseif(NUM_CMAJOR GREATER 1)
            message(FATAL_ERROR "Multiple .cmajor files found in ${SRC_EXPORT_DIR}. Please remove all but one.")
        endif()
    endif()
endif()

if(NOT CMAJOR_FILE OR NOT EXISTS "${CMAJOR_FILE}")
    message(FATAL_ERROR "No .cmajor file found. Please use -DUSE_CLIPBOARD=ON, or place exactly one .cmajor file in SrcExportDir")
endif()

# .cmajorpatch JSON (name, instrument vs effect, etc.)
message(STATUS "Generating .cmajorpatch file for ${CMAJOR_FILE}...")
file(READ "${CMAJOR_FILE}" CMAJOR_CONTENT)

string(REGEX MATCH "processor[ \t]+([a-zA-Z0-9_]+)" PROCESSOR_MATCH "${CMAJOR_CONTENT}")
if(CMAKE_MATCH_1)
    set(PROCESSOR_NAME "${CMAKE_MATCH_1}")
else()
    message(FATAL_ERROR "Could not find a 'processor' definition in ${CMAJOR_FILE}")
endif()

# Determine if it's an instrument or effect
string(FIND "${CMAJOR_CONTENT}" "input stream" HAS_INPUT_STREAM)
if(HAS_INPUT_STREAM EQUAL -1)
    set(IS_INSTRUMENT "true")
    set(CATEGORY "generator")
else()
    set(IS_INSTRUMENT "false")
    set(CATEGORY "effect")
endif()

get_filename_component(CMAJOR_FILENAME "${CMAJOR_FILE}" NAME)
string(TOLOWER "${PROCESSOR_NAME}" PROCESSOR_LOWER)

set(PATCH_JSON "{
    \"CmajorVersion\": 1,
    \"ID\": \"com.amorph.${PROCESSOR_LOWER}\",
    \"version\": \"1.0.0\",
    \"name\": \"${PROCESSOR_NAME}\",
    \"description\": \"Auto-generated patch for ${PROCESSOR_NAME}\",
    \"category\": \"${CATEGORY}\",
    \"manufacturer\": \"Amorph\",
    \"isInstrument\": ${IS_INSTRUMENT},
    \"source\": \"${CMAJOR_FILENAME}\"
}")

string(REPLACE ".cmajor" ".cmajorpatch" CMAJOR_PATCH_FILE "${CMAJOR_FILE}")
file(WRITE "${CMAJOR_PATCH_FILE}" "${PATCH_JSON}")
message(STATUS "Successfully generated ${CMAJOR_PATCH_FILE} for processor '${PROCESSOR_NAME}'")

file(READ ${CMAJOR_PATCH_FILE} JSON_DESCRIPTION)
string(REGEX MATCH "\"name\" *: *\"([^\"]*)\"" CMAJOR_NAME_MATCH "${JSON_DESCRIPTION}")
if(CMAKE_MATCH_1)
    set(PLUGIN_NAME "${CMAKE_MATCH_1}")
else()
    set(PLUGIN_NAME "CmajorPlugin")
endif()

# PROJECT_NAME matches patch "name" field
project(${PLUGIN_NAME})
message(STATUS "Building Project: ${PROJECT_NAME} for System: ${CMAKE_SYSTEM_NAME}")
# MSVC: /Zc:__cplusplus so __cplusplus matches /std (generated code checks it). /EHsc for STL.
if(MSVC)
    add_compile_options(/Zc:__cplusplus /EHsc)
else()
    add_compile_options(-O3)
endif()

# cmaj generate → cmajor_patch.cpp
set(CMAJOR_EXPORT_DIR "${CMAKE_BINARY_DIR}/CmajorExport")
file(MAKE_DIRECTORY ${CMAJOR_EXPORT_DIR})

set(CMAJOR_CPP_FILE "${CMAJOR_EXPORT_DIR}/cmajor_patch.cpp")

# Codegen fixed arrays: must be >= largest FMOD process() length you will see (set FMOD DSP buffer size / driver limits accordingly).
set(CMAJOR_MAX_FRAMES_PER_BLOCK 8192 CACHE STRING "cmaj generate --maxFramesPerBlock; must match CMAJOR_CODEGEN_MAX_FRAMES_PER_BLOCK (runtime guard vs UB if FMOD length exceeds codegen arrays)")
execute_process(
    COMMAND ${CMAJ_EXECUTABLE} generate --target=cpp ${CMAJOR_PATCH_FILE} --output=${CMAJOR_CPP_FILE} --maxFramesPerBlock=${CMAJOR_MAX_FRAMES_PER_BLOCK}
    RESULT_VARIABLE CMAJ_RESULT
)
if(NOT CMAJ_RESULT EQUAL 0)
    message(FATAL_ERROR "cmaj generate failed")
endif()

file(READ ${CMAJOR_CPP_FILE} CMAJOR_CPP_CONTENT)
string(REGEX MATCH "struct ([a-zA-Z0-9_]+)" CMAJOR_CLASS_MATCH "${CMAJOR_CPP_CONTENT}")
if(CMAKE_MATCH_1)
    set(CMAJOR_CLASS_NAME "${CMAKE_MATCH_1}")
else()
    message(FATAL_ERROR "Could not find struct name in generated C++ file")
endif()

# FMOD wrapper + object library
configure_file(../CmajorWrapper/Cmajor_FMOD.cpp.in ${CMAKE_BINARY_DIR}/CmajorWrapper/generated/Cmajor_FMOD.cpp)
file(GLOB GENERATED_FILES "${CMAKE_BINARY_DIR}/CmajorWrapper/generated/*")

file(GLOB WRAPPER_HPP_FILES "../CmajorWrapper/CmajorWrapper.hpp")

add_library(oLib OBJECT
    ${CMAJOR_CPP_FILE}
    ${GENERATED_FILES}
)

set_target_properties(oLib PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_compile_definitions(oLib PRIVATE "CMAJOR_CODEGEN_MAX_FRAMES_PER_BLOCK=${CMAJOR_MAX_FRAMES_PER_BLOCK}")

if(CMAKE_GENERATOR STREQUAL "Xcode")
    target_sources(oLib PRIVATE ${WRAPPER_HPP_FILES})
endif()

# Outputs: static (Emscripten) or shared DLL/SO (+ optional macOS bundle)
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    add_library(cmajor_base STATIC
        ${GENERATED_FILES}
    )
    set_target_properties(cmajor_base PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${PARENT_DIR}/BuildProducts")

    target_include_directories(cmajor_base PRIVATE
        ./inc
        ${CMAJOR_EXPORT_DIR}
        ../CmajorWrapper
    )

    add_library(${PROJECT_NAME} STATIC $<TARGET_OBJECTS:oLib>)
    set_target_properties(${PROJECT_NAME} PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${PARENT_DIR}/BuildProducts")
else()
    add_library(${PROJECT_NAME} SHARED $<TARGET_OBJECTS:oLib>)

    if(APPLE)
        add_library(${PROJECT_NAME}_bundle MODULE $<TARGET_OBJECTS:oLib>)
        set_target_properties(${PROJECT_NAME}_bundle PROPERTIES
            BUNDLE TRUE
            OUTPUT_NAME "${PROJECT_NAME}"
            LIBRARY_OUTPUT_DIRECTORY "${PARENT_DIR}/BuildProducts"
        )
    endif()

    if(APPLE OR UNIX)
        set_target_properties(${PROJECT_NAME} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${PARENT_DIR}/BuildProducts)
    else()
        set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PARENT_DIR}/BuildProducts)
    endif()
endif()

# oLib include dirs
target_include_directories(oLib PRIVATE
    ./inc
    ${CMAJOR_EXPORT_DIR}
    ../CmajorWrapper
    ${CMAKE_BINARY_DIR}/CmajorWrapper/generated
)

set_target_properties(oLib PROPERTIES POSITION_INDEPENDENT_CODE ON)
