cmake_minimum_required(VERSION 3.21)

# MSVC: one self-contained .exe with the C/C++ runtime linked statically
# (/MT, /MTd in Debug). Set globally *before* any dependency is configured so
# FetchContent subprojects (miniz, ...) use the same runtime - a mix of /MT
# and /MD causes LNK4098 / LNK4217 and runtime-heap mismatches. The policy
# default makes subprojects with an older cmake_minimum_required honour it.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC runtime library")

project(UnifiedStreamChat VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies
#
# Windows: no OpenSSL at all. HTTPS and WebSockets use WinHTTP, which ships
#          with Windows (TLS via SChannel + the Windows certificate store).
#          Opening the folder in Visual Studio 2022 is enough to build.
# Linux/macOS: OpenSSL (system package) + IXWebSocket.
#
# Each dependency is taken from vcpkg (vcpkg.json, manifest mode) when a
# vcpkg toolchain is active, otherwise it is downloaded with FetchContent.
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET ON)
# Header-only / hand-built dependencies are fetched with SOURCE_SUBDIR set to
# a folder that has no CMakeLists.txt: FetchContent_MakeAvailable() then only
# downloads them and never runs their own CMake scripts (some declare
# cmake_minimum_required < 3.5, which CMake 4 rejects). This replaces the
# deprecated single-argument FetchContent_Populate() (policy CMP0169).
set(USC_NO_CMAKE SOURCE_SUBDIR usc-download-only)
find_package(Threads REQUIRED)

# nlohmann/json (header only)
find_package(nlohmann_json 3.11 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
  FetchContent_MakeAvailable(json)
endif()

# cpp-httplib (single header: the local HTTP server, and the HTTPS client on
# Linux/macOS). Pinned because its API changes between releases.
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.18.3
  GIT_SHALLOW ON
  ${USC_NO_CMAKE})
FetchContent_MakeAvailable(httplib) # header only

if(NOT WIN32)
  find_package(OpenSSL 3.0 REQUIRED)
  find_package(ixwebsocket CONFIG QUIET)
  if(NOT ixwebsocket_FOUND)
    set(USE_TLS ON CACHE BOOL "" FORCE)
    set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
    set(USE_ZLIB OFF CACHE BOOL "" FORCE)
    set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(ixwebsocket
      GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
      GIT_TAG v11.4.5
      GIT_SHALLOW ON)
    FetchContent_MakeAvailable(ixwebsocket)
    target_compile_options(ixwebsocket PRIVATE -w) # third-party: keep our build log clean
    if(NOT TARGET ixwebsocket::ixwebsocket)
      add_library(ixwebsocket::ixwebsocket ALIAS ixwebsocket)
    endif()
  endif()
endif()

# miniaudio (single header): lists audio output devices and plays TTS natively.
FetchContent_Declare(miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG 0.11.25
  GIT_SHALLOW ON
  ${USC_NO_CMAKE})
FetchContent_MakeAvailable(miniaudio) # header only

# miniz: gzip/deflate for TikTok's compressed WebSocket payloads (works on
# Windows without zlib). Its own CMakeLists.txt needs CMake < 4, so the four
# sources are built here as a plain static library instead.
FetchContent_Declare(miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG 3.0.2
  GIT_SHALLOW ON
  ${USC_NO_CMAKE})
FetchContent_MakeAvailable(miniz)
enable_language(C)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/miniz-include/miniz_export.h
  "#pragma once\n#define MINIZ_EXPORT\n") # static library: no dllexport
add_library(miniz STATIC
  ${miniz_SOURCE_DIR}/miniz.c
  ${miniz_SOURCE_DIR}/miniz_tdef.c
  ${miniz_SOURCE_DIR}/miniz_tinfl.c
  ${miniz_SOURCE_DIR}/miniz_zip.c)
