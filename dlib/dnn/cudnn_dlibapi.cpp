// Copyright (C) 2015  Davis E. King (davis@dlib.net)
// License: Boost Software License   See LICENSE.txt for the full license.
#ifndef DLIB_DNN_CuDNN_CPP_
#define DLIB_DNN_CuDNN_CPP_

#ifdef DLIB_USE_CUDA

#include "cudnn_dlibapi.h"
#include "tensor.h"
#include <cudnn.h>
#include <iostream>
#include <string>
#include "cuda_utils.h"


namespace dlib
{

    namespace cuda 
    {

        // TODO, make into a macro that prints more information like the line number, etc.
        static void check(cudnnStatus_t s)
        {
            switch(s)
            {
                case CUDNN_STATUS_SUCCESS: return;
                case CUDNN_STATUS_NOT_INITIALIZED: 
                    throw cudnn_error("CUDA Runtime API initialization failed.");
                case CUDNN_STATUS_ALLOC_FAILED: 
                    throw cudnn_error("CUDA Resources could not be allocated.");
                case CUDNN_STATUS_BAD_PARAM:
                    throw cudnn_error("CUDNN_STATUS_BAD_PARAM");
                default:
                    throw cudnn_error("A call to cuDNN failed: " + std::string(cudnnGetErrorString(s)));
            }
        }

    // ------------------------------------------------------------------------------------

        static cudnnTensorDescriptor_t descriptor(const tensor& t) 
        {
            return (const cudnnTensorDescriptor_t)t.get_cudnn_tensor_descriptor().get_handle();
        }
        static cudnnTensorDescriptor_t descriptor(const tensor_descriptor& t) 
        {
            return (const cudnnTensorDescriptor_t)t.get_handle();
        }

    // ------------------------------------------------------------------------------------

        class cudnn_context
        {
        public:
            // not copyable 
            cudnn_context(const cudnn_context&) = delete;
            cudnn_context& operator=(const cudnn_context&) = delete;

            cudnn_context()
            {
                check(cudnnCreate(&handle));
            }

            ~cudnn_context()
            {
                cudnnDestroy(handle);
            }

            cudnnHandle_t get_handle (
            ) const { return handle; }

        private:
            cudnnHandle_t handle;
        };

        static cudnnHandle_t context()
        {
            thread_local cudnn_context c;
            return c.get_handle();
        }

    // ------------------------------------------------------------------------------------

        tensor_descriptor::
        tensor_descriptor(
        ) : handle(nullptr)
        {
        }

        tensor_descriptor::
        ~tensor_descriptor()
        {
            set_size(0,0,0,0);
        }

        void tensor_descriptor::
        set_size(
            int n, 
            int k,
            int nr, 
            int nc 
        )
        {
            if (n == 0 || nr == 0 || nc == 0 || k == 0)
            {
                if (handle)
                {
                    cudnnDestroyTensorDescriptor((cudnnTensorDescriptor_t)handle);
                    handle = nullptr;
                }
            }
            else
            {
                cudnnTensorDescriptor_t h;
                check(cudnnCreateTensorDescriptor(&h));
                handle = h;

                check(cudnnSetTensor4dDescriptor((cudnnTensorDescriptor_t)handle,
                        CUDNN_TENSOR_NCHW,
                        CUDNN_DATA_FLOAT,
                        n,
                        k,
                        nr,
                        nc));
            }
        }

        void tensor_descriptor::
        get_size (
            int& n, 
            int& k,
            int& nr,
            int& nc
        ) const
        {
            if (handle)
            {
                int nStride, cStride, hStride, wStride;
                cudnnDataType_t datatype;
                check(cudnnGetTensor4dDescriptor((cudnnTensorDescriptor_t)handle,
                        &datatype,
                        &n,
                        &k,
                        &nr,
                        &nc,
                        &nStride,
                        &cStride,
                        &hStride,
                        &wStride));
            }
            else
            {
                n = 0;
                k = 0;
                nr = 0;
                nc = 0;
            }
        }

    // ------------------------------------------------------------------------------------

