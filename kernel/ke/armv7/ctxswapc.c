/*++

Copyright (c) 2012 Minoca Corp. All Rights Reserved

Module Name:

    ctxswapc.c

Abstract:

    This module implements context swapping support routines.

Author:

    Evan Green 25-Aug-2012

Environment:

    Kernel

--*/

//
// ------------------------------------------------------------------- Includes
//

#include <minoca/kernel.h>
#include <minoca/arm.h>

//
// ---------------------------------------------------------------- Definitions
//

//
// ----------------------------------------------- Internal Function Prototypes
//

//
// ------------------------------------------------------ Data Type Definitions
//

//
// -------------------------------------------------------------------- Globals
//

//
// ------------------------------------------------------------------ Functions
//

VOID
KepArchPrepareForContextSwap (
    PPROCESSOR_BLOCK ProcessorBlock,
    PKTHREAD CurrentThread,
    PKTHREAD NewThread
    )

/*++

Routine Description:

    This routine performs any architecture specific work before context swapping
    between threads. This must be called at dispatch level.

Arguments:

    ProcessorBlock - Supplies a pointer to the processor block of the current
        processor.

    CurrentThread - Supplies a pointer to the current (old) thread.

    NewThread - Supplies a pointer to the thread that's about to be switched to.

Return Value:

    None.

--*/

{

    PULONG StorageAddress;

    ASSERT((KeGetRunLevel() == RunLevelDispatch) ||
           (ArAreInterruptsEnabled() == FALSE));

    //
    // Store the user read/write thread pointer in the upper 32-bits.
    //

    StorageAddress = ((PULONG)&(CurrentThread->ThreadPointer)) + 1;
    *StorageAddress = ArGetThreadPointerUser();

    //
    // If the thread is using the FPU, save it. Some FPU state (d8-d15) must be
    // preserved across function calls, so the FPU state cannot be abandoned by
    // virtue of simply being in a system call.
    //

    if ((CurrentThread->Flags & THREAD_FLAG_USING_FPU) != 0) {

        //
        // The FPU context could be NULL if a thread got context swapped while
        // terminating.
        //

        if (CurrentThread->FpuContext != NULL) {

            //
            // Save the FPU state if it was used this iteration. A thread may
            // be using the FPU in general but not have used it for its
            // duration on this processor, so it would be bad to save in that
            // case.
            //

            if ((CurrentThread->Flags & THREAD_FLAG_FPU_OWNER) != 0) {
                ArSaveFpuState(CurrentThread->FpuContext);
            }
        }

        CurrentThread->Flags &= ~THREAD_FLAG_FPU_OWNER;
        ArDisableFpu();
    }

    return;
}

//
// --------------------------------------------------------- Internal Functions
//

