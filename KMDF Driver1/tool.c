#include "tool.h"
#include <ntddk.h>
#include <ntimage.h>
#include <windef.h>
#include <ntddkbd.h>
#include <ntddmou.h>


//初始化系统信息模块
PSYSTEM_MODULE_INFORMATION pSystemModuleTable = NULL;
//初始化键鼠数据队列
KEYBOARD_INPUT_QUEUE gKeyboardDataQueue = { 0 };
MOUSE_INPUT_QUEUE gMouseDataQueue = { 0 };
// 初始化全局保存键盘/鼠标设备对象的指针（在 tool.h 中以 extern 声明）
PDEVICE_OBJECT gKeyboardDeviceObject = NULL;
PDEVICE_OBJECT gMouseDeviceObject = NULL;











//初始化键鼠数据环形队列
NTSTATUS InitializeInputDataQueues() {
	// 清空键盘环形队列，同时将数据数量和读写索引恢复为零。
	RtlZeroMemory(&gKeyboardDataQueue, sizeof(gKeyboardDataQueue));
	// 清空鼠标环形队列，同时将数据数量和读写索引恢复为零。
	RtlZeroMemory(&gMouseDataQueue, sizeof(gMouseDataQueue));
	// 初始化键盘队列自旋锁，保护队列在并发访问时的数据一致性。
	KeInitializeSpinLock(&gKeyboardDataQueue.Lock);
	// 初始化鼠标队列自旋锁，保护队列在并发访问时的数据一致性。
	KeInitializeSpinLock(&gMouseDataQueue.Lock);
	// 队列初始化完成，返回成功状态。
	return STATUS_SUCCESS;
}

//初始化系统模块表的函数(主要给钩子服务)
NTSTATUS InitializeSystemModuleTable() {
	if (pSystemModuleTable) {
		PSYSTEM_MODULE_INFORMATION newTable = GetSystemModuleInformation();
		if (newTable) {
			PSYSTEM_MODULE_INFORMATION oldTable = pSystemModuleTable;
			pSystemModuleTable = newTable;
			ExFreePool(oldTable);
			return STATUS_SUCCESS;
		}
		else {
			DbgPrint("初始化系统模块表失败。\n");
			return STATUS_UNSUCCESSFUL;
		}
	}
	pSystemModuleTable = GetSystemModuleInformation();
	return STATUS_SUCCESS;
}

//释放系统模块表的函数
NTSTATUS FreeSystemModuleTable() {
	if (pSystemModuleTable) {
		ExFreePool(pSystemModuleTable);
		pSystemModuleTable = NULL;
	}
	return STATUS_SUCCESS;
}









//原函数
fnKeyboardClassServiceCallback pKeyboardClassServiceCallback = NULL;
//复印件
fnKeyboardClassServiceCallback hkBridgeKeyboardClassServiceCallback = NULL;


unsigned char OrigKeyboardOpcodes[12];
unsigned char OrigMouseOpcodes[12];