        void add(
            float beta,
            tensor& dest,
            float alpha,
            const tensor& src
        )
        {
            DLIB_CASSERT(
                  (dest.num_samples()==src.num_samples() || src.num_samples()==1) &&
                  (dest.nr()==src.nr() || src.nr()==1) &&
                  (dest.nc()==src.nc() || src.nc()==1) &&
                  (dest.k()==src.k()   || src.k()==1), "");

            check(cudnnAddTensor_v3(context(),
                                    &alpha,
                                    descriptor(src),
                                    src.device(),
                                    &beta,
                                    descriptor(dest),
                                    dest.device()));
        }

        void set_tensor (
            tensor& t,
            float value
        )
        {
            if (t.size() == 0)
                return;
            check(cudnnSetTensor(context(),
                                 descriptor(t),
                                 t.device(),
                                 &value));
        }

        void scale_tensor (
            tensor& t,
            float value
        )
        {
            if (t.size() == 0)
                return;
            check(cudnnScaleTensor(context(),
                                   descriptor(t),
                                   t.device(),
                                   &value));
        }

        void add_conv_bias_gradient (
            tensor& grad,
            const tensor& gradient_input
        )
        {
            DLIB_CASSERT(
                  grad.num_samples() == 1 &&
                  grad.k()  >= 1 &&
                  grad.nr() == 1 &&
                  grad.nc() == 1 &&
                  gradient_input.k() == grad.k() &&
                  gradient_input.size() > 0,"");

            const float alpha = 1;
            const float beta = 1;
            check(cudnnConvolutionBackwardBias(context(),
                                               &alpha,
                                               descriptor(gradient_input),
                                               gradient_input.device(),
                                               &beta,
                                               descriptor(grad),
                                               grad.device()));
        }

    // ------------------------------------------------------------------------------------
    // ------------------------------------------------------------------------------------

        tensor_conv::
        tensor_conv(
        ) : 
            filter_handle(nullptr),
            conv_handle(nullptr),
            out_num_samples(0),
            out_k(0),
            out_nr(0),
            out_nc(0),
            forward_algo(0),
            forward_workspace_size_in_bytes(0),
            forward_workspace(nullptr),
            backward_data_algo(0),
            backward_data_workspace_size_in_bytes(0),
            backward_data_workspace(nullptr),
            backward_filters_algo(0),
            backward_filters_workspace_size_in_bytes(0),
            backward_filters_workspace(nullptr)
        {
        }

        void tensor_conv::
        clear (
        )
        {
            if (filter_handle) 
                cudnnDestroyFilterDescriptor((cudnnFilterDescriptor_t)filter_handle);
            if (conv_handle) 
                cudnnDestroyConvolutionDescriptor((cudnnConvolutionDescriptor_t)conv_handle);
            filter_handle = nullptr;
            conv_handle = nullptr;
            out_num_samples = 0;
            out_k = 0;
            out_nr = 0;
            out_nc = 0;

            if (forward_workspace)
                cudaFree(forward_workspace);
            forward_workspace = nullptr;
            forward_algo = 0;
            forward_workspace_size_in_bytes = 0;

            if (backward_data_workspace)
                cudaFree(backward_data_workspace);
            backward_data_workspace = nullptr;
            backward_data_algo = 0;
            backward_data_workspace_size_in_bytes = 0;

            if (backward_filters_workspace)
                cudaFree(backward_filters_workspace);
            backward_filters_workspace = nullptr;
            backward_filters_algo = 0;
            backward_filters_workspace_size_in_bytes = 0;
        }

