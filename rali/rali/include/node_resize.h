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
#include "node.h"
#include "node_generic.h"
#include <vx_ext_rpp.h>
#include <graph.h>
#include "exception.h"

template <typename T>
class ResizeNode : public NodeGeneric<T>
{
public:
    ResizeNode(const std::vector<T *> &inputs, const std::vector<T *> &outputs):
            NodeGeneric<T>(inputs, outputs) {}
    ResizeNode() = delete;
protected:
    void create_node()  override
    {
        return ResizeNode<T>::create_node(this);
    }
   // override;
    void update_node() {}
private:
    vx_array  _dst_roi_width , _dst_roi_height ;
};

template<>
void ResizeNode<Image>::create_node()
{
    if(this->_node)
        return;

    unsigned owidth, oheight;
    owidth = static_cast<Image *>(this->_outputs[0])->info().width();
    oheight = static_cast<Image *>(this->_outputs[0])->info().height_single();
    std::vector<uint32_t> dst_roi_width(this->_batch_size, owidth);
    std::vector<uint32_t> dst_roi_height(this->_batch_size, oheight);

    _dst_roi_width = vxCreateArray(vxGetContext((vx_reference)this->_graph->get()), VX_TYPE_UINT32, this->_batch_size);
    _dst_roi_height = vxCreateArray(vxGetContext((vx_reference)this->_graph->get()), VX_TYPE_UINT32, this->_batch_size);

    vx_status width_status, height_status;

    width_status = vxAddArrayItems(_dst_roi_width, this->_batch_size, dst_roi_width.data(), sizeof(vx_uint32));
    height_status = vxAddArrayItems(_dst_roi_height, this->_batch_size, dst_roi_height.data(), sizeof(vx_uint32));
    if(width_status != 0 || height_status != 0)
        THROW(" vxAddArrayItems failed in the resize (vxExtrppNode_ResizebatchPD) node: "+ TOSTR(width_status) + "  "+ TOSTR(height_status))

    this->_node = vxExtrppNode_ResizebatchPD(this->_graph->get(), dynamic_cast<Image *>(this->_inputs[0])->handle(), this->_src_roi_width, this->_src_roi_height,
                                             dynamic_cast<Image *>(this->_outputs[0])->handle(), _dst_roi_width, _dst_roi_height, this->_batch_size);
    vx_status status;
    if((status = vxGetStatus((vx_reference)this->_node)) != VX_SUCCESS)
        THROW("Adding the resize (vxExtrppNode_ResizebatchPD) node failed: "+ TOSTR(status))

}

template<>
inline void ResizeNode<Tensor>::create_node() {
    if(this->_node)
        return;

    unsigned owidth, oheight;
    owidth = static_cast<Tensor *>(this->_outputs[0])->info().width();
    oheight = static_cast<Tensor *>(this->_outputs[0])->info().height();
    std::vector<uint32_t> dst_roi_width(this->_batch_size, owidth);
    std::vector<uint32_t> dst_roi_height(this->_batch_size, oheight);

    _dst_roi_width = vxCreateArray(vxGetContext((vx_reference)this->_graph->get()), VX_TYPE_UINT32, this->_batch_size);
    _dst_roi_height = vxCreateArray(vxGetContext((vx_reference)this->_graph->get()), VX_TYPE_UINT32, this->_batch_size);

    vx_status width_status, height_status;

    width_status = vxAddArrayItems(_dst_roi_width, this->_batch_size, dst_roi_width.data(), sizeof(vx_uint32));
    height_status = vxAddArrayItems(_dst_roi_height, this->_batch_size, dst_roi_height.data(), sizeof(vx_uint32));
    if(width_status != 0 || height_status != 0)
        THROW(" vxAddArrayItems failed in the resize (vxExtrppNode_ResizebatchPD) node: "+ TOSTR(width_status) + "  "+ TOSTR(height_status))

    // todo:: call vxExtrppNode_ResizeTensor node (or equivalent)

    vx_status status;
    if((status = vxGetStatus((vx_reference)this->_node)) != VX_SUCCESS)
        THROW("Adding the resize (vxExtrppNode_ResizebatchPD) node failed: "+ TOSTR(status))
}
