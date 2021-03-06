/*! \file */
/* ************************************************************************
 * Copyright (c) 2020 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * ************************************************************************ */

#include "rocsparse_bsrmv_spzl.hpp"

// BSRMV kernel for BSR block dimension of 16
template <unsigned int BLOCKSIZE, typename T>
__device__ void bsrmvn_16x16_device(rocsparse_int       mb,
                                    rocsparse_direction dir,
                                    T                   alpha,
                                    const rocsparse_int* __restrict__ bsr_row_ptr,
                                    const rocsparse_int* __restrict__ bsr_col_ind,
                                    const T* __restrict__ bsr_val,
                                    const T* __restrict__ x,
                                    T beta,
                                    T* __restrict__ y,
                                    rocsparse_index_base idx_base)
{
    // BSR block dimension
    static constexpr int BSRDIM = 16;

    // BSR block lane id
    rocsparse_int lid = hipThreadIdx_x % BSRDIM;

    // Each thread block processes a single BSR row
    rocsparse_int row = hipBlockIdx_x;

    // Offset into x vector
    rocsparse_int idx
        = (dir == rocsparse_direction_column) ? ((hipThreadIdx_x / BSRDIM) % BSRDIM) : lid;

    // BSR row entry and exit point
    rocsparse_int row_begin = bsr_row_ptr[row] - idx_base;
    rocsparse_int row_end   = bsr_row_ptr[row + 1] - idx_base;

    // BSR block row accumulator
    T sum = static_cast<T>(0);

    // Loop over all BSR blocks in the current row where each lane
    // processes a BSR block value
    for(rocsparse_int j = row_begin; j < row_end; ++j)
    {
        rocsparse_int k = j + hipThreadIdx_x / (BSRDIM * BSRDIM);

        // Do not exceed the row
        if(k < row_end)
        {
            // Column index into x vector
            rocsparse_int col = (bsr_col_ind[k] - idx_base) * BSRDIM;

            // Compute the sum of the two rows within the BSR blocks of the current
            // BSR row
            sum = rocsparse_fma(bsr_val[j * BSRDIM * BSRDIM + hipThreadIdx_x], x[col + idx], sum);
        }
    }

    // Accumulate each row sum of the BSR block
    __shared__ T sdata[BSRDIM * BSRDIM];

    sdata[hipThreadIdx_x] = sum;

    __syncthreads();

    if(dir == rocsparse_direction_column)
    {
        if(hipThreadIdx_x < BSRDIM * 8)
            sdata[hipThreadIdx_x] += sdata[hipThreadIdx_x + BSRDIM * 8];
        __syncthreads();
        if(hipThreadIdx_x < BSRDIM * 4)
            sdata[hipThreadIdx_x] += sdata[hipThreadIdx_x + BSRDIM * 4];
        __threadfence_block();
        if(hipThreadIdx_x < BSRDIM * 2)
            sdata[hipThreadIdx_x] += sdata[hipThreadIdx_x + BSRDIM * 2];
        __threadfence_block();
        if(hipThreadIdx_x < BSRDIM * 1)
            sum = sdata[hipThreadIdx_x] + sdata[hipThreadIdx_x + BSRDIM * 1];
    }
    else
    {
        // Reduce the intra block row sum
        if(lid < 8)
            sdata[hipThreadIdx_x] += sdata[hipThreadIdx_x + 8];
        __syncthreads();
        if(lid < 4)
            sdata[hipThreadIdx_x] += sdata[hipThreadIdx_x + 4];
        __syncthreads();
        if(lid < 2)
            sdata[hipThreadIdx_x] += sdata[hipThreadIdx_x + 2];
        __syncthreads();

        // Final reduction
        if(hipThreadIdx_x < BSRDIM)
            sum = sdata[hipThreadIdx_x * BSRDIM] + sdata[hipThreadIdx_x * BSRDIM + 1];
    }

    // First 16 threads write row sums to global memory
    if(hipThreadIdx_x < BSRDIM)
    {
        if(beta != static_cast<T>(0))
        {
            y[row * BSRDIM + hipThreadIdx_x]
                = rocsparse_fma(beta, y[row * BSRDIM + hipThreadIdx_x], alpha * sum);
        }
        else
        {
            y[row * BSRDIM + hipThreadIdx_x] = alpha * sum;
        }
    }
}

template <unsigned int BLOCKSIZE, typename T, typename U>
__launch_bounds__(BLOCKSIZE) __global__
    void bsrmvn_16x16_kernel(rocsparse_int       mb,
                             rocsparse_direction dir,
                             U                   alpha_device_host,
                             const rocsparse_int* __restrict__ bsr_row_ptr,
                             const rocsparse_int* __restrict__ bsr_col_ind,
                             const T* __restrict__ bsr_val,
                             const T* __restrict__ x,
                             U beta_device_host,
                             T* __restrict__ y,
                             rocsparse_index_base idx_base)
{
    auto alpha = load_scalar_device_host(alpha_device_host);
    auto beta  = load_scalar_device_host(beta_device_host);
    if(alpha != static_cast<T>(0) || beta != static_cast<T>(1))
    {
        bsrmvn_16x16_device<BLOCKSIZE>(
            mb, dir, alpha, bsr_row_ptr, bsr_col_ind, bsr_val, x, beta, y, idx_base);
    }
}

template <typename T, typename U>
void bsrmvn_16x16(rocsparse_handle     handle,
                  rocsparse_direction  dir,
                  rocsparse_int        mb,
                  rocsparse_int        nnzb,
                  U                    alpha_device_host,
                  const rocsparse_int* bsr_row_ptr,
                  const rocsparse_int* bsr_col_ind,
                  const T*             bsr_val,
                  const T*             x,
                  U                    beta_device_host,
                  T*                   y,
                  rocsparse_index_base base)
{

    hipLaunchKernelGGL((bsrmvn_16x16_kernel<256>),
                       dim3(mb),
                       dim3(256),
                       0,
                       handle->stream,
                       mb,
                       dir,
                       alpha_device_host,
                       bsr_row_ptr,
                       bsr_col_ind,
                       bsr_val,
                       x,
                       beta_device_host,
                       y,
                       base);
}

//
// INSTANTIATE.
//
#define INSTANTIATE(TYPE)                                              \
    template void bsrmvn_16x16(rocsparse_handle     handle,            \
                               rocsparse_direction  dir,               \
                               rocsparse_int        mb,                \
                               rocsparse_int        nnzb,              \
                               const TYPE*          alpha_device_host, \
                               const rocsparse_int* bsr_row_ptr,       \
                               const rocsparse_int* bsr_col_ind,       \
                               const TYPE*          bsr_val,           \
                               const TYPE*          x,                 \
                               const TYPE*          beta_device_host,  \
                               TYPE*                y,                 \
                               rocsparse_index_base base);             \
    template void bsrmvn_16x16(rocsparse_handle     handle,            \
                               rocsparse_direction  dir,               \
                               rocsparse_int        mb,                \
                               rocsparse_int        nnzb,              \
                               TYPE                 alpha_device_host, \
                               const rocsparse_int* bsr_row_ptr,       \
                               const rocsparse_int* bsr_col_ind,       \
                               const TYPE*          bsr_val,           \
                               const TYPE*          x,                 \
                               TYPE                 beta_device_host,  \
                               TYPE*                y,                 \
                               rocsparse_index_base base)

INSTANTIATE(float);
INSTANTIATE(double);
INSTANTIATE(rocsparse_float_complex);
INSTANTIATE(rocsparse_double_complex);

#undef INSTANTIATE
