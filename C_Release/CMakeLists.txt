cmake_minimum_required(VERSION 3.15)

# =============================================================================
# Spacedrop (C edition) — Build Configuration
#
# This Cmake file is the build configuration for the C version of Spacedrop.
# It configures the CivetWeb HTTP server library and builds the Spacedrop server.
#
# Purpose of this file:
#   CivetWeb is a small, embeddable web server written in C. It supports
#   HTTP/HTTPS, WebSockets, CGI, and more. This file configures CivetWeb
#   as an OBJECT library and compiles it directly into our demo server.
#
# TL;DR for contributors:
#   mkdir -p C_Release/build && cd C_Release/build
#   cmake ..                # generate the build system (Makefiles/Xcode/etc.)
#   cmake --build .         # compile
#   ./hello                 # run the demo server on :8080
# =============================================================================

project(spacedrop_c LANGUAGES C)    # Project name and language

# --- Language settings --------------------------------------------------------
set(CMAKE_C_STANDARD 11)            # Use C11 standard
set(CMAKE_C_STANDARD_REQUIRED ON)   # Require C11
set(CMAKE_C_EXTENSIONS OFF)         # No compiler-specific extensions

# --- Feature toggles (you can pass -D…=ON/OFF to cmake) -----------------------
# Example: cmake -DSPACEDROP_ENABLE_TLS=ON -DSPACEDROP_ENABLE_ZLIB=ON ..
option(SPACEDROP_ENABLE_TLS  "Enable HTTPS via OpenSSL or MbedTLS" OFF)
option(SPACEDROP_ENABLE_ZLIB "Enable gzip/deflate features (zlib)" OFF)

# --- Where CivetWeb lives (vendored submodule) --------------------------------
# The repo layout is: C_Release/third_party/civetweb/{include,src,…}
set(CIVETWEB_DIR "${CMAKE_SOURCE_DIR}/third_party/civetweb" CACHE PATH
    "Path to vendored CivetWeb root (contains include/ and src/)")

# Friendly error if the submodule hasn't been fetched yet
if(NOT EXISTS "${CIVETWEB_DIR}/src/civetweb.c")
  message(FATAL_ERROR
    "CivetWeb source not found at: ${CIVETWEB_DIR}\n"
    "Run:  make submodule  (from C_Release/)   — or —   git submodule update --init --recursive")
endif()

# --- Build CivetWeb from source (safe & deterministic) ------------------------
# OBJECT library = we compile civetweb.c once and link its object into targets.
add_library(civetweb_obj OBJECT
  "${CIVETWEB_DIR}/src/civetweb.c"
)

# CivetWeb public headers
target_include_directories(civetweb_obj PUBLIC
  "${CIVETWEB_DIR}/include"
)

# Base defines:
#  • If TLS is OFF, define NO_SSL (no OpenSSL/MbedTLS needed).
#  • Optionally enable zlib (requires libz).
if(NOT SPACEDROP_ENABLE_TLS)
  target_compile_definitions(civetweb_obj PUBLIC NO_SSL)
endif()

if(SPACEDROP_ENABLE_ZLIB)
  target_compile_definitions(civetweb_obj PUBLIC USE_ZLIB)
  find_package(ZLIB) # nice if available
  if(ZLIB_FOUND)
    target_link_libraries(civetweb_obj PUBLIC ZLIB::ZLIB)
  else()
    # macOS usually has libz; fall back to -lz if find_package fails
    find_library(SPACEDROP_ZLIB_LIB z)
    if(NOT SPACEDROP_ZLIB_LIB)
      message(FATAL_ERROR "SPACEDROP_ENABLE_ZLIB=ON but zlib not found.")
    endif()
    target_link_libraries(civetweb_obj PUBLIC ${SPACEDROP_ZLIB_LIB})
  endif()
endif()

# TLS wiring (optional, OFF by default to keep first build simple)
if(SPACEDROP_ENABLE_TLS)
  find_package(OpenSSL)
  if(OPENSSL_FOUND)
    target_compile_definitions(civetweb_obj PUBLIC OPENSSL_API_3_0)
    target_link_libraries(civetweb_obj PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  else()
    # Try MbedTLS via Homebrew (/usr/local or /opt/homebrew)
    find_path(MBEDTLS_INCLUDE_DIR mbedtls/ssl.h PATHS /usr/local/include /opt/homebrew/include)
    find_library(MBEDTLS_LIB     mbedtls     PATHS /usr/local/lib /opt/homebrew/lib)
    find_library(MBEDX509_LIB    mbedx509    PATHS /usr/local/lib /opt/homebrew/lib)
    find_library(MBEDCRYPTO_LIB  mbedcrypto  PATHS /usr/local/lib /opt/homebrew/lib)
    if(MBEDTLS_INCLUDE_DIR AND MBEDTLS_LIB AND MBEDX509_LIB AND MBEDCRYPTO_LIB)
      target_include_directories(civetweb_obj PUBLIC ${MBEDTLS_INCLUDE_DIR})
      target_compile_definitions(civetweb_obj PUBLIC USE_MBEDTLS)
      target_link_libraries(civetweb_obj PUBLIC ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})
    else()
      message(FATAL_ERROR
        "TLS requested but neither OpenSSL nor MbedTLS found.\n"
        "Install one (e.g., `brew install openssl@3` or `brew install mbedtls`),\n"
        "or set -DSPACEDROP_ENABLE_TLS=OFF.")
    endif()
  endif()
endif()

# --- Our example app ----------------------------------------------------------
add_executable(hello
  "${CMAKE_SOURCE_DIR}/hello.c"
  "${CMAKE_SOURCE_DIR}/modules/env_module/env_module.c"
  "${CMAKE_SOURCE_DIR}/modules/pages_module/pages_module.c"
  "${CMAKE_SOURCE_DIR}/modules/drop_module/drop_module.c"
  "${CMAKE_SOURCE_DIR}/modules/auth_module/auth_module.c"
)

target_include_directories(hello PRIVATE
  "${CIVETWEB_DIR}/include"
  "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(hello PRIVATE civetweb_obj pthread)

# --- Friendly summary ---------------------------------------------------------
message(STATUS "==============================================================")
message(STATUS "Spacedrop C build configuration")
message(STATUS "  CIVETWEB_DIR        : ${CIVETWEB_DIR}")
message(STATUS "  TLS enabled         : ${SPACEDROP_ENABLE_TLS}")
message(STATUS "  zlib enabled        : ${SPACEDROP_ENABLE_ZLIB}")
message(STATUS "  Build generator     : ${CMAKE_GENERATOR}")
message(STATUS "==============================================================")