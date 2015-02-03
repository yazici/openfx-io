/*
   OFX ffmpegReader plugin.
   Reads a video input file using the libav library.

   Copyright (C) 2013 INRIA
   Author Alexandre Gauthier-Foichat alexandre.gauthier-foichat@inria.fr

   Redistribution and use in source and binary forms, with or without modification,
   are permitted provided that the following conditions are met:

   Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

   Redistributions in binary form must reproduce the above copyright notice, this
   list of conditions and the following disclaimer in the documentation and/or
   other materials provided with the distribution.

   Neither the name of the {organization} nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
   ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
   ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
   (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
   LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
   ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   INRIA
   Domaine de Voluceau
   Rocquencourt - B.P. 105
   78153 Le Chesnay Cedex - France

 */


#if (defined(_STDINT_H) || defined(_STDINT_H_) || defined(_MSC_STDINT_H_ ) ) && !defined(UINT64_C)
#warning "__STDC_CONSTANT_MACROS has to be defined before including <stdint.h>, this file will probably not compile."
#endif
#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS // ...or stdint.h wont' define UINT64_C, needed by libavutil
#endif
#include "FFmpegFile.h"

#include <cmath>
#include <iostream>

#include "ReadFFmpeg.h"

#if defined(_WIN32) || defined(WIN64)
#  include <windows.h> // for GetSystemInfo()
#define strncasecmp _strnicmp
#else
#  include <unistd.h> // for sysconf()
#endif

#define CHECK(x) \
    { \
        int error = x; \
        if (error < 0) { \
            setInternalError(error); \
            return; \
        } \
    } \

//#define TRACE_DECODE_PROCESS 1

// Use one decoding thread per processor for video decoding.
// source: http://git.savannah.gnu.org/cgit/bino.git/tree/src/media_object.cpp
static int
video_decoding_threads()
{
    static long n = -1;

    if (n < 0) {
#if defined(WIN32) || defined(WIN64)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        n = si.dwNumberOfProcessors;
#else
        n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
        if (n < 1) {
            n = 1;
        } else if (n > 16) {
            n = 16;
        }
    }

    return n;
}

static bool
extensionCorrespondToImageFile(const std::string & ext)
{
    return ext == "bmp" ||
           ext == "pix" ||
           ext == "dpx" ||
           ext == "exr" ||
           ext == "jpeg" ||
           ext == "jpg" ||
           ext == "png" ||
           ext == "ppm" ||
           ext == "ptx" ||
           ext == "tiff" ||
           ext == "tga" ||
           ext == "rgba" ||
           ext == "rgb";
}

bool
FFmpegFile::isImageFile(const std::string & filename)
{
    ///find the last index of the '.' character
    size_t lastDot = filename.find_last_of('.');

    if (lastDot == std::string::npos) { //we reached the start of the file, return false because we can't determine from the extension
        return false;
    }
    ++lastDot;//< bypass the '.' character
    std::string ext;
    std::locale loc;
    while ( lastDot < filename.size() ) {
        ext.append( 1,std::tolower(filename.at(lastDot),loc) );
        ++lastDot;
    }

    return extensionCorrespondToImageFile(ext);
}

SwsContext*
FFmpegFile::Stream::getConvertCtx(AVPixelFormat srcPixelFormat,
                                  int srcWidth,
                                  int srcHeight,
                                  int srcColorRange,
                                  AVPixelFormat dstPixelFormat,
                                  int dstWidth,
                                  int dstHeight)
{
    // Reset is flagged when the UI colour matrix selection is
    // modified. This causes a new convert context to be created
    // that reflects the UI selection.
    if (_resetConvertCtx) {
        _resetConvertCtx = false;
        if (_convertCtx) {
            sws_freeContext(_convertCtx);
            _convertCtx = NULL;
        }
    }

    if (!_convertCtx) {
        //Preventing deprecated pixel format used error messages, see:
        //https://libav.org/doxygen/master/pixfmt_8h.html#a9a8e335cf3be472042bc9f0cf80cd4c5
        //This manually sets them to the new versions of equivalent types.
        switch (srcPixelFormat) {
        case AV_PIX_FMT_YUVJ420P:
            srcPixelFormat = AV_PIX_FMT_YUV420P;
            break;
        case AV_PIX_FMT_YUVJ422P:
            srcPixelFormat = AV_PIX_FMT_YUV422P;
            break;
        case AV_PIX_FMT_YUVJ444P:
            srcPixelFormat = AV_PIX_FMT_YUV444P;
            break;
        case AV_PIX_FMT_YUVJ440P:
            srcPixelFormat = AV_PIX_FMT_YUV440P;
        default:
            break;
        }

        _convertCtx = sws_getContext(srcWidth, srcHeight, srcPixelFormat, // src format
                                     dstWidth, dstHeight, dstPixelFormat,        // dest format
                                     SWS_BICUBIC, NULL, NULL, NULL);

        // Set up the SoftWareScaler to convert colorspaces correctly.
        // Colorspace conversion makes no sense for RGB->RGB conversions
        if ( !isYUV() ) {
            return _convertCtx;
        }

        int colorspace = isRec709Format() ? SWS_CS_ITU709 : SWS_CS_ITU601;
        // Optional color space override
        if (_colorMatrixTypeOverride > 0) {
            if (_colorMatrixTypeOverride == 1) {
                colorspace = SWS_CS_ITU709;
            } else   {
                colorspace = SWS_CS_ITU601;
            }
        }

        // sws_setColorspaceDetails takes a flag indicating the white-black range of the input:
        //     0  -  mpeg, 16..235
        //     1  -  jpeg,  0..255
        int srcRange;
        // Set this flag according to the color_range reported by the codec context.
        switch (srcColorRange) {
        case AVCOL_RANGE_MPEG:
            srcRange = 0;
            break;
        case AVCOL_RANGE_JPEG:
            srcRange = 1;
            break;
        case AVCOL_RANGE_UNSPECIFIED:
        default:
            // If the colour range wasn't specified, set the flag according to
            // whether the data is YUV or not.
            srcRange = isYUV() ? 0 : 1;
            break;
        }

        int result = sws_setColorspaceDetails(_convertCtx,
                                              sws_getCoefficients(colorspace), // inv_table
                                              srcRange, // srcRange -flag indicating the white-black range of the input (1=jpeg / 0=mpeg) 0 = 16..235, 1 = 0..255
                                              sws_getCoefficients(SWS_CS_DEFAULT), // table
                                              1, // dstRange - 0 = 16..235, 1 = 0..255
                                              0, // brightness fixed point, with 0 meaning no change,
                                              1 << 16, // contrast   fixed point, with 1<<16 meaning no change,
                                              1 << 16); // saturation fixed point, with 1<<16 meaning no change);

        assert(result != -1);
    }

    return _convertCtx;
} // FFmpegFile::Stream::getConvertCtx