        void tensor_conv::
        setup(
            const tensor& data,
            const tensor& filters,
            int stride_y_,
            int stride_x_
        ) 
        {
            DLIB_CASSERT(data.k() == filters.k(),"");
            clear();
            try
            {
                stride_y = stride_y_;
                stride_x = stride_x_;

                check(cudnnCreateFilterDescriptor((cudnnFilterDescriptor_t*)&filter_handle));
                check(cudnnSetFilter4dDescriptor((cudnnFilterDescriptor_t)filter_handle, 
                                                 CUDNN_DATA_FLOAT, 
                                                 filters.num_samples(),
                                                 filters.k(),
                                                 filters.nr(),
                                                 filters.nc()));

                check(cudnnCreateConvolutionDescriptor((cudnnConvolutionDescriptor_t*)&conv_handle));
                check(cudnnSetConvolution2dDescriptor((cudnnConvolutionDescriptor_t)conv_handle,
                        filters.nr()/2, // vertical padding
                        filters.nc()/2, // horizontal padding
                        stride_y,
                        stride_x,
                        1, 1, // must be 1,1
                        CUDNN_CONVOLUTION)); // could also be CUDNN_CROSS_CORRELATION

                check(cudnnGetConvolution2dForwardOutputDim(
                        (const cudnnConvolutionDescriptor_t)conv_handle,
                        descriptor(data),
                        (const cudnnFilterDescriptor_t)filter_handle,
                        &out_num_samples,
                        &out_k,
                        &out_nr,
                        &out_nc));

                tensor_descriptor dest_desc;
                dest_desc.set_size(out_num_samples,out_k,out_nr,out_nc);

                // Pick which forward algorithm we will use and allocate the necessary
                // workspace buffer.
                cudnnConvolutionFwdAlgo_t forward_best_algo;
                check(cudnnGetConvolutionForwardAlgorithm(
                        context(), 
                        descriptor(data),
                        (const cudnnFilterDescriptor_t)filter_handle,
                        (const cudnnConvolutionDescriptor_t)conv_handle,
                        descriptor(dest_desc),
                        CUDNN_CONVOLUTION_FWD_PREFER_FASTEST, // or CUDNN_CONVOLUTION_FWD_NO_WORKSPACE,
                        std::numeric_limits<size_t>::max(),
                        &forward_best_algo));
                forward_algo = forward_best_algo;
                check(cudnnGetConvolutionForwardWorkspaceSize( 
                        context(),
                        descriptor(data),
                        (const cudnnFilterDescriptor_t)filter_handle,
                        (const cudnnConvolutionDescriptor_t)conv_handle,
                        descriptor(dest_desc),
                        forward_best_algo,
                        &forward_workspace_size_in_bytes));
                CHECK_CUDA(cudaMalloc(&forward_workspace, forward_workspace_size_in_bytes));


                // Pick which backward data algorithm we will use and allocate the
                // necessary workspace buffer.
                cudnnConvolutionBwdDataAlgo_t backward_data_best_algo;
                check(cudnnGetConvolutionBackwardDataAlgorithm(
                        context(),
                        (const cudnnFilterDescriptor_t)filter_handle,
                        descriptor(dest_desc),
                        (const cudnnConvolutionDescriptor_t)conv_handle,
                        descriptor(data),
                        CUDNN_CONVOLUTION_BWD_DATA_PREFER_FASTEST,
                        std::numeric_limits<size_t>::max(),
                        &backward_data_best_algo));
                backward_data_algo = backward_data_best_algo;
                check(cudnnGetConvolutionBackwardDataWorkspaceSize( 
                        context(),
                        (const cudnnFilterDescriptor_t)filter_handle,
                        descriptor(dest_desc),
                        (const cudnnConvolutionDescriptor_t)conv_handle,
                        descriptor(data),
                        backward_data_best_algo,
                        &backward_data_workspace_size_in_bytes));
                CHECK_CUDA(cudaMalloc(&backward_data_workspace, backward_data_workspace_size_in_bytes));


                // Pick which backward filters algorithm we will use and allocate the
                // necessary workspace buffer.
                cudnnConvolutionBwdFilterAlgo_t backward_filters_best_algo;
                check(cudnnGetConvolutionBackwardFilterAlgorithm(
                        context(),
                        descriptor(data),
                        descriptor(dest_desc),
                        (const cudnnConvolutionDescriptor_t)conv_handle,
                        (const cudnnFilterDescriptor_t)filter_handle,
                        CUDNN_CONVOLUTION_BWD_FILTER_PREFER_FASTEST,
                        std::numeric_limits<size_t>::max(),
                        &backward_filters_best_algo));
                backward_filters_algo = backward_filters_best_algo;
                check(cudnnGetConvolutionBackwardFilterWorkspaceSize( 
                        context(),
                        descriptor(data),
                        descriptor(dest_desc),
                        (const cudnnConvolutionDescriptor_t)conv_handle,
                        (const cudnnFilterDescriptor_t)filter_handle,
                        backward_filters_best_algo,
                        &backward_filters_workspace_size_in_bytes));
                CHECK_CUDA(cudaMalloc(&backward_filters_workspace, backward_filters_workspace_size_in_bytes));
            }
            catch(...)
            {
                clear();
            }
        }

