// libctru's system calls for the Vita build: time, threads and locks, memory, input, the system
// language, the SD card paths and the 3DS framebuffers nodata.c draws into. Vita-only; there is
// no PC counterpart (the PC game sits on Win32, the 3DS port on libctru).
#include <3ds.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/io/stat.h>
#include <pthread.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include "vita.h"
#include "c3d_internal.h"

#define HEAP_SIZE (160 * 1024 * 1024)    // the newlib heap (see linearAlloc)

// The process's own thread has 256 KB of stack, and the system's shader compiler (libshacccg,
// called from glCompileShader) fails with "fatal internal error" on that. The game's main() runs
// on a thread of its own instead (the linker's --wrap=main sends the start-up code here).
#define GAME_STACK (2 * 1024 * 1024)
int __real_main(int argc, char** argv);
static int s_argc, s_ret;
static char** s_argv;

static int game_thread(SceSize args, void* argp)
{
	(void)args;
	(void)argp;
	s_ret = __real_main(s_argc, s_argv);
	return 0;
}

// profile.c sizes the textures and their cache from the 3DS's heaps (__system_allocateHeaps, never
// called here): left at 0 they read as a 3DS with no linear memory, which halved every texture,
// capped map textures at 256 and characters at 512, and gave the texture cache 6 MB. The Vita
// stands for the New 3DS's 124 MB mode as profile.c splits it: 32 MB of malloc, the rest linear.
extern u32 __ctru_heap_size, __ctru_linear_heap_size;

