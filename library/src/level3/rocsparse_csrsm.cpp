/*! \file */
/* ************************************************************************
 * Copyright (c) 2020-2021 Advanced Micro Devices, Inc.
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
#include "rocsparse_csrsm.hpp"

#include "definitions.h"
#include "utility.h"

#include "../level1/rocsparse_gthr.hpp"
#include "../level2/rocsparse_csrsv.hpp"
#include "csrsm_device.h"
#include <rocprim/rocprim.hpp>

template <typename T>
rocsparse_status rocsparse_csrsm_buffer_size_template(rocsparse_handle          handle,
                                                      rocsparse_operation       trans_A,
                                                      rocsparse_operation       trans_B,
                                                      rocsparse_int             m,
                                                      rocsparse_int             nrhs,
                                                      rocsparse_int             nnz,
                                                      const T*                  alpha,
                                                      const rocsparse_mat_descr descr,
                                                      const T*                  csr_val,
                                                      const rocsparse_int*      csr_row_ptr,
                                                      const rocsparse_int*      csr_col_ind,
                                                      const T*                  B,
                                                      rocsparse_int             ldb,
                                                      rocsparse_mat_info        info,
                                                      rocsparse_solve_policy    policy,
                                                      size_t*                   buffer_size)
{
    // Check for valid handle and matrix descriptor
    if(handle == nullptr)
    {
        return rocsparse_status_invalid_handle;
    }
    else if(descr == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(info == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    // Logging
    log_trace(handle,
              replaceX<T>("rocsparse_Xcsrsm_buffer_size"),
              trans_A,
              trans_B,
              m,
              nrhs,
              nnz,
              LOG_TRACE_SCALAR_VALUE(handle, alpha),
              (const void*&)descr,
              (const void*&)csr_val,
              (const void*&)csr_row_ptr,
              (const void*&)csr_col_ind,
              (const void*&)B,
              ldb,
              (const void*&)info,
              policy,
              (const void*&)buffer_size);

    // Check index base
    if(descr->type != rocsparse_matrix_type_general)
    {
        // TODO
        return rocsparse_status_not_implemented;
    }

    // Check operation type
    if(trans_A != rocsparse_operation_none && trans_A != rocsparse_operation_transpose)
    {
        return rocsparse_status_not_implemented;
    }
    if(trans_B != rocsparse_operation_none && trans_B != rocsparse_operation_transpose)
    {
        return rocsparse_status_not_implemented;
    }

    // Check sizes
    if(m < 0)
    {
        return rocsparse_status_invalid_size;
    }
    else if(nrhs < 0)
    {
        return rocsparse_status_invalid_size;
    }
    else if(nnz < 0)
    {
        return rocsparse_status_invalid_size;
    }

    if(trans_B == rocsparse_operation_none && ldb < m)
    {
        return rocsparse_status_invalid_size;
    }
    else if(trans_B == rocsparse_operation_transpose && ldb < nrhs)
    {
        return rocsparse_status_invalid_size;
    }

    // Check for valid buffer_size pointer
    if(buffer_size == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    // Quick return if possible
    if(m == 0 || nrhs == 0 || nnz == 0)
    {
        // Do not return 0 as buffer size
        *buffer_size = 4;
        return rocsparse_status_success;
    }

    // Check pointer arguments
    if(csr_row_ptr == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(csr_col_ind == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(csr_val == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(B == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(alpha == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    // Stream
    hipStream_t stream = handle->stream;

    // rocsparse_int max_nnz
    *buffer_size = 256;

    // Each thread block performs at most blockdim columns of the
    // rhs matrix. Therefore, the number of blocks depend on nrhs
    // and the blocksize.
    // Because of this, we might need a larger done_array compared
    // to csrsv.
    int blockdim = 512;
    while(nrhs <= blockdim && blockdim > 32)
    {
        blockdim >>= 1;
    }

    blockdim <<= 1;
    int narrays = (nrhs - 1) / blockdim + 1;

    // int done_array
    *buffer_size += sizeof(int) * ((m * narrays - 1) / 256 + 1) * 256;

    // rocsparse_int workspace
    *buffer_size += sizeof(rocsparse_int) * ((m - 1) / 256 + 1) * 256;

    // int workspace2
    *buffer_size += sizeof(int) * ((m - 1) / 256 + 1) * 256;

    size_t         rocprim_size;
    rocsparse_int* ptr  = reinterpret_cast<rocsparse_int*>(buffer_size);
    int*           ptr2 = reinterpret_cast<int*>(buffer_size);

    rocprim::double_buffer<rocsparse_int> dummy(ptr, ptr);
    rocprim::double_buffer<int>           dummy2(ptr2, ptr2);

    RETURN_IF_HIP_ERROR(
        rocprim::radix_sort_pairs(nullptr, rocprim_size, dummy2, dummy, m, 0, 32, stream));

    // rocprim buffer
    *buffer_size += rocprim_size;

    // Additional buffer to store transpose of B, if trans_B == rocsparse_operation_none
    if(trans_B == rocsparse_operation_none)
    {
        *buffer_size += sizeof(T) * ((m * nrhs - 1) / 256 + 1) * 256;
    }

    // Additional buffer to store transpose A, if transA == rocsparse_operation_transpose
    if(trans_A == rocsparse_operation_transpose)
    {
        size_t transpose_size;

        // Determine rocprim buffer size
        RETURN_IF_HIP_ERROR(
            rocprim::radix_sort_pairs(nullptr, transpose_size, dummy, dummy, nnz, 0, 32, stream));

        // rocPRIM does not support in-place sorting, so we need an additional buffer
        transpose_size += sizeof(rocsparse_int) * ((nnz - 1) / 256 + 1) * 256;
        transpose_size += sizeof(T) * ((nnz - 1) / 256 + 1) * 256;

        *buffer_size += transpose_size;
    }

    return rocsparse_status_success;
}

template <typename T>
rocsparse_status rocsparse_csrsm_analysis_template(rocsparse_handle          handle,
                                                   rocsparse_operation       trans_A,
                                                   rocsparse_operation       trans_B,
                                                   rocsparse_int             m,
                                                   rocsparse_int             nrhs,
                                                   rocsparse_int             nnz,
                                                   const T*                  alpha,
                                                   const rocsparse_mat_descr descr,
                                                   const T*                  csr_val,
                                                   const rocsparse_int*      csr_row_ptr,
                                                   const rocsparse_int*      csr_col_ind,
                                                   const T*                  B,
                                                   rocsparse_int             ldb,
                                                   rocsparse_mat_info        info,
                                                   rocsparse_analysis_policy analysis,
                                                   rocsparse_solve_policy    solve,
                                                   void*                     temp_buffer)
{
    // Check for valid handle and matrix descriptor
    if(handle == nullptr)
    {
        return rocsparse_status_invalid_handle;
    }
    else if(descr == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }
    else if(info == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    // Logging
    log_trace(handle,
              replaceX<T>("rocsparse_Xcsrsm_analysis"),
              trans_A,
              trans_B,
              m,
              nrhs,
              nnz,
              LOG_TRACE_SCALAR_VALUE(handle, alpha),
              (const void*&)descr,
              (const void*&)csr_val,
              (const void*&)csr_row_ptr,
              (const void*&)csr_col_ind,
              (const void*&)B,
              ldb,
              (const void*&)info,
              analysis,
              solve,
              (const void*&)temp_buffer);

    // Check operation type
    if(trans_A != rocsparse_operation_none && trans_A != rocsparse_operation_transpose)
    {
        return rocsparse_status_not_implemented;
    }
    if(trans_B != rocsparse_operation_none && trans_B != rocsparse_operation_transpose)
    {
        return rocsparse_status_not_implemented;
    }

    // Check matrix type
    if(descr->type != rocsparse_matrix_type_general)
    {
        // TODO
        return rocsparse_status_not_implemented;
    }

    // Check analysis policy
    if(analysis != rocsparse_analysis_policy_reuse && analysis != rocsparse_analysis_policy_force)
    {
        return rocsparse_status_invalid_value;
    }

    // Check solve policy
    if(solve != rocsparse_solve_policy_auto)
    {
        return rocsparse_status_invalid_value;
    }

    // Check sizes
    if(m < 0)
    {
        return rocsparse_status_invalid_size;
    }
    else if(nrhs < 0)
    {
        return rocsparse_status_invalid_size;
    }
    else if(nnz < 0)
    {
        return rocsparse_status_invalid_size;
    }

    // Quick return if possible
    if(m == 0 || nrhs == 0 || nnz == 0)
    {
        return rocsparse_status_success;
    }

    // Check pointer arguments
    if(csr_row_ptr == nullptr || csr_col_ind == nullptr || csr_val == nullptr || B == nullptr
       || alpha == nullptr || temp_buffer == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    // Switch between lower and upper triangular analysis
    if(descr->fill_mode == rocsparse_fill_mode_upper)
    {
        // Differentiate the analysis policies
        if(analysis == rocsparse_analysis_policy_reuse)
        {
            // We try to re-use already analyzed upper part, if available.
            // It is the user's responsibility that this data is still valid,
            // since he passed the 'reuse' flag.

            // If csrsm meta data is already available, do nothing
            if(trans_A == rocsparse_operation_none && info->csrsm_upper_info != nullptr)
            {
                return rocsparse_status_success;
            }
            else if(trans_A == rocsparse_operation_transpose && info->csrsmt_upper_info != nullptr)
            {
                return rocsparse_status_success;
            }

            // Check for other upper analysis meta data

            if(trans_A == rocsparse_operation_none && info->csrsv_upper_info != nullptr)
            {
                // csrsv meta data
                info->csrsm_upper_info = info->csrsv_upper_info;
                return rocsparse_status_success;
            }

            if(trans_A == rocsparse_operation_transpose && info->csrsvt_upper_info != nullptr)
            {
                // csrsv meta data
                info->csrsmt_upper_info = info->csrsvt_upper_info;
                return rocsparse_status_success;
            }
        }

        // User is explicitly asking to force a re-analysis, or no valid data has been
        // found to be re-used

        // Clear csrsm info
        RETURN_IF_ROCSPARSE_ERROR(rocsparse_destroy_trm_info((trans_A == rocsparse_operation_none)
                                                                 ? info->csrsm_upper_info
                                                                 : info->csrsmt_upper_info));

        // Create csrsm info
        RETURN_IF_ROCSPARSE_ERROR(rocsparse_create_trm_info((trans_A == rocsparse_operation_none)
                                                                ? &info->csrsm_upper_info
                                                                : &info->csrsmt_upper_info));

        // Perform analysis
        RETURN_IF_ROCSPARSE_ERROR(rocsparse_trm_analysis(handle,
                                                         trans_A,
                                                         m,
                                                         nnz,
                                                         descr,
                                                         csr_val,
                                                         csr_row_ptr,
                                                         csr_col_ind,
                                                         (trans_A == rocsparse_operation_none)
                                                             ? info->csrsm_upper_info
                                                             : info->csrsmt_upper_info,
                                                         &info->zero_pivot,
                                                         temp_buffer));
    }
    else
    {
        // Differentiate the analysis policies
        if(analysis == rocsparse_analysis_policy_reuse)
        {
            // We try to re-use already analyzed lower part, if available.
            // It is the user's responsibility that this data is still valid,
            // since he passed the 'reuse' flag.

            // If csrsm meta data is already available, do nothing
            if(trans_A == rocsparse_operation_none && info->csrsm_lower_info != nullptr)
            {
                return rocsparse_status_success;
            }
            else if(trans_A == rocsparse_operation_transpose && info->csrsmt_lower_info != nullptr)
            {
                return rocsparse_status_success;
            }

            // Check for other lower analysis meta data

            if(trans_A == rocsparse_operation_none && info->csrilu0_info != nullptr)
            {
                // csrilu0 meta data
                info->csrsm_lower_info = info->csrilu0_info;
                return rocsparse_status_success;
            }
            else if(trans_A == rocsparse_operation_none && info->csric0_info != nullptr)
            {
                // csric0 meta data
                info->csrsm_lower_info = info->csric0_info;
                return rocsparse_status_success;
            }
            else if(trans_A == rocsparse_operation_none && info->csrsv_lower_info != nullptr)
            {
                // csrsv meta data
                info->csrsm_lower_info = info->csrsv_lower_info;
                return rocsparse_status_success;
            }

            if(trans_A == rocsparse_operation_transpose && info->csrsvt_lower_info != nullptr)
            {
                // csrsv meta data
                info->csrsm_upper_info = info->csrsvt_lower_info;
                return rocsparse_status_success;
            }
        }

        // User is explicitly asking to force a re-analysis, or no valid data has been
        // found to be re-used

        // Clear csrsm info
        RETURN_IF_ROCSPARSE_ERROR(rocsparse_destroy_trm_info((trans_A == rocsparse_operation_none)
                                                                 ? info->csrsm_lower_info
                                                                 : info->csrsmt_lower_info));

        // Create csrsm info
        RETURN_IF_ROCSPARSE_ERROR(rocsparse_create_trm_info((trans_A == rocsparse_operation_none)
                                                                ? &info->csrsm_lower_info
                                                                : &info->csrsmt_lower_info));

        // Perform analysis
        RETURN_IF_ROCSPARSE_ERROR(rocsparse_trm_analysis(handle,
                                                         trans_A,
                                                         m,
                                                         nnz,
                                                         descr,
                                                         csr_val,
                                                         csr_row_ptr,
                                                         csr_col_ind,
                                                         (trans_A == rocsparse_operation_none)
                                                             ? info->csrsm_lower_info
                                                             : info->csrsmt_lower_info,
                                                         &info->zero_pivot,
                                                         temp_buffer));
    }

    return rocsparse_status_success;
}

template <unsigned int BLOCKSIZE, unsigned int WFSIZE, bool SLEEP, typename T, typename U>
__launch_bounds__(BLOCKSIZE) __global__ void csrsm(rocsparse_int m,
                                                   rocsparse_int nrhs,
                                                   U             alpha_device_host,
                                                   const rocsparse_int* __restrict__ csr_row_ptr,
                                                   const rocsparse_int* __restrict__ csr_col_ind,
                                                   const T* __restrict__ csr_val,
                                                   T* __restrict__ B,
                                                   rocsparse_int ldb,
                                                   int* __restrict__ done_array,
                                                   rocsparse_int* __restrict__ map,
                                                   rocsparse_int* __restrict__ zero_pivot,
                                                   rocsparse_index_base idx_base,
                                                   rocsparse_fill_mode  fill_mode,
                                                   rocsparse_diag_type  diag_type)
{
    auto alpha = load_scalar_device_host(alpha_device_host);
    csrsm_device<BLOCKSIZE, WFSIZE, SLEEP>(m,
                                           nrhs,
                                           alpha,
                                           csr_row_ptr,
                                           csr_col_ind,
                                           csr_val,
                                           B,
                                           ldb,
                                           done_array,
                                           map,
                                           zero_pivot,
                                           idx_base,
                                           fill_mode,
                                           diag_type);
}

template <typename T, typename U>
rocsparse_status rocsparse_csrsm_solve_dispatch(rocsparse_handle          handle,
                                                rocsparse_operation       trans_A,
                                                rocsparse_operation       trans_B,
                                                rocsparse_int             m,
                                                rocsparse_int             nrhs,
                                                rocsparse_int             nnz,
                                                U                         alpha_device_host,
                                                const rocsparse_mat_descr descr,
                                                const T*                  csr_val,
                                                const rocsparse_int*      csr_row_ptr,
                                                const rocsparse_int*      csr_col_ind,
                                                T*                        B,
                                                rocsparse_int             ldb,
                                                rocsparse_mat_info        info,
                                                rocsparse_solve_policy    policy,
                                                void*                     temp_buffer)
{

    // Stream
    hipStream_t stream = handle->stream;

    // Buffer
    char* ptr = reinterpret_cast<char*>(temp_buffer);

    ptr += 256;

    // Each thread block performs at most blockdim columns of the
    // rhs matrix. Therefore, the number of blocks depend on nrhs
    // and the blocksize.
    // Because of this, we might need a larger done_array compared
    // to csrsv.
    int blockdim = 512;
    while(nrhs <= blockdim && blockdim > 32)
    {
        blockdim >>= 1;
    }
    blockdim <<= 1;

    int narrays = (nrhs - 1) / blockdim + 1;

    // done array
    int* done_array = reinterpret_cast<int*>(ptr);
    ptr += sizeof(int) * ((m * narrays - 1) / 256 + 1) * 256;

    // Temporary array to store transpose of B
    T* Bt = B;
    if(trans_B == rocsparse_operation_none)
    {
        Bt = reinterpret_cast<T*>(ptr);
        ptr += sizeof(T) * ((m * nrhs - 1) / 256 + 1) * 256;
    }

    // Temporary array to store transpose of A
    T* At = nullptr;
    if(trans_A == rocsparse_operation_transpose)
    {
        At = reinterpret_cast<T*>(ptr);
    }

    // Initialize buffers
    RETURN_IF_HIP_ERROR(hipMemsetAsync(done_array, 0, sizeof(int) * m * narrays, stream));

    rocsparse_trm_info csrsm_info
        = (descr->fill_mode == rocsparse_fill_mode_upper)
              ? ((trans_A == rocsparse_operation_none) ? info->csrsm_upper_info
                                                       : info->csrsmt_upper_info)
              : ((trans_A == rocsparse_operation_none) ? info->csrsm_lower_info
                                                       : info->csrsmt_lower_info);

    // If diag type is unit, re-initialize zero pivot to remove structural zeros
    if(descr->diag_type == rocsparse_diag_type_unit)
    {
        rocsparse_int max = std::numeric_limits<rocsparse_int>::max();
        RETURN_IF_HIP_ERROR(hipMemcpyAsync(
            info->zero_pivot, &max, sizeof(rocsparse_int), hipMemcpyHostToDevice, stream));

        // Wait for device transfer to finish
        RETURN_IF_HIP_ERROR(hipStreamSynchronize(stream));
    }

    // Leading dimension
    rocsparse_int ldimB = ldb;

    // Transpose B if B is not transposed yet to improve performance
    if(trans_B == rocsparse_operation_none)
    {
        // Leading dimension for transposed B
        ldimB = nrhs;

#define CSRSM_DIM_X 32
#define CSRSM_DIM_Y 8
        dim3 csrsm_blocks((m - 1) / CSRSM_DIM_X + 1);
        dim3 csrsm_threads(CSRSM_DIM_X * CSRSM_DIM_Y);

        hipLaunchKernelGGL((csrsm_transpose<CSRSM_DIM_X, CSRSM_DIM_Y>),
                           csrsm_blocks,
                           csrsm_threads,
                           0,
                           stream,
                           m,
                           nrhs,
                           B,
                           ldb,
                           Bt,
                           ldimB);
#undef CSRSM_DIM_X
#undef CSRSM_DIM_Y
    }

    // Pointers to differentiate between transpose mode
    const rocsparse_int* local_csr_row_ptr = csr_row_ptr;
    const rocsparse_int* local_csr_col_ind = csr_col_ind;
    const T*             local_csr_val     = csr_val;

    rocsparse_fill_mode fill_mode = descr->fill_mode;

    // When computing transposed triangular solve, we first need to update the
    // transposed matrix values
    if(trans_A == rocsparse_operation_transpose)
    {
        T* csrt_val = At;

        // Gather values
        RETURN_IF_ROCSPARSE_ERROR(rocsparse_gthr_template(
            handle, nnz, csr_val, csrt_val, csrsm_info->trmt_perm, rocsparse_index_base_zero));

        local_csr_row_ptr = csrsm_info->trmt_row_ptr;
        local_csr_col_ind = csrsm_info->trmt_col_ind;
        local_csr_val     = csrt_val;

        fill_mode = (fill_mode == rocsparse_fill_mode_lower) ? rocsparse_fill_mode_upper
                                                             : rocsparse_fill_mode_lower;
    }

    dim3 csrsm_blocks(((nrhs - 1) / blockdim + 1) * m);
    dim3 csrsm_threads(blockdim);

    // Determine gcnArch and ASIC revision
    int gcnArch = handle->properties.gcnArch;
    int asicRev = handle->asic_rev;

    // rocsparse_pointer_mode_device

    if(blockdim == 64)
    {
        if(gcnArch == 908 && asicRev < 2)
        {
            hipLaunchKernelGGL((csrsm<64, 64, true>),
                               csrsm_blocks,
                               csrsm_threads,
                               0,
                               stream,
                               m,
                               nrhs,
                               alpha_device_host,
                               local_csr_row_ptr,
                               local_csr_col_ind,
                               local_csr_val,
                               Bt,
                               ldimB,
                               done_array,
                               csrsm_info->row_map,
                               info->zero_pivot,
                               descr->base,
                               fill_mode,
                               descr->diag_type);
        }
        else
        {
            hipLaunchKernelGGL((csrsm<64, 64, false>),
                               csrsm_blocks,
                               csrsm_threads,
                               0,
                               stream,
                               m,
                               nrhs,
                               alpha_device_host,
                               local_csr_row_ptr,
                               local_csr_col_ind,
                               local_csr_val,
                               Bt,
                               ldimB,
                               done_array,
                               csrsm_info->row_map,
                               info->zero_pivot,
                               descr->base,
                               fill_mode,
                               descr->diag_type);
        }
    }
    else if(blockdim == 128)
    {
        if(gcnArch == 908 && asicRev < 2)
        {
            hipLaunchKernelGGL((csrsm<128, 64, true>),
                               csrsm_blocks,
                               csrsm_threads,
                               0,
                               stream,
                               m,
                               nrhs,
                               alpha_device_host,
                               local_csr_row_ptr,
                               local_csr_col_ind,
                               local_csr_val,
                               Bt,
                               ldimB,
                               done_array,
                               csrsm_info->row_map,
                               info->zero_pivot,
                               descr->base,
                               fill_mode,
                               descr->diag_type);
        }
        else
        {
            hipLaunchKernelGGL((csrsm<128, 64, false>),
                               csrsm_blocks,
                               csrsm_threads,
                               0,
                               stream,
                               m,
                               nrhs,
                               alpha_device_host,
                               local_csr_row_ptr,
                               local_csr_col_ind,
                               local_csr_val,
                               Bt,
                               ldimB,
                               done_array,
                               csrsm_info->row_map,
                               info->zero_pivot,
                               descr->base,
                               fill_mode,
                               descr->diag_type);
        }
    }
    else if(blockdim == 256)
    {
        if(gcnArch == 908 && asicRev < 2)
        {
            hipLaunchKernelGGL((csrsm<256, 64, true>),
                               csrsm_blocks,
                               csrsm_threads,
                               0,
                               stream,
                               m,
                               nrhs,
                               alpha_device_host,
                               local_csr_row_ptr,
                               local_csr_col_ind,
                               local_csr_val,
                               Bt,
                               ldimB,
                               done_array,
                               csrsm_info->row_map,
                               info->zero_pivot,
                               descr->base,
                               fill_mode,
                               descr->diag_type);
        }
        else
        {
            hipLaunchKernelGGL((csrsm<256, 64, false>),
                               csrsm_blocks,
                               csrsm_threads,
                               0,
                               stream,
                               m,
                               nrhs,
                               alpha_device_host,
                               local_csr_row_ptr,
                               local_csr_col_ind,
                               local_csr_val,
                               Bt,
                               ldimB,
                               done_array,
                               csrsm_info->row_map,
                               info->zero_pivot,
                               descr->base,
                               fill_mode,
                               descr->diag_type);
        }
    }
    else if(blockdim == 512)
    {
        if(gcnArch == 908 && asicRev < 2)
        {
            hipLaunchKernelGGL((csrsm<512, 64, true>),
                               csrsm_blocks,
                               csrsm_threads,
                               0,
                               stream,
                               m,
                               nrhs,
                               alpha_device_host,
                               local_csr_row_ptr,
                               local_csr_col_ind,
                               local_csr_val,
                               Bt,
                               ldimB,
                               done_array,
                               csrsm_info->row_map,
                               info->zero_pivot,
                               descr->base,
                               fill_mode,
                               descr->diag_type);
        }
        else
        {
            hipLaunchKernelGGL((csrsm<512, 64, false>),
                               csrsm_blocks,
                               csrsm_threads,
                               0,
                               stream,
                               m,
                               nrhs,
                               alpha_device_host,
                               local_csr_row_ptr,
                               local_csr_col_ind,
                               local_csr_val,
                               Bt,
                               ldimB,
                               done_array,
                               csrsm_info->row_map,
                               info->zero_pivot,
                               descr->base,
                               fill_mode,
                               descr->diag_type);
        }
    }
    else if(blockdim == 1024)
    {
        if(gcnArch == 908 && asicRev < 2)
        {
            hipLaunchKernelGGL((csrsm<1024, 64, true>),
                               csrsm_blocks,
                               csrsm_threads,
                               0,
                               stream,
                               m,
                               nrhs,
                               alpha_device_host,
                               local_csr_row_ptr,
                               local_csr_col_ind,
                               local_csr_val,
                               Bt,
                               ldimB,
                               done_array,
                               csrsm_info->row_map,
                               info->zero_pivot,
                               descr->base,
                               fill_mode,
                               descr->diag_type);
        }
        else
        {
            hipLaunchKernelGGL((csrsm<1024, 64, false>),
                               csrsm_blocks,
                               csrsm_threads,
                               0,
                               stream,
                               m,
                               nrhs,
                               alpha_device_host,
                               local_csr_row_ptr,
                               local_csr_col_ind,
                               local_csr_val,
                               Bt,
                               ldimB,
                               done_array,
                               csrsm_info->row_map,
                               info->zero_pivot,
                               descr->base,
                               fill_mode,
                               descr->diag_type);
        }
    }
    else
    {
        return rocsparse_status_internal_error;
    }

    // Transpose B back if B was not initially transposed
    if(trans_B == rocsparse_operation_none)
    {
#define CSRSM_DIM_X 32
#define CSRSM_DIM_Y 8
        dim3 csrsm_blocks((m - 1) / CSRSM_DIM_X + 1);
        dim3 csrsm_threads(CSRSM_DIM_X * CSRSM_DIM_Y);

        hipLaunchKernelGGL((csrsm_transpose_back<CSRSM_DIM_X, CSRSM_DIM_Y>),
                           csrsm_blocks,
                           csrsm_threads,
                           0,
                           stream,
                           m,
                           nrhs,
                           Bt,
                           ldimB,
                           B,
                           ldb);
#undef CSRSM_DIM_X
#undef CSRSM_DIM_Y
    }

    return rocsparse_status_success;
}

template <typename T>
rocsparse_status rocsparse_csrsm_solve_template(rocsparse_handle          handle,
                                                rocsparse_operation       trans_A,
                                                rocsparse_operation       trans_B,
                                                rocsparse_int             m,
                                                rocsparse_int             nrhs,
                                                rocsparse_int             nnz,
                                                const T*                  alpha_device_host,
                                                const rocsparse_mat_descr descr,
                                                const T*                  csr_val,
                                                const rocsparse_int*      csr_row_ptr,
                                                const rocsparse_int*      csr_col_ind,
                                                T*                        B,
                                                rocsparse_int             ldb,
                                                rocsparse_mat_info        info,
                                                rocsparse_solve_policy    policy,
                                                void*                     temp_buffer)
{
    // Check for valid handle and matrix descriptor
    if(handle == nullptr)
    {
        return rocsparse_status_invalid_handle;
    }
    else if(descr == nullptr || info == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    // Logging
    log_trace(handle,
              replaceX<T>("rocsparse_Xcsrsm_solve"),
              trans_A,
              trans_B,
              m,
              nrhs,
              nnz,
              LOG_TRACE_SCALAR_VALUE(handle, alpha_device_host),
              (const void*&)descr,
              (const void*&)csr_val,
              (const void*&)csr_row_ptr,
              (const void*&)csr_col_ind,
              (const void*&)B,
              ldb,
              (const void*&)info,
              policy,
              (const void*&)temp_buffer);

    log_bench(handle,
              "./rocsparse-bench -f csrsm -r",
              replaceX<T>("X"),
              "--mtx <matrix.mtx> ",
              "--alpha",
              LOG_BENCH_SCALAR_VALUE(handle, alpha_device_host));

    // Check operation type
    if(trans_A != rocsparse_operation_none && trans_A != rocsparse_operation_transpose)
    {
        return rocsparse_status_not_implemented;
    }
    if(trans_B != rocsparse_operation_none && trans_B != rocsparse_operation_transpose)
    {
        return rocsparse_status_not_implemented;
    }

    if(descr->type != rocsparse_matrix_type_general)
    {
        // TODO
        return rocsparse_status_not_implemented;
    }

    // Check sizes
    if(m < 0 || nrhs < 0 || nnz < 0)
    {
        return rocsparse_status_invalid_size;
    }

    // Quick return if possible
    if(m == 0 || nrhs == 0 || nnz == 0)
    {
        return rocsparse_status_success;
    }

    // Check pointer arguments
    if(csr_val == nullptr || csr_row_ptr == nullptr || csr_col_ind == nullptr
       || alpha_device_host == nullptr || B == nullptr || temp_buffer == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    if(handle->pointer_mode == rocsparse_pointer_mode_device)
    {
        return rocsparse_csrsm_solve_dispatch(handle,
                                              trans_A,
                                              trans_B,
                                              m,
                                              nrhs,
                                              nnz,
                                              alpha_device_host,
                                              descr,
                                              csr_val,
                                              csr_row_ptr,
                                              csr_col_ind,
                                              B,
                                              ldb,
                                              info,
                                              policy,
                                              temp_buffer);
    }
    else
    {
        return rocsparse_csrsm_solve_dispatch(handle,
                                              trans_A,
                                              trans_B,
                                              m,
                                              nrhs,
                                              nnz,
                                              *alpha_device_host,
                                              descr,
                                              csr_val,
                                              csr_row_ptr,
                                              csr_col_ind,
                                              B,
                                              ldb,
                                              info,
                                              policy,
                                              temp_buffer);
    }
}

/*
 * ===========================================================================
 *    C wrapper
 * ===========================================================================
 */