        tensor_conv::
        ~tensor_conv (
        )
        {
            clear();
        }

        void tensor_conv::operator() (
            resizable_tensor& output,
            const tensor& data,
            const tensor& filters
        )
        {
            output.set_size(out_num_samples, out_k, out_nr, out_nc);


            // TODO, remove
            DLIB_CASSERT(output.num_samples() == data.num_samples(),out_num_samples << "  " << data.num_samples());
            DLIB_CASSERT(output.k() == filters.num_samples(),"");
            DLIB_CASSERT(output.nr() == 1+(data.nr()-1)/stride_y,"");
            DLIB_CASSERT(output.nc() == 1+(data.nc()-1)/stride_x,"");

            const float alpha = 1;
            const float beta = 0;
            check(cudnnConvolutionForward(
                    context(),
                    &alpha,
                    descriptor(data),
                    data.device(),
                    (const cudnnFilterDescriptor_t)filter_handle,
                    filters.device(),
                    (const cudnnConvolutionDescriptor_t)conv_handle,
                    (cudnnConvolutionFwdAlgo_t)forward_algo,
                    forward_workspace,
                    forward_workspace_size_in_bytes,
                    &beta,
                    descriptor(output),
                    output.device()));
        }

        void tensor_conv::get_gradient_for_data (
            const tensor& gradient_input, 
            const tensor& filters,
            tensor& data_gradient
        )
        {
            const float alpha = 1;
            const float beta = 1;


            check(cudnnConvolutionBackwardData_v3(context(),
                                                  &alpha,
                                                  (const cudnnFilterDescriptor_t)filter_handle,
                                                  filters.device(),
                                                  descriptor(gradient_input),
                                                  gradient_input.device(),
                                                  (const cudnnConvolutionDescriptor_t)conv_handle,
                                                  (cudnnConvolutionBwdDataAlgo_t)backward_data_algo,
                                                  backward_data_workspace,
                                                  backward_data_workspace_size_in_bytes,
                                                  &beta,
                                                  descriptor(data_gradient),
                                                  data_gradient.device()));
        }

        void tensor_conv::
        get_gradient_for_filters (
            const tensor& gradient_input, 
            const tensor& data,
            tensor& filters_gradient
        )
        {
            const float alpha = 1;
            const float beta = 1;
            check(cudnnConvolutionBackwardFilter_v3(context(),
                                                    &alpha,
                                                    descriptor(data),
                                                    data.device(),
                                                    descriptor(gradient_input),
                                                    gradient_input.device(),
                                                    (const cudnnConvolutionDescriptor_t)conv_handle,
                                                    (cudnnConvolutionBwdFilterAlgo_t)backward_filters_algo,
                                                    backward_filters_workspace,
                                                    backward_filters_workspace_size_in_bytes,
                                                    &beta,
                                                    (const cudnnFilterDescriptor_t)filter_handle,
                                                    filters_gradient.device()));
        }

    // ------------------------------------------------------------------------------------
    // ------------------------------------------------------------------------------------

        max_pool::max_pool (
        ) : handle(nullptr),stride_y(0),stride_x(0)
        {
        }

        max_pool::~max_pool(
        )
        {
            clear();
        }

        void max_pool::
        clear(
        )
        {
            if (handle)
                cudnnDestroyPoolingDescriptor((cudnnPoolingDescriptor_t)handle);
            handle = nullptr;
            stride_y = 0;
            stride_x = 0;
        }

