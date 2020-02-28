#include "hooks.h"
#include "undocumented.h"
#include "ssdt.h"
#include "hider.h"
#include "misc.h"
#include "log.h"

static HOOK hNtQueryInformationProcess = 0;
static HOOK hNtQueryInformationThread = 0;
static HOOK hNtQueryObject = 0;
static HOOK hNtQuerySystemInformation = 0;
static HOOK hNtClose = 0;
static HOOK hNtDuplicateObject = 0;
static HOOK hNtSetInformationThread = 0;
static HOOK hNtGetContextThread = 0;
static HOOK hNtSetContextThread = 0;
static HOOK hNtSystemDebugControl = 0;
static KMUTEX gDebugPortMutex;

//https://forum.tuts4you.com/topic/40011-debugme-vmprotect-312-build-886-anti-debug-method-improved/#comment-192824
//https://github.com/x64dbg/ScyllaHide/issues/47
//https://github.com/mrexodia/TitanHide/issues/27
#define BACKUP_RETURNLENGTH() \
    ULONG TempReturnLength = 0; \
    if(ARGUMENT_PRESENT(ReturnLength)) \
        TempReturnLength = *ReturnLength

#define RESTORE_RETURNLENGTH() \
    if(ARGUMENT_PRESENT(ReturnLength)) \
        (*ReturnLength) = TempReturnLength

#define OBJ_PROTECT_CLOSE 0x00000001L

