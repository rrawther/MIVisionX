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


#include "video_loader_module.h"
#ifdef RALI_VIDEO
VideoLoaderModule::VideoLoaderModule(std::shared_ptr<VideoFileNode> video_node):_video_node(std::move(video_node))
{
}

LoaderModuleStatus 
VideoLoaderModule::load_next()
{
    // Do nothing since call to process graph suffices (done externally)
    return LoaderModuleStatus::OK;
}

void
VideoLoaderModule::set_output_image (Image* output_image)
{
}

void 
VideoLoaderModule::de_init()
{
    _batch_size = 1;
    _is_initialized = false;
    _remaining_count = 0;
}


void
VideoLoaderModule::initialize(ReaderConfig reader_config, DecoderConfig decoder_config, RaliMemType mem_type, unsigned batch_size, bool keep_orig_size)
{
    // initialize video reader.
    try
    {
        _reader = create_reader(reader_config);
    }
    catch(const std::exception& e)
    {
        de_init();
        throw;
    }

    for (unsigned int i=0 ; i < batch_size; i++)
      _output_names.push_back("-1");

    _is_initialized = true;
    LOG("VideoLoaderModule initialized");
}

size_t VideoLoaderModule::remaining_count()
{
    // TODO: use FFMPEG to find the total number of frames and keep counting 
    // how many times laod_next() is called successfully, subtract them and 
    // that would be the count of frames remained to be decoded
    return 9999;
}

// return ids of last batch of videos loaded
std::vector<std::string> VideoLoaderModule::get_id()
{
    // todo::
    // for now get _output_names from video node
    if (_video_node) {
        _output_names = _video_node->get_output_names();
        return _output_names;
    }else
    {
        return {};
    }
}

void VideoLoaderModule::reset()
{
    // Functionality not there yet in the OpenVX API
}

decoded_image_info VideoLoaderModule::get_decode_image_info()
{
    // todo:: this is invalid structure retirned
    return _output_decoded_img_info;
}


Timing VideoLoaderModule::timing()
{
    Timing t;
    //t.image_read_time = _file_load_time.get_timing();
    //t.image_process_time = _swap_handle_time.get_timing();
    t.image_read_time = 0;
    t.image_process_time = 0;
    return t;
}

#endif
