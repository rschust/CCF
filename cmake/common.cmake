# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the Apache 2.0 License.

set(MSGPACK_INCLUDE_DIR ${CCF_DIR}/3rdparty/msgpack-c)
set(SNMALLOC_INSTALL_DIR /opt/snmalloc)

set(default_build_type "RelWithDebInfo")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to '${default_build_type}' as none was specified.")
    set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(Boost_ADDITIONAL_VERSIONS "1.67" "1.67.0")
find_package(Boost 1.60.0 REQUIRED)
find_package(Threads REQUIRED)

# Azure Pipelines does not support color codes
if (DEFINED ENV{BUILD_BUILDNUMBER})
  set(PYTHON python3)
else()
  set(PYTHON unbuffer python3)
endif()

if(MSVC)
  add_compile_options(/W3 /std:c++latest)
else()
  # GCC requires libatomic as well as libpthread.
  if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    set(${CMAKE_THREAD_LIBS_INIT} "$CMAKE_THREAD_LIBS_INIT} atomic")
    separate_arguments(COVERAGE_FLAGS UNIX_COMMAND "--coverage -fprofile-arcs -ftest-coverage")
    separate_arguments(COVERAGE_LINK UNIX_COMMAND "gcov")
  else()
    separate_arguments(COVERAGE_FLAGS UNIX_COMMAND "-fprofile-instr-generate -fcoverage-mapping")
    separate_arguments(COVERAGE_LINK UNIX_COMMAND "-fprofile-instr-generate -fcoverage-mapping")
  endif()
endif()

function(enable_coverage name)
  if (COVERAGE)
    target_compile_options(${name} PRIVATE ${COVERAGE_FLAGS})
    target_link_libraries(${name} PRIVATE ${COVERAGE_LINK})
  endif()
endfunction()

set(CURVE_CHOICE "secp384r1" CACHE STRING "One of secp384r1, curve25519, secp256k1_mbedtls, secp256k1_bitcoin")
if (${CURVE_CHOICE} STREQUAL "secp384r1")
  add_definitions(-DCURVE_CHOICE_SECP384R1)
elseif (${CURVE_CHOICE} STREQUAL "curve25519")
  add_definitions(-DCURVE_CHOICE_CURVE25519)
elseif (${CURVE_CHOICE} STREQUAL "secp256k1_mbedtls")
  add_definitions(-DCURVE_CHOICE_SECP256K1_MBEDTLS)
elseif (${CURVE_CHOICE} STREQUAL "secp256k1_bitcoin")
  add_definitions(-DCURVE_CHOICE_SECP256K1_BITCOIN)
else ()
  message(FATAL_ERROR "Unsupported curve choice ${CURVE_CHOICE}")
endif ()

option (COLORED_OUTPUT "Always produce ANSI-colored output (GNU/Clang only)." TRUE)

if (${COLORED_OUTPUT})
    if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
       add_compile_options (-fdiagnostics-color=always)
    elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
       add_compile_options (-fcolor-diagnostics)
    endif ()
endif ()

option(VERBOSE_LOGGING "Enable verbose logging" OFF)
set(TEST_HOST_LOGGING_LEVEL "info")
if(VERBOSE_LOGGING)
  add_definitions(-DVERBOSE_LOGGING)
  set(TEST_HOST_LOGGING_LEVEL "debug")
endif()

option(NO_STRICT_TLS_CIPHERSUITES "Disable strict list of valid TLS ciphersuites" OFF)
if(NO_STRICT_TLS_CIPHERSUITES)
  add_definitions(-DNO_STRICT_TLS_CIPHERSUITES)
endif()

option(USE_NULL_ENCRYPTOR "Turn off encryption of ledger updates - debug only" OFF)
if (USE_NULL_ENCRYPTOR)
  add_definitions(-DUSE_NULL_ENCRYPTOR)
endif()

option(SAN "Enable Address and Undefined Behavior Sanitizers" OFF)
option(DISABLE_QUOTE_VERIFICATION "Disable quote verification" OFF)
option(BUILD_END_TO_END_TESTS "Build end to end tests" ON)
option(COVERAGE "Enable coverage mapping" OFF)

