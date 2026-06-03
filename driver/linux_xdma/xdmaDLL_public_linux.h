#ifndef XDMA_DLL_PUBLIC_LINUX_H
#define XDMA_DLL_PUBLIC_LINUX_H

#include <stddef.h>
#include <stdint.h>

#include <errno.h>
#include <unistd.h>

#if defined(K7CTRBOARDDRIVER_EXPORTS)
#define K7CTRBOARDDRIVER_API __attribute__((visibility("default")))
#else
#define K7CTRBOARDDRIVER_API __attribute__((visibility("default")))
#endif

typedef uint8_t BYTE;
typedef uint32_t DWORD;
typedef void* HANDLE;
typedef int BOOL;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#endif

#ifndef FILE_BEGIN
#define FILE_BEGIN 0
#endif

#ifndef INVALID_SET_FILE_POINTER
#define INVALID_SET_FILE_POINTER ((DWORD)0xFFFFFFFFu)
#endif

#ifndef GENERIC_READ
#define GENERIC_READ 0x80000000u
#endif
#ifndef GENERIC_WRITE
#define GENERIC_WRITE 0x40000000u
#endif

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

typedef struct _GUID {
	uint32_t Data1;
	uint16_t Data2;
	uint16_t Data3;
	uint8_t Data4[8];
} GUID;

#ifndef DEFINE_GUID
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
	static const GUID name = {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}}
#endif

DEFINE_GUID(GUID_DEVINTERFACE_XDMA,
	0x74c7e4a9, 0x6d5d, 0x4a70, 0xbc, 0x0d, 0x20, 0x69, 0x1d, 0xff, 0x9e, 0x9d);

#define	XDMA_FILE_USER		"user"
#define	XDMA_FILE_H2C_0		"h2c_0"
#define	XDMA_FILE_C2H_0		"c2h_0"
#define	XDMA_FILE_H2C_1		"h2c_1"
#define	XDMA_FILE_C2H_1		"c2h_1"
#define	XDMA_FILE_H2C_2		"h2c_2"
#define	XDMA_FILE_C2H_2		"c2h_2"
#define	XDMA_FILE_H2C_3		"h2c_3"
#define	XDMA_FILE_C2H_3		"c2h_3"
#define	XDMA_FILE_BYPASS	"bypass"
#define	XDMA_FILE_EVENT_0   "event_0"

static inline HANDLE xdma_fd_to_handle(int fd)
{
	if (fd < 0) {
		return INVALID_HANDLE_VALUE;
	}
	return (HANDLE)(intptr_t)(fd + 1);
}

static inline int xdma_handle_to_fd(HANDLE handle)
{
	intptr_t raw = (intptr_t)handle;
	if (handle == NULL || handle == INVALID_HANDLE_VALUE || raw <= 0) {
		return -1;
	}
	return (int)(raw - 1);
}

static inline BOOL CloseHandle(HANDLE handle)
{
	int fd = xdma_handle_to_fd(handle);
	if (fd < 0) {
		errno = EBADF;
		return FALSE;
	}
	return close(fd) == 0 ? TRUE : FALSE;
}

#ifdef __cplusplus
extern "C" {
#endif

K7CTRBOARDDRIVER_API BYTE* allocate_buffer(size_t size, size_t alignment);
K7CTRBOARDDRIVER_API void free_buffer(BYTE* buf);

K7CTRBOARDDRIVER_API int write_device(HANDLE device, long address, DWORD size, BYTE *buffer);
K7CTRBOARDDRIVER_API int read_device(HANDLE device, long address, DWORD size, BYTE *buffer);

K7CTRBOARDDRIVER_API int get_devices(GUID guid, char** devpath, size_t len_devpath);
K7CTRBOARDDRIVER_API int open_devices(HANDLE *device_hd, DWORD dwAccessPatter, char *device_base_path, const char*device_name);

K7CTRBOARDDRIVER_API int reset_devices(HANDLE device_hd);
K7CTRBOARDDRIVER_API int ready_state(HANDLE device_hd, unsigned int* opstate, unsigned int* DDRstate);
K7CTRBOARDDRIVER_API int last_packetEn(HANDLE device_hd);
K7CTRBOARDDRIVER_API int last_packetSize(HANDLE device_hd);
K7CTRBOARDDRIVER_API int GXset_channel(HANDLE device_hd, int ch);

#ifdef __cplusplus
}
#endif

#endif
