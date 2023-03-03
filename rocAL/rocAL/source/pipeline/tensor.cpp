
/*
Copyright (c) 2019 - 2022 Advanced Micro Devices, Inc. All rights reserved.

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
#if !ENABLE_HIP
#include <CL/cl.h>
#endif
#include <vx_ext_amd.h>

#include <cstring>
#include <stdexcept>

#include "commons.h"
#include "tensor.h"

vx_enum vx_mem_type(RocalMemType mem) {
    switch (mem) {
        case RocalMemType::OCL:
            return VX_MEMORY_TYPE_OPENCL;
        case RocalMemType::HOST:
            return VX_MEMORY_TYPE_HOST;
        case RocalMemType::HIP:
            return VX_MEMORY_TYPE_HIP;
        default:
            throw std::runtime_error("Memory type not valid");
    }
}

vx_size tensor_data_size(RocalTensorDataType data_type) {
    switch (data_type) {
        case RocalTensorDataType::FP32:
            return sizeof(vx_float32);
        case RocalTensorDataType::FP16:
#if defined(AMD_FP16_SUPPORT)
            return sizeof(vx_float16);
#else
            THROW("FLOAT16 type tensor not supported")
            return 0;
#endif
        case RocalTensorDataType::UINT8:
            return sizeof(vx_uint8);
        case RocalTensorDataType::UINT32:
            return sizeof(vx_uint32);
        case RocalTensorDataType::INT32:
            return sizeof(vx_int32);
        default:
            throw std::runtime_error("tensor data_type not valid");
    }
}

//! Converts the Rocal data_type to OpenVX
vx_enum interpret_tensor_data_type(RocalTensorDataType data_type) {
    switch (data_type) {
        case RocalTensorDataType::FP32:
            return VX_TYPE_FLOAT32;
        case RocalTensorDataType::FP16:
            return VX_TYPE_FLOAT16;
        case RocalTensorDataType::UINT8:
            return VX_TYPE_UINT8;
        default:
            THROW("Unsupported Tensor type " + TOSTR(data_type))
    }
}

void allocate_host_or_pinned_mem(void **ptr, size_t size, RocalMemType mem_type) {
    if (mem_type == RocalMemType::HIP) {
#if ENABLE_HIP
        hipError_t err = hipHostMalloc((void **)ptr, size, hipHostMallocDefault);
        if(err != hipSuccess || !*ptr)
            THROW("hipHostMalloc of size " + TOSTR(size) + " failed " + TOSTR(err))
        err = hipMemset((void *)*ptr, 0, size);
        if(err != hipSuccess)
            THROW("hipMemset of size " + TOSTR(size) + " failed " + TOSTR(err))
#endif
    } else {
        *ptr = (void *)malloc(size);
        memset((void *)*ptr, 0, size);
    }
}

bool operator==(const rocalTensorInfo &rhs, const rocalTensorInfo &lhs) {
    return (rhs.dims() == lhs.dims() &&
            rhs.mem_type() == lhs.mem_type() &&
            rhs.data_type() == lhs.data_type() &&
            rhs.color_format() == lhs.color_format() &&
            rhs.layout() == lhs.layout());
}


void rocalTensorInfo::reset_tensor_roi_buffers() {
    if(!_roi_buf)
        allocate_host_or_pinned_mem(&_roi_buf, _batch_size * 4 * sizeof(unsigned), _mem_type);
    if (_is_image) {
        auto roi = get_roi();
        for (unsigned i = 0; i < _batch_size; i++) {
            roi[i].x2 = _max_shape.at(0);
            roi[i].y2 = _max_shape.at(1);
        }
    } else {
        // TODO - For other tensor types
    }
}

rocalTensorInfo::rocalTensorInfo()
    : _type(Type::UNKNOWN),
      _num_of_dims(0),
      _dims({}),
      _mem_type(RocalMemType::HOST),
      _data_type(RocalTensorDataType::FP32) {}

rocalTensorInfo::rocalTensorInfo(std::vector<size_t> dims,
                                 RocalMemType mem_type,
                                 RocalTensorDataType data_type)
    : _type(Type::UNKNOWN),
      _dims(dims),
      _mem_type(mem_type),
      _data_type(data_type) {
    _batch_size = dims.at(0);
    _num_of_dims = dims.size();
    _data_size = tensor_data_size(data_type);
    for (unsigned i = 0; i < _num_of_dims; i++) _data_size *= dims.at(i);

    if (_num_of_dims <= 3) _is_image = false;
}

rocalTensorInfo::rocalTensorInfo(const rocalTensorInfo &other) {
    _type = other._type;
    _num_of_dims = other._num_of_dims;
    _dims = other._dims;
    _batch_size = other._batch_size;
    _mem_type = other._mem_type;
    _roi_type = other._roi_type;
    _data_type = other._data_type;
    _layout = other._layout;
    _color_format = other._color_format;
    _data_type_size = other._data_type_size;
    _data_size = other._data_size;
    _max_shape = other._max_shape;
    _is_image = other._is_image;
    _is_metadata = other._is_metadata;
    _channels = other._channels;
    if(!other.is_metadata()) {  // For Metadata ROI buffer is not required
        allocate_host_or_pinned_mem(&_roi_buf, _batch_size * 4 * sizeof(unsigned), _mem_type);
        memcpy((void *)_roi_buf, (const void *)other.get_roi(), _batch_size * 4 * sizeof(unsigned));
    }
}

rocalTensorInfo::~rocalTensorInfo() {
    if(!_is_metadata) {
        if(_mem_type == RocalMemType::HIP) {
#if ENABLE_HIP
            if(_roi_buf) {
                hipError_t err = hipHostFree(_roi_buf);
                if (err != hipSuccess)
                    ERR("hipHostFree failed " + TOSTR(err));
            }
#endif
        } else {
            if(_roi_buf) free(_roi_buf);
        }
        _roi_buf = nullptr;
    } 
}

void rocalTensor::update_tensor_roi(const std::vector<uint32_t> &width,
                                    const std::vector<uint32_t> &height) {
    if (_info.is_image()) {
        auto max_shape = _info.max_shape();
        unsigned max_width = max_shape.at(0);
        unsigned max_height = max_shape.at(1);

        if (width.size() != height.size())
            THROW("Batch size of Tensor height and width info does not match")

        if (width.size() != _info.batch_size())
            THROW("The batch size of actual Tensor height and width different from Tensor batch size " + TOSTR(width.size()) + " != " + TOSTR(info().batch_size()))

        for (unsigned i = 0; i < _info.batch_size(); i++) {
            if (width[i] > max_width) {
                WRN("Given ROI width is larger than buffer width for tensor[" + TOSTR(i) + "] " + TOSTR(width[i]) + " > " + TOSTR(max_width))
                _info.get_roi()[i].x2 = max_width;
            } else {
                _info.get_roi()[i].x2 = width[i];
            }
            if (height[i] > max_height) {
                WRN("Given ROI height is larger than buffer height for tensor[" + TOSTR(i) + "] " + TOSTR(height[i]) + " > " + TOSTR(max_height))
                _info.get_roi()[i].y2 = max_height;
            } else {
                _info.get_roi()[i].y2 = height[i];
            }
        }
    }
}

rocalTensor::~rocalTensor() {
    _mem_handle = nullptr;
    if (_vx_handle) vxReleaseTensor(&_vx_handle);
}

rocalTensor::rocalTensor(const rocalTensorInfo &tensor_info)
    : _info(tensor_info) {
    _info._type = rocalTensorInfo::Type::UNKNOWN;
    _mem_handle = nullptr;
}

int rocalTensor::create_virtual(vx_context context, vx_graph graph) {
    if (_vx_handle) {
        WRN("Tensor object create method is already called ")
        return -1;
    }

    _context = context;
    _vx_handle = vxCreateVirtualTensor(graph, _info.num_of_dims(), _info.dims().data(), interpret_tensor_data_type(_info.data_type()), 0);
    vx_status status;
    if ((status = vxGetStatus((vx_reference)_vx_handle)) != VX_SUCCESS)
        THROW("Error: vxCreateVirtualTensor(input:[" + TOSTR(_info.max_shape().at(0)) + "W" + TOSTR(_info.max_shape().at(1)) + "H" + "]): failed " + TOSTR(status))

    _info._type = rocalTensorInfo::Type::VIRTUAL;
    return 0;
}

int rocalTensor::create_from_handle(vx_context context) {
    if (_vx_handle) {
        WRN("Tensor object create method is already called ")
        return -1;
    }

    _context = context;
    vx_enum tensor_data_type = interpret_tensor_data_type(_info.data_type());
    unsigned num_of_dims = _info.num_of_dims();
    vx_size stride[num_of_dims];
    void *ptr[1] = {nullptr};

    stride[0] = tensor_data_size(_info.data_type());
    for (unsigned i = 1; i < num_of_dims; i++)
        stride[i] = stride[i - 1] * _info.dims().at(i - 1);

    _vx_handle = vxCreateTensorFromHandle(_context, _info.num_of_dims(), _info.dims().data(), tensor_data_type, 0, stride, ptr, vx_mem_type(_info._mem_type));
    vx_status status;
    if ((status = vxGetStatus((vx_reference)_vx_handle)) != VX_SUCCESS)
        THROW("Error: vxCreateTensorFromHandle(input: failed " + TOSTR(status))
    _info._type = rocalTensorInfo::Type::HANDLE;
    return 0;
}

int rocalTensor::create(vx_context context) {
    if (_vx_handle) {
        WRN("Tensor object create method is already called ")
        return -1;
    }

    _context = context;
    vx_status status;
    vx_enum tensor_data_type = interpret_tensor_data_type(_info.data_type());
    _vx_handle = vxCreateTensor(context, _info.num_of_dims(), _info.dims().data(), tensor_data_type, 0);
    if ((status = vxGetStatus((vx_reference)_vx_handle)) != VX_SUCCESS)
        THROW("Error: vxCreateTensor(input: failed " + TOSTR(status))
    _info._type = rocalTensorInfo::Type::REGULAR;
    return 0;
}

#if !ENABLE_HIP
unsigned rocalTensor::copy_data(cl_command_queue queue, unsigned char *user_buffer, bool sync) {
    if (_info._type != rocalTensorInfo::Type::HANDLE) return 0;

    if (_info._mem_type == RocalMemType::OCL) {
        cl_int status;
        if ((status = clEnqueueReadBuffer(queue,
                                          (cl_mem)_mem_handle, 
                                          sync ? (CL_TRUE) : CL_FALSE, 0,
                                          _info.data_size(), 
                                          user_buffer, 
                                          0, nullptr, nullptr)) != CL_SUCCESS) {
            THROW("clEnqueueReadBuffer failed: " + TOSTR(status))
        }
    } else {
        memcpy(user_buffer, _mem_handle, _info.data_size());
    }
    return 0;
}
#else
unsigned rocalTensor::copy_data(hipStream_t stream, void *host_memory, bool sync) {
    if (_info._type != rocalTensorInfo::Type::HANDLE) return 0;

    if (_info._mem_type == RocalMemType::HIP) {
        // copy from device to host
        hipError_t status;
        if ((status = hipMemcpyDtoHAsync((void *)host_memory, _mem_handle, _info.data_size(), stream)))
            THROW("copy_data::hipMemcpyDtoH failed: " + TOSTR(status))
        if (sync) {
            if ((status = hipStreamSynchronize(stream)))
                THROW("copy_data::hipStreamSynchronize failed: " + TOSTR(status))
        }
    } else {
        memcpy(host_memory, _mem_handle, _info.data_size());
    }
    return 0;
}
#endif

unsigned rocalTensor::copy_data(void *user_buffer) {
    if (_info._type != rocalTensorInfo::Type::HANDLE) return 0;

#if ENABLE_HIP
    if (_info._mem_type == RocalMemType::HIP) {
        // copy from device to host
        hipError_t status;
        if ((status = hipMemcpyDtoH((void *)user_buffer, _mem_handle, _info.data_size())))
            THROW("copy_data::hipMemcpyDtoH failed: " + TOSTR(status))
    } else
#endif
    {
        // copy from host to host
        memcpy(user_buffer, _mem_handle, _info.data_size());
    }
    return 0;
}

int rocalTensor::swap_handle(void *handle) {
    vx_status status;
    if ((status = vxSwapTensorHandle(_vx_handle, handle, nullptr)) != VX_SUCCESS) {
        ERR("Swap handles failed for tensor" + TOSTR(status));
        return -1;
    }

    // Updating the buffer pointer as well,
    // user might want to copy directly using it
    _mem_handle = handle;
    return 0;
}