option(PBFT "Enable PBFT" OFF)
if (PBFT)
  add_definitions(-DPBFT)
  set(PBFT_BUILD_ENCLAVE TRUE)
  set(PBFT_BUILD_HOST TRUE)
  set(PBFT_USE_LIBC TRUE)
endif()

option(DEBUG_CONFIG "Enable non-production options options to aid debugging" OFF)
if(DEBUG_CONFIG)
  add_definitions(-DDEBUG_CONFIG)
endif()

option(USE_NLJSON_KV_SERIALISER "Use nlohmann JSON as the KV serialiser" OFF)
if (USE_NLJSON_KV_SERIALISER)
  add_definitions(-DUSE_NLJSON_KV_SERIALISER)
endif()

option(CCF_HOST_USE_SNMALLOC "Use snmalloc as the default allocator for non-encrypted memory allocations" OFF)
if (CCF_HOST_USE_SNMALLOC)
  add_definitions(-DCCF_HOST_USE_SNMALLOC)
  set(SNMALLOC_SRC_FILES_HOST
    ${SNMALLOC_INSTALL_DIR}/include/override/malloc.cc
    ${SNMALLOC_INSTALL_DIR}/include/override/new.cc)
  set(SNMALLOC_SRC_FILES_ENCLAVE ${CCF_DIR}/src/enclave/shared_allocator.cpp)
else()
  set(SNMALLOC_SRC_FILES_HOST )
  set(SNMALLOC_SRC_FILES_ENCLAVE )
endif()

enable_language(ASM)

include_directories(
  ${CCF_DIR}/src
)

include_directories(
  SYSTEM
  ${CCF_DIR}/3rdparty
  ${MSGPACK_INCLUDE_DIR}
  ${SNMALLOC_INSTALL_DIR}/include
)

set(TARGET "all" CACHE STRING "One of sgx, virtual, all")

set(OE_PREFIX "/opt/openenclave" CACHE PATH "Path to Open Enclave install")
message(STATUS "Open Enclave prefix set to ${OE_PREFIX}")

set(CLIENT_MBEDTLS_PREFIX "/usr/local" CACHE PATH "Prefix to the mbedtls install the client should use")
message(STATUS "Client mbedtls prefix set to ${CLIENT_MBEDTLS_PREFIX}")

set(CLIENT_MBEDTLS_INCLUDE_DIR "${CLIENT_MBEDTLS_PREFIX}/include")
set(CLIENT_MBEDTLS_LIB_DIR "${CLIENT_MBEDTLS_PREFIX}/lib")

set(OE_INCLUDE_DIR "${OE_PREFIX}/include")
set(OE_LIB_DIR "${OE_PREFIX}/lib/openenclave")
set(OE_BIN_DIR "${OE_PREFIX}/bin")

set(OE_TP_INCLUDE_DIR   "${OE_INCLUDE_DIR}/openenclave/3rdparty")
set(OE_LIBC_INCLUDE_DIR   "${OE_INCLUDE_DIR}/openenclave/3rdparty/libc")
set(OE_LIBCXX_INCLUDE_DIR "${OE_INCLUDE_DIR}/openenclave/3rdparty/libcxx")

set(OESIGN "${OE_BIN_DIR}/oesign")
set(OEGEN "${OE_BIN_DIR}/oeedger8r")


add_custom_command(
    COMMAND ${OEGEN} ${CCF_DIR}/src/edl/ccf.edl --trusted --trusted-dir ${CMAKE_CURRENT_BINARY_DIR} --untrusted --untrusted-dir ${CMAKE_CURRENT_BINARY_DIR}
    COMMAND mv ${CMAKE_CURRENT_BINARY_DIR}/ccf_t.c ${CMAKE_CURRENT_BINARY_DIR}/ccf_t.cpp
    COMMAND mv ${CMAKE_CURRENT_BINARY_DIR}/ccf_u.c ${CMAKE_CURRENT_BINARY_DIR}/ccf_u.cpp
)

configure_file(${CCF_DIR}/tests/tests.sh ${CMAKE_CURRENT_BINARY_DIR}/tests.sh COPYONLY)

if(NOT ${TARGET} STREQUAL "virtual")
# If OE was built with LINK_SGX=1, then we also need to link SGX
execute_process(COMMAND "ldd" ${OESIGN}
                COMMAND "grep" "-c" "sgx"
                OUTPUT_QUIET
               RESULT_VARIABLE OE_NO_SGX)