//这块代码通过封装 ZwQuerySystemInformation(SystemModuleInformation, ...) 获取系统所有内核模块的列表，
//遍历列表并用 strstr 匹配目标模块名，拿到它的基址和大小。然后解析该模块的 PE 结构，定位到.text 代码段，
//在代码段里用 FindPattern + CheckMask 做带通配符的字节模式扫描，返回第一处匹配的内存地址。找不到就返回 0。
//获取系统模块信息的函数
PSYSTEM_MODULE_INFORMATION GetSystemModuleInformation() {
	if (KeGetCurrentIrql() <= DISPATCH_LEVEL) {
		ULONG szModule = 0;
		NTSTATUS status = ZwQuerySystemInformation(SystemModuleInformation, 0, 0, &szModule);
		if (STATUS_INFO_LENGTH_MISMATCH != status) {
			DbgPrint("ZwQuerySystemInformation 获取大小失败: %p !\n", status);
			return NULL;
		}
		DbgPrint("ZwQuerySystemInformation 大小: %d\n", szModule);
		PSYSTEM_MODULE_INFORMATION pBuffer = ExAllocatePool(NonPagedPool, szModule);
		if (!pBuffer) {
			DbgPrint("为模块分配 %d 字节失败！\n", szModule);
			return NULL;
		}

		if (!NT_SUCCESS(status = ZwQuerySystemInformation(SystemModuleInformation, pBuffer, szModule, 0))) {
			ExFreePool(pBuffer);
			DbgPrint("ZwQuerySystemInformation 调用失败: %p !\n", status);
			return NULL;
		}
		return pBuffer;
	}
	return NULL;
}
BOOL CheckMask(PCHAR base, PCHAR pattern, PCHAR mask) {
	for (; *mask; ++base, ++pattern, ++mask) {
		if ('x' == *mask && *base != *pattern) {
			return FALSE;
		}
	}

	return TRUE;
}
PVOID FindPattern(PCHAR base, ULONG length, PCHAR pattern, PCHAR mask) {
	length -= (DWORD)strlen(mask);
	for (DWORD i = 0; i <= length; ++i) {
		PVOID addr = &base[i];
		if (CheckMask(addr, pattern, mask)) {
			return addr;
		}
	}
	return 0;
}
PVOID FindPatternImage(PCHAR base, PCHAR pattern, PCHAR mask) {
	PVOID match = 0;
	PIMAGE_NT_HEADERS headers = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
	PIMAGE_SECTION_HEADER sections = IMAGE_FIRST_SECTION(headers);
	for (DWORD i = 0; i < headers->FileHeader.NumberOfSections; ++i) {
		PIMAGE_SECTION_HEADER section = &sections[i];
		if ('EGAP' == *(PINT)section->Name || memcmp(section->Name, ".text", 5) == 0) {
			match = FindPattern(base + section->VirtualAddress, section->Misc.VirtualSize, pattern, mask);
			if (match) {
				break;
			}
		}
	}
	return match;
}
//小写转换
PCHAR LowerStr(PCHAR str) {
	for (PCHAR s = str; *s; ++s) {
		*s = (CHAR)tolower(*s);
	}
	return str;
}
PVOID GetBaseAddress(IN PCHAR pModuleName, OUT PULONG pSize) {
	PVOID pModuleBase = NULL;
	PSYSTEM_MODULE_INFORMATION pBuffer = GetSystemModuleInformation();
	if (!pBuffer) {
		DbgPrint("GetSystemModuleInformation 获取失败。\n");
		return pModuleBase;
	}
	for (int i = 0; i < pBuffer->NumberOfModules; i++) {
		if (strstr(LowerStr((PCHAR)pBuffer->Modules[i].FullPathName), pModuleName)) {
			pModuleBase = pBuffer->Modules[i].ImageBase;
			if (pSize) {
				*pSize = pBuffer->Modules[i].ImageSize;
			}
			break;
		}
	}
	ExFreePool(pBuffer);
	return pModuleBase;
}




//键盘数据入队
static VOID EnqueueKeyboardInput(_In_ const KEYBOARD_INPUT_DATA* InputData) {
	KIRQL oldIrql;
	KeAcquireSpinLock(&gKeyboardDataQueue.Lock, &oldIrql);

	gKeyboardDataQueue.Data[gKeyboardDataQueue.WriteIndex] = *InputData;
	gKeyboardDataQueue.WriteIndex = (gKeyboardDataQueue.WriteIndex + 1) % RTL_NUMBER_OF(gKeyboardDataQueue.Data);
	if (gKeyboardDataQueue.Count == RTL_NUMBER_OF(gKeyboardDataQueue.Data)) {
		gKeyboardDataQueue.ReadIndex = (gKeyboardDataQueue.ReadIndex + 1) % RTL_NUMBER_OF(gKeyboardDataQueue.Data);
	}
	else {
		gKeyboardDataQueue.Count++;
	}

	KeReleaseSpinLock(&gKeyboardDataQueue.Lock, oldIrql);
}

