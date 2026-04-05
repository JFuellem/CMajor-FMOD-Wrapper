# FetchCmaj.cmake
# Locates the cmaj CLI. Downloads portable builds where upstream provides them.
#
# Windows: GitHub only publishes cmajor_win_x64.exe — that file is an *installer*, not cmaj.exe.
# Do not download it as the CLI. Install Cmajor from that exe, add cmaj to PATH, or pass
# -DCMAJ_EXECUTABLE=C:/path/to/cmaj.exe

set(CMAJ_VERSION "1.0.3066")

# CMake cannot parse $ENV{ProgramFiles(x86)} — parentheses break the lexer. Resolve via cmd on Windows.
set(_CMAJ_FIND_HINTS
    "$ENV{ProgramFiles}/Cmajor"
    "$ENV{ProgramFiles}/SoundStacks/Cmajor"
    "$ENV{LOCALAPPDATA}/Programs/Cmajor"
    "$ENV{LOCALAPPDATA}/Cmajor"
)
if(WIN32)
    execute_process(
        COMMAND cmd /c "echo %ProgramFiles(x86)%"
        OUTPUT_VARIABLE _CMAJ_PROGRAMFILES_X86
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_CMAJ_PROGRAMFILES_X86 MATCHES "^[A-Za-z]:")
        list(APPEND _CMAJ_FIND_HINTS "${_CMAJ_PROGRAMFILES_X86}/Cmajor")
    endif()
endif()

find_program(CMAJ_EXECUTABLE
    NAMES cmaj cmaj.exe
    HINTS ${_CMAJ_FIND_HINTS}
    PATH_SUFFIXES bin ""
    DOC "Cmajor CLI (cmaj)"
)

if(NOT CMAJ_EXECUTABLE)
    set(CMAJ_CACHE_DIR "${PARENT_DIR}/.cache")
    set(CMAJ_DOWNLOAD_DIR "${CMAJ_CACHE_DIR}/cmajor_cli")

    if(APPLE)
        set(LOCAL_CMAJ "${CMAJ_DOWNLOAD_DIR}/cmaj")
    elseif(WIN32)
        set(LOCAL_CMAJ "")
    else()
        set(LOCAL_CMAJ "${CMAJ_DOWNLOAD_DIR}/linux/x64/cmaj")
    endif()

    if(NOT WIN32 AND EXISTS "${LOCAL_CMAJ}")
        set(CMAJ_EXECUTABLE "${LOCAL_CMAJ}")
    elseif(NOT WIN32)
        message(STATUS "cmaj CLI not found in PATH or cache. Downloading...")
        file(MAKE_DIRECTORY "${CMAJ_DOWNLOAD_DIR}")

        if(APPLE)
            set(CMAJ_URL "https://github.com/cmajor-lang/cmajor/releases/download/${CMAJ_VERSION}/cmajor.dmg")
            set(CMAJ_DMG "${CMAJ_CACHE_DIR}/cmajor.dmg")
            if(NOT EXISTS "${CMAJ_DMG}")
                file(DOWNLOAD ${CMAJ_URL} "${CMAJ_DMG}" SHOW_PROGRESS)
            endif()

            execute_process(
                COMMAND hdiutil attach "${CMAJ_DMG}" -nobrowse -mountpoint "${CMAJ_CACHE_DIR}/cmaj_mount"
                RESULT_VARIABLE HDIUTIL_RES
            )
            if(HDIUTIL_RES EQUAL 0)
                file(COPY "${CMAJ_CACHE_DIR}/cmaj_mount/cmaj" DESTINATION "${CMAJ_DOWNLOAD_DIR}")
                execute_process(COMMAND hdiutil detach "${CMAJ_CACHE_DIR}/cmaj_mount" -force)
                set(CMAJ_EXECUTABLE "${LOCAL_CMAJ}")
                execute_process(COMMAND chmod +x "${CMAJ_EXECUTABLE}")
            else()
                message(WARNING "Failed to mount cmajor.dmg")
            endif()

        else() # Linux
            set(CMAJ_URL "https://github.com/cmajor-lang/cmajor/releases/download/${CMAJ_VERSION}/cmajor.linux.x64.zip")
            set(CMAJ_ZIP "${CMAJ_CACHE_DIR}/cmajor.zip")
            if(NOT EXISTS "${CMAJ_ZIP}")
                file(DOWNLOAD ${CMAJ_URL} "${CMAJ_ZIP}" SHOW_PROGRESS)
            endif()
            file(ARCHIVE_EXTRACT INPUT "${CMAJ_ZIP}" DESTINATION "${CMAJ_DOWNLOAD_DIR}")
            set(CMAJ_EXECUTABLE "${LOCAL_CMAJ}")
            execute_process(COMMAND chmod +x "${CMAJ_EXECUTABLE}")
        endif()
    endif()
endif()

if(NOT CMAJ_EXECUTABLE OR NOT EXISTS "${CMAJ_EXECUTABLE}")
    if(WIN32)
        message(FATAL_ERROR
            "cmaj.exe not found (searched PATH and common install folders under Program Files / AppData).\n"
            "Windows releases ship an installer only — run it once, then either add the install location "
            "to PATH or pass the full path to CMake, e.g.:\n"
            "  cmake ... -DCMAJ_EXECUTABLE=\"C:/Program Files/Cmajor/bin/cmaj.exe\"\n"
            "Installer download:\n"
            "  https://github.com/cmajor-lang/cmajor/releases/download/${CMAJ_VERSION}/cmajor_win_x64.exe\n"
            "If CMake was previously configured with a wrong cmaj path, delete your build folder or CMakeCache.txt."
        )
    else()
        message(FATAL_ERROR "Failed to locate or download cmaj CLI.")
    endif()
endif()

message(STATUS "Using cmaj CLI: ${CMAJ_EXECUTABLE}")
