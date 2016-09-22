/*----------------------------------------------------------------
| config
+----------------------------------------------------------------*/
/* undefine this to get rid of the identifying sound in the audio         */
/* or define it to an array of small integers to make a frequency pattern */
/* like for example: {1,2,1,3}                                            */
//#define AT_DECODER_ADD_IDENTIFYING_SOUND {2,3,2,4}

/*----------------------------------------------------------------
| includes
+----------------------------------------------------------------*/
#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavcodec/avcodec.h"

#if !defined(__COREAUDIO_USE_FLAT_INCLUDES__)
    #include <AudioToolbox/AudioToolbox.h>
    #include <CoreFoundation/CoreFoundation.h>
#else
    #include "AudioToolbox.h"
    #include "CoreFoundation.h"
#endif

/*----------------------------------------------------------------
| constants
+----------------------------------------------------------------*/
#define AT_AAC_DECODER_VERSION 0x010000
#define AT_MP3_DECODER_VERSION 0x010000

#define AT_AUDIO_CONVERTER_DECODER_AAC_PACKETS_PER_CONVERSION 1024
#define AT_AUDIO_CONVERTER_DECODER_MP3_PACKETS_PER_CONVERSION 1152
#define AT_AUDIO_CONVERTER_DECODER_MAX_OUTPUT_BUFFER_SIZE     (2048*2*8) /* max 2048 samples, 8 channels @ 16 bits */
#define AT_AUDIO_CONVERTER_DATA_UNDERFLOW_ERROR               1234       /* arbitrary error code */

#define AT_MPEG_AUDIO_OBJECT_TYPE_AAC_MAIN              1  /**< AAC Main Profile                             */
#define AT_MPEG_AUDIO_OBJECT_TYPE_AAC_LC                2  /**< AAC Low Complexity                           */
#define AT_MPEG_AUDIO_OBJECT_TYPE_AAC_SSR               3  /**< AAC Scalable Sample Rate                     */
#define AT_MPEG_AUDIO_OBJECT_TYPE_AAC_LTP               4  /**< AAC Long Term Predictor                      */
#define AT_MPEG_AUDIO_OBJECT_TYPE_SBR                   5  /**< Spectral Band Replication                    */
#define AT_MPEG_AUDIO_OBJECT_TYPE_AAC_SCALABLE          6  /**< AAC Scalable                                 */
#define AT_MPEG_AUDIO_OBJECT_TYPE_TWINVQ                7  /**< Twin VQ                                      */
#define AT_MPEG_AUDIO_OBJECT_TYPE_CELP                  8  /**< CELP                                         */
#define AT_MPEG_AUDIO_OBJECT_TYPE_HVXC                  9  /**< HVXC                                         */
#define AT_MPEG_AUDIO_OBJECT_TYPE_TTSI                  12 /**< TTSI                                         */
#define AT_MPEG_AUDIO_OBJECT_TYPE_MAIN_SYNTHETIC        13 /**< Main Synthetic                               */
#define AT_MPEG_AUDIO_OBJECT_TYPE_WAVETABLE_SYNTHESIS   14 /**< WavetableSynthesis                           */
#define AT_MPEG_AUDIO_OBJECT_TYPE_GENERAL_MIDI          15 /**< General MIDI                                 */
#define AT_MPEG_AUDIO_OBJECT_TYPE_ALGORITHMIC_SYNTHESIS 16 /**< Algorithmic Synthesis                        */
#define AT_MPEG_AUDIO_OBJECT_TYPE_ER_AAC_LC             17 /**< Error Resilient AAC Low Complexity           */
#define AT_MPEG_AUDIO_OBJECT_TYPE_ER_AAC_LTP            19 /**< Error Resilient AAC Long Term Prediction     */
#define AT_MPEG_AUDIO_OBJECT_TYPE_ER_AAC_SCALABLE       20 /**< Error Resilient AAC Scalable                 */
#define AT_MPEG_AUDIO_OBJECT_TYPE_ER_TWINVQ             21 /**< Error Resilient Twin VQ                      */
#define AT_MPEG_AUDIO_OBJECT_TYPE_ER_BSAC               22 /**< Error Resilient Bit Sliced Arithmetic Coding */
#define AT_MPEG_AUDIO_OBJECT_TYPE_ER_AAC_LD             23 /**< Error Resilient AAC Low Delay                */
#define AT_MPEG_AUDIO_OBJECT_TYPE_ER_CELP               24 /**< Error Resilient CELP                         */
#define AT_MPEG_AUDIO_OBJECT_TYPE_ER_HVXC               25 /**< Error Resilient HVXC                         */
#define AT_MPEG_AUDIO_OBJECT_TYPE_ER_HILN               26 /**< Error Resilient HILN                         */
#define AT_MPEG_AUDIO_OBJECT_TYPE_ER_PARAMETRIC         27 /**< Error Resilient Parametric                   */
#define AT_MPEG_AUDIO_OBJECT_TYPE_SSC                   28 /**< SSC                                          */
#define AT_MPEG_AUDIO_OBJECT_TYPE_PS                    29 /**< Parametric Stereo                            */
#define AT_MPEG_AUDIO_OBJECT_TYPE_MPEG_SURROUND         30 /**< MPEG Surround                                */
#define AT_MPEG_AUDIO_OBJECT_TYPE_LAYER_1               32 /**< MPEG Layer 1                                 */
#define AT_MPEG_AUDIO_OBJECT_TYPE_LAYER_2               33 /**< MPEG Layer 2                                 */
#define AT_MPEG_AUDIO_OBJECT_TYPE_LAYER_3               34 /**< MPEG Layer 3                                 */
#define AT_MPEG_AUDIO_OBJECT_TYPE_DST                   35 /**< DST Direct Stream Transfer                   */
#define AT_MPEG_AUDIO_OBJECT_TYPE_ALS                   36 /**< ALS Lossless Coding                          */
#define AT_MPEG_AUDIO_OBJECT_TYPE_SLS                   37 /**< SLS Scalable Lossless Coding                 */
#define AT_MPEG_AUDIO_OBJECT_TYPE_SLS_NON_CORE          38 /**< SLS Sclable Lossless Coding Non-Core         */
#define AT_MPEG_AUDIO_OBJECT_TYPE_ER_AAC_ELD            39 /**< Error Resilient AAC ELD                      */
#define AT_MPEG_AUDIO_OBJECT_TYPE_SMR_SIMPLE            40 /**< SMR Simple                                   */
#define AT_MPEG_AUDIO_OBJECT_TYPE_SMR_MAIN              41 /**< SMR Main                                     */

