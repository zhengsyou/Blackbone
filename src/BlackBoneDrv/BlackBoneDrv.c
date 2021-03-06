#include "BlackBoneDrv.h"
#include "Remap.h"
#include "Loader.h"
#include <Ntstrsafe.h>

// OS Dependant data
DYNAMIC_DATA dynData;

NTSTATUS DriverEntry( IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING registryPath );
NTSTATUS BBInitDynamicData( IN OUT PDYNAMIC_DATA pData );
VOID     BBUnload( IN PDRIVER_OBJECT DriverObject );

#pragma alloc_text(PAGE, DriverEntry)
#pragma alloc_text(PAGE, BBUnload)
#pragma alloc_text(PAGE, BBInitDynamicData)


/*
*/
NTSTATUS DriverEntry( IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath )
{
    NTSTATUS status = STATUS_SUCCESS;
    PDEVICE_OBJECT deviceObject = NULL;
    UNICODE_STRING deviceName;
    UNICODE_STRING deviceLink;

    UNREFERENCED_PARAMETER( RegistryPath );

    // Get OS Dependant offsets
    status = BBInitDynamicData( &dynData );
    if (!NT_SUCCESS( status ))
    {
        DPRINT( "BlackBone: %s: Unsupported OS version. Aborting\n", __FUNCTION__ );
        return status;
    }

    // Initialize some loader structures
    status = BBInitLdrData( (PKLDR_DATA_TABLE_ENTRY)DriverObject->DriverSection );
    if (!NT_SUCCESS( status ))
        return status;
    //
    // Globals init
    //
    InitializeListHead( &g_PhysProcesses );
    RtlInitializeGenericTableAvl( &g_ProcessPageTables, &AvlCompare, &AvlAllocate, &AvlFree, NULL );
    KeInitializeGuardedMutex( &g_globalLock );

    // Setup process termination notifier
    status = PsSetCreateProcessNotifyRoutine( BBProcessNotify, FALSE );
    if (!NT_SUCCESS( status ))
    {
        DPRINT( "BlackBone: %s: Failed to setup notify routine with staus 0x%X\n", __FUNCTION__, status );
        return status;
    }

    RtlUnicodeStringInit( &deviceName, DEVICE_NAME );
     
    status = IoCreateDevice( DriverObject, 0, &deviceName, FILE_DEVICE_BLACKBONE, 0, FALSE, &deviceObject );
    if (!NT_SUCCESS( status ))
    {
        DPRINT( "BlackBone: %s: IoCreateDevice failed with status 0x%X\n", __FUNCTION__, status );
        return status;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE]          =
    DriverObject->MajorFunction[IRP_MJ_CLOSE]           =
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]  = BBDispatch;
    DriverObject->DriverUnload                          = BBUnload;

    RtlUnicodeStringInit( &deviceLink, DOS_DEVICE_NAME );

    status = IoCreateSymbolicLink( &deviceLink, &deviceName );

    if (!NT_SUCCESS( status ))
    {
        DPRINT( "BlackBone: %s: IoCreateSymbolicLink failed with status 0x%X\n", __FUNCTION__, status );
        IoDeleteDevice (deviceObject);
    }

    return status;
}


/*
*/
VOID BBUnload( IN PDRIVER_OBJECT DriverObject )
{
    UNICODE_STRING deviceLinkUnicodeString;

    // Unregister notification
    PsSetCreateProcessNotifyRoutine( BBProcessNotify, TRUE );

    // Cleanup physical regions
    BBCleanupProcessPhysList();

    // Cleanup process mapping info
    BBCleanupProcessTable();

    RtlUnicodeStringInit( &deviceLinkUnicodeString, DOS_DEVICE_NAME );
    IoDeleteSymbolicLink( &deviceLinkUnicodeString );
    IoDeleteDevice( DriverObject->DeviceObject );

    return;
}


