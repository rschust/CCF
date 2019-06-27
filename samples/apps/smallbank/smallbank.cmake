# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the Apache 2.0 License.
# Small Bank Client executable
add_client_exe(small_bank_client
  SRCS ${CMAKE_CURRENT_LIST_DIR}/clients/small_bank_client.cpp
)
target_link_libraries(small_bank_client PRIVATE secp256k1.host)

# SmallBank application
add_enclave_lib(smallbankenc
  ${CMAKE_CURRENT_LIST_DIR}/app/oe_sign.conf
  ${CCF_DIR}/src/apps/sample_key.pem
  SRCS ${CMAKE_CURRENT_LIST_DIR}/app/smallbank.cpp
)

if(BUILD_TESTS)
  ## Small Bank end to end and performance test
  add_perf_test(
    NAME small_bank_client_test
    PYTHON_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/tests/small_bank_client.py
    CLIENT_BIN ./small_bank_client
    VERIFICATION_FILE ${CMAKE_CURRENT_LIST_DIR}/tests/verify_small_bank.json
    ADDITIONAL_ARGS
      --label Small_Bank_ClientCpp
      --max-writes-ahead 1000
      --metrics-file small_bank_metrics.json
  )

  if (${CURVE_CHOICE} STREQUAL "secp256k1_bitcoin")
    set(SMALL_BANK_SIGNED_VERIFICATION_FILE ${CMAKE_CURRENT_LIST_DIR}/tests/verify_small_bank_50k.json)
    set(SMALL_BANK_SIGNED_ITERATIONS 50000)
  else ()
    set(SMALL_BANK_SIGNED_VERIFICATION_FILE ${CMAKE_CURRENT_LIST_DIR}/tests/verify_small_bank_2k.json)
    set(SMALL_BANK_SIGNED_ITERATIONS 2000)
  endif ()

  add_perf_test(
    NAME small_bank_sigs_client_test
    PYTHON_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/tests/small_bank_client.py
    CLIENT_BIN ./small_bank_client
    VERIFICATION_FILE ${SMALL_BANK_SIGNED_VERIFICATION_FILE}
    ITERATIONS ${SMALL_BANK_SIGNED_ITERATIONS}
    ADDITIONAL_ARGS
      --label Small_Bank_Client_Sigs
      --max-writes-ahead 1000
      --sign
      --metrics-file small_bank_sigs_metrics.json
  )

  add_perf_test(
    NAME small_bank_warmup_cooldown_client_test
    PYTHON_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/tests/small_bank_client.py
    CLIENT_BIN ./small_bank_client
    VERIFICATION_FILE ${CMAKE_CURRENT_LIST_DIR}/tests/verify_small_bank.json
    ADDITIONAL_ARGS
      --label Small_Bank_WarmupCooldown
      --max-writes-ahead 1
      --warmup 1000
      --cooldown 1000
      --metrics-file small_bank_wc_metrics.json
  )

  # It is better to run performance tests with forwarding on different machines
  # (i.e. nodes and clients)
  add_perf_test(
    NAME small_bank_sigs_forwarding
    PYTHON_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/tests/small_bank_client.py
    CLIENT_BIN ./small_bank_client
    ITERATIONS ${SMALL_BANK_SIGNED_ITERATIONS}
    ADDITIONAL_ARGS
      --label Small_Bank_ClientSigs_Forwarding
      --max-writes-ahead 1000
      --metrics-file small_bank_fwd_metrics.json
      -n localhost -n localhost
      -cn localhost
      --send-tx-to followers
      --sign
  )
endif()