#define AT_DECODER_ADD_IDENTIFYING_SOUND_CYCLE  60
#define AT_DECODER_ADD_IDENTIFYING_SOUND_AMP    5000
#define AT_DECODER_ADD_IDENTIFYING_SOUND_REPEAT 40

#define AT_SUCCESS              ( 0)
#define AT_FAILURE              (-1)
#define AT_ERROR_INVALID_FORMAT (-2)
#define AT_ERROR_NOT_SUPPORTED  (-3)

#define AT_FAILED(_result) ((_result) != AT_SUCCESS)

#define AT_AAC_MAX_SAMPLING_FREQUENCY_INDEX  12
static const unsigned int AT_AacSamplingFreqTable[13] =
{
	96000, 88200, 64000, 48000, 
    44100, 32000, 24000, 22050, 
    16000, 12000, 11025, 8000, 
    7350
};

/*----------------------------------------------------------------
| types
+----------------------------------------------------------------*/
typedef struct {
    const AVClass *class;
    
    AudioConverterRef            converter;
    AVPacket*                    input_packet;
    AudioStreamPacketDescription input_packet_description;
    unsigned char                output_buffer[AT_AUDIO_CONVERTER_DECODER_MAX_OUTPUT_BUFFER_SIZE];
    unsigned int                 output_buffer_conversion_size;
    uint64_t                     packet_count;
} AT_DecoderContext;

typedef struct {
    const uint8_t*  data;
    unsigned int    data_size;
    unsigned int    position;
} AT_AudioConfigParser;

typedef enum {
    AT_CHANNEL_CONFIG_NONE   = 0,             /**< No channel (not used)       */
    AT_CHANNEL_CONFIG_MONO   = 1,             /**< Mono (single audio channel) */
    AT_CHANNEL_CONFIG_STEREO = 2,             /**< Stereo (Two audio channels) */
    AT_CHANNEL_CONFIG_STEREO_PLUS_CENTER = 3, /**< Stereo plus one center channel */
    AT_CHANNEL_CONFIG_STEREO_PLUS_CENTER_PLUS_REAR_MONO = 4, /**< Stereo plus one center and one read channel */
    AT_CHANNEL_CONFIG_FIVE = 5,               /**< Five channels */
    AT_CHANNEL_CONFIG_FIVE_PLUS_ONE = 6,      /**< Five channels plus one low frequency channel */
    AT_CHANNEL_CONFIG_SEVEN_PLUS_ONE = 7,     /**< Seven channels plus one low frequency channel */
    AT_CHANNEL_CONFIG_UNSUPPORTED
} AT_AudioChannelConfiguration;

