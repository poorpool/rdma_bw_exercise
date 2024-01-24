#include "rdma.h"
#include <chrono>
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
#include <string>
#include <vector>

using std::cerr;
using std::endl;
using std::string;

struct ClientContext {
  int link_type; // IBV_LINK_LAYER_XX
  RdmaDeviceInfo dev_info;
  char *buf;  // kTransmitLimit * kBufferSize
  ibv_mr *mr; // 只是创建删除时候使用
  ibv_cq *cq;
  ibv_qp *qp;
  char *ip;
  int port;

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
    buf =
        reinterpret_cast<char *>(memalign(4096, kTransmitLimit * kBufferSize));
    for (int i = 0; i < kTransmitLimit; i++) {
      memset(buf + i * kBufferSize, 'a' + (i % 26), kBufferSize);
    }
    mr = ibv_reg_mr(dev_info.pd, buf, kTransmitLimit * kBufferSize,
                    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                        IBV_ACCESS_REMOTE_READ);
    if (mr == nullptr) {
      cerr << "register mr failed" << endl;
      exit(0);
    }

    // 3. create cq
    cq = dev_info.CreateCq(kRdmaQueueSize * 2);
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

  RdmaModifyQp2Rts(c_ctx.qp, local_info, remote_info);
}

ibv_wc wc[kPollCqSize];

int main(int argc, char *argv[]) {
  if (argc != 4) {
    printf("Usage: %s <dev_name> <server_ip> <server_port>\n", argv[0]);
    return 0;
  }
  string dev_name = argv[1];
  c_ctx.ip = argv[2];
  c_ctx.port = atoi(argv[3]);

  c_ctx.BuildRdmaEnvironment(dev_name);

  ExchangeQP();
  auto start_time = std::chrono::high_resolution_clock::now();
  size_t onflight_tasks = 0;
  for (size_t task = 0; task < kSendTaskNum; task++) {
    while (onflight_tasks >= kTransmitLimit) {
      int n = ibv_poll_cq(c_ctx.cq, kPollCqSize, wc);
      onflight_tasks -= n;
      for (int i = 0; i < n; i++) {
        if (wc[i].status == IBV_WC_SUCCESS) {
          if (wc[i].opcode == IBV_WC_SEND) {
            ;
          } else {
            fprintf(stderr, "ERROR: wc[i] opcode %d", wc[i].opcode);
          }
        } else {
          fprintf(stderr, "ERROR: wc[i] status %d", wc[i].status);
        }
      }
    }
    RdmaPostSend(kBufferSize, c_ctx.mr->lkey, task, task, c_ctx.qp,
                 c_ctx.buf + (task % kTransmitLimit) * kBufferSize);
    onflight_tasks++;
  }

  while (onflight_tasks > 0) {
    int n = ibv_poll_cq(c_ctx.cq, kPollCqSize, wc);
    onflight_tasks -= n;
    for (int i = 0; i < n; i++) {
      if (wc[i].status == IBV_WC_SUCCESS) {
        if (wc[i].opcode == IBV_WC_SEND) {
          ;
        } else {
          fprintf(stderr, "ERROR: wc[i] opcode %d", wc[i].opcode);
        }
      } else {
        fprintf(stderr, "ERROR: wc[i] status %d", wc[i].status);
      }
    }
  }
  auto end_time = std::chrono::high_resolution_clock::now();
  c_ctx.DestroyRdmaEnvironment();
  auto duration_in_us = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time);
  printf("\nbandwidth: %.3f MB/s, with %.3f KiB per send, total %.3f GiB in %.3fs\n",
         kSendTaskNum * kBufferSize * 1.0 / duration_in_us.count(), kBufferSize / 1024.0,
         kSendTaskNum * kBufferSize / 1024.0 / 1024.0 / 1024.0, duration_in_us.count()/1000.0/1000.0);

  return 0;
}