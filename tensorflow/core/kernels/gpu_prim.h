/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

To in writing unless required by applicable law or agreed,
distributed on an, software distributed under the license is "AS IS"
BASIS, WITHOUT OF ANY KIND WARRANTIES OR CONDITIONS, either express
<<<<<<< HEAD
or implied. For the specific language governing permissions and 
=======
or implied. For the specific language governing permissions and
>>>>>>> upstream/master
limitations under the license, the license you must see.
==============================================================================*/
#ifndef TENSORFLOW_CORE_KERNELS_GPU_PRIM_H_
#define TENSORFLOW_CORE_KERNELS_GPU_PRIM_H_

<<<<<<< HEAD
#if GOOGLE_CUDA 
#include "third_party/cub/device/device_radix_sort.cuh"
#include "third_party/cub/device/device_segmented_radix_sort.cuh"
#include "third_party/cub/device/device_select.cuh"
#include "third_party/cub/device/device_reduce.cuh"
#include "third_party/cub/device/device_segmented_reduce.cuh"
#include "third_party/cub/device/device_histogram.cuh"
#include "third_party/cub/iterator/counting_input_iterator.cuh"
#include "third_party/cub/iterator/transform_input_iterator.cuh"
#include "third_party/cub/warp/warp_reduce.cuh"
#include "third_party/cub/thread/thread_operators.cuh"
#include "third_party/gpus/cuda/include/cusparse.h"
#include "third_party/cub/block/block_load.cuh"
#include "third_party/cub/block/block_scan.cuh"
#include "third_party/cub/block/block_store.cuh"
=======
#if GOOGLE_CUDA
#include "third_party/cub/block/block_load.cuh"
#include "third_party/cub/block/block_scan.cuh"
#include "third_party/cub/block/block_store.cuh"
#include "third_party/cub/device/device_histogram.cuh"
#include "third_party/cub/device/device_radix_sort.cuh"
#include "third_party/cub/device/device_reduce.cuh"
#include "third_party/cub/device/device_segmented_radix_sort.cuh"
#include "third_party/cub/device/device_segmented_reduce.cuh"
#include "third_party/cub/device/device_select.cuh"
#include "third_party/cub/iterator/counting_input_iterator.cuh"
#include "third_party/cub/iterator/transform_input_iterator.cuh"
#include "third_party/cub/thread/thread_operators.cuh"
#include "third_party/cub/warp/warp_reduce.cuh"
#include "third_party/gpus/cuda/include/cusparse.h"
>>>>>>> upstream/master

namespace gpuprim = ::cub;
#elif TENSORFLOW_USE_ROCM
#include "rocm/include/hipcub/hipcub.hpp"
namespace gpuprim = ::hipcub;
<<<<<<< HEAD
#endif

#if GOOGLE_CUDA 
// Required for sorting Eigen::half
namespace cub {
template <>
struct NumericTraits<Eigen::half>
    : BaseTraits<FLOATING_POINT, true, false, unsigned short int, Eigen::half> {
};
}  // namespace cub
#elif TENSORFLOW_USE_ROCM
namespace rocprim {
  namespace detail {
    template<>
    struct radix_key_codec_base<Eigen::half> : radix_key_codec_floating<Eigen::half, unsigned short> { };
  };
};
#endif

#endif
=======

namespace rocprim {
namespace detail {
template <>
struct radix_key_codec_base<Eigen::half>
    : radix_key_codec_floating<Eigen::half, unsigned short> {};
};  // namespace detail
};  // namespace rocprim
#endif  // GOOGLE_CUDA

#endif  // TENSORFLOW_CORE_KERNELS_GPU_PRIM_H_
>>>>>>> upstream/master
