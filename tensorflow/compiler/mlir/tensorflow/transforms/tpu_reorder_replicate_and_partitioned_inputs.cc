/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <cstddef>

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/passes.h"

namespace mlir {
namespace TFTPU {
namespace {

#define GEN_PASS_DEF_TPUREORDERREPLICATEANDPARTITIONEDINPUTSPASS
#include "tensorflow/compiler/mlir/tensorflow/transforms/tf_passes.h.inc"

struct TPUReorderReplicateAndPartitionedInputsPass
    : public impl::TPUReorderReplicateAndPartitionedInputsPassBase<
          TPUReorderReplicateAndPartitionedInputsPass> {
  void runOnOperation() override;
};

LogicalResult ReorderReplicateAndPartitionedInputs(
    TF::TPUReplicatedInputOp replicated_input) {
  if (!llvm::all_of(replicated_input.getInputs(), [](Value input) {
        return llvm::isa_and_nonnull<TF::TPUPartitionedInputV2Op>(
            input.getDefiningOp());
      }))
    return replicated_input.emitOpError()
           << "expects all inputs from 'tf.TPUPartitionedInputV2' ops";

  auto first_partitioned_input = llvm::cast<TF::TPUPartitionedInputV2Op>(
      replicated_input.getOperand(0).getDefiningOp());
  std::optional<::llvm::StringRef> xla_sharding =
      first_partitioned_input.get_XlaSharding();
  auto partition_dims = first_partitioned_input.getPartitionDims();
  size_t num_cores_per_replica = first_partitioned_input.getNumOperands();

  for (auto operand : replicated_input.getInputs().drop_front()) {
    auto partitioned_input =
        llvm::cast<TF::TPUPartitionedInputV2Op>(operand.getDefiningOp());
    std::optional<::llvm::StringRef> op_xla_sharding =
        partitioned_input.get_XlaSharding();
    auto op_partition_dims = partitioned_input.getPartitionDims();
    // Abort if TPUPartitionedInputV2(s) do not have the same attributes.
    if (!llvm::equal(partition_dims, op_partition_dims))
      return partitioned_input->emitOpError()
             << "expects partition_dims = " << partition_dims << " but found "
             << op_partition_dims;
    if (partitioned_input.getNumOperands() != num_cores_per_replica)
      return partitioned_input->emitOpError()
             << "expects " << num_cores_per_replica << " operands but found "
             << partitioned_input.getNumOperands();
    if (xla_sharding != op_xla_sharding)
      return replicated_input.emitOpError()
             << "expects all inputs from 'tf.TPUPartitionedInputV2' ops to "
                "have identical XLA sharding";
  }

  // 2D Matrix to store per core per replica operands. The matrix dimensions are
  // num_cores_per_replica x num_replicas. i-th row holds the operands for i-th
  // core. j-th column holds the operands for j-th replica.
  llvm::SmallVector<llvm::SmallVector<Value, 4>, 4>
      operands_per_replica_per_core;
  operands_per_replica_per_core.resize(num_cores_per_replica);

  // Collect all operands in the 2D matrix.
  for (auto operand : replicated_input.getInputs()) {
    auto pi = llvm::cast<TF::TPUPartitionedInputV2Op>(operand.getDefiningOp());
    for (auto& pi_operand : pi->getOpOperands()) {
      unsigned core_id = pi_operand.getOperandNumber();
      operands_per_replica_per_core[core_id].push_back(pi_operand.get());
    }
  }

  // Create new `tf.TPUReplicatedInput` ops feeding into one
  // `tf.TPUPartitionedInputV2` op.
  OpBuilder builder(replicated_input);
  llvm::SmallVector<Value, 4> operands_per_core;
  for (const auto& operands_per_replica : operands_per_replica_per_core) {
    auto replicate_op = builder.create<TF::TPUReplicatedInputOp>(
        replicated_input.getLoc(), replicated_input.getType(),
        operands_per_replica, replicated_input->getAttrs());
    operands_per_core.push_back(replicate_op);
  }

  auto pi = builder.create<TF::TPUPartitionedInputV2Op>(
      first_partitioned_input.getLoc(), replicated_input.getType(),
      operands_per_core, first_partitioned_input->getAttrs());
  replicated_input.replaceAllUsesWith(pi.getOutput());
  return success();
}

void TPUReorderReplicateAndPartitionedInputsPass::runOnOperation() {
  auto result =
      getOperation()->walk([](TF::TPUReplicatedInputOp replicated_input) {
        if (llvm::none_of(replicated_input.getInputs(), [](Value input) {
              return llvm::isa_and_nonnull<TF::TPUPartitionedInputV2Op>(
                  input.getDefiningOp());
            }))
          return WalkResult::advance();
        if (failed(ReorderReplicateAndPartitionedInputs(replicated_input)))
          return WalkResult::interrupt();

        assert(replicated_input->use_empty());
        replicated_input->erase();
        return WalkResult::advance();
      });

  if (result.wasInterrupted()) {
    signalPassFailure();
    return;
  }

  getOperation()->walk([](TF::TPUPartitionedInputV2Op partitioned_input) {
    if (partitioned_input->use_empty()) partitioned_input->erase();
  });
}

}  // namespace

std::unique_ptr<OperationPass<func::FuncOp>>
CreateTPUReorderReplicateAndPartitionedInputsPass() {
  return std::make_unique<TPUReorderReplicateAndPartitionedInputsPass>();
}

}  // namespace TFTPU
}  // namespace mlir
