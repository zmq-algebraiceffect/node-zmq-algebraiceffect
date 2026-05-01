cmake_minimum_required(VERSION 3.14)
project(zmq-algebraiceffect-node)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# --- zmq-algebraiceffect submodule ---
set(ZMQAE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZMQAE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURVE OFF CACHE BOOL "" FORCE)
set(ENABLE_DRAFTS OFF CACHE BOOL "" FORCE)
set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/zmq-algebraiceffect)

# --- Collect submodule lib names ---
set(ZMQAE_LIBZMQ libzmq-static)
set(ZMQAE_CPPZMQ cppzmq-static)

# --- Node addon ---
add_library(${PROJECT_NAME} SHARED
    src/addon.cpp
    deps/zmq-algebraiceffect/src/c_api.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_JS_INC}
    ${CMAKE_SOURCE_DIR}/node_modules/node-addon-api
    deps/zmq-algebraiceffect/include
    deps/zmq-algebraiceffect/extern/cppzmq
    deps/zmq-algebraiceffect/extern/json/single_include
    deps/zmq-algebraiceffect/extern/spdlog/include
    deps/zmq-algebraiceffect/extern/libzmq/include
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${CMAKE_JS_LIB}
    ${ZMQAE_CPPZMQ}
    ${ZMQAE_LIBZMQ}
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    SUFFIX ".node"
)

# Platform-specific linking
if(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE pthread)
endif()

if(APPLE)
    find_library(SECURITY_FRAMEWORK Security)
    if(SECURITY_FRAMEWORK)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${SECURITY_FRAMEWORK})
    endif()
endif()
