cmake_minimum_required(VERSION 3.16)
project(pacmkr VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
configure_file(packaging/com.pacmkr.policy.in com.pacmkr.policy @ONLY)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

string(TOUPPER "${CMAKE_BUILD_TYPE}" PACMKR_BUILD_TYPE_UPPER)
set(PACMKR_EFFECTIVE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
if(PACMKR_BUILD_TYPE_UPPER AND DEFINED CMAKE_CXX_FLAGS_${PACMKR_BUILD_TYPE_UPPER})
    string(APPEND PACMKR_EFFECTIVE_CXX_FLAGS
        " ${CMAKE_CXX_FLAGS_${PACMKR_BUILD_TYPE_UPPER}}")
endif()
string(STRIP "${PACMKR_EFFECTIVE_CXX_FLAGS}" PACMKR_EFFECTIVE_CXX_FLAGS)
configure_file(src/app/build_info.h.in generated/pacmkr/build_info.h @ONLY)

# ─── Options ─────────────────────────────────────────────────────────
option(PACMKR_BUILD_TESTS "Build tests" OFF)
option(PACMKR_USE_OPENSSL "Use OpenSSL for HTTPS (cpp-httplib)" ON)
option(PACMKR_BUILD_GTK_GUI "Build the GTK4 GUI dashboard" OFF)

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
    src/app/terminal.cpp
    src/app/json_output.cpp
    src/backend/alpm.cpp
    src/backend/aur.cpp
    src/backend/aur_cache.cpp
    src/backend/deps.cpp
    src/backend/hybrid_search.cpp
    src/backend/remove.cpp
    src/build/build.cpp
    src/build/pgp.cpp
    src/build/pkgbuild.cpp
    src/build/source.cpp
    src/core/config.cpp
    src/core/disk.cpp
    src/core/fuzzy_search.cpp
    src/core/optimize.cpp
    src/core/package.cpp
    src/core/pacman_config.cpp
    src/app/cli.cpp
)

# ─── GTK GUI resources ───────────────────────────────────────────────
set(PACMKR_GTK_GUI_RESOURCES "")
if(PACMKR_BUILD_GTK_GUI)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)
    pkg_check_modules(GIOUNIX REQUIRED gio-unix-2.0)
    find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)

    set(PACMKR_GTK_GUI_RESOURCES "${CMAKE_CURRENT_BINARY_DIR}/gui-gtk-resources.cpp")
    add_custom_command(
        OUTPUT "${PACMKR_GTK_GUI_RESOURCES}"
        COMMAND "${GLIB_COMPILE_RESOURCES}"
        --generate-source
        --target "${PACMKR_GTK_GUI_RESOURCES}"
        --sourcedir "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/gtk"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/gtk/resources.xml"
        DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/gtk/resources.xml"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/gtk/pacmkr.css"
        VERBATIM
    )
endif()

add_executable(pacmkr ${PACMKR_SOURCES})