//键盘数据入队
NTSTATUS DequeueKeyboardInput(_Out_ PKEYBOARD_INPUT_DATA InputData) {
	KIRQL oldIrql;

	if (!InputData) {
		return STATUS_INVALID_PARAMETER;
	}

	KeAcquireSpinLock(&gKeyboardDataQueue.Lock, &oldIrql);
	if (gKeyboardDataQueue.Count == 0) {
		KeReleaseSpinLock(&gKeyboardDataQueue.Lock, oldIrql);
		return STATUS_NO_MORE_ENTRIES;
	}

	*InputData = gKeyboardDataQueue.Data[gKeyboardDataQueue.ReadIndex];
	gKeyboardDataQueue.ReadIndex = (gKeyboardDataQueue.ReadIndex + 1) % RTL_NUMBER_OF(gKeyboardDataQueue.Data);
	gKeyboardDataQueue.Count--;

	KeReleaseSpinLock(&gKeyboardDataQueue.Lock, oldIrql);
	return STATUS_SUCCESS;
}

//获取键盘回调函数地址，参数1：kbdclass.sys起始地址，参数2：输出驱动中回调函数的位置
NTSTATUS getKeyboardClassServiceCallback(PVOID moduleBase, PVOID* pOutAddr) {
	if (!moduleBase) {
		return STATUS_UNSUCCESSFUL;
	}
	PVOID pFunctionRef = FindPatternImage(moduleBase, "\xB9\x03\x02\x0B\x00\x48\x8D\x05", "xxxxxxxx"); // 19045.2251
	if (pFunctionRef) {
		PVOID pKeyboardClassServiceCallback = (ULONG64)pFunctionRef + 0x5/*前置指令长度*/ + 0x7/*指令长度*/ + *(INT32*)((ULONG64)pFunctionRef + 0x5 + 0x3);
		*pOutAddr = pKeyboardClassServiceCallback;
		return STATUS_SUCCESS;
	}
	return STATUS_UNSUCCESSFUL;
}

//键盘回调函数
void __fastcall hkKeyboardClassServiceCallback(PDEVICE_OBJECT DeviceObject, PKEYBOARD_INPUT_DATA InputDataStart, PKEYBOARD_INPUT_DATA InputDataEnd, PULONG InputDataConsumed) {


	//只存第一个设备
	if (DeviceObject != NULL && gKeyboardDeviceObject == NULL) {
		gKeyboardDeviceObject = DeviceObject;
	}


	//输出并入队
	for (int i = 0; i < (InputDataEnd - InputDataStart); i++) {
		PKEYBOARD_INPUT_DATA keyboardData = InputDataStart + i;
		DbgPrint("fox键盘 UnitId=%u MakeCode=0x%02X Flags=0x%02X Reserved=%u Extra=%u\n",
			keyboardData->UnitId,
			keyboardData->MakeCode,
			keyboardData->Flags,
			keyboardData->Reserved,
			keyboardData->ExtraInformation);
		EnqueueKeyboardInput(keyboardData);
	}



	//继续让捕获的键盘数据继续进行
	return hkBridgeKeyboardClassServiceCallback(DeviceObject, InputDataStart, InputDataEnd, InputDataConsumed);
}