int __wrap_main(int argc, char** argv)
{
	// The data folder had the 3DS port's name: an old install moves over, saves and all, before anything opens it
	SceIoStat st;
	if (sceIoGetstat(VITA_DATA_DIR, &st) < 0 && sceIoGetstat("ux0:data/sh23ds", &st) >= 0)
		sceIoRename("ux0:data/sh23ds", VITA_DATA_DIR);
	__ctru_heap_size = 32u << 20;
	__ctru_linear_heap_size = 92u << 20;
	vita_log("sh2vita start: heap %u MB, %u KB free", HEAP_SIZE >> 20, (unsigned)(linearSpaceFree() >> 10));
	s_argc = argc;
	s_argv = argv;
	SceUID t = sceKernelCreateThread("sh2vita", game_thread, 0xA0, GAME_STACK, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
	if (t < 0)
		return __real_main(argc, argv);
	sceKernelStartThread(t, 0, NULL);
	sceKernelWaitThreadEnd(t, NULL, NULL);
	sceKernelDeleteThread(t);
	return s_ret;
}

// ---------------------------------------------------------------- time

u64 svcGetSystemTick(void)
{
	u64 us = sceKernelGetProcessTimeWide();
	return (us / 1000000ULL) * SYSCLOCK_ARM11 + (us % 1000000ULL) * SYSCLOCK_ARM11 / 1000000ULL;
}

void svcSleepThread(s64 ns)
{
	if (ns > 0)
		sceKernelDelayThread((SceUInt)(ns / 1000));
	else
		sceKernelDelayThread(0);
}

u64 osGetTime(void) { return sceKernelGetProcessTimeWide() / 1000ULL; }

void svcBreak(u32 reason)
{
	vita_log("svcBreak %lu", (unsigned long)reason);
	abort();
}

s32 svcGetProcessorID(void) { return 0; }
Result svcGetThreadPriority(s32* prio, Handle thread) { (void)thread; *prio = 0x30; return 0; }
Result svcFlushProcessDataCache(Handle p, u32 addr, u32 size) { (void)p; (void)addr; (void)size; return 0; }
Result svcCloseHandle(Handle h) { (void)h; return 0; }
Result svcClearEvent(Handle h) { (void)h; return 0; }
Result svcWaitSynchronization(Handle h, s64 ns) { (void)h; (void)ns; return -1; }
Result svcGetResourceLimit(Handle* r, Handle p) { (void)p; *r = 0; return -1; }
Result svcGetResourceLimitLimitValues(s64* v, Handle r, ResourceLimitType* n, s32 c) { (void)v; (void)r; (void)n; (void)c; return -1; }
Result svcGetResourceLimitCurrentValues(s64* v, Handle r, ResourceLimitType* n, s32 c) { (void)v; (void)r; (void)n; (void)c; return -1; }
Result svcControlMemory(u32* a, u32 a0, u32 a1, u32 s, MemOp op, MemPerm p) { (void)a; (void)a0; (void)a1; (void)s; (void)op; (void)p; return -1; }
void mappableInit(u32 lo, u32 hi) { (void)lo; (void)hi; }

// ---------------------------------------------------------------- memory

// The newlib heap holds both what the 3DS kept in malloc and in its linear heap (textures, vertex
// and index buffers, sound buffers): 123 MB on the New 3DS, more than newlib's default 128 MB
// leaves room for once the layer's own buffers are added. vitaGL takes RAM beyond it for the GL
// copies (vglInitExtended in c3d_gl.c).
__attribute__((used)) int _newlib_heap_size_user = HEAP_SIZE;

static void* logged(void* p, size_t size, const char* what)
{
	static unsigned fails;
	if (!p && fails++ < 50)
		vita_log("%s of %u bytes failed (%u KB free)", what, (unsigned)size, (unsigned)(linearSpaceFree() >> 10));
	return p;
}

// The blocks handed out, by address, so that a pointer into one (a batch's indices, a chunk's run
// of them) finds the whole block (c3d_linear_block, for the GL buffer objects of c3d_gl.c)
#define MAX_BLOCKS 16384
static struct { const u8* base; u32 size; } s_blocks[MAX_BLOCKS];
static int s_nblocks;

static int block_at(const void* p)            // the first block whose base is above p
{
	int lo = 0, hi = s_nblocks;
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		if (s_blocks[mid].base <= (const u8*)p)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static void* track(void* p, size_t size)
{
	if (p && s_nblocks < MAX_BLOCKS) {
		int i = block_at(p);
		memmove(&s_blocks[i + 1], &s_blocks[i], (s_nblocks - i) * sizeof s_blocks[0]);
		s_blocks[i].base = p, s_blocks[i].size = (u32)size;
		s_nblocks++;
	}
	return p;
}

static void untrack(const void* p)
{
	int i = block_at(p) - 1;
	if (i >= 0 && s_blocks[i].base == p) {
		memmove(&s_blocks[i], &s_blocks[i + 1], (s_nblocks - i - 1) * sizeof s_blocks[0]);
		s_nblocks--;
	}
}

bool c3d_linear_block(const void* p, const u8** base, u32* size)
{
	int i = block_at(p) - 1;
	if (i < 0 || (const u8*)p >= s_blocks[i].base + s_blocks[i].size)
		return false;
	*base = s_blocks[i].base, *size = s_blocks[i].size;
	return true;
}

void* linearAlloc(size_t size) { return track(logged(memalign(0x80, size), size, "linearAlloc"), size); }
void* linearMemAlign(size_t size, size_t alignment)
{
	return track(logged(memalign(alignment < 0x80 ? 0x80 : alignment, size), size, "linearMemAlign"), size);
}
void linearFree(void* mem)
{
	c3d_buf_forget(mem);
	untrack(mem);
	free(mem);
}
void* vramAlloc(size_t size) { return track(logged(memalign(0x80, size), size, "vramAlloc"), size); }
void vramFree(void* mem)
{
	c3d_buf_forget(mem);
	untrack(mem);
	free(mem);
}

// The game sizes its caches by what is left (the streamer keeps fewer tiles when linear memory is
// short), so this is the heap's real free space, less 8 MB kept for the layer's upload buffers
u32 linearSpaceFree(void)
{
	struct mallinfo mi = mallinfo();
	size_t used = mi.uordblks, keep = 8u << 20;
	return used + keep < HEAP_SIZE ? (u32)(HEAP_SIZE - used - keep) : 0;
}
u32 vramSpaceFree(void) { return 6u << 20; }

// ---------------------------------------------------------------- threads and locks

struct Thread_tag {
	pthread_t t;
	ThreadFunc entry;
	void* arg;
	bool detached;
};

// The 3DS ran the game's workers (music and effect readers, the tile streamer, the movie decoders)
// on its second core, away from the main loop. Here the main thread keeps core 0 and the workers
// share cores 1 and 2, above the main thread's priority, so a heavy frame can't starve the music.
#define WORKER_PRIORITY 0x60
#define WORKER_CORES (SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2)

static void* thread_main(void* p)
{
	struct Thread_tag* t = p;
	sceKernelChangeThreadPriority(0, WORKER_PRIORITY);
	sceKernelChangeThreadCpuAffinityMask(0, WORKER_CORES);
	t->entry(t->arg);
	return NULL;
}

// The 3DS priorities (0x18 highest .. 0x3F) and cores are replaced by thread_main's.
Thread threadCreate(ThreadFunc entry, void* arg, size_t stack_size, int prio, int core_id, bool detached)
{
	(void)prio;
	(void)core_id;
	struct Thread_tag* t = calloc(1, sizeof *t);
	if (!t)
		return NULL;
	t->entry = entry;
	t->arg = arg;
	t->detached = detached;
	pthread_attr_t a;
	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, stack_size < 0x4000 ? 0x4000 : stack_size * 2);
	if (detached)
		pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
	int r = pthread_create(&t->t, &a, thread_main, t);
	pthread_attr_destroy(&a);
	if (r) {
		free(t);
		return NULL;
	}
	return t;
}

Result threadJoin(Thread t, u64 timeout_ns)
{
	(void)timeout_ns;
	if (!t || t->detached)
		return -1;
	pthread_join(t->t, NULL);
	return 0;
}

void threadFree(Thread t)
{
	if (t && !t->detached)
		free(t);
}

Handle threadGetHandle(Thread t) { (void)t; return 0; }

void LightLock_Init(LightLock* l)
{
	pthread_mutex_t* m = malloc(sizeof *m);
	pthread_mutex_init(m, NULL);
	l->impl = m;
}
void LightLock_Lock(LightLock* l) { pthread_mutex_lock((pthread_mutex_t*)l->impl); }
void LightLock_Unlock(LightLock* l) { pthread_mutex_unlock((pthread_mutex_t*)l->impl); }

typedef struct {
	pthread_mutex_t m;
	pthread_cond_t c;
	bool set;
	ResetType reset;
} Event;

void LightEvent_Init(LightEvent* ev, ResetType reset)
{
	Event* e = calloc(1, sizeof *e);
	pthread_mutex_init(&e->m, NULL);
	pthread_cond_init(&e->c, NULL);
	e->reset = reset;
	ev->impl = e;
}

void LightEvent_Signal(LightEvent* ev)
{
	Event* e = ev->impl;
	pthread_mutex_lock(&e->m);
	e->set = true;
	pthread_cond_broadcast(&e->c);
	pthread_mutex_unlock(&e->m);
}

void LightEvent_Clear(LightEvent* ev)
{
	Event* e = ev->impl;
	pthread_mutex_lock(&e->m);
	e->set = false;
	pthread_mutex_unlock(&e->m);
}

void LightEvent_Wait(LightEvent* ev)
{
	Event* e = ev->impl;
	pthread_mutex_lock(&e->m);
	while (!e->set)
		pthread_cond_wait(&e->c, &e->m);
	if (e->reset == RESET_ONESHOT)
		e->set = false;
	pthread_mutex_unlock(&e->m);
}

int LightEvent_WaitTimeout(LightEvent* ev, s64 timeout_ns)
{
	Event* e = ev->impl;
	u64 until = sceKernelGetProcessTimeWide() + (u64)(timeout_ns / 1000);
	for (;;) {
		pthread_mutex_lock(&e->m);
		if (e->set) {
			if (e->reset == RESET_ONESHOT)
				e->set = false;
			pthread_mutex_unlock(&e->m);
			return 0;
		}
		pthread_mutex_unlock(&e->m);
		if (sceKernelGetProcessTimeWide() >= until)
			return 1;
		sceKernelDelayThread(500);
	}
}

// ---------------------------------------------------------------- applet

bool aptMainLoop(void) { return !vita_quit_requested(); }
bool aptShouldClose(void) { return vita_quit_requested(); }
Result APT_SetAppCpuTimeLimit(u32 percent) { (void)percent; return 0; }
Result APT_CheckNew3DS(bool* out) { *out = true; return 0; }   // the Vita runs the New 3DS profile
void osSetSpeedupEnable(bool enable) { (void)enable; }
float osGet3DSliderState(void) { return 0.0f; }

// ---------------------------------------------------------------- input

static u32 s_held, s_down, s_up;
static circlePosition s_circle, s_cstick;
static touchPosition s_touch;
static bool s_cross_confirms = true;
static bool s_input_ready;

static void input_init(void)
{
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
	SceAppUtilInitParam init = { 0 };
	SceAppUtilBootParam boot = { 0 };
	sceAppUtilInit(&init, &boot);
	int enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);
	// The buttons are fixed whatever the console's setting: Cross acts and confirms, Circle is the
	// flashlight and goes back (controls.c), so a Circle-confirm console plays the same
	(void)enter;
	s_cross_confirms = true;
	s_input_ready = true;
}

