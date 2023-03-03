
/*
MIT License
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

#ifndef ROCAL_API_CUSTOM_H
#define ROCAL_API_CUSTOM_H

#include "rocal_api_types.h"

/**
 * @brief Load plugin OpenVX extension library module
 * @param [in] module_name name of the extension library module, e.g. "amd_custom"
 * @param [in] global_symbols if true, the library is loaded with RTLD_GLOBAL flag or equivalent
*                                 otherwise, RTLD_LOCAL is used (ignored now)
  * @returns RocalStatus
  */
extern "C"  RocalStatus  ROCAL_API_CALL rocalLoadModule(RocalContext rocal_context, const char *module_name, bool global_symbols = false);

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

extern "C" RocalImage  ROCAL_API_CALL
rocalCustom(
        RocalContext p_context,
        RocalTensor p_input,
        bool is_output,
        const char *fun_name,
        RocalTensor custom_param,
        size_t custom_param_size,
        RocalTensorLayout rocal_tensor_output_layout = RocalTensorLayout::ROCAL_NHWC,
        RocalTensorDataType rocal_tensor_output_datatype = RocalTensorOutputType::U8);


#endif