typedef struct {
    uint8_t                      object_type;              /**< Type identifier for the audio data */
    unsigned int                 sampling_frequency_index; /**< Index of the sampling frequency in the sampling frequency table */
    unsigned int                 sampling_frequency;       /**< Sampling frequency */
    unsigned int                 channel_count;            /**< Number of audio channels */
    AT_AudioChannelConfiguration channel_configuration;    /**< Channel configuration */
    bool                         frame_length_flag;        /**< Frame Length Flag     */
    bool                         depends_on_core_coder;    /**< Depends on Core Coder */
    unsigned int                 core_coder_delay;         /**< Core Code delay       */
    
    /** extension details */
    struct {
        uint8_t         sbr_present;           /**< SBR is present        */
        uint8_t         ps_present;            /**< PS is present         */
        uint8_t         object_type;           /**< Extension object type */
        unsigned int sampling_frequency_index; /**< Sampling frequency index of the extension */
        unsigned int sampling_frequency;       /**< Sampling frequency of the extension */
    } extension;
} AT_AudioConfig;

/*----------------------------------------------------------------*/
static unsigned int
AT_AudioConfigParser_BitsLeft(AT_AudioConfigParser* self)
{
    return 8 * self->data_size - self->position;
}

/*----------------------------------------------------------------*/
static uint32_t
AT_AudioConfigParser_ReadBits(AT_AudioConfigParser* self, unsigned int n)
{
    uint32_t       result = 0;
    const uint8_t* data = self->data;
    while (n) {
        unsigned int bits_avail = 8-(self->position%8);
        unsigned int chunk_size = bits_avail >= n ? n : bits_avail;
        unsigned int chunk_bits = (((unsigned int)(data[self->position/8]))>>(bits_avail-chunk_size))&((1<<chunk_size)-1);
        result = (result << chunk_size) | chunk_bits;
        n -= chunk_size;
        self->position += chunk_size;
    }

    return result;
}

/*----------------------------------------------------------------*/
static int
AT_AudioConfig_ParseAudioObjectType(AT_AudioConfigParser* parser, uint8_t* object_type)
{
    if (AT_AudioConfigParser_BitsLeft(parser) < 5) return AT_ERROR_INVALID_FORMAT;
    *object_type = (uint8_t)AT_AudioConfigParser_ReadBits(parser, 5);
	if ((int)*object_type == 31) {
        if (AT_AudioConfigParser_BitsLeft(parser) < 6) return AT_ERROR_INVALID_FORMAT;
		*object_type = (uint8_t)(32 + AT_AudioConfigParser_ReadBits(parser, 6));
	}
	return AT_SUCCESS;
}

/*----------------------------------------------------------------*/
static int
AT_AudioConfig_ParseSamplingFrequency(AT_AudioConfigParser* parser,
                                             unsigned int*  sampling_frequency_index,
                                             unsigned int*  sampling_frequency)
{
    if (AT_AudioConfigParser_BitsLeft(parser) < 4) {
        return AT_ERROR_INVALID_FORMAT;
    }

    *sampling_frequency_index = AT_AudioConfigParser_ReadBits(parser, 4);
    if (*sampling_frequency_index == 0xF) {
        if (AT_AudioConfigParser_BitsLeft(parser) < 24) {
            return AT_ERROR_INVALID_FORMAT;
        }
        *sampling_frequency = AT_AudioConfigParser_ReadBits(parser, 24);
    } else if (*sampling_frequency_index <= AT_AAC_MAX_SAMPLING_FREQUENCY_INDEX) {
        *sampling_frequency = AT_AacSamplingFreqTable[*sampling_frequency_index];
    } else {
        *sampling_frequency = 0;
        return AT_ERROR_INVALID_FORMAT;
    }

    return AT_SUCCESS;
}

