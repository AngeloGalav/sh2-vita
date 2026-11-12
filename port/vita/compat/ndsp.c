// ndsp for the Vita build: the DSP's 24 channels mixed in software into one 48 kHz stereo
// SceAudioOut port. Each channel plays its queue of PCM8 / PCM16 wave buffers at its own rate with
// linear interpolation and a front left / right gain, and marks them PLAYING and DONE as the DSP
// does, so audio.c, sfx.c and movie.c refill them unchanged. Vita-only.
#include <3ds.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "vita.h"

#define NCH 24
#define OUT_RATE 48000
#define GRAIN 1024

typedef struct {
	ndspWaveBuf* head;          // queue, oldest first
	double pos;                 // in samples, inside head
	float step;                 // rate / OUT_RATE
	float gain[2];              // now; moves to target across a grain (mix)
	float target[2];            // as last set (ndspChnSetMix)
	u16 format;
	bool paused;
	u16 seq_next, seq_playing;
	float last[2];              // the channel's last output sample, gain applied
	float tail[2];              // what it fades down from once it stops (start_tail)
	int tail_n;                 // samples of the fade left
} Chn;

// A channel that stops (its last buffer played out, cleared or reset) fades from its last sample to
// silence over DECLICK samples (2.7 ms) instead of dropping to 0 at once: the cut was a click at
// the end of many effects (reported from play). The port's own, not the 3DS DSP's.
#define DECLICK 128

static Chn s_ch[NCH];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static ndspOutputMode s_mode = NDSP_OUTPUT_STEREO;
static volatile bool s_run;
static pthread_t s_thread;
static int s_port = -1;

// ---------------------------------------------------------------- audio capture (diagnostics)
// With ux0:data/sh2-vita/audiodump.txt present, the first 20 s after ndspInit are kept: the mixed
// output (mix.raw, s16 stereo 48 kHz) and the music channel's buffers as the game queued them
// (music.raw, s16 stereo at the stream's own rate, named in the log), then written out once.
#define CAP_SECONDS 20
static s16 *s_cap_mix, *s_cap_mus;
static size_t s_cap_mix_n, s_cap_mus_n, s_cap_mus_max;
static bool s_cap_on, s_cap_done;

static void cap_init(void)
{
	FILE* f = fopen(VITA_DATA_DIR "/audiodump.txt", "r");
	if (!f)
		return;
	fclose(f);
	s_cap_mus_max = (size_t)CAP_SECONDS * 48000 * 2;
	s_cap_mix = malloc((size_t)CAP_SECONDS * OUT_RATE * 2 * sizeof(s16));
	s_cap_mus = malloc(s_cap_mus_max * sizeof(s16));
	s_cap_on = s_cap_mix && s_cap_mus;
	vita_log("audio capture %s", s_cap_on ? "on" : "failed (memory)");
}

static void cap_write(void)
{
	FILE* f = fopen(VITA_DATA_DIR "/mix.raw", "wb");
	if (f) {
		fwrite(s_cap_mix, sizeof(s16), s_cap_mix_n, f);
		fclose(f);
	}
	f = fopen(VITA_DATA_DIR "/music.raw", "wb");
	if (f) {
		fwrite(s_cap_mus, sizeof(s16), s_cap_mus_n, f);
		fclose(f);
	}
	vita_log("audio capture written: mix %u frames, music %u frames", (unsigned)(s_cap_mix_n / 2),
	         (unsigned)(s_cap_mus_n / 2));
	free(s_cap_mix);
	free(s_cap_mus);
	s_cap_mix = s_cap_mus = NULL;
	s_cap_on = false;
	s_cap_done = true;
}

static void start_tail(Chn* c)
{
	c->tail[0] = c->last[0];
	c->tail[1] = c->last[1];
	c->tail_n = (c->last[0] != 0.0f || c->last[1] != 0.0f) ? DECLICK : 0;
	c->last[0] = c->last[1] = 0.0f;
}

static void tail_mix(Chn* c, float* out, int from, int frames)
{
	for (int f = from; f < frames && c->tail_n > 0; f++, c->tail_n--) {
		float k = (float)c->tail_n / DECLICK;
		out[2 * f] += c->tail[0] * k;
		out[2 * f + 1] += c->tail[1] * k;
	}
}

