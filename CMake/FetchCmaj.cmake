# FetchCmaj.cmake
# Downloads and extracts the cmaj CLI tool if not found

find_program(CMAJ_EXECUTABLE cmaj)

if(NOT CMAJ_EXECUTABLE)
    set(CMAJ_VERSION "1.0.3066")
    set(CMAJ_CACHE_DIR "${PARENT_DIR}/.cache")
    set(CMAJ_DOWNLOAD_DIR "${CMAJ_CACHE_DIR}/cmajor_cli")
    
    if(APPLE)
        set(LOCAL_CMAJ "${CMAJ_DOWNLOAD_DIR}/cmaj")
    elseif(WIN32)
        set(LOCAL_CMAJ "${CMAJ_DOWNLOAD_DIR}/cmaj.exe")
    else()
        set(LOCAL_CMAJ "${CMAJ_DOWNLOAD_DIR}/linux/x64/cmaj")
    endif()

    if(EXISTS "${LOCAL_CMAJ}")
        set(CMAJ_EXECUTABLE "${LOCAL_CMAJ}")
    else()
        message(STATUS "cmaj CLI not found in PATH or cache. Downloading...")
        file(MAKE_DIRECTORY "${CMAJ_DOWNLOAD_DIR}")

        if(APPLE)
            set(CMAJ_URL "https://github.com/cmajor-lang/cmajor/releases/download/${CMAJ_VERSION}/cmajor.dmg")
            set(CMAJ_DMG "${CMAJ_CACHE_DIR}/cmajor.dmg")
            if(NOT EXISTS "${CMAJ_DMG}")
                file(DOWNLOAD ${CMAJ_URL} "${CMAJ_DMG}" SHOW_PROGRESS)
            endif()
            
            # Mount DMG and copy cmaj
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

        elseif(WIN32)
            set(CMAJ_URL "https://github.com/cmajor-lang/cmajor/releases/download/${CMAJ_VERSION}/cmajor_win_x64.exe")
            if(NOT EXISTS "${LOCAL_CMAJ}")
                file(DOWNLOAD ${CMAJ_URL} "${LOCAL_CMAJ}" SHOW_PROGRESS)
            endif()
            set(CMAJ_EXECUTABLE "${LOCAL_CMAJ}")

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
    message(FATAL_ERROR "Failed to locate or download cmaj CLI.")
endif()

message(STATUS "Using cmaj CLI: ${CMAJ_EXECUTABLE}")
