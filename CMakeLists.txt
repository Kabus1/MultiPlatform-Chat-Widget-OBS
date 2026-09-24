cmake_minimum_required(VERSION 3.21)
project(UnifiedStreamChat VERSION 0.2.0 LANGUAGES CXX)

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
  src/net/OAuth.cpp
  src/platforms/TwitchClient.cpp
  src/platforms/YouTubeClient.cpp
  src/platforms/KickClient.cpp
  src/services/Translator.cpp
  src/services/TtsService.cpp
  src/server/WebServer.cpp
  ${EMBED_OUT})

target_include_directories(usc_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${httplib_SOURCE_DIR})

target_compile_definitions(usc_core PUBLIC
  CPPHTTPLIB_THREAD_POOL_COUNT=16
  APP_VERSION="${PROJECT_VERSION}")

target_link_libraries(usc_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)

if(WIN32)
  target_sources(usc_core PRIVATE
    src/net/Http_winhttp.cpp
    src/net/WebSocketClient_winhttp.cpp)
  target_compile_definitions(usc_core PUBLIC
    NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 UNICODE _UNICODE)
  target_link_libraries(usc_core PUBLIC winhttp ws2_32 shell32)
else()
  target_sources(usc_core PRIVATE
    src/net/Http_httplib.cpp
    src/net/WebSocketClient_ix.cpp)
  target_compile_definitions(usc_core PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
  target_link_libraries(usc_core PUBLIC ixwebsocket::ixwebsocket OpenSSL::SSL OpenSSL::Crypto)
  if(APPLE)
    target_compile_definitions(usc_core PUBLIC CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN)
    target_link_libraries(usc_core PUBLIC "-framework CoreFoundation" "-framework Security")
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