/*----------------------------------------------------------------*/
static int
AT_AudioConfig_ParseExtension(AT_AudioConfig* self, AT_AudioConfigParser* parser)
{
    unsigned int sync_extension_type = 0;
    
    if (AT_AudioConfigParser_BitsLeft(parser) < 16) return AT_ERROR_INVALID_FORMAT;
    
    sync_extension_type = AT_AudioConfigParser_ReadBits(parser, 11);
    if (sync_extension_type == 0x2b7) {
        int result = AT_AudioConfig_ParseAudioObjectType(parser, &self->extension.object_type);
        if (AT_FAILED(result)) return result;
        if (self->extension.object_type == AT_MPEG_AUDIO_OBJECT_TYPE_SBR) {
            self->extension.sbr_present = AT_AudioConfigParser_ReadBits(parser, 1);
            if (self->extension.sbr_present) {
                result = AT_AudioConfig_ParseSamplingFrequency(parser,
                                                               &self->extension.sampling_frequency_index,
                                                               &self->extension.sampling_frequency);
                if (AT_FAILED(result)) return result;
                if (AT_AudioConfigParser_BitsLeft(parser) >= 12) {
                    sync_extension_type = AT_AudioConfigParser_ReadBits(parser, 11);
                    if (sync_extension_type == 0x548) {
                        self->extension.ps_present = AT_AudioConfigParser_ReadBits(parser, 1);
                    }
                }
            }
        } else if (self->extension.object_type == AT_MPEG_AUDIO_OBJECT_TYPE_ER_BSAC) {
            self->extension.sbr_present = (AT_AudioConfigParser_ReadBits(parser, 1) == 1);
            if (self->extension.sbr_present) {
                result = AT_AudioConfig_ParseSamplingFrequency(parser,
                                                               &self->extension.sampling_frequency_index,
                                                               &self->extension.sampling_frequency);
                if (AT_FAILED(result)) return result;
            } 
            AT_AudioConfigParser_ReadBits(parser, 4); // extensionChannelConfiguration           
        }
    }
	return AT_SUCCESS;
}

/*----------------------------------------------------------------*/
static int
AT_AudioConfig_ParseGASpecificInfo(AT_AudioConfig* self, AT_AudioConfigParser* parser)
{
    unsigned int extension_flag = 0;

    if (AT_AudioConfigParser_BitsLeft(parser) < 2) return AT_ERROR_INVALID_FORMAT;
	self->frame_length_flag     = AT_AudioConfigParser_ReadBits(parser, 1);
	self->depends_on_core_coder = AT_AudioConfigParser_ReadBits(parser, 1);
	if (self->depends_on_core_coder) {
        if (AT_AudioConfigParser_BitsLeft(parser) < 14) return AT_ERROR_INVALID_FORMAT;
		self->core_coder_delay = AT_AudioConfigParser_ReadBits(parser, 14);
    } else {
        self->core_coder_delay = 0;
    }
    if (AT_AudioConfigParser_BitsLeft(parser) < 1) return AT_ERROR_INVALID_FORMAT;
	extension_flag = AT_AudioConfigParser_ReadBits(parser, 1);
	if (self->channel_configuration == AT_CHANNEL_CONFIG_NONE) {
		/*program_config_element (); */
        return AT_ERROR_NOT_SUPPORTED;
	}		
    if (self->object_type == AT_MPEG_AUDIO_OBJECT_TYPE_AAC_SCALABLE ||
        self->object_type == AT_MPEG_AUDIO_OBJECT_TYPE_ER_AAC_SCALABLE) {
        if (AT_AudioConfigParser_BitsLeft(parser) < 3) return AT_ERROR_INVALID_FORMAT;
        AT_AudioConfigParser_ReadBits(parser, 3); // layerNr
    }
    if (extension_flag) {
        if (self->object_type == AT_MPEG_AUDIO_OBJECT_TYPE_ER_BSAC) {
            if (AT_AudioConfigParser_BitsLeft(parser) < 16) return AT_ERROR_INVALID_FORMAT;
            AT_AudioConfigParser_ReadBits(parser, 16); // numOfSubFrame (5); layer_length (11)
        }
        if (self->object_type == AT_MPEG_AUDIO_OBJECT_TYPE_ER_AAC_LC       ||
            self->object_type == AT_MPEG_AUDIO_OBJECT_TYPE_ER_AAC_SCALABLE ||
            self->object_type == AT_MPEG_AUDIO_OBJECT_TYPE_ER_AAC_LD) {
            if (AT_AudioConfigParser_BitsLeft(parser) < 3) return AT_ERROR_INVALID_FORMAT;
            AT_AudioConfigParser_ReadBits(parser, 3); // aacSectionDataResilienceFlag (1)
                                                      // aacScalefactorDataResilienceFlag (1)
                                                      // aacSpectralDataResilienceFlag (1)
        }
        if (AT_AudioConfigParser_BitsLeft(parser) < 1) return AT_ERROR_INVALID_FORMAT;
        {
            unsigned int extension_flag_3 = AT_AudioConfigParser_ReadBits(parser, 1);
            if (extension_flag_3) {
                return AT_ERROR_NOT_SUPPORTED;
            }
        }
    }
    
    return AT_SUCCESS;
}