/*static*/ double
FFmpegFile::Stream::GetStreamAspectRatio(Stream* stream)
{
    if (stream->_avstream->sample_aspect_ratio.num) {
#if TRACE_FILE_OPEN
        std::cout << "      Aspect ratio (from stream)=" << av_q2d(stream->_avstream->sample_aspect_ratio) << std::endl;
#endif

        return av_q2d(stream->_avstream->sample_aspect_ratio);
    } else if (stream->_codecContext->sample_aspect_ratio.num)   {
#if TRACE_FILE_OPEN
        std::cout << "      Aspect ratio (from codec)=" << av_q2d(stream->_codecContext->sample_aspect_ratio) << std::endl;
#endif

        return av_q2d(stream->_codecContext->sample_aspect_ratio);
    }
#if TRACE_FILE_OPEN
    else {
        std::cout << "      Aspect ratio unspecified, assuming " << stream->_aspect << std::endl;
    }
#endif

    return stream->_aspect;
}

// get stream start time
int64_t
FFmpegFile::getStreamStartTime(Stream & stream)
{
#if TRACE_FILE_OPEN
    std::cout << "      Determining stream start PTS:" << std::endl;
#endif

    // Read from stream. If the value read isn't valid, get it from the first frame in the stream that provides such a
    // value.
    int64_t startPTS = stream._avstream->start_time;
#if TRACE_FILE_OPEN
    if ( startPTS != int64_t(AV_NOPTS_VALUE) ) {
        std::cout << "        Obtained from AVStream::start_time=";
    }
#endif

    if ( startPTS ==  int64_t(AV_NOPTS_VALUE) ) {
#if TRACE_FILE_OPEN
        std::cout << "        Not specified by AVStream::start_time, searching frames..." << std::endl;
#endif

        // Seek 1st key-frame in video stream.
        avcodec_flush_buffers(stream._codecContext);

        if (av_seek_frame(_context, stream._idx, 0, 0) >= 0) {
            av_init_packet(&_avPacket);

            // Read frames until we get one for the video stream that contains a valid PTS.
            do {
                if (av_read_frame(_context, &_avPacket) < 0) {
                    // Read error or EOF. Abort search for PTS.
#if TRACE_FILE_OPEN
                    std::cout << "          Read error, aborted search" << std::endl;
#endif
                    break;
                }
                if (_avPacket.stream_index == stream._idx) {
                    // Packet read for video stream. Get its PTS. Loop will continue if the PTS is AV_NOPTS_VALUE.
                    startPTS = _avPacket.pts;
                }

                av_free_packet(&_avPacket);
            } while ( startPTS ==  int64_t(AV_NOPTS_VALUE) );
        }
#if TRACE_FILE_OPEN
        else {
            std::cout << "          Seek error, aborted search" << std::endl;
        }
#endif

#if TRACE_FILE_OPEN
        if ( startPTS != int64_t(AV_NOPTS_VALUE) ) {
            std::cout << "        Found by searching frames=";
        }
#endif
    }

    // If we still don't have a valid initial PTS, assume 0. (This really shouldn't happen for any real media file, as
    // it would make meaningful playback presentation timing and seeking impossible.)
    if ( startPTS ==  int64_t(AV_NOPTS_VALUE) ) {
#if TRACE_FILE_OPEN
        std::cout << "        Not found by searching frames, assuming ";
#endif
        startPTS = 0;
    }

#if TRACE_FILE_OPEN
    std::cout << startPTS << " ticks, " << double(startPTS) * double(stream._avstream->time_base.num) /
        double(stream._avstream->time_base.den) << " s" << std::endl;
#endif

    return startPTS;
} // FFmpegFile::getStreamStartTime