// for the log, per second: channels that ran dry while playing, samples past full scale
static unsigned s_starved[NCH], s_clipped, s_late;

// The position past the end carries into the next buffer, so a stream split into buffers plays as
// one (dropping the fraction moved every boundary by up to a sample: a tick in steady music)
static void finish_head(Chn* c)
{
	ndspWaveBuf* b = c->head;
	double over = c->pos - b->nsamples;
	if (over < 0)
		over = 0;
	if (b->looping) {
		c->pos = over;
		return;
	}
	b->status = NDSP_WBUF_DONE;
	c->head = b->next;
	c->pos = c->head ? over : 0;
	if (c->head) {
		c->head->status = NDSP_WBUF_PLAYING;
		c->seq_playing = c->head->sequence_id;
	}
}

static inline void sample_at(const Chn* c, const ndspWaveBuf* b, u32 i, float* l, float* r)
{
	bool stereo = (c->format & 3) == 2;
	bool pcm8 = ((c->format >> 2) & 3) == NDSP_ENCODING_PCM8;
	u32 n = stereo ? i * 2 : i;
	float a, z;
	if (pcm8) {
		a = b->data_pcm8[n] * 256.0f;
		z = stereo ? b->data_pcm8[n + 1] * 256.0f : a;
	} else {
		a = b->data_pcm16[n];
		z = stereo ? b->data_pcm16[n + 1] : a;
	}
	*l = a;
	*r = z;
}

static void mix(float* out, int frames)
{
	memset(out, 0, sizeof(float) * 2 * frames);
	for (int k = 0; k < NCH; k++) {
		Chn* c = &s_ch[k];
		if (c->paused)
			continue;
		// a new gain is reached across the grain, not at its first sample: effects move their
		// gains and pan while they play (sfx.c), and a step at the grain's start is a click
		float g0 = c->gain[0], g1 = c->gain[1];
		float d0 = (c->target[0] - g0) / frames, d1 = (c->target[1] - g1) / frames;
		c->gain[0] = c->target[0], c->gain[1] = c->target[1];
		int stop_at = 0;                // where this grain's fade starts (a stop in this grain)
		bool had = c->head != NULL;
		int f = 0;
		for (; f < frames && c->head; f++, g0 += d0, g1 += d1) {
			ndspWaveBuf* b = c->head;
			u32 n = b->nsamples;
			u32 i = (u32)c->pos;
			if (i >= n) {
				finish_head(c);
				f--, g0 -= d0, g1 -= d1;    // the same output sample again, from the next buffer
				continue;
			}
			float l0, r0, l1, r1, t = (float)(c->pos - i);
			sample_at(c, b, i, &l0, &r0);
			if (i + 1 < n)
				sample_at(c, b, i + 1, &l1, &r1);
			else if (b->next && b->next->nsamples)
				sample_at(c, b->next, 0, &l1, &r1);    // across into the next buffer
			else
				l1 = l0, r1 = r0;
			float vl = (l0 + (l1 - l0) * t) * g0, vr = (r0 + (r1 - r0) * t) * g1;
			out[2 * f] += vl;
			out[2 * f + 1] += vr;
			c->last[0] = vl, c->last[1] = vr;
			c->pos += c->step;
			if (!c->head->next && c->pos >= c->head->nsamples && !c->head->looping)
				s_starved[k]++;
		}
		if (had && !c->head) {
			start_tail(c);              // played out in this grain, at sample f
			stop_at = f;
		}
		tail_mix(c, out, stop_at, frames);
	}
}

// ndsp's default clipping mode is NDSP_CLIP_SOFT: past about three quarters of full scale the
// sum bends towards the limit instead of being cut, which is what keeps loud music, voices and
// effects from crackling when they add up past full scale
static inline s16 soft_clip(float v)
{
	const float knee = 24576.0f, room = 32767.0f - 24576.0f;
	float a = v < 0 ? -v : v;
	if (a > knee) {
		if (a > 32767.0f)
			s_clipped++;
		float x = (a - knee) / room;
		a = knee + room * (x / (1.0f + x));      // approaches 32767, never past it
	}
	return (s16)(v < 0 ? -a : a);
}

