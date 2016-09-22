/*----------------------------------------------------------------
| config
+----------------------------------------------------------------*/
// undefine this to get rid of the identifying mark in the pictures
// or define it to a max-4 digit number encoded as hex-coded decimal
//#define MF_VIDEO_DECODER_IDENTIFYING_MARK 0xF00D

// define this if you want to use older versions of ffmpeg that don't
// have the newer BSF API
#define MF_VIDEO_DECODER_USE_DEPRECATED_BSF_API

/*----------------------------------------------------------------
| includes
+----------------------------------------------------------------*/
extern "C" {
#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavcodec/avcodec.h"
}

#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>

#include <queue>

/*----------------------------------------------------------------
| constants
+----------------------------------------------------------------*/
#define MF_VIDEO_DECODER_VERSION 0x010001

const int MF_VIDEO_DECODER_SUCCESS        =  0;
const int MF_VIDEO_DECODER_FAILURE        = -1;
const int MF_VIDEO_DECODER_ERROR_INTERNAL = -2;
const int MF_VIDEO_DECODER_NEED_DATA      = -3;

static const AVProfile mf_video_decoder_profiles[] = {
    { FF_PROFILE_H264_BASELINE,             "Baseline"              },
    { FF_PROFILE_H264_CONSTRAINED_BASELINE, "Constrained Baseline"  },
    { FF_PROFILE_H264_MAIN,                 "Main"                  },
    { FF_PROFILE_H264_EXTENDED,             "Extended"              },
    { FF_PROFILE_H264_HIGH,                 "High"                  },
    { FF_PROFILE_H264_HIGH_10,              "High 10"               },
    { FF_PROFILE_H264_HIGH_10_INTRA,        "High 10 Intra"         },
    { FF_PROFILE_H264_HIGH_422,             "High 4:2:2"            },
    { FF_PROFILE_H264_HIGH_422_INTRA,       "High 4:2:2 Intra"      },
    { FF_PROFILE_H264_HIGH_444,             "High 4:4:4"            },
    { FF_PROFILE_H264_HIGH_444_PREDICTIVE,  "High 4:4:4 Predictive" },
    { FF_PROFILE_H264_HIGH_444_INTRA,       "High 4:4:4 Intra"      },
    { FF_PROFILE_H264_CAVLC_444,            "CAVLC 4:4:4"           },
    { FF_PROFILE_UNKNOWN },
};

#if defined(MF_VIDEO_DECODER_IDENTIFYING_MARK)
static const unsigned char DigitPatterns[16][5] = {
    {7,5,5,5,7}, // 0
    {2,2,2,2,2}, // 1
    {7,1,7,4,7}, // 2
    {7,1,3,1,7}, // 3
    {4,5,7,1,1}, // 4
    {7,4,7,1,7}, // 5
    {4,4,7,5,7}, // 6
    {7,1,1,1,1}, // 7
    {7,5,7,5,7}, // 8
    {7,5,7,1,1}, // 9
    {7,5,7,5,5}, // A
    {6,5,6,5,6}, // B
    {7,4,4,4,7}, // C
    {6,5,5,5,6}, // D
    {7,4,6,4,7}, // E
    {7,4,6,4,4}  // F
};
#endif

/*----------------------------------------------------------------------
|    macros
+---------------------------------------------------------------------*/
#if !defined(MF_SUCCEEDED)
#define MF_SUCCEEDED(x) (((HRESULT)(x)) >= 0)
#endif

#if !defined(MF_FAILED)
#define MF_FAILED(x) (((HRESULT)(x)) < 0)
#endif

/*----------------------------------------------------------------
| MF_VIDEO_DecoderContext struct (keep this a plain C struct)
+----------------------------------------------------------------*/
typedef struct {
    const AVClass* _class;
    
#if defined(MF_VIDEO_DECODER_USE_DEPRECATED_BSF_API)
	AVBitStreamFilterContext *annexb_bsf_ctx; 
#else
	AVBSFContext* annexb_bsf_ctx;
#endif
	IMFTransform*           decoder;
	std::queue<IMFSample*>* samples; // keep this a pointer
	bool                    draining;
	uint64_t                output_frame_count;
	uint64_t                input_frame_count;
} MF_VIDEO_DecoderContext;

