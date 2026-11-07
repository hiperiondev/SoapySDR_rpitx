# SoapySDR_rpitx - SoapySDR driver for librpitx (Raspberry Pi GPIO transmitter), optionally a transceiver with an RTL-SDR receiver
cmake_minimum_required(VERSION 3.10)
project(SoapyRPITX VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(RPITX_BUILD_DRIVER "Build the SoapySDR module" ON)
option(RPITX_BUILD_RESET_TOOL "Build the rpitx_reset recovery tool" ON)
option(RPITX_INSTALL_TOOLS "Install the Python tools (tone test, HPSDR bridge, amplitude calibration, autotune)" ON)
option(RPITX_WERROR "Treat compiler warnings as errors" OFF)
# The tests run on the build machine, so they are off by default when cross-compiling.
if(CMAKE_CROSSCOMPILING)
    option(BUILD_TESTING "Build the host-side unit tests" OFF)
else()
    option(BUILD_TESTING "Build the host-side unit tests" ON)
endif()

find_package(Threads REQUIRED)
include(GNUInstallDirs)

set(RPITX_WARNINGS -Wall -Wextra -Wshadow)
if(RPITX_WERROR)
    list(APPEND RPITX_WARNINGS -Werror)
endif()

# Hardware access shared by the driver, the reset tool and the tests (mailbox.cpp and rpi.cpp are replaced
# by tests/mock_hw.cpp in the tests).
set(RPITX_HW_SOURCES
    src/SoapyRPITX_Session.cpp
    librpitx/src/gpio.cpp
    librpitx/src/util.cpp)

if(RPITX_BUILD_DRIVER)
    # SoapySDR 0.7.x picks LIB_SUFFIX=64 from the build host's /etc/redhat-release (e.g. a Fedora PC) and
    # then looks for the library in <sysroot>/usr/lib64; a Debian sysroot keeps it in usr/lib/<multiarch>.
    if(CMAKE_CROSSCOMPILING AND NOT DEFINED LIB_SUFFIX)
        set(LIB_SUFFIX "" CACHE STRING "lib directory suffix")
    endif()

    find_package(SoapySDR "0.7" CONFIG QUIET)

    if(NOT SoapySDR_FOUND)
        # No SoapySDRConfig.cmake (for example a cross-compilation sysroot without /usr/share/cmake):
        # locate the headers and the library directly and install the module by hand.
        find_path(SOAPYSDR_INCLUDE_DIR SoapySDR/Device.hpp)
        find_library(SOAPYSDR_LIBRARY NAMES SoapySDR)
        if(NOT SOAPYSDR_INCLUDE_DIR OR NOT SOAPYSDR_LIBRARY)
            message(FATAL_ERROR "SoapySDR not found: install libsoapysdr-dev (in the sysroot when cross-compiling), "
                                "or set SoapySDR_DIR / CMAKE_PREFIX_PATH")
        endif()

        file(STRINGS "${SOAPYSDR_INCLUDE_DIR}/SoapySDR/Version.h" _abi REGEX "^#define SOAPY_SDR_ABI_VERSION ")
        string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" SOAPY_SDR_ABI_VERSION "${_abi}")
        if(NOT SOAPY_SDR_ABI_VERSION)
            message(FATAL_ERROR "Cannot read SOAPY_SDR_ABI_VERSION from ${SOAPYSDR_INCLUDE_DIR}/SoapySDR/Version.h")
        endif()
        message(STATUS "SoapySDR (no CMake config): ${SOAPYSDR_LIBRARY}, ABI ${SOAPY_SDR_ABI_VERSION}")

        add_library(SoapySDR UNKNOWN IMPORTED)
        set_target_properties(SoapySDR PROPERTIES
            IMPORTED_LOCATION "${SOAPYSDR_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${SOAPYSDR_INCLUDE_DIR}")

        function(SOAPY_SDR_MODULE_UTIL)
            cmake_parse_arguments(MODULE "" "TARGET" "SOURCES;LIBRARIES" ${ARGN})
            add_library(${MODULE_TARGET} MODULE ${MODULE_SOURCES})
            target_link_libraries(${MODULE_TARGET} PRIVATE SoapySDR ${MODULE_LIBRARIES})
            set_target_properties(${MODULE_TARGET} PROPERTIES DEBUG_POSTFIX "")
            install(TARGETS ${MODULE_TARGET} DESTINATION ${CMAKE_INSTALL_LIBDIR}/SoapySDR/modules${SOAPY_SDR_ABI_VERSION}/)
        endfunction()
    endif()

    set(RPITX_SOURCES
        src/SoapyRPITX_IQ_dmaSync.cpp
        src/SoapyRPITX_Ptt.cpp
        src/SoapyRPITX_Receiver.cpp
        src/SoapyRPITX_Registration.cpp
        src/SoapyRPITX_Settings.cpp
        src/SoapyRPITX_Streaming.cpp
        src/SoapySDR_Sensors.cpp
        ${RPITX_HW_SOURCES}
        librpitx/src/dma.cpp
        librpitx/src/dsp.cpp
        librpitx/src/mailbox.cpp
        librpitx/src/rpi.cpp)

    # Installs into <libdir>/SoapySDR/modules<ABI>/ so that SoapySDR finds the module without manual copying.
    SOAPY_SDR_MODULE_UTIL(
        TARGET rpitxSupport
        SOURCES ${RPITX_SOURCES}
        LIBRARIES Threads::Threads)

    target_include_directories(rpitxSupport PRIVATE src librpitx/src)
    target_compile_definitions(rpitxSupport PRIVATE SOAPY_RPITX_VERSION="${PROJECT_VERSION}")
    target_compile_options(rpitxSupport PRIVATE ${RPITX_WARNINGS})
    # The module registers itself through static constructors: nothing has to be exported, and hiding the
    # vendored librpitx avoids clashes with another copy of it in the host process.
    set_target_properties(rpitxSupport PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    # Set as a link flag rather than with target_link_libraries(): SOAPY_SDR_MODULE_UTIL uses the plain signature
    # in SoapySDR 0.7 and the keyword signature in 0.8, and the two cannot be mixed on one target.
    set_property(TARGET rpitxSupport APPEND_STRING PROPERTY LINK_FLAGS " -Wl,--no-undefined")
endif()

if(RPITX_BUILD_RESET_TOOL)
    add_executable(rpitx_reset tools/rpitx_reset.cpp ${RPITX_HW_SOURCES} librpitx/src/mailbox.cpp librpitx/src/rpi.cpp)
    target_include_directories(rpitx_reset PRIVATE src librpitx/src)
    target_compile_options(rpitx_reset PRIVATE ${RPITX_WARNINGS})
    install(TARGETS rpitx_reset DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(RPITX_INSTALL_TOOLS)
    install(PROGRAMS tools/rpitx_tone_test.py tools/hpsdr_rpitx_bridge.py tools/rpitx_amp_calibrate.py tools/rpitx_autotune.py
            DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(BUILD_TESTING)
    enable_testing()
    add_executable(rpitx_unit_tests tests/test_buffers.cpp librpitx/src/dsp.cpp)
    target_include_directories(rpitx_unit_tests PRIVATE src librpitx/src)
    target_compile_options(rpitx_unit_tests PRIVATE ${RPITX_WARNINGS})
    target_link_libraries(rpitx_unit_tests PRIVATE Threads::Threads)
    add_test(NAME rpitx_unit_tests COMMAND rpitx_unit_tests)

    # Device-tree queries of librpitx against trees generated by the test.
    add_executable(rpitx_devicetree_tests tests/test_devicetree.cpp librpitx/src/rpi.cpp)
    target_include_directories(rpitx_devicetree_tests PRIVATE librpitx/src)
    target_compile_definitions(rpitx_devicetree_tests PRIVATE RPITX_DT_ROOT="${CMAKE_CURRENT_BINARY_DIR}/test_root/dt_generated")
    target_compile_options(rpitx_devicetree_tests PRIVATE ${RPITX_WARNINGS})
    add_test(NAME rpitx_devicetree_tests COMMAND rpitx_devicetree_tests)

    if(RPITX_BUILD_DRIVER)
        # Simulated board: a device tree, /dev/mem and /dev/vcio stand-ins and the lock directory live in the
        # build directory; tests/mock_hw.cpp replaces the mailbox, /dev/mem mappings and device-tree queries.
        set(RPITX_TEST_ROOT "${CMAKE_CURRENT_BINARY_DIR}/test_root")
        file(MAKE_DIRECTORY "${RPITX_TEST_ROOT}/dt")
        file(WRITE "${RPITX_TEST_ROOT}/dt/model" "Raspberry Pi 3 Model B Rev 1.2")
        file(WRITE "${RPITX_TEST_ROOT}/dt/serial-number" "00000000cafe0001")
        file(WRITE "${RPITX_TEST_ROOT}/mem" "")
        file(WRITE "${RPITX_TEST_ROOT}/vcio" "")
        set(RPITX_TEST_DEFINITIONS
            RPITX_DT_ROOT="${RPITX_TEST_ROOT}/dt"
            RPITX_LOCK_DIR="${RPITX_TEST_ROOT}/run"
            RPITX_DEV_MEM="${RPITX_TEST_ROOT}/mem"
            RPITX_DEV_VCIO="${RPITX_TEST_ROOT}/vcio")
        set(RPITX_TEST_HW_SOURCES
            tests/mock_hw.cpp
            src/SoapyRPITX_IQ_dmaSync.cpp
            ${RPITX_HW_SOURCES}
            librpitx/src/dma.cpp
            librpitx/src/dsp.cpp)

        add_executable(rpitx_transmitter_tests tests/test_transmitter.cpp ${RPITX_TEST_HW_SOURCES})
        target_include_directories(rpitx_transmitter_tests PRIVATE src librpitx/src tests)
        target_compile_definitions(rpitx_transmitter_tests PRIVATE ${RPITX_TEST_DEFINITIONS})
        target_compile_options(rpitx_transmitter_tests PRIVATE ${RPITX_WARNINGS})
        target_link_libraries(rpitx_transmitter_tests PRIVATE SoapySDR Threads::Threads)
        add_test(NAME rpitx_transmitter_tests COMMAND rpitx_transmitter_tests)

        add_executable(rpitx_driver_tests
            tests/test_driver.cpp
            ${RPITX_TEST_HW_SOURCES}
            src/SoapyRPITX_Ptt.cpp
            src/SoapyRPITX_Receiver.cpp
            src/SoapyRPITX_Registration.cpp
            src/SoapyRPITX_Settings.cpp
            src/SoapyRPITX_Streaming.cpp
            src/SoapySDR_Sensors.cpp)
        target_include_directories(rpitx_driver_tests PRIVATE src librpitx/src tests)
        target_compile_definitions(rpitx_driver_tests PRIVATE ${RPITX_TEST_DEFINITIONS} SOAPY_RPITX_VERSION="${PROJECT_VERSION}")
        target_compile_options(rpitx_driver_tests PRIVATE ${RPITX_WARNINGS})
        target_link_libraries(rpitx_driver_tests PRIVATE SoapySDR Threads::Threads)
        add_test(NAME rpitx_driver_tests COMMAND rpitx_driver_tests)

        # The tests share the simulated lock file.
        set_tests_properties(rpitx_transmitter_tests rpitx_driver_tests PROPERTIES RESOURCE_LOCK rpitx_simulated_board)
    endif()
endif()
