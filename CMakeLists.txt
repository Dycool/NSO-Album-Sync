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
set(
    NSO_MACOS_CODESIGN_IDENTITY
    "-"
    CACHE STRING
    "macOS code-signing identity; '-' performs a local ad-hoc signature"
)
set(
    NSO_DISCORD_SOCIAL_SDK_VERSION
    "1.10.18687"
    CACHE STRING
    "Discord Social SDK version used for Rich Presence"
)
set(
    NSO_DISCORD_SOCIAL_SDK_ROOT
    "${CMAKE_SOURCE_DIR}/third_party/discord_social_sdk"
    CACHE PATH
    "Extracted Discord Social SDK root"
)

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
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

if(NOT WIN32)
    find_package(OpenSSL REQUIRED)
endif()
find_package(PkgConfig QUIET)

# Rich Presence uses Discord's current Social SDK only. CI checks out the
# private SDK cache directly at NSO_DISCORD_SOCIAL_SDK_ROOT. Local builds can
# point this variable at an official SDK extracted from the Developer Portal.
# The desktop Rich Presence path does not use Discord OAuth, bot tokens,
# client secrets, or Client::Connect().
if(NOT EXISTS "${NSO_DISCORD_SOCIAL_SDK_ROOT}/include/discordpp.h" OR
   NOT EXISTS "${NSO_DISCORD_SOCIAL_SDK_ROOT}/include/cdiscord.h")
    message(FATAL_ERROR
        "Discord Social SDK ${NSO_DISCORD_SOCIAL_SDK_VERSION} was not found at "
        "${NSO_DISCORD_SOCIAL_SDK_ROOT}. Extract the official standalone C++ SDK "
        "there or set -DNSO_DISCORD_SOCIAL_SDK_ROOT=<path>."
    )
endif()

set(NSO_THIRD_PARTY_NOTICES "${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt")
if(NOT EXISTS "${NSO_THIRD_PARTY_NOTICES}")
    message(FATAL_ERROR "THIRD-PARTY-NOTICES.txt is required for release packaging")
endif()

set(NSO_DISCORD_SDK_INCLUDE_DIR "${NSO_DISCORD_SOCIAL_SDK_ROOT}/include")
set(NSO_DISCORD_SDK_RESOURCE_ID 201)
set(NSO_THIRD_PARTY_NOTICES_RESOURCE_ID 202)
set(NSO_DISCORD_SDK_IS_FRAMEWORK FALSE)

if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR "Discord Social SDK Windows builds currently require MSVC")
    endif()
    set(NSO_DISCORD_SDK_LINK_LIBRARY
        "${NSO_DISCORD_SOCIAL_SDK_ROOT}/lib/release/discord_partner_sdk.lib")
    set(NSO_DISCORD_SDK_RUNTIME
        "${NSO_DISCORD_SOCIAL_SDK_ROOT}/bin/release/discord_partner_sdk.dll")
elseif(APPLE)
    # Discord Social SDK 1.10+ packages macOS as a versioned framework. Prefer
    # the real versioned Mach-O binary so a cache populated through GitHub's web
    # uploader still works even if the framework symlinks became marker files.
    set(NSO_DISCORD_SDK_FRAMEWORK
        "${NSO_DISCORD_SOCIAL_SDK_ROOT}/lib/release/discord_partner_sdk.framework")
    set(NSO_DISCORD_SDK_FRAMEWORK_BINARY
        "${NSO_DISCORD_SDK_FRAMEWORK}/discord_partner_sdk")
    set(NSO_DISCORD_SDK_FRAMEWORK_VERSIONED_BINARY
        "${NSO_DISCORD_SDK_FRAMEWORK}/Versions/A/discord_partner_sdk")
    if(EXISTS "${NSO_DISCORD_SDK_FRAMEWORK_VERSIONED_BINARY}")
        set(NSO_DISCORD_SDK_IS_FRAMEWORK TRUE)
        set(NSO_DISCORD_SDK_LINK_LIBRARY
            "${NSO_DISCORD_SDK_FRAMEWORK_VERSIONED_BINARY}")
        set(NSO_DISCORD_SDK_RUNTIME "${NSO_DISCORD_SDK_FRAMEWORK}")
    elseif(EXISTS "${NSO_DISCORD_SDK_FRAMEWORK_BINARY}")
        set(NSO_DISCORD_SDK_IS_FRAMEWORK TRUE)
        set(NSO_DISCORD_SDK_LINK_LIBRARY "${NSO_DISCORD_SDK_FRAMEWORK_BINARY}")
        set(NSO_DISCORD_SDK_RUNTIME "${NSO_DISCORD_SDK_FRAMEWORK}")
    else()
        set(NSO_DISCORD_SDK_LINK_LIBRARY
            "${NSO_DISCORD_SOCIAL_SDK_ROOT}/lib/release/libdiscord_partner_sdk.dylib")
        set(NSO_DISCORD_SDK_RUNTIME "${NSO_DISCORD_SDK_LINK_LIBRARY}")
    endif()
