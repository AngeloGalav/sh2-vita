// Y2R, the 3DS's YUV to RGB unit. The Vita has none: y2rInit fails and movie.c takes its CPU
// conversion path (the same tiled RGB565 layout).
#pragma once

typedef enum { INPUT_YUV422_INDIV_8 = 0, INPUT_YUV420_INDIV_8 = 1 } Y2RU_InputFormat;
typedef enum { OUTPUT_RGB_32 = 0, OUTPUT_RGB_24 = 1, OUTPUT_RGB_16_555 = 2, OUTPUT_RGB_16_565 = 3 } Y2RU_OutputFormat;
typedef enum { ROTATION_NONE = 0 } Y2RU_Rotation;
typedef enum { BLOCK_LINE = 0, BLOCK_8_BY_8 = 1 } Y2RU_BlockAlignment;
typedef enum { COEFFICIENT_ITU_R_BT_601 = 0, COEFFICIENT_ITU_R_BT_709 = 1,
               COEFFICIENT_ITU_R_BT_601_SCALING = 2, COEFFICIENT_ITU_R_BT_709_SCALING = 3 } Y2RU_StandardCoefficient;

typedef struct {
	Y2RU_InputFormat input_format : 8;
	Y2RU_OutputFormat output_format : 8;
	Y2RU_Rotation rotation : 8;
	Y2RU_BlockAlignment block_alignment : 8;
	s16 input_line_width;
	s16 input_lines;
	Y2RU_StandardCoefficient standard_coefficient : 8;
	u8 unused;
	u16 alpha;
} Y2RU_ConversionParams;

Result y2rInit(void);
void y2rExit(void);
Result Y2RU_SetTransferEndInterrupt(bool enable);
Result Y2RU_GetTransferEndEvent(Handle* ev);
Result Y2RU_SetConversionParams(const Y2RU_ConversionParams* p);
Result Y2RU_SetSendingY(const void* src, u32 size, s16 line, s16 gap);
Result Y2RU_SetSendingU(const void* src, u32 size, s16 line, s16 gap);
Result Y2RU_SetSendingV(const void* src, u32 size, s16 line, s16 gap);
Result Y2RU_SetReceiving(void* dst, u32 size, s16 line, s16 gap);
Result Y2RU_StartConversion(void);
Result Y2RU_StopConversion(void);
