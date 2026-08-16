/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 14-June-2026
 *
 * libusbk.h - clean-room, ABI-compatible public header for the USBIP
 * drop-in `libusbK.dll`. It declares ONLY the libusbK API surface this wrapper
 * implements, with struct/enum/function-table layouts byte-for-byte matching
 * Travis Robinson's libusbK SDK header so a program compiled against the real
 * <libusbk.h> can load this DLL unchanged.
 *
 * This is NOT the vendored SDK header - it is an independent reimplementation of
 * the interface (the same approach the sibling libusb-1.0 wrapper takes with its
 * clean-room <libusb.h>). The fixed-size structs carry compile-time size asserts
 * so any layout drift is caught at build time.
 */
#ifndef LIBUSBK_H__
#define LIBUSBK_H__

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* SAL-style annotations (no-ops) so signatures read like the SDK's    */
/* ------------------------------------------------------------------ */
#ifndef _in
#define _in
#define _inopt
#define _out
#define _outopt
#define _ref
#define _refopt
#endif

/* Exported API marker (exports are driven by libusbK.def) and the calling
 * convention - always WINAPI (__stdcall on i686), exactly as the SDK. */
#ifndef KUSB_EXP
#define KUSB_EXP
#endif
#ifndef KUSB_API
#define KUSB_API WINAPI
#endif

/* Compile-time layout assert (one typedef per fixed-size struct). */
#define USBK_C_ASSERT(name, expr) typedef char _usbk_assert_##name[(expr) ? 1 : -1]

/* UsbK base function pointer, see LibK_GetProcAddress. */
typedef INT_PTR (FAR WINAPI* KPROC)(void);

/* User-defined handle context space, see LibK_GetContext. */
typedef INT_PTR KLIB_USER_CONTEXT;

/* ------------------------------------------------------------------ */
/* Opaque handle types                                                 */
/* ------------------------------------------------------------------ */
typedef void* KLIB_HANDLE;
typedef KLIB_HANDLE KUSB_HANDLE;        /* UsbK_Init    */
typedef KLIB_HANDLE KLST_HANDLE;        /* LstK_Init    */
typedef KLIB_HANDLE KHOT_HANDLE;        /* HotK_Init    */
typedef KLIB_HANDLE KOVL_HANDLE;        /* OvlK_Acquire */
typedef KLIB_HANDLE KOVL_POOL_HANDLE;   /* OvlK_Init    */
typedef KLIB_HANDLE KSTM_HANDLE;        /* StmK_Init    */
typedef KLIB_HANDLE KISOCH_HANDLE;      /* IsochK_Init  */

typedef enum _KLIB_HANDLE_TYPE
{
	KLIB_HANDLE_TYPE_HOTK,
	KLIB_HANDLE_TYPE_USBK,
	KLIB_HANDLE_TYPE_USBSHAREDK,
	KLIB_HANDLE_TYPE_LSTK,
	KLIB_HANDLE_TYPE_LSTINFOK,
	KLIB_HANDLE_TYPE_OVLK,
	KLIB_HANDLE_TYPE_OVLPOOLK,
	KLIB_HANDLE_TYPE_STMK,
	KLIB_HANDLE_TYPE_ISOCHK,
	KLIB_HANDLE_TYPE_COUNT
} KLIB_HANDLE_TYPE;

typedef INT KUSB_API KLIB_HANDLE_CLEANUP_CB(_in KLIB_HANDLE Handle, _in KLIB_HANDLE_TYPE HandleType, _in KLIB_USER_CONTEXT UserContext);

typedef struct _KLIB_VERSION
{
	INT Major;
	INT Minor;
	INT Micro;
	INT Nano;
} KLIB_VERSION;
typedef KLIB_VERSION* PKLIB_VERSION;

/* ------------------------------------------------------------------ */
/* Byte-packed wire/descriptor types                                   */
/* ------------------------------------------------------------------ */
#pragma pack(push, 1)

typedef enum _USBD_PIPE_TYPE
{
	UsbdPipeTypeControl,
	UsbdPipeTypeIsochronous,
	UsbdPipeTypeBulk,
	UsbdPipeTypeInterrupt
} USBD_PIPE_TYPE;

typedef struct _WINUSB_SETUP_PACKET
{
	UCHAR  RequestType;
	UCHAR  Request;
	USHORT Value;
	USHORT Index;
	USHORT Length;
} WINUSB_SETUP_PACKET;
typedef WINUSB_SETUP_PACKET* PWINUSB_SETUP_PACKET;

typedef enum _KISO_FLAG
{
	KISO_FLAG_NONE = 0,
	KISO_FLAG_SET_START_FRAME = 0x00000001
} KISO_FLAG;

typedef struct _KISO_PACKET
{
	UINT   Offset;
	USHORT Length;
	USHORT Status;
} KISO_PACKET;
typedef KISO_PACKET* PKISO_PACKET;

typedef struct _KISO_CONTEXT
{
	KISO_FLAG Flags;
	UINT      StartFrame;
	SHORT     ErrorCount;
	SHORT     NumberOfPackets;
	UINT      UrbHdrStatus;
	KISO_PACKET IsoPackets[0];
} KISO_CONTEXT;
typedef KISO_CONTEXT* PKISO_CONTEXT;

typedef struct _USB_DEVICE_DESCRIPTOR
{
	UCHAR  bLength;
	UCHAR  bDescriptorType;
	USHORT bcdUSB;
	UCHAR  bDeviceClass;
	UCHAR  bDeviceSubClass;
	UCHAR  bDeviceProtocol;
	UCHAR  bMaxPacketSize0;
	USHORT idVendor;
	USHORT idProduct;
	USHORT bcdDevice;
	UCHAR  iManufacturer;
	UCHAR  iProduct;
	UCHAR  iSerialNumber;
	UCHAR  bNumConfigurations;
} USB_DEVICE_DESCRIPTOR;
typedef USB_DEVICE_DESCRIPTOR* PUSB_DEVICE_DESCRIPTOR;

typedef struct _USB_CONFIGURATION_DESCRIPTOR
{
	UCHAR  bLength;
	UCHAR  bDescriptorType;
	USHORT wTotalLength;
	UCHAR  bNumInterfaces;
	UCHAR  bConfigurationValue;
	UCHAR  iConfiguration;
	UCHAR  bmAttributes;
	UCHAR  MaxPower;
} USB_CONFIGURATION_DESCRIPTOR;
typedef USB_CONFIGURATION_DESCRIPTOR* PUSB_CONFIGURATION_DESCRIPTOR;

typedef struct _USB_INTERFACE_DESCRIPTOR
{
	UCHAR bLength;
	UCHAR bDescriptorType;
	UCHAR bInterfaceNumber;
	UCHAR bAlternateSetting;
	UCHAR bNumEndpoints;
	UCHAR bInterfaceClass;
	UCHAR bInterfaceSubClass;
	UCHAR bInterfaceProtocol;
	UCHAR iInterface;
} USB_INTERFACE_DESCRIPTOR;
typedef USB_INTERFACE_DESCRIPTOR* PUSB_INTERFACE_DESCRIPTOR;

typedef struct _USB_ENDPOINT_DESCRIPTOR
{
	UCHAR  bLength;
	UCHAR  bDescriptorType;
	UCHAR  bEndpointAddress;
	UCHAR  bmAttributes;
	USHORT wMaxPacketSize;
	UCHAR  bInterval;
} USB_ENDPOINT_DESCRIPTOR;
typedef USB_ENDPOINT_DESCRIPTOR* PUSB_ENDPOINT_DESCRIPTOR;

