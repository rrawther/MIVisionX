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

#include <memory>
#include <sstream>
#include <vx_amd_media.h>
#include <vx_ext_amd.h>
#include <vx_ext_rpp.h>
#include "node_video_file_source.h"
#include "video_loader_module.h"
#ifdef RALI_VIDEO

void VideoFileNode::create_node()
{
    std::ostringstream iss;
    // The format for the input string to the OVX decoder API is as follows:
    // <video_stream_count>,<path_to_video_0>:0|1,<path_to_video_1>:0|1,<path_to_video_2>:0|1 ...
    // after each path to videos 0 stands for sw decoding, while 1 stands for hw decoding
    iss << _video_stream_count << ",";
    for(size_t i = 0; i < _video_stream_count; i++)
        iss << (_path_to_videos[i])<< ":" << ((_decode_mode == DecodeMode::USE_HW) ? "1":"0") << ",";

    LOG("Total of "+TOSTR(_video_stream_count)+ " videos going to be loaded " + iss.str())

    _node = amdMediaDecoderNode(_graph->get(), iss.str().c_str(), _outputs[0]->handle(), NULL, _loop, _output_in_ocl);
    vx_status res = VX_SUCCESS;
    if((res = vxGetStatus((vx_reference)_node)) != VX_SUCCESS)
        THROW("Failed to add video decoder node")
}

void VideoFileNode::init(const std::string &source_path, DecodeMode decoder_mode, bool loop, int batch_size)
{
    _decode_mode = decoder_mode;
    _source_path = source_path;
    _loop = loop;
    _batch_size = batch_size;
    if(_batch_size > MAXIMUM_VIDEO_CONCURRENT_DECODE)
        THROW("Video batch size " + TOSTR(_batch_size)+" is bigger than " + TOSTR(MAXIMUM_VIDEO_CONCURRENT_DECODE))
    // load batchsize of videos
    _video_stream_count = _batch_size;
    auto video_path = _source_path+":";
    std::string path;
    size_t new_pos = 0, prev_pos = 0, vid_count = 0;
    _path_to_videos.resize(_batch_size);
    while((new_pos = video_path.find(":", prev_pos)) != std::string::npos)
    {
        //if (new_pos) new_pos  += 1; // for ignoring :
        path = video_path.substr(prev_pos, new_pos-prev_pos);
        if(vid_count >= _batch_size)
            break;
        prev_pos = new_pos+1;
        _path_to_videos[vid_count++] = path;
        LOG( "video " + path + " parsed from input")
    }
    LOG("Total of "+TOSTR(vid_count)+ " videos going to be loaded , using "+((_decode_mode == DecodeMode::USE_HW) ? " hardware decoder ":" software decoder"))
}

VideoFileNode::VideoFileNode(const std::vector<Image*>& inputs, const std::vector<Image*>& outputs):
        Node(inputs, outputs)
{
//    _batch_size = outputs[0]->info().batch_size();
    _output_in_ocl = false;     // enable this for decoded buffer in ocl
}

#if 0
std::shared_ptr<LoaderModule> VideoFileNode::get_loader_module()
{
    if(!_loader_module)
        WRN("VideoFileNode's loader module is null, not initialized")
    return _loader_module;
}
#endif
#endif