cmake_minimum_required(VERSION 3.16)
project(pacmkr VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── CPM.cmake (package manager) ─────────────────────────────────────
set(CPM_DOWNLOAD_VERSION 0.40.2)
if(CPM_SOURCE_CACHE)
    set(CPM_DOWNLOAD_LOCATION "${CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
else()
    set(CPM_DOWNLOAD_LOCATION "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
endif()

if(NOT (EXISTS ${CPM_DOWNLOAD_LOCATION}))
    message(STATUS "Downloading CPM.cmake to ${CPM_DOWNLOAD_LOCATION}")
    file(DOWNLOAD
        https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake
        ${CPM_DOWNLOAD_LOCATION}
        EXPECTED_HASH SHA256=c8cdc32c03816538ce22781ed72964dc864b2a34a310d3b7104812a5ca2d835d
        TIMEOUT 30
        STATUS download_status
    )
    list(GET download_status 0 status_code)
    if(NOT status_code EQUAL 0)
        message(WARNING "Failed to download CPM.cmake (offline mode).")
        set(CPM_ENABLED OFF)
    else()
        include(${CPM_DOWNLOAD_LOCATION})
        set(CPM_ENABLED ON)
    endif()
else()
    include(${CPM_DOWNLOAD_LOCATION})
    set(CPM_ENABLED ON)
endif()

# ─── Options ─────────────────────────────────────────────────────────
option(PACMKR_BUILD_TESTS "Build tests" ON)
option(PACMKR_USE_OPENSSL "Use OpenSSL for HTTPS (cpp-httplib)" ON)

# ─── Dependencies ────────────────────────────────────────────────────
add_library(third_party INTERFACE)
target_include_directories(third_party INTERFACE third_party)

if(PACMKR_USE_OPENSSL)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(third_party INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
    target_link_libraries(third_party INTERFACE OpenSSL::SSL OpenSSL::Crypto)
endif()

# ─── Main binary ─────────────────────────────────────────────────────
set(PACMKR_SOURCES
    src/app/main.cpp
    src/app/operations.cpp
    src/backend/alpm.cpp
    src/backend/aur.cpp
    src/backend/aur_cache.cpp
    src/backend/deps.cpp
    src/backend/pacman.cpp
    src/backend/remove.cpp
    src/build/build.cpp
    src/build/pgp.cpp
    src/build/pkgbuild.cpp
    src/build/source.cpp
    src/core/config.cpp
    src/core/optimize.cpp
    src/core/package.cpp
    src/tui/cli.cpp
    src/tui/progress.cpp
    src/tui/status.cpp
)

add_executable(pacmkr ${PACMKR_SOURCES})

target_link_libraries(pacmkr PRIVATE third_party)
target_include_directories(pacmkr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Preserve the domain layout in IDE project views.
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src PREFIX "Source" FILES
    ${PACMKR_SOURCES}
)

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    find_library(ALPM_LIBRARY NAMES alpm REQUIRED)
    target_include_directories(pacmkr PRIVATE /usr/include)
    target_link_libraries(pacmkr PRIVATE ${ALPM_LIBRARY} pthread)
endif()

# ─── Tests ───────────────────────────────────────────────────────────
if(PACMKR_BUILD_TESTS)
    enable_testing()

    add_executable(test_deps tests/test_deps.cpp)
    add_executable(test_pkgbuild tests/test_pkgbuild.cpp)
    add_executable(test_cli
        tests/test_cli.cpp
        src/tui/cli.cpp
        src/app/operations.cpp
    )
    target_include_directories(test_cli PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    add_test(NAME deps COMMAND test_deps)
    add_test(NAME pkgbuild COMMAND test_pkgbuild)
    add_test(NAME cli COMMAND test_cli)
endif()

# ─── Install ─────────────────────────────────────────────────────────
install(TARGETS pacmkr DESTINATION bin)
