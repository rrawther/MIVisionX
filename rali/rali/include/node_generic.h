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

#pragma once
#include <set>
#include <memory>
#include "graph.h"
#include "image.h"
#include "tensor.h"

template <typename T>
class NodeGeneric: public Node
{
public:
    NodeGeneric(const std::vector<T *> &inputs, const std::vector<T *> &outputs, size_t batch_size):
            _inputs(inputs),
            _outputs(outputs),
            _batch_size(batch_size)
    {
    }
    explicit NodeGeneric(const std::vector<T *> &inputs, const std::vector<T *> &outputs):
            NodeGeneric(inputs, outputs, 0) {}

    void create(std::shared_ptr<Graph> graph)
    {
        if(_outputs.empty() || _inputs.empty())
            THROW("Uninitialized input/output images to the node")

        _graph = graph;
        if (!_batch_size) _batch_size = static_cast< T *>(_outputs[0])->info().batch_size();

        if(!_inputs.empty())
        {
            vx_status width_status, height_status;
            unsigned width, height;
            width = static_cast<T *>(_inputs[0])->info().width();
            height = static_cast<T *>(_inputs[0])->info().height_single();
            std::vector<uint32_t> roi_width(_batch_size, width);
            std::vector<uint32_t> roi_height(_batch_size, height);
            _src_roi_width = vxCreateArray(vxGetContext((vx_reference) _graph->get()), VX_TYPE_UINT32, _batch_size);
            _src_roi_height = vxCreateArray(vxGetContext((vx_reference) _graph->get()), VX_TYPE_UINT32, _batch_size);
            width_status = vxAddArrayItems(_src_roi_width, _batch_size, roi_width.data(), sizeof(vx_uint32));
            height_status = vxAddArrayItems(_src_roi_height, _batch_size, roi_height.data(), sizeof(vx_uint32));
            if (width_status != 0 || height_status != 0)
                THROW(" NodeGeneric:: vxAddArrayItems failed : " + TOSTR(_batch_size) + " " + TOSTR(width_status) + "  " + TOSTR(height_status))
        }
        create_node();
    }

    std::vector<T *> input() { return _inputs; };
    std::vector<T *> output() { return _outputs; };

protected:
    virtual void create_node() = 0;
    virtual void update_node() = 0;
    virtual void update_src_roi()
    {
        vx_status width_status, height_status;
        width_status = vxCopyArrayRange((vx_array) _src_roi_width, 0, _batch_size, sizeof(vx_uint32), static_cast<T *>(_inputs[0])->info().get_roi_width(),
                                        VX_WRITE_ONLY, VX_MEMORY_TYPE_HOST);
        height_status = vxCopyArrayRange((vx_array) _src_roi_height, 0, _batch_size, sizeof(vx_uint32), static_cast<T *>(_inputs[0])->info().get_roi_height(),
                                         VX_WRITE_ONLY, VX_MEMORY_TYPE_HOST);
        if(width_status != 0 || height_status != 0)
            THROW(" Failed calling vxCopyArrayRange for width / height status : "+ TOSTR(width_status) + " / "+ TOSTR(height_status))
    }

    std::vector<T *> _inputs;         // image or tensor
    std::vector<T *> _outputs;        // image or tensor
    size_t _batch_size;
};