// A Vita stick (0..255, 128 centred, down positive) as the Circle Pad reads (about +-156, up
// positive). The dead zone is radial and wide enough for a worn stick's drift, and the range past
// it is stretched back to the full +-156, so a stick pushed to its edge reads as the 3DS's does.
#define STICK_DEAD 28
static void stick(int vx, int vy, circlePosition* out)
{
	float x = (float)(vx - 128), y = (float)(128 - vy);
	float m = sqrtf(x * x + y * y);
	if (m < STICK_DEAD) {
		out->dx = out->dy = 0;
		return;
	}
	float s = (m > 127.0f ? 127.0f : m);
	s = (s - STICK_DEAD) / (127.0f - STICK_DEAD) * 156.0f / m;
	out->dx = (s16)(x * s);
	out->dy = (s16)(y * s);
}

static u32 stick_keys(const circlePosition* p, u32 right, u32 left, u32 up, u32 down)
{
	u32 k = 0;
	if (p->dx > 40) k |= right;
	if (p->dx < -40) k |= left;
	if (p->dy > 40) k |= up;
	if (p->dy < -40) k |= down;
	return k;
}

// The face buttons (port/vita/README.md, Input): A is the console's confirm button, so Cross or
// Circle by the system setting, B the other; Triangle is X, Square is Y.
bool vita_cross_confirms(void)
{
	if (!s_input_ready)
		input_init();
	return s_cross_confirms;
}