/*----------------------------------------------------------------*/
static int
AT_AudioConfig_Parse(AT_AudioConfig*      self,
                     const unsigned char* data,
                     unsigned int         data_size)
{
    int                  result;
    AT_AudioConfigParser bits;

    // setup the parser
    bits.data      = data;
    bits.data_size = data_size;
    bits.position  = 0;

    // default config
    memset(self, 0, sizeof(*self));
    
    // parse the audio object type
	result = AT_AudioConfig_ParseAudioObjectType(&bits, &self->object_type);
    if (AT_FAILED(result)) return result;

    // parse the sampling frequency
    result = AT_AudioConfig_ParseSamplingFrequency(&bits,
                                                   &self->sampling_frequency_index,
                                                   &self->sampling_frequency);
    if (AT_FAILED(result)) return result;

    if (AT_AudioConfigParser_BitsLeft(&bits) < 4) {
        return AT_ERROR_INVALID_FORMAT;
    }
	self->channel_configuration = (AT_AudioChannelConfiguration)AT_AudioConfigParser_ReadBits(&bits, 4);
    self->channel_count = (unsigned int)self->channel_configuration;
    if (self->channel_count == 7) {
        self->channel_count = 8;
    } else if (self->channel_count > 7) {
        self->channel_count = 0;
    }
    
	if (self->object_type == AT_MPEG_AUDIO_OBJECT_TYPE_SBR ||
        self->object_type == AT_MPEG_AUDIO_OBJECT_TYPE_PS) {
		self->extension.object_type = AT_MPEG_AUDIO_OBJECT_TYPE_SBR;
		self->extension.sbr_present = 1;
        self->extension.ps_present  = (self->object_type == AT_MPEG_AUDIO_OBJECT_TYPE_PS);
        result = AT_AudioConfig_ParseSamplingFrequency(&bits,
                                                       &self->extension.sampling_frequency_index,
                                                       &self->extension.sampling_frequency);
        if (AT_FAILED(result)) return result;
		result = AT_AudioConfig_ParseAudioObjectType(&bits, &self->object_type);
        if (AT_FAILED(result)) return result;
        if (self->object_type == AT_MPEG_AUDIO_OBJECT_TYPE_ER_BSAC) {
            if (AT_AudioConfigParser_BitsLeft(&bits)  < 4) return AT_ERROR_INVALID_FORMAT;
            AT_AudioConfigParser_ReadBits(&bits, 4); // extensionChannelConfiguration (4)
        }
	} else {
        self->extension.object_type              = 0;
        self->extension.sampling_frequency       = 0;
        self->extension.sampling_frequency_index = 0;
        self->extension.sbr_present              = 0;
        self->extension.ps_present               = 0;
    }
    
	switch (self->object_type) {
        case AT_MPEG_AUDIO_OBJECT_TYPE_AAC_MAIN:
        case AT_MPEG_AUDIO_OBJECT_TYPE_AAC_LC:
            result = AT_AudioConfig_ParseGASpecificInfo(self, &bits);
            if (result == AT_SUCCESS) {
                if (self->extension.object_type !=  AT_MPEG_AUDIO_OBJECT_TYPE_SBR &&
                    AT_AudioConfigParser_BitsLeft(&bits) >= 16) {
                    result = AT_AudioConfig_ParseExtension(self, &bits);
                }
            }
            if (result == AT_ERROR_NOT_SUPPORTED) {
                // not a fatal error
                result = AT_SUCCESS;
            }
            if (result != AT_SUCCESS) return result;
            break;

        default:
            return AT_ERROR_NOT_SUPPORTED;
    }

    return AT_SUCCESS;
}

/*----------------------------------------------------------------*/
static OSStatus
at_decoder_data_proc(AudioConverterRef              inAudioConverter,
                     UInt32*                        ioNumberDataPackets,
                     AudioBufferList*               ioData,
                     AudioStreamPacketDescription** outDataPacketDescription,
                     void*                          inUserData)
{
    AT_DecoderContext* self = (AT_DecoderContext*)inUserData;
 
    (void)inAudioConverter;
    
    // if we don't have data available, return nothing
    if (self->input_packet == NULL || self->input_packet->size == 0) {
        *ioNumberDataPackets = 0;
        return AT_AUDIO_CONVERTER_DATA_UNDERFLOW_ERROR;
    }
    
    // return one packet's worth of data
    ioData->mNumberBuffers              = 1;
    ioData->mBuffers[0].mNumberChannels = 0;
    ioData->mBuffers[0].mDataByteSize   = self->input_packet->size;
    ioData->mBuffers[0].mData           = self->input_packet->data;
    *ioNumberDataPackets = 1;
    if (outDataPacketDescription) {
        self->input_packet_description.mStartOffset            = 0;
        self->input_packet_description.mDataByteSize           = ioData->mBuffers[0].mDataByteSize;
        self->input_packet_description.mVariableFramesInPacket = 0;
        *outDataPacketDescription = &self->input_packet_description;
    }
    
    return noErr;
}

