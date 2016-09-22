/*----------------------------------------------------------------
| config
+----------------------------------------------------------------*/
// undefine this to get rid of the identifying sound in the audio
// or define it to an array of small integers to make a frequency pattern
// like for example: {1,2,1,3}
//#define MF_AUDIO_DECODER_ADD_IDENTIFYING_SOUND {2,3,2,4}

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
#include <Wmcodecdsp.h>

/*----------------------------------------------------------------
| constants
+----------------------------------------------------------------*/
#define MF_AUDIO_AAC_DECODER_VERSION 0x010000
#define MF_AUDIO_MP3_DECODER_VERSION 0x010000

const int MF_AUDIO_DECODER_SUCCESS        =  0;
const int MF_AUDIO_DECODER_FAILURE        = -1;
const int MF_AUDIO_DECODER_ERROR_INTERNAL = -2;
const int MF_AUDIO_DECODER_NEED_DATA      = -3;

#define MF_AUDIO_DECODER_ADD_IDENTIFYING_SOUND_CYCLE  60
#define MF_AUDIO_DECODER_ADD_IDENTIFYING_SOUND_AMP    5000
#define MF_AUDIO_DECODER_ADD_IDENTIFYING_SOUND_REPEAT 40


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
| types
+----------------------------------------------------------------*/
typedef struct {
    const AVClass* _class;
    
	IMFTransform* decoder;
	GUID          audio_format;
    uint64_t      packet_count;

	struct {
		unsigned int sample_rate;
		unsigned int channel_count;
	} input_type;
	bool input_type_set;

	struct {
		unsigned int sample_rate;
		unsigned int channel_count;
	} output_type;
	bool output_type_set;
} MF_AUDIO_DecoderContext;

/*----------------------------------------------------------------*/
static struct AttributeName {
	GUID        guid;
	const char* name;
} GuidNames[] = {
	{MF_MT_MAJOR_TYPE,                         "MF_MT_MAJOR_TYPE"},
	{MF_MT_SUBTYPE,                            "MF_MT_SUBTYPE"},
	{MF_MT_AUDIO_NUM_CHANNELS,                 "MF_MT_AUDIO_NUM_CHANNELS"},
	{MF_MT_AUDIO_CHANNEL_MASK,                 "MF_MT_AUDIO_CHANNEL_MASK"},
	{MF_MT_AUDIO_SAMPLES_PER_SECOND,           "MF_MT_AUDIO_SAMPLES_PER_SECOND"},
	{MF_MT_AUDIO_AVG_BYTES_PER_SECOND,         "MF_MT_AUDIO_AVG_BYTES_PER_SECOND"},
	{MF_MT_AUDIO_BLOCK_ALIGNMENT,              "MF_MT_AUDIO_BLOCK_ALIGNMENT"},
	{MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, "MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION"},
	{MF_MT_AUDIO_PREFER_WAVEFORMATEX,          "MF_MT_AUDIO_PREFER_WAVEFORMATEX"},
	{MF_MT_USER_DATA,                          "MF_MT_USER_DATA"},
	{MF_MT_COMPRESSED,                         "MF_MT_COMPRESSED"},
	{MF_MT_AAC_PAYLOAD_TYPE,                   "MF_MT_AAC_PAYLOAD_TYPE"},
	{MF_MT_AUDIO_BITS_PER_SAMPLE,              "MF_MT_AUDIO_BITS_PER_SAMPLE"},
	{MF_MT_ALL_SAMPLES_INDEPENDENT,            "MF_MT_ALL_SAMPLES_INDEPENDENT"},
	{MFMediaType_Audio,                        "MFMediaType_Audio"},
	{MFAudioFormat_MP3,                        "MFAudioFormat_MP3"},
	{MFAudioFormat_MPEG,                       "MFAudioFormat_MPEG"},
	{MFAudioFormat_AAC,                        "MFAudioFormat_AAC"},
	{MFAudioFormat_ADTS,                       "MFAudioFormat_ADTS"},
	{MFAudioFormat_PCM,                        "MFAudioFormat_PCM"},
	{MFAudioFormat_Float,                      "MFAudioFormat_Float"}
};
static void
mf_audio_decoder_dump_attributes(AVCodecContext* avctx, IMFAttributes* attributes)
{
	UINT32 attribute_count = 0;
	HRESULT mf_result = attributes->GetCount(&attribute_count);
	if (MF_FAILED(mf_result)) return;

	for (unsigned int i = 0; i < attribute_count; i++) {
		GUID        attribute_guid;
		BYTE*       attribute_guid_bytes = (BYTE*)&attribute_guid;
		PROPVARIANT attribute_value;
		mf_result = attributes->GetItemByIndex(i, &attribute_guid, &attribute_value);
		const char* attribute_name = NULL;
		for (unsigned int x = 0; x < sizeof(GuidNames)/sizeof(GuidNames[0]); x++) {
			if (attribute_guid == GuidNames[x].guid) {
				attribute_name = GuidNames[x].name;
				break;
			}
		}
		char guid_string[33];
		if (attribute_name == NULL) {
			for (unsigned int j = 0; j < 16; j++) {
				unsigned int nibble = attribute_guid_bytes[j] >> 4;
				guid_string[2 * j] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
				nibble = attribute_guid_bytes[j] & 0xF;
				guid_string[2 * j + 1] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
			}
			guid_string[32] = 0;
			attribute_name = guid_string;
		}
		av_log(avctx, AV_LOG_DEBUG, "--- %s (type=%d) = ", attribute_name, attribute_value.vt);

		switch (attribute_value.vt) {
		  case VT_UI4:
			av_log(avctx, AV_LOG_DEBUG, "%u", attribute_value.uintVal);
			break;

		  case VT_CLSID: {
			const char* guid_name = NULL;
 			for (unsigned int x = 0; x < sizeof(GuidNames)/sizeof(GuidNames[0]); x++) {
				if (*attribute_value.puuid == GuidNames[x].guid) {
					guid_name = GuidNames[x].name;
					break;
				}
			}
			if (guid_name == NULL) {
				for (unsigned int j = 0; j < 16; j++) {
					BYTE* clsid_bytes = (BYTE*)attribute_value.puuid;
					unsigned int nibble = clsid_bytes[j] >> 4;
					guid_string[2 * j] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
					nibble = clsid_bytes[j] & 0xF;
					guid_string[2 * j + 1] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
				}
				guid_string[32] = 0;
				guid_name = guid_string;
			}
			av_log(avctx, AV_LOG_DEBUG, "%s", guid_name);
		    break;
		  }
		}
		av_log(avctx, AV_LOG_DEBUG, "\n");
		PropVariantClear(&attribute_value);
	}
}