/*----------------------------------------------------------------*/
static void
mf_video_decoder_flush_samples(AVCodecContext *avctx)
{
    MF_VIDEO_DecoderContext* self = (MF_VIDEO_DecoderContext*)avctx->priv_data;

	// flush all buffered samples
	while (!self->samples->empty()) {
		self->samples->front()->Release();
		self->samples->pop();
	}
}

/*----------------------------------------------------------------*/
static int
mf_video_decoder_close(AVCodecContext* avctx)
{
    MF_VIDEO_DecoderContext* self = (MF_VIDEO_DecoderContext*)avctx->priv_data;

    av_log(avctx, AV_LOG_TRACE, "mf_video_decoder_close\n");

	if (self->annexb_bsf_ctx) {
#if defined(MF_VIDEO_DECODER_USE_DEPRECATED_BSF_API)
		av_bitstream_filter_close(self->annexb_bsf_ctx);
		self->annexb_bsf_ctx = NULL;
#else
		av_bsf_free(&self->annexb_bsf_ctx);
#endif
	}
	if (self->decoder) {
		self->decoder->Release();
		self->decoder = NULL;
	}

	// make sure we flush all buffered samples
	mf_video_decoder_flush_samples(avctx);
	delete self->samples;

    return 0;
}

/*----------------------------------------------------------------*/
static void
mf_video_decoder_set_output_type(MF_VIDEO_DecoderContext* self)
{
    // set output type
    IMFMediaType* output_type = NULL;
    HRESULT mf_result = S_OK;
	bool done = false;
    for (unsigned int i=0; MF_SUCCEEDED(mf_result) && !done; i++) {
        mf_result = self->decoder->GetOutputAvailableType(0, i, &output_type);
        if (MF_SUCCEEDED(mf_result)) {
            GUID subtype;
            mf_result = output_type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (MF_SUCCEEDED(mf_result) && (subtype == MFVideoFormat_I420 || subtype == MFVideoFormat_IYUV)) {
                mf_result = self->decoder->SetOutputType(0, output_type, 0);
                if (MF_SUCCEEDED(mf_result)) {
					done = true;
                }
            }
            output_type->Release();
        }
    }
}

/*----------------------------------------------------------------*/
static HRESULT
mf_video_decoder_get_stride(IMFMediaType* media_type, LONG& stride)
{
	stride = 0;
    UINT32 stride_32 = 0;

    // try to get the default stride from the media type.
    HRESULT mf_result = media_type->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&stride_32);
	if (MF_SUCCEEDED(mf_result)) {
		stride = (LONG)stride_32;
	} else {
        // attribute not set, try to calculate the default stride.
        GUID   subtype = GUID_NULL;
        UINT32 width = 0;
        UINT32 height = 0;

        // get the subtype and the image size.
        mf_result = media_type->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (MF_FAILED(mf_result)) {
            goto end;
        }

        mf_result = MFGetAttributeSize(media_type, MF_MT_FRAME_SIZE, &width, &height);
        if (MF_FAILED(mf_result)) {
            goto end;
        }

        mf_result = MFGetStrideForBitmapInfoHeader(subtype.Data1, width, &stride);
        if (MF_FAILED(mf_result)) {
            goto end;
        }

		// set the attribute for later reference.
        (void)media_type->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(stride));
    }

end:
    return mf_result;
}

