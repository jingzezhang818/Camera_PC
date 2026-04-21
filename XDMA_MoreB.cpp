// XDMAdll_V10.cpp
// 该文件实现 XDMA 封装 DLL 的导出函数，供上层 Qt 程序直接调用。
// 主要能力：
// 1) 对齐内存申请与释放；
// 2) 设备文件读写；
// 3) XDMA 设备枚举与通道打开；
// 4) 若干寄存器级控制/状态接口。
#include "XDMA_MoreB.h"
#include "xdmaDLL_public.h"

// 申请对齐缓冲区。
// - size=0 时兜底为 4 字节，避免返回空指针；
// - alignment=0 时使用系统页大小，适配 DMA 常见对齐要求。
BYTE* allocate_buffer(size_t size, size_t alignment)
{

	if (size == 0) {
		size = 4;
	}

	if (alignment == 0) {
		SYSTEM_INFO sys_info;
		GetSystemInfo(&sys_info);
		alignment = sys_info.dwPageSize;
		//printf("alignment = %d\n",alignment);
	}
	return (BYTE*)_aligned_malloc(size, alignment);
}

// 释放 allocate_buffer 申请的对齐内存。
void free_buffer(BYTE* buf)
{
	_aligned_free(buf);
}

// 向指定通道写数据。
// 返回值约定：
// - >0: 实际写入字节数（正常为 size）；
// - -1: WriteFile 失败；
// - -2: 短写（写入字节数与请求不一致）；
// - -3: SetFilePointer 失败。
int write_device(HANDLE device, long address, DWORD size, BYTE *buffer)
{
	DWORD wr_size = 0;
	if (INVALID_SET_FILE_POINTER == SetFilePointer(device, address, NULL, FILE_BEGIN)) {
		fprintf(stderr, "Error setting file pointer, win32 error code: %ld\n", GetLastError());
		return -3;
	}
	if (!WriteFile(device, (void *)(buffer), (DWORD)(size), &wr_size, NULL))
	{
		return -1;
	}
	if (wr_size != (size))
	{
		return -2;
	}
	return size;
}

// 从指定通道读数据。
// 返回值约定与 write_device 对称：
// - >0: 实际读取字节数（正常为 size）；
// - -1: ReadFile 失败；
// - -2: 短读；
// - -3: SetFilePointer 失败。
int read_device(HANDLE device, long address, DWORD size, BYTE *buffer)
{
	DWORD wr_size = 0;
	if (INVALID_SET_FILE_POINTER == SetFilePointer(device, address, NULL, FILE_BEGIN)) {
		fprintf(stderr, "Error setting file pointer, win32 error code: %ld\n", GetLastError());
		return -3;
	}
	if (!ReadFile(device, (void *)(buffer), (DWORD)(size), &wr_size, NULL))
	{
		return -1;
	}
	if (wr_size != (size))
	{
		return -2;
	}
	return size;
}

// 通过 XDMA GUID 枚举设备路径。
// 调用方需提供 devpath 二维缓冲区，函数按 index 写入每条路径。
int get_devices(GUID guid, char** devpath, size_t len_devpath)
{

	SP_DEVICE_INTERFACE_DATA device_interface;
	PSP_DEVICE_INTERFACE_DETAIL_DATA dev_detail;
	DWORD index;
	HDEVINFO device_info;
	wchar_t tmp[256];
	device_info = SetupDiGetClassDevs((LPGUID)&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (device_info == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "GetDevices INVALID_HANDLE_VALUE\n");
		exit(-1);
	}

	device_interface.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
	// 枚举所有匹配 GUID 的设备接口。

	for (index = 0; SetupDiEnumDeviceInterfaces(device_info, NULL, &guid, index, &device_interface); ++index) {

		// 先查询接口详情所需缓冲区大小。
		ULONG detailLength = 0;
		if (!SetupDiGetDeviceInterfaceDetail(device_info, &device_interface, NULL, 0, &detailLength, NULL) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
			fprintf(stderr, "SetupDiGetDeviceInterfaceDetail - get length failed\n");
			break;
		}

		// 申请接口详情结构体缓冲区。
		dev_detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, detailLength);
		if (!dev_detail) {
			fprintf(stderr, "HeapAlloc failed\n");
			break;
		}
		dev_detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

		// 读取设备路径详情并拷贝到调用方缓冲区。
		if (!SetupDiGetDeviceInterfaceDetail(device_info, &device_interface, dev_detail, detailLength, NULL, NULL)) {
			fprintf(stderr, "SetupDiGetDeviceInterfaceDetail - get detail failed\n");
			HeapFree(GetProcessHeap(), 0, dev_detail);
			break;
		}
		StringCchCopy(tmp, len_devpath, dev_detail->DevicePath);
		wcstombs(devpath[index], tmp, 256);
		HeapFree(GetProcessHeap(), 0, dev_detail);
	}

	SetupDiDestroyDeviceInfoList(device_info);

	return index;
}