/*----------------------------------------------------------------*/
static int
mf_audio_decoder_close(AVCodecContext* avctx)
{
    MF_AUDIO_DecoderContext* self = (MF_AUDIO_DecoderContext*)avctx->priv_data;

    av_log(avctx, AV_LOG_TRACE, "mf_audio_decoder_close\n");

	if (self->decoder) {
		self->decoder->Release();
		self->decoder = NULL;
	}

    return 0;
}

/*----------------------------------------------------------------*/
static int
mf_audio_decoder_set_input_type(AVCodecContext* avctx)
{
    MF_AUDIO_DecoderContext* self = (MF_AUDIO_DecoderContext*)avctx->priv_data;

	// do nothing if we don't have a sample rate and channel count
	// or if we have already set the type
	if (self->input_type_set || avctx->sample_rate == 0 || avctx->channels == 0) {
		return MF_AUDIO_DECODER_SUCCESS;
	}

	// create a media type
	IMFMediaType* media_type = NULL;
	HRESULT mf_result = MFCreateMediaType(&media_type);
	if (!media_type) {
		return MF_AUDIO_DECODER_FAILURE;
	}

	// set the type attributes
	media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	media_type->SetGUID(MF_MT_SUBTYPE, self->audio_format);
	media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)avctx->channels);
	media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)avctx->sample_rate);
	if (avctx->codec_id == AV_CODEC_ID_AAC) {
		// the AAC decoder needs to know how many bits per sample we want at the output 
		media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
		
		// AAC Payload Type
		media_type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);

		// AAC decoder config
		BYTE  wave_info[12] = {
			0x00, 0x00,  // wPayloadType (0=The stream contains raw_data_block elements only)
			0x00, 0x00,  // wAudioProfileLevelIndication (0=unknown)
			0x00, 0x00,  // wStructType (0=The data that follows the HEAACWAVEINFO structure contains the value of AudioSpecificConfig(), as defined by ISO/IEC 14496-3)
			0x00, 0x00,  // wReserved1
			0x00, 0x00, 0x00, 0x00, // dwReserved2
		};
		unsigned int user_data_size = sizeof(wave_info) + avctx->extradata_size;
		BYTE* user_data = new BYTE[user_data_size];
		memcpy(user_data, wave_info, sizeof(wave_info));
		if (avctx->extradata_size && avctx->extradata) {
			memcpy(&user_data[sizeof(wave_info)], avctx->extradata, avctx->extradata_size);
		}
		media_type->SetBlob(MF_MT_USER_DATA, user_data, user_data_size);
	}

	// set the input type
	mf_result = self->decoder->SetInputType(0, media_type, 0);
	media_type->Release();
	if (MF_FAILED(mf_result)) {
		av_log(avctx, AV_LOG_ERROR, "SetInputType failed (%d)\n", mf_result);
		return MF_AUDIO_DECODER_FAILURE;
	}

	self->input_type.sample_rate   = avctx->sample_rate;
	self->input_type.channel_count = avctx->channels;
	self->input_type_set           = true;
	return MF_AUDIO_DECODER_SUCCESS;
}