// Get the video stream duration in frames...
int64_t
FFmpegFile::getStreamFrames(Stream & stream)
{
#if TRACE_FILE_OPEN
    std::cout << "      Determining stream frame count:" << std::endl;
#endif

    int64_t frames = 0;

    // Obtain from movie duration if specified. This is preferred since mov/mp4 formats allow the media in
    // tracks (=streams) to be remapped in time to the final movie presentation without needing to recode the
    // underlying tracks content; the movie duration thus correctly describes the final presentation.
    if (_context->duration != 0) {
        // Annoyingly, FFmpeg exposes the movie duration converted (with round-to-nearest semantics) to units of
        // AV_TIME_BASE (microseconds in practice) and does not expose the original rational number duration
        // from a mov/mp4 file's "mvhd" atom/box. Accuracy may be lost in this conversion; a duration that was
        // an exact number of frames as a rational may end up as a duration slightly over or under that number
        // of frames in units of AV_TIME_BASE.
        // Conversion to whole frames rounds up the resulting number of frames because a partial frame is still
        // a frame. However, in an attempt to compensate for AVFormatContext's inaccurate representation of
        // duration, with unknown rounding direction, the conversion to frames subtracts 1 unit (microsecond)
        // from that duration. The rationale for this is thus:
        // * If the stored duration exactly represents an exact number of frames, then that duration minus 1
        //   will result in that same number of frames once rounded up.
        // * If the stored duration is for an exact number of frames that was rounded down, then that duration
        //   minus 1 will result in that number of frames once rounded up.
        // * If the stored duration is for an exact number of frames that was rounded up, then that duration
        //   minus 1 will result in that number of frames once rounded up, while that duration unchanged would
        //   result in 1 more frame being counted after rounding up.
        // * If the original duration in the file was not for an exact number of frames, then the movie timebase
        //   would have to be >= 10^6 for there to be any chance of this calculation resulting in the wrong
        //   number of frames. This isn't a case that I've seen. Even if that were to be the case, the original
        //   duration would have to be <= 1 microsecond greater than an exact number of frames in order to
        //   result in the wrong number of frames, which is highly improbable.
        int64_t divisor = int64_t(AV_TIME_BASE) * stream._fpsDen;
        frames = ( (_context->duration - 1) * stream._fpsNum + divisor - 1 ) / divisor;

        // The above calculation is not reliable, because it seems in some situations (such as rendering out a mov
        // with 5 frames at 24 fps from Nuke) the duration has been rounded up to the nearest millisecond, which
        // leads to an extra frame being reported.  To attempt to work around this, compare against the number of
        // frames in the stream, and if they differ by one, use that value instead.
        int64_t streamFrames = stream._avstream->nb_frames;
        if ( (streamFrames > 0) && (std::abs((double)(frames - streamFrames)) <= 1) ) {
            frames = streamFrames;
        }
#if TRACE_FILE_OPEN
        std::cout << "        Obtained from AVFormatContext::duration & framerate=";
#endif
    }

    // If number of frames still unknown, obtain from stream's number of frames if specified. Will be 0 if
    // unknown.
    if (!frames) {
#if TRACE_FILE_OPEN
        std::cout << "        Not specified by AVFormatContext::duration, obtaining from AVStream::nb_frames..." << std::endl;
#endif
        frames = stream._avstream->nb_frames;
#if TRACE_FILE_OPEN
        if (frames) {
            std::cout << "        Obtained from AVStream::nb_frames=";
        }
#endif
    }

    // If number of frames still unknown, attempt to calculate from stream's duration, fps and timebase.
    if (!frames) {
#if TRACE_FILE_OPEN
        std::cout << "        Not specified by AVStream::nb_frames, calculating from duration & framerate..." << std::endl;
#endif
        frames = (int64_t(stream._avstream->duration) * stream._avstream->time_base.num  * stream._fpsNum) /
                 (int64_t(stream._avstream->time_base.den) * stream._fpsDen);
#if TRACE_FILE_OPEN
        if (frames) {
            std::cout << "        Calculated from duration & framerate=";
        }
#endif
    }

    // If the number of frames is still unknown, attempt to measure it from the last frame PTS for the stream in the
    // file relative to first (which we know from earlier).
    if (!frames) {
#if TRACE_FILE_OPEN
        std::cout << "        Not specified by duration & framerate, searching frames for last PTS..." << std::endl;
#endif

        int64_t maxPts = stream._startPTS;

        // Seek last key-frame.
        avcodec_flush_buffers(stream._codecContext);
        av_seek_frame(_context, stream._idx, stream.frameToPts(1 << 29), AVSEEK_FLAG_BACKWARD);

        // Read up to last frame, extending max PTS for every valid PTS value found for the video stream.
        av_init_packet(&_avPacket);

        while (av_read_frame(_context, &_avPacket) >= 0) {
            if ( (_avPacket.stream_index == stream._idx) && ( _avPacket.pts != int64_t(AV_NOPTS_VALUE) ) && (_avPacket.pts > maxPts) ) {
                maxPts = _avPacket.pts;
            }
            av_free_packet(&_avPacket);
        }
#if TRACE_FILE_OPEN
        std::cout << "          Start PTS=" << stream._startPTS << ", Max PTS found=" << maxPts << std::endl;
#endif

        // Compute frame range from min to max PTS. Need to add 1 as both min and max are at starts of frames, so stream
        // extends for 1 frame beyond this.
        frames = 1 + stream.ptsToFrame(maxPts);
#if TRACE_FILE_OPEN
        std::cout << "        Calculated from frame PTS range=";
#endif
    }

#if TRACE_FILE_OPEN
    std::cout << frames << std::endl;
#endif

    return frames;
} // FFmpegFile::getStreamFrames

FFmpegFile::FFmpegFile(const std::string & filename)
    : _filename(filename)
    , _context(NULL)
    , _format(NULL)
    , _streams()
    , _errorMsg()
    , _invalidState(false)
    , _avPacket()
    , _data(0)
#ifdef OFX_IO_MT_FFMPEG
    , _lock(0)
