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

#include <string>
#include <exception>
#include "commons.h"
#include "context.h"
#include "rocal_api.h"
#include "tensor.h"
#include "node_custom.h"

RocalStatus  ROCAL_API_CALL rocalLoadModule(RocalContext p_context, const char *module_name, bool global_symbols) {

    auto context = static_cast<Context*>(p_context);
    try
    {
        context->master_graph->load_module(module_name, global_symbols);
    }
    catch(const std::exception& e)
    {
        context->capture_error(e.what());
        ERR(e.what())
        return ROCAL_RUNTIME_ERROR;
    }
    return ROCAL_OK;
}

/// 
/// \param context
/// \param input
/// \param is_output
/// \param fun_name custom function name implemented in module (match the exact definition:: case sensitive)
/// \param custom_param custom_param buffer.
/// \param custom_param_size The size in bytes of custom_param.
/// \param rocal_tensor_output_layout The layout of input/output tensor.
/// \param rocal_tensor_output_datatype The data_type of input/output tensor.
/// \return

RocalImage  ROCAL_API_CALL
rocalCustom(
        RocalContext p_context,
        RocalTensor p_input,
        bool is_output,
        const char *fun_name,
        void *custom_param,
        size_t custom_param_size,
        RocalTensorLayout rocal_tensor_output_layout,
        RocalTensorDataType rocal_tensor_output_datatype) {

    rocalTensor* output = nullptr;
    if ((p_context == nullptr) || (p_input == nullptr)) {
        ERR("Invalid ROCAL context or invalid input image")
        return output;
    }
    auto context = static_cast<Context*>(p_context);
    auto input = static_cast<rocalTensor*>(p_input);    // todo:: change it to tensor
    try
    {
        // calculate output dims from input tensor dims
        rocalTensorInfo output_info;
        output_info.set_data_type(rocal_tensor_output_datatype);
        std::vector<size_t> output_dims = input->info().dims();
        output_info.set_dims(output_dims);
        output_info.set_tensor_layout((RocalTensorlayout)rocal_tensor_output_layout);

        output = context->master_graph->create_tensor(output_info, is_output);

        std::shared_ptr<CustomNode> custom_node =  context->master_graph->add_node<CustomNode>({input}, {output});
        custom_node->init(fun_name, context->affinity, custom_param, custom_param_size);
    }
    catch(const std::exception& e)
    {
        context->capture_error(e.what());
        ERR(e.what())
    }
    return output;
}
