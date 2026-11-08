cmake_minimum_required(VERSION 3.14)
project(CremaCorePlugin LANGUAGES CXX)
if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found")
endif()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" METADATA_JSON)
string(JSON MODULE_NAME GET ${METADATA_JSON} name)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)
logos_module(
    NAME ${MODULE_NAME}
    SOURCES
        src/crema_core_impl.h
        src/crema_core_impl.cpp
        src/crema.hpp
        src/maptiles.hpp
        src/qrcodegen.hpp
        src/qrcodegen.cpp
        src/logos_sync/catchup.hpp
        src/logos_sync/crypto.hpp
        src/logos_sync/event.hpp
        src/logos_sync/merge.hpp
        src/logos_sync/reconcile.hpp
        src/logos_sync/signing.hpp
    INCLUDE_DIRS src
    FIND_PACKAGES OpenSSL CURL ZLIB
    LINK_LIBRARIES OpenSSL::Crypto CURL::libcurl ZLIB::ZLIB
)
