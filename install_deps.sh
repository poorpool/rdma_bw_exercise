#!/bin/bash

deps_dir=/home/cyx/rdma_bw_exercise/deps # 换成你的目录
install_dir=${deps_dir}/install

mkdir -p ${deps_dir}/install
cd ${deps_dir}/gits

echo "installing jsoncpp..."
cd jsoncpp
mkdir -p build && cd build
cmake -D BUILD_STATIC_LIBS=ON -D BUILD_SHARED_LIBS=ON -D CMAKE_INSTALL_PREFIX=${install_dir} -DCMAKE_POSITION_INDEPENDENT_CODE=ON ..
make -j10
make install
cd ../..

echo "installing libjson-rpc-cpp..."
cd libjson-rpc-cpp
mkdir -p build && cd build
cmake  -DCMAKE_PREFIX_PATH=${install_dir} \
  -DCMAKE_LIBRARY_PATH=${install_dir} \
  -DCMAKE_INSTALL_PREFIX=${install_dir} \
  -DCOMPILE_TESTS=NO -DCOMPILE_STUBGEN=NO -DCOMPILE_EXAMPLES=NO \
  -DHTTP_SERVER=NO -DHTTP_CLIENT=NO -DREDIS_SERVER=NO -DREDIS_CLIENT=NO \
  -DTCP_SOCKET_SERVER=YES -DTCP_SOCKET_CLIENT=YES \
  ..
make -j10
make install
cd ../..