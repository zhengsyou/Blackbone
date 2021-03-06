#include "RemoteHook.h"
#include "../ProcessMemory.h"
#include "../ProcessCore.h"

#include <algorithm>

namespace blackbone
{

// Maximum possible instruction length for x86-64
// CPU will raise #GP if length exceeds this value
#define MAX_INSTRUCTION_LENGTH 15

RemoteHook::RemoteHook( class ProcessMemory& memory )
    : _memory( memory )
    , _core( _memory.core() )
{  
}

RemoteHook::~RemoteHook()
{
    reset();
}

/// <summary>
/// Debug selected process
/// </summary>
/// <returns>true on success</returns>
bool RemoteHook::EnsureDebug()
{
    // Close previous debug object
    if (_debugPID != 0 && _debugPID != _core.pid())
    {
        EndDebug();
    }
    // Already debugging
    else if (_debugPID == _core.pid())
    {
        return true;
    }

    //
    // Check if debug is possible.
    //
    BOOL isDebugged = FALSE;

    // Can't debug x64 process from x32 one.
    if (_core.native()->GetWow64Barrier().type == wow_32_64)
        return false;

    // Already under debugger
    CheckRemoteDebuggerPresent( _core.handle(), &isDebugged );
    if (isDebugged == TRUE)
        return false;

    // Create debug event thread
    if (_hEventThd != NULL)
        return false;

    _hEventThd = CreateThread( NULL, 0, &RemoteHook::EventThreadWrap, this, 0, NULL );

    return (_hEventThd != NULL);
}

/// <summary>
/// Stop process debug
/// </summary>
void RemoteHook::EndDebug()
{
    // Stop event thread
    if (_hEventThd != NULL)
    {
        _active = false;
        WaitForSingleObject( _hEventThd, INFINITE );
        _hEventThd = NULL;
    }
}

/// <summary>
/// Hook specified address
/// </summary>
/// <param name="type">Hook type</param>
/// <param name="ptr">Address</param>
/// <param name="newFn">Callback</param>
/// <param name="pClass">Class reference.</param>
/// <param name="pThread">Thread to hook. Valid only for HWBP</param>
/// <returns>true on success</returns>
bool RemoteHook::ApplyP( eHookType type, uint64_t ptr, fnCallback newFn, const void* pClass /*= nullptr*/, Thread* pThread /*= nullptr*/ )
{
    if (!EnsureDebug())
        return false;

    HookData data = { 0 };

    // Store old byte
    data.oldByte = _memory.Read<uint8_t>( ptr );
    data.type = type;
    data.onExecute.freeFn = newFn;
    data.onExecute.classFn.classPtr = pClass;
    data.threadID = (pThread != nullptr) ? pThread->id() : 0;
    
    // Set HWBP
    if(type == ht_hwbp)
    {
        // Set for single thread
        if (pThread != nullptr)
        {
            data.hwbp_idx = pThread->AddHWBP( ptr, hwbp_execute, hwbp_1 );

            if (data.hwbp_idx == -1)
                return false;
        }
        // Set for all threads
        else
        {
            ProcessThreads thds( _core );
            auto& threads = thds.getAll();
            
            for (auto& thread : threads)
                thread.AddHWBP( ptr, hwbp_execute, hwbp_1 );
        }
    }
    // Write int3
    else
    {
        if (_memory.Write<uint8_t>( ptr, 0xCC ) != STATUS_SUCCESS)
            return false;
    }

    // Store hooked address
    _hooks.emplace( std::make_pair( ptr, data ) );

    return true;
}

/// <summary>
/// Hook function return
/// This hook will only work if function is hooked normally
/// </summary>
/// <param name="ptr">Hooked function address</param>
/// <param name="newFn">Callback</param>
/// <param name="pClass">Class reference.</param>
/// <returns>true on success</returns>
bool RemoteHook::AddReturnHookP( uint64_t ptr, fnCallback newFn, const void* pClass /*= nullptr */ )
{
    if(_hooks.count(ptr))
    {
        auto& hook = _hooks[ptr];

        // Add return callback
        hook.onReturn.freeFn = newFn;
        hook.onReturn.classFn.classPtr = pClass;
        hook.flags = static_cast<eHookFlags>(hook.flags | hf_returnHook);

        return true;
    }

    return false;
}

/// <summary>
/// Remove existing hook
/// </summary>
/// <param name="ptr">Hooked address</param>
void RemoteHook::Remove( uint64_t ptr )
{
    // Restore hooked function
    if (_hooks.count( ptr ))
    {
        auto& hook = _hooks[ptr];

        Restore( hook, ptr );
        _hooks.erase( ptr );
    }    
}

/// <summary>
/// Restore hooked function
/// </summary>
/// <param name="hook">Hook data</param>
/// <param name="ptr">Hooked address</param>
void RemoteHook::Restore( const HookData &hook, uint64_t ptr )
{
    // Remove HWBP
    if (hook.type == ht_hwbp)
    {       
        if (hook.threadID != 0)
        {
            Thread thd( hook.threadID, &_core );
            thd.RemoveHWBP( hook.hwbp_idx );
        }   
        else
        {
            ProcessThreads thds( _core );
            auto& threads = thds.getAll();

            for (auto& thread : threads)
                thread.RemoveHWBP( ptr );
        }
    }
    // Restore original byte
    else
    {
        _memory.Write( ptr, hook.oldByte );
    }
}

/// <summary>
/// Wrapper for debug event thread
/// </summary>
/// <param name="lpParam">RemoteHook pointer</param>
/// <returns>Error code</returns>
DWORD RemoteHook::EventThreadWrap( LPVOID lpParam )
{
    if (lpParam)
        return reinterpret_cast<RemoteHook*>(lpParam)->EventThread();

    return ERROR_INVALID_PARAMETER;
}

/// <summary>
/// Debug thread
/// </summary>
/// <returns>Error code</returns>
DWORD RemoteHook::EventThread()
{
    // Try to debug process
    if (DebugActiveProcess( _core.pid() ) != TRUE)
        return LastNtStatus();

    DebugSetProcessKillOnExit( FALSE );

    _debugPID = _core.pid();
    _active   = true;

    // Determine if target is Wow64
    _x64Target = !_core.native()->GetWow64Barrier().targetWow64;
    _wordSize = (_x64Target) ? sizeof(int64_t) : sizeof(int32_t);

    // 
    // Reset debug flag in PEB
    //
    _memory.Write( _core.peb() + FIELD_OFFSET( _PEB64, BeingDebugged ), uint8_t( 0 ) );
    if (!_x64Target)
        _memory.Write( _core.peb() + FIELD_OFFSET( _PEB32, BeingDebugged ), uint8_t( 0 ) );

    // Process debug events
    while (_active)
    {
        DEBUG_EVENT DebugEv = { 0 };
        DWORD status = DBG_CONTINUE;

        if (!WaitForDebugEvent( &DebugEv, 100 ))
            continue;

        switch (DebugEv.dwDebugEventCode)
        {
            case EXCEPTION_DEBUG_EVENT:           
                status = OnDebugEvent( DebugEv );
                break;

                // Add HWBP to created thread
            case CREATE_THREAD_DEBUG_EVENT:
                for(auto& hook : _hooks)
                    if (hook.second.type == ht_hwbp && hook.second.threadID == 0)
                    {
                        Thread th( DebugEv.u.CreateThread.hThread, &_core );
                        th.AddHWBP( hook.first, hwbp_execute, hwbp_1 );
                    }
                break;

            default:
                break;
        }

        ContinueDebugEvent( DebugEv.dwProcessId, DebugEv.dwThreadId, status );
    }

    // Safely detach debugger
    if (_debugPID != 0)
    {
        DebugActiveProcessStop( _debugPID );
        _debugPID = 0;
    }

    return STATUS_SUCCESS;
}

/// <summary>
/// Debug event handler
/// </summary>
/// <param name="DebugEv">Debug event data</param>
/// <returns>Status</returns>
DWORD RemoteHook::OnDebugEvent( const DEBUG_EVENT& DebugEv )
{
    switch (DebugEv.u.Exception.ExceptionRecord.ExceptionCode)
    {
    case EXCEPTION_BREAKPOINT:
    case STATUS_WX86_BREAKPOINT:
        return OnBreakpoint( DebugEv );

    case EXCEPTION_SINGLE_STEP:
    case STATUS_WX86_SINGLE_STEP:
        return OnSinglestep( DebugEv );

    case EXCEPTION_ACCESS_VIOLATION:
        return OnAccessViolation( DebugEv );

    default:
        return (DWORD)DBG_EXCEPTION_NOT_HANDLED;
    }
}

/// <summary>
/// Int 3 handler
/// </summary>
/// <param name="DebugEv">Debug event data</param>
/// <returns>Status</returns>
DWORD RemoteHook::OnBreakpoint( const DEBUG_EVENT& DebugEv )
{
    // Prevent highest bit extension.
    ptr_t addr = (size_t)DebugEv.u.Exception.ExceptionRecord.ExceptionAddress;
    ptr_t ip = 0, sp = 0;
    Thread thd( DebugEv.dwThreadId, &_core );

    if (_hooks.count( addr ))
    {
        _CONTEXT64 ctx64;
        _CONTEXT32 ctx32;
        thd.GetContext( ctx64, CONTEXT64_FULL, true );

        if (_memory.core().isWow64())
        {
            thd.GetContext( ctx32, WOW64_CONTEXT_FULL, true );
            ip = ctx32.Eip;
            sp = ctx32.Esp;
        }
        else
        {
            ip = ctx64.Rip;
            sp = ctx64.Rsp;
        }
        
        // Get stack frame pointer
        std::vector<std::pair<ptr_t, ptr_t>> results;
        StackBacktrace( ip, sp, thd, results, 1 );

        RemoteContext context( _memory, thd, ctx64, !results.empty() ? results.back().first : 0, _x64Target, _wordSize );

        // Execute user callback
        auto hook = _hooks[addr];

        if (hook.onExecute.classFn.classPtr && hook.onExecute.classFn.ptr != nullptr)
            hook.onExecute.classFn.ptr( hook.onExecute.classFn.classPtr, context );
        else if (hook.onExecute.freeFn != nullptr)
            hook.onExecute.freeFn( context );

        // Resume execution
        _memory.Write( addr, hook.oldByte );
      
        _repatch[addr] = true;
        
        // Single-step failed instruction
        ctx64.Rip -= sizeof(uint8_t);
        ctx64.EFlags |= 0x100;
        thd.SetContext( ctx64, true );

        // Raise exceptions upon return
        if(hook.flags & hf_returnHook)
        {
            auto newReturn = context.hookReturn();
            _retHooks.emplace( std::make_pair( newReturn, addr ) );
        }

        return DBG_CONTINUE;
    }

    return (DWORD)DBG_EXCEPTION_NOT_HANDLED;
}

/// <summary>
/// Trace and hardware breakpoints handler
/// </summary>
/// <param name="DebugEv">Debug event data</param>
/// <returns>Status</returns>
DWORD RemoteHook::OnSinglestep( const DEBUG_EVENT& DebugEv )
{
    // Prevent highest bit extension.
    ptr_t addr = (size_t)DebugEv.u.Exception.ExceptionRecord.ExceptionAddress;
    ptr_t ip = 0, sp = 0;
    _CONTEXT32 ctx32;
    _CONTEXT64 ctx64;

    Thread thd( DebugEv.dwThreadId, &_core );

    thd.GetContext( ctx64, CONTEXT64_ALL, true );
    ip = ctx64.Rip;
    sp = ctx64.Rsp;

    // Use 32 bit context if available
    if (_memory.core().isWow64())
    {
        thd.GetContext( ctx32, WOW64_CONTEXT_ALL, true );
        ip = ctx32.Eip;
        sp = ctx32.Esp;
    }

    // Detect hardware breakpoint
    DWORD index = 0;
    if (_BitScanForward( &index, LODWORD( ctx64.Dr6 ) ) != 0 && index < 4)
    {
        if (_hooks.count( addr ))
        {
            // Get stack frame pointer
            std::vector<std::pair<ptr_t, ptr_t>> results;
            StackBacktrace( ip, sp, thd, results, 1 );

            RemoteContext context( _memory, thd, ctx64, !results.empty() ? results.back().first : 0, _x64Target, _wordSize );

            // Execute user callback
            auto hook = _hooks[addr];
            if (hook.onExecute.classFn.classPtr && hook.onExecute.classFn.ptr != nullptr)
                hook.onExecute.classFn.ptr( hook.onExecute.classFn.classPtr, context );
            else if (hook.onExecute.freeFn != nullptr)
                hook.onExecute.freeFn( context );

            // Reset breakpoint
            //*(reinterpret_cast<uint64_t*>(&ctx64.Dr0) + index) = 0;
            RESET_BIT( ctx64.Dr7, (2 * index) );

            _repatch[addr] = true;

            ctx64.Dr6 = 0;                // reset flags
            ctx64.EFlags |= 0x100;        // single step
            thd.SetContext( ctx64, true );

            // Raise exception upon return
            if (hook.flags & hf_returnHook)
            {
                auto newReturn = context.hookReturn();
                _retHooks.emplace( std::make_pair( newReturn, addr ) );
            }

            return DBG_CONTINUE;
        }
    }

    // Restore pending hooks
    for(auto place : _repatch)
    {
        if (place.second == true)
        {
            auto& hook = _hooks[place.first];

            if (hook.type == ht_hwbp)
                thd.AddHWBP( place.first, hwbp_execute, hwbp_1 );
            else if (hook.type == ht_int3)
                _memory.Write( place.first, uint8_t( 0xCC ) );

            place.second = false;
        }
    }

    return DBG_CONTINUE;
}

/// <summary>
/// Access violation handler
/// Used when hooking function return
/// </summary>
/// <param name="DebugEv">Debug event data</param>
/// <returns>Status</returns>
DWORD RemoteHook::OnAccessViolation( const DEBUG_EVENT& DebugEv )
{ 
    // Prevent highest bit extension.
    ptr_t addr = (size_t)DebugEv.u.Exception.ExceptionRecord.ExceptionAddress;
    ptr_t ip = 0, sp = 0;
    _CONTEXT32 ctx32;
    _CONTEXT64 ctx64;

    Thread thd( DebugEv.dwThreadId, &_core );

    thd.GetContext( ctx64, CONTEXT64_ALL, true );
    ip = ctx64.Rip;
    sp = ctx64.Rsp;

    // Use 32 bit context if available
    if(_memory.core().isWow64())
    {
        thd.GetContext( ctx32, WOW64_CONTEXT_ALL, true );
        ip = ctx32.Eip;
        sp = ctx32.Esp;
    }

    // Get stack frame pointer
    std::vector<std::pair<ptr_t, ptr_t>> results;
    StackBacktrace( ip, sp, thd, results, 1 );

    RemoteContext context( _memory, thd, ctx64, !results.empty() ? results.back().first : 0, _x64Target, _wordSize );

    // Under AMD64 exception is thrown before 'ret' gets executed
    // Return must be detected manually
    if (_x64Target && results.size() > 0)
        addr = results.back().second;
            
    // Test if this is a return hook
    if(_retHooks.count(addr))
    {
        // Execute user callback
        auto hook = _hooks[_retHooks[addr]];

        if(hook.flags & hf_returnHook)
        {
            if (hook.onReturn.classFn.classPtr && hook.onReturn.classFn.ptr != nullptr)
                hook.onReturn.classFn.ptr( hook.onExecute.classFn.classPtr, context );
            else if (hook.onReturn.freeFn != nullptr)
                hook.onReturn.freeFn( context );
        }

        // Under AMD64there is no need to update IP, because exception is thrown before actual return.
        // Return address still must be fixed though.
        if(_x64Target)
        {
            context.unhookReturn();
        }    
        else
        {
            auto retAddr = addr;
            RESET_BIT(retAddr, (_wordSize * 8 - 1));

            ctx64.Rip = retAddr;
        }  

        thd.SetContext( ctx64, true );
        _retHooks.erase( addr );

        return (DWORD)DBG_CONTINUE;
    }

    return (DWORD)DBG_EXCEPTION_NOT_HANDLED;
}

/// <summary>
/// Walk stack frames
/// </summary>
/// <param name="ip">Thread instruction pointer</param>
/// <param name="sp">>Thread stack pointer</param>
/// <param name="thd">Stack owner</param>
/// <param name="results">Stack frames</param>
/// <param name="depth">Max frame count</param>
/// <returns>Frame count</returns>
DWORD RemoteHook::StackBacktrace( ptr_t ip, ptr_t sp, Thread& thd, std::vector<std::pair<ptr_t, ptr_t>>& results, int depth /*= 100 */ )
{
    int i = 0;
    uint64_t stack_base = 0;

    // Get stack base
    if(_core.isWow64())
    {
        _TEB32 teb32 = { 0 };
        if (thd.teb( &teb32 ) == 0)
            return 0;

        stack_base = teb32.NtTib.StackBase;
    }
    else
    {
        _TEB64 teb64 = { 0 };
        if (thd.teb( &teb64 ) == 0)
            return 0;

        stack_base = teb64.NtTib.StackBase;
    }

    // Store exception address
    results.emplace_back( std::make_pair( 0, ip ) );

    // Walk stack
    for (ptr_t stackPtr = sp; stackPtr < stack_base && i < depth; stackPtr += _wordSize)
    {
        ptr_t stack_val = 0;
        _memory.Read( stackPtr, _wordSize, &stack_val );
        MEMORY_BASIC_INFORMATION64 meminfo = { 0 };

        ptr_t original = stack_val & 0x7FFFFFFFFFFFFFFF;

        // Invalid value
        if (stack_val < _core.native()->minAddr() || original > _core.native()->maxAddr())
            continue;

        // Check if memory is executable
        if (_core.native()->VirtualQueryExT( original, &meminfo ) != STATUS_SUCCESS)
            continue;

        if ( meminfo.AllocationProtect != PAGE_EXECUTE_READ &&
             meminfo.AllocationProtect != PAGE_EXECUTE_WRITECOPY &&
             meminfo.AllocationProtect != PAGE_EXECUTE_READWRITE)
        {
            continue;
        }

        uint8_t codeChunk[6] = {0};
        _memory.Read( original - 6, sizeof(codeChunk), codeChunk );

        // Detect 'call' instruction
        // TODO: Implement more reliable way to detect 'call'
        if (codeChunk[0] == 0xFF || codeChunk[1] == 0xE8 || codeChunk[4] == 0xFF)
        {
            results.emplace_back( std::make_pair( stackPtr, stack_val ) );
            i++;
        }
    }

    return i;
}

/// <summary>
/// Stop debug and remove all hooks
/// </summary>
void RemoteHook::reset()
{
    for (auto& hook : _hooks)
        Restore( hook.second, hook.first );

    EndDebug();
}

}