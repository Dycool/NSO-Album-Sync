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
    NSO_STATIC_RUNTIME
    "Prefer static language/TLS runtimes for compact standalone releases"
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
elseif(APPLE)
    add_compile_options(-Wall -Wextra -Wpedantic)
    add_compile_options(
        $<$<CONFIG:Release>:-ffunction-sections>
        $<$<CONFIG:Release>:-fdata-sections>
    )
    add_link_options($<$<CONFIG:Release>:-Wl,-dead_strip>)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    add_compile_options(
        $<$<CONFIG:Release>:-ffunction-sections>
        $<$<CONFIG:Release>:-fdata-sections>
    )
    add_link_options($<$<CONFIG:Release>:-Wl,--gc-sections>)
endif()

if(NSO_STATIC_RUNTIME)
    set(OPENSSL_USE_STATIC_LIBS TRUE)

    if(MSVC)
        set(
            CMAKE_MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()
endif()

if(NOT WIN32)
    find_package(OpenSSL REQUIRED)
endif()
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
    src/discord_ipc.cpp
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
    include/nso_album_sync/discord_ipc.hpp
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
        src/http_windows.cpp
        src/platform_windows.cpp
        src/secure_store_windows.cpp
        src/discord_ipc_windows.cpp
    )
elseif(APPLE)
    enable_language(OBJCXX)
    list(APPEND NSO_SOURCES
        src/http_openssl.cpp
        src/platform_macos.mm
        src/secure_store_macos.mm
        src/discord_ipc_unix.cpp
    )
else()
    list(APPEND NSO_SOURCES
        src/http_openssl.cpp
        src/platform_linux.cpp
        src/secure_store_linux.cpp
        src/discord_ipc_unix.cpp
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

# Let CMake enable link-time optimization only when the active compiler/linker
# toolchain can actually support it.  Forcing -flto breaks Clang installations
# that do not ship an LLVMgold linker plugin.
include(CheckIPOSupported)
check_ipo_supported(RESULT NSO_IPO_SUPPORTED OUTPUT NSO_IPO_ERROR LANGUAGES CXX)
if(NSO_IPO_SUPPORTED)
    set_property(
        TARGET nso-album-sync
        PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    )
else()
    message(STATUS "IPO/LTO disabled: ${NSO_IPO_ERROR}")
endif()

if(NOT WIN32)
    target_link_libraries(
        nso-album-sync
        PRIVATE
            OpenSSL::SSL
            OpenSSL::Crypto
    )
endif()

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
            bcrypt
            crypt32
            winhttp
            shell32
            ole32
            comdlg32
    )
elseif(APPLE)
    # platform_macos.mm uses AppKit/Cocoa for the menu-bar UI, Security for
    # Keychain storage, and UserNotifications for native notifications.  Resolve
    # the frameworks explicitly so the final Objective-C++ link step contains
    # every framework whose classes are referenced.
    find_library(NSO_COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(NSO_SECURITY_FRAMEWORK Security REQUIRED)
    find_library(NSO_USER_NOTIFICATIONS_FRAMEWORK UserNotifications REQUIRED)

    target_link_libraries(
        nso-album-sync
        PRIVATE
            ${NSO_COCOA_FRAMEWORK}
            ${NSO_SECURITY_FRAMEWORK}
            ${NSO_USER_NOTIFICATIONS_FRAMEWORK}
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
# Discord Rich Presence
# -----------------------------------------------------------------------------

# Discord Rich Presence uses the desktop client's documented local RPC/IPC
# transport directly. No Discord SDK, extra library, secret, or download is
# required at build or runtime.

install(
    TARGETS nso-album-sync
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION .
)
