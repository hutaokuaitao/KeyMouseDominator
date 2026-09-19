#include <ntddk.h>
#include <wdf.h>
#include "driver.h"


NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {

    NTSTATUS status = STATUS_SUCCESS;
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDriverDeviceAdd);

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE
    );
    if (!NT_SUCCESS(status)) {
        KdPrint(("WdfDriverCreate failed: 0x%X\n", status));
    }
    KdPrint(("Driver registered\n"));
    return status;
}