#endif
{
#ifdef OFX_IO_MT_FFMPEG
    //OFX::MultiThread::AutoMutex guard(_lock); // not needed in a constructor: we are the only owner
#endif

    if ( filename.empty() ) {
        _invalidState = true;

        return;
    }
    CHECK( avformat_open_input(&_context, _filename.c_str(), _format, NULL) );
    CHECK( avformat_find_stream_info(_context, NULL) );

#if TRACE_FILE_OPEN
    std::cout << "  " << _context->nb_streams << " streams:" << std::endl;
#endif

    // fill the array with all available video streams
    bool unsuported_codec = false;

    // find all streams that the library is able to decode
    for (unsigned i = 0; i < _context->nb_streams; ++i) {
#if TRACE_FILE_OPEN
        std::cout << "    FFmpeg stream index " << i << ": ";
#endif
        AVStream* avstream = _context->streams[i];

        // be sure to have a valid stream
        if (!avstream || !avstream->codec) {
#if TRACE_FILE_OPEN
            std::cout << "No valid stream or codec, skipping..." << std::endl;
#endif
            continue;
        }

        // considering only video streams, skipping audio
        if (avstream->codec->codec_type != AVMEDIA_TYPE_VIDEO) {
#if TRACE_FILE_OPEN
            std::cout << "Not a video stream, skipping..." << std::endl;
#endif
            continue;
        }

        // find the codec
        AVCodec* videoCodec = avcodec_find_decoder(avstream->codec->codec_id);
        if (videoCodec == NULL) {
#if TRACE_FILE_OPEN
            std::cout << "Decoder not found, skipping..." << std::endl;
#endif
            continue;
        }

        if (avstream->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            // source: http://git.savannah.gnu.org/cgit/bino.git/tree/src/media_object.cpp

            // Activate multithreaded decoding. This must be done before opening the codec; see
            // http://lists.gnu.org/archive/html/bino-list/2011-08/msg00019.html
            avstream->codec->thread_count = video_decoding_threads();
            // Set CODEC_FLAG_EMU_EDGE in the same situations in which ffplay sets it.
            // I don't know what exactly this does, but it is necessary to fix the problem
            // described in this thread: http://lists.nongnu.org/archive/html/bino-list/2012-02/msg00039.html
            int lowres = 0;
#ifdef FF_API_LOWRES
            lowres = avstream->codec->lowres;
#endif
            if ( lowres || ( videoCodec && (videoCodec->capabilities & CODEC_CAP_DR1) ) ) {
                avstream->codec->flags |= CODEC_FLAG_EMU_EDGE;
            }
        }

        // skip if the codec can't be open
        if (avcodec_open2(avstream->codec, videoCodec, NULL) < 0) {
#if TRACE_FILE_OPEN
            std::cout << "Decoder \"" << videoCodec->name << "\" failed to open, skipping..." << std::endl;
#endif
            continue;
        }

#if TRACE_FILE_OPEN
        std::cout << "Video decoder \"" << videoCodec->name << "\" opened ok, getting stream properties:" << std::endl;
#endif

        Stream* stream = new Stream();
        stream->_idx = i;
        stream->_avstream = avstream;
        stream->_codecContext = avstream->codec;
        stream->_videoCodec = videoCodec;
        stream->_avFrame = av_frame_alloc();
        {
            stream->_bitDepth = avstream->codec->bits_per_raw_sample;

            const AVPixFmtDescriptor* avPixFmtDescriptor = av_pix_fmt_desc_get(stream->_codecContext->pix_fmt);
            // Sanity check the number of components.
            // Only 3 or 4 components are supported by |engine|, that is
            // Nuke/NukeStudio will only accept 3 or 4 component data.
            // For a monochrome image (single channel) promote to 3
            // channels. This is in keeping with all the assumptions
            // throughout the code that if it is not 4 channels data
            // then it must be three channel data. This ensures that
            // all the buffer size calculations are correct.
            stream->_numberOfComponents = avPixFmtDescriptor->nb_components;
            if (3 > stream->_numberOfComponents) {
                stream->_numberOfComponents = 3;
            }
            // AVCodecContext::bits_pre_raw_sample may not be set, if
            // it's not set, try with the following utility function.
            if (0 == stream->_bitDepth) {
                stream->_bitDepth = av_get_bits_per_pixel(avPixFmtDescriptor) / stream->_numberOfComponents;
            }
        }

        if (stream->_bitDepth > 8) {
#        if VERSION_CHECK(LIBAVUTIL_VERSION_INT, <, 53, 6, 0, 53, 6, 0)
            stream->_outputPixelFormat = (4 == stream->_numberOfComponents) ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB48LE; // 16-bit.
#         else
            stream->_outputPixelFormat = (4 == stream->_numberOfComponents) ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGB48LE; // 16-bit.
#         endif
        } else                                                                                                                                     {
            stream->_outputPixelFormat = (4 == stream->_numberOfComponents) ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24; // 8-bit
        }
#if TRACE_FILE_OPEN
        std::cout << "      Timebase=" << avstream->time_base.num << "/" << avstream->time_base.den << " s/tick" << std::endl;
        std::cout << "      Duration=" << avstream->duration << " ticks, " <<
            double(avstream->duration) * double(avstream->time_base.num) /
            double(avstream->time_base.den) << " s" << std::endl;
        std::cout << "      BitDepth=" << stream->_bitDepth << std::endl;
        std::cout << "      NumberOfComponents=" << stream->_numberOfComponents << std::endl;
#endif

        // If FPS is specified, record it.
        // Otherwise assume 1 fps (default value).
        if ( (avstream->r_frame_rate.num != 0) && (avstream->r_frame_rate.den != 0) ) {
            stream->_fpsNum = avstream->r_frame_rate.num;
            stream->_fpsDen = avstream->r_frame_rate.den;
#if TRACE_FILE_OPEN
            std::cout << "      Framerate=" << stream->_fpsNum << "/" << stream->_fpsDen << ", " <<
                double(stream->_fpsNum) / double(stream->_fpsDen) << " fps" << std::endl;
#endif
        }
#if TRACE_FILE_OPEN
        else {
            std::cout << "      Framerate unspecified, assuming 1 fps" << std::endl;
        }
#endif

        stream->_width  = avstream->codec->width;
        stream->_height = avstream->codec->height;
#if TRACE_FILE_OPEN
        std::cout << "      Image size=" << stream->_width << "x" << stream->_height << std::endl;
#endif

        // set aspect ratio
        stream->_aspect = Stream::GetStreamAspectRatio(stream);

        // set stream start time and numbers of frames
        stream->_startPTS = getStreamStartTime(*stream);
        stream->_frames   = getStreamFrames(*stream);

        if (_streams.empty()) {
            std::size_t pixelDepth = stream->_bitDepth > 8 ? sizeof(unsigned short) : sizeof(unsigned char);
            // this is the first stream (in fact the only one we consider for now), allocate the output buffer according to the bitdepth
            assert(!_data);
            _data = new unsigned char[stream->_width * stream->_height * 3 * pixelDepth];
        }
        
        // save the stream
        _streams.push_back(stream);
    }
    if ( _streams.empty() ) {
        setError( unsuported_codec ? "unsupported codec..." : "unable to find video stream" );
    }
}

// destructor
FFmpegFile::~FFmpegFile()
{
#ifdef OFX_IO_MT_FFMPEG
    OFX::MultiThread::AutoMutex guard(_lock);
#endif

    // force to close all resources needed for all streams
    for (unsigned int i = 0; i < _streams.size(); ++i) {
        delete _streams[i];
    }
    _streams.clear();

    if (_context) {
        avformat_close_input(&_context);
        av_free(_context);
    }
    _filename.clear();
    _errorMsg.clear();
    _invalidState = false;
    delete _data;
}

