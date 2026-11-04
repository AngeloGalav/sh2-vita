// ndsp, the 3DS DSP mixer, as the game uses it: 24 channels of PCM16 wave buffers with a rate and
// a front left/right mix each. compat/ndsp.c mixes them in software on an SceAudioOut port.
#pragma once

typedef enum { NDSP_OUTPUT_MONO = 0, NDSP_OUTPUT_STEREO = 1, NDSP_OUTPUT_SURROUND = 2 } ndspOutputMode;
typedef enum { NDSP_INTERP_POLYPHASE = 0, NDSP_INTERP_LINEAR = 1, NDSP_INTERP_NONE = 2 } ndspInterpType;
enum {
	NDSP_WBUF_FREE = 0, NDSP_WBUF_QUEUED = 1, NDSP_WBUF_PLAYING = 2, NDSP_WBUF_DONE = 3,
};
#define NDSP_CHANNELS(n) ((u32)(n) & 3)
#define NDSP_ENCODING(n) (((u32)(n) & 3) << 2)
enum { NDSP_ENCODING_PCM8 = 0, NDSP_ENCODING_PCM16 = 1, NDSP_ENCODING_ADPCM = 2 };
#define NDSP_FORMAT_MONO_PCM8 (NDSP_CHANNELS(1) | NDSP_ENCODING(NDSP_ENCODING_PCM8))
#define NDSP_FORMAT_MONO_PCM16 (NDSP_CHANNELS(1) | NDSP_ENCODING(NDSP_ENCODING_PCM16))
#define NDSP_FORMAT_STEREO_PCM8 (NDSP_CHANNELS(2) | NDSP_ENCODING(NDSP_ENCODING_PCM8))
#define NDSP_FORMAT_STEREO_PCM16 (NDSP_CHANNELS(2) | NDSP_ENCODING(NDSP_ENCODING_PCM16))

typedef struct tag_ndspWaveBuf ndspWaveBuf;
struct tag_ndspWaveBuf {
	union {
		s8* data_pcm8;
		s16* data_pcm16;
		void* data_vaddr;
	};
	u32 nsamples;
	void* adpcm_data;
	u32 offset;
	bool looping;
	volatile u8 status;
	u16 sequence_id;
	ndspWaveBuf* next;
};

Result ndspInit(void);
void ndspExit(void);
void ndspSetOutputMode(ndspOutputMode mode);
void ndspChnReset(int id);
void ndspChnSetInterp(int id, ndspInterpType type);
void ndspChnSetRate(int id, float rate);
void ndspChnSetFormat(int id, u16 format);
void ndspChnSetMix(int id, float mix[12]);
void ndspChnSetPaused(int id, bool paused);
bool ndspChnIsPaused(int id);
bool ndspChnIsPlaying(int id);
u32 ndspChnGetSamplePos(int id);
u16 ndspChnGetWaveBufSeq(int id);
void ndspChnWaveBufAdd(int id, ndspWaveBuf* buf);
void ndspChnWaveBufClear(int id);