typedef struct _USB_STRING_DESCRIPTOR
{
	UCHAR bLength;
	UCHAR bDescriptorType;
	WCHAR bString[1];
} USB_STRING_DESCRIPTOR;
typedef USB_STRING_DESCRIPTOR* PUSB_STRING_DESCRIPTOR;

typedef struct _USB_COMMON_DESCRIPTOR
{
	UCHAR bLength;
	UCHAR bDescriptorType;
} USB_COMMON_DESCRIPTOR;
typedef USB_COMMON_DESCRIPTOR* PUSB_COMMON_DESCRIPTOR;

typedef struct _USB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR
{
	UCHAR  bLength;
	UCHAR  bDescriptorType;
	UCHAR  bMaxBurst;
	union
	{
		UCHAR AsUchar;
		struct { UCHAR MaxStreams : 5; UCHAR Reserved1 : 3; } Bulk;
		struct { UCHAR Mult : 2; UCHAR Reserved2 : 5; UCHAR SspCompanion : 1; } Isochronous;
	} bmAttributes;
	USHORT wBytesPerInterval;
} USB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR, *PUSB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR;

#pragma pack(pop)

USBK_C_ASSERT(WINUSB_SETUP_PACKET, sizeof(WINUSB_SETUP_PACKET) == 8);
USBK_C_ASSERT(KISO_PACKET, sizeof(KISO_PACKET) == 8);

/* Not packed: WINUSB_PIPE_INFORMATION uses natural alignment in the SDK. */
typedef struct _WINUSB_PIPE_INFORMATION
{
	USBD_PIPE_TYPE PipeType;
	UCHAR  PipeId;
	USHORT MaximumPacketSize;
	UCHAR  Interval;
} WINUSB_PIPE_INFORMATION;
typedef WINUSB_PIPE_INFORMATION* PWINUSB_PIPE_INFORMATION;

typedef struct _WINUSB_PIPE_INFORMATION_EX
{
	USBD_PIPE_TYPE PipeType;
	UCHAR  PipeId;
	USHORT MaximumPacketSize;
	UCHAR  Interval;
	ULONG  MaximumBytesPerInterval;
} WINUSB_PIPE_INFORMATION_EX;
typedef WINUSB_PIPE_INFORMATION_EX* PWINUSB_PIPE_INFORMATION_EX;

typedef struct _KISOCH_PACKET_INFORMATION
{
	UINT PacketsPerFrame;
	UINT PollingPeriodMicroseconds;
	UINT BytesPerMillisecond;
} KISOCH_PACKET_INFORMATION;
typedef KISOCH_PACKET_INFORMATION* PKISOCH_PACKET_INFORMATION;

/* KUSB control setup packet - union view, identical size to WINUSB_SETUP_PACKET. */
typedef union _KUSB_SETUP_PACKET
{
	UCHAR  Bytes[8];
	USHORT Words[4];
	struct
	{
		struct { UCHAR Recipient : 2; UCHAR Reserved : 3; UCHAR Type : 2; UCHAR Dir : 1; } BmRequest;
		UCHAR  Request;
		USHORT Value;
		USHORT Index;
		USHORT Length;
	};
} KUSB_SETUP_PACKET;
USBK_C_ASSERT(KUSB_SETUP_PACKET, sizeof(KUSB_SETUP_PACKET) == 8);

/* ------------------------------------------------------------------ */
/* Device list (LstK)                                                  */
/* ------------------------------------------------------------------ */
#define KLST_STRING_MAX_LEN 256

typedef enum _KLST_SYNC_FLAG
{
	KLST_SYNC_FLAG_NONE          = 0L,
	KLST_SYNC_FLAG_UNCHANGED     = 0x0001,
	KLST_SYNC_FLAG_ADDED         = 0x0002,
	KLST_SYNC_FLAG_REMOVED       = 0x0004,
	KLST_SYNC_FLAG_CONNECT_CHANGE= 0x0008,
	KLST_SYNC_FLAG_MASK          = 0x000F
} KLST_SYNC_FLAG;

typedef struct _KLST_DEV_COMMON_INFO
{
	INT  Vid;
	INT  Pid;
	INT  MI;
	CHAR InstanceID[KLST_STRING_MAX_LEN];
} KLST_DEV_COMMON_INFO;
typedef KLST_DEV_COMMON_INFO* PKLST_DEV_COMMON_INFO;

typedef struct _KLST_DEVINFO
{
	KLST_DEV_COMMON_INFO Common;
	INT  DriverID;
	CHAR DeviceInterfaceGUID[KLST_STRING_MAX_LEN];
	CHAR DeviceID[KLST_STRING_MAX_LEN];
	CHAR ClassGUID[KLST_STRING_MAX_LEN];
	CHAR Mfg[KLST_STRING_MAX_LEN];
	CHAR DeviceDesc[KLST_STRING_MAX_LEN];
	CHAR Service[KLST_STRING_MAX_LEN];
	CHAR SymbolicLink[KLST_STRING_MAX_LEN];
	CHAR DevicePath[KLST_STRING_MAX_LEN];
	INT  LUsb0FilterIndex;
	BOOL Connected;
	KLST_SYNC_FLAG SyncFlags;
	INT  BusNumber;
	INT  DeviceAddress;
	CHAR SerialNumber[KLST_STRING_MAX_LEN];
} KLST_DEVINFO;
typedef KLST_DEVINFO* KLST_DEVINFO_HANDLE;

typedef enum _KLST_FLAG
{
	KLST_FLAG_NONE = 0L,
	KLST_FLAG_INCLUDE_RAWGUID = 0x0001,
	KLST_FLAG_INCLUDE_DISCONNECT = 0x0002
} KLST_FLAG;

typedef struct _KLST_PATTERN_MATCH
{
	CHAR  DeviceID[KLST_STRING_MAX_LEN];
	CHAR  DeviceInterfaceGUID[KLST_STRING_MAX_LEN];
	CHAR  ClassGUID[KLST_STRING_MAX_LEN];
	UCHAR z_F_i_x_e_d[1024 - KLST_STRING_MAX_LEN * 3];
} KLST_PATTERN_MATCH;
typedef KLST_PATTERN_MATCH* PKLST_PATTERN_MATCH;
USBK_C_ASSERT(KLST_PATTERN_MATCH, sizeof(KLST_PATTERN_MATCH) == 1024);

typedef BOOL KUSB_API KLST_ENUM_DEVINFO_CB(_in KLST_HANDLE DeviceList, _in KLST_DEVINFO_HANDLE DeviceInfo, _in PVOID Context);

/* ------------------------------------------------------------------ */
/* UsbK properties / driver ids / function ids                         */
/* ------------------------------------------------------------------ */
typedef enum _KUSB_PROPERTY
{
	KUSB_PROPERTY_DEVICE_FILE_HANDLE,
	KUSB_PROPERTY_COUNT
} KUSB_PROPERTY;

typedef enum _KUSB_DRVID
{
	KUSB_DRVID_LIBUSBK,
	KUSB_DRVID_LIBUSB0,
	KUSB_DRVID_WINUSB,
	KUSB_DRVID_LIBUSB0_FILTER,
	KUSB_DRVID_COUNT
} KUSB_DRVID;