/*----------------------------------------------------------------*/
static int
mf_video_decoder_init(AVCodecContext* avctx)
{
    MF_VIDEO_DecoderContext* self = (MF_VIDEO_DecoderContext*)avctx->priv_data;

    av_log(avctx, AV_LOG_TRACE, "mf_video_decoder_init\n");

	// init
	self->annexb_bsf_ctx     = NULL;
	self->decoder            = NULL;
	self->samples            = NULL;
	self->draining           = false;
	self->input_frame_count  = 0;
	self->output_frame_count = 0;

	// init the annex-b filter
#if defined(MF_VIDEO_DECODER_USE_DEPRECATED_BSF_API)
	self->annexb_bsf_ctx = av_bitstream_filter_init("h264_mp4toannexb");
	if (self->annexb_bsf_ctx == NULL) {
		av_log(avctx, AV_LOG_ERROR, "failed to find h264_mp4toannexb bsf\n");
		return -1;
	}
#else
	const AVBitStreamFilter* annexb_bsf = av_bsf_get_by_name("h264_mp4toannexb");
	if (annexb_bsf == NULL) {
		av_log(avctx, AV_LOG_ERROR, "failed to find h264_mp4toannexb bsf\n");
		return -1;
	}
	int av_result = av_bsf_alloc(annexb_bsf, &self->annexb_bsf_ctx);
	if (av_result != 0) {
		av_log(avctx, AV_LOG_ERROR, "av_bsf_alloc failed (%d)\n", av_result);
		return -1;
	}
	avcodec_parameters_from_context(self->annexb_bsf_ctx->par_in, avctx);
	av_bsf_init(self->annexb_bsf_ctx);
#endif

	// allocate the sample queue
	self->samples = new std::queue<IMFSample*>;

	// initialize COM, we'll need it
    CoInitialize(NULL);

    // init Media Foundation
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

	// find a suitable decoder
    MFT_REGISTER_TYPE_INFO input_type_info = {
        MFMediaType_Video,
        MFVideoFormat_H264
    };

    UINT32 flags = MFT_ENUM_FLAG_SYNCMFT  |
                   MFT_ENUM_FLAG_ASYNCMFT |
                   MFT_ENUM_FLAG_LOCALMFT |
                   MFT_ENUM_FLAG_HARDWARE |
                   MFT_ENUM_FLAG_SORTANDFILTER;

    IMFActivate** factories = NULL;
    UINT32        factory_count = 0;

    HRESULT mf_result = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                                  flags,
                                  &input_type_info,
                                  NULL,
                                  &factories,
                                  &factory_count);

    IMFActivate* decoder_factory = NULL;
    av_log(avctx, AV_LOG_TRACE, "found %d decoders\n", factory_count);

    if (factory_count > 0) {
        decoder_factory = factories[0];
        mf_result = decoder_factory->ActivateObject(IID_IMFTransform, (void**)&self->decoder);
        if (mf_result != S_OK) {
            av_log(avctx, AV_LOG_WARNING, "failed to created decoder (%d)\n", mf_result);
        }

        for (unsigned int i = 0; i < factory_count; i++) {
            factories[i]->Release();
        }
    }

    CoTaskMemFree(factories);

	// stop now if we couldn't create the decoder
	if (self->decoder == NULL) {
		return -1;
	}

	// query and set transform properties
	IMFAttributes* decoder_attributes = NULL;
	mf_result = self->decoder->GetAttributes(&decoder_attributes);
	if (MF_SUCCEEDED(mf_result) && decoder_attributes) {
		UINT32 decoder_is_accelerated = 0;
		mf_result = decoder_attributes->GetUINT32(CODECAPI_AVDecVideoAcceleration_H264, &decoder_is_accelerated);
		if (MF_SUCCEEDED(mf_result)) {
			av_log(avctx, AV_LOG_INFO, "Media Foundation H.264 Decoder HW Acceleration: %d\n", decoder_is_accelerated);
			if (!decoder_is_accelerated) {
				mf_result = decoder_attributes->SetUINT32(CODECAPI_AVDecVideoAcceleration_H264, 1);
				if (MF_SUCCEEDED(mf_result)) {
					av_log(avctx, AV_LOG_TRACE, "enabled MF H.264 HW acceleration\n");
				} else {
					av_log(avctx, AV_LOG_TRACE, "failed to enable MF H.264 HW acceleration (%d)\n", mf_result);
				}
			}
		}
	}

    // set the input type
    IMFMediaType* input_type = NULL;
    mf_result = self->decoder->GetInputAvailableType(0, 0, &input_type);
    if (MF_SUCCEEDED(mf_result)) {
        input_type->SetUINT32(MF_MT_INTERLACE_MODE , MFVideoInterlace_MixedInterlaceOrProgressive);
		MFSetAttributeSize(input_type, MF_MT_FRAME_SIZE, avctx->width, avctx->height);

        mf_result = self->decoder->SetInputType(0, input_type, 0);
        if (!MF_SUCCEEDED(mf_result)) {
            av_log(avctx, AV_LOG_WARNING, "SetInputType failed (%d)\n", mf_result);
        }

        input_type->Release();
    }

    // set the output type
    mf_video_decoder_set_output_type(self);

    // make sure the decoder is ready
    self->decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);

    return 0;
}

