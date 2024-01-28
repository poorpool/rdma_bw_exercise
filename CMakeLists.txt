cmake_minimum_required(VERSION 3.0.0)
project(rdma_bw_exercise VERSION 0.1.0)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE "Release")
SET(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -g -ggdb -fno-omit-frame-pointer -fno-inline-functions")
# SET(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -g -ggdb -fsanitize=address -static-libsan")
SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3")

# 记得换成你的目录
set(CMAKE_PREFIX_PATH "/home/cyx/rdma_bw_exercise/deps/install")
include_directories("/home/cyx/rdma_bw_exercise/deps/install/include")

find_package(jsoncpp REQUIRED)
find_package(libjson-rpc-cpp REQUIRED)

add_executable(saw_server server.cc rdma.cc)
target_link_libraries(saw_server
  ibverbs
  libjson-rpc-cpp::jsonrpcserver
  # jsoncpp_static
)

add_executable(saw_client client.cc rdma.cc)
target_link_libraries(saw_client
  ibverbs
  libjson-rpc-cpp::jsonrpcclient
  # jsoncpp_static
)
