/*----------------------------------------------------------------
| config
+----------------------------------------------------------------*/
/* undefine this to get rid of the identifying mark in the pictures  */
/* or define it to a max-4 digit number encoded as hex-coded decimal */
/* (don't use hex chars A-F)                                         */
//#define VT_H264_DECODER_IDENTIFYING_MARK 0x1236

/*----------------------------------------------------------------
| includes
+----------------------------------------------------------------*/
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libavutil/opt.h"
#include <VideoToolbox/VideoToolbox.h>

/*----------------------------------------------------------------
| types
+----------------------------------------------------------------*/
typedef struct VT_H264DecoderContext {
    AVClass*        av_class;
    AVCodecContext* avctx;

    CMVideoFormatDescriptionRef input_format;
    VTDecompressionSessionRef   session;
    AVFrame*                    frame;
    CFBooleanRef                is_hardware_accelerated;
} VT_H264DecoderContext;

/*----------------------------------------------------------------
| constants
+----------------------------------------------------------------*/
#define VT_H264_DECODER_VERSION 0x010000

static const AVProfile vt_h264_decoder_profiles[] = {
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

#if defined(VT_H264_DECODER_IDENTIFYING_MARK)
static const unsigned char DigitPatterns[10][5] = {
    {7,5,5,5,7},
    {2,2,2,2,2},
    {7,1,7,4,7},
    {7,1,3,1,7},
    {4,5,7,1,1},
    {7,4,7,1,7},
    {4,4,7,5,7},
    {7,1,1,1,1},
    {7,5,7,5,7},
    {7,5,7,1,1}
};
#endif

/*----------------------------------------------------------------*/
static void
vt_h264_decoder_flush(AVCodecContext* avctx)
{
    av_log(avctx, AV_LOG_TRACE, "vt_h264_decoder_flush\n");
}

/*----------------------------------------------------------------*/
static void
vt_h264_decoder_free_context(VT_H264DecoderContext* self)
{
    av_log(self->avctx, AV_LOG_TRACE, "vt_h264_decoder_free_context\n");
    
    if (self) {
        if (self->input_format) {
            CFRelease(self->input_format);
        }
        
        if (self->session) {
            CFRelease(self->session);
        }
        
        if (self->is_hardware_accelerated) {
            CFRelease(self->is_hardware_accelerated);
        }
    }
}

/*----------------------------------------------------------------*/
static int
vt_h264_decoder_close(AVCodecContext* avctx)
{
    VT_H264DecoderContext* self = avctx->priv_data;

    av_log(avctx, AV_LOG_TRACE, "vt_h264_decoder_close\n");

    if (self->session) {
        VTDecompressionSessionInvalidate(self->session);
    }
    
    vt_h264_decoder_free_context(self);

    return 0;
}

/*----------------------------------------------------------------*/
static void
vt_h264_decoder_callback(void*             _self,
                         void*             sourceFrameRefCon,
                         OSStatus          status,
                         VTDecodeInfoFlags flags,
                         CVImageBufferRef  image_buffer,
                         CMTime            pts,
                         CMTime            duration)
{
    VT_H264DecoderContext* self = (VT_H264DecoderContext*)_self;
    OSType                 pixel_format;
    CGSize                 image_size;
    AVFrame*               frame = self->frame;
    CVReturn               result;
    int                    ff_result;

    av_log(self->avctx, AV_LOG_TRACE, "vt_h264_decoder_callback, pts=%f\n", pts.timescale?((double)pts.value/(double)pts.timescale) : 0.0);
    
    
    // start a new frame
    av_frame_unref(frame);
    
    // null out the frame field in case we don't produce a frame
    self->frame = NULL;
    
    // read picture parameters
    image_size   = CVImageBufferGetEncodedSize(image_buffer);
    pixel_format = CVPixelBufferGetPixelFormatType(image_buffer);
    
    // check if we can handle this data
    if (pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
        av_log(self->avctx, AV_LOG_WARNING, "pixel format not supported (%x)\n", pixel_format);
        return;
    }
    
    // setup the frame parameters
    frame->format  = AV_PIX_FMT_YUV420P; // AV_PIX_FMT_NV12 would be the matching native format, but not all client will like that
    frame->width   = (unsigned int)image_size.width;
    frame->height  = (unsigned int)image_size.height;
    frame->pts     = (int64_t)pts.value; // because we've hardcoded the timescale to 1
    frame->pkt_pts = frame->pts;
    frame->best_effort_timestamp = frame->pts;
    frame->reordered_opaque = self->avctx->reordered_opaque;
    
    // allocate the frame buffers
    self->avctx->pix_fmt = frame->format; // TODO: complain to the FFMPEG maintainers that get_buffer2 should use the frame fields and not the context fields
    ff_result = self->avctx->get_buffer2(self->avctx, frame, 0);
    if (ff_result != 0) {
        av_log(self->avctx, AV_LOG_WARNING, "get_buffer2 failed (%d)\n", ff_result);
        return;
    }
    
    // lock the buffer
    result = CVPixelBufferLockBaseAddress(image_buffer, kCVPixelBufferLock_ReadOnly);
    if (result != kCVReturnSuccess) {
        av_log(self->avctx, AV_LOG_WARNING, "cannot lock buffer (%d)\n", result);
        return;
    }

    // copy the data
    {
        unsigned int   y_in_bpr     = (unsigned int)CVPixelBufferGetBytesPerRowOfPlane(image_buffer, 0);
        const uint8_t* y_in         = CVPixelBufferGetBaseAddressOfPlane(image_buffer, 0);
        uint8_t*       y_out        = frame->data[0];
        unsigned int   cb_cr_in_bpr = (unsigned int)CVPixelBufferGetBytesPerRowOfPlane(image_buffer, 1);
        unsigned int   x,y;

        for (y=0; y<frame->height; y++) {
            memcpy(y_out, y_in, y_in_bpr);
            y_out += frame->linesize[0];
            y_in  += y_in_bpr;
        }
        
        for (y=0; y<frame->height/2; y++) {
            const uint8_t* cb_cr_in = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(image_buffer, 1)+y*cb_cr_in_bpr;
            uint8_t*       cb_out   = frame->data[1]+y*frame->linesize[1];
            uint8_t*       cr_out   = frame->data[2]+y*frame->linesize[2];
            for (x=0; x<cb_cr_in_bpr; x++) {
                *cb_out++ = *cb_cr_in++;
                *cr_out++ = *cb_cr_in++;
            }
        }
    }
    
    // unlock the buffer
    CVPixelBufferUnlockBaseAddress(image_buffer, kCVPixelBufferLock_ReadOnly);

#if defined(VT_H264_DECODER_IDENTIFYING_MARK)
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
                    unsigned int offset = ( (VT_H264_DECODER_IDENTIFYING_MARK >> (12-4*(x/5))) & 0x0F) % 10;
                    unsigned char bits = DigitPatterns[offset][y-1];
                    if ((bits >> (3-(x%5))) & 1) {
                        pixel = 1;
                    }
                }
                for (yy=y*8; yy<(y+1)*8 && yy<frame->height; yy++) {
                    for (xx=x*8; xx<(x+1)*8 && xx<frame->linesize[0]; xx++) {
                        if (pixel == 0) frame->data[0][xx+yy*frame->linesize[0]] = 255-frame->data[0][xx+yy*frame->linesize[0]];
                    }
                }
            }
        }
    }
