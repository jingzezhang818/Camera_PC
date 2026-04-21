/********************************************************************************
* @File name: xdmaDLL_public.h
* @Author: __
* @Version: 1.0
* @Date: 2020年10月25日09:55:26
* @Description: This is a file about exporting function declarations

* @ 硬件设计：XDMA-IP核
********************************************************************************/

#ifdef K7CTRBOARDDRIVER_EXPORTS
#define K7CTRBOARDDRIVER_API __declspec(dllexport)
#else
#ifndef K7CTRBOARDDRIVER
#define K7CTRBOARDDRIVER_API __declspec(dllimport)
#else
#define K7CTRBOARDDRIVER_API
#endif
#endif

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <tchar.h>
#include <strsafe.h>

#include <Windows.h>
#include <SetupAPI.h>
#include <INITGUID.H>
//#include <WinIoCtl.h>

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


#ifdef  __cplusplus
extern "C" {
#endif
	/*-
	Function name : allocate_buffer
	Description   : 为缓冲区申请空间
	Parameter
	@ size		  : 缓冲区大小
	@ alignment   : 分页大小（为0时由GetSystemInfo函数获得）
	* Return      : 返回缓冲区首地址
	-*/
	K7CTRBOARDDRIVER_API BYTE* allocate_buffer(size_t size, size_t alignment);

	/*-
	Function name : free_buffer
	Description   : 为缓冲区释放空间
	Parameter
	@ buf		  : 缓冲区
	* Return      : 
	-*/
	K7CTRBOARDDRIVER_API void free_buffer(BYTE* buf);

	/*-
	Function name : write_device
	Description   : 向板卡写入数据（下行模式）
	Parameter
	@ device	  : 通道句柄
	@ address     : 写地址
	@ size        : 写数据长度
	@ buffer      : 写的数据缓冲区
	* Return      : 返回写数据长度
	-*/
	K7CTRBOARDDRIVER_API int write_device(HANDLE device, long address, DWORD size, BYTE *buffer);

	/*-
	Function name : read_device
	Description   : 从板卡读回数据（上行模式）
	Parameter
	@ device	  : 通道句柄
	@ address     : 读地址
	@ size        : 读数据长度
	@ buffer      : 读的数据缓冲区
	* Return      : 返回读数据长度
	-*/
	K7CTRBOARDDRIVER_API int read_device(HANDLE device, long address, DWORD size, BYTE *buffer);

	/*-
	Function name : get_devices
	Description   : 通过GUID获取设备信息
	Parameter
	@ guid	      : GUID值
	@ devpath     : 设备路径缓冲区
	@ len_devpath : 设备路径缓冲区长度
	* Return      : 设备个数
	-*/
	K7CTRBOARDDRIVER_API int get_devices(GUID guid, char** devpath, size_t len_devpath);

	/*-
	Function name      : open_devices
	Description        : 打开设备通道
	Parameter
	@ device_hd	       : 通道句柄
	@ dwAccessPatter   : 访问模式
	@ device_base_path : 设备路径
	@ device_name      : 要打开的通道名
	* Return           : 成功返回1，失败返回0
	-*/
	K7CTRBOARDDRIVER_API int open_devices(HANDLE *device_hd, DWORD dwAccessPatter, char *device_base_path, const char*device_name);

	/*-
	Function name      : reset_devices
	Description        : 复位设备
	Parameter
	@ device_hd	       : 通道句柄
	* Return           : 成功返回0，失败返回<0
	-*/
	K7CTRBOARDDRIVER_API int reset_devices(HANDLE device_hd);

	/*-
	Function name      : ready_state
	Description        : 查看光纤和DDR初始状态
	Parameter
	@ device_hd	       : 通道句柄
	@ opstate		   : 光纤状态（初始化成功为100，失败为0）
	@ DDRstate		   : DDR状态（初始化成功为100，失败为0）
	* Return           : 成功返回字节数，失败返回<0
	-*/
	K7CTRBOARDDRIVER_API int ready_state(HANDLE device_hd, unsigned int* opstate, unsigned int* DDRstate);

	/*-
	Function name      : last_packetEn
	Description        : 最后一包数据使能
	Parameter
	@ device_hd	       : 通道句柄
	* Return           : 成功返回0，失败返回<0
	-*/
	K7CTRBOARDDRIVER_API int last_packetEn(HANDLE device_hd);

	/*-
	Function name      : last_packetSize
	Description        : 最后一包数据大小
	Parameter
	@ device_hd	       : 通道句柄
	* Return           : 成功返回最后一包实际字节数，失败返回<0
	-*/
	K7CTRBOARDDRIVER_API int last_packetSize(HANDLE device_hd);

	/*-
	Function name      : GXset_channel
	Description        : 接收光纤通道选择
	Parameter
	@ device_hd	       : 通道句柄
	@ ch			   : 通道号
	* Return           : 成功返回0，失败返回<0
	-*/
	K7CTRBOARDDRIVER_API int GXset_channel(HANDLE device_hd, int ch);
#ifdef  __cplusplus
}
#endif  /* end of __cplusplus */