static void* audio_thread(void* arg)
{
	(void)arg;
	// above every game thread, on the cores the main loop doesn't use: a late grain is a click
	sceKernelChangeThreadPriority(0, 0x40);
	sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2);
	static float acc[GRAIN * 2];
	static s16 pcm[2][GRAIN * 2];
	int cur = 0;
	while (s_run) {
		pthread_mutex_lock(&s_lock);
		mix(acc, GRAIN);
		pthread_mutex_unlock(&s_lock);
		// The limiter (the port's own): the grain's peak is known before it goes out, so when the sum
		// would pass LIMIT the whole mix is turned down smoothly across the grain, and back up over
		// about half a second. Shaping each sample alone past the knee (soft_clip, kept as the last
		// guard) bent loud bass waves into a crackle (reported from play; the log counted up to 86
		// samples a second past full scale).
		static float s_lim = 1.0f;
		const float LIMIT = 24000.0f, RELEASE = 1.0f / (0.5f * OUT_RATE / GRAIN);
		float peak = 0.0f;
		for (int i = 0; i < GRAIN * 2; i++) {
			float a = acc[i] < 0 ? -acc[i] : acc[i];
			if (a > peak)
				peak = a;
		}
		float want = peak > LIMIT ? LIMIT / peak : 1.0f;
		float g_to = want < s_lim ? want : (s_lim + RELEASE < want ? s_lim + RELEASE : want);
		// down over the grain's first 64 samples (a step would click; soft_clip takes the overshoot
		// meanwhile), up across the whole grain
		int ramp = want < s_lim ? 64 : GRAIN;
		float g = s_lim, dg = (g_to - s_lim) / ramp;
		s_lim = g_to;
		for (int i = 0; i < GRAIN; i++) {
			acc[2 * i] *= g;
			acc[2 * i + 1] *= g;
			if (i < ramp)
				g += dg;
		}
		for (int i = 0; i < GRAIN; i++) {
			float l = acc[2 * i], r = acc[2 * i + 1];
			if (s_mode == NDSP_OUTPUT_MONO)
				l = r = 0.5f * (l + r);
			pcm[cur][2 * i] = soft_clip(l);
			pcm[cur][2 * i + 1] = soft_clip(r);
		}
		static unsigned grains;
		if (++grains >= OUT_RATE / GRAIN) {    // about once a second
			char b[160];
			int n = 0;
			for (int k = 0; k < NCH; k++)
				if (s_starved[k])
					n += snprintf(b + n, sizeof b - n, " ch%d:%u", k, s_starved[k]);
			if (vita_diag_log && (n || s_clipped || s_late))
				vita_log("audio: clipped %u, late grains %u, ran dry%s", s_clipped, s_late, n ? b : " none");
			memset(s_starved, 0, sizeof s_starved);
			s_clipped = s_late = 0;
			grains = 0;
		}
		if (s_cap_on) {
			size_t room = (size_t)CAP_SECONDS * OUT_RATE * 2 - s_cap_mix_n;
			size_t n = room < (size_t)GRAIN * 2 ? room : (size_t)GRAIN * 2;
			memcpy(s_cap_mix + s_cap_mix_n, pcm[cur], n * sizeof(s16));
			s_cap_mix_n += n;
			if (s_cap_mix_n >= (size_t)CAP_SECONDS * OUT_RATE * 2)
				cap_write();
		}
		// a grain handed over more than one grain after the last means the port ran out meanwhile
		static u64 last;
		u64 now = sceKernelGetProcessTimeWide();
		if (last && now - last > (u64)GRAIN * 1000000 / OUT_RATE * 3 / 2)
			s_late++;
		sceAudioOutOutput(s_port, pcm[cur]);   // blocks until the port takes the grain
		last = sceKernelGetProcessTimeWide();
		cur ^= 1;
	}
	return NULL;
}

Result ndspInit(void)
{
	if (s_run)
		return 0;
	s_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
	if (s_port < 0) {
		vita_log("ndsp: sceAudioOutOpenPort %08x", s_port);
		return -1;
	}
	for (int i = 0; i < NCH; i++)
		ndspChnReset(i);
	if (!s_cap_done)
		cap_init();
	s_run = true;
	pthread_attr_t a;
	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, 64 * 1024);
	pthread_create(&s_thread, &a, audio_thread, NULL);
	pthread_attr_destroy(&a);
	return 0;
}

