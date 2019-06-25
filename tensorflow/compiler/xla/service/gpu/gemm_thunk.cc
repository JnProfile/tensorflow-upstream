/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/service/gpu/gemm_thunk.h"

#include <functional>

#include "absl/container/flat_hash_map.h"
#include "absl/types/optional.h"
#include "tensorflow/compiler/xla/primitive_util.h"
#include "tensorflow/compiler/xla/service/gpu/backend_configs.pb.h"
#include "tensorflow/compiler/xla/service/gpu/ir_emission_utils.h"
#include "tensorflow/compiler/xla/service/gpu/stream_executor_util.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/stream_executor/blas.h"
#include "tensorflow/stream_executor/device_memory.h"

namespace xla {
namespace gpu {

GemmThunk::GemmThunk(const BufferAllocation::Slice &lhs_buffer,
                     const BufferAllocation::Slice &rhs_buffer,
                     const BufferAllocation::Slice &output_buffer,
                     bool implements_whole_instruction,
                     const HloInstruction *hlo_instruction)
    : Thunk(Kind::kGemm, hlo_instruction),
      lhs_buffer_(lhs_buffer),
      rhs_buffer_(rhs_buffer),
      output_buffer_(output_buffer),
      implements_whole_instruction_(implements_whole_instruction) {}

Status GemmThunk::ExecuteOnStream(const ExecuteParams &params) {
  auto get_device_address = [&](const BufferAllocation::Slice &slice) {
    return params.buffer_allocations->GetDeviceAddress(slice);
  };

  VLOG(3) << "Running GEMM thunk on instruction: " << hlo_instruction();
  se::DeviceMemoryBase lhs_data = get_device_address(lhs_buffer_);
  se::DeviceMemoryBase rhs_data = get_device_address(rhs_buffer_);
  se::DeviceMemoryBase output_data = get_device_address(output_buffer_);
  return RunGemm(hlo_instruction(), lhs_data, rhs_data, output_data,
                 params.stream, implements_whole_instruction_, params.profiler);
}

// This struct contains the metadata of a matrix, e.g., its base address and
// dimensions.
struct MatrixDescriptor {
  se::DeviceMemoryBase data;
  bool transpose;  // Whether this matrix needs to be transposed.
  int64 num_rows;
  int64 num_cols;
};

template <typename Element>
static bool DoGemmWithAlgorithm(
    int64 batch_size, MatrixDescriptor lhs_matrix, MatrixDescriptor rhs_matrix,
    MatrixDescriptor output_matrix, double alpha, double beta,
    se::Stream *stream, absl::optional<se::blas::AlgorithmType> algorithm,
    se::blas::ProfileResult *output_profile_result) {
  DCHECK(!output_matrix.transpose);

  PrimitiveType type = primitive_util::NativeToPrimitiveType<Element>();

  // Converts from an XLA PrimitiveType to a blas::ComputationType, which is
  // used to specify the precision with which matmul computations should be
  // performed, separately from the precision of the inputs and result.
  se::blas::ComputationType computation_type = [&](PrimitiveType type) {
    switch (type) {
      case F16:
        // Use F32 as computation type for F16 as we currently only implement
        // the cuDNN pseudo half configuration for half precision.
        return se::blas::ComputationType::kF32;
      case F32:
        return se::blas::ComputationType::kF32;
      case F64:
        return se::blas::ComputationType::kF64;
      case C64:
        return se::blas::ComputationType::kComplexF32;
      case C128:
        return se::blas::ComputationType::kComplexF64;
      default:
        LOG(FATAL) << "Unsupported type.";
    }
  }(type);

  se::DeviceMemory<Element> lhs_data(lhs_matrix.data);
  se::DeviceMemory<Element> rhs_data(rhs_matrix.data);
  se::DeviceMemory<Element> output_data(output_matrix.data);

  auto lhs_transpose = lhs_matrix.transpose ? se::blas::Transpose::kTranspose
                                            : se::blas::Transpose::kNoTranspose;
  auto rhs_transpose = rhs_matrix.transpose ? se::blas::Transpose::kTranspose
                                            : se::blas::Transpose::kNoTranspose;
  auto k = lhs_matrix.transpose ? lhs_matrix.num_rows : lhs_matrix.num_cols;

  if (algorithm) {
    // Autotuning is disabled for batch_size != 1.
    CHECK_EQ(1, batch_size);
    return stream
        ->ThenBlasGemmWithAlgorithm(
            lhs_transpose, rhs_transpose, output_matrix.num_rows,
            output_matrix.num_cols,
            /*size of reduce dim=*/k,
            /*alpha=*/static_cast<Element>(alpha), lhs_data,
            /*leading dim of LHS=*/lhs_matrix.num_rows, rhs_data,
            /*leading dim of RHS=*/rhs_matrix.num_rows,
            /*beta=*/static_cast<Element>(beta), &output_data,
            /*leading dim of output=*/output_matrix.num_rows, computation_type,
            *algorithm, output_profile_result)
        .ok();
  }

  if (batch_size != 1) {
    int64 lhs_stride = lhs_matrix.num_rows * lhs_matrix.num_cols;
    int64 rhs_stride = rhs_matrix.num_rows * rhs_matrix.num_cols;
    int64 output_stride = output_matrix.num_rows * output_matrix.num_cols;
    return stream
        ->ThenBlasGemmStridedBatched(
            lhs_transpose, rhs_transpose, output_matrix.num_rows,
            output_matrix.num_cols, /*size of reduce dim=*/k,
            /*alpha=*/alpha, lhs_data,
            /*leading dim of LHS=*/lhs_matrix.num_rows, lhs_stride, rhs_data,
            /*leading dim of RHS=*/rhs_matrix.num_rows, rhs_stride,
            /*beta=*/beta, &output_data,
            /*leading dim of output=*/output_matrix.num_rows, output_stride,
            batch_size)
        .ok();
  }

  return stream
      ->ThenBlasGemm(
          lhs_transpose, rhs_transpose, output_matrix.num_rows,
          output_matrix.num_cols, /*size of reduce dim=*/k, /*alpha=*/alpha,
          lhs_data, /*leading dim of LHS=*/lhs_matrix.num_rows, rhs_data,
          /*leading dim of RHS=*/rhs_matrix.num_rows, /*beta=*/beta,
          &output_data, /*leading dim of output=*/output_matrix.num_rows)
      .ok();
}

Status RunGemm(const HloInstruction *gemm, se::DeviceMemoryBase lhs_buffer,
               se::DeviceMemoryBase rhs_buffer,
               se::DeviceMemoryBase output_buffer, se::Stream *stream,
               bool implements_whole_instruction,
               HloExecutionProfiler *profiler,
               se::blas::ProfileResult *profile_result,
               absl::optional<se::blas::AlgorithmType> algorithm) {
  VLOG(2) << "Executing a GemmThunk";
  CHECK(IsCublasGemm(*gemm));

  TF_ASSIGN_OR_RETURN(GemmBackendConfig backend_config,
                      gemm->backend_config<GemmBackendConfig>());
  const Shape &output_shape = gemm->shape();
  const HloInstruction *lhs = gemm->operand(0);
  const HloInstruction *rhs = gemm->operand(1);

  const Shape &lhs_shape = lhs->shape();
  const Shape &rhs_shape = rhs->shape();

  const DotDimensionNumbers &dim_nums = backend_config.dot_dimension_numbers();
  CHECK_EQ(dim_nums.lhs_batch_dimensions_size(),
           dim_nums.rhs_batch_dimensions_size());
  CHECK_EQ(dim_nums.lhs_batch_dimensions_size() + 2, output_shape.rank());

  int64 row_dim = dim_nums.lhs_batch_dimensions_size();
  int64 col_dim = dim_nums.lhs_batch_dimensions_size() + 1;

  int64 batch_size = backend_config.batch_size();

  // Check that the batch dims don't cover the last two dims.
  for (int64 batch_dim : dim_nums.lhs_batch_dimensions()) {
    CHECK_NE(row_dim, batch_dim);
    CHECK_NE(col_dim, batch_dim);
  }

  // Verify that the non-batch dimensions are minor-most. This is required for
  // efficient access.
  for (const auto *shape : {&lhs_shape, &rhs_shape, &output_shape}) {
    CHECK_LT(shape->layout().minor_to_major(row_dim), 2);
    CHECK_LT(shape->layout().minor_to_major(col_dim), 2);
  }

  // BLAS gemm reduces rows of LHS and columns of RHS. The Dot operator between
  // matrices reduces dimension 1 of LHS and dimension 0 of RHS regardless of
  // their layout. Therefore, we should treat dimension 0 as row and dimension 1
  // as column when mapping a matrix Dot to BLAS gemm.
  int64 output_num_rows = output_shape.dimensions(row_dim);
  int64 output_num_cols = output_shape.dimensions(col_dim);

  // BLAS gemm expects the inputs and the output are in column-major order.
  // Therefore, we need to convert dot between row-major matrices to that
  // between column-major matrices. The key insight for the conversion is that,
  // in linear storage, matrix M in column-major order is identical to the
  // transpose of M in row-major order. In other words,
  //
  //   column-major(M) = row-major(M^T).
  //
  // Leveraging this insight, we can perform dot between row-major matrices as
  // follows.
  //
  // row-major(C)
  //   = row-major(A x B) = column-major((A x B)^T) = column-major(B^T x A^T)
  //   = gemm(column-major(B^T), column-major(A^T))
  //   = gemm(row-major(B), row-major(A))
  //
  // Although we do not modify the content of A and B in linear memory, we
  // should use the dimensions of B^T and A^T when calling gemm. For example,
  // the leading dimension of the LHS matrix of gemm is the number of rows in
  // B^T and thus the number of columns in B.
  auto make_descriptor = [&](se::DeviceMemoryBase data, const Shape &shape,
                             bool transpose) -> MatrixDescriptor {
    bool is_row_major = LayoutUtil::Minor(shape.layout(), row_dim) != 0;
    bool layout_mismatch = LayoutUtil::Minor(shape.layout(), row_dim) !=
                           LayoutUtil::Minor(output_shape.layout(), row_dim);
    return MatrixDescriptor{
        data, static_cast<bool>(transpose ^ layout_mismatch),
        shape.dimensions(row_dim + static_cast<int64>(is_row_major)),
        shape.dimensions(row_dim + static_cast<int64>(!is_row_major))};
  };

  MatrixDescriptor lhs_matrix = make_descriptor(
      lhs_buffer, lhs_shape, dim_nums.lhs_contracting_dimensions(0) == row_dim);
  MatrixDescriptor rhs_matrix = make_descriptor(
      rhs_buffer, rhs_shape, dim_nums.rhs_contracting_dimensions(0) == col_dim);
  std::unique_ptr<ScopedInstructionProfiler> op_profiler =
      profiler ? profiler->MakeScopedInstructionProfiler(
                     implements_whole_instruction ? gemm : nullptr)
               : nullptr;

  if (LayoutUtil::Minor(output_shape.layout(), row_dim) != 0) {
    std::swap(lhs_matrix, rhs_matrix);
    std::swap(output_num_cols, output_num_rows);
  }

  const MatrixDescriptor output_matrix{output_buffer, /*needs_transpose=*/false,
                                       output_num_rows, output_num_cols};
  auto best_algorithm = [&]() -> absl::optional<se::blas::AlgorithmType> {
    if (algorithm) {
      return *algorithm;
    }
    if (backend_config.algorithm_case() ==
        GemmBackendConfig::ALGORITHM_NOT_SET) {
      return absl::nullopt;
    }
    return backend_config.selected_algorithm();
  }();

<<<<<<< HEAD
  // Dispatches to a regular cublas gemm, a gemm-with-algorithm, or attempts
  // to autotune this gemm to figure out the best algorithm.
  PrimitiveType element_type = output_shape.element_type();
  se::blas::ComputationType computation_type =
      GetBlasComputationType(element_type);

  std::string instr_descr =
      hlo_instruction != nullptr ? hlo_instruction->ToString() : "<null>";

  // Try finding the best algorithm by autotuning, or use older Gemm API
  // if autotuning is disabled or has failed.
  absl::optional<se::blas::AlgorithmType> best_algorithm;
  if (xla_gpu_disable_autotune) {
    VLOG(2) << "Autotuning disabled, using generic algorithm";
  } else if (batch_size != 1) {
    // TODO(b/112111608): Implement auto tune for batched gemm.
    VLOG(2) << "Batch size is non-singular, using generic algorithm";
  } else {
#if !defined(TENSORFLOW_USE_ROCM)

    // Disabling GEMM auto-tuning on the ROCm platform because it is not
    // supported (GetBlasGemmAlgorithms returns an empty list)
    //
    // Calling the gemm auto-tuner is supposed to be harmless (even for cases
    // where it does ot work like ROCm), but there seems to be one exception
    // For some reason the gemm auto-tuning code tries to directly allocate
    // GPU memory, as opposed to using stream_executor::ScratchAllocator
    // (like the conv algo picker code). Depending the gemm operation parameters
    // the amount of memory that the auto-tuner tries to allocate, can
    // exceed 100MB, which can result in memory allocation failure. This is
    // because TF already grabs all available GPU memory during initialization,
    // and there is not much free memory left after that point.

    // Autotune may fail for various reasons (e.g. when when CUDA 8 and GPU
    // sm_50 or older are used). In that case the returned best_algorithm
    // will be an empty optional.
    best_algorithm = DoGemmAutotune<Element>(
        batch_size, lhs_matrix, rhs_matrix, output_matrix, stream, output_shape,
        alpha, beta, instr_descr);

#endif
  }

  bool launch_ok = DoGemmWithAlgorithm<Element>(
      batch_size, lhs_matrix, rhs_matrix, output_matrix, alpha, beta,
      computation_type, stream, best_algorithm,
      /*output_profile_result=*/nullptr);

  if (!launch_ok) {
    return InternalError("Unable to launch cuBLAS gemm on stream %p", stream);
  }
  return Status::OK();
}

}  // namespace
=======
  double alpha = backend_config.alpha();
  CHECK_NE(alpha, 0);
  double beta = backend_config.beta();
>>>>>>> upstream/master

  auto fn = [&]() {
    switch (output_shape.element_type()) {
      case F16:
        return &DoGemmWithAlgorithm<Eigen::half>;
      case F32:
        return &DoGemmWithAlgorithm<float>;
      case F64:
        return &DoGemmWithAlgorithm<double>;
      case C64:
        return &DoGemmWithAlgorithm<std::complex<float>>;
      case C128:
        return &DoGemmWithAlgorithm<std::complex<double>>;
      default:
        LOG(FATAL) << "Unsupported type.";
    }
  }();

  bool launch_ok = fn(batch_size, lhs_matrix, rhs_matrix, output_matrix, alpha,
                      beta, stream, best_algorithm,
                      /*output_profile_result=*/profile_result);

  if (!launch_ok) {
    return InternalError("Unable to launch cuBLAS gemm on stream %p", stream);
  }
  return Status::OK();
}

}  // namespace gpu
}  // namespace xla
