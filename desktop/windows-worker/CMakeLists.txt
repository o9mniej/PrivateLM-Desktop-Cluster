cmake_minimum_required(VERSION 3.22)
project(privatelm_windows_worker LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build against the exact llama.cpp / GGML source already vendored by the
# Android plugin. This keeps the RPC protocol version aligned with the app.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LLAMA_CPP_DIR
    "${REPO_ROOT}/local_plugins/llama_flutter_android/android/src/main/cpp/llama.cpp")

if(NOT EXISTS "${LLAMA_CPP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Vendored llama.cpp not found at: ${LLAMA_CPP_DIR}")
endif()

option(PRIVATELM_ENABLE_CUDA "Build the Windows CUDA backend" ON)

set(GGML_BACKEND_DL ON CACHE BOOL "Load GGML backends as DLLs" FORCE)
set(GGML_RPC ON CACHE BOOL "Build the GGML RPC backend" FORCE)
set(GGML_RPC_RDMA OFF CACHE BOOL "Use TCP RPC" FORCE)
set(GGML_NATIVE ON CACHE BOOL "Tune CPU backend for this machine" FORCE)
set(GGML_CUDA ${PRIVATELM_ENABLE_CUDA} CACHE BOOL "Build CUDA backend" FORCE)
set(GGML_VULKAN OFF CACHE BOOL "CUDA is the initial GPU backend" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)

add_subdirectory("${LLAMA_CPP_DIR}" llama.cpp)

add_executable(privatelm-worker src/main.cpp)
target_include_directories(privatelm-worker PRIVATE
    "${LLAMA_CPP_DIR}/ggml/include")
target_link_libraries(privatelm-worker PRIVATE ggml-base)

if(WIN32)
    target_compile_definitions(privatelm-worker PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

set_target_properties(privatelm-worker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