void hidScanInput(void)
{
	if (!s_input_ready)
		input_init();
	SceCtrlData pad;
	memset(&pad, 0, sizeof pad);
	// The plain read: the "2" reads (Ext2 among them) report the shoulder buttons as L1 / R1, which
	// this didn't map, so L and R never reached the game (aim, turn round, SELECT + R)
	sceCtrlPeekBufferPositive(0, &pad, 1);
	u32 b = pad.buttons, k = 0;
	u32 confirm = s_cross_confirms ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
	u32 cancel = s_cross_confirms ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
	if (b & confirm) k |= KEY_A;
	if (b & cancel) k |= KEY_B;
	if (b & SCE_CTRL_TRIANGLE) k |= KEY_X;
	if (b & SCE_CTRL_SQUARE) k |= KEY_Y;
	if (b & (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)) k |= KEY_L;
	if (b & (SCE_CTRL_RTRIGGER | SCE_CTRL_R1)) k |= KEY_R;
	if (b & SCE_CTRL_START) k |= KEY_START;
	if (b & SCE_CTRL_SELECT) k |= KEY_SELECT;
	if (b & SCE_CTRL_UP) k |= KEY_DUP;
	if (b & SCE_CTRL_DOWN) k |= KEY_DDOWN;
	if (b & SCE_CTRL_LEFT) k |= KEY_DLEFT;
	if (b & SCE_CTRL_RIGHT) k |= KEY_DRIGHT;

	stick(pad.lx, pad.ly, &s_circle);
	stick(pad.rx, pad.ry, &s_cstick);
	k |= stick_keys(&s_circle, KEY_CPAD_RIGHT, KEY_CPAD_LEFT, KEY_CPAD_UP, KEY_CPAD_DOWN);
	k |= stick_keys(&s_cstick, KEY_CSTICK_RIGHT, KEY_CSTICK_LEFT, KEY_CSTICK_UP, KEY_CSTICK_DOWN);

	// The rear touch pad's outer lower corners stand in for ZL / ZR on a handheld Vita (the pad
	// reports 1920 x 890 or so; the fingers that hold the console rest nearer the middle)
	SceTouchData rear;
	if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &rear, 1) > 0)
		for (unsigned i = 0; i < rear.reportNum; i++)
			if (rear.report[i].y > 600 && (rear.report[i].x < 480 || rear.report[i].x >= 1440))
				k |= rear.report[i].x < 960 ? KEY_ZL : KEY_ZR;

	SceTouchData front;
	if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &front, 1) > 0 && front.reportNum > 0) {
		u16 px, py;
		if (vita_touch_to_bottom(front.report[0].x / 2, front.report[0].y / 2, &px, &py)) {
			s_touch.px = px;
			s_touch.py = py;
			k |= KEY_TOUCH;
		}
	}

	// The test combinations under SELECT (c3d_gl.c, c3d_tex.c) stay away from the game, and SELECT
	// itself, the pause menu's button, reaches it for one frame when let go with none of them pressed
	static u32 raw_held;
	static bool combo;
	u32 raw_down = k & ~raw_held, raw_up = raw_held & ~k;
	raw_held = k;
	if (k & KEY_SELECT) {
		if (raw_down & KEY_X)
			c3d_fog_precision_toggle();
		if (raw_down & KEY_Y)
			c3d_etc1_mips_toggle();
		if (raw_down & KEY_R)
			c3d_gpu_test_next();
		if (raw_down & KEY_L)
			c3d_half_fog_toggle();
		if (raw_down & KEY_DUP) {
			extern void gamecam_trace_dump(void);   // gamecam.c: the camera's last minute, to camtrace.txt
			gamecam_trace_dump();
			vita_log("camera trace written");
		}
		combo |= (raw_down & (KEY_X | KEY_Y | KEY_R | KEY_L | KEY_DUP)) != 0;
		k &= ~(KEY_SELECT | KEY_X | KEY_Y | KEY_R | KEY_L | KEY_DUP);
	} else if (raw_up & KEY_SELECT) {
		if (!combo)
			k |= KEY_SELECT;
		combo = false;
	}
	s_down = k & ~s_held;
	s_up = s_held & ~k;
	s_held = k;
}

