cmake_minimum_required(VERSION 3.20)

project(
    nso-album-sync
    VERSION 2.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(
    NSO_ENABLE_DISCORD
    "Build Discord Social SDK Rich Presence"
    ON
)
option(
    NSO_STATIC_RUNTIME
    "Statically link C++ runtime and OpenSSL on Linux compact releases"
    OFF
)
option(
    NSO_ENABLE_LINUX_TRAY
    "Use GTK/AppIndicator tray on Linux when available"
    ON
)

# -----------------------------------------------------------------------------
# Compiler and linker defaults
# -----------------------------------------------------------------------------

if(MSVC)
    add_compile_options(/W4 /EHsc /utf-8)
    add_link_options(
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    add_compile_options(
        $<$<CONFIG:Release>:-ffunction-sections>
        $<$<CONFIG:Release>:-fdata-sections>
        $<$<CONFIG:Release>:-flto>
    )
    add_link_options(
        $<$<CONFIG:Release>:-Wl,--gc-sections>
        $<$<CONFIG:Release>:-flto>
    )
endif()

if(NSO_STATIC_RUNTIME AND UNIX AND NOT APPLE)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()

find_package(OpenSSL REQUIRED)
find_package(PkgConfig QUIET)

# -----------------------------------------------------------------------------
# Sources
# -----------------------------------------------------------------------------

set(NSO_SOURCES
    src/main.cpp
    src/app.cpp
    src/config.cpp
    src/coral.cpp
    src/discord.cpp
    src/http.cpp
    src/json.cpp
    src/nintendo_auth.cpp
    src/nxapi.cpp
    src/sync.cpp
    src/util.cpp
)

set(NSO_HEADERS
    include/nso_album_sync/app.hpp
    include/nso_album_sync/config.hpp
    include/nso_album_sync/coral.hpp
    include/nso_album_sync/discord.hpp
    include/nso_album_sync/game_aliases.hpp
    include/nso_album_sync/http.hpp
    include/nso_album_sync/json.hpp
    include/nso_album_sync/nintendo_auth.hpp
    include/nso_album_sync/nxapi.hpp
    include/nso_album_sync/platform.hpp
    include/nso_album_sync/secure_store.hpp
    include/nso_album_sync/sync.hpp
    include/nso_album_sync/util.hpp
)

if(WIN32)
    list(APPEND NSO_SOURCES
        src/platform_windows.cpp
        src/secure_store_windows.cpp
    )
elseif(APPLE)
    enable_language(OBJCXX)
    list(APPEND NSO_SOURCES
        src/platform_macos.mm
        src/secure_store_macos.mm
    )
else()
    list(APPEND NSO_SOURCES
        src/platform_linux.cpp
        src/secure_store_linux.cpp
    )
endif()

# -----------------------------------------------------------------------------
# Main target
# -----------------------------------------------------------------------------

if(WIN32)
    add_executable(nso-album-sync WIN32 ${NSO_SOURCES} ${NSO_HEADERS})
elseif(APPLE)
    add_executable(nso-album-sync MACOSX_BUNDLE ${NSO_SOURCES} ${NSO_HEADERS})
    set_target_properties(
        nso-album-sync
        PROPERTIES
            MACOSX_BUNDLE_BUNDLE_NAME "NSO Album Sync"
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.dycool.nso-album-sync"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/Info.plist.in"
    )
else()
    add_executable(nso-album-sync ${NSO_SOURCES} ${NSO_HEADERS})
endif()

target_include_directories(
    nso-album-sync
    PRIVATE "${CMAKE_SOURCE_DIR}/include"
)

target_link_libraries(
    nso-album-sync
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
)

# Compact Linux builds can carry the C++ runtime and OpenSSL statically while
# still using the host libc. This keeps the distributed binary small and simple.
if(NSO_STATIC_RUNTIME AND UNIX AND NOT APPLE)
    target_link_options(nso-album-sync PRIVATE -static-libgcc -static-libstdc++)

    find_library(
        NSO_ZSTD_STATIC
        NAMES libzstd.a zstd.a
        PATHS
            /usr/lib
            /usr/lib/x86_64-linux-gnu
            /usr/local/lib
    )

    if(NSO_ZSTD_STATIC)
        target_link_libraries(nso-album-sync PRIVATE ${NSO_ZSTD_STATIC})
    endif()
endif()

# -----------------------------------------------------------------------------
# Platform integration
# -----------------------------------------------------------------------------

if(WIN32)
    target_link_libraries(
        nso-album-sync
        PRIVATE
            advapi32
            crypt32
            shell32
            ole32
            comdlg32
    )
elseif(APPLE)
    target_link_libraries(
        nso-album-sync
        PRIVATE
            "-framework Cocoa"
            "-framework Security"
    )
else()
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSECRET QUIET libsecret-1)
        if(LIBSECRET_FOUND)
            target_compile_definitions(
                nso-album-sync
                PRIVATE NSO_HAVE_LIBSECRET=1
            )
            target_include_directories(
                nso-album-sync
                PRIVATE ${LIBSECRET_INCLUDE_DIRS}
            )
            target_link_libraries(
                nso-album-sync
                PRIVATE ${LIBSECRET_LIBRARIES}
            )
        endif()

        if(NSO_ENABLE_LINUX_TRAY)
            pkg_check_modules(GTK3 QUIET gtk+-3.0)
            pkg_check_modules(APPIND QUIET ayatana-appindicator3-0.1)

            if(GTK3_FOUND)
                target_compile_definitions(
                    nso-album-sync
                    PRIVATE NSO_HAVE_GTK=1
                )
                target_include_directories(
                    nso-album-sync
                    PRIVATE ${GTK3_INCLUDE_DIRS}
                )
                target_link_libraries(
                    nso-album-sync
                    PRIVATE ${GTK3_LIBRARIES}
                )

                if(APPIND_FOUND)
                    target_compile_definitions(
                        nso-album-sync
                        PRIVATE NSO_HAVE_APPINDICATOR=1
                    )
                    target_include_directories(
                        nso-album-sync
                        PRIVATE ${APPIND_INCLUDE_DIRS}
                    )
                    target_link_libraries(
                        nso-album-sync
                        PRIVATE ${APPIND_LIBRARIES}
                    )
                endif()
            endif()
        endif()
    endif()
endif()

# -----------------------------------------------------------------------------
# Discord Social SDK
# -----------------------------------------------------------------------------

if(NSO_ENABLE_DISCORD)
    if(DEFINED ENV{DISCORD_SOCIAL_SDK_DIR})
        set(DISCORD_SOCIAL_SDK_DIR "$ENV{DISCORD_SOCIAL_SDK_DIR}")
    endif()

    if(DISCORD_SOCIAL_SDK_DIR AND
       EXISTS "${DISCORD_SOCIAL_SDK_DIR}/include/discordpp.h")
        target_compile_definitions(
            nso-album-sync
            PRIVATE NSO_DISCORD_SOCIAL_SDK=1
        )
        target_include_directories(
            nso-album-sync
            PRIVATE "${DISCORD_SOCIAL_SDK_DIR}/include"
        )
        target_link_directories(
            nso-album-sync
            PRIVATE "${DISCORD_SOCIAL_SDK_DIR}/lib/release"
        )
        target_link_libraries(
            nso-album-sync
            PRIVATE discord_partner_sdk
        )

        if(WIN32)
            add_custom_command(
                TARGET nso-album-sync
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DISCORD_SOCIAL_SDK_DIR}/bin/release/discord_partner_sdk.dll"
                    "$<TARGET_FILE_DIR:nso-album-sync>"
            )
        elseif(APPLE)
            set_target_properties(
                nso-album-sync
                PROPERTIES BUILD_RPATH "@executable_path/../Frameworks"
            )
        else()
            set_target_properties(
                nso-album-sync
                PROPERTIES BUILD_RPATH "$ORIGIN"
            )
        endif()
    else()
        message(
            WARNING
            "Discord Social SDK not found. Set DISCORD_SOCIAL_SDK_DIR to build Rich Presence."
        )
    endif()
endif()

install(
    TARGETS nso-album-sync
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION .
)