const char*
FFmpegFile::getColorspace() const
{
    //The preferred colorspace is figured out from a number of sources - initially we look for a number
    //of different metadata sources that may be present in the file. If these fail we then fall back
    //to using the codec's underlying storage mechanism - if RGB we default to gamma 1.8, if YCbCr we
    //default to gamma 2.2 (note prores special case). Note we also ignore the NCLC atom for reading
    //purposes, as in practise it tends to be incorrect.

    //First look for the meta keys that (recent) Nukes would've written, or special cases in Arri meta.
    //Doubles up searching for lower case keys as the ffmpeg searches are case sensitive, and the keys
    //have been seen to be lower cased (particularly in old Arri movs).
    if (_context && _context->metadata) {
        AVDictionaryEntry* t;

        t = av_dict_get(_context->metadata, "uk.co.thefoundry.Colorspace", NULL, AV_DICT_IGNORE_SUFFIX);
        if (!t) {
            av_dict_get(_context->metadata, "uk.co.thefoundry.colorspace", NULL, AV_DICT_IGNORE_SUFFIX);
        }
        if (t) {
#if 0
            //Validate t->value against root list, to make sure it's been written with a LUT
            //we have a matching conversion for.
            bool found = false;
            int i     = 0;
            while (!found && LUT::builtin_names[i] != NULL) {
                found = !strcasecmp(t->value, LUT::builtin_names[i++]);
            }
#else
            bool found = true;
#endif
            if (found) {
                return t->value;
            }
        }

        t = av_dict_get(_context->metadata, "com.arri.camera.ColorGammaSxS", NULL, AV_DICT_IGNORE_SUFFIX);
        if (!t) {
            av_dict_get(_context->metadata, "com.arri.camera.colorgammasxs", NULL, AV_DICT_IGNORE_SUFFIX);
        }
        if ( t && !strncasecmp(t->value, "LOG-C", 5) ) {
            return "AlexaV3LogC";
        }
        if ( t && !strncasecmp(t->value, "REC-709", 7) ) {
            return "rec709";
        }
    }

    return isYUV() ? "Gamma2.2" : "Gamma1.8";
}

const std::string &
FFmpegFile::getError() const
{
#ifdef OFX_IO_MT_FFMPEG
    OFX::MultiThread::AutoMutex guard(_lock);
#endif

    return _errorMsg;
}

// return true if the reader can't decode the frame
bool
FFmpegFile::isInvalid() const
{
#ifdef OFX_IO_MT_FFMPEG
    OFX::MultiThread::AutoMutex guard(_lock);
#endif

    return _invalidState;
}

bool
FFmpegFile::seekFrame(int frame,
                      Stream* stream)
{
    ///Private should not lock

    avcodec_flush_buffers(stream->_codecContext);
    int64_t timestamp = stream->frameToPts(frame);
    int error = av_seek_frame(_context, stream->_idx, timestamp, AVSEEK_FLAG_BACKWARD);
    if (error < 0) {
        // Seek error. Abort attempt to read and decode frames.
        setInternalError(error, "FFmpeg Reader failed to seek frame: ");

        return false;
    }

    return true;
}

