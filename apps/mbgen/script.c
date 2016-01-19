/*++

Copyright (c) 2015 Minoca Corp. All Rights Reserved

Module Name:

    script.c

Abstract:

    This module implements Chalk script support for the Minoca Build Generator
    tool.

Author:

    Evan Green 3-Dec-2015

Environment:

    Build

--*/

//
// ------------------------------------------------------------------- Includes
//

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mbgen.h"

//
// ---------------------------------------------------------------- Definitions
//

//
// ------------------------------------------------------ Data Type Definitions
//

//
// ----------------------------------------------- Internal Function Prototypes
//

PMBGEN_SCRIPT
MbgenFindScript (
    PMBGEN_CONTEXT Context,
    PMBGEN_TARGET_SPECIFIER TargetPath
    );

VOID
MbgenDestroyScript (
    PMBGEN_SCRIPT Script
    );

//
// -------------------------------------------------------------------- Globals
//

CHALK_C_STRUCTURE_MEMBER MbgenProjectRootMembers[] = {
    {
        ChalkCString,
        "globalenv",
        offsetof(MBGEN_CONTEXT, GlobalName),
        FALSE,
        {0}
    },

    {
        ChalkCString,
        "default_target",
        offsetof(MBGEN_CONTEXT, DefaultName),
        FALSE,
        {0}
    },

    {0}
};

//
// ------------------------------------------------------------------ Functions
//

INT
MbgenLoadTargetScript (
    PMBGEN_CONTEXT Context,
    PSTR TargetSpecifier,
    MBGEN_SCRIPT_ORDER Order,
    PMBGEN_SCRIPT *Script
    )

/*++

Routine Description:

    This routine loads the script corresponding to the given target specifier
    string.

Arguments:

    Context - Supplies a pointer to the application context.

    TargetSpecifier - Supplies a pointer to the target specifier string.

    Order - Supplies the order to apply to the script.

    Script - Supplies a pointer where a pointer to the loaded or found script
        will be returned on success.

Return Value:

    0 on success.

    Non-zero on failure.

--*/

{

    INT Status;
    MBGEN_TARGET_SPECIFIER Target;

    memset(&Target, 0, sizeof(MBGEN_TARGET_SPECIFIER));
    Status = MbgenParseTargetSpecifier(Context,
                                       TargetSpecifier,
                                       MbgenSourceTree,
                                       NULL,
                                       &Target);

    if (Status != 0) {
        goto LoadTargetScriptEnd;
    }

    //
    // Reset the context to give each run a fresh slate.
    //

    Status = ChalkClearInterpreter(&(Context->Interpreter));
    if (Status != 0) {
        goto LoadTargetScriptEnd;
    }

    Status = MbgenAddChalkBuiltins(Context);
    if (Status != 0) {
        goto LoadTargetScriptEnd;
    }

    //
    // Execute the command line arguments and global contents.
    //

    Status = ChalkExecuteDeferredScripts(&(Context->Interpreter),
                                         MbgenScriptOrderCommandLine);

    if (Status != 0) {
        goto LoadTargetScriptEnd;
    }

    if (Order > MbgenScriptOrderGlobal) {
        Status = ChalkExecuteDeferredScripts(&(Context->Interpreter),
                                             MbgenScriptOrderGlobal);

        if (Status != 0) {
            goto LoadTargetScriptEnd;
        }
    }

    Status = MbgenLoadScript(Context, Order, &Target, Script);
    if (Status != 0) {
        goto LoadTargetScriptEnd;
    }

LoadTargetScriptEnd:
    if (Target.Path != NULL) {
        free(Target.Path);
    }

    return Status;
}

INT
MbgenLoadProjectRoot (
    PMBGEN_CONTEXT Context
    )

/*++

Routine Description:

    This routine loads and interprets the project root script.

Arguments:

    Context - Supplies a pointer to the application context.

Return Value:

    0 on success.

    Non-zero on failure.

--*/

