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
    "1.10.18369"
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

# Rich Presence uses Discord's current Social SDK only. The SDK's direct
# desktop Rich Presence transport does not use OAuth, access tokens, bot
# tokens, client secrets, or Client::Connect().
if(NOT EXISTS "${NSO_DISCORD_SOCIAL_SDK_ROOT}/include/discordpp.h" OR
   NOT EXISTS "${NSO_DISCORD_SOCIAL_SDK_ROOT}/include/cdiscord.h")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    execute_process(
        COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/scripts/fetch-discord-social-sdk.py"
            --version "${NSO_DISCORD_SOCIAL_SDK_VERSION}"
            --destination "${CMAKE_SOURCE_DIR}/third_party"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE NSO_DISCORD_SDK_FETCH_RESULT
        COMMAND_ECHO STDOUT
    )
    if(NOT NSO_DISCORD_SDK_FETCH_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Could not obtain Discord Social SDK ${NSO_DISCORD_SOCIAL_SDK_VERSION}. "
            "Download the official DiscordSocialSdk-${NSO_DISCORD_SOCIAL_SDK_VERSION}.zip "
            "archive from the Discord Developer Portal and place it in third_party/."
        )
    endif()
endif()

set(NSO_DISCORD_SDK_INCLUDE_DIR "${NSO_DISCORD_SOCIAL_SDK_ROOT}/include")
set(NSO_DISCORD_SDK_RESOURCE_ID 201)
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
    # Social SDK 1.10+ ships macOS as a framework; retain a dylib fallback so
    # local builds with an older manually extracted compatible SDK still work.
    set(NSO_DISCORD_SDK_FRAMEWORK
        "${NSO_DISCORD_SOCIAL_SDK_ROOT}/lib/release/discord_partner_sdk.framework")
    if(EXISTS "${NSO_DISCORD_SDK_FRAMEWORK}/discord_partner_sdk")
        set(NSO_DISCORD_SDK_IS_FRAMEWORK TRUE)
        set(NSO_DISCORD_SDK_LINK_LIBRARY
            "${NSO_DISCORD_SDK_FRAMEWORK}/discord_partner_sdk")
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

# Discord's standalone archive has used the paths above for its documented C++
# layout. If a point release rearranges the archive, locate the release binary
# rather than silently falling back to a system-wide Discord library.
if(NOT EXISTS "${NSO_DISCORD_SDK_LINK_LIBRARY}")
    if(WIN32)
        file(GLOB_RECURSE NSO_DISCORD_LINK_CANDIDATES
            "${NSO_DISCORD_SOCIAL_SDK_ROOT}/*discord_partner_sdk.lib")
        file(GLOB_RECURSE NSO_DISCORD_RUNTIME_CANDIDATES
            "${NSO_DISCORD_SOCIAL_SDK_ROOT}/*discord_partner_sdk.dll")
    elseif(APPLE)
        file(GLOB_RECURSE NSO_DISCORD_FRAMEWORK_CANDIDATES
            "${NSO_DISCORD_SOCIAL_SDK_ROOT}/*discord_partner_sdk.framework/discord_partner_sdk")
        if(NSO_DISCORD_FRAMEWORK_CANDIDATES)
            list(FILTER NSO_DISCORD_FRAMEWORK_CANDIDATES EXCLUDE REGEX "/debug/")
        endif()
        if(NSO_DISCORD_FRAMEWORK_CANDIDATES)
            list(GET NSO_DISCORD_FRAMEWORK_CANDIDATES 0 NSO_DISCORD_SDK_LINK_LIBRARY)
            get_filename_component(NSO_DISCORD_SDK_RUNTIME
                "${NSO_DISCORD_SDK_LINK_LIBRARY}" DIRECTORY)
            set(NSO_DISCORD_SDK_IS_FRAMEWORK TRUE)
        else()
            file(GLOB_RECURSE NSO_DISCORD_LINK_CANDIDATES
                "${NSO_DISCORD_SOCIAL_SDK_ROOT}/*libdiscord_partner_sdk.dylib")
        endif()
    else()
        file(GLOB_RECURSE NSO_DISCORD_LINK_CANDIDATES
            "${NSO_DISCORD_SOCIAL_SDK_ROOT}/*libdiscord_partner_sdk.so")
    endif()

    if(NSO_DISCORD_LINK_CANDIDATES AND NOT NSO_DISCORD_SDK_IS_FRAMEWORK)
        list(FILTER NSO_DISCORD_LINK_CANDIDATES EXCLUDE REGEX "/debug/")
        if(NSO_DISCORD_LINK_CANDIDATES)
            list(GET NSO_DISCORD_LINK_CANDIDATES 0 NSO_DISCORD_SDK_LINK_LIBRARY)
            if(NOT WIN32)
                set(NSO_DISCORD_SDK_RUNTIME "${NSO_DISCORD_SDK_LINK_LIBRARY}")
            endif()
        endif()
    endif()
    if(WIN32 AND NSO_DISCORD_RUNTIME_CANDIDATES)
        list(FILTER NSO_DISCORD_RUNTIME_CANDIDATES EXCLUDE REGEX "/debug/")
        if(NSO_DISCORD_RUNTIME_CANDIDATES)
            list(GET NSO_DISCORD_RUNTIME_CANDIDATES 0 NSO_DISCORD_SDK_RUNTIME)
        endif()
    endif()
endif()

if(NOT EXISTS "${NSO_DISCORD_SDK_LINK_LIBRARY}" OR
   NOT EXISTS "${NSO_DISCORD_SDK_RUNTIME}")
    message(FATAL_ERROR
        "Discord Social SDK ${NSO_DISCORD_SOCIAL_SDK_VERSION} is missing the runtime/link library for this platform"
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

    # Keep the Windows release as one .exe: embed Discord's signed runtime as
    # RCDATA and delay-load it after extracting it to the user's temp folder.
    file(TO_CMAKE_PATH "${NSO_DISCORD_SDK_RUNTIME}" NSO_DISCORD_SDK_RUNTIME_RC)
    set(NSO_DISCORD_SDK_RC "${CMAKE_CURRENT_BINARY_DIR}/discord-social-sdk.rc")
    file(WRITE "${NSO_DISCORD_SDK_RC}"
        "${NSO_DISCORD_SDK_RESOURCE_ID} RCDATA \"${NSO_DISCORD_SDK_RUNTIME_RC}\"\n")

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
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
    )
    list(APPEND NSO_SOURCES
        src/auth_callback_macos.mm
        src/http_openssl.cpp
        src/platform_macos.mm
        src/secure_store_macos.mm
        "${NSO_MACOS_ICON}"
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
        add_custom_command(
            TARGET nso-album-sync
            POST_BUILD
            COMMAND
                "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_BUNDLE_DIR:nso-album-sync>/Contents/Frameworks"
            COMMAND
                "${CMAKE_COMMAND}" -E rm -rf
                "$<TARGET_BUNDLE_DIR:nso-album-sync>/Contents/Frameworks/discord_partner_sdk.framework"
            COMMAND
                "${NSO_DITTO_EXECUTABLE}"
                "${NSO_DISCORD_SDK_RUNTIME}"
                "$<TARGET_BUNDLE_DIR:nso-album-sync>/Contents/Frameworks/discord_partner_sdk.framework"
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
            COMMENT "Bundling Discord Social SDK framework and locally code-signing NSO Album Sync.app"
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
            COMMENT "Bundling Discord Social SDK and locally code-signing NSO Album Sync.app"
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