typedef enum _KUSB_FNID
{
	KUSB_FNID_Init,
	KUSB_FNID_Free,
	KUSB_FNID_ClaimInterface,
	KUSB_FNID_ReleaseInterface,
	KUSB_FNID_SetAltInterface,
	KUSB_FNID_GetAltInterface,
	KUSB_FNID_GetDescriptor,
	KUSB_FNID_ControlTransfer,
	KUSB_FNID_SetPowerPolicy,
	KUSB_FNID_GetPowerPolicy,
	KUSB_FNID_SetConfiguration,
	KUSB_FNID_GetConfiguration,
	KUSB_FNID_ResetDevice,
	KUSB_FNID_Initialize,
	KUSB_FNID_SelectInterface,
	KUSB_FNID_GetAssociatedInterface,
	KUSB_FNID_Clone,
	KUSB_FNID_QueryInterfaceSettings,
	KUSB_FNID_QueryDeviceInformation,
	KUSB_FNID_SetCurrentAlternateSetting,
	KUSB_FNID_GetCurrentAlternateSetting,
	KUSB_FNID_QueryPipe,
	KUSB_FNID_SetPipePolicy,
	KUSB_FNID_GetPipePolicy,
	KUSB_FNID_ReadPipe,
	KUSB_FNID_WritePipe,
	KUSB_FNID_ResetPipe,
	KUSB_FNID_AbortPipe,
	KUSB_FNID_FlushPipe,
	KUSB_FNID_IsoReadPipe,
	KUSB_FNID_IsoWritePipe,
	KUSB_FNID_GetCurrentFrameNumber,
	KUSB_FNID_GetOverlappedResult,
	KUSB_FNID_GetProperty,
	KUSB_FNID_IsochReadPipe,
	KUSB_FNID_IsochWritePipe,
	KUSB_FNID_QueryPipeEx,
	KUSB_FNID_GetSuperSpeedPipeCompanionDescriptor,
	KUSB_FNID_COUNT
} KUSB_FNID;

/* ------------------------------------------------------------------ */
/* UsbK_* function-pointer typedefs (members of KUSB_DRIVER_API)       */
/* ------------------------------------------------------------------ */
typedef BOOL KUSB_API KUSB_Init(_out KUSB_HANDLE* InterfaceHandle, _in KLST_DEVINFO_HANDLE DevInfo);
typedef BOOL KUSB_API KUSB_Free(_in KUSB_HANDLE InterfaceHandle);
typedef BOOL KUSB_API KUSB_ClaimInterface(_in KUSB_HANDLE InterfaceHandle, _in UCHAR NumberOrIndex, _in BOOL IsIndex);
typedef BOOL KUSB_API KUSB_ReleaseInterface(_in KUSB_HANDLE InterfaceHandle, _in UCHAR NumberOrIndex, _in BOOL IsIndex);
typedef BOOL KUSB_API KUSB_SetAltInterface(_in KUSB_HANDLE InterfaceHandle, _in UCHAR NumberOrIndex, _in BOOL IsIndex, _in UCHAR AltSettingNumber);
typedef BOOL KUSB_API KUSB_GetAltInterface(_in KUSB_HANDLE InterfaceHandle, _in UCHAR NumberOrIndex, _in BOOL IsIndex, _out PUCHAR AltSettingNumber);
typedef BOOL KUSB_API KUSB_GetDescriptor(_in KUSB_HANDLE InterfaceHandle, _in UCHAR DescriptorType, _in UCHAR Index, _in USHORT LanguageID, _out PUCHAR Buffer, _in UINT BufferLength, _outopt PUINT LengthTransferred);
typedef BOOL KUSB_API KUSB_ControlTransfer(_in KUSB_HANDLE InterfaceHandle, _in WINUSB_SETUP_PACKET SetupPacket, _refopt PUCHAR Buffer, _in UINT BufferLength, _outopt PUINT LengthTransferred, _inopt LPOVERLAPPED Overlapped);
typedef BOOL KUSB_API KUSB_SetPowerPolicy(_in KUSB_HANDLE InterfaceHandle, _in UINT PolicyType, _in UINT ValueLength, _in PVOID Value);
typedef BOOL KUSB_API KUSB_GetPowerPolicy(_in KUSB_HANDLE InterfaceHandle, _in UINT PolicyType, _ref PUINT ValueLength, _out PVOID Value);
typedef BOOL KUSB_API KUSB_SetConfiguration(_in KUSB_HANDLE InterfaceHandle, _in UCHAR ConfigurationNumber);
typedef BOOL KUSB_API KUSB_GetConfiguration(_in KUSB_HANDLE InterfaceHandle, _out PUCHAR ConfigurationNumber);
typedef BOOL KUSB_API KUSB_ResetDevice(_in KUSB_HANDLE InterfaceHandle);
typedef BOOL KUSB_API KUSB_Initialize(_in HANDLE DeviceHandle, _out KUSB_HANDLE* InterfaceHandle);
typedef BOOL KUSB_API KUSB_SelectInterface(_in KUSB_HANDLE InterfaceHandle, _in UCHAR NumberOrIndex, _in BOOL IsIndex);
typedef BOOL KUSB_API KUSB_GetAssociatedInterface(_in KUSB_HANDLE InterfaceHandle, _in UCHAR AssociatedInterfaceIndex, _out KUSB_HANDLE* AssociatedInterfaceHandle);
typedef BOOL KUSB_API KUSB_Clone(_in KUSB_HANDLE InterfaceHandle, _out KUSB_HANDLE* DstInterfaceHandle);
typedef BOOL KUSB_API KUSB_QueryInterfaceSettings(_in KUSB_HANDLE InterfaceHandle, _in UCHAR AltSettingIndex, _out PUSB_INTERFACE_DESCRIPTOR UsbAltInterfaceDescriptor);
typedef BOOL KUSB_API KUSB_QueryDeviceInformation(_in KUSB_HANDLE InterfaceHandle, _in UINT InformationType, _ref PUINT BufferLength, _ref PUCHAR Buffer);
typedef BOOL KUSB_API KUSB_SetCurrentAlternateSetting(_in KUSB_HANDLE InterfaceHandle, _in UCHAR AltSettingNumber);
typedef BOOL KUSB_API KUSB_GetCurrentAlternateSetting(_in KUSB_HANDLE InterfaceHandle, _out PUCHAR AltSettingNumber);
typedef BOOL KUSB_API KUSB_QueryPipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR AltSettingNumber, _in UCHAR PipeIndex, _out PWINUSB_PIPE_INFORMATION PipeInformation);
typedef BOOL KUSB_API KUSB_SetPipePolicy(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID, _in UINT PolicyType, _in UINT ValueLength, _in PVOID Value);
typedef BOOL KUSB_API KUSB_GetPipePolicy(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID, _in UINT PolicyType, _ref PUINT ValueLength, _out PVOID Value);
typedef BOOL KUSB_API KUSB_ReadPipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID, _out PUCHAR Buffer, _in UINT BufferLength, _outopt PUINT LengthTransferred, _inopt LPOVERLAPPED Overlapped);
typedef BOOL KUSB_API KUSB_WritePipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID, _in PUCHAR Buffer, _in UINT BufferLength, _outopt PUINT LengthTransferred, _inopt LPOVERLAPPED Overlapped);
typedef BOOL KUSB_API KUSB_ResetPipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID);
typedef BOOL KUSB_API KUSB_AbortPipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID);
typedef BOOL KUSB_API KUSB_FlushPipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID);
typedef BOOL KUSB_API KUSB_IsoReadPipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID, _out PUCHAR Buffer, _in UINT BufferLength, _in LPOVERLAPPED Overlapped, _refopt PKISO_CONTEXT IsoContext);
typedef BOOL KUSB_API KUSB_IsoWritePipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID, _in PUCHAR Buffer, _in UINT BufferLength, _in LPOVERLAPPED Overlapped, _refopt PKISO_CONTEXT IsoContext);
typedef BOOL KUSB_API KUSB_GetCurrentFrameNumber(_in KUSB_HANDLE InterfaceHandle, _out PUINT FrameNumber);
typedef BOOL KUSB_API KUSB_GetOverlappedResult(_in KUSB_HANDLE InterfaceHandle, _in LPOVERLAPPED Overlapped, _out PUINT lpNumberOfBytesTransferred, _in BOOL bWait);
typedef BOOL KUSB_API KUSB_GetProperty(_in KUSB_HANDLE InterfaceHandle, _in KUSB_PROPERTY PropertyType, _ref PUINT PropertySize, _out PVOID Value);
typedef BOOL KUSB_API KUSB_IsochReadPipe(_in KISOCH_HANDLE IsochHandle, _inopt UINT DataLength, _refopt PUINT FrameNumber, _inopt UINT NumberOfPackets, _in LPOVERLAPPED Overlapped);
typedef BOOL KUSB_API KUSB_IsochWritePipe(_in KISOCH_HANDLE IsochHandle, _inopt UINT DataLength, _refopt PUINT FrameNumber, _inopt UINT NumberOfPackets, _in LPOVERLAPPED Overlapped);
typedef BOOL KUSB_API KUSB_QueryPipeEx(_in KUSB_HANDLE InterfaceHandle, _in UCHAR AltSettingNumber, _in UCHAR PipeIndex, _out PWINUSB_PIPE_INFORMATION_EX PipeInformationEx);
typedef BOOL KUSB_API KUSB_GetSuperSpeedPipeCompanionDescriptor(_in KUSB_HANDLE InterfaceHandle, _in UCHAR AltSettingNumber, _in UCHAR PipeIndex, _out PUSB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR PipeCompanionDescriptor);