{

    INT Status;
    MBGEN_TARGET_SPECIFIER TargetPath;

    memset(&TargetPath, 0, sizeof(MBGEN_TARGET_SPECIFIER));
    TargetPath.Root = MbgenSourceTree;
    Status = MbgenLoadScript(Context,
                             MbgenScriptOrderProjectRoot,
                             &TargetPath,
                             NULL);

    if (Status != 0) {
        return Status;
    }

    if ((Context->Options & MBGEN_OPTION_DEBUG) != 0) {
        printf("Global context after project root:\n");
        ChalkPrintObject(Context->Interpreter.Global.Dict, 0);
        printf("\n");
    }

    //
    // Read the important variables into the context structure.
    //

    Status = ChalkConvertDictToStructure(&(Context->Interpreter),
                                         Context->Interpreter.Global.Dict,
                                         MbgenProjectRootMembers,
                                         Context);

    if (Status != 0) {
        return Status;
    }

    if (Context->DefaultName == NULL) {
        Context->DefaultName = strdup(MBGEN_DEFAULT_NAME);
        if (Context->DefaultName == NULL) {
            return ENOMEM;
        }
    }

    //
    // Load up the global environment script to get it loaded with the correct
    // order.
    //

    if (Context->GlobalName != NULL) {
        Status = MbgenLoadTargetScript(Context,
                                       Context->GlobalName,
                                       MbgenScriptOrderGlobal,
                                       NULL);

        if (Status != 0) {
            fprintf(stderr,
                    "Error: Failed to load global environment script.\n");

            return Status;
        }
    }

    //
    // Load the default target.
    //

    if (Context->DefaultName != NULL) {
        Status = MbgenLoadTargetScript(Context,
                                       Context->DefaultName,
                                       MbgenScriptOrderTarget,
                                       NULL);

        if (Status != 0) {
            fprintf(stderr, "Error: Failed to load default target.\n");
            return Status;
        }
    }

    return 0;
}

INT
MbgenLoadScript (
    PMBGEN_CONTEXT Context,
    MBGEN_SCRIPT_ORDER Order,
    PMBGEN_TARGET_SPECIFIER TargetPath,
    PMBGEN_SCRIPT *FinalScript
    )

/*++

Routine Description:

    This routine loads and interprets a given target path. If the script
    containing the given target path is already loaded, then this is a no-op.

Arguments:

    Context - Supplies a pointer to the application context.

    Order - Supplies the order to apply to the script.

    TargetPath - Supplies a pointer to the target path to load. The target
        name is ignored, only the root and path are observed.

    FinalScript - Supplies a pointer where a pointer to the newly loaded or
        found script will be returned on success.

Return Value:

    0 on success.

    Non-zero on failure.

--*/

{

    ULONG ExecuteOrder;
    FILE *File;
    PSTR FinalPath;
    PMBGEN_SCRIPT Script;
    struct stat Stat;
    INT Status;
    PSTR Tree;

    File = NULL;
    FinalPath = NULL;
    Script = NULL;
    if (Order == MbgenScriptOrderProjectRoot) {

        assert((TargetPath->Root == MbgenSourceTree) &&
               (LIST_EMPTY(&(Context->ScriptList))));

        FinalPath = MbgenAppendPaths(Context->SourceRoot,
                                     Context->ProjectFileName);

    } else {
        if (MbgenFindScript(Context, TargetPath) != NULL) {
            return 0;
        }

        Tree = MbgenPathForTree(Context, TargetPath->Root);
        if (Order == MbgenScriptOrderGlobal) {
            FinalPath = MbgenAppendPaths(Tree, TargetPath->Path);

        } else {
            FinalPath = MbgenAppendPaths3(Tree,
                                          TargetPath->Path,
                                          Context->BuildFileName);
        }
    }

    if (FinalPath == NULL) {
        return ENOMEM;
    }

    if ((Context->Options & MBGEN_OPTION_VERBOSE) != 0) {
        printf("Loading Script %s\n", FinalPath);
    }

    //
    // Load the script.
    //

    File = fopen(FinalPath, "rb");
    if (File == NULL) {
        Status = errno;
        fprintf(stderr,
                "Unable to open %s: %s\n",
                FinalPath,
                strerror(Status));

        goto LoadScriptEnd;
    }

    if (stat(FinalPath, &Stat) != 0) {
        Status = errno;
        goto LoadScriptEnd;
    }

    Script = malloc(sizeof(MBGEN_SCRIPT));
    if (Script == NULL) {
        Status = errno;
        goto LoadScriptEnd;
    }

    memset(Script, 0, sizeof(MBGEN_SCRIPT));
    INITIALIZE_LIST_HEAD(&(Script->TargetList));
    Script->Root = TargetPath->Root;
    Script->CompletePath = FinalPath;
    if (TargetPath->Path != NULL) {
        Script->Path = strdup(TargetPath->Path);
        if (Script->Path == NULL) {
            Status = errno;
            goto LoadScriptEnd;
        }
    }

    Script->Size = Stat.st_size;
    Script->Script = malloc(Script->Size + 1);
    if (Script->Script == NULL) {
        Status = errno;
        goto LoadScriptEnd;
    }

    if (fread(Script->Script, 1, Script->Size, File) != Script->Size) {
        fprintf(stderr, "Read failure");
        Status = errno;
        goto LoadScriptEnd;
    }

    Script->Script[Script->Size] = '\0';

    //
    // Execute the script. If it's a target script, execute now to get the
    // return value.
    //

    ExecuteOrder = Order;
    if (Order == MbgenScriptOrderTarget) {
        ExecuteOrder = 0;
    }

    Status = ChalkLoadScriptBuffer(&(Context->Interpreter),
                                   FinalPath,
                                   Script->Script,
                                   Script->Size,
                                   ExecuteOrder,
                                   &(Script->Result));

    if (Status != 0) {
        fprintf(stderr,
                "Error: Failed to execute script %s: %s.\n",
                FinalPath,
                strerror(Status));

        goto LoadScriptEnd;
    }

    if (ExecuteOrder != 0) {
        Status = ChalkExecuteDeferredScripts(&(Context->Interpreter), Order);
        if (Status != 0) {
            goto LoadScriptEnd;
        }

        INSERT_BEFORE(&(Script->ListEntry), &(Context->ScriptList));

    } else {
        INSERT_BEFORE(&(Script->ListEntry), &(Context->ScriptList));
        Status = MbgenParseScriptResults(Context, Script);
        if (Status != 0) {
            goto LoadScriptEnd;
        }
    }

    Status = 0;

LoadScriptEnd:
    if ((FinalPath != NULL) && (Script == NULL)) {
        free(FinalPath);
    }

    if (File != NULL) {
        fclose(File);
    }

    if (Status != 0) {
        if (Script != NULL) {
            if (Script->ListEntry.Next != NULL) {
                LIST_REMOVE(&(Script->ListEntry));
            }

            MbgenDestroyScript(Script);
            Script = NULL;
        }
    }

    if (FinalScript != NULL) {
        *FinalScript = Script;
    }

    return Status;
}