        void max_pool::
        setup(
            int window_height,
            int window_width,
            int stride_y_,
            int stride_x_
        )
        {
            stride_x = stride_x_;
            stride_y = stride_y_;
            cudnnPoolingDescriptor_t poolingDesc;
            check(cudnnCreatePoolingDescriptor(&poolingDesc));
            handle = poolingDesc;

            check(cudnnSetPooling2dDescriptor(poolingDesc,
                                              CUDNN_POOLING_MAX,
                                              window_height,
                                              window_width,
                                              0,0, // no padding
                                              stride_y,
                                              stride_x));
        }

        void max_pool::
        operator() (
            resizable_tensor& dest,
            const tensor& src
        )
        {
            const float alpha = 1;
            const float beta = 0;
            int outN;
            int outC;
            int outH;
            int outW;
            check(cudnnGetPooling2dForwardOutputDim((const cudnnPoolingDescriptor_t)handle,
                                                    descriptor(src),
                                                    &outN,
                                                    &outC,
                                                    &outH,
                                                    &outW));


            dest.set_size(outN,outC,outH,outW);

            DLIB_CASSERT(dest.num_samples() == src.num_samples(),"");
            DLIB_CASSERT(dest.k() == src.k(),"");
            DLIB_CASSERT(dest.nr() == src.nr()/stride_y,"");
            DLIB_CASSERT(dest.nc() == src.nc()/stride_x,"");

            check(cudnnPoolingForward(context(),
                                     (const cudnnPoolingDescriptor_t)handle,
                                     &alpha,
                                     descriptor(src),
                                     src.device(),
                                     &beta,
                                     descriptor(dest),
                                     dest.device()));
        }

        void max_pool::get_gradient(
            const tensor& gradient_input, 
            const tensor& dest,
            const tensor& src,
            tensor& grad 
        )
        {
            DLIB_CASSERT(have_same_dimensions(gradient_input,dest),"");
            DLIB_CASSERT(have_same_dimensions(src,grad),"");

            const float alpha = 1;
            const float beta = 0;
            check(cudnnPoolingBackward(context(),
                                       (const cudnnPoolingDescriptor_t)handle,
                                       &alpha,
                                       descriptor(dest),
                                       dest.device(),
                                       descriptor(gradient_input),
                                       gradient_input.device(),
                                       descriptor(src),
                                       src.device(),
                                       &beta,
                                       descriptor(grad),
                                       grad.device()));
        }

    // ------------------------------------------------------------------------------------
    // ------------------------------------------------------------------------------------

        void softmax (
            resizable_tensor& dest,
            const tensor& src
        )
        {
            dest.copy_size(src);
            if (src.size() == 0)
                return;

            const float alpha = 1;
            const float beta = 0;

            check(cudnnSoftmaxForward(context(),
                                      CUDNN_SOFTMAX_ACCURATE,
                                      CUDNN_SOFTMAX_MODE_CHANNEL,
                                      &alpha,
                                      descriptor(src),
                                      src.device(),
                                      &beta,
                                      descriptor(dest),
                                      dest.device()));
        }


        void softmax_gradient (
            tensor& grad,
            const tensor& softmaxed_data,
            const tensor& gradient_input
        )
        {
            DLIB_CASSERT(
                  have_same_dimensions(softmaxed_data,gradient_input) == true &&
                  have_same_dimensions(softmaxed_data,grad) == true , "");
            if (softmaxed_data.size() == 0)
                return;

            const float alpha = 1;
            const float beta = 1;
            check(cudnnSoftmaxBackward(context(),
                                      CUDNN_SOFTMAX_ACCURATE,
                                      CUDNN_SOFTMAX_MODE_CHANNEL,
                                      &alpha,
                                      descriptor(softmaxed_data),
                                      softmaxed_data.device(),
                                      descriptor(gradient_input),
                                      gradient_input.device(),
                                      &beta,
                                      descriptor(grad),
                                      grad.device()));
        }

    // ------------------------------------------------------------------------------------
    // ------------------------------------------------------------------------------------