#endif

    // all is good
    self->frame = frame;
}

/*----------------------------------------------------------------*/
static int
vt_h264_decoder_init(AVCodecContext* avctx)
{
    VT_H264DecoderContext*              self = avctx->priv_data;
    CFMutableDictionaryRef              decoder_spec = NULL;

    int32_t                             width = (int32_t)avctx->width;
    int32_t                             height = (int32_t)avctx->height;
    uint32_t                            pixel_format = 0;
    CFMutableDictionaryRef              input_format_extensions = NULL;

    CFMutableDictionaryRef              buffer_attributes = NULL;
    CFMutableDictionaryRef              io_surface_properties = NULL;
    CFNumberRef                         pixel_format_n = NULL;
    CFNumberRef                         width_n = NULL;
    CFNumberRef                         height_n = NULL;
    CFDictionaryRef                     output_format = NULL;

    VTDecompressionOutputCallbackRecord decoder_callback;
    OSStatus                            status;
    int                                 result = 0;

    av_log(avctx, AV_LOG_TRACE, "vt_h264_decoder_init\n");

    self->avctx = avctx;

    // create a dictionary for the input format extensions
    input_format_extensions = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                        1,
                                                        &kCFTypeDictionaryKeyCallBacks,
                                                        &kCFTypeDictionaryValueCallBacks);
    // add the avc info
    if (avctx->extradata_size && avctx->extradata) {
        CFMutableDictionaryRef avc_info = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                                    1,
                                                                    &kCFTypeDictionaryKeyCallBacks,
                                                                    &kCFTypeDictionaryValueCallBacks);
        CFDataRef avc_data = CFDataCreate(kCFAllocatorDefault, avctx->extradata, avctx->extradata_size);;

        CFDictionarySetValue(avc_info, CFSTR("avcC"), avc_data);
        CFDictionarySetValue(input_format_extensions,
                             kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms,
                             avc_info);

        CFRelease(avc_data);
        CFRelease(avc_info);
    }
    
    // create the input format description
    pixel_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    status = CMVideoFormatDescriptionCreate(kCFAllocatorDefault,
                                            kCMVideoCodecType_H264,
                                            width,
                                            height,
                                            input_format_extensions,
                                            &self->input_format);
    if (status != noErr) {
        av_log(avctx, AV_LOG_ERROR, "CMVideoFormatDescriptionCreate failed (%d)\n", status);
        result = AVERROR_INVALIDDATA;
        goto end;
    }
    
    // create the output format description
    width_n           = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &width);
    height_n          = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &height);
    pixel_format_n    = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format);
    buffer_attributes = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                  4,
                                                  &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    io_surface_properties = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                      0,
                                                      &kCFTypeDictionaryKeyCallBacks,
                                                      &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(buffer_attributes, kCVPixelBufferPixelFormatTypeKey, pixel_format_n);
    CFDictionarySetValue(buffer_attributes, kCVPixelBufferIOSurfacePropertiesKey, io_surface_properties);
    CFDictionarySetValue(buffer_attributes, kCVPixelBufferWidthKey, width_n);
    CFDictionarySetValue(buffer_attributes, kCVPixelBufferHeightKey, height_n);

    // create a decoder spec dictionary
    decoder_spec = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                              1,
                                                              &kCFTypeDictionaryKeyCallBacks,
                                                              &kCFTypeDictionaryValueCallBacks);
    
    // enable hardware decoding (don't require it, we still want to have software decoding as a fallback
    CFDictionarySetValue(decoder_spec,
                         CFSTR("EnableHardwareAcceleratedVideoDecoder"), // kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder
                         kCFBooleanTrue);


    // create the decoding session
    decoder_callback.decompressionOutputCallback = vt_h264_decoder_callback;
    decoder_callback.decompressionOutputRefCon   = self;
    status = VTDecompressionSessionCreate(NULL,               // allocator
                                          self->input_format, // videoFormatDescription
                                          decoder_spec,       // videoDecoderSpecification
                                          output_format,      // destinationImageBufferAttributes
                                          &decoder_callback,  // outputCallback
                                          &self->session);    // decompressionSessionOut
    if (status != noErr) {
        av_log(avctx, AV_LOG_ERROR, "VTDecompressionSessionCreate failed (%d)\n", status);
        result = AVERROR_EXTERNAL;
        goto end;
    }
    
    // query some properties to check what decoder was instantiated
    status = VTSessionCopyProperty(self->session,
                                   CFSTR("UsingHardwareAcceleratedVideoDecoder"), // kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder
                                   kCFAllocatorDefault,
                                   &self->is_hardware_accelerated);
    if (status == noErr && self->is_hardware_accelerated != NULL) {
        av_log(avctx, AV_LOG_INFO, "VTDecompressionSession hadrware acceleration: %s\n", self->is_hardware_accelerated == kCFBooleanTrue ? "yes" : "no");
    }
    
