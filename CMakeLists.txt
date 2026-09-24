cmake_minimum_required(VERSION 3.20)
project(UnifiedStreamChat VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)

# --- nlohmann/json ---------------------------------------------------------
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(json)

# --- cpp-httplib (header only: local HTTP server + HTTPS client) ------------
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.18.3
  GIT_SHALLOW ON)
FetchContent_GetProperties(httplib)
if(NOT httplib_POPULATED)
  FetchContent_Populate(httplib)
endif()

# --- IXWebSocket (WebSocket client for Twitch IRC and Kick/Pusher) ---------
set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG v11.4.5
  GIT_SHALLOW ON)
FetchContent_MakeAvailable(ixwebsocket)

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
  src/net/Http.cpp
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
  CPPHTTPLIB_OPENSSL_SUPPORT
  CPPHTTPLIB_THREAD_POOL_COUNT=16
  APP_VERSION="${PROJECT_VERSION}")

target_link_libraries(usc_core PUBLIC
  nlohmann_json::nlohmann_json
  ixwebsocket
  OpenSSL::SSL OpenSSL::Crypto
  Threads::Threads)

if(WIN32)
  target_compile_definitions(usc_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_link_libraries(usc_core PUBLIC ws2_32 crypt32 shell32)
endif()
if(APPLE)
  target_compile_definitions(usc_core PUBLIC CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN)
  target_link_libraries(usc_core PUBLIC "-framework CoreFoundation" "-framework Security")
endif()

if(MSVC)
  target_compile_options(usc_core PUBLIC /W4 /utf-8 /bigobj)
else()
  target_compile_options(usc_core PUBLIC -Wall -Wextra)
endif()

add_executable(unified_stream_chat src/main.cpp)
target_link_libraries(unified_stream_chat PRIVATE usc_core)
if(WIN32)
  set_target_properties(unified_stream_chat PROPERTIES OUTPUT_NAME UnifiedStreamChat)
endif()

option(USC_BUILD_TESTS "Build unit tests" ON)
if(USC_BUILD_TESTS)
  enable_testing()
  add_executable(unit_tests tests/unit_tests.cpp)
  target_link_libraries(unit_tests PRIVATE usc_core)
  add_test(NAME unit_tests COMMAND unit_tests)
endif()

install(TARGETS unified_stream_chat RUNTIME DESTINATION .)