/*----------------------------------------------------------------*/
static int
at_decoder_close(AVCodecContext* avctx)
{
    AT_DecoderContext* self = avctx->priv_data;

    av_log(avctx, AV_LOG_TRACE, "at_decoder_close\n");

    if (self->converter) {
        AudioConverterDispose(self->converter);
    }
    
    return 0;
}

/*----------------------------------------------------------------*/
static int
at_decoder_init(AVCodecContext* avctx)
{
    AT_DecoderContext*          self = avctx->priv_data;
    AudioStreamBasicDescription source_format;
    AudioStreamBasicDescription dest_format;
    OSStatus                    status = noErr;
    
    av_log(avctx, AV_LOG_TRACE, "at_decoder_init\n");

    // setup the source format defaults
    memset(&source_format, 0, sizeof(source_format));
    source_format.mSampleRate       = (Float64)avctx->sample_rate;
    source_format.mFormatFlags      = 0;
    source_format.mBytesPerPacket   = 0;
    source_format.mFramesPerPacket  = 0;
    source_format.mBytesPerFrame    = 0;
    source_format.mChannelsPerFrame = avctx->channels;
    source_format.mBitsPerChannel   = 0;

    // be more specific about the format based on the codec we're using
    if (avctx->codec_id == AV_CODEC_ID_MP3) {
        av_log(avctx, AV_LOG_INFO, "AudioToolboxDecoder: MP3\n");

        self->output_buffer_conversion_size = AT_AUDIO_CONVERTER_DECODER_MP3_PACKETS_PER_CONVERSION;
        source_format.mFormatID = kAudioFormatMPEGLayer3;
    } else if (avctx->codec_id == AV_CODEC_ID_AAC) {
        av_log(avctx, AV_LOG_INFO, "AudioToolboxDecoder: AAC - extradata_size=%d\n", avctx->extradata_size);

        self->output_buffer_conversion_size = AT_AUDIO_CONVERTER_DECODER_AAC_PACKETS_PER_CONVERSION;
        source_format.mFormatID = kAudioFormatMPEG4AAC;

        if (avctx->extradata_size && avctx->extradata) {
            // if the decoder info is more than 2 bytes, do some more in-depth parsing
            if (avctx->extradata_size > 2) {
                AT_AudioConfig audio_config;
                int result = AT_AudioConfig_Parse(&audio_config, avctx->extradata, avctx->extradata_size);
                if (result == AT_SUCCESS) {
                    if (audio_config.extension.sbr_present || audio_config.extension.ps_present) {
                        self->output_buffer_conversion_size *= 2;
                    }
                    if (audio_config.extension.sbr_present) {
                        av_log(avctx, AV_LOG_INFO, "HE-AAC SBR detected\n");
                        source_format.mFormatID = kAudioFormatMPEG4AAC_HE;
                    }
                    if (audio_config.extension.ps_present) {
                        av_log(avctx, AV_LOG_INFO, "HE-AAC PS detected\n");
                        source_format.mFormatID = kAudioFormatMPEG4AAC_HE_V2;
                    }
                } else {
                    av_log(avctx, AV_LOG_WARNING, "failed to parse audio decoder config (%d)\n", result);
                }
            }
        }
    } else {
        return AVERROR_BUG;
    }
    
    // setup the dest format
    memset(&dest_format, 0, sizeof(dest_format));
    dest_format.mFormatID         = kAudioFormatLinearPCM;
    dest_format.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    dest_format.mSampleRate       = 0;
    dest_format.mBytesPerPacket   = 0;
    dest_format.mFramesPerPacket  = 1;
    dest_format.mBytesPerFrame    = 0;
    dest_format.mChannelsPerFrame = 2;
    dest_format.mBitsPerChannel   = 16;
                   
    status = AudioConverterNew(&source_format,
                               &dest_format,
                               &self->converter);
    if (status != noErr) {
        av_log(avctx, AV_LOG_WARNING, "AudioConverterNew failed (%d)\n", (int)status);
        return AVERROR_EXTERNAL;
    }
    
    // setup codec-specific parameters
    if (avctx->codec_id == AV_CODEC_ID_AAC) {
        unsigned int   magic_cookie_size = avctx->extradata_size+25;
        unsigned char* magic_cookie = av_malloc(magic_cookie_size);

        /* construct the content of the magic cookie (the 'ES Descriptor') */
        magic_cookie[ 0] = 0x03;                 /* ES_Descriptor tag */
        magic_cookie[ 1] = magic_cookie_size-2;  /* ES_Descriptor payload size */
        magic_cookie[ 2] = 0;                    /* ES ID */      
        magic_cookie[ 3] = 0;                    /* ES ID */
        magic_cookie[ 4] = 0;                    /* flags */
        magic_cookie[ 5] = 0x04;                 /* DecoderConfig tag */
        magic_cookie[ 6] = magic_cookie_size-10; /* DecoderConfig payload size */
        magic_cookie[ 7] = 0x40;                 /* object type */
        magic_cookie[ 8] = 0x05<<2 | 1;          /* stream type | reserved */
        magic_cookie[ 9] = 0;                    /* buffer size */
        magic_cookie[10] = 0x18;                 /* buffer size */
        magic_cookie[11] = 0;                    /* buffer size */
        magic_cookie[12] = 0;                    /* max bitrate */
        magic_cookie[13] = 0x08;                 /* max bitrate */
        magic_cookie[14] = 0;                    /* max bitrate */
        magic_cookie[15] = 0;                    /* max bitrate */
        magic_cookie[16] = 0;                    /* avg bitrate */
        magic_cookie[17] = 0x04;                 /* avg bitrate */
        magic_cookie[18] = 0;                    /* avg bitrate */
        magic_cookie[19] = 0;                    /* avg bitrate */
        magic_cookie[20] = 0x05;                 /* DecoderSpecificInfo tag */
        magic_cookie[21] = avctx->extradata_size; /* DecoderSpecificInfo payload size */
        if (avctx->extradata_size) {
            memcpy(&magic_cookie[22], avctx->extradata, avctx->extradata_size);
        }
        magic_cookie[22+avctx->extradata_size  ] = 0x06; /* SLConfigDescriptor tag    */
        magic_cookie[22+avctx->extradata_size+1] = 0x01; /* SLConfigDescriptor length */
        magic_cookie[22+avctx->extradata_size+2] = 0x02; /* fixed                     */

        status = AudioConverterSetProperty(self->converter,
                                           kAudioConverterDecompressionMagicCookie,
                                           magic_cookie_size,
                                           magic_cookie);
        av_free(magic_cookie);
        if (status != noErr) {
            av_log(avctx, AV_LOG_WARNING, "AudioConverterSetProperty failed (%d)\n", (int)status);
            return AVERROR_EXTERNAL;
        }
    }

    return 0;
}