if(NOT OE_NO_SGX)
  message(STATUS "Linking SGX")
  set(SGX_LIBS
    sgx_enclave_common
    sgx_dcap_ql
    sgx_urts
  )

  if (NOT DISABLE_QUOTE_VERIFICATION)
    set(QUOTES_ENABLED ON)
    else()
      set(TEST_IGNORE_QUOTE "--ignore-quote")
  endif()
  else()
    set(TEST_IGNORE_QUOTE "--ignore-quote")
endif()
else()
  set(TEST_ENCLAVE_TYPE
    -e virtual)
endif()

# Test-only option to enable extensive tests
option(EXTENSIVE_TESTS "Enable extensive tests" OFF)
if (EXTENSIVE_TESTS)
  set(RECOVERY_ARGS
    --recovery 5
    --msgs-per-recovery 10)
else()
  set(RECOVERY_ARGS
    --recovery 2
    --msgs-per-recovery 5)
endif()

# Lua module
set(LUA_DIR
  ${CCF_DIR}/3rdparty/lua)
set(LUA_SOURCES
  ${LUA_DIR}/lapi.c
  ${LUA_DIR}/lauxlib.c
  ${LUA_DIR}/lbaselib.c
  ${LUA_DIR}/lcode.c
  ${LUA_DIR}/lcorolib.c
  ${LUA_DIR}/lctype.c
  ${LUA_DIR}/ldebug.c
  ${LUA_DIR}/ldo.c
  ${LUA_DIR}/ldump.c
  ${LUA_DIR}/lfunc.c
  ${LUA_DIR}/lgc.c
  ${LUA_DIR}/llex.c
  ${LUA_DIR}/lmathlib.c
  ${LUA_DIR}/lmem.c
  ${LUA_DIR}/lobject.c
  ${LUA_DIR}/lopcodes.c
  ${LUA_DIR}/lparser.c
  ${LUA_DIR}/lstate.c
  ${LUA_DIR}/lstring.c
  ${LUA_DIR}/lstrlib.c
  ${LUA_DIR}/ltable.c
  ${LUA_DIR}/ltablib.c
  ${LUA_DIR}/ltm.c
  ${LUA_DIR}/lundump.c
  ${LUA_DIR}/lutf8lib.c
  ${LUA_DIR}/lvm.c
  ${LUA_DIR}/lzio.c)

set(OE_MBEDTLS_LIBRARIES
  "${OE_LIB_DIR}/enclave/libmbedtls.a"
  "${OE_LIB_DIR}/enclave/libmbedx509.a"
  "${OE_LIB_DIR}/enclave/libmbedcrypto.a"
)

find_library(CRYPTO_LIBRARY crypto)

set(OE_ENCLAVE_LIBRARY "${OE_LIB_DIR}/enclave/liboeenclave.a")
set(OE_ENCLAVE_CORE "${OE_LIB_DIR}/enclave/liboecore.a")
set(OE_ENCLAVE_LIBC "${OE_LIB_DIR}/enclave/liboelibc.a")
set(OE_ENCLAVE_LIBCXX "${OE_LIB_DIR}/enclave/liboelibcxx.a")
set(OE_HOST_LIBRARY "${OE_LIB_DIR}/host/liboehost.a")

set(CLIENT_MBEDTLS_LIBRARIES
  "${CLIENT_MBEDTLS_LIB_DIR}/libmbedtls.a"
  "${CLIENT_MBEDTLS_LIB_DIR}/libmbedx509.a"
  "${CLIENT_MBEDTLS_LIB_DIR}/libmbedcrypto.a")

# The OE libraries must be listed in a specific order. Issue #887 on github
set(ENCLAVE_LIBS
  lua.enclave
  ${OE_ENCLAVE_LIBRARY}
  ${OE_MBEDTLS_LIBRARIES}
  ${OE_ENCLAVE_LIBCXX}
  ${OE_ENCLAVE_LIBC}
  ${OE_ENCLAVE_CORE}
  ccfcrypto.enclave
)