add_executable(pacmkr-repo
    src/repo/main.cpp
    src/backend/local_repo.cpp
    src/core/optimize.cpp
)
target_include_directories(pacmkr-repo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(pacmkr PRIVATE third_party)
target_include_directories(pacmkr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
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
    add_executable(test_pkgbuild tests/test_pkgbuild.cpp src/build/pkgbuild.cpp)
    add_executable(test_cli
        tests/test_cli.cpp
        src/app/cli.cpp
        src/app/operations.cpp
        src/core/pacman_config.cpp
    )
    add_executable(test_optimize
        tests/test_optimize.cpp
        src/core/optimize.cpp
    )
    add_executable(test_alpm_integration
        tests/test_alpm_integration.cpp
        src/backend/alpm.cpp
        src/app/terminal.cpp
        src/core/pacman_config.cpp
        src/core/package.cpp
    )
    add_executable(test_source
        tests/test_source.cpp
        src/build/source.cpp
        src/build/pkgbuild.cpp
        src/app/terminal.cpp
    )
    add_executable(test_cli_integration
        tests/test_cli_integration.cpp
        src/backend/alpm.cpp
        src/app/terminal.cpp
        src/core/pacman_config.cpp
        src/core/package.cpp
    )
    add_executable(test_local_repo
        tests/test_local_repo.cpp
        src/backend/local_repo.cpp
        src/core/optimize.cpp
    )
    target_include_directories(test_optimize PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_include_directories(test_pkgbuild PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_include_directories(test_cli PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    target_include_directories(test_alpm_integration PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_include_directories(test_source PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_include_directories(test_cli_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_include_directories(test_local_repo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(test_cli_integration PRIVATE PACMKR_TEST_EXECUTABLE="$<TARGET_FILE:pacmkr>")
    add_dependencies(test_cli_integration pacmkr)
    foreach(test_target test_deps test_pkgbuild test_cli test_optimize test_alpm_integration test_source test_cli_integration test_local_repo)
        target_compile_options(${test_target} PRIVATE -UNDEBUG)
    endforeach()
    if(CMAKE_SYSTEM_NAME MATCHES "Linux")
        find_library(ALPM_LIBRARY NAMES alpm REQUIRED)
        target_link_libraries(test_alpm_integration PRIVATE ${ALPM_LIBRARY} pthread)
        target_link_libraries(test_cli_integration PRIVATE ${ALPM_LIBRARY} pthread)
    endif()

    add_test(NAME deps COMMAND test_deps)
    add_test(NAME pkgbuild COMMAND test_pkgbuild)
    add_test(NAME cli COMMAND test_cli)
    add_test(NAME optimize COMMAND test_optimize)
    add_test(NAME alpm_integration COMMAND test_alpm_integration)
    add_test(NAME source COMMAND test_source)
    add_test(NAME cli_integration COMMAND test_cli_integration)
    add_test(NAME local_repo COMMAND test_local_repo)
endif()

# ─── GTK GUI binary (optional) ───────────────────────────────────────
if(PACMKR_BUILD_GTK_GUI)
    add_executable(pacmkr-gui
        src/gui/gtk/main.cpp
        src/gui/gtk/backend.cpp
        src/app/terminal.cpp
        src/app/json_output.cpp
        ${PACMKR_GTK_GUI_RESOURCES}
        src/backend/alpm.cpp
        src/backend/aur.cpp
        src/backend/aur_cache.cpp
        src/backend/deps.cpp
        src/backend/remove.cpp
        src/build/build.cpp
        src/build/pgp.cpp
        src/build/pkgbuild.cpp
        src/build/source.cpp
        src/core/config.cpp
        src/core/optimize.cpp
        src/core/package.cpp
        src/core/pacman_config.cpp
    )
    target_link_libraries(pacmkr-gui PRIVATE
        third_party
        ${GTKMM_LIBRARIES}
        ${GIOUNIX_LIBRARIES}
    )
    target_include_directories(pacmkr-gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${GTKMM_INCLUDE_DIRS}
        ${GIOUNIX_INCLUDE_DIRS}
    )
    target_compile_definitions(pacmkr-gui PRIVATE
        PACMKR_GUI_ENABLED
        PACMKR_SOURCE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
        PACMKR_INSTALLED_EXECUTABLE="${CMAKE_INSTALL_FULL_BINDIR}/pacmkr"
        PACKAGE_VERSION="${PROJECT_VERSION}"
    )
    if(CMAKE_SYSTEM_NAME MATCHES "Linux")
        find_library(ALPM_LIBRARY NAMES alpm REQUIRED)
        target_link_libraries(pacmkr-gui PRIVATE ${ALPM_LIBRARY} pthread)
    endif()
endif()

# ─── Install ─────────────────────────────────────────────────────────
install(TARGETS pacmkr pacmkr-repo RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(PACMKR_BUILD_GTK_GUI)
    install(TARGETS pacmkr-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/com.pacmkr.gui.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/com.pacmkr.gui.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/com.pacmkr.policy"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)
    install(FILES assets/pacmkr-logo.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
        RENAME com.pacmkr.gui.png)
endif()
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