/*----------------------------------------------------------------*/
static int
mf_audio_decoder_set_output_type(AVCodecContext* avctx)
{
    MF_AUDIO_DecoderContext* self = (MF_AUDIO_DecoderContext*)avctx->priv_data;

	// do nothing if we don't have a sample rate and channel count
	if (avctx->sample_rate == 0 || avctx->channels == 0) {
		return MF_AUDIO_DECODER_SUCCESS;
	}

	// create a media type
	IMFMediaType* media_type = NULL;
	HRESULT mf_result = MFCreateMediaType(&media_type);
	if (!media_type) {
		return MF_AUDIO_DECODER_FAILURE;
	}

	// FIXME: test
	IMFMediaType* output_type = NULL;
	unsigned int type_index = 0;
	do {
		mf_result = self->decoder->GetOutputAvailableType(0, type_index, &output_type);
		if (MF_SUCCEEDED(mf_result)) {
			av_log(avctx, AV_LOG_DEBUG, "*** Available Output Type %d\n", type_index);
			mf_audio_decoder_dump_attributes(avctx, output_type);
			output_type->Release();
		}
		++type_index;
	} while (MF_SUCCEEDED(mf_result));

	// set the type attributes
	media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	media_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
	media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)avctx->channels);
	media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)avctx->sample_rate);
	media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
	if (avctx->codec_id == AV_CODEC_ID_MP3) {
		media_type->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK, avctx->channels == 1 ? 4 : 3); // 0x4 for mono or 0x3 for stereo as documented (?)
		media_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2*avctx->channels);
		media_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 2*avctx->channels*avctx->sample_rate);
		media_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, 1); 
	}

	// set the output type
	mf_result = self->decoder->SetOutputType(0, media_type, 0);
	media_type->Release();
	if (MF_FAILED(mf_result)) {
		av_log(avctx, AV_LOG_ERROR, "SetOutputType failed (%d)\n", mf_result);
		return MF_AUDIO_DECODER_FAILURE;
	}

	self->output_type.sample_rate   = avctx->sample_rate;
	self->output_type.channel_count = avctx->channels;
	self->output_type_set           = true;
	return MF_AUDIO_DECODER_SUCCESS;
}