//安装卸载键盘钩子
NTSTATUS installKeyboardHook() { 
	if (hkBridgeKeyboardClassServiceCallback)
		return STATUS_SUCCESS;
	NTSTATUS status = STATUS_SUCCESS;
	ULONG szModule = 0;

	PVOID kbdclass = GetBaseAddress("kbdclass.sys", &szModule);
	if (!kbdclass) {
		DbgPrint("未找到 kbdclass.sys。\n");
		return STATUS_UNSUCCESSFUL;
	}

	status = getKeyboardClassServiceCallback(kbdclass, &pKeyboardClassServiceCallback);
	if (status != STATUS_SUCCESS) {
		DbgPrint("未找到 KeyboardClassServiceCallback。\n");
		return STATUS_UNSUCCESSFUL;
	}

	if (!MmIsAddressValid(pKeyboardClassServiceCallback)) {
		DbgPrint("验证 KeyboardClassServiceCallback 失败。\n");
		return STATUS_UNSUCCESSFUL;
	}

	DbgPrint("找到: %p。\n", pKeyboardClassServiceCallback);

	PHYSICAL_ADDRESS PAKeyboardClassServiceCallback = MmGetPhysicalAddress(pKeyboardClassServiceCallback);

	if (PAKeyboardClassServiceCallback.QuadPart)
	{

		PVOID VAKeyboardClassServiceCallback = MmMapIoSpaceEx(PAKeyboardClassServiceCallback, 1024, 4i64);
		if (VAKeyboardClassServiceCallback)
		{
			BOOLEAN bFound = FALSE;
			if (!hkBridgeKeyboardClassServiceCallback) {
				int i;
				for (i = 0; i < 64; i++) {
					unsigned char Opcode = *(unsigned char*)((ULONG64)VAKeyboardClassServiceCallback + i);
					if (Opcode == 0x55) {
						bFound = TRUE;
						break;
					}
				}
				if (bFound) {
					GetSystemModuleInformation();
					unsigned char JumpOrig[] = { 0x48, 0xB8, 0x00,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00, /*将回调地址移入RAX*/
											0xFF, 0xE0 /*跳转到RAX*/ };
					hkBridgeKeyboardClassServiceCallback = ExAllocatePool(NonPagedPool, 1024);
					RtlCopyMemory(hkBridgeKeyboardClassServiceCallback, (PVOID)pKeyboardClassServiceCallback, i);
					*(PVOID*)(JumpOrig + 2) = (ULONG64)pKeyboardClassServiceCallback + i;
					RtlCopyMemory((ULONG64)hkBridgeKeyboardClassServiceCallback + i, JumpOrig, sizeof(JumpOrig));
					{
						unsigned char BT[] = { 0x48, 0xB8, 0x00,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00, /*将回调地址移入RAX*/
										0xFF, 0xE0 /*跳转到RAX*/ };
						*(PVOID*)(BT + 2) = hkKeyboardClassServiceCallback;
						RtlCopyMemory(OrigKeyboardOpcodes, VAKeyboardClassServiceCallback, sizeof(BT));
						RtlCopyMemory(VAKeyboardClassServiceCallback, BT, sizeof(BT));
						MmUnmapIoSpace(VAKeyboardClassServiceCallback, 1024);
						status = STATUS_SUCCESS;
						return status;
					}
				}
				else {
					DbgPrint("安装挂钩失败[0]\n");
				}

			}
		}
	}
	return STATUS_UNSUCCESSFUL;
}
NTSTATUS uninstallKeyboardHook() {
	if (pKeyboardClassServiceCallback && hkBridgeKeyboardClassServiceCallback) {
		PHYSICAL_ADDRESS PAKeyboardClassServiceCallback = MmGetPhysicalAddress(pKeyboardClassServiceCallback);
		if (PAKeyboardClassServiceCallback.QuadPart)
		{
			PVOID VAKeyboardClassServiceCallback = MmMapIoSpaceEx(PAKeyboardClassServiceCallback, 1024, 4i64);
			if (VAKeyboardClassServiceCallback) {
				RtlCopyMemory(VAKeyboardClassServiceCallback, OrigKeyboardOpcodes, sizeof(OrigKeyboardOpcodes));
				MmUnmapIoSpace(VAKeyboardClassServiceCallback, 1024);
				ExFreePool(hkBridgeKeyboardClassServiceCallback);
				hkBridgeKeyboardClassServiceCallback = NULL;
				return STATUS_SUCCESS;
			}
		}

	}
	return STATUS_UNSUCCESSFUL;
}