void ndspExit(void)
{
	if (!s_run)
		return;
	s_run = false;
	pthread_join(s_thread, NULL);
	sceAudioOutReleasePort(s_port);
	s_port = -1;
}

void ndspSetOutputMode(ndspOutputMode mode) { s_mode = mode; }

void ndspChnReset(int id)
{
	pthread_mutex_lock(&s_lock);
	Chn* c = &s_ch[id];
	for (ndspWaveBuf* b = c->head; b; b = b->next)
		b->status = NDSP_WBUF_DONE;
	float tail[2] = { c->last[0], c->last[1] };
	if (c->head)
		start_tail(c), tail[0] = c->tail[0], tail[1] = c->tail[1];
	int tail_n = c->tail_n;
	memset(c, 0, sizeof *c);
	c->tail[0] = tail[0], c->tail[1] = tail[1], c->tail_n = tail_n;   // a reset mid-sound fades too
	c->step = 1.0f;
	c->gain[0] = c->gain[1] = c->target[0] = c->target[1] = 1.0f;
	c->format = NDSP_FORMAT_STEREO_PCM16;
	pthread_mutex_unlock(&s_lock);
}

void ndspChnSetInterp(int id, ndspInterpType type) { (void)id; (void)type; }
void ndspChnSetRate(int id, float rate) { s_ch[id].step = rate / (float)OUT_RATE; }
void ndspChnSetFormat(int id, u16 format) { s_ch[id].format = format; }

// mix[0] and mix[1]: the front left and right gains (the rest are the 3DS's surround and aux buses)
void ndspChnSetMix(int id, float m[12])
{
	Chn* c = &s_ch[id];
	c->target[0] = m[0];
	c->target[1] = m[1];
	if (!c->head)                    // nothing playing: the new gain from the first sample
		c->gain[0] = m[0], c->gain[1] = m[1];
}

void ndspChnSetPaused(int id, bool paused) { s_ch[id].paused = paused; }
bool ndspChnIsPaused(int id) { return s_ch[id].paused; }
bool ndspChnIsPlaying(int id) { return s_ch[id].head != NULL; }

u32 ndspChnGetSamplePos(int id)
{
	pthread_mutex_lock(&s_lock);
	u32 p = s_ch[id].head ? (u32)s_ch[id].pos : 0;
	pthread_mutex_unlock(&s_lock);
	return p;
}

u16 ndspChnGetWaveBufSeq(int id) { return s_ch[id].head ? s_ch[id].seq_playing : 0; }

void ndspChnWaveBufAdd(int id, ndspWaveBuf* buf)
{
	if (!buf || !buf->nsamples)
		return;
	pthread_mutex_lock(&s_lock);
	Chn* c = &s_ch[id];
	if (s_cap_on && id == 0 && (c->format & 3) == 2 && ((c->format >> 2) & 3) == NDSP_ENCODING_PCM16) {
		static float rate_logged;
		if (rate_logged != c->step * OUT_RATE) {
			rate_logged = c->step * OUT_RATE;
			vita_log("audio capture: music at %.0f Hz", rate_logged);
		}
		size_t n = (size_t)buf->nsamples * 2;
		if (n > s_cap_mus_max - s_cap_mus_n)
			n = s_cap_mus_max - s_cap_mus_n;
		memcpy(s_cap_mus + s_cap_mus_n, buf->data_pcm16, n * sizeof(s16));
		s_cap_mus_n += n;
	}
	buf->next = NULL;
	buf->sequence_id = ++c->seq_next;
	if (!c->head) {
		c->head = buf;
		c->pos = buf->offset;
		buf->status = NDSP_WBUF_PLAYING;
		c->seq_playing = buf->sequence_id;
	} else {
		buf->status = NDSP_WBUF_QUEUED;
		ndspWaveBuf* t = c->head;
		while (t->next)
			t = t->next;
		t->next = buf;
	}
	pthread_mutex_unlock(&s_lock);
}

void ndspChnWaveBufClear(int id)
{
	pthread_mutex_lock(&s_lock);
	Chn* c = &s_ch[id];
	for (ndspWaveBuf* b = c->head; b; b = b->next)
		b->status = NDSP_WBUF_DONE;
	if (c->head)
		start_tail(c);                  // a stop mid-sound fades out (DECLICK)
	c->head = NULL;
	c->pos = 0;
	pthread_mutex_unlock(&s_lock);
}