/* ------------------------------------------------------------------ */
/* KUSB_DRIVER_API - fixed 512-byte function table                     */
/* ------------------------------------------------------------------ */
typedef struct _KUSB_DRIVER_API_INFO
{
	INT DriverID;
	INT FunctionCount;
} KUSB_DRIVER_API_INFO;

typedef struct _KUSB_DRIVER_API
{
	KUSB_DRIVER_API_INFO Info;
	KUSB_Init*                   Init;
	KUSB_Free*                   Free;
	KUSB_ClaimInterface*         ClaimInterface;
	KUSB_ReleaseInterface*       ReleaseInterface;
	KUSB_SetAltInterface*        SetAltInterface;
	KUSB_GetAltInterface*        GetAltInterface;
	KUSB_GetDescriptor*          GetDescriptor;
	KUSB_ControlTransfer*        ControlTransfer;
	KUSB_SetPowerPolicy*         SetPowerPolicy;
	KUSB_GetPowerPolicy*         GetPowerPolicy;
	KUSB_SetConfiguration*       SetConfiguration;
	KUSB_GetConfiguration*       GetConfiguration;
	KUSB_ResetDevice*            ResetDevice;
	KUSB_Initialize*             Initialize;
	KUSB_SelectInterface*        SelectInterface;
	KUSB_GetAssociatedInterface* GetAssociatedInterface;
	KUSB_Clone*                  Clone;
	KUSB_QueryInterfaceSettings* QueryInterfaceSettings;
	KUSB_QueryDeviceInformation* QueryDeviceInformation;
	KUSB_SetCurrentAlternateSetting* SetCurrentAlternateSetting;
	KUSB_GetCurrentAlternateSetting* GetCurrentAlternateSetting;
	KUSB_QueryPipe*              QueryPipe;
	KUSB_SetPipePolicy*          SetPipePolicy;
	KUSB_GetPipePolicy*          GetPipePolicy;
	KUSB_ReadPipe*               ReadPipe;
	KUSB_WritePipe*              WritePipe;
	KUSB_ResetPipe*              ResetPipe;
	KUSB_AbortPipe*              AbortPipe;
	KUSB_FlushPipe*              FlushPipe;
	KUSB_IsoReadPipe*            IsoReadPipe;
	KUSB_IsoWritePipe*           IsoWritePipe;
	KUSB_GetCurrentFrameNumber*  GetCurrentFrameNumber;
	KUSB_GetOverlappedResult*    GetOverlappedResult;
	KUSB_GetProperty*            GetProperty;
	KUSB_IsochReadPipe*          IsochReadPipe;
	KUSB_IsochWritePipe*         IsochWritePipe;
	KUSB_QueryPipeEx*            QueryPipeEx;
	KUSB_GetSuperSpeedPipeCompanionDescriptor* GetSuperSpeedPipeCompanionDescriptor;

	UCHAR z_F_i_x_e_d[512 - sizeof(KUSB_DRIVER_API_INFO) - sizeof(UINT_PTR) * KUSB_FNID_COUNT - (KUSB_FNID_COUNT & (sizeof(UINT_PTR) - 1) ? (KUSB_FNID_COUNT & (~(sizeof(UINT_PTR) - 1))) + sizeof(UINT_PTR) : KUSB_FNID_COUNT)];
	UCHAR z_FuncSupported[(KUSB_FNID_COUNT & (sizeof(UINT_PTR) - 1) ? (KUSB_FNID_COUNT & (~(sizeof(UINT_PTR) - 1))) + sizeof(UINT_PTR) : KUSB_FNID_COUNT)];
} KUSB_DRIVER_API;
typedef KUSB_DRIVER_API* PKUSB_DRIVER_API;
USBK_C_ASSERT(KUSB_DRIVER_API, sizeof(KUSB_DRIVER_API) == 512);

/* ------------------------------------------------------------------ */
/* Hot plug (HotK)                                                     */
/* ------------------------------------------------------------------ */
typedef enum _KHOT_FLAG
{
	KHOT_FLAG_NONE,
	KHOT_FLAG_PLUG_ALL_ON_INIT   = 0x0001,
	KHOT_FLAG_PASS_DUPE_INSTANCE = 0x0002,
	KHOT_FLAG_POST_USER_MESSAGE  = 0x0004
} KHOT_FLAG;

typedef VOID KUSB_API KHOT_PLUG_CB(_in KHOT_HANDLE HotHandle, _in KLST_DEVINFO_HANDLE DeviceInfo, _in KLST_SYNC_FLAG PlugType);
typedef VOID KUSB_API KHOT_POWER_BROADCAST_CB(_in KHOT_HANDLE HotHandle, _in KLST_DEVINFO_HANDLE DeviceInfo, _in UINT PbtEvent);

typedef struct _KHOT_PARAMS
{
	HWND UserHwnd;
	UINT UserMessage;
	KHOT_FLAG Flags;
	KLST_PATTERN_MATCH PatternMatch;
	KHOT_PLUG_CB* OnHotPlug;
	KHOT_POWER_BROADCAST_CB* OnPowerBroadcast;
	UCHAR z_F_i_x_e_d[2048 - sizeof(KLST_PATTERN_MATCH) - sizeof(UINT_PTR) * 3 - sizeof(UINT) * 2];
} KHOT_PARAMS;
typedef KHOT_PARAMS* PKHOT_PARAMS;
USBK_C_ASSERT(KHOT_PARAMS, sizeof(KHOT_PARAMS) == 2048);

/* ------------------------------------------------------------------ */
/* Overlapped pool (OvlK)                                              */
/* ------------------------------------------------------------------ */
typedef enum _KOVL_WAIT_FLAG
{
	KOVL_WAIT_FLAG_NONE                    = 0L,
	KOVL_WAIT_FLAG_RELEASE_ON_SUCCESS      = 0x0001,
	KOVL_WAIT_FLAG_RELEASE_ON_FAIL         = 0x0002,
	KOVL_WAIT_FLAG_RELEASE_ON_SUCCESS_FAIL = 0x0003,
	KOVL_WAIT_FLAG_CANCEL_ON_TIMEOUT       = 0x0004,
	KOVL_WAIT_FLAG_RELEASE_ON_TIMEOUT      = 0x000C,
	KOVL_WAIT_FLAG_RELEASE_ALWAYS          = 0x000F,
	KOVL_WAIT_FLAG_ALERTABLE               = 0x0010
} KOVL_WAIT_FLAG;

