cmake_minimum_required(VERSION 3.21)
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
  GIT_SHALLOW ON)
FetchContent_GetProperties(httplib)
if(NOT httplib_POPULATED)
  FetchContent_Populate(httplib) # header only: no add_subdirectory
endif()

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
    if(NOT TARGET ixwebsocket::ixwebsocket)
      add_library(ixwebsocket::ixwebsocket ALIAS ixwebsocket)
    endif()
  endif()
endif()

# miniaudio (single header): lists audio output devices and plays TTS natively.
FetchContent_Declare(miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG 0.11.25
  GIT_SHALLOW ON)
FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
  FetchContent_Populate(miniaudio) # header only: no add_subdirectory
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
      DOWNLOAD_EXTRACT_TIMESTAMP ON)
    FetchContent_GetProperties(mswebview2)
    if(NOT mswebview2_POPULATED)
      FetchContent_Populate(mswebview2)
    endif()
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
  ${CMAKE_CURRENT_SOURCE_DIR}/web/app.js)
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

target_compile_definitions(usc_core PUBLIC
  CPPHTTPLIB_THREAD_POOL_COUNT=16
  APP_VERSION="${PROJECT_VERSION}")

target_link_libraries(usc_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads ${CMAKE_DL_LIBS})
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
    src/net/WebSocketClient_winhttp.cpp)
  target_compile_definitions(usc_core PUBLIC
    NOMINMAX WIN32_LEAN_AND_MEAN= _WIN32_WINNT=0x0A00 UNICODE _UNICODE) # empty value matches webview.h
  target_link_libraries(usc_core PUBLIC winhttp ws2_32 shell32 ole32 user32)
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
  # One self-contained .exe: link the C++ runtime statically.
  set_property(TARGET usc_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
    set_property(TARGET unified_stream_chat PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT unified_stream_chat)
  endif()
endif()

option(USC_BUILD_TESTS "Build unit tests" ON)
if(USC_BUILD_TESTS)
  enable_testing()
  add_executable(unit_tests tests/unit_tests.cpp)
  target_link_libraries(unit_tests PRIVATE usc_core)
  if(MSVC)
    set_property(TARGET unit_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  add_test(NAME unit_tests COMMAND unit_tests)
endif()

install(TARGETS unified_stream_chat RUNTIME DESTINATION .)