// decode a single frame into the buffer thread safe
bool
FFmpegFile::decode(int frame,
                   bool loadNearest,
                   int maxRetries)
{
    
    const unsigned int streamIdx = 0;
    
#ifdef OFX_IO_MT_FFMPEG
    OFX::MultiThread::AutoMutex guard(_lock);
#endif

    if ( streamIdx >= _streams.size() ) {
        return false;
    }

    assert(streamIdx == 0);//, "FFmpegFile functions always assume only the first stream is in use");

    // get the stream
    Stream* stream = _streams[streamIdx];

    // Early-out if out-of-range frame requested.

    if (frame < 0) {
        if (loadNearest) {
            frame = 0;
        } else {
            throw std::runtime_error("Missing frame");
        }
    } else if (frame >= stream->_frames) {
        if (loadNearest) {
            frame = (int)stream->_frames - 1;
        } else {
            throw std::runtime_error("Missing frame");
        }
    }

#if TRACE_DECODE_PROCESS
    std::cout << "mov64Reader=" << this << "::decode(): frame=" << frame << ", videoStream=" << streamIdx << ", streamIdx=" << stream->_idx << std::endl;
#endif

    // Number of read retries remaining when decode stall is detected before we give up (in the case of post-seek stalls,
    // such retries are applied only after we've searched all the way back to the start of the file and failed to find a
    // successful start point for playback)..
    //
    // We have a rather annoying case with a small subset of media files in which decode latency (between input and output
    // frames) will exceed the maximum above which we detect decode stall at certain frames on the first pass through the
    // file but those same frames will decode succesfully on a second attempt. The root cause of this is not understood but
    // it appears to be some oddity of FFmpeg. While I don't really like it, retrying decode enables us to successfully
    // decode those files rather than having to fail the read.
    int retriesRemaining = std::max(1,maxRetries);

    // Whether we have just performed a seek and are still awaiting the first decoded frame after that seek. This controls
    // how we respond when a decode stall is detected.
    //
    // One cause of such stalls is when a file contains incorrect information indicating that a frame is a key-frame when it
    // is not; a seek may land at such a frame but the decoder will not be able to start decoding until a real key-frame is
    // reached, which may be a long way in the future. Once a frame has been decoded, we will expect it to be the first frame
    // input to decode but it will actually be the next real key-frame found, leading to subsequent frames appearing as
    // earlier frame numbers and the movie ending earlier than it should. To handle such cases, when a stall is detected
    // immediately after a seek, we seek to the frame before the previous seek's landing frame, allowing us to search back
    // through the movie for a valid key frame from which decode commences correctly; if this search reaches the beginning of
    // the movie, we give up and fail the read, thus ensuring that this method will exit at some point.
    //
    // Stalls once seeking is complete and frames are being decoded are handled differently; these result in immediate read
    // failure.
    bool awaitingFirstDecodeAfterSeek = false;

    // If the frame we want is not the next one to be decoded, seek to the keyframe before/at our desired frame. Set the last
    // seeked frame to indicate that we need to synchronise frame indices once we've read the first frame of the video stream,
    // since we don't yet know which frame number the seek will land at. Also invalidate current indices, reset accumulated
    // decode latency and record that we're awaiting the first decoded frame after a seek.
    int lastSeekedFrame = -1; // 0-based index of the last frame to which we seeked when seek in progress / negative when no
    // seek in progress,

    if (frame != stream->_decodeNextFrameOut) {
#if TRACE_DECODE_PROCESS
        std::cout << "  Next frame expected out=" << stream->_decodeNextFrameOut << ", Seeking to desired frame" << std::endl;
#endif

        lastSeekedFrame = frame;
        stream->_decodeNextFrameIn  = -1;
        stream->_decodeNextFrameOut = -1;
        stream->_accumDecodeLatency = 0;
        awaitingFirstDecodeAfterSeek = true;

        if ( !seekFrame(frame, stream) ) {
            return false;
        }
    }
#if TRACE_DECODE_PROCESS
    else {
        std::cout << "  Next frame expected out=" << stream->_decodeNextFrameOut << ", No seek required" << std::endl;
    }
#endif

    av_init_packet(&_avPacket);

    // Loop until the desired frame has been decoded. May also break from within loop on failure conditions where the
    // desired frame will never be decoded.
    bool hasPicture = false;
    do {
        bool decodeAttempted = false;
        int frameDecoded = 0;
        int srcColourRange = stream->_codecContext->color_range;

        // If the next frame to decode is within range of frames (or negative implying invalid; we've just seeked), read
        // a new frame from the source file and feed it to the decoder if it's for the video stream.
        if (stream->_decodeNextFrameIn < stream->_frames) {
#if TRACE_DECODE_PROCESS
            std::cout << "  Next frame expected in=";
            if (stream->_decodeNextFrameIn >= 0) {
                std::cout << stream->_decodeNextFrameIn;
            } else {
                std::cout << "unknown";
            }
#endif

            int error = av_read_frame(_context, &_avPacket);
            // [FD] 2015/01/20
            // the following if() was not in Nuke's mov64Reader.cpp
            if (error == (int)AVERROR_EOF) {
                // getStreamFrames() was probably wrong
                stream->_frames = stream->_decodeNextFrameIn;
                if (loadNearest) {
                    // try again
                    frame = (int)stream->_frames - 1;
                    lastSeekedFrame = frame;
                    stream->_decodeNextFrameIn  = -1;
                    stream->_decodeNextFrameOut = -1;
                    stream->_accumDecodeLatency = 0;
                    awaitingFirstDecodeAfterSeek = true;

                    if ( !seekFrame(frame, stream) ) {
                        return false;
                    }
                }
                continue;
            }
            if (error < 0) {
                // Read error. Abort attempt to read and decode frames.
#if TRACE_DECODE_PROCESS
                std::cout << ", Read failed" << std::endl;
#endif
                setInternalError(error, "FFmpeg Reader failed to read frame: ");
                break;
            }
#if TRACE_DECODE_PROCESS
            std::cout << ", Read OK, Packet data:" << std::endl;
            std::cout << "    PTS=" << _avPacket.pts <<
                ", DTS=" << _avPacket.dts <<
                ", Duration=" << _avPacket.duration <<
                ", KeyFrame=" << ( (_avPacket.flags & AV_PKT_FLAG_KEY) ? 1 : 0 ) <<
                ", Corrupt=" << ( (_avPacket.flags & AV_PKT_FLAG_CORRUPT) ? 1 : 0 ) <<
                ", StreamIdx=" << _avPacket.stream_index <<
                ", PktSize=" << _avPacket.size;
#endif

            // If the packet read belongs to the video stream, synchronise frame indices from it if required and feed it
            // into the decoder.
            if (_avPacket.stream_index == stream->_idx) {
#if TRACE_DECODE_PROCESS
                std::cout << ", Relevant stream" << std::endl;
#endif

                // If the packet read has a valid PTS, record that we have seen a PTS for this stream.
                if ( _avPacket.pts != int64_t(AV_NOPTS_VALUE) ) {
                    stream->_ptsSeen = true;
                }

                // If a seek is in progress, we need to synchronise frame indices if we can...
                if (lastSeekedFrame >= 0) {
#if TRACE_DECODE_PROCESS
                    std::cout << "    In seek (" << lastSeekedFrame << ")";
#endif

                    // Determine which frame the seek landed at, using whichever kind of timestamp is currently selected for this
                    // stream. If there's no timestamp available at that frame, we can't synchronise frame indices to know which
                    // frame we're first going to decode, so we need to seek back to an earlier frame in hope of obtaining a
                    // timestamp. Likewise, if the landing frame is after the seek target frame (this can happen, presumably a bug
                    // in FFmpeg seeking), we need to seek back to an earlier frame so that we can start decoding at or before the
                    // desired frame.
                    int landingFrame = stream->ptsToFrame(_avPacket.*stream->_timestampField);

                    if ( ( _avPacket.*stream->_timestampField == int64_t(AV_NOPTS_VALUE) ) || (landingFrame  > lastSeekedFrame) ) {
#if TRACE_DECODE_PROCESS
                        std::cout << ", landing frame not found";
                        if ( _avPacket.*stream->_timestampField == int64_t(AV_NOPTS_VALUE) ) {
                            std::cout << " (no timestamp)";
                        } else {
                            std::cout << " (landed after target at " << landingFrame << ")";
                        }
#endif

                        // Wind back 1 frame from last seeked frame. If that takes us to before frame 0, we're never going to be
                        // able to synchronise using the current timestamp source...
                        if (--lastSeekedFrame < 0) {
#if TRACE_DECODE_PROCESS
                            std::cout << ", can't seek before start";
#endif

                            // If we're currently using PTSs to determine the landing frame and we've never seen a valid PTS for any
                            // frame from this stream, switch to using DTSs and retry the read from the initial desired frame.
                            if ( (stream->_timestampField == &AVPacket::pts) && !stream->_ptsSeen ) {
                                stream->_timestampField = &AVPacket::dts;
                                lastSeekedFrame = frame;
#if TRACE_DECODE_PROCESS
                                std::cout << ", PTSs absent, switching to use DTSs";
#endif
                            }
                            // Otherwise, failure to find a landing point isn't caused by an absence of PTSs from the file or isn't
                            // recovered by using DTSs instead. Something is wrong with the file. Abort attempt to read and decode frames.
                            else {
#if TRACE_DECODE_PROCESS
                                if (stream->_timestampField == &AVPacket::dts) {
                                    std::cout << ", search using DTSs failed";
                                } else {
                                    std::cout << ", PTSs present";
                                }
                                std::cout << ",  giving up" << std::endl;
#endif
                                setError("FFmpeg Reader failed to find timing reference frame, possible file corruption");
                                break;
                            }
                        }

                        // Seek to the new frame. By leaving the seek in progress, we will seek backwards frame by frame until we
                        // either successfully synchronise frame indices or give up having reached the beginning of the stream.
#if TRACE_DECODE_PROCESS
                        std::cout << ", seeking to " << lastSeekedFrame << std::endl;
#endif
                        if ( !seekFrame(lastSeekedFrame, stream) ) {
                            break;
                        }
                    }
                    // Otherwise, we have a valid landing frame, so set that as the next frame into and out of decode and set
                    // no seek in progress.
                    else {
#if TRACE_DECODE_PROCESS
                        std::cout << ", landed at " << landingFrame << std::endl;
#endif
                        stream->_decodeNextFrameOut = stream->_decodeNextFrameIn = landingFrame;
                        lastSeekedFrame = -1;
                    }
                }

                // If there's no seek in progress, feed this frame into the decoder.
                if (lastSeekedFrame < 0) {
#if TRACE_DECODE_BITSTREAM
                    // H.264 ONLY
                    std::cout << "  Decoding input frame " << stream->_decodeNextFrameIn << " bitstream:" << std::endl;
                    uint8_t *data = _avPacket.data;
                    uint32_t remain = _avPacket.size;
                    while (remain > 0) {
                        if (remain < 4) {
                            std::cout << "    Insufficient remaining bytes (" << remain << ") for block size at BlockOffset=" << (data - _avPacket.data) << std::endl;
                            remain = 0;
                        } else   {
                            uint32_t blockSize = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
                            data += 4;
                            remain -= 4;
                            std::cout << "    BlockOffset=" << (data - _avPacket.data) << ", Size=" << blockSize;
                            if (remain < blockSize) {
                                std::cout << ", Insufficient remaining bytes (" << remain << ")" << std::endl;
                                remain = 0;
                            } else   {
                                std::cout << ", Bytes:";
                                int count = (blockSize > 16 ? 16 : blockSize);
                                for (int offset = 0; offset < count; offset++) {
                                    static const char hexTable[] = "0123456789ABCDEF";
                                    uint8_t byte = data[offset];
                                    std::cout << ' ' << hexTable[byte >> 4] << hexTable[byte & 0xF];
                                }
                                std::cout << std::endl;
                                data += blockSize;
                                remain -= blockSize;
                            }
                        }
                    }
#elif TRACE_DECODE_PROCESS
                    std::cout << "  Decoding input frame " << stream->_decodeNextFrameIn << std::endl;
#endif

                    // Advance next frame to input.
                    ++stream->_decodeNextFrameIn;

                    // Decode the frame just read. frameDecoded indicates whether a decoded frame was output.
                    decodeAttempted = true;

                    {
                        error = avcodec_decode_video2(stream->_codecContext, stream->_avFrame, &frameDecoded, &_avPacket);
                    }
                    if (error < 0) {
                        // Decode error. Abort attempt to read and decode frames.
                        setInternalError(error, "FFmpeg Reader failed to decode frame: ");
                        break;
                    }
                }
            } //_avPacket.stream_index == stream->_idx
#if TRACE_DECODE_PROCESS
            else {
                std::cout << ", Irrelevant stream" << std::endl;
            }
#endif
        } //stream->_decodeNextFrameIn < stream->_frames
          // If the next frame to decode is out of frame range, there's nothing more to read and the decoder will be fed
          // null input frames in order to obtain any remaining output.
        else {
#if TRACE_DECODE_PROCESS
            std::cout << "  No more frames to read, pumping remaining decoder output" << std::endl;
#endif

            // Obtain remaining frames from the decoder. pkt_ contains NULL packet data pointer and size at this point,
            // required to pump out remaining frames with no more input. frameDecoded indicates whether a decoded frame
            // was output.
            decodeAttempted = true;
            int error = 0;
            {
                error = avcodec_decode_video2(stream->_codecContext, stream->_avFrame, &frameDecoded, &_avPacket);
            }
            if (error < 0) {
                // Decode error. Abort attempt to read and decode frames.
                setInternalError(error, "FFmpeg Reader failed to decode frame: ");
                break;
            }
        }

        // If a frame was decoded, ...
        if (frameDecoded) {
#if TRACE_DECODE_PROCESS
            std::cout << "    Frame decoded=" << stream->_decodeNextFrameOut;
#endif

            // Now that we have had a frame decoded, we know that seek landed at a valid place to start decode. Any decode
            // stalls detected after this point will result in immediate decode failure.
            awaitingFirstDecodeAfterSeek = false;

            // If the frame just output from decode is the desired one, get the decoded picture from it and set that we
            // have a picture.
            if (stream->_decodeNextFrameOut == frame) {
#if TRACE_DECODE_PROCESS
                std::cout << ", is desired frame" << std::endl;
#endif

                AVPicture output;
                avpicture_fill(&output, _data, stream->_outputPixelFormat, stream->_width, stream->_height);

                SwsContext* context = NULL;
                {
                    context = stream->getConvertCtx(stream->_codecContext->pix_fmt, stream->_width, stream->_height,
                                                    srcColourRange,
                                                    stream->_outputPixelFormat, stream->_width, stream->_height);
                }

                // Scale if any of the decoding path has provided a convert
                // context. Otherwise, no scaling/conversion is required after
                // decoding the frame.
                if (context) {
                    sws_scale(context,
                              stream->_avFrame->data,
                              stream->_avFrame->linesize,
                              0,
                              stream->_height,
                              output.data,
                              output.linesize);
                }

                hasPicture = true;
            }
#if TRACE_DECODE_PROCESS
            else {
                std::cout << ", is not desired frame (" << frame << ")" << std::endl;
            }
#endif

            // Advance next output frame expected from decode.
            ++stream->_decodeNextFrameOut;
        }
        // If no frame was decoded but decode was attempted, determine whether this constitutes a decode stall and handle if so.
        else if (decodeAttempted) {
            // Failure to get an output frame for an input frame increases the accumulated decode latency for this stream.
            ++stream->_accumDecodeLatency;

#if TRACE_DECODE_PROCESS
            std::cout << "    No frame decoded, accumulated decode latency=" << stream->_accumDecodeLatency << ", max allowed latency=" << stream->getCodecDelay() << std::endl;
#endif

            // If the accumulated decode latency exceeds the maximum delay permitted for this codec at this time (the delay can
            // change dynamically if the codec discovers B-frames mid-stream), we've detected a decode stall.
            if ( stream->_accumDecodeLatency > stream->getCodecDelay() ) {
                int seekTargetFrame; // Target frame for any seek we might perform to attempt decode stall recovery.

                // Handle a post-seek decode stall.
                if (awaitingFirstDecodeAfterSeek) {
                    // If there's anywhere in the file to seek back to before the last seek's landing frame (which can be found in
                    // stream->_decodeNextFrameOut, since we know we've not decoded any frames since landing), then set up a seek to
                    // the frame before that landing point to try to find a valid decode start frame earlier in the file.
                    if (stream->_decodeNextFrameOut > 0) {
                        seekTargetFrame = stream->_decodeNextFrameOut - 1;
#if TRACE_DECODE_PROCESS
                        std::cout << "    Post-seek stall detected, trying earlier decode start, seeking frame " << seekTargetFrame << std::endl;
#endif
                    }
                    // Otherwise, there's nowhere to seek back. If we have any retries remaining, use one to attempt the read again,
                    // starting from the desired frame.
                    else if (retriesRemaining > 0) {
                        --retriesRemaining;
                        seekTargetFrame = frame;
#if TRACE_DECODE_PROCESS
                        std::cout << "    Post-seek stall detected, at start of file, retrying from desired frame " << seekTargetFrame << std::endl;
#endif
                    }
                    // Otherwise, all we can do is to fail the read so that this method exits safely.
                    else {
#if TRACE_DECODE_PROCESS
                        std::cout << "    Post-seek STALL DETECTED, at start of file, no more retries, failed read" << std::endl;
#endif
                        setError("FFmpeg Reader failed to find decode reference frame, possible file corruption");
                        break;
                    }
                }
                // Handle a mid-decode stall. All we can do is to fail the read so that this method exits safely.
                else {
                    // If we have any retries remaining, use one to attempt the read again, starting from the desired frame.
                    if (retriesRemaining > 0) {
                        --retriesRemaining;
                        seekTargetFrame = frame;
#if TRACE_DECODE_PROCESS
                        std::cout << "    Mid-decode stall detected, retrying from desired frame " << seekTargetFrame << std::endl;
#endif
                    }
                    // Otherwise, all we can do is to fail the read so that this method exits safely.
                    else {
#if TRACE_DECODE_PROCESS
                        std::cout << "    Mid-decode STALL DETECTED, no more retries, failed read" << std::endl;
#endif
                        setError("FFmpeg Reader detected decoding stall, possible file corruption");
                        break;
                    }
                }

                // If we reach here, seek to the target frame chosen above in an attempt to recover from the decode stall.
                lastSeekedFrame = seekTargetFrame;
                stream->_decodeNextFrameIn  = -1;
                stream->_decodeNextFrameOut = -1;
                stream->_accumDecodeLatency = 0;
                awaitingFirstDecodeAfterSeek = true;

                if ( !seekFrame(seekTargetFrame, stream) ) {
                    break;
                }
            }
        }

        av_free_packet(&_avPacket);
    } while (!hasPicture);

#if TRACE_DECODE_PROCESS
    std::cout << "<-validPicture=" << hasPicture << " for frame " << frame << std::endl;
#endif

    // If read failed, reset the next frame expected out so that we seek and restart decode on next read attempt. Also free
    // the AVPacket, since it won't have been freed at the end of the above loop (we reach here by a break from the main
    // loop when hasPicture is false).
    if (!hasPicture) {
        if (_avPacket.size > 0) {
            av_free_packet(&_avPacket);
        }
        stream->_decodeNextFrameOut = -1;
    }

    return hasPicture;
} // FFmpegFile::decode

