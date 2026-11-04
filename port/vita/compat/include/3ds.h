// The libctru calls the game makes, for the PS Vita build. Each one is implemented on the Sce*
// libraries in compat/ctru.c (system, input, threads, memory), compat/ndsp.c (audio) and
// compat/y2r.c (the movie's colour conversion). Names and values follow libctru so the game
// sources compile unchanged; only what the port uses is here.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
typedef volatile u64 vu64;
typedef volatile s32 vs32;
typedef s32 Result;
typedef u32 Handle;
typedef void (*ThreadFunc)(void*);

#define BIT(n) (1U << (n))
#define R_SUCCEEDED(r) ((r) >= 0)
#define R_FAILED(r) ((r) < 0)
#define CUR_PROCESS_HANDLE 0xFFFF8001
#define ALIGN(x) __attribute__((aligned(x)))
#define PACKED __attribute__((packed))

// ---- time ----
// The game counts in the 3DS ARM11 clock; svcGetSystemTick returns the Vita's microsecond clock
// scaled to it, so every SYSCLOCK_ARM11 division in the game still gives seconds.
#define SYSCLOCK_ARM11 268111856ULL
#define CPU_TICKS_PER_MSEC (SYSCLOCK_ARM11 / 1000.0)
u64 svcGetSystemTick(void);
void svcSleepThread(s64 ns);
u64 osGetTime(void);                    // milliseconds
void svcBreak(u32 reason);
s32 svcGetProcessorID(void);
Result svcGetThreadPriority(s32* prio, Handle thread);
Result svcFlushProcessDataCache(Handle process, u32 addr, u32 size);
Result svcCloseHandle(Handle h);
Result svcClearEvent(Handle h);
Result svcWaitSynchronization(Handle h, s64 ns);
#define CUR_THREAD_HANDLE 0xFFFF8000
#define USERBREAK_PANIC 0
#define USERBREAK_ASSERT 1

// The 3DS heap reservation in profile.c: the Vita build has its own profile
typedef enum { RESLIMIT_PRIORITY = 0, RESLIMIT_COMMIT = 1 } ResourceLimitType;
typedef enum { MEMOP_ALLOC = 3, MEMOP_ALLOC_LINEAR = 0x10003 } MemOp;
typedef enum { MEMPERM_READ = 1, MEMPERM_WRITE = 2 } MemPerm;

#define U64_MAX UINT64_MAX
#define __dmb() __sync_synchronize()

// The 3DS heap split in profile.c (libctru's __system_allocateHeaps hook); never called on the Vita
Result svcGetResourceLimit(Handle* reslimit, Handle process);
Result svcGetResourceLimitLimitValues(s64* values, Handle reslimit, ResourceLimitType* names, s32 count);
Result svcGetResourceLimitCurrentValues(s64* values, Handle reslimit, ResourceLimitType* names, s32 count);
Result svcControlMemory(u32* addr_out, u32 addr0, u32 addr1, u32 size, MemOp op, MemPerm perm);
void mappableInit(u32 addrMin, u32 addrMax);
#define OS_HEAP_AREA_BEGIN 0x08000000
#define OS_MAP_AREA_BEGIN 0x10000000
#define OS_MAP_AREA_END 0x14000000

// Cache maintenance: the PICA and the DSP read memory behind the CPU's caches; no-ops here
// ... except that a flush over a texture's pixels means the CPU wrote them: compat/c3d_tex.c
// re-uploads that texture before it is next drawn
void c3d_tex_mark_range(const void* adr, u32 size);
static inline Result GSPGPU_FlushDataCache(const void* adr, u32 size) { c3d_tex_mark_range(adr, size); return 0; }
static inline Result GSPGPU_InvalidateDataCache(const void* adr, u32 size) { (void)adr; (void)size; return 0; }
static inline Result DSP_FlushDataCache(const void* adr, u32 size) { (void)adr; (void)size; return 0; }
static inline Result DSP_InvalidateDataCache(const void* adr, u32 size) { (void)adr; (void)size; return 0; }