/*----------------------------------------------------------------*/
static int
mf_audio_decoder_init(AVCodecContext* avctx)
{
    MF_AUDIO_DecoderContext* self = (MF_AUDIO_DecoderContext*)avctx->priv_data;

    av_log(avctx, AV_LOG_TRACE, "mf_audio_decoder_init\n");

	// init
	self->decoder                   = NULL;
	self->packet_count              = 0;
	self->input_type_set            = false;
	self->input_type.channel_count  = 0;
	self->input_type.sample_rate    = 0;
	self->output_type.channel_count = 0;
	self->output_type.sample_rate   = 0;
	self->output_type_set           = false;

    // initialize COM, we'll need it
    CoInitialize(NULL);

    // init Media Foundation
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

	// find a suitable decoder
	HRESULT mf_result;
	if (avctx->codec_id == AV_CODEC_ID_AAC) {
		// use the standard MFT factory mechanism
		self->audio_format = MFAudioFormat_AAC;
		MFT_REGISTER_TYPE_INFO input_type_info = {
			MFMediaType_Audio,
			MFAudioFormat_AAC
		};
		MFT_REGISTER_TYPE_INFO output_type_info = {
			MFMediaType_Audio,
			MFAudioFormat_PCM
		};

		UINT32 flags = MFT_ENUM_FLAG_SYNCMFT  |
					   MFT_ENUM_FLAG_ASYNCMFT |
					   MFT_ENUM_FLAG_LOCALMFT |
					   MFT_ENUM_FLAG_HARDWARE |
					   MFT_ENUM_FLAG_SORTANDFILTER;

		IMFActivate** factories = NULL;
		UINT32        factory_count = 0;

		mf_result = MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER,
						      flags,
							  &input_type_info,
							  &output_type_info,
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
	} else if (avctx->codec_id == AV_CODEC_ID_MP3) {
		// we need to instantiate the MFTransform object directly...
		self->audio_format = MFAudioFormat_MP3;
	    mf_result = CoCreateInstance(CLSID_CMP3DecMediaObject, NULL,  CLSCTX_INPROC_SERVER, IID_IMFTransform, (void**)&self->decoder);
		if (mf_result != S_OK) {
			av_log(avctx, AV_LOG_WARNING, "failed to created MP3 decoder (%d)\n", mf_result);
		}
	} else {
		av_log(avctx, AV_LOG_ERROR, "unexpected codec ID %d\n", avctx->codec_id);
		return -1;
	}

	// stop now if we couldn't create the decoder
	if (self->decoder == NULL) {
		return -1;
	}

    // set the input type if we can
	mf_audio_decoder_set_input_type(avctx);

    // set the output type
    mf_audio_decoder_set_output_type(avctx);

    // make sure the decoder is ready
    self->decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL);

    return 0;
}