set(ENCLAVE_FILES
  ${CCF_DIR}/src/enclave/main.cpp
  ${SNMALLOC_SRC_FILES_ENCLAVE}
)

function(enable_quote_code name)
  if (QUOTES_ENABLED)
    target_compile_definitions(${name} PRIVATE -DGET_QUOTE)
  endif()
endfunction()

function(add_enclave_library_c name files)
  add_library(${name} STATIC
    ${files})
  target_compile_options(${name} PRIVATE
    -nostdinc
    -U__linux__)
  target_include_directories(${name} SYSTEM PRIVATE
    ${OE_LIBC_INCLUDE_DIR}
    )
  set_property(TARGET ${name} PROPERTY POSITION_INDEPENDENT_CODE ON)
  enable_quote_code(${name})
endfunction()

function(use_client_mbedtls name)
  target_include_directories(${name} PRIVATE ${CLIENT_MBEDTLS_INCLUDE_DIR})
  target_link_libraries(${name} PRIVATE ${CLIENT_MBEDTLS_LIBRARIES})
endfunction()

function(use_oe_mbedtls name)
  target_include_directories(${name} PRIVATE ${OE_TP_INCLUDE_DIR})
  target_link_libraries(${name} PRIVATE ${OE_MBEDTLS_LIBRARIES})
endfunction()

function(add_san name)
  if(SAN)
    target_compile_options(${name} PRIVATE
      -fsanitize=undefined,address -fno-omit-frame-pointer -fno-sanitize-recover=all
      -fno-sanitize=function -fsanitize-blacklist=${CCF_DIR}/src/ubsan.blacklist
    )
    target_link_libraries(${name} PRIVATE
      -fsanitize=undefined,address -fno-omit-frame-pointer -fno-sanitize-recover=all
      -fno-sanitize=function -fsanitize-blacklist=${CCF_DIR}/src/ubsan.blacklist
    )
  endif()
endfunction()

function(sign_app_library name app_oe_conf_path enclave_sign_key_path)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lib${name}.so.signed
    COMMAND ${OESIGN} sign
      -e ${CMAKE_CURRENT_BINARY_DIR}/lib${name}.so
      -c ${app_oe_conf_path}
      -k ${enclave_sign_key_path}
    DEPENDS ${name}
      ${app_oe_conf_path}
      ${enclave_sign_key_path}
  )

  add_custom_target(${name}_signed ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/lib${name}.so.signed
  )
endfunction()

include(${CCF_DIR}/cmake/crypto.cmake)
include(${CCF_DIR}/cmake/secp256k1.cmake)

## Build PBFT if used as consensus
if (PBFT)
  message(STATUS "Using PBFT as consensus")
  include(${CCF_DIR}/ePBFT/cmake/pbft.cmake)

  target_include_directories(libbyz.host PRIVATE
    ${CCF_DIR}/src/ds
    ${EVERCRYPT_INC}
  )

  target_include_directories(libbyz.enclave PRIVATE
    ${CCF_DIR}/src/ds
    ${OE_INCLUDE_DIR}
    ${OE_LIBCXX_INCLUDE_DIR}
    ${OE_LIBC_INCLUDE_DIR}
    ${OE_TP_INCLUDE_DIR}
    ${PARSED_ARGS_INCLUDE_DIRS}
    ${EVERCRYPT_INC}
  )
endif()

function(create_patched_enclave_lib name app_oe_conf_path enclave_sign_key_path)
  set(patched_name ${name}.patched)
  add_custom_target(${patched_name}
      COMMAND cp ${CMAKE_CURRENT_BINARY_DIR}/lib${name}.so ${CMAKE_CURRENT_BINARY_DIR}/lib${patched_name}.so
      COMMAND PYTHONPATH=${CCF_DIR}/tests:$ENV{PYTHONPATH} python3 patch_binary.py -p ${CMAKE_CURRENT_BINARY_DIR}/lib${patched_name}.so
      WORKING_DIRECTORY ${CCF_DIR}/tests
      DEPENDS ${name}
  )
  sign_app_library(${patched_name} ${app_oe_conf_path} ${enclave_sign_key_path})
endfunction()

