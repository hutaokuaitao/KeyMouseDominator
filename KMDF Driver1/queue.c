#include <ntddk.h>
#include <ntddkbd.h>
#include <ntddmou.h>
#include <wdf.h>
#include "tool.h"


//定义了控制码，控制码应该不唯一吧，应该吧...
#define IOCTL_Test1 CTL_CODE(FILE_DEVICE_UNKNOWN, 0X800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GetKeyboardData CTL_CODE(FILE_DEVICE_UNKNOWN, 0X801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GetMouseData CTL_CODE(FILE_DEVICE_UNKNOWN, 0X802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KeyboardInput CTL_CODE(FILE_DEVICE_UNKNOWN, 0X803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MouseInput CTL_CODE(FILE_DEVICE_UNKNOWN, 0X804, METHOD_BUFFERED, FILE_ANY_ACCESS)

NTSTATUS DequeueKeyboardInput(_Out_ PKEYBOARD_INPUT_DATA InputData);
NTSTATUS DequeueMouseInput(_Out_ PMOUSE_INPUT_DATA InputData);





VOID EvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,  //应用程序期望驱动返回数据的最大长度
    _In_ size_t InputBufferLength,   //应用程序输入数据长度
    _In_ ULONG IoControlCode         //收到的控制码
)
{
    //初始化数据
    PVOID InputBuffer = NULL;        // 输入缓冲区指针（用户模式传入）
    size_t InputBufferLengthRet = 0; // 实际获取的输入缓冲区长度
    PVOID OutputBuffer = NULL;       // 输出缓冲区指针（返回给用户模式）
    size_t OutputBufferLengthRet = 0;// 实际获取的输出缓冲区长度

    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;


    switch (IoControlCode) {
    case IOCTL_Test1:
    {
        // 1. 取输入缓冲区（3环向驱动传递的数据、长度、地址）
        status = WdfRequestRetrieveInputBuffer(
            Request,//告知是哪一次IOCTL
            InputBufferLength,//3环程序发送的数据长度
            &InputBuffer,//3环程序传递的数据地址
            &InputBufferLengthRet//3环程序发送的数据长度地址
        );

        if (!NT_SUCCESS(status))
        {
            KdPrint(("键盘控制码： 获取输入缓冲区失败 0x%X\n", status));
            WdfRequestComplete(Request, status);
            return;
        }



        // 2. 把输入内容打印出来
        KdPrint(("得到键盘操作指令，输入长度=%zu\n", InputBufferLengthRet));

        // 这里假设输入的是字符串
        if (InputBufferLengthRet > 0)
        {
            KdPrint(("输入内容：%.*s\n", (int)InputBufferLengthRet, (char*)InputBuffer));
        }





        // 3. 取输出缓冲区（驱动向3环返回的数据、长度、地址）
        status = WdfRequestRetrieveOutputBuffer(
            Request,
            OutputBufferLength,//3环程序发送的数据长度，我设置默认为1024字节
            &OutputBuffer,//返回给3环程的数据地址
            &OutputBufferLengthRet//3环程序默认接收长度的地址
        );

        if (!NT_SUCCESS(status))
        {
            KdPrint(("键盘控制码： 获取输出缓冲区失败 0x%X\n", status));
            WdfRequestComplete(Request, status);
            return;
        }

        // 4. 定义固定的返回字符串
        const char* fixedResponse = "来自内核驱动的消息！";
        size_t responseLength = strlen(fixedResponse) + 1; // 包含结尾的 '\0'

        // 5. 检查输出缓冲区是否足够容纳固定字符串
        if (responseLength > OutputBufferLengthRet)
        {
            status = STATUS_BUFFER_TOO_SMALL;
            WdfRequestComplete(Request, status);
            return;
        }

        // 6. 把固定字符串复制到驱动的输出内存中
        RtlCopyMemory(OutputBuffer, fixedResponse, responseLength);

        // 7. 告诉系统返回了多少字节
        WdfRequestSetInformation(Request, responseLength);

        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_GetKeyboardData:
    {
        status = WdfRequestRetrieveOutputBuffer(
            Request,
            sizeof(KEYBOARD_INPUT_DATA),
            &OutputBuffer,
            &OutputBufferLengthRet
        );
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }

        status = DequeueKeyboardInput((PKEYBOARD_INPUT_DATA)OutputBuffer);
        if (NT_SUCCESS(status)) {
            WdfRequestSetInformation(Request, sizeof(KEYBOARD_INPUT_DATA));
        }
        break;
    }
    case IOCTL_GetMouseData:
    {
        status = WdfRequestRetrieveOutputBuffer(
            Request,
            sizeof(MOUSE_INPUT_DATA),
            &OutputBuffer,
            &OutputBufferLengthRet
        );
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }

        status = DequeueMouseInput((PMOUSE_INPUT_DATA)OutputBuffer);
        if (NT_SUCCESS(status)) {
            WdfRequestSetInformation(Request, sizeof(MOUSE_INPUT_DATA));
        }
        break;
    }
    case IOCTL_KeyboardInput:
    {
        // 取 3 环传入的输入缓冲区
        status = WdfRequestRetrieveInputBuffer(
            Request,
            sizeof(KEYBOARD_INPUT_DATA),   // 至少需要一个结构体大小
            &InputBuffer,
            &InputBufferLengthRet
        );

        if (!NT_SUCCESS(status)) {
            KdPrint(("IOCTL_KeyboardInput: 获取输入缓冲区失败 0x%X\n", status));
            WdfRequestComplete(Request, status);
            return;
        }

        // 直接调用前先记录收到的数据，方便诊断用户态->驱动路径
        {
            PKEYBOARD_INPUT_DATA pInput = (PKEYBOARD_INPUT_DATA)InputBuffer;
            ULONG consumed = 0;

            KdPrint(("fox键盘输入:UnitId=%u MakeCode=0x%02X Flags=0x%04X Reserved=%u Extra=0x%08X\n",
                pInput->UnitId,
                pInput->MakeCode,
                pInput->Flags,
                pInput->Reserved,
                pInput->ExtraInformation));

            //参数1：设备对象，参数2：键盘数据结构体，参数3：加的数量是键盘数据的数量，参数4：给一个变量，执行完后会把消耗的键盘数据数量写入这个变量
            hkBridgeKeyboardClassServiceCallback(gKeyboardDeviceObject, pInput, pInput + 1, &consumed);
        }

        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_MouseInput:
    {
        // 取 3 环传入的输入缓冲区
        status = WdfRequestRetrieveInputBuffer(
            Request,
            sizeof(MOUSE_INPUT_DATA),
            &InputBuffer,
            &InputBufferLengthRet
        );

        if (!NT_SUCCESS(status)) {
            KdPrint(("fox键盘输入: 获取输入缓冲区失败 0x%X\n", status));
            WdfRequestComplete(Request, status);
            return;
        }

        // 直接调用前先记录收到的数据，方便诊断用户态->驱动路径
        {
            PMOUSE_INPUT_DATA pInput = (PMOUSE_INPUT_DATA)InputBuffer;
            ULONG consumed = 0;

            KdPrint(("fox鼠标输入:UnitId=%u Flags=0x%04X Buttons=0x%08X ButtonFlags=0x%04X ButtonData=%u RawButtons=0x%08X LastX=%d LastY=%d Extra=0x%08X\n",
                pInput->UnitId,
                pInput->Flags,
                pInput->Buttons,
                pInput->ButtonFlags,
                pInput->ButtonData,
                pInput->RawButtons,
                pInput->LastX,
                pInput->LastY,
                pInput->ExtraInformation));

            //参数1：设备对象，参数2：键盘数据结构体，参数3：加的数量是键盘数据的数量，参数4：给一个变量，执行完后会把消耗的键盘数据数量写入这个变量
            hkBridgeMouseClassServiceCallback(gMouseDeviceObject, pInput, pInput + 1, &consumed);
        }

        status = STATUS_SUCCESS;
        break;
    }
    default:
    {
        KdPrint(("未知的控制码"));
        break;
    }
    }
    //表示本次IOCTL请求处理完毕，返回给3环
    WdfRequestComplete(Request, STATUS_SUCCESS);
}