VOID
MbgenDestroyAllScripts (
    PMBGEN_CONTEXT Context
    )

/*++

Routine Description:

    This routine destroys all scripts in the application context.

Arguments:

    Context - Supplies a pointer to the application context.

Return Value:

    0 on success.

    Non-zero on failure.

--*/

{

    PMBGEN_SCRIPT Script;

    while (!LIST_EMPTY(&(Context->ScriptList))) {
        Script = LIST_VALUE(Context->ScriptList.Next,
                            MBGEN_SCRIPT,
                            ListEntry);

        LIST_REMOVE(&(Script->ListEntry));
        MbgenDestroyScript(Script);
    }

    return;
}

//
// --------------------------------------------------------- Internal Functions
//

PMBGEN_SCRIPT
MbgenFindScript (
    PMBGEN_CONTEXT Context,
    PMBGEN_TARGET_SPECIFIER TargetPath
    )

/*++

Routine Description:

    This routine searches for an already loaded script matching the given
    target root and directory path.

Arguments:

    Context - Supplies a pointer to the application context.

    TargetPath - Supplies a pointer to the target path search for. The target
        name and toolchain are ignored, only the root and path are observed.

Return Value:

    0 on success.

    Non-zero on failure.

--*/

{

    PLIST_ENTRY CurrentEntry;
    PMBGEN_SCRIPT Script;

    CurrentEntry = Context->ScriptList.Next;
    while (CurrentEntry != &(Context->ScriptList)) {
        Script = LIST_VALUE(CurrentEntry, MBGEN_SCRIPT, ListEntry);
        CurrentEntry = CurrentEntry->Next;
        if ((Script->Root == TargetPath->Root) &&
            (Script->Path != NULL) &&
            (strcmp(Script->Path, TargetPath->Path) == 0)) {

            return Script;
        }
    }

    return NULL;
}

VOID
MbgenDestroyScript (
    PMBGEN_SCRIPT Script
    )

/*++

Routine Description:

    This routine destroys a script object.

Arguments:

    Script - Supplies a pointer to the script.

Return Value:

    None.

--*/

{

    PMBGEN_TARGET Target;

    if (Script->Script != NULL) {
        free(Script->Script);
    }

    if (Script->Path != NULL) {
        free(Script->Path);
    }

    if (Script->CompletePath != NULL) {
        free(Script->CompletePath);
    }

    if (Script->Result != NULL) {
        ChalkObjectReleaseReference(Script->Result);
    }

    while (!LIST_EMPTY(&(Script->TargetList))) {
        Target = LIST_VALUE(Script->TargetList.Next, MBGEN_TARGET, ListEntry);
        LIST_REMOVE(&(Target->ListEntry));
        Script->TargetCount -= 1;
        MbgenDestroyTarget(Target);
    }

    assert(Script->TargetCount == 0);

    free(Script);
    return;
}

