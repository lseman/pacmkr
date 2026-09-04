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
option(PACMKR_STRICT_WARNINGS "Treat compiler warnings as errors" OFF)
option(PACMKR_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)

add_library(pacmkr_project_options INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pacmkr_project_options INTERFACE
        -Wall -Wextra -Wpedantic)
    if(PACMKR_STRICT_WARNINGS)
        target_compile_options(pacmkr_project_options INTERFACE -Werror)
    endif()
    if(PACMKR_ENABLE_SANITIZERS)
        target_compile_options(pacmkr_project_options INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(pacmkr_project_options INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
endif()

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
    src/backend/aur_review.cpp
    src/backend/aur_cache.cpp
    src/backend/deps.cpp
    src/backend/dep_graph.cpp
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

target_link_libraries(pacmkr PRIVATE third_party pacmkr_project_options)
target_link_libraries(pacmkr-repo PRIVATE pacmkr_project_options)
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

    add_executable(test_deps tests/test_deps.cpp src/backend/dep_graph.cpp)
    target_include_directories(test_deps PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
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
    add_executable(test_aur_review
        tests/test_aur_review.cpp
        src/backend/aur_review.cpp
        src/app/terminal.cpp
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
    target_include_directories(test_aur_review PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(test_cli_integration PRIVATE PACMKR_TEST_EXECUTABLE="$<TARGET_FILE:pacmkr>")
    add_dependencies(test_cli_integration pacmkr)
    foreach(test_target test_deps test_pkgbuild test_cli test_optimize test_alpm_integration test_source test_cli_integration test_local_repo test_aur_review)
        target_compile_options(${test_target} PRIVATE -UNDEBUG)
        target_link_libraries(${test_target} PRIVATE pacmkr_project_options)
    endforeach()
    if(CMAKE_SYSTEM_NAME MATCHES "Linux")
        find_library(ALPM_LIBRARY NAMES alpm REQUIRED)
        target_link_libraries(test_alpm_integration PRIVATE ${ALPM_LIBRARY} pthread)
        target_link_libraries(test_cli_integration PRIVATE ${ALPM_LIBRARY} pthread)
        target_link_libraries(test_deps PRIVATE ${ALPM_LIBRARY})
    endif()

    add_test(NAME deps COMMAND test_deps)
    add_test(NAME pkgbuild COMMAND test_pkgbuild)
    add_test(NAME cli COMMAND test_cli)
    add_test(NAME optimize COMMAND test_optimize)
    add_test(NAME alpm_integration COMMAND test_alpm_integration)
    add_test(NAME source COMMAND test_source)
    add_test(NAME cli_integration COMMAND test_cli_integration)
    add_test(NAME local_repo COMMAND test_local_repo)
    add_test(NAME aur_review COMMAND test_aur_review)
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
        src/backend/aur_review.cpp
        src/backend/aur_cache.cpp
        src/backend/deps.cpp
        src/backend/dep_graph.cpp
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
    )
    target_link_libraries(pacmkr-gui PRIVATE
        third_party
        pacmkr_project_options
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

# ─── Man pages ───────────────────────────────────────────────────────
install(FILES doc/pacmkr.1 doc/pacmkr-repo.1
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

# ─── Shell completions ───────────────────────────────────────────────
# Honor the distro-standard locations; overridable from the cache.
set(PACMKR_BASHCOMPLETION_DIR "${CMAKE_INSTALL_DATADIR}/bash-completion/completions"
    CACHE PATH "Install directory for bash completions")
set(PACMKR_ZSHCOMPLETION_DIR "${CMAKE_INSTALL_DATADIR}/zsh/site-functions"
    CACHE PATH "Install directory for zsh completions")
set(PACMKR_FISHCOMPLETION_DIR "${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d"
    CACHE PATH "Install directory for fish completions")

install(FILES completions/pacmkr.bash
    DESTINATION ${PACMKR_BASHCOMPLETION_DIR} RENAME pacmkr)
install(FILES completions/pacmkr-repo.bash
    DESTINATION ${PACMKR_BASHCOMPLETION_DIR} RENAME pacmkr-repo)
install(FILES completions/pacmkr.zsh
    DESTINATION ${PACMKR_ZSHCOMPLETION_DIR} RENAME _pacmkr)
install(FILES completions/pacmkr-repo.zsh
    DESTINATION ${PACMKR_ZSHCOMPLETION_DIR} RENAME _pacmkr-repo)
install(FILES completions/pacmkr.fish completions/pacmkr-repo.fish
    DESTINATION ${PACMKR_FISHCOMPLETION_DIR})