/*----------------------------------------------------------------*/
static int
mf_video_decoder_get_next_picture(AVCodecContext* avctx, AVFrame* frame)
{
    MF_VIDEO_DecoderContext* self  = (MF_VIDEO_DecoderContext*)avctx->priv_data;

	// check the output stream info
	MFT_OUTPUT_STREAM_INFO stream_info;
	HRESULT mf_result = self->decoder->GetOutputStreamInfo(0, &stream_info);
	if (!MF_SUCCEEDED(mf_result)) {
		av_log(avctx, AV_LOG_WARNING, "GetOutputStreamInfo failed (%d)\n", mf_result);
		return MF_VIDEO_DECODER_ERROR_INTERNAL;
	}

	// allocate a buffer for the output sample
	IMFSample*      sample        = NULL;
	IMFMediaBuffer* sample_buffer = NULL;
	if ((stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
		mf_result = MFCreateSample(&sample);

		if (MF_SUCCEEDED(mf_result)) {
			if (stream_info.cbAlignment > 0) {
				mf_result = MFCreateAlignedMemoryBuffer(stream_info.cbSize, stream_info.cbAlignment - 1, &sample_buffer);
			} else {
				mf_result = MFCreateMemoryBuffer(stream_info.cbSize, &sample_buffer);
			}
		}

		if (MF_SUCCEEDED(mf_result)) {
			mf_result = sample->AddBuffer(sample_buffer);
		}

		if (!MF_SUCCEEDED(mf_result)) {
			return MF_VIDEO_DECODER_FAILURE;
		}
	}

	// get the output sample
	MFT_OUTPUT_DATA_BUFFER output_buffer;
	DWORD status = 0;
	memset(&output_buffer, 0, sizeof(output_buffer));
	output_buffer.pSample = sample;
	mf_result = self->decoder->ProcessOutput(0, 1, &output_buffer, &status);
	if (mf_result == MF_E_TRANSFORM_STREAM_CHANGE) {
		mf_video_decoder_set_output_type(self);
		mf_result = self->decoder->ProcessOutput(0, 1, &output_buffer, &status);
	}
	int result = MF_VIDEO_DECODER_SUCCESS;
	if (mf_result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
		result = MF_VIDEO_DECODER_NEED_DATA;
		goto end;
	} else if (MF_FAILED(mf_result)) {
		av_log(avctx, AV_LOG_WARNING, "ProcessOutput returned %d\n", mf_result);
		result = MF_VIDEO_DECODER_FAILURE;
		goto end;
	}

	// start a new frame
    av_frame_unref(frame);
    
	// get the output type
	IMFMediaType* output_type = NULL;
	mf_result = self->decoder->GetOutputCurrentType(0, &output_type);
	if (MF_FAILED(mf_result)) {
		av_log(avctx, AV_LOG_WARNING, "GetOutputCurrentType returned %d\n", mf_result);
		result = MF_VIDEO_DECODER_FAILURE;
		goto end;
	}
	UINT32 width = 0;
	UINT32 height = 0;
	MFGetAttributeSize(output_type, MF_MT_FRAME_SIZE, &width, &height);
	if (width == 0 || height == 0) {
		av_log(avctx, AV_LOG_WARNING, "width=%d, height=%d, something is wrong\n", width, height);
		goto end;
	}

	// compute the image stride
	LONG stride = 0;
	mf_result = mf_video_decoder_get_stride(output_type, stride);
	if (MF_FAILED(mf_result)) {
		av_log(avctx, AV_LOG_WARNING, "mf_video_decoder_get_stride failed (%d)\n", mf_result);
		goto end;
	}
	if (stride <= 0) {
		av_log(avctx, AV_LOG_WARNING, "unsupported stride (%d)\n", (int)stride);
		goto end;
	}

    // setup the frame parameters
    frame->format  = AV_PIX_FMT_YUV420P;
    frame->width   = avctx->width;
    frame->height  = avctx->height;
    //frame->pts     = 
    //frame->pkt_pts = frame->pts;
    //frame->best_effort_timestamp = frame->pts;
    frame->reordered_opaque = avctx->reordered_opaque;
    
    // allocate the frame buffers
    avctx->pix_fmt = (AVPixelFormat)frame->format; // TODO: complain to the FFMPEG maintainers that get_buffer2 should use the frame fields and not the context fields
    int av_result = avctx->get_buffer2(avctx, frame, 0);
    if (av_result != 0) {
        av_log(avctx, AV_LOG_WARNING, "get_buffer2 failed (%d)\n", av_result);
        goto end;
    }

	// copy the data
	BYTE* sample_buffer_address = NULL;
	mf_result = sample_buffer->Lock(&sample_buffer_address, NULL, NULL);
	if (MF_SUCCEEDED(mf_result)) {
		unsigned int y_plane_size = stride * height;
		unsigned int uv_plane_size = y_plane_size / 4;
		BYTE* src;
		BYTE* dst;

		// copy the Y plane
		unsigned int src_step = stride;
		unsigned int dst_step = frame->linesize[0];
		src = sample_buffer_address;
		dst = frame->data[0];
		for (int y = 0; y < frame->height; y++) {
			memcpy(dst, src, frame->width);
			src += src_step;
			dst += dst_step;
		}

		// copy the U and V planes
		for (unsigned int p = 0; p < 2; p++) {
			src_step = stride / 2;
			dst_step = frame->linesize[1 + p];
			src = sample_buffer_address + y_plane_size + p*uv_plane_size;
			dst = frame->data[1 + p];
			for (int y = 0; y < frame->height / 2; y++) {
				memcpy(dst, src, frame->width / 2);
				src += src_step;
				dst += dst_step;
			}
		}
		sample_buffer->Unlock();
	}

#if defined(MF_VIDEO_DECODER_IDENTIFYING_MARK)
    // TEST: add pattern for debugging purposes
    {
        unsigned int x;
        unsigned int y;
        for (y=0; y<7; y++) {
            for (x=0; x<20; x++) {
                unsigned int xx;
                unsigned int yy;
                unsigned int pixel = 0;
                if (y>=1 && y<=5 && (x%5) >= 1 && (x%5) <= 3) {
                    unsigned int offset = ( (MF_VIDEO_DECODER_IDENTIFYING_MARK >> (12-4*(x/5))) & 0x0F);
                    unsigned char bits = DigitPatterns[offset][y-1];
                    if ((bits >> (3-(x%5))) & 1) {
                        pixel = 1;
                    }
                }
                for (yy=y*8; yy<(y+1)*8 && (int)yy<frame->height; yy++) {
                    for (xx=x*8; xx<(x+1)*8 && (int)xx<frame->linesize[0]; xx++) {
                        if (pixel == 0) frame->data[0][xx+yy*frame->linesize[0]] = 255-frame->data[0][xx+yy*frame->linesize[0]];
                    }
                }
            }
        }
    }
#endif

	// set the timestamp
	LONGLONG sample_time = 0;
	mf_result = sample->GetSampleTime(&sample_time);
	if (MF_SUCCEEDED(mf_result)) {
		AVRational nano_timebase = { 1, 1000000000 };
		frame->pts = av_rescale_q(sample_time, nano_timebase, avctx->pkt_timebase);
	}

	// copy the sample format in the context (shouldn't be needed, but some progs depend on it)
	avctx->sample_fmt = (AVSampleFormat)frame->format;

end:
	if (sample_buffer) sample_buffer->Release();
	if (sample)        sample->Release();

	return result;
}

/*----------------------------------------------------------------*/
static void
mf_video_decoder_queue_sample(AVCodecContext* avctx, void* data, AVPacket* input_packet)
{
	MF_VIDEO_DecoderContext* self = (MF_VIDEO_DecoderContext*)avctx->priv_data;

	// convert the input buffer to annexb format
#if defined(MF_VIDEO_DECODER_USE_DEPRECATED_BSF_API)
	uint8_t* filtered_buffer = NULL;
	int      filtered_buffer_size = 0;
	int av_result = av_bitstream_filter_filter(self->annexb_bsf_ctx, 
											   avctx, "private_spspps_buf", 
											   &filtered_buffer, 
											   &filtered_buffer_size, 
											   input_packet->data, 
											   input_packet->size, 
											   (input_packet->flags & AV_PKT_FLAG_KEY) ? 1 : 0);
	if (av_result < 0) {
		av_log(avctx, AV_LOG_WARNING, "av_bitstream_filter_filter failed (%d)\n", av_result);
		return;
	}

	AVPacket filtered_packet = { 0 };
	av_init_packet(&filtered_packet);
	filtered_packet.data = filtered_buffer;
	filtered_packet.size = filtered_buffer_size;
#else
	AVPacket workspace_packet = { 0 };
	av_packet_ref(&workspace_packet, input_packet);
	int av_result = av_bsf_send_packet(self->annexb_bsf_ctx, &workspace_packet);
	if (av_result != 0) {
		av_log(avctx, AV_LOG_WARNING, "av_bsf_send_packet failed (%d)\n", av_result);
		av_packet_unref(&workspace_packet);
		return;
	}
	AVPacket filtered_packet = { 0 };
	av_result = av_bsf_receive_packet(self->annexb_bsf_ctx, &filtered_packet);
	if (av_result < 0) {
		av_log(avctx, AV_LOG_WARNING, "av_bsf_receive_packet failed (%d)\n", av_result);
		return;
	}
#endif

	// create a sample
	IMFSample* sample = NULL;
	HRESULT mf_result = MFCreateSample(&sample);

	IMFMediaBuffer* buffer = NULL;
	mf_result = MFCreateMemoryBuffer(filtered_packet.size, &buffer);

	BYTE* buffer_address = NULL;
	if (MF_SUCCEEDED(mf_result)) {
		mf_result = buffer->Lock(&buffer_address, NULL, NULL);
	}

	if (MF_SUCCEEDED(mf_result)) {
		memcpy(buffer_address, filtered_packet.data, filtered_packet.size);
		mf_result = buffer->Unlock();
	}

	if (MF_SUCCEEDED(mf_result)) {
		mf_result = buffer->SetCurrentLength(filtered_packet.size);
	}

	if (MF_SUCCEEDED(mf_result)) {
		mf_result = sample->AddBuffer(buffer);
		buffer->Release();
	}

	// set the timestamp
	if (input_packet->pts >= 0) {
		AVRational nano_timebase = {
			1, 1000000000
		};
		LONGLONG pts = (LONGLONG)av_rescale_q(input_packet->pts, avctx->pkt_timebase, nano_timebase);
		sample->SetSampleTime(pts);
	}

	// queue the sample
	if (MF_SUCCEEDED(mf_result)) {
		self->samples->push(sample);
		++self->input_frame_count;
		av_log(avctx, AV_LOG_TRACE, "sample queued (%lld total)\n", self->input_frame_count);
	}

	// cleanup
	av_free(filtered_buffer);
	av_packet_unref(&filtered_packet);
}

/*----------------------------------------------------------------*/
static int
mf_video_decoder_decode(AVCodecContext* avctx, void* data, int* got_frame, AVPacket* input_packet)
{
	MF_VIDEO_DecoderContext* self = (MF_VIDEO_DecoderContext*)avctx->priv_data;
	AVFrame*                 frame = (AVFrame*)data;

	av_log(avctx, AV_LOG_TRACE, "mf_video_decoder_decode - size=%d, dts=%lld, pts=%lld\n", input_packet->size, input_packet->dts, input_packet->pts);

	// default return value
	*got_frame = 0;

	// init the frame
	av_frame_unref(frame);

	// check that we have a decoder
	if (!self->decoder) {
		return -1;
	}

	// enqueue a sample unless the input packet is empty
	if (input_packet->size) {
		mf_video_decoder_queue_sample(avctx, data, input_packet);
	}

	// check if we've reached the end of the stream
	if (input_packet->size == 0 && self->samples->empty()) {
		// we've decoded everything
		if (!self->draining) {
			av_log(avctx, AV_LOG_TRACE, "end of stream, start draining\n");
			self->decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
			self->decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
			self->draining = true;
		}
	}

	// process the input data
	for (;;) {
		// get an output picture if one is ready
		int result = mf_video_decoder_get_next_picture(avctx, frame);
		if (result == MF_VIDEO_DECODER_SUCCESS) {
			*got_frame = 1;
			++self->output_frame_count;
			av_log(avctx, AV_LOG_TRACE, "got frame (%lld total)\n", self->output_frame_count);
			break;
		} else if (result != MF_VIDEO_DECODER_NEED_DATA) {
			av_log(avctx, AV_LOG_WARNING, "mf_video_decoder_get_next_picture returned %d\n", result);
			return -1;
		}

		// feed more data if we can
		if (self->samples->empty()) {
			// no more data to feed
			av_log(avctx, AV_LOG_TRACE, "sample queue empty\n");
			break;
		}
		IMFSample* sample = self->samples->front();
		HRESULT mf_result = self->decoder->ProcessInput(0, sample, 0);
		if (MF_SUCCEEDED(mf_result)) {
			self->samples->pop();
			sample->Release();
		} else {
			if (mf_result = MF_E_NOTACCEPTING) {
				av_log(avctx, AV_LOG_TRACE, "MF decoder input full\n");
			} else {
				av_log(avctx, AV_LOG_WARNING, "ProcessInput failed (%d)\n", mf_result);

				// discard the sample
				self->samples->pop();
				sample->Release();
			}
			break;
		}
	}

	return input_packet->size;
}

/*----------------------------------------------------------------*/
static void
mf_video_decoder_flush(AVCodecContext* avctx)
{
	MF_VIDEO_DecoderContext* self = (MF_VIDEO_DecoderContext*)avctx->priv_data;

	// flush the decoder
    self->decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH , 0);

	// flush all buffered samples
	mf_video_decoder_flush_samples(avctx);

	// reset the draining state
	self->draining = false;
}