target_include_directories(miniz SYSTEM PUBLIC ${miniz_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/miniz-include)
set_target_properties(miniz PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_definitions(miniz PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# --- Native desktop window (webview: WebView2 / WKWebView / WebKitGTK) ------
# Windows and macOS always get the window. Linux gets it when WebKitGTK
# development files are installed; otherwise the build is server-only.
set(USC_DESKTOP_DEFAULT ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_search_module(USC_WEBKITGTK QUIET webkit2gtk-4.1 webkit2gtk-4.0)
  endif()
  if(NOT USC_WEBKITGTK_FOUND)
    set(USC_DESKTOP_DEFAULT OFF)
    message(STATUS "WebKitGTK not found: building without the desktop window (server/OBS mode only)")
  endif()
endif()
option(USC_DESKTOP_WINDOW "Build the native desktop window" ${USC_DESKTOP_DEFAULT})

if(USC_DESKTOP_WINDOW)
  if(WIN32)
    # WebView2 SDK headers come from NuGet (downloaded, nothing to install).
    # The app ships webview's built-in loader, so no WebView2Loader.dll.
    set(USC_WEBVIEW2_VERSION 1.0.2903.40)
    FetchContent_Declare(mswebview2
      URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${USC_WEBVIEW2_VERSION}/microsoft.web.webview2.${USC_WEBVIEW2_VERSION}.nupkg
      DOWNLOAD_NAME webview2.zip
      DOWNLOAD_EXTRACT_TIMESTAMP ON
      ${USC_NO_CMAKE})
    FetchContent_MakeAvailable(mswebview2) # headers only
    set(MSWebView2_ROOT ${mswebview2_SOURCE_DIR} CACHE PATH "" FORCE)
    set(WEBVIEW_MSWEBVIEW2_VERSION ${USC_WEBVIEW2_VERSION} CACHE STRING "" FORCE)
    if(MINGW)
      set(WEBVIEW_USE_COMPAT_MINGW ON CACHE BOOL "" FORCE)
    endif()
  endif()
  foreach(opt WEBVIEW_BUILD_TESTS WEBVIEW_BUILD_EXAMPLES WEBVIEW_BUILD_DOCS WEBVIEW_INSTALL_DOCS
              WEBVIEW_INSTALL_TARGETS WEBVIEW_BUILD_SHARED_LIBRARY WEBVIEW_BUILD_STATIC_LIBRARY
              WEBVIEW_ENABLE_CHECKS WEBVIEW_ENABLE_CLANG_FORMAT WEBVIEW_ENABLE_CLANG_TIDY WEBVIEW_ENABLE_PACKAGING)
    set(${opt} OFF CACHE BOOL "" FORCE)
  endforeach()
  FetchContent_Declare(webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG 0.12.0
    GIT_SHALLOW ON)
  FetchContent_MakeAvailable(webview)
endif()

# --- Embed the dock UI (web/*) into the binary -----------------------------
set(WEB_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
  ${CMAKE_CURRENT_SOURCE_DIR}/web/app.css
  ${CMAKE_CURRENT_SOURCE_DIR}/web/app.js
  ${CMAKE_CURRENT_SOURCE_DIR}/web/capture.html
  ${CMAKE_CURRENT_SOURCE_DIR}/web/capture.css
  ${CMAKE_CURRENT_SOURCE_DIR}/web/capture.js)
set(EMBED_OUT ${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedAssets.cpp)
add_custom_command(
  OUTPUT ${EMBED_OUT}
  COMMAND ${CMAKE_COMMAND}
          -DOUT=${EMBED_OUT}
          -DWEB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/web
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAssets.cmake
  DEPENDS ${WEB_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAssets.cmake
  COMMENT "Embedding web assets")

# Everything except main() lives in a static library shared with the tests.
add_library(usc_core STATIC
  src/core/App.cpp
  src/core/Settings.cpp
  src/core/TokenStore.cpp
  src/core/MessagePipeline.cpp
  src/core/EventHub.cpp
  src/core/Util.cpp
  src/core/BotFilter.cpp
  src/core/TtsTextFilter.cpp
  src/core/LanguageTools.cpp
  src/core/EmoteRegistry.cpp
  src/net/OAuth.cpp
  src/platforms/TwitchClient.cpp
  src/platforms/YouTubeClient.cpp
  src/platforms/KickClient.cpp
  src/platforms/TikTokClient.cpp
  src/integrations/ObsWatcher.cpp
  src/capture/CaptureTypes.cpp
  src/capture/CaptureRelay.cpp
  src/capture/CaptureStubs.cpp
  src/services/Translator.cpp
  src/services/TtsService.cpp
  src/services/AudioPlayer.cpp
  src/services/miniaudio_impl.cpp
  src/desktop/DesktopWindow.cpp
  src/server/WebServer.cpp
  ${EMBED_OUT})

target_include_directories(usc_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${httplib_SOURCE_DIR}
  ${miniaudio_SOURCE_DIR})

# Built-in Google OAuth client (YouTube login without a Google Cloud setup).
# The secret is never committed: pass -DUSC_GOOGLE_CLIENT_SECRET=... or set
# the USC_GOOGLE_CLIENT_SECRET environment variable when configuring.
set(USC_GOOGLE_CLIENT_ID "233537098772-9ehsehkjvebipcvomcjeujlhrfeocbe9.apps.googleusercontent.com"
    CACHE STRING "Built-in Google OAuth client ID")
set(USC_GOOGLE_CLIENT_SECRET "$ENV{USC_GOOGLE_CLIENT_SECRET}" CACHE STRING "Built-in Google OAuth client secret")
if(USC_GOOGLE_CLIENT_SECRET STREQUAL "")
  message(STATUS "No built-in Google client secret: YouTube login will need one in Settings (or set USC_GOOGLE_CLIENT_SECRET)")
endif()
set_source_files_properties(src/core/App.cpp PROPERTIES COMPILE_DEFINITIONS
  "USC_GOOGLE_CLIENT_ID=\"${USC_GOOGLE_CLIENT_ID}\";USC_GOOGLE_CLIENT_SECRET=\"${USC_GOOGLE_CLIENT_SECRET}\"")

target_compile_definitions(usc_core PUBLIC
  CPPHTTPLIB_THREAD_POOL_COUNT=16
  APP_VERSION="${PROJECT_VERSION}")

target_link_libraries(usc_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads miniz ${CMAKE_DL_LIBS})
if(USC_DESKTOP_WINDOW)
  target_compile_definitions(usc_core PUBLIC USC_HAS_WEBVIEW)
  target_link_libraries(usc_core PUBLIC webview::core)
endif()
if(NOT WIN32 AND NOT APPLE)
  target_link_libraries(usc_core PUBLIC m)
endif()

if(WIN32)
  target_sources(usc_core PRIVATE
    src/net/Http_winhttp.cpp
    src/net/WebSocketClient_winhttp.cpp
    src/capture/MfVideoBackend.cpp
    src/capture/D3DOutputWindow.cpp)
  target_compile_definitions(usc_core PUBLIC
    NOMINMAX WIN32_LEAN_AND_MEAN= _WIN32_WINNT=0x0A00 UNICODE _UNICODE) # empty value matches webview.h
  target_link_libraries(usc_core PUBLIC winhttp ws2_32 shell32 ole32 user32
    # video capture relay: Media Foundation + Direct3D 11 + WIC (all in-box)
    # (dxguid: IID_* for D3D11/DXGI; the code uses __uuidof, this is a backstop)
    mfplat mf mfreadwrite mfuuid d3d11 dxgi dxguid windowscodecs uuid)
else()
  target_sources(usc_core PRIVATE
    src/net/Http_httplib.cpp
    src/net/WebSocketClient_ix.cpp)
  target_compile_definitions(usc_core PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
  target_link_libraries(usc_core PUBLIC ixwebsocket::ixwebsocket OpenSSL::SSL OpenSSL::Crypto)
  if(APPLE)
    target_compile_definitions(usc_core PUBLIC CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN)
    target_link_libraries(usc_core PUBLIC "-framework CoreFoundation" "-framework Security"
      "-framework CoreAudio" "-framework AudioToolbox")
  endif()
endif()

if(MSVC)
  target_compile_options(usc_core PUBLIC /W4 /utf-8 /bigobj /permissive-)
elseif(MINGW)
  target_compile_options(usc_core PUBLIC -Wall -Wextra -Wa,-mbig-obj)
  target_link_options(usc_core PUBLIC -static)
else()
  target_compile_options(usc_core PUBLIC -Wall -Wextra)
endif()

add_executable(unified_stream_chat src/main.cpp)
target_link_libraries(unified_stream_chat PRIVATE usc_core)
if(WIN32)
  set_target_properties(unified_stream_chat PROPERTIES OUTPUT_NAME UnifiedStreamChat)
  if(USC_DESKTOP_WINDOW)
    # A windowed app: no console pops up (logs go to app.log, or to the
    # terminal when started from one). Keeps the standard main() entry.
    set_target_properties(unified_stream_chat PROPERTIES WIN32_EXECUTABLE ON)
    if(MSVC)
      target_link_options(unified_stream_chat PRIVATE /ENTRY:mainCRTStartup)
    endif()
  endif()
  if(MSVC)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT unified_stream_chat)
  endif()
endif()

option(USC_BUILD_TESTS "Build unit tests" ON)
if(USC_BUILD_TESTS)
  enable_testing()
  add_executable(unit_tests tests/unit_tests.cpp)
  target_link_libraries(unit_tests PRIVATE usc_core)
  add_test(NAME unit_tests COMMAND unit_tests)
endif()

install(TARGETS unified_stream_chat RUNTIME DESTINATION .)
