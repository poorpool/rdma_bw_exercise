#include "rdma.h"
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <infiniband/verbs.h>
#include <iostream>
#include <json/value.h>
#include <jsonrpccpp/client.h>
#include <jsonrpccpp/client/connectors/tcpsocketclient.h>
#include <jsonrpccpp/common/procedure.h>
#include <jsonrpccpp/common/specification.h>
#include <malloc.h>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

using std::cerr;
using std::endl;
using std::string;
using std::unordered_map;

struct ClientContext {
  int link_type; // IBV_LINK_LAYER_XX
  RdmaDeviceInfo dev_info;
  char *buf; // 存放 write/send 的 buffer, kWriteSize * kRdmaQueueSize 长
             // 前 kRdmaQueueSize / 2 做 write 的 buffer，后 kRdmaQueueSize
             // / 2 做 send buffer
  ibv_mr *mr; // 只是创建删除时候使用
  ibv_mr *recv_mr;
  char *small_buf;
  ibv_cq *cq;
  ibv_qp *qp;
  char *ip;
  int port;
  std::queue<size_t> que;
  uint32_t rkey; // 对面的 rkey
  uint64_t remote_addr;
  unordered_map<uint64_t, int> wr_id2id;

  void BuildRdmaEnvironment(const string &dev_name) {
    // 1. dev_info and pd
    link_type = IBV_LINK_LAYER_UNSPECIFIED;
    auto dev_infos = RdmaGetRdmaDeviceInfoByNames({dev_name}, link_type);
    if (dev_infos.size() != 1 || link_type == IBV_LINK_LAYER_UNSPECIFIED) {
      cerr << "query " << dev_name << "failed" << endl;
      exit(0);
    }
    dev_info = dev_infos[0];

    // 2. mr and buffer
    buf = reinterpret_cast<char *>(memalign(4096, kWriteSize * kRdmaQueueSize));
    mr = ibv_reg_mr(dev_info.pd, buf, kWriteSize * kRdmaQueueSize,
                    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                        IBV_ACCESS_REMOTE_READ);
    for (size_t i = 0; i < kRdmaQueueSize / 2; i++) {
      que.push(i);
    }
    if (mr == nullptr) {
      cerr << "register mr failed" << endl;
      exit(0);
    }
    small_buf = reinterpret_cast<char *>(memalign(4096, 4096));
    recv_mr = ibv_reg_mr(dev_info.pd, small_buf, 4096,
                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                             IBV_ACCESS_REMOTE_READ);
    if (recv_mr == nullptr) {
      cerr << "register recv_mr failed" << endl;
      exit(0);
    }

    // 3. create cq
    cq = dev_info.CreateCq(kRdmaQueueSize);
    if (cq == nullptr) {
      cerr << "create cq failed" << endl;
      exit(0);
    }
    qp = nullptr;
  }

  void DestroyRdmaEnvironment() {
    if (qp != nullptr) {
      ibv_destroy_qp(qp);
      qp = nullptr;
    }
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    free(buf);
    ibv_dereg_mr(recv_mr);
    free(small_buf);
    ibv_dealloc_pd(dev_info.pd);
    ibv_close_device(dev_info.ctx);
  }
} c_ctx;

void ExchangeQP() { // NOLINT
  if (c_ctx.qp != nullptr) {
    cerr << "qp already inited" << endl;
  }
  c_ctx.qp = RdmaCreateQp(c_ctx.dev_info.pd, c_ctx.cq, c_ctx.cq, kRdmaQueueSize,
                          IBV_QPT_RC);
  if (c_ctx.qp == nullptr) {
    cerr << "create qp failed" << endl;
    exit(0);
  }
  RdmaQpExchangeInfo local_info;
  local_info.lid = c_ctx.dev_info.port_attr.lid;
  local_info.qpNum = c_ctx.qp->qp_num;
  ibv_query_gid(c_ctx.dev_info.ctx, kRdmaDefaultPort, kGidIndex,
                &local_info.gid);
  local_info.gid_index = kGidIndex;
  printf("local lid %d qp_num %d gid %s gid_index %d\n", local_info.lid,
         local_info.qpNum, RdmaGid2Str(local_info.gid).c_str(),
         local_info.gid_index);
  Json::Value req;
  req["lid"] = local_info.lid;
  req["qp_num"] = local_info.qpNum;
  req["gid"] = RdmaGid2Str(local_info.gid);
  req["gid_index"] = local_info.gid_index;

  jsonrpc::TcpSocketClient client(c_ctx.ip, c_ctx.port);
  jsonrpc::Client c(client);
  Json::Value resp = c.CallMethod("ExchangeQP", req);

  RdmaQpExchangeInfo remote_info;
  remote_info.lid = static_cast<uint16_t>(resp["lid"].asUInt());
  remote_info.qpNum = resp["qp_num"].asUInt();
  remote_info.gid = RdmaStr2Gid(resp["gid"].asString());
  remote_info.gid_index = resp["gid_index"].asInt();
  printf("remote lid %d qp_num %d gid %s gid_index %d\n", local_info.lid,
         local_info.qpNum, RdmaGid2Str(local_info.gid).c_str(),
         local_info.gid_index);
  c_ctx.rkey = resp["rkey"].asUInt();
  c_ctx.remote_addr = resp["remote_addr"].asUInt64();

  RdmaModifyQp2Rts(c_ctx.qp, local_info, remote_info);
  RdmaPostRecv(4096, c_ctx.recv_mr->length, 114514, c_ctx.qp, c_ctx.small_buf);
}

