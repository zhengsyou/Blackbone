#pragma once
#include <wdm.h>


/// <summary>
/// Allocate new Unicode string from Paged pool
/// </summary>
/// <param name="result">Resulting string</param>
/// <param name="size">Buffer size in bytes to alloacate</param>
/// <returns>Status code</returns>
NTSTATUS BBSafeAllocateString( OUT PUNICODE_STRING result, IN USHORT size );

/// <summary>
/// Allocate and copy string
/// </summary>
/// <param name="result">Resulting string</param>
/// <param name="source">Source string</param>
/// <returns>Status code</returns>
NTSTATUS BBSafeInitString( OUT PUNICODE_STRING result, IN PUNICODE_STRING source );


/// <summary>
/// Get file name from full path
/// </summary>
/// <param name="path">Path.</param>
/// <param name="name">Resulting name</param>
/// <returns>Status code</returns>
NTSTATUS BBStripPath( IN PUNICODE_STRING path, OUT PUNICODE_STRING name );

/// <summary>
/// Get directory path name from full path
/// </summary>
/// <param name="path">Path</param>
/// <param name="name">Resulting directory path</param>
/// <returns>Status code</returns>
NTSTATUS BBStripFilename( IN PUNICODE_STRING path, OUT PUNICODE_STRING dir );

/// <summary>
/// Check if file exists
/// </summary>
/// <param name="path">Fully qualifid path to a file</param>
/// <returns>Status code</returns>
NTSTATUS BBFileExists( IN PUNICODE_STRING path );

//
// Machine code generation routines
//
ULONG GenPrologue32( IN PUCHAR pBuf );
ULONG GenEpilogue32( IN PUCHAR pBuf, IN INT retSize );
ULONG GenCall32( IN PUCHAR pBuf, IN PVOID pFn, IN INT argc, ... );
ULONG GenCall32V( IN PUCHAR pBuf, IN PVOID pFn, IN INT argc, IN va_list vl );
ULONG GenSync32( IN PUCHAR pBuf, IN PNTSTATUS pStatus, IN PVOID pSetEvent, IN HANDLE hEvent );


ULONG GenPrologue64( IN PUCHAR pBuf );
ULONG GenEpilogue64( IN PUCHAR pBuf, IN INT retSize );
ULONG GenCall64( IN PUCHAR pBuf, IN PVOID pFn, INT argc, ... );
ULONG GenCall64V( IN PUCHAR pBuf, IN PVOID pFn, INT argc, va_list vl );
ULONG GenSync64( IN PUCHAR pBuf, IN PNTSTATUS pStatus, IN PVOID pSetEvent, IN HANDLE hEvent );


ULONG GenPrologueT( IN BOOLEAN wow64, IN PUCHAR pBuf );
ULONG GenEpilogueT( IN BOOLEAN wow64, IN PUCHAR pBuf, IN INT retSize );
ULONG GenCallT( IN BOOLEAN wow64, IN PUCHAR pBuf, IN PVOID pFn, IN INT argc, ... );
ULONG GenCallTV( IN BOOLEAN wow64, IN PUCHAR pBuf, IN PVOID pFn, IN INT argc, IN va_list vl );
ULONG GenSyncT( IN BOOLEAN wow64, IN PUCHAR pBuf, IN PNTSTATUS pStatus, IN PVOID pSetEvent, IN HANDLE hEvent );
