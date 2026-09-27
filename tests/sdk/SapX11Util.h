#ifndef TEST_SAP_UTIL_H
#define TEST_SAP_UTIL_H
#include "cordef.h"
UINT32 GetPixelSizeInBytes(UINT32);
void ConvertBayerToRGB(int, UINT32, UINT32, UINT32, void *, UINT32, void *);
#endif