// 打开指定设备子通道（如 user、h2c_0、c2h_0）。
// device_base_path 由 get_devices 返回，device_name 为子节点名。
int open_devices(HANDLE *device_hd, DWORD dwAccessPatter, char *device_base_path, const char *device_name)
{
	// 组合最终路径：<base_path>\<device_name>
	char device_path[MAX_PATH + 1] = "";
	wchar_t device_path_w[MAX_PATH + 1];
	strcpy_s(device_path, sizeof device_path, device_base_path);
	strcat_s(device_path, sizeof device_path, "\\");
	strcat_s(device_path, sizeof device_path, device_name);
	// 打开设备文件句柄。
	mbstowcs(device_path_w, device_path, sizeof(device_path));
	printf("%s\n", device_path);
	*device_hd = CreateFile(device_path_w, dwAccessPatter, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (*device_hd == INVALID_HANDLE_VALUE)
	{
		fprintf(stderr, "Error opening device, win32 error code: %ld\n", GetLastError());
		return 0;
	}
	return 1;
}

// 设备复位：向 0x00 寄存器写 1 再写 0。
int reset_devices(HANDLE device_hd)
{
	unsigned int val = 1;
	int ret = 0;
	if (ret = write_device(device_hd, 0x00, 4, (BYTE*)&val), ret < 0) {
		return ret;
	}
	val = 0;
	if (ret = write_device(device_hd, 0x00, 4, (BYTE*)&val), ret < 0) {
		return ret;
	}
	return 0;
}

// 读取系统状态：
// - 0x00: 光纤状态
// - 0x14: DDR 状态
int ready_state(HANDLE device_hd, unsigned int *opstate, unsigned int *DDRstate)
{
	int ret = 0;
	if (ret = read_device(device_hd, 0x00, 4, (BYTE*)opstate), ret < 0) {
		return ret;
	}
	if (ret = read_device(device_hd, 0x14, 4, (BYTE*)DDRstate), ret < 0) {
		return ret;
	}
	return ret;
}

// 末包使能控制。
int last_packetEn(HANDLE device_hd)
{
	unsigned int val = 1;
	int ret = 0;
	if (ret = write_device(device_hd, 0x1C, 4, (BYTE*)&val), ret < 0) {
		return ret;
	}
	return 0;
}

// 读取末包大小（字节）。
int last_packetSize(HANDLE device_hd)
{
	unsigned int val = 0;
	int ret = 0;
	if (ret = read_device(device_hd, 0x20, 4, (BYTE*)&val), ret < 0) {
		return ret;
	}
	return val;
}

// 选择接收光纤通道。
// 当前实现支持：
// - ch=1 -> val=0
// - ch=2 -> val=1
// 其它输入回退到通道 1。
int GXset_channel(HANDLE device_hd, int ch)
{
	unsigned int val = 0;
	int ret = 0;
	switch (ch)
	{
	case 1:
		val = 0;
		break;
	case 2:
		val = 1;
		break;
	default:
		val = 0;
		break;
	}
	if (ret = write_device(device_hd, 0x24, 4, (BYTE*)&val), ret < 0) {
		return ret;
	}
	return 0;
}