/*----------------------------------------------------------------*/
static int
at_decoder_decode(AVCodecContext* avctx, void* data, int* got_frame, AVPacket* avpkt)
{
    AT_DecoderContext*          self = avctx->priv_data;
    AVFrame*                    frame = data;
    AudioBufferList             output_buffers;
    UInt32                      output_packet_count = self->output_buffer_conversion_size;
    AudioStreamBasicDescription output_format;
    UInt32                      output_format_size = sizeof(output_format);
    unsigned int                output_sample_count = 0;
    OSStatus                    status = noErr;
    int                         ff_result = 0;

    av_log(avctx, AV_LOG_TRACE, "at_decoder_decode - size=%d\n", avpkt->size);

    // default return value
    *got_frame = 0;
    
    // init the frame
    av_frame_unref(frame);

    // remember the current input packet for the data callback
    self->input_packet = avpkt;
    
    // decode some of the input data
    output_buffers.mNumberBuffers              = 1;
    output_buffers.mBuffers[0].mNumberChannels = 0;
    output_buffers.mBuffers[0].mDataByteSize   = AT_AUDIO_CONVERTER_DECODER_MAX_OUTPUT_BUFFER_SIZE;
    output_buffers.mBuffers[0].mData           = self->output_buffer;
    status = AudioConverterFillComplexBuffer(self->converter,
                                             at_decoder_data_proc,
                                             (void*)self,
                                             &output_packet_count,
                                             &output_buffers,
                                             NULL);
    av_log(avctx, AV_LOG_TRACE, "AudioConverterFillComplexBuffer() returned %d - output_packet_count=%u\n", (int)status, output_packet_count);

    if (status != noErr && status != AT_AUDIO_CONVERTER_DATA_UNDERFLOW_ERROR) {
        av_log(avctx, AV_LOG_WARNING,  "AudioConverterFillComplexBuffer() failed (%d)\n", (int)status);
        return AVERROR_EXTERNAL;
    }
    if (output_packet_count == 0) {
        return avpkt->size;
    }
    
    // get the output format
    status = AudioConverterGetProperty(self->converter,
                                       kAudioConverterCurrentOutputStreamDescription,
                                       &output_format_size,
                                       &output_format);
    if (status != noErr) {
        av_log(avctx, AV_LOG_WARNING,  "AudioConverterGetProperty() failed (%d)\n", (int)status);
        return AVERROR_EXTERNAL;
    }
    
    // compute the number of samples we decoded
    ++self->packet_count;
    if (output_format.mChannelsPerFrame) {
        output_sample_count = output_buffers.mBuffers[0].mDataByteSize/(2*output_format.mChannelsPerFrame);
    } else {
        return 0;
    }
    
    // setup and copy the output frame
    frame->format         = AV_SAMPLE_FMT_S16;
    frame->nb_samples     = output_sample_count;
    frame->sample_rate    = (unsigned int)output_format.mSampleRate;
    frame->channels       = output_format.mChannelsPerFrame;
    frame->channel_layout = AV_CH_LAYOUT_STEREO; // FIXME: hardcoded
    ff_result = avctx->get_buffer2(avctx, frame, 0);
    if (ff_result != 0) {
        av_log(avctx, AV_LOG_WARNING, "get_buffer2 failed (%d)\n", ff_result);
        return ff_result;
    }
    memcpy(frame->data[0],
           self->output_buffer,
           frame->channels * output_sample_count * 2);
    
    // copy some of the parameters to the codec context (shouldn't be needed, but some progs depend on it)
    avctx->sample_fmt     = AV_SAMPLE_FMT_S16;
    avctx->frame_size     = frame->nb_samples;
    avctx->channels       = frame->channels;
    avctx->channel_layout = frame->channel_layout;
    
#if defined(AT_DECODER_ADD_IDENTIFYING_SOUND)
    if ((self->packet_count % AT_DECODER_ADD_IDENTIFYING_SOUND_REPEAT) < (AT_DECODER_ADD_IDENTIFYING_SOUND_REPEAT/2)) {
        const unsigned int pattern[] = AT_DECODER_ADD_IDENTIFYING_SOUND;
        unsigned int cycle        = (unsigned int)(self->packet_count/AT_DECODER_ADD_IDENTIFYING_SOUND_REPEAT)%4;
        uint16_t*    samples      = ((uint16_t*)frame->data[0])+((self->packet_count/AT_DECODER_ADD_IDENTIFYING_SOUND_REPEAT)%2);
        unsigned int samples_step = frame->channels;
        unsigned int r = AT_DECODER_ADD_IDENTIFYING_SOUND_CYCLE*pattern[cycle];
        int          i;
        for (i=0; i<output_sample_count; i++) {
            unsigned int f=((self->packet_count%(AT_DECODER_ADD_IDENTIFYING_SOUND_REPEAT/2))*output_sample_count+i)%r;
            if (f >= r/2) f = r-f-1;
            *samples = (uint16_t)((f*AT_DECODER_ADD_IDENTIFYING_SOUND_AMP)/r);
            samples += samples_step;
        }
    }
#endif

    // done!
    *got_frame = 1;
    
    return avpkt->size;
}

