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

#pragma once
#include "node.h"

class CustomNode : public NodeTensor
{
public:
    explicit CustomNode(const std::vector<rocalTensor *> &inputs, const std::vector<rocalTensor *> &outputs);
    ~CustomNode();

    void init(const char *fun_name, RocalAffinity affinity, const void *custom_params, size_t custom_params_size);

protected:
    void update_node() override;
    void create_node() override;
private:
    // todo:: to be removed when we switch to full tensor pipeline
    vx_tensor _input_tensor;   
    vx_uint32 _custom_func = 0;
    vx_uint32 _backend = 0;
    vx_array _custom_params_array;

    std::map<std::string, int> _func_map = {{"Copy", 0}};   // default value
};
