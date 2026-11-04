// Y2R for the Vita build: there is no such unit, so y2rInit fails and movie.c converts the
// frames on the CPU into the same tiled RGB565 layout. Vita-only.
#include <3ds.h>

Result y2rInit(void) { return -1; }
void y2rExit(void) { }
Result Y2RU_SetTransferEndInterrupt(bool e) { (void)e; return -1; }
Result Y2RU_GetTransferEndEvent(Handle* ev) { *ev = 0; return -1; }
Result Y2RU_SetConversionParams(const Y2RU_ConversionParams* p) { (void)p; return -1; }
Result Y2RU_SetSendingY(const void* s, u32 n, s16 l, s16 g) { (void)s; (void)n; (void)l; (void)g; return -1; }
Result Y2RU_SetSendingU(const void* s, u32 n, s16 l, s16 g) { (void)s; (void)n; (void)l; (void)g; return -1; }
Result Y2RU_SetSendingV(const void* s, u32 n, s16 l, s16 g) { (void)s; (void)n; (void)l; (void)g; return -1; }
Result Y2RU_SetReceiving(void* d, u32 n, s16 l, s16 g) { (void)d; (void)n; (void)l; (void)g; return -1; }
Result Y2RU_StartConversion(void) { return -1; }
Result Y2RU_StopConversion(void) { return -1; }
