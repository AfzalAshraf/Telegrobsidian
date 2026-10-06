cmake_minimum_required(VERSION 3.20)

project(Telegrobsidian
    VERSION 1.0.0
    DESCRIPTION "Obsidian <-> Telegram <-> Notion sync daemon"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(TELEGROBSIDIAN_WITH_SSL "Enable TLS (needed for the Notion API and Telegram)" ON)
option(TELEGROBSIDIAN_WARNINGS_AS_ERRORS "Fail the build on any warning" OFF)
option(TELEGROBSIDIAN_BUILD_TESTS "Register the end-to-end smoke test with CTest" ON)

find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
# Dependencies
# -----------------------------------------------------------------------------
# httplib.h and nlohmann/json.hpp are vendored under include/ so the build works
# on a machine without network access (see scripts/install-deps.sh).
add_library(telegrobsidian_deps INTERFACE)
# SYSTEM: warnings coming from the vendored headers are not actionable here and
# would otherwise fail the -Werror builds as soon as a compiler adds a warning.
target_include_directories(telegrobsidian_deps SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(telegrobsidian_deps INTERFACE Threads::Threads)

if(TELEGROBSIDIAN_WITH_SSL)
    find_package(OpenSSL REQUIRED)
    message(STATUS "TLS enabled: OpenSSL ${OPENSSL_VERSION}")
    target_link_libraries(telegrobsidian_deps INTERFACE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(telegrobsidian_deps INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
else()
    message(WARNING "TELEGROBSIDIAN_WITH_SSL=OFF: the Notion integration will be disabled at "
                    "runtime because its API is HTTPS-only")
endif()

# -----------------------------------------------------------------------------
# Daemon
# -----------------------------------------------------------------------------
add_executable(telegrobsidian main.cpp)
target_link_libraries(telegrobsidian PRIVATE telegrobsidian_deps)
target_compile_options(telegrobsidian PRIVATE -Wall -Wextra -Wpedantic -Wshadow)

if(TELEGROBSIDIAN_WARNINGS_AS_ERRORS)
    # -Werror alone is not enough: compilers disagree about which diagnostics
    # -Wall implies (Clang enables -Wunused-const-variable for C++, GCC does
    # not), so the stricter build opts into the extra checks explicitly.
    include(CheckCXXCompilerFlag)
    foreach(extra_flag -Werror -Wunused-const-variable -Wunused-but-set-variable)
        string(MAKE_C_IDENTIFIER "TELEGROBSIDIAN_FLAG${extra_flag}" flag_variable)
        check_cxx_compiler_flag("${extra_flag}" ${flag_variable})
        if(${flag_variable})
            target_compile_options(telegrobsidian PRIVATE "${extra_flag}")
        endif()
    endforeach()
endif()

include(GNUInstallDirs)
install(TARGETS telegrobsidian RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES deploy/obsidian-sync.service DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/system"
        OPTIONAL)
install(FILES .env.example DESTINATION "${CMAKE_INSTALL_DOCDIR}" OPTIONAL)

# -----------------------------------------------------------------------------
# Tests (end-to-end against tests/mock_notion.py; no network or credentials)
# -----------------------------------------------------------------------------
if(TELEGROBSIDIAN_BUILD_TESTS)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        enable_testing()
        add_test(NAME smoke
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/smoke_test.py"
                         --binary "$<TARGET_FILE:telegrobsidian>")
        set_tests_properties(smoke PROPERTIES TIMEOUT 300)
    else()
        message(STATUS "python3 not found: the smoke test will not be registered")
    endif()
endif()
