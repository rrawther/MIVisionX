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
#include <string>
#include <utility>
#include "commons.h"
#include "loader_module.h"
#include "node_video_file_source.h"
#include "reader_factory.h"

#ifdef RALI_VIDEO
class VideoLoaderModule : public LoaderModule
{
public:
    explicit VideoLoaderModule(std::shared_ptr<VideoFileNode> video_node);

    LoaderModuleStatus load_next() override;
    virtual void initialize(ReaderConfig reader_config, DecoderConfig decoder_config, RaliMemType mem_type, unsigned batch_size, bool keep_orig_size) override;
    void set_output_image (Image* output_image) override;
    size_t remaining_count() override; // returns number of remaining items to be loaded
    void reset() override; // Resets the loader to load from the beginning of the media
    //void stop() override  {}
    virtual std::vector<std::string> get_id(); // returns the id of the last batch of images/frames loaded
    void start_loading() override {};
    decoded_image_info get_decode_image_info() override;
    Timing timing() override;
private:
    std::shared_ptr<Reader> _reader;
    std::shared_ptr<VideoFileNode> _video_node;
    void de_init();
    std::vector<std::string> _output_names;//!< video name/ids that are stores in the _output_image
    decoded_image_info _output_decoded_img_info;
    size_t _remaining_count;//!< How many videos are there yet to be loaded
    bool _is_initialized;
    size_t _batch_size;

};
#endif