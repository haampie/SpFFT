/*
 * Copyright (c) 2019 ETH Zurich, Simon Frasch
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef SPFFT_TRANSFORM_REAL_2D_GPU_HPP
#define SPFFT_TRANSFORM_REAL_2D_GPU_HPP

#include <cassert>
#include <complex>
#include <cstddef>
#include <memory>
#include "fft/transform_interface.hpp"
#include "gpu_util/gpu_fft_api.hpp"
#include "gpu_util/gpu_runtime_api.hpp"
#include "gpu_util/gpu_stream_handle.hpp"
#include "memory/array_view_utility.hpp"
#include "memory/gpu_array.hpp"
#include "memory/gpu_array_view.hpp"
#include "spfft/config.h"
#include "util/common_types.hpp"
#include "symmetry/symmetry_gpu.hpp"

namespace spfft {

template <typename T>
class TransformReal2DGPU : public TransformGPU {
public:
  using ValueType = T;
  using ComplexType = gpu::fft::ComplexType<T>;

  TransformReal2DGPU(GPUArrayView3D<T> spaceDomain,
                     GPUArrayView3D<typename gpu::fft::ComplexType<T>::type> freqDomain,
                     GPUStreamHandle stream, std::shared_ptr<GPUArray<char>> workBuffer)
      : stream_(std::move(stream)),
        workBuffer_(std::move(workBuffer)),
        spaceDomain_(spaceDomain),
        freqDomain_(freqDomain) {
    assert(disjoint(spaceDomain, freqDomain));
    assert(workBuffer_);
    assert(spaceDomain.dim_outer() == freqDomain.dim_outer());
    assert(spaceDomain.dim_mid() == freqDomain.dim_mid());
    assert(spaceDomain.dim_inner() / 2 + 1 == freqDomain.dim_inner());

    gpu::fft::check_result(gpu::fft::create(&planForward_));
    gpu::fft::check_result(gpu::fft::create(&planBackward_));

    gpu::fft::check_result(gpu::fft::set_auto_allocation(planForward_, 0));
    gpu::fft::check_result(gpu::fft::set_auto_allocation(planBackward_, 0));

    std::size_t worksizeForward = 0;
    std::size_t worksizeBackward = 0;

    // Starting with CUDA 10.2, a bug with 2D R2C transforms of size (1, x) with x being a prime
    // number was introduced. As workaround, create batched 1D transforms, if one dimension is 1.
    if (spaceDomain.dim_mid() == 1) {
      int rank = 1;
      int n[1] = {spaceDomain.dim_inner()};
      int nembedReal[1] = {spaceDomain.dim_inner()};
      int nembedFreq[1] = {freqDomain.dim_inner()};
      int stride = 1;
      int distReal = spaceDomain.dim_inner();
      int distFreq = freqDomain.dim_inner();
      int batch = spaceDomain.dim_outer();

      // create plan
      gpu::fft::check_result(gpu::fft::make_plan_many(
          planForward_, rank, n, nembedReal, stride, distReal, nembedFreq, stride, distFreq,
          gpu::fft::TransformType::RealToComplex<T>::value, batch, &worksizeForward));
      gpu::fft::check_result(gpu::fft::make_plan_many(
          planBackward_, rank, n, nembedFreq, stride, distFreq, nembedReal, stride, distReal,
          gpu::fft::TransformType::ComplexToReal<T>::value, batch, &worksizeBackward));

    } else if (spaceDomain.dim_inner() == 1) {
      // For consistency, the full result is required along the mid (y) dimension. Therefore, use
      // hermitian symmetry to calculate missing values after R2C transform.
      symm_.reset(new PlaneSymmetryGPU<T>(stream_, freqDomain));

      int rank = 1;
      int n[1] = {spaceDomain.dim_mid()};
      int nembedReal[1] = {spaceDomain.dim_mid()};
      int nembedFreq[1] = {freqDomain.dim_mid()};
      int stride = 1;
      int distReal = spaceDomain.dim_mid();
      int distFreq = freqDomain.dim_mid();
      int batch = spaceDomain.dim_outer();

      // create plan
      gpu::fft::check_result(gpu::fft::make_plan_many(
          planForward_, rank, n, nembedReal, stride, distReal, nembedFreq, stride, distFreq,
          gpu::fft::TransformType::RealToComplex<T>::value, batch, &worksizeForward));
      gpu::fft::check_result(gpu::fft::make_plan_many(
          planBackward_, rank, n, nembedFreq, stride, distFreq, nembedReal, stride, distReal,
          gpu::fft::TransformType::ComplexToReal<T>::value, batch, &worksizeBackward));

    } else {
      int rank = 2;
      int n[2] = {spaceDomain.dim_mid(), spaceDomain.dim_inner()};
      int nembedReal[2] = {spaceDomain.dim_mid(), spaceDomain.dim_inner()};
      int nembedFreq[2] = {freqDomain.dim_mid(), freqDomain.dim_inner()};
      int stride = 1;
      int distReal = spaceDomain.dim_inner() * spaceDomain.dim_mid();
      int distFreq = freqDomain.dim_inner() * freqDomain.dim_mid();
      int batch = spaceDomain.dim_outer();

      // create plan
      gpu::fft::check_result(gpu::fft::make_plan_many(
          planForward_, rank, n, nembedReal, stride, distReal, nembedFreq, stride, distFreq,
          gpu::fft::TransformType::RealToComplex<T>::value, batch, &worksizeForward));
      gpu::fft::check_result(gpu::fft::make_plan_many(
          planBackward_, rank, n, nembedFreq, stride, distFreq, nembedReal, stride, distReal,
          gpu::fft::TransformType::ComplexToReal<T>::value, batch, &worksizeBackward));
    }

    // set stream
    gpu::fft::check_result(gpu::fft::set_stream(planForward_, stream_.get()));
    gpu::fft::check_result(gpu::fft::set_stream(planBackward_, stream_.get()));

    const std::size_t worksize =
        worksizeForward > worksizeBackward ? worksizeForward : worksizeBackward;
    // resize work buffer if necessary
    if (workBuffer_->size() < worksize) {
      *workBuffer_ = GPUArray<char>(worksize);
    }
  }

  TransformReal2DGPU(const TransformReal2DGPU& transform) = delete;

  TransformReal2DGPU(TransformReal2DGPU&& transform) noexcept
      : stream_(std::move(transform.stream_)),
        planForward_(std::move(transform.planForward_)),
        planBackward_(std::move(transform.planBackward_)),
        workBuffer_(std::move(transform.workBuffer_)),
        spaceDomain_(transform.spaceDomain_),
        freqDomain_(transform.freqDomain_),
        symm_(std::move(transform.symm_)) {
    transform.planForward_ = 0;
    transform.planBackward_ = 0;
  }

  ~TransformReal2DGPU() {
    if (planForward_) {
      gpu::fft::destroy(planForward_);
      planForward_ = 0;
    }
    if (planBackward_) {
      gpu::fft::destroy(planBackward_);
      planBackward_ = 0;
    }
  }

  auto operator=(const TransformReal2DGPU& transform) -> TransformReal2DGPU& = delete;

  auto operator=(TransformReal2DGPU&& transform) noexcept -> TransformReal2DGPU& {
    if (planForward_) {
      gpu::fft::destroy(planForward_);
      planForward_ = 0;
    }
    if (planBackward_) {
      gpu::fft::destroy(planBackward_);
      planBackward_ = 0;
    }
    stream_ = std::move(transform.stream_);
    planForward_ = std::move(transform.planForward_);
    planBackward_ = std::move(transform.planBackward_);
    workBuffer_ = std::move(transform.workBuffer_);
    spaceDomain_ = transform.spaceDomain_;
    freqDomain_ = transform.freqDomain_;
    symm_ = std::move(transform.symm_);

    transform.planForward_ = 0;
    transform.planBackward_ = 0;
    return *this;
  }

  inline auto device_id() const noexcept -> int { return stream_.device_id(); }

  auto forward() -> void override {
    if(symm_) {
      // Make sure buffer is zero before transform, such that the symmtry operation can identify
      // elements, which have not been written to by the FFT
      gpu::check_status(gpu::memset_async(
          static_cast<void*>(freqDomain_.data()), 0,
          freqDomain_.size() * sizeof(typename decltype(freqDomain_)::ValueType), stream_.get()));
    }
    gpu::fft::check_result(gpu::fft::set_work_area(planForward_, workBuffer_->data()));
    gpu::fft::check_result(
        gpu::fft::execute(planForward_, spaceDomain_.data(), freqDomain_.data()));

    if(symm_) symm_->apply();
  }

  auto backward() -> void override {
    gpu::fft::check_result(gpu::fft::set_work_area(planBackward_, workBuffer_->data()));
    gpu::fft::check_result(
        gpu::fft::execute(planBackward_, freqDomain_.data(), spaceDomain_.data()));
  }

private:
  GPUStreamHandle stream_;
  gpu::fft::HandleType planForward_ = 0;
  gpu::fft::HandleType planBackward_ = 0;
  std::shared_ptr<GPUArray<char>> workBuffer_;
  GPUArrayView3D<T> spaceDomain_;
  GPUArrayView3D<typename gpu::fft::ComplexType<T>::type> freqDomain_;
  std::unique_ptr<PlaneSymmetryGPU<T>> symm_;
};
}  // namespace spfft

#endif