/*----------------------------------------------------------------
| H.264 module definitions
+----------------------------------------------------------------*/
static const AVOption mf_video_decoder_options[] = {
    { NULL }
};

static const AVClass mf_video_decoder_class = {
    "media foundation h.264 decoder", av_default_item_name, mf_video_decoder_options, MF_VIDEO_DECODER_VERSION
};

extern "C" {
AVCodec ff_h264_mf_video_decoder = {
    /* .name                  = */ "h264_mf",
    /* .long_name             = */ "H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 [Media Foundation]",
    /* .type                  = */ AVMEDIA_TYPE_VIDEO,
    /* .id                    = */ AV_CODEC_ID_H264,
    /* .capabilities          = */ AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY,
    /* .supported_framerates  = */ NULL,
    /* .pix_fmts              = */ NULL,
    /* .supported_samplerates = */ NULL,
    /* .sample_fmts           = */ NULL,
    /* .channel_layouts       = */ NULL,
    /* .max_lowres            = */ 0,
    /* .priv_class            = */ &mf_video_decoder_class,
    /* .profiles              = */ mf_video_decoder_profiles,
    /* .priv_data_size        = */ sizeof(MF_VIDEO_DecoderContext),
    /* .next                  = */ NULL,
    /* .init_thread_copy      = */ NULL,
    /* .update_thread_context = */ NULL,
    /* .defaults              = */ NULL,
    /* .init_static_data      = */ NULL,
    /* .init                  = */ mf_video_decoder_init,
    /* .encode_sub            = */ NULL,
    /* .encode2               = */ NULL,
    /* .decode                = */ mf_video_decoder_decode,
    /* .close                 = */ mf_video_decoder_close,
#if 0 // those are only in ffmpeg 3.1
    /* .send_frame            = */ NULL,
    /* .send_packet           = */ NULL,
    /* .receive_frame         = */ NULL,
    /* .receive_packet        = */ NULL,
#endif
	/* .flush                 = */ mf_video_decoder_flush,
    /* .caps_internal         = */ 0
};
}