end:
    // cleanup
    if (width_n)               CFRelease(width_n);
    if (height_n)              CFRelease(height_n);
    if (pixel_format_n)        CFRelease(pixel_format_n);
    if (buffer_attributes)     CFRelease(buffer_attributes);
    if (io_surface_properties) CFRelease(io_surface_properties);
    if (decoder_spec)          CFRelease(decoder_spec);
    if (output_format)         CFRelease(output_format);
    
    return result;
}

/*----------------------------------------------------------------*/
static int
vt_h264_decoder_decode(AVCodecContext* avctx, void* data, int* got_frame, AVPacket* avpkt)
{
    VT_H264DecoderContext* self = avctx->priv_data;
    CMSampleBufferRef      sample_buffer = NULL;
    CMSampleTimingInfo     sample_timing;
    CMBlockBufferRef       block_buffer = NULL;
    OSStatus               status;
    int                    result = 0;

    av_log(avctx, AV_LOG_TRACE, "h264_decoder_decode - dts=%lld, pts=%lld\n", avpkt->dts, avpkt->pts);

    // default return value
    *got_frame = 0;

    // return now if the input buffer is empty
    if (avpkt == NULL || avpkt->data == NULL || avpkt->size == 0) {
        return 0;
    }
    
// keep track of the output frame for the callback
    self->frame = (AVFrame*)data;
    
    // setup the sample timing
    sample_timing.duration.flags                  = 0;
    sample_timing.duration.epoch                  = 0;
    sample_timing.duration.value                  = 0;
    sample_timing.duration.timescale              = 0;
    sample_timing.decodeTimeStamp.flags           = kCMTimeFlags_Valid;
    sample_timing.decodeTimeStamp.epoch           = 0;
    sample_timing.decodeTimeStamp.value           = avpkt->dts >= 0 ? avpkt->dts : 0;
    sample_timing.decodeTimeStamp.timescale       = 1;
    sample_timing.presentationTimeStamp.flags     = kCMTimeFlags_Valid;
    sample_timing.presentationTimeStamp.epoch     = 0;
    sample_timing.presentationTimeStamp.value     = avpkt->pts >= 0 ? avpkt->pts : 0;
    sample_timing.presentationTimeStamp.timescale = 1;
    
    // create a memory block to hold the coded data
    status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, // structureAllocator
                                                avpkt->data,         // memoryBlock
                                                avpkt->size,         // blockLength
                                                kCFAllocatorNull,    // blockAllocator
                                                NULL,                // customBlockSource
                                                0,                   // offsetToData
                                                avpkt->size,         // dataLength
                                                0,                   // flags
                                                &block_buffer);

    if (status != noErr) {
        av_log(avctx, AV_LOG_ERROR, "CMBlockBufferCreateWithMemoryBlock failed (%d)\n", status);
        return -1;
    }
    status = CMSampleBufferCreate(kCFAllocatorDefault,  // allocator
                                  block_buffer,         // dataBuffer
                                  TRUE,                 // dataReady
                                  0,                    // makeDataReadyCallback
                                  0,                    // makeDataReadyRefcon
                                  self->input_format,   // formatDescription
                                  1,                    // numSamples
                                  1,                    // numSampleTimingEntries
                                  &sample_timing,       // sampleTimingArray
                                  0,                    // numSampleSizeEntries
                                  NULL,                 // sampleSizeArray
                                  &sample_buffer);
    if (status != noErr) {
        av_log(avctx, AV_LOG_ERROR, "CMSampleBufferCreate failed (%d)\n", status);
        CFRelease(block_buffer);
        return -1;
    }

    if (block_buffer) CFRelease(block_buffer);

    // decode the coded data
    status = VTDecompressionSessionDecodeFrame(self->session,
                                               sample_buffer,
                                               0,       // decodeFlags
                                               NULL,    // sourceFrameRefCon
                                               0);      // infoFlagsOut
    if (status == noErr) {
        status = VTDecompressionSessionWaitForAsynchronousFrames(self->session);
        result = avpkt->size;
    } else {
        av_log(avctx, AV_LOG_ERROR, "VTDecompressionSessionDecodeFrame failed (%d)\n", status);
        result = -1;
    }
    CFRelease(sample_buffer);
    
    // check if we have decoded a frame
    if (self->frame) {
        *got_frame = 1;
    }

    // done
    return result;
}

/*----------------------------------------------------------------
| module definitions
+----------------------------------------------------------------*/
static const AVOption vt_h264_decoder_options[] = {
    { NULL },
};

static const AVClass vt_h264_decoder_class = {
    .class_name = "H264 VideoToolbox Decoder",
    .item_name  = av_default_item_name,
    .option     = vt_h264_decoder_options,
    .version    = VT_H264_DECODER_VERSION,
};

AVCodec ff_h264_videotoolbox_decoder = {
    .name                  = "h264_videotoolbox",
    .long_name             = "H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 [VideoToolbox]",
    .type                  = AVMEDIA_TYPE_VIDEO,
    .id                    = AV_CODEC_ID_H264,
    .priv_data_size        = sizeof(VT_H264DecoderContext),
    .init                  = vt_h264_decoder_init,
    .close                 = vt_h264_decoder_close,
    .decode                = vt_h264_decoder_decode,
    .capabilities          = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY,
    .flush                 = vt_h264_decoder_flush,
    .profiles              = vt_h264_decoder_profiles,
    .priv_class            = &vt_h264_decoder_class,
};