extern "C" rocsparse_status rocsparse_csrsm_zero_pivot(rocsparse_handle   handle,
                                                       rocsparse_mat_info info,
                                                       rocsparse_int*     position)
{
    // Check for valid handle and matrix descriptor
    if(handle == nullptr)
    {
        return rocsparse_status_invalid_handle;
    }
    else if(info == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    // Logging
    log_trace(handle, "rocsparse_csrsm_zero_pivot", (const void*&)info, (const void*&)position);

    // Check pointer arguments
    if(position == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    // Stream
    hipStream_t stream = handle->stream;

    // If m == 0 || nrhs == 0 || nnz == 0 it can happen, that info structure is not created.
    // In this case, always return -1.
    if(info->zero_pivot == nullptr)
    {
        if(handle->pointer_mode == rocsparse_pointer_mode_device)
        {
            RETURN_IF_HIP_ERROR(hipMemsetAsync(position, 255, sizeof(rocsparse_int), stream));
        }
        else
        {
            *position = -1;
        }

        return rocsparse_status_success;
    }

    // Differentiate between pointer modes
    if(handle->pointer_mode == rocsparse_pointer_mode_device)
    {
        // rocsparse_pointer_mode_device
        rocsparse_int zero_pivot;

        RETURN_IF_HIP_ERROR(hipMemcpyAsync(
            &zero_pivot, info->zero_pivot, sizeof(rocsparse_int), hipMemcpyDeviceToHost, stream));

        // Wait for host transfer to finish
        RETURN_IF_HIP_ERROR(hipStreamSynchronize(stream));

        if(zero_pivot == std::numeric_limits<rocsparse_int>::max())
        {
            RETURN_IF_HIP_ERROR(hipMemsetAsync(position, 255, sizeof(rocsparse_int), stream));
        }
        else
        {
            RETURN_IF_HIP_ERROR(hipMemcpyAsync(position,
                                               info->zero_pivot,
                                               sizeof(rocsparse_int),
                                               hipMemcpyDeviceToDevice,
                                               stream));

            return rocsparse_status_zero_pivot;
        }
    }
    else
    {
        // rocsparse_pointer_mode_host
        RETURN_IF_HIP_ERROR(
            hipMemcpy(position, info->zero_pivot, sizeof(rocsparse_int), hipMemcpyDeviceToHost));

        // If no zero pivot is found, set -1
        if(*position == std::numeric_limits<rocsparse_int>::max())
        {
            *position = -1;
        }
        else
        {
            return rocsparse_status_zero_pivot;
        }
    }

    return rocsparse_status_success;
}

extern "C" rocsparse_status rocsparse_csrsm_clear(rocsparse_handle handle, rocsparse_mat_info info)
{
    // Check for valid handle and matrix descriptor
    if(handle == nullptr)
    {
        return rocsparse_status_invalid_handle;
    }
    else if(info == nullptr)
    {
        return rocsparse_status_invalid_pointer;
    }

    // Logging
    log_trace(handle, "rocsparse_csrsm_clear", (const void*&)info);

    // Clear csrsm meta data (this includes lower, upper and their transposed equivalents
    if(!rocsparse_check_trm_shared(info, info->csrsm_lower_info))
    {
        RETURN_IF_ROCSPARSE_ERROR(rocsparse_destroy_trm_info(info->csrsm_lower_info));
    }
    if(!rocsparse_check_trm_shared(info, info->csrsm_upper_info))
    {
        RETURN_IF_ROCSPARSE_ERROR(rocsparse_destroy_trm_info(info->csrsm_upper_info));
    }

    info->csrsm_lower_info = nullptr;
    info->csrsm_upper_info = nullptr;

    return rocsparse_status_success;
}

//
// rocsparse_xcsrsm_buffer_size
//
#define C_IMPL(NAME, TYPE)                                                  \
    extern "C" rocsparse_status NAME(rocsparse_handle          handle,      \
                                     rocsparse_operation       trans_A,     \
                                     rocsparse_operation       trans_B,     \
                                     rocsparse_int             m,           \
                                     rocsparse_int             nrhs,        \
                                     rocsparse_int             nnz,         \
                                     const TYPE*               alpha,       \
                                     const rocsparse_mat_descr descr,       \
                                     const TYPE*               csr_val,     \
                                     const rocsparse_int*      csr_row_ptr, \
                                     const rocsparse_int*      csr_col_ind, \
                                     const TYPE*               B,           \
                                     rocsparse_int             ldb,         \
                                     rocsparse_mat_info        info,        \
                                     rocsparse_solve_policy    policy,      \
                                     size_t*                   buffer_size) \
    {                                                                       \
        return rocsparse_csrsm_buffer_size_template(handle,                 \
                                                    trans_A,                \
                                                    trans_B,                \
                                                    m,                      \
                                                    nrhs,                   \
                                                    nnz,                    \
                                                    alpha,                  \
                                                    descr,                  \
                                                    csr_val,                \
                                                    csr_row_ptr,            \
                                                    csr_col_ind,            \
                                                    B,                      \
                                                    ldb,                    \
                                                    info,                   \
                                                    policy,                 \
                                                    buffer_size);           \
    }

C_IMPL(rocsparse_scsrsm_buffer_size, float);
C_IMPL(rocsparse_dcsrsm_buffer_size, double);
C_IMPL(rocsparse_ccsrsm_buffer_size, rocsparse_float_complex);
C_IMPL(rocsparse_zcsrsm_buffer_size, rocsparse_double_complex);

#undef C_IMPL

//
// rocsparse_xcsrsm_analysis
//
#define C_IMPL(NAME, TYPE)                                                  \
    extern "C" rocsparse_status NAME(rocsparse_handle          handle,      \
                                     rocsparse_operation       trans_A,     \
                                     rocsparse_operation       trans_B,     \
                                     rocsparse_int             m,           \
                                     rocsparse_int             nrhs,        \
                                     rocsparse_int             nnz,         \
                                     const TYPE*               alpha,       \
                                     const rocsparse_mat_descr descr,       \
                                     const TYPE*               csr_val,     \
                                     const rocsparse_int*      csr_row_ptr, \
                                     const rocsparse_int*      csr_col_ind, \
                                     const TYPE*               B,           \
                                     rocsparse_int             ldb,         \
                                     rocsparse_mat_info        info,        \
                                     rocsparse_analysis_policy analysis,    \
                                     rocsparse_solve_policy    solve,       \
                                     void*                     temp_buffer) \
    {                                                                       \
        return rocsparse_csrsm_analysis_template(handle,                    \
                                                 trans_A,                   \
                                                 trans_B,                   \
                                                 m,                         \
                                                 nrhs,                      \
                                                 nnz,                       \
                                                 alpha,                     \
                                                 descr,                     \
                                                 csr_val,                   \
                                                 csr_row_ptr,               \
                                                 csr_col_ind,               \
                                                 B,                         \
                                                 ldb,                       \
                                                 info,                      \
                                                 analysis,                  \
                                                 solve,                     \
                                                 temp_buffer);              \
    }

C_IMPL(rocsparse_scsrsm_analysis, float);
C_IMPL(rocsparse_dcsrsm_analysis, double);
C_IMPL(rocsparse_ccsrsm_analysis, rocsparse_float_complex);
C_IMPL(rocsparse_zcsrsm_analysis, rocsparse_double_complex);

#undef C_IMPL

//
// rocsparse_xcsrsm_solve
//
#define C_IMPL(NAME, TYPE)                                                  \
    extern "C" rocsparse_status NAME(rocsparse_handle          handle,      \
                                     rocsparse_operation       trans_A,     \
                                     rocsparse_operation       trans_B,     \
                                     rocsparse_int             m,           \
                                     rocsparse_int             nrhs,        \
                                     rocsparse_int             nnz,         \
                                     const TYPE*               alpha,       \
                                     const rocsparse_mat_descr descr,       \
                                     const TYPE*               csr_val,     \
                                     const rocsparse_int*      csr_row_ptr, \
                                     const rocsparse_int*      csr_col_ind, \
                                     TYPE*                     B,           \
                                     rocsparse_int             ldb,         \
                                     rocsparse_mat_info        info,        \
                                     rocsparse_solve_policy    policy,      \
                                     void*                     temp_buffer) \
    {                                                                       \
        return rocsparse_csrsm_solve_template(handle,                       \
                                              trans_A,                      \
                                              trans_B,                      \
                                              m,                            \
                                              nrhs,                         \
                                              nnz,                          \
                                              alpha,                        \
                                              descr,                        \
                                              csr_val,                      \
                                              csr_row_ptr,                  \
                                              csr_col_ind,                  \
                                              B,                            \
                                              ldb,                          \
                                              info,                         \
                                              policy,                       \
                                              temp_buffer);                 \
    }

C_IMPL(rocsparse_scsrsm_solve, float);
C_IMPL(rocsparse_dcsrsm_solve, double);
C_IMPL(rocsparse_ccsrsm_solve, rocsparse_float_complex);
C_IMPL(rocsparse_zcsrsm_solve, rocsparse_double_complex);

#undef C_IMPL