else()
    set(NSO_DISCORD_SDK_LINK_LIBRARY
        "${NSO_DISCORD_SOCIAL_SDK_ROOT}/lib/release/libdiscord_partner_sdk.so")
    set(NSO_DISCORD_SDK_RUNTIME "${NSO_DISCORD_SDK_LINK_LIBRARY}")
endif()

if(NOT EXISTS "${NSO_DISCORD_SDK_LINK_LIBRARY}" OR
   NOT EXISTS "${NSO_DISCORD_SDK_RUNTIME}")
    message(FATAL_ERROR
        "Discord Social SDK ${NSO_DISCORD_SOCIAL_SDK_VERSION} is missing the "
        "release runtime/link library required for this platform under "
        "${NSO_DISCORD_SOCIAL_SDK_ROOT}."
    )
endif()

set(NSO_SOURCES
    src/main.cpp
    src/app.cpp
    src/auth_callback.cpp
    src/config.cpp
    src/coral.cpp
    src/discord.cpp
    src/json.cpp
    src/nintendo_auth.cpp
    src/nxapi.cpp
    src/sync.cpp
    src/util.cpp
)

set(NSO_HEADERS
    include/nso_album_sync/app.hpp
    include/nso_album_sync/auth_callback.hpp
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
    enable_language(RC)
    configure_file(
        "${CMAKE_SOURCE_DIR}/resources/windows/app.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/nso-album-sync.rc"
        @ONLY
    )

    # Keep the Windows release as one .exe. The Discord runtime and required
    # third-party notices are both embedded as RCDATA resources. The runtime is
    # extracted only when Rich Presence is first used.
    file(TO_CMAKE_PATH "${NSO_DISCORD_SDK_RUNTIME}" NSO_DISCORD_SDK_RUNTIME_RC)
    file(TO_CMAKE_PATH "${NSO_THIRD_PARTY_NOTICES}" NSO_THIRD_PARTY_NOTICES_RC)
    set(NSO_DISCORD_SDK_RC "${CMAKE_CURRENT_BINARY_DIR}/discord-social-sdk.rc")
    file(WRITE "${NSO_DISCORD_SDK_RC}"
        "${NSO_DISCORD_SDK_RESOURCE_ID} RCDATA \"${NSO_DISCORD_SDK_RUNTIME_RC}\"\n"
        "${NSO_THIRD_PARTY_NOTICES_RESOURCE_ID} RCDATA \"${NSO_THIRD_PARTY_NOTICES_RC}\"\n")

    list(APPEND NSO_SOURCES
        src/auth_callback_windows.cpp
        src/http_windows.cpp
        src/platform_windows.cpp
        src/secure_store_windows.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/nso-album-sync.rc"
        "${NSO_DISCORD_SDK_RC}"
    )
elseif(APPLE)
    enable_language(OBJCXX)
    set(NSO_MACOS_ICON "${CMAKE_SOURCE_DIR}/app.icns")
    set_source_files_properties(
        "${NSO_MACOS_ICON}"
        "${NSO_THIRD_PARTY_NOTICES}"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
    )
    list(APPEND NSO_SOURCES
        src/auth_callback_macos.mm
        src/http_openssl.cpp
        src/platform_macos.mm
        src/secure_store_macos.mm
        "${NSO_MACOS_ICON}"
        "${NSO_THIRD_PARTY_NOTICES}"
    )
else()
    list(APPEND NSO_SOURCES
        src/auth_callback_linux.cpp
        src/http_openssl.cpp
        src/platform_linux.cpp
        src/secure_store_linux.cpp
    )
endif()

if(WIN32)
    add_executable(nso-album-sync WIN32 ${NSO_SOURCES} ${NSO_HEADERS})
elseif(APPLE)
    add_executable(nso-album-sync MACOSX_BUNDLE ${NSO_SOURCES} ${NSO_HEADERS})
    set_target_properties(
        nso-album-sync
        PROPERTIES
            MACOSX_BUNDLE_BUNDLE_NAME "NSO Album Sync"
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.dycool.nso-album-sync"
            MACOSX_BUNDLE_ICON_FILE "app.icns"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/Info.plist.in"
            BUILD_RPATH "@executable_path/../Frameworks"
            INSTALL_RPATH "@executable_path/../Frameworks"
    )
else()
    add_executable(nso-album-sync ${NSO_SOURCES} ${NSO_HEADERS})
    set_target_properties(
        nso-album-sync
        PROPERTIES
            BUILD_RPATH "$ORIGIN;$ORIGIN/../lib"
            INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib"
    )
endif()

target_include_directories(
    nso-album-sync
    PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${NSO_DISCORD_SDK_INCLUDE_DIR}"
)
target_compile_definitions(
    nso-album-sync
    PRIVATE
        NSO_DISCORD_SOCIAL_SDK_VERSION="${NSO_DISCORD_SOCIAL_SDK_VERSION}"
        NSO_DISCORD_SDK_RESOURCE_ID=${NSO_DISCORD_SDK_RESOURCE_ID}
        NSO_THIRD_PARTY_NOTICES_RESOURCE_ID=${NSO_THIRD_PARTY_NOTICES_RESOURCE_ID}
)
target_link_libraries(nso-album-sync PRIVATE "${NSO_DISCORD_SDK_LINK_LIBRARY}")

if(MSVC)
    target_link_options(nso-album-sync PRIVATE /DELAYLOAD:discord_partner_sdk.dll)
    target_link_libraries(nso-album-sync PRIVATE delayimp)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT NSO_IPO_SUPPORTED OUTPUT NSO_IPO_ERROR LANGUAGES CXX)