//鼠标数据入队
static VOID EnqueueMouseInput(_In_ const MOUSE_INPUT_DATA* InputData) {
	KIRQL oldIrql;
	KeAcquireSpinLock(&gMouseDataQueue.Lock, &oldIrql);

	gMouseDataQueue.Data[gMouseDataQueue.WriteIndex] = *InputData;
	gMouseDataQueue.WriteIndex = (gMouseDataQueue.WriteIndex + 1) % RTL_NUMBER_OF(gMouseDataQueue.Data);
	if (gMouseDataQueue.Count == RTL_NUMBER_OF(gMouseDataQueue.Data)) {
		gMouseDataQueue.ReadIndex = (gMouseDataQueue.ReadIndex + 1) % RTL_NUMBER_OF(gMouseDataQueue.Data);
	}
	else {
		gMouseDataQueue.Count++;
	}

	KeReleaseSpinLock(&gMouseDataQueue.Lock, oldIrql);
}
//鼠标数据出队
NTSTATUS DequeueMouseInput(_Out_ PMOUSE_INPUT_DATA InputData) {
	KIRQL oldIrql;

	if (!InputData) {
		return STATUS_INVALID_PARAMETER;
	}

	KeAcquireSpinLock(&gMouseDataQueue.Lock, &oldIrql);
	if (gMouseDataQueue.Count == 0) {
		KeReleaseSpinLock(&gMouseDataQueue.Lock, oldIrql);
		return STATUS_NO_MORE_ENTRIES;
	}

	*InputData = gMouseDataQueue.Data[gMouseDataQueue.ReadIndex];
	gMouseDataQueue.ReadIndex = (gMouseDataQueue.ReadIndex + 1) % RTL_NUMBER_OF(gMouseDataQueue.Data);
	gMouseDataQueue.Count--;

	KeReleaseSpinLock(&gMouseDataQueue.Lock, oldIrql);
	return STATUS_SUCCESS;
}

//鼠标原函数
fnMouseClassServiceCallback pMouseClassServiceCallback = NULL;
//鼠标复制函数
fnMouseClassServiceCallback hkBridgeMouseClassServiceCallback = NULL;

//获取鼠标回调函数地址，参数1：mouclass.sys起始地址，参数2：输出驱动中回调函数的位置
NTSTATUS getMouseClassServiceCallback(PVOID moduleBase, PVOID* pOutAddr) {
	if (!moduleBase) {
		return STATUS_UNSUCCESSFUL;
	}
	PVOID pFunctionRef = FindPatternImage(moduleBase, "\xB9\x03\x02\x0F\x00\x48\x8D\x05", "xxxxxxxx"); // 19045.2251
	if (pFunctionRef) {
		PVOID pMouseClassServiceCallback = (ULONG64)pFunctionRef + 0x5/*前置指令长度*/ + 0x7/*指令长度*/ + *(INT32*)((ULONG64)pFunctionRef + 0x5 + 0x3);
		*pOutAddr = pMouseClassServiceCallback;
		return STATUS_SUCCESS;
	}
	return STATUS_UNSUCCESSFUL;
}

//鼠标回调函数
void __fastcall hkMouseClassServiceCallback(PDEVICE_OBJECT DeviceObject, PMOUSE_INPUT_DATA InputDataStart, PMOUSE_INPUT_DATA InputDataEnd, PULONG InputDataConsumed) {

	//只存第一个设备
	if (DeviceObject != NULL && gMouseDeviceObject == NULL) {
		gMouseDeviceObject = DeviceObject;
	}

	// 输出鼠标输入数据。
	for (int i = 0; i < (InputDataEnd - InputDataStart); i++) {
		PMOUSE_INPUT_DATA mouseData = InputDataStart + i;
		//DbgPrint("fox鼠标 UnitId=%u Flags=0x%04X Buttons=0x%08X "
		//	"ButtonFlags=0x%04X ButtonData=%d "
		//	"RawButtons=0x%08X LastX=%d LastY=%d Extra=0x%08X\n",
		//	mouseData->UnitId,
		//	mouseData->Flags,
		//	mouseData->Buttons,
		//	mouseData->ButtonFlags,
		//	mouseData->ButtonData,
		//	mouseData->RawButtons,
		//	mouseData->LastX,
		//	mouseData->LastY,
		//	mouseData->ExtraInformation);
		EnqueueMouseInput(mouseData);
	}
	return hkBridgeMouseClassServiceCallback(DeviceObject, InputDataStart, InputDataEnd, InputDataConsumed);
}