u32 hidKeysDown(void) { return s_down; }
u32 hidKeysHeld(void) { return s_held; }
u32 hidKeysUp(void) { return s_up; }
void hidTouchRead(touchPosition* pos) { *pos = (s_held & KEY_TOUCH) ? s_touch : (touchPosition){ 0, 0 }; }
void hidCircleRead(circlePosition* pos) { *pos = s_circle; }
void hidCstickRead(circlePosition* pos) { *pos = s_cstick; }

// ---------------------------------------------------------------- system language

Result cfguInit(void) { return 0; }
void cfguExit(void) { }

Result CFGU_GetSystemLanguage(u8* language)
{
	int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
	if (!s_input_ready)
		input_init();
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
	switch (lang) {
	case SCE_SYSTEM_PARAM_LANG_JAPANESE: *language = CFG_LANGUAGE_JP; break;
	case SCE_SYSTEM_PARAM_LANG_FRENCH: *language = CFG_LANGUAGE_FR; break;
	case SCE_SYSTEM_PARAM_LANG_GERMAN: *language = CFG_LANGUAGE_DE; break;
	case SCE_SYSTEM_PARAM_LANG_ITALIAN: *language = CFG_LANGUAGE_IT; break;
	case SCE_SYSTEM_PARAM_LANG_SPANISH: *language = CFG_LANGUAGE_ES; break;
	default: *language = CFG_LANGUAGE_EN; break;
	}
	return 0;
}

// ---------------------------------------------------------------- the SD card
// The game opens "sdmc:/3ds/sh23ds/..."; on the Vita the pack lives in ux0:data/sh2-vita/, and any
// other sdmc: path under ux0:data/sh2-vita/sdmc/. The linker routes the calls here (--wrap).

const char* vita_path(const char* in, char* buf, size_t n)
{
	static const char game[] = "sdmc:/3ds/sh23ds";
	if (!in || strncmp(in, "sdmc:", 5))
		return in;
	if (!strncmp(in, game, sizeof game - 1))
		snprintf(buf, n, VITA_DATA_DIR "%s", in + sizeof game - 1);
	else
		snprintf(buf, n, VITA_DATA_DIR "/sdmc%s", in + 5);
	return buf;
}

FILE* __real_fopen(const char* path, const char* mode);
DIR* __real_opendir(const char* path);
int __real_stat(const char* path, struct stat* st);
int __real_mkdir(const char* path, mode_t mode);
int __real_remove(const char* path);
int __real_rename(const char* a, const char* b);