/*----------------------------------------------------------------*/
static int
mf_audio_decoder_convert_output(AVCodecContext* avctx, AVFrame* frame)
{
    MF_AUDIO_DecoderContext* self  = (MF_AUDIO_DecoderContext*)avctx->priv_data;

	// check the output stream info
	MFT_OUTPUT_STREAM_INFO stream_info;
	HRESULT mf_result = self->decoder->GetOutputStreamInfo(0, &stream_info);
	if (!MF_SUCCEEDED(mf_result)) {
		av_log(avctx, AV_LOG_WARNING, "GetOutputStreamInfo failed (%d)\n", mf_result);
		return -1;
	}

	// allocate a buffer for the output sample
	IMFSample* sample = NULL;
	mf_result = MFCreateSample(&sample);

	IMFMediaBuffer* sample_buffer = NULL;
	if (MF_SUCCEEDED(mf_result)) {
		mf_result = MFCreateMemoryBuffer(stream_info.cbSize, &sample_buffer);
	}

	if (MF_SUCCEEDED(mf_result)) {
		mf_result = sample->AddBuffer(sample_buffer);
	}

	if (!MF_SUCCEEDED(mf_result)) {
		return MF_AUDIO_DECODER_FAILURE;
	}

	// get the output sample data until we have retrieved everything that is available
	unsigned int change_count = 0;
	int result = MF_AUDIO_DECODER_SUCCESS;
	unsigned int internal_buffer_length = 0;
	BYTE*        internal_buffer = NULL;
	do {
		MFT_OUTPUT_DATA_BUFFER output_buffer;
		DWORD status = 0;
		memset(&output_buffer, 0, sizeof(output_buffer));
		output_buffer.pSample = sample;
		mf_result = self->decoder->ProcessOutput(0, 1, &output_buffer, &status);
		if (mf_result == MF_E_TRANSFORM_STREAM_CHANGE) {
			if (change_count++ != 0) {
				// we have already done this, stop now to avoid an infinite loop
				av_log(avctx, AV_LOG_ERROR, "too many format changes, giving up\n");
				break;
			}
			mf_audio_decoder_set_output_type(avctx);
			mf_result = self->decoder->ProcessOutput(0, 1, &output_buffer, &status);
		}
		if (mf_result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
			result = MF_AUDIO_DECODER_NEED_DATA;
			break;
		} else if (MF_FAILED(mf_result)) {
			av_log(avctx, AV_LOG_WARNING, "ProcessOutput returned %d\n", mf_result);
			result = MF_AUDIO_DECODER_FAILURE;
			break;
		}

		// add the sample data to the internal buffer
		DWORD sample_buffer_length = 0;
		mf_result = sample_buffer->GetCurrentLength(&sample_buffer_length);
		if (MF_FAILED(mf_result)) {
			result = MF_AUDIO_DECODER_FAILURE;
			break;
		}

		BYTE* sample_buffer_address = NULL;
		mf_result = sample_buffer->Lock(&sample_buffer_address, NULL, NULL);
		if (MF_SUCCEEDED(mf_result)) {
			internal_buffer = (BYTE*)realloc(internal_buffer, internal_buffer_length+sample_buffer_length);
			memcpy(internal_buffer + internal_buffer_length, sample_buffer_address, sample_buffer_length);
			sample_buffer->Unlock();
		}
		internal_buffer_length += sample_buffer_length;

		// reset the sample buffer
		sample_buffer->SetCurrentLength(0);
	} while (MF_SUCCEEDED(mf_result));

	if (internal_buffer_length) {
		// if we have decoded something, return it, even if an error occurred
		result = MF_AUDIO_DECODER_SUCCESS;

		// allocate space in the return frame
		unsigned int sample_count = internal_buffer_length/(avctx->channels*2);
		frame->format = AV_SAMPLE_FMT_S16;
		frame->nb_samples = sample_count;
		frame->sample_rate = self->output_type.sample_rate;
		frame->channels = self->output_type.channel_count;
		frame->channel_layout = AV_CH_LAYOUT_STEREO; // TODO: deal with multi-channel
		int ff_result = avctx->get_buffer2(avctx, frame, 0);
		if (ff_result != 0) {
			av_log(avctx, AV_LOG_WARNING, "get_buffer2 failed (%d)\n", ff_result);
			result = MF_AUDIO_DECODER_FAILURE;
			goto end;
		}

		// copy the data from the internal buffer
		memcpy(frame->data[0], internal_buffer, internal_buffer_length);

#if defined(MF_AUDIO_DECODER_ADD_IDENTIFYING_SOUND)
		if ((self->packet_count % MF_AUDIO_DECODER_ADD_IDENTIFYING_SOUND_REPEAT) < (MF_AUDIO_DECODER_ADD_IDENTIFYING_SOUND_REPEAT / 2)) {
			const unsigned int pattern[] = MF_AUDIO_DECODER_ADD_IDENTIFYING_SOUND;
			unsigned int cycle = (unsigned int)(self->packet_count / MF_AUDIO_DECODER_ADD_IDENTIFYING_SOUND_REPEAT) % 4;
			uint16_t*    samples = ((uint16_t*)frame->data[0]) + ((self->packet_count / MF_AUDIO_DECODER_ADD_IDENTIFYING_SOUND_REPEAT) % 2);
			unsigned int samples_step = frame->channels;
			unsigned int r = MF_AUDIO_DECODER_ADD_IDENTIFYING_SOUND_CYCLE*pattern[cycle];
			for (unsigned i = 0; i < sample_count; i++) {
				unsigned int f = ((self->packet_count % (MF_AUDIO_DECODER_ADD_IDENTIFYING_SOUND_REPEAT / 2))*sample_count + i) % r;
				if (f >= r / 2) f = r - f - 1;
				*samples = (uint16_t)((f*MF_AUDIO_DECODER_ADD_IDENTIFYING_SOUND_AMP) / r);
				samples += samples_step;
			}
		}
#endif

		// copy the sample format in the context (shouldn't be needed, but some progs depend on it)
		avctx->sample_fmt     = (AVSampleFormat)frame->format;
		avctx->frame_size     = frame->nb_samples;
		avctx->channels       = frame->channels;
		avctx->channel_layout = frame->channel_layout;

		// adjust counters
		++self->packet_count;
	}

end:
	if (sample_buffer)   sample_buffer->Release();
	if (sample)          sample->Release();
	if (internal_buffer) free(internal_buffer);

	return result;
}