typedef enum _KOVL_POOL_FLAG
{
	KOVL_POOL_FLAG_NONE = 0L
} KOVL_POOL_FLAG;

/* ------------------------------------------------------------------ */
/* Pipe streams (StmK)                                                 */
/* ------------------------------------------------------------------ */
typedef enum _KSTM_FLAG
{
	KSTM_FLAG_NONE             = 0L,
	KSTM_FLAG_NO_PARTIAL_XFERS = 0x00100000,
	KSTM_FLAG_USE_TIMEOUT      = 0x80000000,
	KSTM_FLAG_TIMEOUT_MASK     = 0x0001FFFF
} KSTM_FLAG;

typedef enum _KSTM_COMPLETE_RESULT
{
	KSTM_COMPLETE_RESULT_VALID = 0L,
	KSTM_COMPLETE_RESULT_INVALID
} KSTM_COMPLETE_RESULT;

typedef struct _KSTM_XFER_CONTEXT
{
	PUCHAR Buffer;
	INT    BufferSize;
	INT    TransferLength;
	PVOID  UserState;
} KSTM_XFER_CONTEXT;
typedef KSTM_XFER_CONTEXT* PKSTM_XFER_CONTEXT;

typedef struct _KSTM_INFO
{
	KUSB_HANDLE UsbHandle;
	UCHAR PipeID;
	INT   MaxPendingTransfers;
	INT   MaxTransferSize;
	INT   MaxPendingIO;
	USB_ENDPOINT_DESCRIPTOR EndpointDescriptor;
	KUSB_DRIVER_API DriverAPI;
	HANDLE DeviceHandle;
	KSTM_HANDLE StreamHandle;
	PVOID  UserState;
} KSTM_INFO;
typedef KSTM_INFO* PKSTM_INFO;

typedef INT KUSB_API KSTM_ERROR_CB(_in PKSTM_INFO StreamInfo, _in PKSTM_XFER_CONTEXT XferContext, _in INT XferContextIndex, _in INT ErrorCode);
typedef INT KUSB_API KSTM_SUBMIT_CB(_in PKSTM_INFO StreamInfo, _in PKSTM_XFER_CONTEXT XferContext, _in INT XferContextIndex, _in LPOVERLAPPED Overlapped);
typedef INT KUSB_API KSTM_STARTED_CB(_in PKSTM_INFO StreamInfo, _in PKSTM_XFER_CONTEXT XferContext, _in INT XferContextIndex);
typedef INT KUSB_API KSTM_STOPPED_CB(_in PKSTM_INFO StreamInfo, _in PKSTM_XFER_CONTEXT XferContext, _in INT XferContextIndex);
typedef INT KUSB_API KSTM_COMPLETE_CB(_in PKSTM_INFO StreamInfo, _in PKSTM_XFER_CONTEXT XferContext, _in INT XferContextIndex, _in INT ErrorCode);
typedef KSTM_COMPLETE_RESULT KUSB_API KSTM_BEFORE_COMPLETE_CB(_in PKSTM_INFO StreamInfo, _in PKSTM_XFER_CONTEXT XferContext, _in INT XferContextIndex, _in PINT ErrorCode);

typedef struct _KSTM_CALLBACK
{
	KSTM_ERROR_CB*           Error;
	KSTM_SUBMIT_CB*          Submit;
	KSTM_COMPLETE_CB*        Complete;
	KSTM_STARTED_CB*         Started;
	KSTM_STOPPED_CB*         Stopped;
	KSTM_BEFORE_COMPLETE_CB* BeforeComplete;
	UCHAR z_F_i_x_e_d[64 - sizeof(UINT_PTR) * 6];
} KSTM_CALLBACK;
typedef KSTM_CALLBACK* PKSTM_CALLBACK;
USBK_C_ASSERT(KSTM_CALLBACK, sizeof(KSTM_CALLBACK) == 64);

/* ------------------------------------------------------------------ */
/* Isochronous enum callbacks                                          */
/* ------------------------------------------------------------------ */
typedef BOOL KUSB_API KISO_ENUM_PACKETS_CB(_in UINT PacketIndex, _in PKISO_PACKET IsoPacket, _in PVOID UserState);
typedef BOOL KUSB_API KISOCH_ENUM_PACKETS_CB(_in UINT PacketIndex, _ref PUINT Offset, _ref PUINT Length, _ref PUINT Status, _in PVOID UserState);

/* ------------------------------------------------------------------ */
/* WinUSB pipe/power policy type ids (subset; values match WinUSB)     */
/* ------------------------------------------------------------------ */
#define SHORT_PACKET_TERMINATE  0x01
#define AUTO_CLEAR_STALL        0x02
#define PIPE_TRANSFER_TIMEOUT   0x03
#define IGNORE_SHORT_PACKETS    0x04
#define ALLOW_PARTIAL_READS     0x05
#define AUTO_FLUSH              0x06
#define RAW_IO                  0x07
#define MAXIMUM_TRANSFER_SIZE   0x08
#define RESET_PIPE_ON_RESUME    0x09
#define AUTO_SUSPEND            0x81
#define SUSPEND_DELAY           0x83
/* QueryDeviceInformation: information type + device speed values */
#define DEVICE_SPEED            0x01
#define LowSpeed                0x01
#define FullSpeed               0x02
#define HighSpeed               0x03

/* ------------------------------------------------------------------ */
/* Exported functions                                                  */
/* ------------------------------------------------------------------ */
/* LibK */
KUSB_EXP VOID KUSB_API LibK_GetVersion(_out PKLIB_VERSION Version);
KUSB_EXP KLIB_USER_CONTEXT KUSB_API LibK_GetContext(_in KLIB_HANDLE Handle, _in KLIB_HANDLE_TYPE HandleType);
KUSB_EXP BOOL KUSB_API LibK_SetContext(_in KLIB_HANDLE Handle, _in KLIB_HANDLE_TYPE HandleType, _in KLIB_USER_CONTEXT ContextValue);
KUSB_EXP BOOL KUSB_API LibK_SetCleanupCallback(_in KLIB_HANDLE Handle, _in KLIB_HANDLE_TYPE HandleType, _in KLIB_HANDLE_CLEANUP_CB* CleanupCB);
KUSB_EXP BOOL KUSB_API LibK_LoadDriverAPI(_out PKUSB_DRIVER_API DriverAPI, _in INT DriverID);
KUSB_EXP BOOL KUSB_API LibK_IsFunctionSupported(_in PKUSB_DRIVER_API DriverAPI, _in UINT FunctionID);
KUSB_EXP BOOL KUSB_API LibK_CopyDriverAPI(_out PKUSB_DRIVER_API DriverAPI, _in KUSB_HANDLE UsbHandle);
KUSB_EXP BOOL KUSB_API LibK_GetProcAddress(_out KPROC* ProcAddress, _in INT DriverID, _in INT FunctionID);
KUSB_EXP BOOL KUSB_API LibK_SetDefaultContext(_in KLIB_HANDLE_TYPE HandleType, _in KLIB_USER_CONTEXT ContextValue);
KUSB_EXP KLIB_USER_CONTEXT KUSB_API LibK_GetDefaultContext(_in KLIB_HANDLE_TYPE HandleType);
KUSB_EXP BOOL KUSB_API LibK_Context_Init(_inopt HANDLE Heap, _in PVOID Reserved);
KUSB_EXP VOID KUSB_API LibK_Context_Free(VOID);

