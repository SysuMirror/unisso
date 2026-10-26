cmake_minimum_required(VERSION 3.15)
project(unisso_v2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

find_library(HIREDIS_LIB hiredis REQUIRED)
find_library(MYSQL_LIB mysqlclient REQUIRED)
find_library(CRYPT_LIB crypt REQUIRED)
find_library(CPPHTTPLIB_LIB cpp-httplib REQUIRED)

# Vendor: framework core (MIT, see LICENSE). Unused demo modules are excluded.
set(FRAMEWORK_SRC
    src/Server.cpp
    src/DBConnector.cpp
    src/RDConnector.cpp
    src/JsonValue.cpp
    src/Log.cpp
    src/Threadpool.cpp
    src/Coroutine.cpp
    src/Crash.cpp
    src/Channel.cpp
    src/DuringTask.cpp
)

file(GLOB APP_SRC CONFIGURE_DEPENDS src/app/*.cpp)

add_executable(unisso_v2 main.cpp ${FRAMEWORK_SRC} ${APP_SRC})

target_include_directories(unisso_v2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    /usr/include/mysql
)

target_compile_options(unisso_v2 PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    $<$<CONFIG:Release>:-O2>
)

target_link_libraries(unisso_v2 PRIVATE
    ${MYSQL_LIB}
    ${HIREDIS_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
    ${ZLIB_LIBRARIES}
    CURL::libcurl
    ${CRYPT_LIB}
    ${CPPHTTPLIB_LIB}
    Threads::Threads
    dl
    rt
)

install(TARGETS unisso_v2 RUNTIME DESTINATION bin)