/*----------------------------------------------------------------*/
static int
mf_audio_decoder_decode(AVCodecContext* avctx, void* data, int* got_frame, AVPacket* avpkt)
{
	MF_AUDIO_DecoderContext* self = (MF_AUDIO_DecoderContext*)avctx->priv_data;
	AVFrame*                 frame = (AVFrame*)data;

	av_log(avctx, AV_LOG_TRACE, "mf_audio_decoder_decode - size=%d\n", avpkt->size);

	// default return value
	*got_frame = 0;

	// init the frame
	av_frame_unref(frame);

	// check that we have a decoder
	if (!self->decoder) {
		return -1;
	}

	// update the input type if needed
	if (!self->input_type_set) {
		int result = mf_audio_decoder_set_input_type(avctx);
		if (result != MF_AUDIO_DECODER_SUCCESS) {
			av_log(avctx, AV_LOG_ERROR, "mf_audio_decoder_set_input_type failed (%d)\n", result);
			return -1;
		}

		// FIXME: test
		IMFMediaType* output_type = NULL;
		unsigned int type_index = 0;
		HRESULT mf_result;
		do {
		    mf_result = self->decoder->GetOutputAvailableType(0, type_index, &output_type);
			if (MF_SUCCEEDED(mf_result)) {
				av_log(avctx, AV_LOG_DEBUG, "*** Output Type %d\n", type_index);
				mf_audio_decoder_dump_attributes(avctx, output_type);
				output_type->Release();
			}
			++type_index;
		} while (MF_SUCCEEDED(mf_result));

		// also update the output type
		mf_audio_decoder_set_output_type(avctx);
	}

	// process the input data
	unsigned int bytes_consumed = 0;
	int result = MF_AUDIO_DECODER_SUCCESS;
	for (; result == MF_AUDIO_DECODER_SUCCESS && *got_frame == 0;) {
		// check if we have data ready
		DWORD output_status = 0;
		HRESULT mf_result = self->decoder->GetOutputStatus(&output_status);
		if (MF_FAILED(mf_result)) {
			if (mf_result == E_NOTIMPL) {
				// not all codecs implement GetOutputStatus, so we'll just try to get a sample
				output_status |= MFT_OUTPUT_STATUS_SAMPLE_READY;
			} else {
				return -1;
			}
		}
		if (output_status & MFT_OUTPUT_STATUS_SAMPLE_READY) {
			result = mf_audio_decoder_convert_output(avctx, frame);
			if (result == MF_AUDIO_DECODER_SUCCESS) {
				*got_frame = 1;
			} else if (result != MF_AUDIO_DECODER_NEED_DATA) {
				break;
			}
		}

		// feed more data if we haven't done so already
		if (bytes_consumed) break;
		bytes_consumed = avpkt->size;

		// create a sample
		IMFSample* sample = NULL;
		mf_result = MFCreateSample(&sample);

		IMFMediaBuffer* buffer = NULL;
		mf_result = MFCreateMemoryBuffer(avpkt->size, &buffer);

		BYTE* buffer_address = NULL;
		if (MF_SUCCEEDED(mf_result)) {
			mf_result = buffer->Lock(&buffer_address, NULL, NULL);
		}

		if (MF_SUCCEEDED(mf_result)) {
			memcpy(buffer_address, avpkt->data, avpkt->size);
			mf_result = buffer->Unlock();
		}

		if (MF_SUCCEEDED(mf_result)) {
			mf_result = buffer->SetCurrentLength(avpkt->size);
		}

		if (MF_SUCCEEDED(mf_result)) {
			mf_result = sample->AddBuffer(buffer);
		}

		mf_result = self->decoder->ProcessInput(0, sample, 0);
		if (MF_FAILED(mf_result)) {
			av_log(avctx, AV_LOG_WARNING, "ProcessInput failed (%d)\n", mf_result);
			result = MF_AUDIO_DECODER_FAILURE;
		}

		if (buffer) buffer->Release();
		if (sample) sample->Release();
	}

	if (result == 0) {
		return bytes_consumed;
	} else {
		return result;
	}
}

/*----------------------------------------------------------------*/
static void
mf_audio_decoder_flush(AVCodecContext *avctx)
{
    //MF_AUDIO_DecoderContext* self = (MF_AUDIO_DecoderContext*)avctx->priv_data;

    av_log(avctx, AV_LOG_TRACE, "mf_audio_decoder_flush\n");

}