        void sigmoid (
            resizable_tensor& dest,
            const tensor& src
        )
        {
            dest.copy_size(src);
            if (src.size() == 0)
                return;

            const float alpha = 1;
            const float beta = 0;
            check(cudnnActivationForward(context(),
                                         CUDNN_ACTIVATION_SIGMOID,
                                         &alpha,
                                         descriptor(src),
                                         src.device(),
                                         &beta,
                                         descriptor(dest),
                                         dest.device()));
        }

        void sigmoid_gradient (
            tensor& grad,
            const tensor& dest,
            const tensor& src,
            const tensor& gradient_input
        )
        {
            DLIB_CASSERT(
                  have_same_dimensions(src,gradient_input) == true &&
                  have_same_dimensions(src,grad) == true &&
                  have_same_dimensions(src,dest) == true , "");
            if (src.size() == 0)
                return;

            const float alpha = 1;
            const float beta = 1;
            check(cudnnActivationBackward(context(),
                                          CUDNN_ACTIVATION_SIGMOID,
                                          &alpha,
                                          descriptor(dest),
                                          dest.device(),
                                          descriptor(gradient_input),
                                          gradient_input.device(),
                                          descriptor(src),
                                          src.device(),
                                          &beta,
                                          descriptor(grad),
                                          grad.device()));
        }

    // ------------------------------------------------------------------------------------

        void relu (
            resizable_tensor& dest,
            const tensor& src
        )
        {
            dest.copy_size(src);
            if (src.size() == 0)
                return;

            const float alpha = 1;
            const float beta = 0;
            check(cudnnActivationForward(context(),
                                         CUDNN_ACTIVATION_RELU,
                                         &alpha,
                                         descriptor(src),
                                         src.device(),
                                         &beta,
                                         descriptor(dest),
                                         dest.device()));
        }

        void relu_gradient (
            tensor& grad,
            const tensor& dest,
            const tensor& src,
            const tensor& gradient_input
        )
        {
            DLIB_CASSERT(
                  have_same_dimensions(src,gradient_input) == true &&
                  have_same_dimensions(src,grad) == true &&
                  have_same_dimensions(src,dest) == true , "");
            if (src.size() == 0)
                return;

            const float alpha = 1;
            const float beta = 1;
            check(cudnnActivationBackward(context(),
                                          CUDNN_ACTIVATION_RELU,
                                          &alpha,
                                          descriptor(dest),
                                          dest.device(),
                                          descriptor(gradient_input),
                                          gradient_input.device(),
                                          descriptor(src),
                                          src.device(),
                                          &beta,
                                          descriptor(grad),
                                          grad.device()));
        }

    // ------------------------------------------------------------------------------------

        void tanh (
            resizable_tensor& dest,
            const tensor& src
        )
        {
            dest.copy_size(src);
            if (src.size() == 0)
                return;

            const float alpha = 1;
            const float beta = 0;
            check(cudnnActivationForward(context(),
                                         CUDNN_ACTIVATION_TANH,
                                         &alpha,
                                         descriptor(src),
                                         src.device(),
                                         &beta,
                                         descriptor(dest),
                                         dest.device()));
        }

        void tanh_gradient (
            tensor& grad,
            const tensor& dest,
            const tensor& src,
            const tensor& gradient_input
        )
        {
            DLIB_CASSERT(
                  have_same_dimensions(src,gradient_input) == true &&
                  have_same_dimensions(src,grad) == true &&
                  have_same_dimensions(src,dest) == true , "");
            if (src.size() == 0)
                return;

            const float alpha = 1;
            const float beta = 1;
            check(cudnnActivationBackward(context(),
                                          CUDNN_ACTIVATION_TANH,
                                          &alpha,
                                          descriptor(dest),
                                          dest.device(),
                                          descriptor(gradient_input),
                                          gradient_input.device(),
                                          descriptor(src),
                                          src.device(),
                                          &beta,
                                          descriptor(grad),
                                          grad.device()));
        }

    // ------------------------------------------------------------------------------------

    } 
}

#endif // DLIB_USE_CUDA

#endif // DLIB_DNN_CuDNN_CPP_