/// <summary>
/// Initialize dynamic data.
/// </summary>
/// <param name="pData">Data to initialize</param>
/// <returns>Status code</returns>
NTSTATUS BBInitDynamicData( IN OUT PDYNAMIC_DATA pData )
{
    NTSTATUS status = STATUS_SUCCESS;
    RTL_OSVERSIONINFOEXW verInfo = { 0 };

    if (pData == NULL)
        return STATUS_INVALID_ADDRESS;

    RtlZeroMemory( pData, sizeof( DYNAMIC_DATA ) );

    verInfo.dwOSVersionInfoSize = sizeof(verInfo);
    status = RtlGetVersion( (PRTL_OSVERSIONINFOW)&verInfo );

    if (status == STATUS_SUCCESS)
    {
        ULONG ver_short = (verInfo.dwMajorVersion << 8) | (verInfo.dwMinorVersion << 4) | verInfo.wServicePackMajor;

        pData->ver = (WinVer)ver_short;

        // Validate current driver version
    #if defined(_WIN7_)
        if (ver_short != WINVER_7 && ver_short != WINVER_7_SP1)
            return STATUS_NOT_SUPPORTED;
    #elif defined(_WIN8_)
        if (ver_short != WINVER_8)
            return STATUS_NOT_SUPPORTED;
    #elif defined (_WIN81_)
        if (ver_short != WINVER_81)
            return STATUS_NOT_SUPPORTED;
    #elif defined (_WIN10_)
        if (ver_short != WINVER_10)
            return STATUS_NOT_SUPPORTED;
    #endif

        DPRINT( "BlackBone: OS version %d.%d.%d.%d.%d - 0x%x\n",
                verInfo.dwMajorVersion,
                verInfo.dwMinorVersion,
                verInfo.dwBuildNumber,
                verInfo.wServicePackMajor,
                verInfo.wServicePackMinor,
                ver_short );

        switch (ver_short)
        {
                // Windows 7
                // Windows 7 SP1
            case WINVER_7:
            case WINVER_7_SP1:
                pData->KExecOpt         = 0x0D2;
                pData->Protection       = 0x43C;  // Bitfield, bit index - 0xB
                pData->ObjTable         = 0x200;
                pData->VadRoot          = 0x448;
                pData->NtProtectIndex   = 0x04D;
                pData->NtCreateThdIndex = 0x0A5;
                pData->NtTermThdIndex   = 0x50;
                pData->PrevMode         = 0x1F6;
                pData->ExitStatus       = 0x380;
                //pData->MiAllocPage    = 0x40A9C0;
                pData->ExRemoveTable    = 0x32D404;
                if (ver_short == WINVER_7_SP1)
                {
                    //pData->MiAllocPage = 0x410D70;
                    pData->ExRemoveTable = 0x32E6B8;
                }
                break;

                // Windows 8
            case WINVER_8:
                pData->KExecOpt         = 0x1B7;
                pData->Protection       = 0x648;
                pData->ObjTable         = 0x408;
                pData->VadRoot          = 0x590;
                pData->NtProtectIndex   = 0x04E;
                pData->NtCreateThdIndex = 0x0AF;
                pData->NtTermThdIndex   = 0x51;
                pData->PrevMode         = 0x232;
                pData->ExitStatus       = 0x450;
                //pData->MiAllocPage    = 0x3AF374;
                pData->ExRemoveTable    = 0x487518;
                break;

                // Windows 8.1
            case WINVER_81:
                pData->KExecOpt         = 0x1B7;
                pData->Protection       = 0x67A;
                pData->ObjTable         = 0x408;
                pData->VadRoot          = 0x5D8;
                pData->NtCreateThdIndex = 0xB0;
                pData->NtTermThdIndex   = 0x52;
                pData->PrevMode         = 0x232;
                pData->ExitStatus       = 0x6D8;
                //pData->MiAllocPage    = 0x4BDDF4;
                pData->ExRemoveTable    = 0x38E320;
                break;

                // Windows 10, technical preview, build 9841
            case WINVER_10:
                pData->KExecOpt         = 0x1BF;
                pData->Protection       = 0x692;
                pData->ObjTable         = 0x410;
                pData->VadRoot          = 0x5F0;
                pData->NtCreateThdIndex = 0xB0;
                pData->NtTermThdIndex   = 0x53;
                pData->PrevMode         = 0x232;
                pData->ExitStatus       = 0x6D8;
                //pData->MiAllocPage    = 0x5191BC;
                pData->ExRemoveTable    = 0x419C2C;
                break;

            default:
                break;
        }

        return (pData->VadRoot != 0 ? status : STATUS_NOT_SUPPORTED);
    }

    return status;
}
