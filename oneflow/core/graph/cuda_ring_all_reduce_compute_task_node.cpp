#include "oneflow/core/graph/cuda_ring_all_reduce_compute_task_node.h"

namespace oneflow {

bool CudaRingAllReduceCompTaskNode::IsReadyForBuild() {
  return GetSoleConsumedRegst("in")->IsLocked();
}

void CudaRingAllReduceCompTaskNode::ProduceAllRegstsAndBindEdges() {
  const CudaRingAllReduceOpConf& cuda_ring_all_reduce_conf =
      logical_node()->SoleOp()->op_conf().cuda_ring_all_reduce_conf();
  const int64_t num_link = cuda_ring_all_reduce_conf.link_size();
  const int64_t slice_factor = cuda_ring_all_reduce_conf.slice_factor();
  std::shared_ptr<RegstDesc> out_regst_desc = ProduceRegst("out", false, 1, 1);
  HashSet<TaskNode*> send_to_nodes;
  FOR_RANGE(int64_t, i, 0, num_link) {
    std::shared_ptr<RegstDesc> send_regst_desc =
        ProduceRegst("send_" + std::to_string(i), false, slice_factor * 2, slice_factor * 2);
    FixSendRegstMemCase(send_regst_desc->mut_mem_case(), send_to_.at(i));
    send_to_nodes.emplace(send_to_.at(i));
  }
  this->ForEachOutDataEdge([&](TaskEdge* edge) {
    auto it = send_to_nodes.find(edge->dst_node());
    if (it == send_to_nodes.cend()) {
      edge->AddRegst("out", out_regst_desc);
    } else {
      UNIMPLEMENTED();
    }
  });
}

void CudaRingAllReduceCompTaskNode::ConsumeAllRegsts() {
  FOR_RANGE(int64_t, i, 0, recv_from_.size()) {
    if (dynamic_cast<CudaRingAllReduceCompTaskNode*>(recv_from_.at(i)) != nullptr) {
      ConsumeRegst("recv_" + std::to_string(i),
                   recv_from_.at(i)->GetProducedRegst("send_" + std::to_string(i)));
    } else {
      UNIMPLEMENTED();
    }
  }
  ConsumeRegst("in", SoleInDataEdge()->GetSoleRegst());
}

void CudaRingAllReduceCompTaskNode::BuildExecGphAndRegst() {
  ExecNode* node = mut_exec_gph().NewNode();
  std::shared_ptr<Operator> op = this->logical_node()->SoleOp();
  const LogicalBlobId& lbi = op->op_conf().cuda_ring_all_reduce_conf().lbi();
  node->mut_op() = op;
  node->BindBnWithRegst("in", GetSoleConsumedRegst("in"));
  std::shared_ptr<RegstDesc> out_regst = GetProducedRegst("out");
  out_regst->AddLbi(lbi);
  node->BindBnWithRegst("out", out_regst);
  FOR_RANGE(int64_t, i, 0, send_to_.size()) {
    const std::string recv_name = "recv_" + std::to_string(i);
    const std::string send_name = "send_" + std::to_string(i);
    node->BindBnWithRegst(recv_name, GetSoleConsumedRegst(recv_name));
    std::shared_ptr<RegstDesc> send_regst = GetProducedRegst(send_name);
    send_regst->AddLbi(lbi);
    node->BindBnWithRegst(send_name, send_regst);
  }
  node->InferBlobDescs(parallel_ctx());
}

void CudaRingAllReduceCompTaskNode::InferProducedDataRegstTimeShape() {
  *GetProducedRegst("out")->mut_data_regst_time_shape() =
      GetSoleConsumedRegst("in")->data_regst_time_shape();
  FOR_RANGE(int64_t, i, 0, send_to_.size()) {
    *GetProducedRegst("send_" + std::to_string(i))->mut_data_regst_time_shape() =
        GetSoleConsumedRegst("in")->data_regst_time_shape();
  }
}

void CudaRingAllReduceCompTaskNode::PinConsumedRegstMemCase(MemoryCase* mem_case) {
  if (mem_case->has_host_mem()) {
    mem_case->mutable_host_mem()->mutable_cuda_pinned_mem()->set_device_id(GpuPhyId());
  }
}

void CudaRingAllReduceCompTaskNode::FixSendRegstMemCase(MemoryCase* mem_case, TaskNode* send_to) {
  bool p2p = false;
  if (Global<JobDesc>::Get()->cuda_ring_all_reduce_enable_p2p()
      && send_to->device_type() == DeviceType::kGPU && machine_id() == 0
      && send_to->machine_id() == 0) {
    const int32_t this_dev_id = GpuPhyId();
    const int32_t send_to_dev_id = send_to->GpuPhyId();
    int32_t can_access_peer = 0;
    {
      CudaCurrentDeviceGuard guard(this_dev_id);
      CudaCheck(cudaDeviceCanAccessPeer(&can_access_peer, this_dev_id, send_to_dev_id));
      if (can_access_peer == 1) {
        cudaError_t err = cudaDeviceEnablePeerAccess(send_to_dev_id, 0);
        if (err != cudaErrorPeerAccessAlreadyEnabled) { CudaCheck(err); }
        mem_case->mutable_device_cuda_mem()->set_device_id(send_to_dev_id);
        p2p = true;
      }
    }
  }
  if (!p2p) { mem_case->mutable_host_mem()->mutable_cuda_pinned_mem()->set_device_id(GpuPhyId()); }
}

void CudaRingAllReduceCompTaskNode::EnableMemSharingInReduce(const ReduceMemSharingCtx& ctx) {
  const int64_t offset = ctx.Offset4RankCtxParallelId(GetRankCtx().CtxWithGather(), parallel_id());
  CHECK_EQ(offset, 0);
  ctx.EnableMemSharing4Regst(GetProducedRegst("out").get(), offset);
  if (this->SoleInDataEdge()->src_node()->GetTaskType() == TaskType::kReduceConcat) { return; }
  ctx.EnableMemSharing4Regst(GetSoleConsumedRegst("in").get(), offset);
}

void CudaRingAllReduceCompTaskNode::SetRecvSendNodes(const std::vector<TaskNode*>& recv_from,
                                                     const std::vector<TaskNode*>& send_to) {
  recv_from_ = recv_from;
  send_to_ = send_to;
}

}  // namespace oneflow
