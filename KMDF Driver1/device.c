#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include "device.h"
//#include "tool.h"


NTSTATUS installKeyboardHook();
NTSTATUS uninstallKeyboardHook();
NTSTATUS installMouseHook();
NTSTATUS uninstallMouseHook();
NTSTATUS InitializeSystemModuleTable();
NTSTATUS FreeSystemModuleTable();
NTSTATUS InitializeInputDataQueues();

// {685186F1-995E-40E4-BDD4-184723D15E94}
DEFINE_GUID(DEVICEINTERFACE,
    0x685186f1, 0x995e, 0x40e4, 0xbd, 0xd4, 0x18, 0x47, 0x23, 0xd1, 0x5e, 0x94);



// 设备清理回调 - 在设备对象被销毁时由 WDF 框架调用
VOID UnloadDriver(WDFOBJECT Device)
{
    UNREFERENCED_PARAMETER(Device);
    // 卸载钩子和释放表
    uninstallKeyboardHook();
    uninstallMouseHook();
    FreeSystemModuleTable();

    //gKeyboardDeviceObject = NULL;
    //gMouseDeviceObject = NULL;
    DbgPrint("[#] 设备清理/驱动卸载...\n");
    // 不要直接调用 IoDeleteDevice，WDF 框架会负责设备的销毁
}


NTSTATUS status;


//主要函数，用于创建设备对象、IO队列、符号链接和初始化系统模块表
NTSTATUS EvtDriverDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    UNREFERENCED_PARAMETER(Driver);
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDFDEVICE device;
    WDFQUEUE queue;
    WDF_IO_QUEUE_CONFIG IoConfig;
    UNICODE_STRING symbolicLinkName;



    // 初始化设备属性（必须！）
    WDF_OBJECT_ATTRIBUTES_INIT(&deviceAttributes);
    // 将设备的清理回调指向 UnloadDriver，驱动卸载或设备销毁时会调用
    deviceAttributes.EvtCleanupCallback = UnloadDriver;


    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("wdf设备创建设备失败 : 0x%08x\n", status));
        return status;
    }



    //配置IO队列函数
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&IoConfig, WdfIoQueueDispatchSequential);
    //EvtIoDeviceControl是处理IOCTL请求的回调函数，放在Queue.c中具体书写
    IoConfig.EvtIoDeviceControl = EvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &IoConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        KdPrint(("IO队列创建失败 0x%08X\n", status));
    }
    else {
        KdPrint(("IO队列创建完成 0x%08X\n", status));
    }

    //创建设备接口
    WdfDeviceCreateDeviceInterface(device, &DEVICEINTERFACE, NULL);

    //创建符号链接
    RtlInitUnicodeString(&symbolicLinkName, L"\\DosDevices\\huli");
    status = WdfDeviceCreateSymbolicLink(device, &symbolicLinkName);




















    //初始化系统模块表
    InitializeInputDataQueues();
    InitializeSystemModuleTable();
	//安装键盘钩子和鼠标钩子
    installKeyboardHook();
    installMouseHook();




    return STATUS_SUCCESS;  // ← 必须有返回值
}