//安装卸载鼠标钩子
NTSTATUS installMouseHook() {
	if (hkBridgeMouseClassServiceCallback)
		return STATUS_SUCCESS;
	NTSTATUS status = STATUS_SUCCESS;
	ULONG szModule = 0;

	PVOID mouclass = GetBaseAddress("mouclass.sys", &szModule);
	if (!mouclass) {
		DbgPrint("未找到 mouclass.sys。\n");
		return STATUS_UNSUCCESSFUL;
	}

	status = getMouseClassServiceCallback(mouclass, &pMouseClassServiceCallback);
	if (status != STATUS_SUCCESS) {
		DbgPrint("未找到 MouseClassServiceCallback。\n");
		return STATUS_UNSUCCESSFUL;
	}

	if (!MmIsAddressValid(pMouseClassServiceCallback)) {
		DbgPrint("验证 MouseClassServiceCallback 失败。\n");
		return STATUS_UNSUCCESSFUL;
	}

	DbgPrint("找到: %p。\n", pMouseClassServiceCallback);

	PHYSICAL_ADDRESS PAMouseClassServiceCallback = MmGetPhysicalAddress(pMouseClassServiceCallback);

	if (PAMouseClassServiceCallback.QuadPart)
	{

		PVOID VAMouseClassServiceCallback = MmMapIoSpaceEx(PAMouseClassServiceCallback, 1024, 4i64);
		if (VAMouseClassServiceCallback)
		{
			BOOLEAN bFound = FALSE;
			if (!hkBridgeMouseClassServiceCallback) {
				int i;
				for (i = 0; i < 64; i++) {
					unsigned char Opcode = *(unsigned char*)((ULONG64)VAMouseClassServiceCallback + i);
					if (Opcode == 0x55) {
						bFound = TRUE;
						break;
					}
				}
				if (bFound) {
					GetSystemModuleInformation();
					unsigned char JumpOrig[] = { 0x48, 0xB8, 0x00,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00, /*将回调地址移入RAX*/
											0xFF, 0xE0 /*跳转到RAX*/ };
					hkBridgeMouseClassServiceCallback = ExAllocatePool(NonPagedPool, 1024);
					RtlCopyMemory(hkBridgeMouseClassServiceCallback, (PVOID)pMouseClassServiceCallback, i);
					*(PVOID*)(JumpOrig + 2) = (ULONG64)pMouseClassServiceCallback + i;
					RtlCopyMemory((ULONG64)hkBridgeMouseClassServiceCallback + i, JumpOrig, sizeof(JumpOrig));
					{
						unsigned char BT[] = { 0x48, 0xB8, 0x00,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00, /*将回调地址移入RAX*/
										0xFF, 0xE0 /*跳转到RAX*/ };
						*(PVOID*)(BT + 2) = hkMouseClassServiceCallback;
						RtlCopyMemory(OrigMouseOpcodes, VAMouseClassServiceCallback, sizeof(BT));
						RtlCopyMemory(VAMouseClassServiceCallback, BT, sizeof(BT));
						MmUnmapIoSpace(VAMouseClassServiceCallback, 1024);
						status = STATUS_SUCCESS;
						return status;
					}
				}
				else {
					DbgPrint("安装挂钩失败[0]\n");
				}

			}
		}
	}
	return STATUS_UNSUCCESSFUL;
}
NTSTATUS uninstallMouseHook() {
	if (pMouseClassServiceCallback && hkBridgeMouseClassServiceCallback) {
		PHYSICAL_ADDRESS PAMouseClassServiceCallback = MmGetPhysicalAddress(pMouseClassServiceCallback);
		if (PAMouseClassServiceCallback.QuadPart)
		{
			PVOID VAMouseClassServiceCallback = MmMapIoSpaceEx(PAMouseClassServiceCallback, 1024, 4i64);
			if (VAMouseClassServiceCallback) {
				RtlCopyMemory(VAMouseClassServiceCallback, OrigMouseOpcodes, sizeof(OrigMouseOpcodes));
				MmUnmapIoSpace(VAMouseClassServiceCallback, 1024);
				ExFreePool(hkBridgeMouseClassServiceCallback);
				hkBridgeMouseClassServiceCallback = NULL;
				return STATUS_SUCCESS;
			}
		}

	}
	return STATUS_UNSUCCESSFUL;
}