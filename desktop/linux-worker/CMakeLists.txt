cmake_minimum_required(VERSION 3.22)
project(privatelm_linux_worker LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_BUILD_RPATH "$ORIGIN")
set(CMAKE_INSTALL_RPATH "$ORIGIN")
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LLAMA_CPP_DIR
    "${REPO_ROOT}/local_plugins/llama_flutter_android/android/src/main/cpp/llama.cpp")

if(NOT EXISTS "${LLAMA_CPP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Vendored llama.cpp not found at: ${LLAMA_CPP_DIR}")
endif()

option(PRIVATELM_ENABLE_NATIVE "Optimize for the build machine (disable for portable builds)" OFF)

set(BUILD_SHARED_LIBS ON CACHE BOOL "Build shared GGML libraries" FORCE)
set(GGML_BACKEND_DL ON CACHE BOOL "Load GGML backends dynamically" FORCE)
set(GGML_RPC ON CACHE BOOL "Build GGML RPC backend" FORCE)
set(GGML_RPC_RDMA OFF CACHE BOOL "Use TCP RPC" FORCE)
set(GGML_NATIVE ${PRIVATELM_ENABLE_NATIVE} CACHE BOOL "Build for local CPU only" FORCE)
set(GGML_CUDA OFF CACHE BOOL "CPU-only Linux worker" FORCE)
set(GGML_VULKAN OFF CACHE BOOL "CPU-only Linux worker" FORCE)
set(GGML_HIP OFF CACHE BOOL "CPU-only Linux worker" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)

add_subdirectory("${LLAMA_CPP_DIR}" llama.cpp)

add_executable(privatelm-worker
    src/main.cpp
    src/third_party/qrcodegen.cpp
)
target_include_directories(privatelm-worker PRIVATE
    "${LLAMA_CPP_DIR}/ggml/include"
    "${CMAKE_CURRENT_LIST_DIR}/src"
)
target_link_libraries(privatelm-worker PRIVATE ggml pthread)

set_target_properties(privatelm-worker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