if(NSO_IPO_SUPPORTED)
    set_property(TARGET nso-album-sync PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
else()
    message(STATUS "IPO/LTO disabled: ${NSO_IPO_ERROR}")
endif()

if(NOT WIN32)
    target_link_libraries(nso-album-sync PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(NSO_STATIC_RUNTIME AND UNIX AND NOT APPLE)
    target_link_options(nso-album-sync PRIVATE -static-libgcc -static-libstdc++)
    find_library(
        NSO_ZSTD_STATIC
        NAMES libzstd.a zstd.a
        PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib
    )
    if(NSO_ZSTD_STATIC)
        target_link_libraries(nso-album-sync PRIVATE ${NSO_ZSTD_STATIC})
    endif()
endif()

if(WIN32)
    target_link_libraries(
        nso-album-sync
        PRIVATE advapi32 bcrypt crypt32 winhttp shell32 ole32 comdlg32
    )
elseif(APPLE)
    find_library(NSO_COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(NSO_CORE_SERVICES_FRAMEWORK CoreServices REQUIRED)
    find_library(NSO_SECURITY_FRAMEWORK Security REQUIRED)
    find_library(NSO_USER_NOTIFICATIONS_FRAMEWORK UserNotifications REQUIRED)

    target_link_libraries(
        nso-album-sync
        PRIVATE
            ${NSO_COCOA_FRAMEWORK}
            ${NSO_CORE_SERVICES_FRAMEWORK}
            ${NSO_SECURITY_FRAMEWORK}
            ${NSO_USER_NOTIFICATIONS_FRAMEWORK}
    )

    find_program(NSO_CODESIGN_EXECUTABLE codesign REQUIRED)
    find_program(NSO_DITTO_EXECUTABLE ditto REQUIRED)
    if(NSO_DISCORD_SDK_IS_FRAMEWORK)
        set(NSO_BUNDLED_DISCORD_FRAMEWORK
            "$<TARGET_BUNDLE_DIR:nso-album-sync>/Contents/Frameworks/discord_partner_sdk.framework")
        add_custom_command(
            TARGET nso-album-sync
            POST_BUILD
            COMMAND
                "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_BUNDLE_DIR:nso-album-sync>/Contents/Frameworks"
            COMMAND
                "${CMAKE_COMMAND}" -E rm -rf
                "${NSO_BUNDLED_DISCORD_FRAMEWORK}"
            COMMAND
                "${NSO_DITTO_EXECUTABLE}"
                "${NSO_DISCORD_SDK_RUNTIME}"
                "${NSO_BUNDLED_DISCORD_FRAMEWORK}"
            # A GitHub web upload represents framework symlinks as their link
            # target text. Recreate the canonical framework links in the app.
            COMMAND "${CMAKE_COMMAND}" -E rm -f
                "${NSO_BUNDLED_DISCORD_FRAMEWORK}/Versions/Current"
                "${NSO_BUNDLED_DISCORD_FRAMEWORK}/Frameworks"
                "${NSO_BUNDLED_DISCORD_FRAMEWORK}/Headers"
                "${NSO_BUNDLED_DISCORD_FRAMEWORK}/Resources"
                "${NSO_BUNDLED_DISCORD_FRAMEWORK}/discord_partner_sdk"
            COMMAND "${CMAKE_COMMAND}" -E create_symlink
                "A" "${NSO_BUNDLED_DISCORD_FRAMEWORK}/Versions/Current"
            COMMAND "${CMAKE_COMMAND}" -E create_symlink
                "Versions/Current/Frameworks" "${NSO_BUNDLED_DISCORD_FRAMEWORK}/Frameworks"
            COMMAND "${CMAKE_COMMAND}" -E create_symlink
                "Versions/Current/Headers" "${NSO_BUNDLED_DISCORD_FRAMEWORK}/Headers"
            COMMAND "${CMAKE_COMMAND}" -E create_symlink
                "Versions/Current/Resources" "${NSO_BUNDLED_DISCORD_FRAMEWORK}/Resources"
            COMMAND "${CMAKE_COMMAND}" -E create_symlink
                "Versions/Current/discord_partner_sdk" "${NSO_BUNDLED_DISCORD_FRAMEWORK}/discord_partner_sdk"
            COMMAND
                "${NSO_CODESIGN_EXECUTABLE}"
                --force
                --deep
                --sign "${NSO_MACOS_CODESIGN_IDENTITY}"
                --timestamp=none
                "$<TARGET_BUNDLE_DIR:nso-album-sync>"
            COMMAND
                "${NSO_CODESIGN_EXECUTABLE}"
                --verify
                --deep
                --strict
                --verbose=2
                "$<TARGET_BUNDLE_DIR:nso-album-sync>"
            COMMENT "Bundling Discord Social SDK framework, notices, and locally code-signing NSO Album Sync.app"
            VERBATIM
        )
    else()
        add_custom_command(
            TARGET nso-album-sync
            POST_BUILD
            COMMAND
                "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_BUNDLE_DIR:nso-album-sync>/Contents/Frameworks"
            COMMAND
                "${CMAKE_COMMAND}" -E copy_if_different
                "${NSO_DISCORD_SDK_RUNTIME}"
                "$<TARGET_BUNDLE_DIR:nso-album-sync>/Contents/Frameworks/libdiscord_partner_sdk.dylib"
            COMMAND
                "${NSO_CODESIGN_EXECUTABLE}"
                --force
                --deep
                --sign "${NSO_MACOS_CODESIGN_IDENTITY}"
                --timestamp=none
                "$<TARGET_BUNDLE_DIR:nso-album-sync>"
            COMMAND
                "${NSO_CODESIGN_EXECUTABLE}"
                --verify
                --deep
                --strict
                --verbose=2
                "$<TARGET_BUNDLE_DIR:nso-album-sync>"
            COMMENT "Bundling Discord Social SDK, notices, and locally code-signing NSO Album Sync.app"
            VERBATIM
        )
    endif()
else()
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSECRET QUIET libsecret-1)
        if(LIBSECRET_FOUND)
            target_compile_definitions(nso-album-sync PRIVATE NSO_HAVE_LIBSECRET=1)
            target_include_directories(nso-album-sync PRIVATE ${LIBSECRET_INCLUDE_DIRS})
            target_link_libraries(nso-album-sync PRIVATE ${LIBSECRET_LIBRARIES})
        endif()

        if(NSO_ENABLE_LINUX_TRAY)
            pkg_check_modules(GTK3 QUIET gtk+-3.0)
            pkg_check_modules(APPIND QUIET ayatana-appindicator3-0.1)
            if(GTK3_FOUND)
                target_compile_definitions(nso-album-sync PRIVATE NSO_HAVE_GTK=1)
                target_include_directories(nso-album-sync PRIVATE ${GTK3_INCLUDE_DIRS})
                target_link_libraries(nso-album-sync PRIVATE ${GTK3_LIBRARIES})
                if(APPIND_FOUND)
                    target_compile_definitions(nso-album-sync PRIVATE NSO_HAVE_APPINDICATOR=1)
                    target_include_directories(nso-album-sync PRIVATE ${APPIND_INCLUDE_DIRS})
                    target_link_libraries(nso-album-sync PRIVATE ${APPIND_LIBRARIES})
                endif()
            endif()
        endif()
    endif()

    add_custom_command(
        TARGET nso-album-sync
        POST_BUILD
        COMMAND
            "${CMAKE_COMMAND}" -E copy_if_different
            "${NSO_DISCORD_SDK_RUNTIME}"
            "$<TARGET_FILE_DIR:nso-album-sync>/libdiscord_partner_sdk.so"
        COMMENT "Bundling Discord Social SDK runtime"
        VERBATIM
    )
endif()

install(
    TARGETS nso-album-sync
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION .
)
if(NOT WIN32 AND NOT APPLE)
    install(
        FILES "${NSO_THIRD_PARTY_NOTICES}"
        DESTINATION share/doc/nso-album-sync
    )
endif()