## Enclave library wrapper
function(add_enclave_lib name app_oe_conf_path enclave_sign_key_path)

  cmake_parse_arguments(PARSE_ARGV 1 PARSED_ARGS
    ""
    ""
    "SRCS;INCLUDE_DIRS;LINK_LIBS"
  )

  if(NOT ${TARGET} STREQUAL "virtual")
  add_library(${name} SHARED
    ${ENCLAVE_FILES}
    ${PARSED_ARGS_SRCS}
    ${CMAKE_CURRENT_BINARY_DIR}/ccf_t.cpp
  )

  target_compile_definitions(${name} PRIVATE
    INSIDE_ENCLAVE
    _LIBCPP_HAS_THREAD_API_PTHREAD
  )
  # Not setting -nostdinc in order to pick up compiler specific xmmintrin.h.
  target_compile_options(${name} PRIVATE
    -nostdinc++
    -U__linux__
    -mcx16
  )
  target_include_directories(${name} SYSTEM PRIVATE
    ${OE_INCLUDE_DIR}
    ${OE_LIBCXX_INCLUDE_DIR}
    ${OE_LIBC_INCLUDE_DIR}
    ${OE_TP_INCLUDE_DIR}
    ${PARSED_ARGS_INCLUDE_DIRS}
      ${EVERCRYPT_INC}
    ${CMAKE_CURRENT_BINARY_DIR}
  )
  if (PBFT)
    target_include_directories(${name} SYSTEM PRIVATE
        ${CCF_DIR}/ePBFT/src/pbft/
    )
  endif()
    if (PBFT)
  target_link_libraries(${name} PRIVATE
        -Wl,--allow-multiple-definition #TODO(#important): This is unfortunate
        libbyz.enclave
      )
    endif()
    target_link_libraries(${name} PRIVATE
    -nostdlib -nodefaultlibs -nostartfiles
    -Wl,--no-undefined
    -Wl,-Bstatic,-Bsymbolic,--export-dynamic,-pie
    -lgcc
    ${PARSED_ARGS_LINK_LIBS}
    ccfcrypto.enclave
      evercrypt.enclave
      ${ENCLAVE_LIBS}
    secp256k1.enclave
  )
  set_property(TARGET ${name} PROPERTY POSITION_INDEPENDENT_CODE ON)
  sign_app_library(${name} ${app_oe_conf_path} ${enclave_sign_key_path})
  enable_quote_code(${name})
    if (${name} STREQUAL "loggingenc")
        create_patched_enclave_lib(${name} ${app_oe_conf_path} ${enclave_sign_key_path})
    endif()
  endif()

  if(${TARGET} STREQUAL "virtual" OR ${TARGET} STREQUAL "all")
  ## Build a virtual enclave, loaded as a shared library without OE
  set(virt_name ${name}.virtual)
  add_library(${virt_name} SHARED
    ${ENCLAVE_FILES}
    ${PARSED_ARGS_SRCS}
    ${CCF_DIR}/src/enclave/ccf_v_ocall.cpp
  )
  add_san(${virt_name})
  target_compile_definitions(${virt_name} PRIVATE
    INSIDE_ENCLAVE
    VIRTUAL_ENCLAVE
    -DIS_ADDRESS_SPACE_CONSTRAINED
  )
    target_compile_options(${virt_name} PRIVATE
      -stdlib=libc++)
  target_include_directories(${virt_name} SYSTEM PRIVATE
    ${PARSED_ARGS_INCLUDE_DIRS}
    ${CCFCRYPTO_INC}
      ${EVERCRYPT_INC}
    ${OE_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
  )
  if (PBFT)
    target_include_directories(${virt_name} SYSTEM PRIVATE
        ${CCF_DIR}/ePBFT/src/pbft/
    )
  endif()
    if (PBFT)
  target_link_libraries(${virt_name} PRIVATE
        -Wl,--allow-multiple-definition #TODO(#important): This is unfortunate
        libbyz.host
      )
    endif()
    target_link_libraries(${virt_name} PRIVATE
    ${PARSED_ARGS_LINK_LIBS}
    -stdlib=libc++
    -lc++
    -lc++abi
    ccfcrypto.host
      evercrypt.host
    lua.host
    ${CMAKE_THREAD_LIBS_INIT}
    secp256k1.host
  )
    enable_coverage(${virt_name})
  use_client_mbedtls(${virt_name})
  set_property(TARGET ${virt_name} PROPERTY POSITION_INDEPENDENT_CODE ON)
  endif()
endfunction()

## Unit test wrapper
function(add_unit_test name)
  add_executable(${name}
    ${ARGN})
  target_include_directories(${name} PRIVATE
    src
    ${CCFCRYPTO_INC})
  target_compile_options(${name} PRIVATE -fdiagnostics-color=always -mcx16)
  enable_coverage(${name})
  target_link_libraries(${name} PRIVATE ${CMAKE_THREAD_LIBS_INIT} ccfcrypto.host)

  use_client_mbedtls(${name})
  add_san(${name})

  add_test(
    NAME ${name}
    COMMAND ${CCF_DIR}/tests/unit_test_wrapper.sh ${name}
  )
  set_property(
    TEST ${name}
    APPEND
    PROPERTY
      LABELS unit_test
  )
endfunction()

# GenesisGenerator Executable
add_executable(genesisgenerator ${CCF_DIR}/src/genesisgen/main.cpp)
use_client_mbedtls(genesisgenerator)
target_link_libraries(genesisgenerator PRIVATE
  ${CMAKE_THREAD_LIBS_INIT}
  lua.host
  secp256k1.host
)

if(NOT ${TARGET} STREQUAL "virtual")
# Host Executable
add_executable(cchost
  ${SNMALLOC_SRC_FILES_HOST}
  ${CCF_DIR}/src/host/main.cpp
  ${CCF_DIR}/src/host/ocalls_snmalloc.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/ccf_u.cpp)
use_client_mbedtls(cchost)
target_include_directories(cchost PRIVATE
  ${OE_INCLUDE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_compile_options(cchost PRIVATE
  -mcx16
  -DIS_ADDRESS_SPACE_CONSTRAINED
  -DSNMALLOC_EXPOSE_PAGEMAP
  -DSNMALLOC_EXPOSE_RESERVE
)
add_san(cchost)

target_link_libraries(cchost PRIVATE
  uv
  ${OE_HOST_LIBRARY}
  ${SGX_LIBS}
  ${CRYPTO_LIBRARY}
  ${CMAKE_DL_LIBS}
  ${CMAKE_THREAD_LIBS_INIT}
  ccfcrypto.host
    evercrypt.host
)
enable_quote_code(cchost)
endif()

if(${TARGET} STREQUAL "virtual" OR ${TARGET} STREQUAL "all")
# Virtual Host Executable
add_executable(cchost.virtual
  ${SNMALLOC_SRC_FILES_HOST}
  ${CCF_DIR}/src/host/main.cpp)
use_client_mbedtls(cchost.virtual)
target_compile_definitions(cchost.virtual PRIVATE
  -DVIRTUAL_ENCLAVE
  -DIS_ADDRESS_SPACE_CONSTRAINED
  -DSNMALLOC_EXPOSE_PAGEMAP
  -DSNMALLOC_EXPOSE_RESERVE)
target_compile_options(cchost.virtual PRIVATE
  -stdlib=libc++
  -mcx16)
target_include_directories(cchost.virtual PRIVATE
  ${OE_INCLUDE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)
add_san(cchost.virtual)
  enable_coverage(cchost.virtual)
target_link_libraries(cchost.virtual PRIVATE
  uv
  ${CRYPTO_LIBRARY}
  ${CMAKE_DL_LIBS}
  ${CMAKE_THREAD_LIBS_INIT}
  -lc++
  -lc++abi
  -stdlib=libc++
  -Wl,--export-dynamic
  ccfcrypto.host
    evercrypt.host
)
endif()

# Client executable
add_executable(client ${CCF_DIR}/src/clients/client.cpp)
use_client_mbedtls(client)
target_link_libraries(client PRIVATE
  ${CMAKE_THREAD_LIBS_INIT}
)

# Lua for host and enclave
add_enclave_library_c(lua.enclave "${LUA_SOURCES}")
target_compile_definitions(lua.enclave PRIVATE NO_IO)
add_library(lua.host STATIC ${LUA_SOURCES})
target_compile_definitions(lua.host PRIVATE NO_IO)
set_property(TARGET lua.host PROPERTY POSITION_INDEPENDENT_CODE ON)

# Common test args for Python scripts starting up CCF networks
set(CCF_NETWORK_TEST_ARGS
  ${TEST_IGNORE_QUOTE}
  ${TEST_ENCLAVE_TYPE}
  -l ${TEST_HOST_LOGGING_LEVEL}
  -g ${CCF_DIR}/src/runtime_config/gov.lua
)

# Lua generic app
add_enclave_lib(luagenericenc ${CCF_DIR}/src/apps/luageneric/oe_sign.conf ${CCF_DIR}/src/apps/sample_key.pem SRCS ${CCF_DIR}/src/apps/luageneric/luageneric.cpp)

# Samples

# Common options
set(TEST_ITERATIONS 200000)

## Helper for building clients inheriting from perf_client
function(add_client_exe name)

  cmake_parse_arguments(PARSE_ARGV 1 PARSED_ARGS
    ""
    ""
    "SRCS;INCLUDE_DIRS;LINK_LIBS"
  )

  add_executable(${name}
    ${PARSED_ARGS_SRCS}
  )

  target_link_libraries(${name} PRIVATE
    ${CMAKE_THREAD_LIBS_INIT}
  )

  target_include_directories(${name} PRIVATE
    ${CCF_DIR}/samples/perf_client
    ${PARSED_ARGS_INCLUDE_DIRS}
  )

  use_client_mbedtls(${name})

endfunction()

## Helper for building end-to-end function tests using the python infrastructure
function(add_e2e_test)
  cmake_parse_arguments(PARSE_ARGV 0 PARSED_ARGS
    ""
    "NAME;PYTHON_SCRIPT;"
    "ADDITIONAL_ARGS"
  )

  if (BUILD_END_TO_END_TESTS)
    add_test(
      NAME ${PARSED_ARGS_NAME}
      COMMAND ${PYTHON} ${PARSED_ARGS_PYTHON_SCRIPT}
        -b .
        --label ${PARSED_ARGS_NAME}
        ${CCF_NETWORK_TEST_ARGS}
        ${PARSED_ARGS_ADDITIONAL_ARGS}
    )

    ## Make python test client framework importable
    set_property(
      TEST ${PARSED_ARGS_NAME}
      APPEND
      PROPERTY
        ENVIRONMENT "PYTHONPATH=${CCF_DIR}/tests:$ENV{PYTHONPATH}"
    )
    set_property(
      TEST ${PARSED_ARGS_NAME}
      APPEND
      PROPERTY
        LABELS end_to_end
    )
  endif()
endfunction()

## Helper for building end-to-end perf tests using the python infrastucture
function(add_perf_test)

  cmake_parse_arguments(PARSE_ARGV 0 PARSED_ARGS
    ""
    "NAME;PYTHON_SCRIPT;CLIENT_BIN;ITERATIONS;VERIFICATION_FILE"
    "ADDITIONAL_ARGS"
  )

  ## Use default value if undefined
  if(NOT PARSED_ARGS_ITERATIONS)
    set(PARSED_ARGS_ITERATIONS ${TEST_ITERATIONS})
  endif()

  if(PARSED_ARGS_VERIFICATION_FILE)
    set(VERIFICATION_ARG "--verify ${PARSED_ARGS_VERIFICATION_FILE}")
  else()
    unset(VERIFICATION_ARG)
  endif()

  add_test(
    NAME ${PARSED_ARGS_NAME}
    COMMAND ${PYTHON} ${PARSED_ARGS_PYTHON_SCRIPT}
      -b .
      -c ${PARSED_ARGS_CLIENT_BIN}
      -i ${PARSED_ARGS_ITERATIONS}
      ${CCF_NETWORK_TEST_ARGS}
      ${PARSED_ARGS_ADDITIONAL_ARGS}
      --write-tx-times
      ${VERIFICATION_ARG}
  )

  ## Make python test client framework importable
  set_property(
    TEST ${PARSED_ARGS_NAME}
    APPEND
    PROPERTY
      ENVIRONMENT "PYTHONPATH=${CCF_DIR}/tests:$ENV{PYTHONPATH}"
  )
  set_property(
    TEST ${PARSED_ARGS_NAME}
    APPEND
    PROPERTY
      LABELS perf
  )
endfunction()