// Reads go through a 64 KB buffer: the game reads its files in small pieces (headers, records),
// and newlib's own buffer made each of them a trip to the card (ldhewm3 buffers its reads the same
// way)
FILE* __wrap_fopen(const char* path, const char* mode)
{
	char b[512];
	FILE* f = __real_fopen(vita_path(path, b, sizeof b), mode);
	if (f && mode[0] == 'r' && !strchr(mode, '+'))
		setvbuf(f, NULL, _IOFBF, 64 * 1024);
	return f;
}
DIR* __wrap_opendir(const char* path)
{
	char b[512];
	return __real_opendir(vita_path(path, b, sizeof b));
}
int __wrap_stat(const char* path, struct stat* st)
{
	char b[512];
	return __real_stat(vita_path(path, b, sizeof b), st);
}
int __wrap_mkdir(const char* path, mode_t mode)
{
	char b[512];
	return __real_mkdir(vita_path(path, b, sizeof b), mode);
}
int __wrap_remove(const char* path)
{
	char b[512];
	return __real_remove(vita_path(path, b, sizeof b));
}
int __wrap_rename(const char* a, const char* b)
{
	char x[512], y[512];
	return __real_rename(vita_path(a, x, sizeof x), vita_path(b, y, sizeof y));
}

FS_Path fsMakePath(FS_PathType type, const void* path)
{
	FS_Path p = { type, path ? (u32)strlen(path) + 1 : 0, path };
	return p;
}

Result FSUSER_OpenFileDirectly(Handle* out, FS_ArchiveID arch, FS_Path ap, FS_Path fp, u32 flags, u32 attr)
{
	(void)arch; (void)ap; (void)attr;
	char b[512];
	FILE* f = __real_fopen(vita_path(fp.data, b, sizeof b), (flags & FS_OPEN_CREATE) ? "wb" : "r+b");
	*out = (Handle)(uintptr_t)f;
	return f ? 0 : -1;
}

Result FSFILE_SetSize(Handle h, u64 size) { (void)h; (void)size; return 0; }

Result FSFILE_Write(Handle h, u32* done, u64 offset, const void* buf, u32 size, u32 flags)
{
	(void)flags;
	FILE* f = (FILE*)(uintptr_t)h;
	fseek(f, (long)offset, SEEK_SET);
	*done = (u32)fwrite(buf, 1, size, f);
	return *done == size ? 0 : -1;
}

Result FSFILE_Close(Handle h)
{
	fclose((FILE*)(uintptr_t)h);
	return 0;
}

// ---------------------------------------------------------------- the 3DS framebuffers
// nodata.c draws its screen in software into the 3DS framebuffers (top BGR8 240 x 400, bottom
// RGB565 240 x 320, both column-major from the bottom left); c3d_gl.c shows them on gfxSwapBuffers.

static u8 s_fb_top[240 * 400 * 3];
static u8 s_fb_bottom[240 * 320 * 3];

// vita-elf-create needs room for its SCE data at the end of the first segment and crashes when the
// code ends a few KB short of the next segment's boundary: this moves it past (remove if it fails the
// other way, when the code has grown)
static const volatile char s_segment_pad[16384] = { 1 };

void gfxInitDefault(void)
{
	(void)s_segment_pad[0];
	vita_gl_init();
}
void gfxExit(void) { }
void gfxSet3D(bool enable) { (void)enable; }

u8* gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16* width, u16* height)
{
	(void)side;
	if (width)
		*width = 240;
	if (height)
		*height = screen == GFX_TOP ? 400 : 320;
	return screen == GFX_TOP ? s_fb_top : s_fb_bottom;
}

void gfxFlushBuffers(void) { }
void gfxSwapBuffers(void) { vita_present_framebuffers(s_fb_top, s_fb_bottom); }
void gspWaitForVBlank(void) { sceKernelDelayThread(16666); }

static PrintConsole s_console;
PrintConsole* consoleInit(gfxScreen_t screen, PrintConsole* console)
{
	(void)screen;
	PrintConsole* c = console ? console : &s_console;
	c->frameBuffer = NULL;
	return c;
}
PrintConsole* consoleSelect(PrintConsole* console) { return console; }
void consoleClear(void) { }
