# Copyright (c) 2018 - 2023 Advanced Micro Devices, Inc. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

import rocal_pybind as b
import amd.rocal.types as types
from amd.rocal.pipeline import Pipeline

def load_module(module_path: str, global_symbols: bool = False):
    """Loads a rocAL plugin module, containing one or more operators.

    Args:
        module_path: Name of the module library (relative or absolute)
        global_symbols: If ``True``, the library is loaded with ``RTLD_GLOBAL`` flag or equivalent;
            otherwise ``RTLD_LOCAL`` is used. Some libraries (for example Halide) require being
            loaded with ``RTLD_GLOBAL`` - use this setting if your plugin uses any such library.

    Returns:
        None.

    Raises:
        RuntimeError: when unable to load the library.
    """
    b.LoadLibrary(module_path, global_symbols)

# define custom op through plugin module
def custom(*inputs, device=None, func: str, param_tensor):
    # pybind call arguments
    kwargs_pybind = {"input_image0": inputs[0],"is_output": False }
    output = b.CustomOp(Pipeline._current_pipeline._handle ,*(kwargs_pybind.values()))
    return (output)
