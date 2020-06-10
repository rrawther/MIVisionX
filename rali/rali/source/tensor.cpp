/*
Copyright (c) 2019 - 2020 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <cstdio>
#include <CL/cl.h>
#include <stdexcept>
#include <vx_ext_amd.h>
#include <cstring>
#include "commons.h"
#include "tensor.h"

vx_size tensor_data_size(RaliTensorDataType data_type)
{
    switch(data_type)
    {
        case RaliTensorDataType::FP32:
        {
            return sizeof(vx_float32);
        }
        break;
        case RaliTensorDataType::FP16:
        {
            return sizeof(vx_int16);
        }
        break;
        case RaliTensorDataType::INT8:
        {
            return sizeof(vx_int8);
        }
        break;
        default:
            throw std::runtime_error("tensor data_type not valid");
    }
}

bool operator==(const TensorInfo& rhs, const TensorInfo& lhs)
{
    return (rhs.width() == lhs.width() &&
            rhs.batch_size() == lhs.batch_size() &&
            rhs.mem_type() == lhs.mem_type() &&
            rhs.data_type() == lhs.data_type() &&
            rhs.channels() == lhs.channels() &&
            rhs.format() == lhs.format());
}

uint32_t * TensorInfo::get_roi_width() const
{
    return _roi_width->data();
}

uint32_t * TensorInfo::get_roi_height() const
{
    return _roi_height->data();
}

const std::vector<uint32_t>& TensorInfo::get_roi_width_vec() const
{
    return *_roi_width;
}

const std::vector<uint32_t>& TensorInfo::get_roi_height_vec() const
{
    return *_roi_height;
}


unsigned TensorInfo::get_roi_width(int batch_idx) const
{
    if((unsigned)batch_idx >= _roi_width->size())
        THROW("Accessing roi width out of tensor range")
    if(!_roi_width->at(batch_idx))
        THROW("Accessing uninitialized int parameter associated with image width")
    return _roi_width->at(batch_idx);
}

unsigned TensorInfo::get_roi_height(int batch_idx) const
{
    if((unsigned)batch_idx >= _roi_height->size())
        THROW("AAccessing roi width out of tensor range")
    if(!_roi_height->at(batch_idx))
        THROW("Accessing uninitialized int parameter associated with image height")
    return _roi_height->at(batch_idx);
}
void
TensorInfo::reallocate_tensor_roi_buffers()
{
    _roi_height = std::make_shared<std::vector<uint32_t>>(_batch_size);
    _roi_width = std::make_shared<std::vector<uint32_t>>(_batch_size);
    for(unsigned i = 0; i < _batch_size; i++)
    {
        _roi_height->at(i) = height();
        _roi_width->at(i) = width();
    }
}
TensorInfo::TensorInfo():
        _type(Type::UNKNOWN),
        _width(0),
        _height(0),
        _batch_size(1),
        _channels(1),
        _data_size(0),
        _mem_type(RaliMemType::HOST),
        _data_type(RaliTensorDataType::FP32){}

TensorInfo::TensorInfo(
    unsigned width_,
    unsigned height_,
    unsigned batches_,
    unsigned channels_,
    RaliMemType mem_type_,
    RaliTensorDataType data_type,
    RaliTensorFormat  tensor_format):
        _type(Type::UNKNOWN),
        _width(width_),
        _height(height_),
        _batch_size(batches_),
        _channels(channels_),
        _mem_type(mem_type_),
        _data_type(data_type),
        _format(tensor_format)
{
    vx_size pix_size  = tensor_data_size(data_type);
    unsigned alignpixels = TENSOR_WIDTH_ALIGNMENT;
    if (alignpixels == 0)
        _stride = width_*pix_size;
    else
        _stride = ((width_ + alignpixels - 1) & ~(alignpixels - 1))*pix_size;
    _data_size = _stride * height_ * _batch_size * _channels;

    // initializing each Tensor dimension in the batch with the maximum Tensor size, they'll get updated later during the runtime
    reallocate_tensor_roi_buffers();
}

void Tensor::update_tensor_roi(const std::vector<uint32_t> &width, const std::vector<uint32_t> &height)
{
    if(width.size() != height.size())
        THROW("Batch size of Tensor height and width info does not match")

    if(width.size() != info().batch_size())
        THROW("The batch size of actual Tensor height and width different from Tensor batch size "+ TOSTR(width.size())+ " != " +  TOSTR(info().batch_size()))
    if(! _info._roi_width || !_info._roi_height)
        THROW("ROI width or ROI height vector not created")
    for(unsigned i = 0; i < info().batch_size(); i++)
    {

        if (width[i] > _info.width())
        {
            ERR("Given ROI width is larger than buffer width for tensor[" + TOSTR(i) + "] " + TOSTR(width[i]) + " > " + TOSTR(_info.width()))
            _info._roi_width->at(i) = _info.width();
        }
        else
        {
            _info._roi_width->at(i) = width[i];
        }

        if(height[i] > _info.height())
        {
            ERR("Given ROI height is larger than buffer with for tensor[" + TOSTR(i) + "] " + TOSTR(height[i]) +" > " + TOSTR(_info.height()))
            _info._roi_height->at(i) = _info.height();
        }
        else
        {
            _info._roi_height->at(i)= height[i];
        }

    }
}

Tensor::~Tensor()
{  
    vxReleaseTensor(&_vx_handle);
}

//! Converts the Rali data_type to OpenVX
vx_enum interpret_tensor_data_type(RaliTensorDataType data_type)
{
    switch(data_type){
        case RaliTensorDataType::FP32:
            return VX_TYPE_FLOAT32;
        case RaliTensorDataType::FP16:
            return VX_TYPE_FLOAT16;
        case RaliTensorDataType::INT8:
            return VX_TYPE_INT8;
        default:
            THROW("Unsupported Tensor type "+ TOSTR(data_type))
    }
}

Tensor::Tensor(const TensorInfo& tensor_info):_info(tensor_info)
{
    _info._type = TensorInfo::Type::UNKNOWN;
    _mem_handle = nullptr;
}

int Tensor::create_virtual(vx_context context, vx_graph graph)
{
    if(_vx_handle)
        return -1;

    _context = context;

    // create a virtual Tensor as the output Tensor for this node
    vx_size dims[4] = {_info.width(), _info.height(), _info.channels(), _info.batch_size() };
    _vx_handle = vxCreateVirtualTensor(graph, 4, dims, interpret_tensor_data_type(_info.data_type()), 0);
    vx_status status;
    if((status = vxGetStatus((vx_reference)_vx_handle)) != VX_SUCCESS)
        THROW("Error: vxCreateVirtualTensor(input:[" + TOSTR(_info.width()) + "W" + TOSTR(_info.height()) + "H" + "]): failed " + TOSTR(status))

    _info._type = TensorInfo::Type::VIRTUAL;
    return 0;                                             
}

int Tensor::create_from_handle(vx_context context)
{
    if(_vx_handle)
    {
        WRN("Tensor object create method is already called ")
        return -1;
    }
    if(_info.height() == 0 || _info.width() == 0 || _info.channels() == 0 || _info.batch_size() == 0)
        THROW( "Invalid tensor dimension" + TOSTR(_info.width()) +  " x " + TOSTR(_info.height()) + " x " + TOSTR(_info.channels()) +" x " + TOSTR(_info.batch_size()));

    _context = context;
    vx_enum tens_data_type =  interpret_tensor_data_type(_info.data_type());
    vx_size dims[4] = { (vx_size)_info.width(), (vx_size)_info.height(), (vx_size)_info.channels(), (vx_size)_info.batch_size() };
    vx_size stride[4];// = {tens_data_size, tens_data_size*_info.width(), tens_data_size*_info.width()*.height(), tens_data_size*_info.width()*.height()*};

    void *ptr[1] = { nullptr };
    stride[0] = tensor_data_size(_info.data_type());

    vx_uint32 alignpixels = TENSOR_WIDTH_ALIGNMENT;

    if (alignpixels == 0)
        stride[1] = _info.width() * stride[0];
    else
        stride[1] = ((_info.width() + alignpixels - 1) & ~(alignpixels - 1))*stride[0];
    stride[2] = _info.height() * stride[1];
    stride[3] = _info.channels() * stride[2];

    vx_status status;
    _vx_handle = vxCreateTensorFromHandle(context, 4, dims, tens_data_type , 0, stride, ptr, vx_mem_type(_info._mem_type));
    if((status = vxGetStatus((vx_reference)_vx_handle)) != VX_SUCCESS)
        THROW("Error: vxCreateTensorFromHandle(input:[" + TOSTR(_info.width()) + "x" + TOSTR(_info.height()) + "]): failed " + TOSTR(status))

    _info._type = TensorInfo::Type::HANDLE;
    _info._data_size = stride[3]*_info.batch_size();
    return 0;
}
int Tensor::create(vx_context context)
{
    if(_vx_handle)
        return -1;

    _context = context;

    vx_status status;
    vx_size dims[4] = { (vx_size)_info.width(), (vx_size)_info.height(), (vx_size)_info.channels(), (vx_size)_info.batch_size() };
    vx_enum tens_data_type =  interpret_tensor_data_type(_info.data_type());
    _vx_handle = vxCreateTensor(context, 4, dims, tens_data_type, 0);
    if((status = vxGetStatus((vx_reference)_vx_handle)) != VX_SUCCESS)
        THROW("Error: vxCreateTensor(input:[" + TOSTR(_info.width()) + "x" + TOSTR(_info.height()) + "x" + TOSTR(_info.channels()) +"]): failed " + TOSTR(status))
    _info._type = TensorInfo::Type::REGULAR;
    return 0;
}

unsigned Tensor::copy_data(cl_command_queue queue, unsigned char* user_buffer, bool sync)
{
    if(_info._type != TensorInfo::Type::HANDLE)
        return 0;

    unsigned size = _info.stride() * _info.height() * _info.channels() * _info.batch_size();

    if(_info._mem_type == RaliMemType::OCL)
    {

        cl_int status;
        if((status = clEnqueueReadBuffer(queue,
                                         (cl_mem) _mem_handle,
                                         sync?(CL_TRUE):CL_FALSE,
                                         0,
                                         size,
                                         user_buffer,
                                         0 , nullptr, nullptr)) != CL_SUCCESS)
            THROW("clEnqueueReadBuffer failed: " + TOSTR(status))

    } else
    {
        memcpy(user_buffer, _mem_handle, size);
    }
    return size;
}
unsigned Tensor::copy_data(cl_command_queue queue, cl_mem user_buffer, bool sync)
{
    return 0;
}

int Tensor::swap_handle(void* handle)
{
    vx_status status;
    void*  ptr_in[] = {handle};
    if((status= vxSwapTensorHandle(_vx_handle, ptr_in, nullptr)) != VX_SUCCESS)
    {
        ERR("Swap handles failed for tensor"+TOSTR(status));
        return -1;
    }

    // Updating the buffer pointer as well,
    // user might want to copy directly using it
    _mem_handle = handle;
    return 0;
}