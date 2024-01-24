#!/bin/bash
deps_dir=/home/cyx/rdma_bw_exercise/deps # 换成你的目录

mkdir -p ${deps_dir}/gits
cd ${deps_dir}/gits

echo "downloading jsoncpp..."
git clone https://github.com/open-source-parsers/jsoncpp.git

echo "downloading libjson-rpc-cpp..."
git clone https://github.com/cinemast/libjson-rpc-cpp.git