static NTSTATUS NTAPI HookNtQueryInformationThread(
    IN HANDLE ThreadHandle,
    IN THREADINFOCLASS ThreadInformationClass,
    IN OUT PVOID ThreadInformation,
    IN ULONG ThreadInformationLength,
    OUT PULONG ReturnLength OPTIONAL)
{
    // ThreadWow64Context returns STATUS_INVALID_INFO_CLASS on x86, and STATUS_INVALID_PARAMETER if PreviousMode is kernel
#ifdef _WIN64
    ULONG pid = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
    ULONG targetPid = Misc::GetProcessIDFromThreadHandle(ThreadHandle);
    if(ThreadInformationClass == ThreadWow64Context &&
            ThreadInformation != nullptr &&
            ThreadInformationLength == sizeof(WOW64_CONTEXT) &&
            ExGetPreviousMode() != KernelMode &&
            Hider::IsHidden(pid, HideNtGetContextThread) &&
            Hider::IsHidden(targetPid, HideNtGetContextThread))
    {
        PWOW64_CONTEXT Wow64Context = (PWOW64_CONTEXT)ThreadInformation;
        ULONG OriginalContextFlags = 0;
        bool DebugRegistersRequested = false;

        Log("[TITANHIDE] NtGetContextThread by %d\r\n", pid);

        __try
        {
            ProbeForWrite(&Wow64Context->ContextFlags, sizeof(ULONG), 1);
            OriginalContextFlags = Wow64Context->ContextFlags;
            Wow64Context->ContextFlags = OriginalContextFlags & ~0x10; //CONTEXT_DEBUG_REGISTERS ^ CONTEXT_AMD64/CONTEXT_i386
            DebugRegistersRequested = Wow64Context->ContextFlags != OriginalContextFlags;
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            NOTHING;
        }

        const NTSTATUS Status = Undocumented::NtQueryInformationThread(ThreadHandle, ThreadInformationClass, ThreadInformation, ThreadInformationLength, ReturnLength);

        __try
        {
            ProbeForWrite(&Wow64Context->ContextFlags, sizeof(ULONG), 1);
            Wow64Context->ContextFlags = OriginalContextFlags;

            // If debug registers were requested, zero user input
            if(DebugRegistersRequested)
            {
                Wow64Context->Dr0 = 0;
                Wow64Context->Dr1 = 0;
                Wow64Context->Dr2 = 0;
                Wow64Context->Dr3 = 0;
                Wow64Context->Dr6 = 0;
                Wow64Context->Dr7 = 0;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            NOTHING;
        }

        return Status;
    }
#endif

    return Undocumented::NtQueryInformationThread(ThreadHandle, ThreadInformationClass, ThreadInformation, ThreadInformationLength, ReturnLength);
}

static NTSTATUS NTAPI HookNtSetInformationThread(
    IN HANDLE ThreadHandle,
    IN THREADINFOCLASS ThreadInformationClass,
    IN PVOID ThreadInformation,
    IN ULONG ThreadInformationLength)
{
    const ULONG pid = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();

    //Bug found by Aguila, thanks!
    if(ThreadInformationClass == ThreadHideFromDebugger && !ThreadInformationLength)
    {
        if(Hider::IsHidden(pid, HideThreadHideFromDebugger))
        {
            Log("[TITANHIDE] ThreadHideFromDebugger by %d\r\n", pid);
            PETHREAD Thread;
            NTSTATUS status;
#if NTDDI_VERSION >= NTDDI_WIN8
            status = ObReferenceObjectByHandleWithTag(ThreadHandle,
                     THREAD_SET_INFORMATION,
                     *PsThreadType,
                     ExGetPreviousMode(),
                     'yQsP', // special 'PsQuery' tag used in many Windows 8/8.1/10 NtXX/ZwXX functions
                     (PVOID*)&Thread,
                     NULL);
#else // Vista and XP don't have ObReferenceObjectByHandleWithTag; 7 has it but doesn't use it in NtSetInformationThread
            status = ObReferenceObjectByHandle(ThreadHandle,
                                               THREAD_SET_INFORMATION,
                                               *PsThreadType,
                                               ExGetPreviousMode(),
                                               (PVOID*)&Thread,
                                               NULL);
#endif
            if(NT_SUCCESS(status))
            {
#if NTDDI_VERSION >= NTDDI_WIN8
                ObfDereferenceObjectWithTag(Thread, 'yQsP');
#else
                ObDereferenceObject(Thread);
#endif
            }
            return status;
        }
    }
    // ThreadWow64Context returns STATUS_INVALID_INFO_CLASS on x86, and STATUS_INVALID_PARAMETER if PreviousMode is kernel
#ifdef _WIN64
    else if(ThreadInformationClass == ThreadWow64Context &&
            ThreadInformation != nullptr &&
            ThreadInformationLength == sizeof(WOW64_CONTEXT) &&
            ExGetPreviousMode() != KernelMode &&
            Hider::IsHidden(pid, HideNtSetContextThread))
    {
        PWOW64_CONTEXT Wow64Context = (PWOW64_CONTEXT)ThreadInformation;
        ULONG OriginalContextFlags = 0;

        Log("[TITANHIDE] NtSetContextThread by %d\r\n", pid);

        __try
        {
            ProbeForWrite(&Wow64Context->ContextFlags, sizeof(ULONG), 1);
            OriginalContextFlags = Wow64Context->ContextFlags;
            Wow64Context->ContextFlags = OriginalContextFlags & ~0x10; //CONTEXT_DEBUG_REGISTERS ^ CONTEXT_AMD64/CONTEXT_i386
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            NOTHING;
        }

        const NTSTATUS Status = Undocumented::NtSetInformationThread(ThreadHandle, ThreadInformationClass, ThreadInformation, ThreadInformationLength);

        __try
        {
            ProbeForWrite(&Wow64Context->ContextFlags, sizeof(ULONG), 1);
            Wow64Context->ContextFlags = OriginalContextFlags;
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            NOTHING;
        }

        return Status;
    }
#endif

    return Undocumented::NtSetInformationThread(ThreadHandle, ThreadInformationClass, ThreadInformation, ThreadInformationLength);
}

static NTSTATUS NTAPI HookNtClose(
    IN HANDLE Handle)
{
    ULONG pid = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    if(Hider::IsHidden(pid, HideNtClose))
    {
        KeWaitForSingleObject(&gDebugPortMutex, Executive, KernelMode, FALSE, nullptr);

        // Check if this is a valid handle without raising exceptionss
        BOOLEAN AuditOnClose;
        NTSTATUS ObStatus = ObQueryObjectAuditingByHandle(Handle, &AuditOnClose);

        NTSTATUS Status;
        if(ObStatus != STATUS_INVALID_HANDLE)  // Don't change the return path for any status except this one
        {
            BOOLEAN BeingDebugged = PsGetProcessDebugPort(PsGetCurrentProcess()) != nullptr;
            OBJECT_HANDLE_INFORMATION HandleInfo = { 0 };

            if(BeingDebugged)
            {
                // Get handle info so we can check if the handle has the ProtectFromClose bit set
                PVOID Object = nullptr;
                ObStatus = ObReferenceObjectByHandle(Handle,
                                                     0,
                                                     nullptr,
                                                     PreviousMode,
                                                     &Object,
                                                     &HandleInfo);
                if(Object != nullptr)
                    ObDereferenceObject(Object);
            }

            if(BeingDebugged && NT_SUCCESS(ObStatus) &&
                    (HandleInfo.HandleAttributes & OBJ_PROTECT_CLOSE))
            {
                // Return STATUS_HANDLE_NOT_CLOSABLE instead of raising an exception
                Log("[TITANHIDE] NtClose(0x%p) (protected handle) by %d\r\n", Handle, pid);
                Status = STATUS_HANDLE_NOT_CLOSABLE;
            }
            else
            {
                Status = ObCloseHandle(Handle, PreviousMode);
            }
        }
        else
        {
            Log("[TITANHIDE] NtClose(0x%p) by %d\r\n", Handle, pid);
            Status = STATUS_INVALID_HANDLE;
        }

        KeReleaseMutex(&gDebugPortMutex, FALSE);

        return Status;
    }
    return ObCloseHandle(Handle, PreviousMode);
}

static NTSTATUS NTAPI HookNtDuplicateObject(
    IN HANDLE SourceProcessHandle,
    IN HANDLE SourceHandle,
    IN HANDLE TargetProcessHandle,
    OUT PHANDLE TargetHandle,
    IN ACCESS_MASK DesiredAccess OPTIONAL,
    IN ULONG HandleAttributes,
    IN ULONG Options)
{
    ULONG pid = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    if(Hider::IsHidden(pid, HideNtClose))
    {
        BOOLEAN BeingDebugged = PsGetProcessDebugPort(PsGetCurrentProcess()) != nullptr;
        if(BeingDebugged && (Options & DUPLICATE_CLOSE_SOURCE))
        {
            // Get handle info so we can check if the handle has the ProtectFromClose bit set
            PVOID Object = nullptr;
            OBJECT_HANDLE_INFORMATION HandleInfo = { 0 };
            NTSTATUS Status = ObReferenceObjectByHandle(SourceHandle,
                              0,
                              nullptr,
                              PreviousMode,
                              &Object,
                              &HandleInfo);

            if(NT_SUCCESS(Status))
            {
                if(Object != nullptr)
                    ObDereferenceObject(Object);

                if(HandleInfo.HandleAttributes & OBJ_PROTECT_CLOSE)
                {
                    // Prevent a user mode exception from happening when ObDuplicateObject calls NtClose on the source handle.
                    // Why doesn't our NtClose hook catch this? Because the kernel uses its own RVAs instead of going through the SSDT
                    Options &= ~DUPLICATE_CLOSE_SOURCE;
                }
            }
        }
    }

    return Undocumented::NtDuplicateObject(SourceProcessHandle, SourceHandle, TargetProcessHandle, TargetHandle, DesiredAccess, HandleAttributes, Options);
}

static NTSTATUS NTAPI HookNtQuerySystemInformation(
    IN SYSTEM_INFORMATION_CLASS SystemInformationClass,
    OUT PVOID SystemInformation,
    IN ULONG SystemInformationLength,
    OUT PULONG ReturnLength OPTIONAL)
{
    NTSTATUS ret = Undocumented::NtQuerySystemInformation(SystemInformationClass, SystemInformation, SystemInformationLength, ReturnLength);
    if(NT_SUCCESS(ret) && SystemInformation)
    {
        ULONG pid = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
        if(SystemInformationClass == SystemKernelDebuggerInformation)
        {
            if(Hider::IsHidden(pid, HideSystemDebuggerInformation))
            {
                Log("[TITANHIDE] SystemKernelDebuggerInformation by %d\r\n", pid);
                typedef struct _SYSTEM_KERNEL_DEBUGGER_INFORMATION
                {
                    BOOLEAN DebuggerEnabled;
                    BOOLEAN DebuggerNotPresent;
                } SYSTEM_KERNEL_DEBUGGER_INFORMATION, *PSYSTEM_KERNEL_DEBUGGER_INFORMATION;
                SYSTEM_KERNEL_DEBUGGER_INFORMATION* DebuggerInfo = (SYSTEM_KERNEL_DEBUGGER_INFORMATION*)SystemInformation;
                __try
                {
                    BACKUP_RETURNLENGTH();

                    DebuggerInfo->DebuggerEnabled = false;
                    DebuggerInfo->DebuggerNotPresent = true;

                    RESTORE_RETURNLENGTH();
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    ret = GetExceptionCode();
                }
            }
        }
    }
    return ret;
}

static NTSTATUS NTAPI HookNtQueryObject(
    IN HANDLE Handle OPTIONAL,
    IN OBJECT_INFORMATION_CLASS ObjectInformationClass,
    OUT PVOID ObjectInformation OPTIONAL,
    IN ULONG ObjectInformationLength,
    OUT PULONG ReturnLength OPTIONAL)
{
    NTSTATUS ret = Undocumented::NtQueryObject(Handle, ObjectInformationClass, ObjectInformation, ObjectInformationLength, ReturnLength);
    if(NT_SUCCESS(ret) && ObjectInformation)
    {
        ULONG pid = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
        UNICODE_STRING DebugObject;
        RtlInitUnicodeString(&DebugObject, L"DebugObject");
        if(ObjectInformationClass == ObjectTypeInformation && Hider::IsHidden(pid, HideDebugObject))
        {
            __try
            {
                BACKUP_RETURNLENGTH();

                OBJECT_TYPE_INFORMATION* type = (OBJECT_TYPE_INFORMATION*)ObjectInformation;
                ProbeForRead(type->TypeName.Buffer, 1, 1);
                if(RtlEqualUnicodeString(&type->TypeName, &DebugObject, FALSE)) //DebugObject
                {
                    Log("[TITANHIDE] DebugObject by %d\r\n", pid);
                    type->TotalNumberOfObjects = 0;
                    type->TotalNumberOfHandles = 0;
                }

                RESTORE_RETURNLENGTH();
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                ret = GetExceptionCode();
            }
        }
        else if(ObjectInformationClass == ObjectTypesInformation && Hider::IsHidden(pid, HideDebugObject))
        {
            //NCC Group Security Advisory
            __try
            {
                BACKUP_RETURNLENGTH();

                OBJECT_ALL_INFORMATION* pObjectAllInfo = (OBJECT_ALL_INFORMATION*)ObjectInformation;
                unsigned char* pObjInfoLocation = (unsigned char*)pObjectAllInfo->ObjectTypeInformation;
                unsigned int TotalObjects = pObjectAllInfo->NumberOfObjects;
                for(unsigned int i = 0; i < TotalObjects; i++)
                {
                    OBJECT_TYPE_INFORMATION* pObjectTypeInfo = (OBJECT_TYPE_INFORMATION*)pObjInfoLocation;
                    ProbeForRead(pObjectTypeInfo, 1, 1);
                    ProbeForRead(pObjectTypeInfo->TypeName.Buffer, 1, 1);
                    if(RtlEqualUnicodeString(&pObjectTypeInfo->TypeName, &DebugObject, FALSE)) //DebugObject
                    {
                        Log("[TITANHIDE] DebugObject by %d\r\n", pid);
                        pObjectTypeInfo->TotalNumberOfObjects = 0;
                        //Bug found by Aguila, thanks!
                        pObjectTypeInfo->TotalNumberOfHandles = 0;
                    }
                    pObjInfoLocation = (unsigned char*)pObjectTypeInfo->TypeName.Buffer;
                    pObjInfoLocation += pObjectTypeInfo->TypeName.MaximumLength;
                    ULONG_PTR tmp = ((ULONG_PTR)pObjInfoLocation) & -(LONG_PTR)sizeof(void*);
                    if((ULONG_PTR)tmp != (ULONG_PTR)pObjInfoLocation)
                        tmp += sizeof(void*);
                    pObjInfoLocation = ((unsigned char*)tmp);
                }

                RESTORE_RETURNLENGTH();
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                ret = GetExceptionCode();
            }
        }
    }
    return ret;
}

static NTSTATUS NTAPI HookNtQueryInformationProcess(
    IN HANDLE ProcessHandle,
    IN PROCESSINFOCLASS ProcessInformationClass,
    OUT PVOID ProcessInformation,
    IN ULONG ProcessInformationLength,
    OUT PULONG ReturnLength)
{
    NTSTATUS ret = Undocumented::NtQueryInformationProcess(ProcessHandle, ProcessInformationClass, ProcessInformation, ProcessInformationLength, ReturnLength);
    if(NT_SUCCESS(ret) &&
            ProcessInformation &&
            ProcessInformationClass != ProcessBasicInformation) //prevent stack overflow
    {
        ULONG pid = Misc::GetProcessIDFromProcessHandle(ProcessHandle);

        if(ProcessInformationClass == ProcessDebugFlags)
        {
            if(Hider::IsHidden(pid, HideProcessDebugFlags))
            {
                Log("[TITANHIDE] ProcessDebugFlags by %d\r\n", pid);
                __try
                {
                    BACKUP_RETURNLENGTH();

                    *(unsigned int*)ProcessInformation = TRUE;

                    RESTORE_RETURNLENGTH();
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    ret = GetExceptionCode();
                }
            }
        }
        else if(ProcessInformationClass == ProcessDebugPort)
        {
            if(Hider::IsHidden(pid, HideProcessDebugPort))
            {
                Log("[TITANHIDE] ProcessDebugPort by %d\r\n", pid);
                __try
                {
                    BACKUP_RETURNLENGTH();

                    *(ULONG_PTR*)ProcessInformation = 0;

                    RESTORE_RETURNLENGTH();
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    ret = GetExceptionCode();
                }
            }
        }
        else if(ProcessInformationClass == ProcessDebugObjectHandle)
        {
            // TODO: the ProcessDebugObjectHandle hook is now so convoluted that it may be better to check
            // for this information class prior to the syscall and emulate what the kernel does instead
            if(Hider::IsHidden(pid, HideProcessDebugObjectHandle))
            {
                Log("[TITANHIDE] ProcessDebugObjectHandle by %d\r\n", pid);
                HANDLE CantTouchThis = nullptr;
                BOOLEAN HandleAndReturnLengthOverlap = FALSE;

                __try
                {
                    __try
                    {
                        // This was a successful request and a valid handle was returned.
                        // That means we should close it and not just nuke it to prevent handle leaks.
                        // Copy the handle to our kernel thread stack first so that VMProte... the nice user application can't mess with it
                        CantTouchThis = *static_cast<PHANDLE>(ProcessInformation);
                    }
                    __except(EXCEPTION_EXECUTE_HANDLER)
                    {
                        NOTHING; // Do nothing; a new exception will follow
                    }

                    // https://github.com/mrexodia/TitanHide/issues/39
                    HandleAndReturnLengthOverlap = ARGUMENT_PRESENT(ReturnLength) &&
                                                   (ULONG_PTR)ReturnLength > (ULONG_PTR)ProcessInformation - sizeof(HANDLE) &&
                                                   (ULONG_PTR)ReturnLength < (ULONG_PTR)ProcessInformation + sizeof(HANDLE);

                    if(!HandleAndReturnLengthOverlap)
                    {
                        // Do not change the order of the following statements ever
                        BACKUP_RETURNLENGTH();

                        *static_cast<PHANDLE>(ProcessInformation) = nullptr;

                        RESTORE_RETURNLENGTH();
                    }

                    // Taken from : http://newgre.net/idastealth
                    ret = STATUS_PORT_NOT_SET;
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    // If an exception occured anywhere, this means the process was manipulating the output buffer on purpose during a write.
                    // Mimic the kernel here and return the exception code it caused by fucking things up for itself, rather than any other status.
                    ret = GetExceptionCode();
                }

                if(HandleAndReturnLengthOverlap)
                {
                    // Since the kernel writes the return length last (overwriting the handle), we must find the unclosed handle.
                    CantTouchThis = nullptr;
                    PEPROCESS Process;
                    const NTSTATUS Status = ObReferenceObjectByHandle(ProcessHandle,
                                            PROCESS_ALL_ACCESS,
                                            *PsProcessType,
                                            KernelMode,
                                            (PVOID*)&Process,
                                            nullptr);
                    if(NT_SUCCESS(Status))
                    {
                        const PVOID DebugPort = PsGetProcessDebugPort(Process);
                        if(DebugPort != nullptr)
                        {
                            ObFindHandleForObject(Process,
                                                  DebugPort,
                                                  nullptr,
                                                  nullptr,
                                                  &CantTouchThis);
                        }
                        ObDereferenceObject(Process);
                    }
                }

                // We passed all of the user mode buffer booby traps; now close the debug object handle. While this handle can't be
                // messed with *anymore*, that doesn't mean we didn't receive garbage when originally dereferencing it :) So test it first
                if(CantTouchThis != nullptr)
                {
                    BOOLEAN AuditOnClose;
                    const NTSTATUS HandleStatus = ObQueryObjectAuditingByHandle(CantTouchThis, &AuditOnClose);
                    if(HandleStatus != STATUS_INVALID_HANDLE)
                        ObCloseHandle(CantTouchThis, ExGetPreviousMode());
                }
            }
        }
    }
    return ret;
}

static NTSTATUS NTAPI HookNtGetContextThread(
    IN HANDLE ThreadHandle,
    IN OUT PCONTEXT Context)
{
    ULONG pid = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
    ULONG targetPid = Misc::GetProcessIDFromThreadHandle(ThreadHandle);
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    bool IsHidden = PreviousMode != KernelMode &&
                    Hider::IsHidden(pid, HideNtGetContextThread) &&
                    Hider::IsHidden(targetPid, HideNtGetContextThread);
    ULONG OriginalContextFlags = 0;
    bool DebugRegistersRequested = false;
    if(IsHidden)
    {
        Log("[TITANHIDE] NtGetContextThread by %d\r\n", pid);
        __try
        {
            ProbeForWrite(&Context->ContextFlags, sizeof(ULONG), 1);
            OriginalContextFlags = Context->ContextFlags;
            Context->ContextFlags = OriginalContextFlags & ~0x10; //CONTEXT_DEBUG_REGISTERS ^ CONTEXT_AMD64/CONTEXT_i386
            DebugRegistersRequested = Context->ContextFlags != OriginalContextFlags;
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            IsHidden = false;
        }
    }
    NTSTATUS ret = Undocumented::NtGetContextThread(ThreadHandle, Context);
    if(IsHidden)
    {
        __try
        {
            ProbeForWrite(&Context->ContextFlags, sizeof(ULONG), 1);
            Context->ContextFlags = OriginalContextFlags;

            // If debug registers were requested, zero user input
            if(DebugRegistersRequested)
            {
                Context->Dr0 = 0;
                Context->Dr1 = 0;
                Context->Dr2 = 0;
                Context->Dr3 = 0;
                Context->Dr6 = 0;
                Context->Dr7 = 0;
#ifdef _WIN64
                Context->LastBranchToRip = 0;
                Context->LastBranchFromRip = 0;
                Context->LastExceptionToRip = 0;
                Context->LastExceptionFromRip = 0;
#endif
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    return ret;
}

static NTSTATUS NTAPI SetContextThreadWithoutDebugRegisters(
    IN HANDLE ThreadHandle,
    IN PCONTEXT Context)
{
    ULONG OriginalContextFlags = 0;
    CONTEXT contextCopy;
    PCONTEXT contextPtr;
    bool copyContextSuccess;

    __try
    {
        // Copy the context, then strip flags: https://github.com/mrexodia/TitanHide/issues/44
        ProbeForRead(Context, sizeof(CONTEXT), 1);
        contextCopy = *Context;
        contextPtr = &contextCopy;

        OriginalContextFlags = contextPtr->ContextFlags;
        contextPtr->ContextFlags = OriginalContextFlags & ~0x10; //CONTEXT_DEBUG_REGISTERS ^ CONTEXT_AMD64/CONTEXT_i386
        copyContextSuccess = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        contextPtr = Context;
        copyContextSuccess = false;
    }

    NTSTATUS ret = Undocumented::NtSetContextThread(ThreadHandle, contextPtr);
    if (copyContextSuccess)
    {
        // Copy the result context back
        contextPtr->ContextFlags = OriginalContextFlags;
        __try
        {
            ProbeForWrite(Context, sizeof(CONTEXT), 1);
            *Context = *contextPtr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    return ret;
}

static NTSTATUS NTAPI HookNtSetContextThread(
    IN HANDLE ThreadHandle,
    IN PCONTEXT Context)
{
    ULONG callerPid = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();;
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    bool StripDebugRegisterFlags;

    if (PreviousMode == KernelMode)
    {
        StripDebugRegisterFlags = false;
    }
    else
    {
        ULONG targetPid = Misc::GetProcessIDFromThreadHandle(ThreadHandle);

        // To prevent other processes from erasing breakpoints, they need to be "hidden" too
        StripDebugRegisterFlags = Hider::IsHidden(targetPid, HideNtSetContextThread) && 
                                  Hider::IsHidden(callerPid, HideNtSetContextThread);
    }

    if (StripDebugRegisterFlags)
    {
        //http://lifeinhex.com/dont-touch-this-writing-good-drivers-is-really-hard
        //http://lifeinhex.com/when-software-is-good-enough
        Log("[TITANHIDE] NtSetContextThread on %d\r\n", callerPid);
        return SetContextThreadWithoutDebugRegisters(ThreadHandle, Context);
    }

    return Undocumented::NtSetContextThread(ThreadHandle, Context);
}

static NTSTATUS NTAPI HookNtSystemDebugControl(
    IN SYSDBG_COMMAND Command,
    IN PVOID InputBuffer,
    IN ULONG InputBufferLength,
    OUT PVOID OutputBuffer,
    IN ULONG OutputBufferLength,
    OUT PULONG ReturnLength)
{
    ULONG pid = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
    if(Hider::IsHidden(pid, HideNtSystemDebugControl))
    {
        Log("[TITANHIDE] NtSystemDebugControl by %d\r\n", pid);
        if(Command == SysDbgGetTriageDump)
            return STATUS_INFO_LENGTH_MISMATCH;
        return STATUS_DEBUGGER_INACTIVE;
    }
    return Undocumented::NtSystemDebugControl(Command, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength, ReturnLength);
}

int Hooks::Initialize()
{
    KeInitializeMutex(&gDebugPortMutex, 0);
    int hook_count = 0;
    hNtQueryInformationProcess = SSDT::Hook("NtQueryInformationProcess", (void*)HookNtQueryInformationProcess);
    if(hNtQueryInformationProcess)
        hook_count++;
    hNtQueryInformationThread = SSDT::Hook("NtQueryInformationThread", (void*)HookNtQueryInformationThread);
    if(hNtQueryInformationThread)
        hook_count++;
    hNtQueryObject = SSDT::Hook("NtQueryObject", (void*)HookNtQueryObject);
    if(hNtQueryObject)
        hook_count++;
    hNtQuerySystemInformation = SSDT::Hook("NtQuerySystemInformation", (void*)HookNtQuerySystemInformation);
    if(hNtQuerySystemInformation)
        hook_count++;
    hNtSetInformationThread = SSDT::Hook("NtSetInformationThread", (void*)HookNtSetInformationThread);
    if(hNtSetInformationThread)
        hook_count++;
    hNtClose = SSDT::Hook("NtClose", (void*)HookNtClose);
    if(hNtClose)
        hook_count++;
    hNtDuplicateObject = SSDT::Hook("NtDuplicateObject", (void*)HookNtDuplicateObject);
    if(hNtDuplicateObject)
        hook_count++;
    hNtGetContextThread = SSDT::Hook("NtGetContextThread", (void*)HookNtGetContextThread);
    if(hNtGetContextThread)
        hook_count++;
    hNtSetContextThread = SSDT::Hook("NtSetContextThread", (void*)HookNtSetContextThread);
    if(hNtSetContextThread)
        hook_count++;
    hNtSystemDebugControl = SSDT::Hook("NtSystemDebugControl", (void*)HookNtSystemDebugControl);
    if(hNtSystemDebugControl)
        hook_count++;
    return hook_count;
}

void Hooks::Deinitialize()
{
    SSDT::Unhook(hNtQueryInformationProcess, true);
    SSDT::Unhook(hNtQueryInformationThread, true);
    SSDT::Unhook(hNtQueryObject, true);
    SSDT::Unhook(hNtQuerySystemInformation, true);
    SSDT::Unhook(hNtSetInformationThread, true);
    SSDT::Unhook(hNtClose, true);
    SSDT::Unhook(hNtDuplicateObject, true);
    SSDT::Unhook(hNtGetContextThread, true);
    SSDT::Unhook(hNtSetContextThread, true);
    SSDT::Unhook(hNtSystemDebugControl, true);
}