// ---- files ----
// main.c's screenshot writes through FSUSER; compat/ctru.c does it with stdio
typedef enum { PATH_INVALID = 0, PATH_EMPTY = 1, PATH_BINARY = 2, PATH_ASCII = 3 } FS_PathType;
typedef enum { ARCHIVE_SDMC = 9 } FS_ArchiveID;
typedef struct { FS_PathType type; u32 size; const void* data; } FS_Path;
enum { FS_OPEN_READ = 1, FS_OPEN_WRITE = 2, FS_OPEN_CREATE = 4 };
enum { FS_WRITE_FLUSH = 1 };
FS_Path fsMakePath(FS_PathType type, const void* path);
Result FSUSER_OpenFileDirectly(Handle* out, FS_ArchiveID archiveId, FS_Path archivePath, FS_Path filePath, u32 openFlags, u32 attributes);
Result FSFILE_SetSize(Handle handle, u64 size);
Result FSFILE_Write(Handle handle, u32* bytesWritten, u64 offset, const void* buffer, u32 size, u32 flags);
Result FSFILE_Close(Handle handle);

// ---- memory ----
// The PICA wants "linear" memory for buffers it reads; on the Vita any heap memory will do,
// since vitaGL copies vertex data into its own GPU memory each draw.
void* linearAlloc(size_t size);
void* linearMemAlign(size_t size, size_t alignment);
void linearFree(void* mem);
u32 linearSpaceFree(void);
void* vramAlloc(size_t size);
void vramFree(void* mem);
u32 vramSpaceFree(void);

// ---- threads and locks ----
typedef struct Thread_tag* Thread;
Thread threadCreate(ThreadFunc entry, void* arg, size_t stack_size, int prio, int core_id, bool detached);
Result threadJoin(Thread thread, u64 timeout_ns);
void threadFree(Thread thread);
Handle threadGetHandle(Thread thread);

typedef struct { void* impl; } LightLock;
void LightLock_Init(LightLock* lock);
void LightLock_Lock(LightLock* lock);
void LightLock_Unlock(LightLock* lock);

typedef enum { RESET_ONESHOT = 0, RESET_STICKY = 1, RESET_PULSE = 2 } ResetType;
typedef struct { void* impl; } LightEvent;
void LightEvent_Init(LightEvent* ev, ResetType reset);
void LightEvent_Signal(LightEvent* ev);
void LightEvent_Clear(LightEvent* ev);
void LightEvent_Wait(LightEvent* ev);
int LightEvent_WaitTimeout(LightEvent* ev, s64 timeout_ns);   // 0 when signalled, 1 on timeout

// ---- applet ----
bool aptMainLoop(void);
bool aptShouldClose(void);
Result APT_SetAppCpuTimeLimit(u32 percent);
Result APT_CheckNew3DS(bool* out);
bool vita_cross_confirms(void);
void vita_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));   // compat/c3d_gl.c   // Vita: KEY_A is Cross when true, Circle otherwise (compat/ctru.c)
void osSetSpeedupEnable(bool enable);
float osGet3DSliderState(void);