/* LstK */
KUSB_EXP BOOL KUSB_API LstK_Init(_out KLST_HANDLE* DeviceList, _in KLST_FLAG Flags);
KUSB_EXP BOOL KUSB_API LstK_InitEx(_out KLST_HANDLE* DeviceList, _in KLST_FLAG Flags, _in PKLST_PATTERN_MATCH PatternMatch);
KUSB_EXP BOOL KUSB_API LstK_Free(_in KLST_HANDLE DeviceList);
KUSB_EXP BOOL KUSB_API LstK_Enumerate(_in KLST_HANDLE DeviceList, _in KLST_ENUM_DEVINFO_CB* EnumDevListCB, _inopt PVOID Context);
KUSB_EXP BOOL KUSB_API LstK_Current(_in KLST_HANDLE DeviceList, _out KLST_DEVINFO_HANDLE* DeviceInfo);
KUSB_EXP BOOL KUSB_API LstK_MoveNext(_in KLST_HANDLE DeviceList, _outopt KLST_DEVINFO_HANDLE* DeviceInfo);
KUSB_EXP VOID KUSB_API LstK_MoveReset(_in KLST_HANDLE DeviceList);
KUSB_EXP BOOL KUSB_API LstK_FindByVidPid(_in KLST_HANDLE DeviceList, _in INT Vid, _in INT Pid, _out KLST_DEVINFO_HANDLE* DeviceInfo);
KUSB_EXP BOOL KUSB_API LstK_Count(_in KLST_HANDLE DeviceList, _ref PUINT Count);
KUSB_EXP BOOL KUSB_API LstK_Sync(_in KLST_HANDLE MasterList, _inopt KLST_HANDLE SlaveList, _inopt KLST_SYNC_FLAG SyncFlags, _inopt PKLST_PATTERN_MATCH SlaveListPatternMatch, _inopt HANDLE Heap);
KUSB_EXP BOOL KUSB_API LstK_Clone(_in KLST_HANDLE SrcList, _out KLST_HANDLE* DstList);
KUSB_EXP BOOL KUSB_API LstK_CloneInfo(_in KLST_DEVINFO_HANDLE SrcInfo, _out KLST_DEVINFO_HANDLE* DstInfo);
KUSB_EXP BOOL KUSB_API LstK_DetachInfo(_in KLST_HANDLE DeviceList, _in KLST_DEVINFO_HANDLE DeviceInfo);
KUSB_EXP BOOL KUSB_API LstK_AttachInfo(_in KLST_HANDLE DeviceList, _in KLST_DEVINFO_HANDLE DeviceInfo);
KUSB_EXP BOOL KUSB_API LstK_FreeInfo(_in KLST_DEVINFO_HANDLE DeviceInfo);

/* UsbK */
KUSB_EXP BOOL KUSB_API UsbK_Init(_out KUSB_HANDLE* InterfaceHandle, _in KLST_DEVINFO_HANDLE DevInfo);
KUSB_EXP BOOL KUSB_API UsbK_Free(_in KUSB_HANDLE InterfaceHandle);
KUSB_EXP BOOL KUSB_API UsbK_ClaimInterface(_in KUSB_HANDLE InterfaceHandle, _in UCHAR NumberOrIndex, _in BOOL IsIndex);
KUSB_EXP BOOL KUSB_API UsbK_ReleaseInterface(_in KUSB_HANDLE InterfaceHandle, _in UCHAR NumberOrIndex, _in BOOL IsIndex);
KUSB_EXP BOOL KUSB_API UsbK_SetAltInterface(_in KUSB_HANDLE InterfaceHandle, _in UCHAR NumberOrIndex, _in BOOL IsIndex, _in UCHAR AltSettingNumber);
KUSB_EXP BOOL KUSB_API UsbK_GetAltInterface(_in KUSB_HANDLE InterfaceHandle, _in UCHAR NumberOrIndex, _in BOOL IsIndex, _out PUCHAR AltSettingNumber);
KUSB_EXP BOOL KUSB_API UsbK_GetDescriptor(_in KUSB_HANDLE InterfaceHandle, _in UCHAR DescriptorType, _in UCHAR Index, _in USHORT LanguageID, _out PUCHAR Buffer, _in UINT BufferLength, _outopt PUINT LengthTransferred);
KUSB_EXP BOOL KUSB_API UsbK_ControlTransfer(_in KUSB_HANDLE InterfaceHandle, _in WINUSB_SETUP_PACKET SetupPacket, _refopt PUCHAR Buffer, _in UINT BufferLength, _outopt PUINT LengthTransferred, _inopt LPOVERLAPPED Overlapped);
KUSB_EXP BOOL KUSB_API UsbK_SetPowerPolicy(_in KUSB_HANDLE InterfaceHandle, _in UINT PolicyType, _in UINT ValueLength, _in PVOID Value);
KUSB_EXP BOOL KUSB_API UsbK_GetPowerPolicy(_in KUSB_HANDLE InterfaceHandle, _in UINT PolicyType, _ref PUINT ValueLength, _out PVOID Value);
KUSB_EXP BOOL KUSB_API UsbK_SetConfiguration(_in KUSB_HANDLE InterfaceHandle, _in UCHAR ConfigurationNumber);
KUSB_EXP BOOL KUSB_API UsbK_GetConfiguration(_in KUSB_HANDLE InterfaceHandle, _out PUCHAR ConfigurationNumber);
KUSB_EXP BOOL KUSB_API UsbK_ResetDevice(_in KUSB_HANDLE InterfaceHandle);
KUSB_EXP BOOL KUSB_API UsbK_Initialize(_in HANDLE DeviceHandle, _out KUSB_HANDLE* InterfaceHandle);
KUSB_EXP BOOL KUSB_API UsbK_SelectInterface(_in KUSB_HANDLE InterfaceHandle, _in UCHAR NumberOrIndex, _in BOOL IsIndex);
KUSB_EXP BOOL KUSB_API UsbK_GetAssociatedInterface(_in KUSB_HANDLE InterfaceHandle, _in UCHAR AssociatedInterfaceIndex, _out KUSB_HANDLE* AssociatedInterfaceHandle);
KUSB_EXP BOOL KUSB_API UsbK_Clone(_in KUSB_HANDLE InterfaceHandle, _out KUSB_HANDLE* DstInterfaceHandle);
KUSB_EXP BOOL KUSB_API UsbK_QueryInterfaceSettings(_in KUSB_HANDLE InterfaceHandle, _in UCHAR AltSettingIndex, _out PUSB_INTERFACE_DESCRIPTOR UsbAltInterfaceDescriptor);
KUSB_EXP BOOL KUSB_API UsbK_QueryDeviceInformation(_in KUSB_HANDLE InterfaceHandle, _in UINT InformationType, _ref PUINT BufferLength, _ref PUCHAR Buffer);
KUSB_EXP BOOL KUSB_API UsbK_SetCurrentAlternateSetting(_in KUSB_HANDLE InterfaceHandle, _in UCHAR AltSettingNumber);
KUSB_EXP BOOL KUSB_API UsbK_GetCurrentAlternateSetting(_in KUSB_HANDLE InterfaceHandle, _out PUCHAR AltSettingNumber);
KUSB_EXP BOOL KUSB_API UsbK_QueryPipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR AltSettingNumber, _in UCHAR PipeIndex, _out PWINUSB_PIPE_INFORMATION PipeInformation);
KUSB_EXP BOOL KUSB_API UsbK_QueryPipeEx(_in KUSB_HANDLE InterfaceHandle, _in UCHAR AltSettingNumber, _in UCHAR PipeIndex, _out PWINUSB_PIPE_INFORMATION_EX PipeInformationEx);
KUSB_EXP BOOL KUSB_API UsbK_GetSuperSpeedPipeCompanionDescriptor(_in KUSB_HANDLE InterfaceHandle, _in UCHAR AltSettingNumber, _in UCHAR PipeIndex, _out PUSB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR PipeCompanionDescriptor);
KUSB_EXP BOOL KUSB_API UsbK_SetPipePolicy(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID, _in UINT PolicyType, _in UINT ValueLength, _in PVOID Value);
KUSB_EXP BOOL KUSB_API UsbK_GetPipePolicy(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID, _in UINT PolicyType, _ref PUINT ValueLength, _out PVOID Value);
KUSB_EXP BOOL KUSB_API UsbK_ReadPipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID, _out PUCHAR Buffer, _in UINT BufferLength, _outopt PUINT LengthTransferred, _inopt LPOVERLAPPED Overlapped);
KUSB_EXP BOOL KUSB_API UsbK_WritePipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID, _in PUCHAR Buffer, _in UINT BufferLength, _outopt PUINT LengthTransferred, _inopt LPOVERLAPPED Overlapped);
KUSB_EXP BOOL KUSB_API UsbK_ResetPipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID);
KUSB_EXP BOOL KUSB_API UsbK_AbortPipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID);
KUSB_EXP BOOL KUSB_API UsbK_FlushPipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID);
KUSB_EXP BOOL KUSB_API UsbK_IsoReadPipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID, _out PUCHAR Buffer, _in UINT BufferLength, _in LPOVERLAPPED Overlapped, _refopt PKISO_CONTEXT IsoContext);
KUSB_EXP BOOL KUSB_API UsbK_IsoWritePipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID, _in PUCHAR Buffer, _in UINT BufferLength, _in LPOVERLAPPED Overlapped, _refopt PKISO_CONTEXT IsoContext);
KUSB_EXP BOOL KUSB_API UsbK_GetCurrentFrameNumber(_in KUSB_HANDLE InterfaceHandle, _out PUINT FrameNumber);
KUSB_EXP BOOL KUSB_API UsbK_GetOverlappedResult(_in KUSB_HANDLE InterfaceHandle, _in LPOVERLAPPED Overlapped, _out PUINT lpNumberOfBytesTransferred, _in BOOL bWait);
KUSB_EXP BOOL KUSB_API UsbK_GetProperty(_in KUSB_HANDLE InterfaceHandle, _in KUSB_PROPERTY PropertyType, _ref PUINT PropertySize, _out PVOID Value);
KUSB_EXP BOOL KUSB_API UsbK_IsochReadPipe(_in KISOCH_HANDLE IsochHandle, _inopt UINT DataLength, _refopt PUINT FrameNumber, _inopt UINT NumberOfPackets, _in LPOVERLAPPED Overlapped);
KUSB_EXP BOOL KUSB_API UsbK_IsochWritePipe(_in KISOCH_HANDLE IsochHandle, _inopt UINT DataLength, _refopt PUINT FrameNumber, _inopt UINT NumberOfPackets, _in LPOVERLAPPED Overlapped);

