/*
Copyright (c) 2019 - 2023 Advanced Micro Devices, Inc. All rights reserved.

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

#include <vx_amd_custom.h>
#include "node_custom.h"
#include "exception.h"

CustomNode::CustomNode(const std::vector<rocalTensor *> &inputs, const std::vector<rocalTensor *> &outputs) :
        NodeTensor(inputs, outputs)
{
  _custom_params_array = nullptr;
}

CustomNode::~CustomNode()
{
  if (_custom_params_array) {
    vxReleaseArray(&_custom_params_array);
  }
}

void CustomNode::create_node()
{
    if(_node)
        return;
    
    _node = vxCustomLayer(_graph->get(), _inputs[0]->handle(), _custom_func, _backend, _custom_params_array, _outputs[0]->handle());

    vx_status status;
    if((status = vxGetStatus((vx_reference)_node)) != VX_SUCCESS)
        THROW("Adding the copy (vxCopyNode) node failed: "+ TOSTR(status))

}

void CustomNode::init(const char *fun_name, RocalAffinity affinity, const void *custom_params, size_t custom_params_size)
{
    if (_func_map.find(fun_name) == _func_map.end()) {
      // function not found in the map; add it to the map
      _func_map.insert(std::pair<std::string, int>(fun_name, (std::prev(_func_map.end())->second+1)));      
    }
    _custom_func = _func_map[fun_name];
    // create and copy the custom parameter tensor
    _custom_params_array = vxCreateArray(vxGetContext((vx_reference)_graph->get()), VX_TYPE_UINT8, custom_params_size);
    vxAddArrayItems(_custom_params_array, custom_params_size, custom_params, sizeof(VX_TYPE_UINT8));
    _backend = (affinity == RocalAffinity::GPU);

#if 0 //todo:: change it to tensor
    vx_status status;
    vx_size dims[2] = {_batch_size, custom_params_size/_batch_size};
    vx_size stride[4];
    stride[0] = 1;      // parameters are bytes
    stride[1] = dims[1];
    _custom_params_tensor = vxCreateTensorFromHandle(_context, 2, dims, VX_TYPE_UINT8, 0, stride, custom_params, VX_MEMORY_TYPE_HOST);
    if ((status = vxGetStatus((vx_reference)_vx_handle)) != VX_SUCCESS)
        THROW("Error: vxCreateTensorFromHandle(custom_params: failed " + TOSTR(status))
#endif        
}


void CustomNode::update_node()
{
    //todo;;
}