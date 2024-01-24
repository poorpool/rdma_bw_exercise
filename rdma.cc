#include "rdma.h"
#include <arpa/inet.h>
#include <cstring>
#include <infiniband/verbs.h>
#include <string>
#include <vector>

using std::string;
using std::vector;

string RdmaGid2Str(ibv_gid gid) {
  string res;
  for (unsigned char i : gid.raw) {
    char s[3];
    sprintf(s, "%02X", i);
    res += string(s);
  }
  return res;
}

char get_xdigit(char ch) {
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  if (ch >= 'a' && ch <= 'f')
    return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F')
    return ch - 'A' + 10;
  return -1;
}

ibv_gid RdmaStr2Gid(string s) {
  union ibv_gid gid;
  for (int32_t i = 0; i < 16; i++) {
    unsigned char x;
    x = get_xdigit(s[i * 2]);
    gid.raw[i] = x << 4;
    x = get_xdigit(s[i * 2 + 1]);
    gid.raw[i] |= x;
  }
  return gid;
}

vector<RdmaDeviceInfo> RdmaGetRdmaDeviceInfoByNames(const vector<string> &names,
                                                    int &link_type) {
  // logger.debug("RdmaGetRdmaDeviceInfoByNames {}", names.size());
  vector<RdmaDeviceInfo> ans;
  link_type = IBV_LINK_LAYER_UNSPECIFIED;

  // 获取所有 RDMA 设备信息
  ibv_device **dev_list = ibv_get_device_list(nullptr);
  if (dev_list == nullptr) {
    return {};
  }
  for (const auto &name : names) {
    RdmaDeviceInfo info;

    // open device
    ibv_device **dev = dev_list;
    while (dev != nullptr &&
           strcmp(ibv_get_device_name(*dev), name.c_str()) != 0) {
      dev++;
    }
    if (dev == nullptr) {
      printf("device %s not found", name.c_str());
      return {};
    }
    info.ctx = ibv_open_device(*dev);
    if (info.ctx == nullptr) {
      printf("open %s failed", name.c_str());
      return {};
    }

    // get dev_attr
    ibv_query_device(info.ctx, &info.dev_attr);

    // get port_attr, check link_type
    ibv_query_port(info.ctx, kRdmaDefaultPort, &info.port_attr);
    if (info.port_attr.link_layer == IBV_LINK_LAYER_UNSPECIFIED) {
      link_type = IBV_LINK_LAYER_UNSPECIFIED;
      return {};
    }
    if (link_type != IBV_LINK_LAYER_UNSPECIFIED &&
        info.port_attr.link_layer != link_type) {
      link_type = info.port_attr.link_layer;
      return {};
    }
    link_type = info.port_attr.link_layer;

    // allocate pd
    info.pd = ibv_alloc_pd(info.ctx);
    if (info.ctx == nullptr) {
      printf("allocate pd for %s failed", name.c_str());
      return {};
    }

    ans.push_back(info);
  }
  ibv_free_device_list(dev_list);
  return ans;
}

ibv_cq *RdmaDeviceInfo::CreateCq(int cqe_size) const {
  return ibv_create_cq(ctx, cqe_size, nullptr, nullptr, 0);
}

ibv_qp *RdmaCreateQp(ibv_pd *pd, ibv_cq *send_cq, ibv_cq *recv_cq,
                     uint32_t qe_size, ibv_qp_type qp_type) {
  ibv_qp_cap cap;
  memset(&cap, 0, sizeof(ibv_qp_cap));
  cap.max_send_wr = qe_size;
  cap.max_recv_wr = qe_size;
  cap.max_send_sge = 1;
  cap.max_recv_sge = 1;

  ibv_qp_init_attr qp_init_attr;
  memset(&qp_init_attr, 0, sizeof(ibv_qp_init_attr));
  qp_init_attr.send_cq = send_cq;
  qp_init_attr.recv_cq = recv_cq;
  qp_init_attr.cap = cap;
  qp_init_attr.qp_type = qp_type;

  return ibv_create_qp(pd, &qp_init_attr);
}

int RdmaModifyQp2Reset(struct ibv_qp *qp) {
  int ret = 0;

  // change QP state to RESET
  {
    struct ibv_qp_attr qp_attr;
    memset(&qp_attr, 0, sizeof(ibv_qp_attr));
    qp_attr.qp_state = IBV_QPS_RESET;

    ret = ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE);
    if (ret != 0) {
      printf("ibv_modify_qp to RESET failed %d", ret);
    }
  }
  return ret;
}

