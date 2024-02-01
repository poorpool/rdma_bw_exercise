# RDMA 测带宽练习

## 下载、安装依赖

注意修改这两个脚本里面的第一行

```bash
./download_deps.sh
./install_deps.sh
```

CMakeLists.txt 也要改一下

## 编译

```bash
./devbuild.sh
```

## 运行

```bash
export LD_LIBRARY_PATH=/home/cyx/rdma_bw_exercise/deps/install/lib
./build/saw_server mlx4_0 7897
```

```bash
export LD_LIBRARY_PATH=/home/cyx/rdma_bw_exercise/deps/install/lib
./build/saw_client mlx4_0 192.168.1.41 7897
```

## 结果

```
local lid 0 qp_num 600 gid FE80000000000000F65214FFFE157E31 gid_index 0
remote lid 0 qp_num 600 gid FE80000000000000F65214FFFE157E31 gid_index 0

bandwidth: 5201.625 MB/s, with 64.000 KiB per send, total 16.000 GiB in 3.303s
```

## 修改常量

都在 rdma.h 里。尤其要注意设置正确的 RDMA 端口号和 gid_index（`show_gids`和`ibstat`等命令查看）