/* WinUsb_* - the WinUSB-named face of the same API, exported as plain forwards to
 * their UsbK_* twins, so a program written against winusb.h links unchanged.
 * A WINUSB_INTERFACE_HANDLE is a KUSB_HANDLE. */
KUSB_EXP BOOL KUSB_API WinUsb_Initialize(_in HANDLE DeviceHandle, _out KUSB_HANDLE* InterfaceHandle);
KUSB_EXP BOOL KUSB_API WinUsb_Free(_in KUSB_HANDLE InterfaceHandle);
KUSB_EXP BOOL KUSB_API WinUsb_GetAssociatedInterface(_in KUSB_HANDLE InterfaceHandle, _in UCHAR AssociatedInterfaceIndex, _out KUSB_HANDLE* AssociatedInterfaceHandle);
KUSB_EXP BOOL KUSB_API WinUsb_GetDescriptor(_in KUSB_HANDLE InterfaceHandle, _in UCHAR DescriptorType, _in UCHAR Index, _in USHORT LanguageID, _out PUCHAR Buffer, _in UINT BufferLength, _outopt PUINT LengthTransferred);
KUSB_EXP BOOL KUSB_API WinUsb_QueryInterfaceSettings(_in KUSB_HANDLE InterfaceHandle, _in UCHAR AltSettingIndex, _out PUSB_INTERFACE_DESCRIPTOR UsbAltInterfaceDescriptor);
KUSB_EXP BOOL KUSB_API WinUsb_QueryDeviceInformation(_in KUSB_HANDLE InterfaceHandle, _in UINT InformationType, _ref PUINT BufferLength, _ref PUCHAR Buffer);
KUSB_EXP BOOL KUSB_API WinUsb_SetCurrentAlternateSetting(_in KUSB_HANDLE InterfaceHandle, _in UCHAR AltSettingNumber);
KUSB_EXP BOOL KUSB_API WinUsb_GetCurrentAlternateSetting(_in KUSB_HANDLE InterfaceHandle, _out PUCHAR AltSettingNumber);
KUSB_EXP BOOL KUSB_API WinUsb_QueryPipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR AltSettingNumber, _in UCHAR PipeIndex, _out PWINUSB_PIPE_INFORMATION PipeInformation);
KUSB_EXP BOOL KUSB_API WinUsb_QueryPipeEx(_in KUSB_HANDLE InterfaceHandle, _in UCHAR AltSettingNumber, _in UCHAR PipeIndex, _out PWINUSB_PIPE_INFORMATION_EX PipeInformationEx);
KUSB_EXP BOOL KUSB_API WinUsb_SetPipePolicy(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID, _in UINT PolicyType, _in UINT ValueLength, _in PVOID Value);
KUSB_EXP BOOL KUSB_API WinUsb_GetPipePolicy(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID, _in UINT PolicyType, _ref PUINT ValueLength, _out PVOID Value);
KUSB_EXP BOOL KUSB_API WinUsb_ReadPipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID, _out PUCHAR Buffer, _in UINT BufferLength, _outopt PUINT LengthTransferred, _inopt LPOVERLAPPED Overlapped);
KUSB_EXP BOOL KUSB_API WinUsb_WritePipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID, _in PUCHAR Buffer, _in UINT BufferLength, _outopt PUINT LengthTransferred, _inopt LPOVERLAPPED Overlapped);
KUSB_EXP BOOL KUSB_API WinUsb_ControlTransfer(_in KUSB_HANDLE InterfaceHandle, _in WINUSB_SETUP_PACKET SetupPacket, _refopt PUCHAR Buffer, _in UINT BufferLength, _outopt PUINT LengthTransferred, _inopt LPOVERLAPPED Overlapped);
KUSB_EXP BOOL KUSB_API WinUsb_ResetPipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID);
KUSB_EXP BOOL KUSB_API WinUsb_AbortPipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID);
KUSB_EXP BOOL KUSB_API WinUsb_FlushPipe(_in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeID);
KUSB_EXP BOOL KUSB_API WinUsb_SetPowerPolicy(_in KUSB_HANDLE InterfaceHandle, _in UINT PolicyType, _in UINT ValueLength, _in PVOID Value);
KUSB_EXP BOOL KUSB_API WinUsb_GetPowerPolicy(_in KUSB_HANDLE InterfaceHandle, _in UINT PolicyType, _ref PUINT ValueLength, _out PVOID Value);
KUSB_EXP BOOL KUSB_API WinUsb_GetOverlappedResult(_in KUSB_HANDLE InterfaceHandle, _in LPOVERLAPPED Overlapped, _out PUINT lpNumberOfBytesTransferred, _in BOOL bWait);

