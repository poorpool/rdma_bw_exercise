#include "rdma.h"
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <infiniband/verbs.h>
#include <iostream>
#include <jsonrpccpp/common/procedure.h>
#include <jsonrpccpp/common/specification.h>
#include <jsonrpccpp/server.h>
#include <jsonrpccpp/server/connectors/tcpsocketserver.h>
#include <malloc.h>
#include <queue>
#include <string>
#include <vector>

using jsonrpc::JSON_STRING;
using jsonrpc::PARAMS_BY_NAME;
using jsonrpc::Procedure;
using std::cerr;
using std::endl;
using std::string;

struct ServerContext {
  int link_type; // IBV_LINK_LAYER_XX
  RdmaDeviceInfo dev_info;
  char *buf; // 存放 write/send 的 buffer, kWriteSize * kRdmaQueueSize 长
             // 前 kRdmaQueueSize / 2 做接收 write 的 buffer，后 kRdmaQueueSize
             // / 2 做 recv buffer
  ibv_mr *mr; // 只是创建删除时候使用
  ibv_cq *cq;
  ibv_qp *qp;

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
    if (mr == nullptr) {
      cerr << "register mr failed" << endl;
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
    ibv_dealloc_pd(dev_info.pd);
    ibv_close_device(dev_info.ctx);
  }
} s_ctx;

class ServerJrpcServer : public jsonrpc::AbstractServer<ServerJrpcServer> {
public:
  explicit ServerJrpcServer(jsonrpc::TcpSocketServer &server)
      : AbstractServer<ServerJrpcServer>(server) {
    this->bindAndAddMethod(
        Procedure("ExchangeQP", PARAMS_BY_NAME, JSON_STRING, nullptr),
        &ServerJrpcServer::ExchangeQP);
  }

  void ExchangeQP(const Json::Value &req, Json::Value &resp) { // NOLINT
    if (s_ctx.qp != nullptr) {
      cerr << "qp already inited" << endl;
    }
    s_ctx.qp = RdmaCreateQp(s_ctx.dev_info.pd, s_ctx.cq, s_ctx.cq,
                            kRdmaQueueSize, IBV_QPT_RC);
    if (s_ctx.qp == nullptr) {
      cerr << "create qp failed" << endl;
      exit(0);
    }
    RdmaQpExchangeInfo local_info;
    local_info.lid = s_ctx.dev_info.port_attr.lid;
    local_info.qpNum = s_ctx.qp->qp_num;
    ibv_query_gid(s_ctx.dev_info.ctx, kRdmaDefaultPort, kGidIndex,
                  &local_info.gid);
    local_info.gid_index = kGidIndex;
    printf("local lid %d qp_num %d gid %s gid_index %d", local_info.lid,
           local_info.qpNum, RdmaGid2Str(local_info.gid).c_str(),
           local_info.gid_index);

    RdmaQpExchangeInfo remote_info = {
        .lid = static_cast<uint16_t>(req["lid"].asUInt()),
        .qpNum = req["qp_num"].asUInt(),
        .gid = RdmaStr2Gid(req["gid"].asString()),
        .gid_index = req["gid_index"].asInt()};
    printf("remote lid %d qp_num %d gid %s gid_index %d", local_info.lid,
           local_info.qpNum, RdmaGid2Str(local_info.gid).c_str(),
           local_info.gid_index);

    RdmaModifyQp2Rts(s_ctx.qp, local_info, remote_info);

    for (size_t i = kRdmaQueueSize / 2; i < kRdmaQueueSize; i++) {
      char *recv_buf = s_ctx.buf + i * kWriteSize;
      RdmaPostRecv(kWriteSize, s_ctx.mr->lkey,
                   reinterpret_cast<uint64_t>(recv_buf), s_ctx.qp, recv_buf);
    }

    resp["lid"] = local_info.lid;
    resp["qp_num"] = local_info.qpNum;
    resp["gid"] = RdmaGid2Str(local_info.gid);
    resp["gid_index"] = local_info.gid_index;
    resp["rkey"] = s_ctx.mr->rkey;
    resp["remote_addr"] =
        reinterpret_cast<unsigned long long>(s_ctx.buf); // NOLINT
  }
};

ServerJrpcServer *jrpc_server = nullptr;
bool should_infini_loop = true;

void HandleCtrlc(int /*signum*/) {
  if (jrpc_server != nullptr) {
    jrpc_server->StopListening();
  }
  should_infini_loop = false;
}

int main(int argc, char *argv[]) {
  signal(SIGINT, HandleCtrlc);
  signal(SIGTERM, HandleCtrlc);

  if (argc != 3) {
    printf("Usage: %s <dev_name> <port>\n", argv[0]);
    return 0;
  }
  string dev_name = argv[1];
  int port = atoi(argv[2]);

  s_ctx.BuildRdmaEnvironment(dev_name);

  jsonrpc::TcpSocketServer server("0.0.0.0", port);
  jrpc_server = new ServerJrpcServer(server);
  jrpc_server->StartListening();
  printf("server start listening...\n");

  ibv_wc wc[kRdmaQueueSize];
  char compare_buffer[26][kWriteSize];
  for (int i = 0; i < 26; i++) {
    char c = 'a' + i;
    memset(compare_buffer[i], c, kWriteSize);
  }
  while (should_infini_loop) {
    int n = ibv_poll_cq(s_ctx.cq, kRdmaQueueSize, wc);
    for (int i = 0; i < n; i++) {
      printf("received #%d send\n", wc[i].imm_data);
      uint64_t wr_id = wc[i].wr_id;
      size_t which =
          (wr_id - reinterpret_cast<uint64_t>(s_ctx.buf)) / kWriteSize -
          (kRdmaQueueSize / 2);
      if (memcmp(s_ctx.buf + which * kWriteSize,
                 compare_buffer[wc[i].imm_data % 26], kWriteSize) != 0) {
        printf("OOPS: write %c it not as expected %d\n",
               static_cast<int>(*(s_ctx.buf + which * kWriteSize)),
               'a' + (wc[i].imm_data % 26));
      }
      RdmaPostRecv(kWriteSize, s_ctx.mr->lkey, wc[i].wr_id, s_ctx.qp,
                   reinterpret_cast<void *>(wc[i].wr_id)); // NOLINT
    }
  }
  s_ctx.DestroyRdmaEnvironment();

  return 0;
}