bool should_infini_loop = true;

void HandleCtrlc(int /*signum*/) { should_infini_loop = false; }

ibv_wc wc[kRdmaQueueSize];
int main(int argc, char *argv[]) {
  signal(SIGINT, HandleCtrlc);
  signal(SIGTERM, HandleCtrlc);

  if (argc != 4) {
    printf("Usage: %s <dev_name> <server_ip> <server_port>\n", argv[0]);
    return 0;
  }
  string dev_name = argv[1];
  c_ctx.ip = argv[2];
  c_ctx.port = atoi(argv[3]);

  c_ctx.BuildRdmaEnvironment(dev_name);

  ExchangeQP();

  uint32_t id = 0;
  int32_t outgoing_reqs = 0; // send + 1, send success -1
  while (should_infini_loop) {
    int n = ibv_poll_cq(c_ctx.cq, kRdmaQueueSize, wc);
    for (int i = 0; i < n; i++) {
      if (wc[i].status == IBV_WC_SUCCESS) {
        if (wc[i].opcode == IBV_WC_RDMA_WRITE) {
#ifdef SHOW_DEBUG_INFO
          printf("write #%d wr_id %lu successed\n", c_ctx.wr_id2id[wc[i].wr_id],
                 wc[i].wr_id);
#endif
        } else if (wc[i].opcode == IBV_WC_SEND) {
#ifdef SHOW_DEBUG_INFO
          printf("send  #%d wr_id %lu successed\n", c_ctx.wr_id2id[wc[i].wr_id],
                 wc[i].wr_id);
#endif
          size_t loc = wc[i].wr_id - reinterpret_cast<uint64_t>(c_ctx.buf);
          loc /= kWriteSize;
          loc -= kRdmaQueueSize / 2;
        } else if (wc[i].opcode == IBV_WC_RECV) {
          // printf("Thank you!\n");
          for (int i = 0; i < kRdmaQueueSize / 2; i++) {
            c_ctx.que.push(i);
          }
          RdmaPostRecv(4096, c_ctx.recv_mr->length, 114514, c_ctx.qp,
                       c_ctx.small_buf);
        } else {
          printf("unknown wc[i].opcode %d\n", wc[i].opcode);
        }
      } else {
        printf("error: wc[i].status %d\n", wc[i].status);
      }
    }
    for (int i = 0; i < kTransmitDepth && !c_ctx.que.empty(); i++) {
      size_t loc = c_ctx.que.front();
      c_ctx.que.pop();
      memset(c_ctx.buf + loc * kWriteSize, 'a' + id % 26, kWriteSize);
      {
        int ret = 0;
        struct ibv_send_wr *bad_send_wr;

        struct ibv_sge write_list;
        memset(&write_list, 0, sizeof(ibv_sge));
        write_list.addr =
            reinterpret_cast<uintptr_t>(c_ctx.buf + loc * kWriteSize);
        write_list.length = kWriteSize;
        write_list.lkey = c_ctx.mr->lkey;

        struct ibv_send_wr write_wr;
        memset(&write_wr, 0, sizeof(ibv_send_wr));
        write_wr.wr_id = write_list.addr;
        write_wr.sg_list = &write_list;
        write_wr.num_sge = 1;
        write_wr.opcode = IBV_WR_RDMA_WRITE;
        write_wr.send_flags = IBV_SEND_SIGNALED;
        write_wr.wr.rdma.remote_addr = c_ctx.remote_addr + loc * kWriteSize;
        write_wr.wr.rdma.rkey = c_ctx.rkey;

        struct ibv_sge send_list;
        memset(&send_list, 0, sizeof(ibv_sge));
        send_list.addr = reinterpret_cast<uintptr_t>(
            c_ctx.buf + (loc + kRdmaQueueSize / 2) * kWriteSize);
        send_list.length = 4096;
        send_list.lkey = c_ctx.mr->lkey;

        struct ibv_send_wr send_wr;
        memset(&send_wr, 0, sizeof(ibv_send_wr));
        send_wr.wr_id = send_list.addr;
        send_wr.next = nullptr;
        send_wr.sg_list = &send_list;
        send_wr.num_sge = 1;
        send_wr.opcode = IBV_WR_SEND_WITH_IMM;
        send_wr.send_flags = IBV_SEND_SIGNALED;
        send_wr.imm_data = id;

        write_wr.next = &send_wr;
        c_ctx.wr_id2id[send_wr.wr_id] = id;
        c_ctx.wr_id2id[write_wr.wr_id] = id;

        ret = ibv_post_send(c_ctx.qp, &write_wr, &bad_send_wr);
        if (ret != 0) {
          printf("post send error %d\n", ret);
        } else {
#ifdef SHOW_DEBUG_INFO
          printf("write-send #%d posted, write wr_id=%lu, send wr_id=%lu\n", id,
                 write_wr.wr_id, send_wr.wr_id);
#endif
          // printf("write-send #%d write loc %zu send loc %zu\n", id, loc,
          // loc+kRdmaQueueSize/2);
        }
        outgoing_reqs++;
      }
      id++;
    }
  }
  c_ctx.DestroyRdmaEnvironment();

  return 0;
}