bool
FFmpegFile::getFPS(double & fps,
                   unsigned streamIdx)
{
#ifdef OFX_IO_MT_FFMPEG
    OFX::MultiThread::AutoMutex guard(_lock);
#endif

    if ( streamIdx >= _streams.size() ) {
        return false;
    }

    // get the stream
    Stream* stream = _streams[streamIdx];
    fps = (double)stream->_fpsNum / stream->_fpsDen;

    return true;
}

// get stream information
bool
FFmpegFile::getInfo(int & width,
                    int & height,
                    double & aspect,
                    int & frames,
                    unsigned streamIdx)
{
#ifdef OFX_IO_MT_FFMPEG
    OFX::MultiThread::AutoMutex guard(_lock);
#endif

    if ( streamIdx >= _streams.size() ) {
        return false;
    }

    // get the stream
    Stream* stream = _streams[streamIdx];

    width  = stream->_width;
    height = stream->_height;
    aspect = stream->_aspect;
    frames = (int)stream->_frames;

    return true;
}

int
FFmpegFile::getRowSize() const
{
    // returns 0 if no stream
    const int bitDepth = getBitDepth();

    if (bitDepth > 8) {
        return getNumberOfComponents() * getWidth() * sizeof(uint16_t);
    }

    return getNumberOfComponents() * getWidth();
}

int
FFmpegFile::getBufferSize() const
{
    return getRowSize() * getHeight();
}