// ---- input ----
enum {
	KEY_A = BIT(0), KEY_B = BIT(1), KEY_SELECT = BIT(2), KEY_START = BIT(3),
	KEY_DRIGHT = BIT(4), KEY_DLEFT = BIT(5), KEY_DUP = BIT(6), KEY_DDOWN = BIT(7),
	KEY_R = BIT(8), KEY_L = BIT(9), KEY_X = BIT(10), KEY_Y = BIT(11),
	KEY_ZL = BIT(14), KEY_ZR = BIT(15), KEY_TOUCH = BIT(20),
	KEY_CSTICK_RIGHT = BIT(24), KEY_CSTICK_LEFT = BIT(25), KEY_CSTICK_UP = BIT(26), KEY_CSTICK_DOWN = BIT(27),
	KEY_CPAD_RIGHT = BIT(28), KEY_CPAD_LEFT = BIT(29), KEY_CPAD_UP = BIT(30), KEY_CPAD_DOWN = BIT(31),
	KEY_UP = KEY_DUP | KEY_CPAD_UP, KEY_DOWN = KEY_DDOWN | KEY_CPAD_DOWN,
	KEY_LEFT = KEY_DLEFT | KEY_CPAD_LEFT, KEY_RIGHT = KEY_DRIGHT | KEY_CPAD_RIGHT,
};
typedef struct { s16 dx, dy; } circlePosition;
typedef struct { u16 px, py; } touchPosition;
void hidScanInput(void);
u32 hidKeysDown(void);
u32 hidKeysHeld(void);
u32 hidKeysUp(void);
void hidTouchRead(touchPosition* pos);
void hidCircleRead(circlePosition* pos);
void hidCstickRead(circlePosition* pos);
bool hidShouldUseIrrst(void);

// ---- screens ----
typedef enum { GFX_TOP = 0, GFX_BOTTOM = 1 } gfxScreen_t;
typedef enum { GFX_LEFT = 0, GFX_RIGHT = 1 } gfx3dSide_t;
typedef enum { GSP_RGBA8_OES = 0, GSP_BGR8_OES = 1, GSP_RGB565_OES = 2 } GSPGPU_FramebufferFormat;
void gfxInitDefault(void);
void gfxExit(void);
void gfxSet3D(bool enable);
u8* gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16* width, u16* height);
void gfxFlushBuffers(void);
void gfxSwapBuffers(void);
void gspWaitForVBlank(void);

// The text console (the debug page and nodata.c's fallback); the Vita build prints to the log
typedef struct PrintConsole { u16* frameBuffer; int cursorX, cursorY; } PrintConsole;
PrintConsole* consoleInit(gfxScreen_t screen, PrintConsole* console);
PrintConsole* consoleSelect(PrintConsole* console);
void consoleClear(void);

// GX display transfer flags: the PICA's framebuffer copies, which the Vita build does in GL
#define GX_TRANSFER_FLIP_VERT(x) ((x) << 0)
#define GX_TRANSFER_OUT_TILED(x) ((x) << 1)
#define GX_TRANSFER_RAW_COPY(x) ((x) << 3)
#define GX_TRANSFER_IN_FORMAT(x) ((x) << 8)
#define GX_TRANSFER_OUT_FORMAT(x) ((x) << 12)
#define GX_TRANSFER_SCALING(x) ((x) << 24)
enum { GX_TRANSFER_FMT_RGBA8 = 0, GX_TRANSFER_FMT_RGB8 = 1, GX_TRANSFER_FMT_RGB565 = 2,
       GX_TRANSFER_FMT_RGB5A1 = 3, GX_TRANSFER_FMT_RGBA4 = 4 };
enum { GX_TRANSFER_SCALE_NO = 0, GX_TRANSFER_SCALE_X = 1, GX_TRANSFER_SCALE_XY = 2 };

// ---- system settings ----
typedef enum {
	CFG_LANGUAGE_JP = 0, CFG_LANGUAGE_EN = 1, CFG_LANGUAGE_FR = 2, CFG_LANGUAGE_DE = 3,
	CFG_LANGUAGE_IT = 4, CFG_LANGUAGE_ES = 5, CFG_LANGUAGE_ZH = 6, CFG_LANGUAGE_KO = 7,
	CFG_LANGUAGE_NL = 8, CFG_LANGUAGE_PT = 9, CFG_LANGUAGE_RU = 10, CFG_LANGUAGE_TW = 11,
} CFG_Language;
Result cfguInit(void);
void cfguExit(void);
Result CFGU_GetSystemLanguage(u8* language);

#include "compat_ndsp.h"
#include "compat_y2r.h"
#include "compat_gpu.h"

#define GX_BUFFER_DIM(w, h) (((h) << 16) | ((w) & 0xFFFF))
