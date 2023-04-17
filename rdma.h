#ifndef MAPLEFS_COMMON_RDMA_H
#define MAPLEFS_COMMON_RDMA_H

#include <infiniband/verbs.h>
#include <string>
#include <vector>

// #define SHOW_DEBUG_INFO
// NOLINTBEGIN(google-objc-function-naming)

// 一个 RDMA 网卡一个 RdmaDeviceInfo
struct RdmaDeviceInfo {
  ibv_context *ctx; // open 设备获得的 context
  ibv_pd *pd;       // 这个设备的 pd
  ibv_port_attr port_attr;
  ibv_device_attr dev_attr;
  [[nodiscard]] ibv_cq *CreateCq(int size) const;
};

// RoCE 网卡建立连接需要交换的信息
struct RdmaQpExchangeInfo {
  uint16_t lid;
  uint32_t qpNum;
  union ibv_gid gid;
  int gid_index;
};

constexpr int kRdmaDefaultPort = 1; // 查询设备信息时使用的默认端口号
constexpr int kRdmaSl = 0;          // service level
constexpr size_t kWriteSize = 1024 * 1024;
constexpr int kTransmitDepth = 2048; // 同时可以有多少个 Write+Send 组合
// WQ、CQ 的大小
constexpr int kRdmaQueueSize = kTransmitDepth * 2;
constexpr int kGidIndex = 3; // magic

// 通过网卡名称获取 RdmaDeviceInfo
std::vector<RdmaDeviceInfo>
RdmaGetRdmaDeviceInfoByNames(const std::vector<std::string> &names,
                             int &link_type);

// 创建 qp，send_wr recv_wr 大小均为 qe_size
ibv_qp *RdmaCreateQp(ibv_pd *pd, ibv_cq *send_cq, ibv_cq *recv_cq,
                     uint32_t qe_size, ibv_qp_type qp_type);

// 将 gid 转换为便于传输的 string
std::string RdmaGid2Str(ibv_gid gid);

// 将收到的 string 转换为 gid
ibv_gid RdmaStr2Gid(std::string s);

// 把 QP 转换为 Reset 状态
int RdmaModifyQp2Reset(struct ibv_qp *qp);

// 把 QP 转换为 RTS 状态
int RdmaModifyQp2Rts(struct ibv_qp *qp, RdmaQpExchangeInfo &local,
                     RdmaQpExchangeInfo &remote);

int RdmaPostSend(uint32_t req_size, uint32_t lkey, uint64_t wr_id,
                 uint32_t imm_data, ibv_qp *qp, const void *buf);
int RdmaPostRecv(uint32_t req_size, uint32_t lkey, uint64_t wr_id, ibv_qp *qp,
                 const void *buf);
// NOLINTEND(google-objc-function-naming)
#endif // MAPLEFS_COMMON_RDMA_H