/*----------------------------------------------------------------*/
static void
at_decoder_flush(AVCodecContext *avctx)
{
    //AT_DecoderContext* self = avctx->priv_data;

    av_log(avctx, AV_LOG_TRACE, "at_decoder_flush\n");

}

/*----------------------------------------------------------------
| AAC module definitions
+----------------------------------------------------------------*/
static const AVOption at_aac_decoder_options[] = {
    { NULL }
};

static const AVClass at_aac_decoder_class = {
    "audiotoolbox aac decoder", av_default_item_name, at_aac_decoder_options, AT_AAC_DECODER_VERSION
};

AVCodec ff_aac_audiotoolbox_decoder = {
    .name           = "aac_audiotoolbox",
    .long_name      = "AudioToolbox AAC decoder",
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = AV_CODEC_ID_AAC,
    .priv_data_size = sizeof(AT_DecoderContext),
    .init           = at_decoder_init,
    .decode         = at_decoder_decode,
    .close          = at_decoder_close,
    .flush          = at_decoder_flush,
    .capabilities   = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_CHANNEL_CONF,
    .priv_class     = &at_aac_decoder_class
};

/*----------------------------------------------------------------
| MP3 module definitions
+----------------------------------------------------------------*/
static const AVOption at_mp3_decoder_options[] = {
    { NULL }
};

static const AVClass at_mp3_decoder_class = {
    "audiotoolbox mp3 decoder", av_default_item_name, at_mp3_decoder_options, AT_MP3_DECODER_VERSION
};

AVCodec ff_mp3_audiotoolbox_decoder = {
    .name           = "mp3_audiotoolbox",
    .long_name      = "AudioToolbox MP3 decoder",
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = AV_CODEC_ID_MP3,
    .priv_data_size = sizeof(AT_DecoderContext),
    .init           = at_decoder_init,
    .decode         = at_decoder_decode,
    .close          = at_decoder_close,
    .flush          = at_decoder_flush,
    .capabilities   = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_CHANNEL_CONF,
    .priv_class     = &at_mp3_decoder_class
};