int RdmaModifyQp2Rts(struct ibv_qp *qp, RdmaQpExchangeInfo &local,
                     RdmaQpExchangeInfo &remote) {
  int ret = 0;

  // change QP state to INIT
  {
    struct ibv_qp_attr qp_attr;
    memset(&qp_attr, 0, sizeof(ibv_qp_attr));

    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                              IBV_ACCESS_REMOTE_ATOMIC |
                              IBV_ACCESS_REMOTE_WRITE;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = kRdmaDefaultPort;

    ret = ibv_modify_qp(qp, &qp_attr,
                        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                            IBV_QP_ACCESS_FLAGS);
    if (ret != 0) {
      printf("ibv_modify_qp to INIT failed %d", ret);
    }
  }

  // Change QP state to RTR
  {
    struct ibv_qp_attr qp_attr;
    memset(&qp_attr, 0, sizeof(ibv_qp_attr));

    qp_attr.qp_state = IBV_QPS_RTR;
    qp_attr.path_mtu = IBV_MTU_1024;
    qp_attr.rq_psn = 0;
    qp_attr.dest_qp_num = remote.qpNum;
    qp_attr.max_dest_rd_atomic = 1;
    qp_attr.min_rnr_timer = 12;

    qp_attr.ah_attr.is_global = 0;
    qp_attr.ah_attr.dlid = remote.lid;
    qp_attr.ah_attr.sl = kRdmaSl;
    qp_attr.ah_attr.src_path_bits = 0;
    qp_attr.ah_attr.port_num = kRdmaDefaultPort;
    if (remote.lid == 0) {
      qp_attr.ah_attr.is_global = 1;
      qp_attr.ah_attr.grh.sgid_index = local.gid_index;
      qp_attr.ah_attr.grh.dgid = remote.gid;
      qp_attr.ah_attr.grh.hop_limit = 0xFF;
      qp_attr.ah_attr.grh.traffic_class = 0;
      qp_attr.ah_attr.grh.flow_label = 0;
    }
    ret =
        ibv_modify_qp(qp, &qp_attr,
                      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                          IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                          IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER | 0);
    if (ret != 0) {
      printf("ibv_modify_qp to RTR failed %d %d %s", ret, errno,
             strerror(errno));
    }
  }

  /* Change QP state to RTS */
  {
    struct ibv_qp_attr qp_attr;
    memset(&qp_attr, 0, sizeof(ibv_qp_attr));
    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = 0;
    qp_attr.max_rd_atomic = 1;
    qp_attr.timeout = 14;
    qp_attr.retry_cnt = 7;
    qp_attr.rnr_retry = 7;

    ret = ibv_modify_qp(qp, &qp_attr,
                        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                            IBV_QP_MAX_QP_RD_ATOMIC);
    if (ret != 0) {
      printf("ibv_modify_qp to RTS failed %d %d", ret, errno);
    }
  }

  return 0;
}

int RdmaPostSend(uint32_t req_size, uint32_t lkey, uint64_t wr_id,
                 uint32_t imm_data, ibv_qp *qp, const void *buf) {
  int ret = 0;
  struct ibv_send_wr *bad_send_wr;

  struct ibv_sge list;
  memset(&list, 0, sizeof(ibv_sge));
  list.addr = reinterpret_cast<uintptr_t>(buf);
  list.length = req_size;
  list.lkey = lkey;

  struct ibv_send_wr send_wr;
  memset(&send_wr, 0, sizeof(ibv_send_wr));
  send_wr.wr_id = wr_id;
  send_wr.sg_list = &list;
  send_wr.num_sge = 1;
  send_wr.opcode = IBV_WR_SEND_WITH_IMM;
  send_wr.send_flags = IBV_SEND_SIGNALED;
  send_wr.imm_data = imm_data;

  ret = ibv_post_send(qp, &send_wr, &bad_send_wr);
  return ret;
}

int RdmaPostRecv(uint32_t req_size, uint32_t lkey, uint64_t wr_id, ibv_qp *qp,
                 const void *buf) {
  int ret = 0;
  struct ibv_recv_wr *bad_recv_wr;

  struct ibv_sge list;
  memset(&list, 0, sizeof(ibv_sge));
  list.addr = reinterpret_cast<uintptr_t>(buf);
  list.length = req_size;
  list.lkey = lkey;

  struct ibv_recv_wr recv_wr;
  memset(&recv_wr, 0, sizeof(ibv_recv_wr));
  recv_wr.wr_id = wr_id;
  recv_wr.sg_list = &list;
  recv_wr.num_sge = 1;

  ret = ibv_post_recv(qp, &recv_wr, &bad_recv_wr);
  return ret;
}