/*----------------------------------------------------------------
| AAC module definitions
+----------------------------------------------------------------*/
static const AVOption mf_audio_aac_decoder_options[] = {
    { NULL }
};

static const AVClass mf_audio_aac_decoder_class = {
    "media foundations aac decoder", av_default_item_name, mf_audio_aac_decoder_options, MF_AUDIO_AAC_DECODER_VERSION
};

extern "C" {
AVCodec ff_aac_mf_audio_decoder = {
    /* .name                  = */ "aac_mf",
    /* .long_name             = */ "Media Foundations AAC decoder",
    /* .type                  = */ AVMEDIA_TYPE_AUDIO,
    /* .id                    = */ AV_CODEC_ID_AAC,
    /* .capabilities          = */ AV_CODEC_CAP_DR1 | AV_CODEC_CAP_CHANNEL_CONF,
    /* .supported_framerates  = */ NULL,
    /* .pix_fmts              = */ NULL,
    /* .supported_samplerates = */ NULL,
    /* .sample_fmts           = */ NULL,
    /* .channel_layouts       = */ NULL,
    /* .max_lowres            = */ 0,
    /* .priv_class            = */ &mf_audio_aac_decoder_class,
    /* .profiles              = */ NULL,
    /* .priv_data_size        = */ sizeof(MF_AUDIO_DecoderContext),
    /* .next                  = */ NULL,
    /* .init_thread_copy      = */ NULL,
    /* .update_thread_context = */ NULL,
    /* .defaults              = */ NULL,
    /* .init_static_data      = */ NULL,
    /* .init                  = */ mf_audio_decoder_init,
    /* .encode_sub            = */ NULL,
    /* .encode2               = */ NULL,
    /* .decode                = */ mf_audio_decoder_decode,
    /* .close                 = */ mf_audio_decoder_close,
#if 0 // those are only in ffmpeg 3.1
	/* .send_frame            = */ NULL,
    /* .send_packet           = */ NULL,
    /* .receive_frame         = */ NULL,
    /* .receive_packet        = */ NULL,
#endif
	/* .flush                 = */ mf_audio_decoder_flush,
    /* .caps_internal         = */ 0
};
}

/*----------------------------------------------------------------
| MP3 module definitions
+----------------------------------------------------------------*/
static const AVOption mf_audio_mp3_decoder_options[] = {
    { NULL }
};

static const AVClass mf_audio_mp3_decoder_class = {
    "media foundations mp3 decoder", av_default_item_name, mf_audio_mp3_decoder_options, MF_AUDIO_MP3_DECODER_VERSION
};

extern "C" {
AVCodec ff_mp3_mf_audio_decoder = {
    /* .name                  = */ "mp3_mf",
    /* .long_name             = */ "AudioToolbox MP3 decoder",
    /* .type                  = */ AVMEDIA_TYPE_AUDIO,
    /* .id                    = */ AV_CODEC_ID_MP3,
    /* .capabilities          = */ AV_CODEC_CAP_DR1 | AV_CODEC_CAP_CHANNEL_CONF,
    /* .supported_framerates  = */ NULL,
    /* .pix_fmts              = */ NULL,
    /* .supported_samplerates = */ NULL,
    /* .sample_fmts           = */ NULL,
    /* .channel_layouts       = */ NULL,
    /* .max_lowres            = */ 0,
    /* .priv_class            = */ &mf_audio_mp3_decoder_class,
    /* .profiles              = */ NULL,
    /* .priv_data_size        = */ sizeof(MF_AUDIO_DecoderContext),
    /* .next                  = */ NULL,
    /* .init_thread_copy      = */ NULL,
    /* .update_thread_context = */ NULL,
    /* .defaults              = */ NULL,
    /* .init_static_data      = */ NULL,
    /* .init                  = */ mf_audio_decoder_init,
    /* .encode_sub            = */ NULL,
    /* .encode2               = */ NULL,
    /* .decode                = */ mf_audio_decoder_decode,
    /* .close                 = */ mf_audio_decoder_close,
#if 0 // those are only in ffmpeg 3.1
    /* .send_frame            = */ NULL,
    /* .send_packet           = */ NULL,
    /* .receive_frame         = */ NULL,
    /* .receive_packet        = */ NULL,
#endif
	/* .flush                 = */ mf_audio_decoder_flush,
    /* .caps_internal         = */ 0
};
}