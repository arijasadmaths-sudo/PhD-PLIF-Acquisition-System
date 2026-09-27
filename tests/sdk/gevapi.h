#ifndef TEST_GEVAPI_H
#define TEST_GEVAPI_H
#include "cordef.h"
#define GEVLIB_OK 0
#define GevExclusiveMode 4
#define SynchronousNextEmpty 1
#define TEST_BAYER 0x01080008u
#define TEST_RGB 0x02180014u
#define TEST_MONO 0x01080001u
typedef int GEV_STATUS;
typedef void *GEV_CAMERA_HANDLE;
typedef struct { UINT32 ipAddr; } GEV_DEVICE_INTERFACE;
typedef struct { UINT32 heartbeat_timeout_ms, enable_passthru_mode; } GEV_CAMERA_OPTIONS;
typedef struct { UINT32 status, w, h, format; UINT8 *address; } GEV_BUFFER_OBJECT;
GEV_STATUS GevApiInitialize(void);
GEV_STATUS GevApiUninitialize(void);
GEV_STATUS GevGetCameraList(GEV_DEVICE_INTERFACE *, int, int *);
GEV_STATUS GevOpenCameraByAddress(unsigned long, int, GEV_CAMERA_HANDLE *);
GEV_STATUS GevCloseCamera(GEV_CAMERA_HANDLE *);
GEV_STATUS GevGetCameraInterfaceOptions(GEV_CAMERA_HANDLE, GEV_CAMERA_OPTIONS *);
GEV_STATUS GevSetCameraInterfaceOptions(GEV_CAMERA_HANDLE, GEV_CAMERA_OPTIONS *);
GEV_STATUS GevSetFeatureValueAsString(GEV_CAMERA_HANDLE, const char *, const char *);
GEV_STATUS GevGetFeatureValueAsString(GEV_CAMERA_HANDLE, const char *, int *, int, char *);
GEV_STATUS GevGetFeatureValue(GEV_CAMERA_HANDLE, const char *, int *, int, void *);
GEV_STATUS GevGetPayloadParameters(GEV_CAMERA_HANDLE, UINT64 *, UINT32 *);
GEV_STATUS GevInitializeTransfer(GEV_CAMERA_HANDLE, int, UINT64, UINT32, UINT8 **);
GEV_STATUS GevStartTransfer(GEV_CAMERA_HANDLE, UINT32);
GEV_STATUS GevAbortTransfer(GEV_CAMERA_HANDLE);
GEV_STATUS GevFreeTransfer(GEV_CAMERA_HANDLE);
GEV_STATUS GevWaitForNextImage(GEV_CAMERA_HANDLE, GEV_BUFFER_OBJECT **, UINT32);
GEV_STATUS GevReleaseImage(GEV_CAMERA_HANDLE, GEV_BUFFER_OBJECT *);
UINT32 GevGetConvertedPixelType(int, UINT32);
int GevIsPixelTypeBayer(UINT32);
UINT32 GevGetBayerAsRGBPixelType(UINT32);
UINT32 GevGetPixelComponentCount(UINT32);
void _CloseSocketAPI(void);
#endif