/* HotK */
KUSB_EXP BOOL KUSB_API HotK_Init(_out KHOT_HANDLE* Handle, _in PKHOT_PARAMS InitParams);
KUSB_EXP BOOL KUSB_API HotK_Free(_in KHOT_HANDLE Handle);
KUSB_EXP VOID KUSB_API HotK_FreeAll(VOID);

/* OvlK */
KUSB_EXP BOOL KUSB_API OvlK_Acquire(_out KOVL_HANDLE* OverlappedK, _in KOVL_POOL_HANDLE PoolHandle);
KUSB_EXP BOOL KUSB_API OvlK_Release(_in KOVL_HANDLE OverlappedK);
KUSB_EXP BOOL KUSB_API OvlK_Init(_out KOVL_POOL_HANDLE* PoolHandle, _in KUSB_HANDLE UsbHandle, _in INT MaxOverlappedCount, _in KOVL_POOL_FLAG Flags);
KUSB_EXP BOOL KUSB_API OvlK_Free(_in KOVL_POOL_HANDLE PoolHandle);
KUSB_EXP HANDLE KUSB_API OvlK_GetEventHandle(_in KOVL_HANDLE OverlappedK);
KUSB_EXP BOOL KUSB_API OvlK_Wait(_in KOVL_HANDLE OverlappedK, _in INT TimeoutMS, _in KOVL_WAIT_FLAG WaitFlags, _outopt PUINT TransferredLength);
KUSB_EXP BOOL KUSB_API OvlK_WaitOldest(_in KOVL_POOL_HANDLE PoolHandle, _outopt KOVL_HANDLE* OverlappedK, _in INT TimeoutMS, _in KOVL_WAIT_FLAG WaitFlags, _outopt PUINT TransferredLength);
KUSB_EXP BOOL KUSB_API OvlK_WaitOrCancel(_in KOVL_HANDLE OverlappedK, _in INT TimeoutMS, _outopt PUINT TransferredLength);
KUSB_EXP BOOL KUSB_API OvlK_WaitAndRelease(_in KOVL_HANDLE OverlappedK, _in INT TimeoutMS, _outopt PUINT TransferredLength);
KUSB_EXP BOOL KUSB_API OvlK_IsComplete(_in KOVL_HANDLE OverlappedK);
KUSB_EXP BOOL KUSB_API OvlK_ReUse(_in KOVL_HANDLE OverlappedK);

/* StmK */
KUSB_EXP BOOL KUSB_API StmK_Init(_out KSTM_HANDLE* StreamHandle, _in KUSB_HANDLE UsbHandle, _in UCHAR PipeID, _in INT MaxTransferSize, _in INT MaxPendingTransfers, _in INT MaxPendingIO, _inopt PKSTM_CALLBACK Callbacks, _in KSTM_FLAG Flags);
KUSB_EXP BOOL KUSB_API StmK_Free(_in KSTM_HANDLE StreamHandle);
KUSB_EXP BOOL KUSB_API StmK_Start(_in KSTM_HANDLE StreamHandle);
KUSB_EXP BOOL KUSB_API StmK_Stop(_in KSTM_HANDLE StreamHandle, _in INT TimeoutCancelMS);
KUSB_EXP BOOL KUSB_API StmK_Read(_in KSTM_HANDLE StreamHandle, _out PUCHAR Buffer, _in INT Offset, _in INT Length, _outopt PUINT TransferredLength);
KUSB_EXP BOOL KUSB_API StmK_Write(_in KSTM_HANDLE StreamHandle, _in PUCHAR Buffer, _in INT Offset, _in INT Length, _outopt PUINT TransferredLength);

/* IsoK */
KUSB_EXP BOOL KUSB_API IsoK_Init(_out PKISO_CONTEXT* IsoContext, _in INT NumberOfPackets, _inopt INT StartFrame);
KUSB_EXP BOOL KUSB_API IsoK_Free(_in PKISO_CONTEXT IsoContext);
KUSB_EXP BOOL KUSB_API IsoK_SetPackets(_in PKISO_CONTEXT IsoContext, _in INT PacketSize);
KUSB_EXP BOOL KUSB_API IsoK_SetPacket(_in PKISO_CONTEXT IsoContext, _in INT PacketIndex, _in PKISO_PACKET IsoPacket);
KUSB_EXP BOOL KUSB_API IsoK_GetPacket(_in PKISO_CONTEXT IsoContext, _in INT PacketIndex, _out PKISO_PACKET IsoPacket);
KUSB_EXP BOOL KUSB_API IsoK_EnumPackets(_in PKISO_CONTEXT IsoContext, _in KISO_ENUM_PACKETS_CB* EnumPackets, _inopt INT StartPacketIndex, _inopt PVOID UserState);
KUSB_EXP BOOL KUSB_API IsoK_ReUse(_ref PKISO_CONTEXT IsoContext);

/* IsochK */
KUSB_EXP BOOL KUSB_API IsochK_Init(_out KISOCH_HANDLE* IsochHandle, _in KUSB_HANDLE InterfaceHandle, _in UCHAR PipeId, _in UINT MaxNumberOfPackets, _in PUCHAR TransferBuffer, _in UINT TransferBufferSize);
KUSB_EXP BOOL KUSB_API IsochK_Free(_in KISOCH_HANDLE IsochHandle);
KUSB_EXP BOOL KUSB_API IsochK_SetPacketOffsets(_in KISOCH_HANDLE IsochHandle, _in UINT PacketSize);
KUSB_EXP BOOL KUSB_API IsochK_SetPacket(_in KISOCH_HANDLE IsochHandle, _in UINT PacketIndex, _in UINT Offset, _in UINT Length, _in UINT Status);
KUSB_EXP BOOL KUSB_API IsochK_GetPacket(_in KISOCH_HANDLE IsochHandle, _in UINT PacketIndex, _out PUINT Offset, _out PUINT Length, _out PUINT Status);
KUSB_EXP BOOL KUSB_API IsochK_EnumPackets(_in KISOCH_HANDLE IsochHandle, _in KISOCH_ENUM_PACKETS_CB* EnumPackets, _inopt UINT StartPacketIndex, _inopt PVOID UserState);
KUSB_EXP BOOL KUSB_API IsochK_CalcPacketInformation(_in BOOL IsHighSpeed, _in PWINUSB_PIPE_INFORMATION_EX PipeInformationEx, _out PKISOCH_PACKET_INFORMATION PacketInformation);
KUSB_EXP BOOL KUSB_API IsochK_GetNumberOfPackets(_in KISOCH_HANDLE IsochHandle, _out PUINT NumberOfPackets);
KUSB_EXP BOOL KUSB_API IsochK_SetNumberOfPackets(_in KISOCH_HANDLE IsochHandle, _in UINT NumberOfPackets);

/* LUsb0 (libusb0-compat shims) */
KUSB_EXP BOOL KUSB_API LUsb0_ControlTransfer(_in KUSB_HANDLE InterfaceHandle, _in WINUSB_SETUP_PACKET SetupPacket, _refopt PUCHAR Buffer, _in UINT BufferLength, _outopt PUINT LengthTransferred, _inopt LPOVERLAPPED Overlapped);
KUSB_EXP BOOL KUSB_API LUsb0_SetConfiguration(_in KUSB_HANDLE InterfaceHandle, _in UCHAR ConfigurationNumber);

#ifdef __cplusplus
}
#endif

#endif /* LIBUSBK_H__ */
