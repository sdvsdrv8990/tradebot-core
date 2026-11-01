# Install script for directory: /home/admin/projects/tradebot-core/third_party/mbedtls/include

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "0")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/sbin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/mbedtls" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ FILES
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/aes.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/aria.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/asn1.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/asn1write.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/base64.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/bignum.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/block_cipher.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/build_info.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/camellia.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/ccm.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/chacha20.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/chachapoly.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/check_config.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/cipher.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/cmac.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/compat-2.x.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/config_adjust_legacy_crypto.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/config_adjust_legacy_from_psa.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/config_adjust_psa_from_legacy.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/config_adjust_psa_superset_legacy.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/config_adjust_ssl.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/config_adjust_x509.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/config_psa.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/constant_time.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/ctr_drbg.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/debug.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/des.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/dhm.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/ecdh.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/ecdsa.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/ecjpake.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/ecp.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/entropy.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/error.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/gcm.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/hkdf.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/hmac_drbg.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/lms.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/mbedtls_config.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/md.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/md5.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/memory_buffer_alloc.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/net_sockets.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/nist_kw.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/oid.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/pem.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/pk.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/pkcs12.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/pkcs5.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/pkcs7.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/platform.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/platform_time.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/platform_util.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/poly1305.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/private_access.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/psa_util.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/ripemd160.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/rsa.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/sha1.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/sha256.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/sha3.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/sha512.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/ssl.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/ssl_cache.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/ssl_ciphersuites.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/ssl_cookie.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/ssl_ticket.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/threading.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/timing.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/version.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/x509.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/x509_crl.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/x509_crt.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/mbedtls/x509_csr.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/psa" TYPE FILE PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ FILES
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/build_info.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_adjust_auto_enabled.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_adjust_config_dependencies.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_adjust_config_key_pair_types.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_adjust_config_synonyms.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_builtin_composites.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_builtin_key_derivation.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_builtin_primitives.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_compat.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_config.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_driver_common.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_driver_contexts_composites.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_driver_contexts_key_derivation.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_driver_contexts_primitives.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_extra.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_legacy.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_platform.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_se_driver.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_sizes.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_struct.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_types.h"
    "/home/admin/projects/tradebot-core/third_party/mbedtls/include/psa/crypto_values.h"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/home/admin/projects/tradebot-core/.build-asan/third_party/mbedtls/include/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
