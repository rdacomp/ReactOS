/*
 * PROJECT:     FreeLoader
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Windows-compatible NT OS Loader.
 * COPYRIGHT:   Copyright 2006-2019 Aleksey Bragin <aleksey@reactos.org>
 */

#include <freeldr.h>
#include <ndk/ldrtypes.h>
#include "winldr.h"
#include "registry.h"

#include <debug.h>
DBG_DEFAULT_CHANNEL(WINDOWS);

// FIXME: Find a better way to retrieve ARC disk information
extern ULONG reactos_disk_count;
extern ARC_DISK_SIGNATURE_EX reactos_arc_disk_info[];

extern ULONG LoaderPagesSpanned;
extern BOOLEAN AcpiPresent;

extern HEADLESS_LOADER_BLOCK LoaderRedirectionInformation;
extern BOOLEAN WinLdrTerminalConnected;
extern void WinLdrSetupEms(IN PCHAR BootOptions);

PLOADER_SYSTEM_BLOCK WinLdrSystemBlock;

// debug stuff
VOID DumpMemoryAllocMap(VOID);

// Init "phase 0"
VOID
AllocateAndInitLPB(PLOADER_PARAMETER_BLOCK *OutLoaderBlock)
{
    PLOADER_PARAMETER_BLOCK LoaderBlock;

    /* Allocate and zero-init the LPB */
    WinLdrSystemBlock = MmAllocateMemoryWithType(sizeof(LOADER_SYSTEM_BLOCK),
                                                 LoaderSystemBlock);
    if (WinLdrSystemBlock == NULL)
    {
        UiMessageBox("Failed to allocate memory for system block!");
        return;
    }

    RtlZeroMemory(WinLdrSystemBlock, sizeof(LOADER_SYSTEM_BLOCK));

    LoaderBlock = &WinLdrSystemBlock->LoaderBlock;
    LoaderBlock->NlsData = &WinLdrSystemBlock->NlsDataBlock;

    /* Init three critical lists, used right away */
    InitializeListHead(&LoaderBlock->LoadOrderListHead);
    InitializeListHead(&LoaderBlock->MemoryDescriptorListHead);
    InitializeListHead(&LoaderBlock->BootDriverListHead);

    *OutLoaderBlock = LoaderBlock;
}

// Init "phase 1"
VOID
WinLdrInitializePhase1(PLOADER_PARAMETER_BLOCK LoaderBlock,
                       PCSTR Options,
                       PCSTR SystemRoot,
                       PCSTR BootPath,
                       USHORT VersionToBoot)
{
    /* Examples of correct options and paths */
    //CHAR    Options[] = "/DEBUGPORT=COM1 /BAUDRATE=115200";
    //CHAR    Options[] = "/NODEBUG";
    //CHAR    SystemRoot[] = "\\WINNT\\";
    //CHAR    ArcBoot[] = "multi(0)disk(0)rdisk(0)partition(1)";

    PSTR  LoadOptions, NewLoadOptions;
    CHAR  HalPath[] = "\\";
    CHAR  ArcBoot[MAX_PATH+1];
    CHAR  MiscFiles[MAX_PATH+1];
    ULONG i;
    ULONG_PTR PathSeparator;
    PLOADER_PARAMETER_EXTENSION Extension;

    /* Construct SystemRoot and ArcBoot from SystemPath */
    PathSeparator = strstr(BootPath, "\\") - BootPath;
    RtlStringCbCopyNA(ArcBoot, sizeof(ArcBoot), BootPath, PathSeparator);

    TRACE("ArcBoot: '%s'\n", ArcBoot);
    TRACE("SystemRoot: '%s'\n", SystemRoot);
    TRACE("Options: '%s'\n", Options);

    /* Fill ARC BootDevice */
    LoaderBlock->ArcBootDeviceName = WinLdrSystemBlock->ArcBootDeviceName;
    RtlStringCbCopyA(LoaderBlock->ArcBootDeviceName, sizeof(WinLdrSystemBlock->ArcBootDeviceName), ArcBoot);
    LoaderBlock->ArcBootDeviceName = PaToVa(LoaderBlock->ArcBootDeviceName);

//
// IMPROVE!!
// SetupBlock->ArcSetupDeviceName must be the path to the setup **SOURCE**,
// and not the setup boot path. Indeed they may differ!!
//
    /* If we have a setup block, adjust also its ARC path */
    if (LoaderBlock->SetupLdrBlock)
    {
        PSETUP_LOADER_BLOCK SetupBlock = LoaderBlock->SetupLdrBlock;

        /* Matches ArcBoot path */
        SetupBlock->ArcSetupDeviceName = WinLdrSystemBlock->ArcBootDeviceName;
        SetupBlock->ArcSetupDeviceName = PaToVa(SetupBlock->ArcSetupDeviceName);

        /* Note: LoaderBlock->SetupLdrBlock is PaToVa'ed at the end of this function */
    }

    /* Fill ARC HalDevice, it matches ArcBoot path */
    LoaderBlock->ArcHalDeviceName = WinLdrSystemBlock->ArcBootDeviceName;
    LoaderBlock->ArcHalDeviceName = PaToVa(LoaderBlock->ArcHalDeviceName);

    /* Fill SystemRoot */
    LoaderBlock->NtBootPathName = WinLdrSystemBlock->NtBootPathName;
    RtlStringCbCopyA(LoaderBlock->NtBootPathName, sizeof(WinLdrSystemBlock->NtBootPathName), SystemRoot);
    LoaderBlock->NtBootPathName = PaToVa(LoaderBlock->NtBootPathName);

    /* Fill NtHalPathName */
    LoaderBlock->NtHalPathName = WinLdrSystemBlock->NtHalPathName;
    RtlStringCbCopyA(LoaderBlock->NtHalPathName, sizeof(WinLdrSystemBlock->NtHalPathName), HalPath);
    LoaderBlock->NtHalPathName = PaToVa(LoaderBlock->NtHalPathName);

    /* Fill LoadOptions and strip the '/' switch symbol in front of each option */
    NewLoadOptions = LoadOptions = LoaderBlock->LoadOptions = WinLdrSystemBlock->LoadOptions;
    RtlStringCbCopyA(LoaderBlock->LoadOptions, sizeof(WinLdrSystemBlock->LoadOptions), Options);

    do
    {
        while (*LoadOptions == '/')
            ++LoadOptions;

        *NewLoadOptions++ = *LoadOptions;
    } while (*LoadOptions++);

    LoaderBlock->LoadOptions = PaToVa(LoaderBlock->LoadOptions);

    /* ARC devices */
    LoaderBlock->ArcDiskInformation = &WinLdrSystemBlock->ArcDiskInformation;
    InitializeListHead(&LoaderBlock->ArcDiskInformation->DiskSignatureListHead);

    /* Convert ARC disk information from freeldr to a correct format */
    for (i = 0; i < reactos_disk_count; i++)
    {
        PARC_DISK_SIGNATURE_EX ArcDiskSig;

        /* Allocate the ARC structure */
        ArcDiskSig = FrLdrHeapAlloc(sizeof(ARC_DISK_SIGNATURE_EX), 'giSD');

        /* Copy the data over */
        RtlCopyMemory(ArcDiskSig, &reactos_arc_disk_info[i], sizeof(ARC_DISK_SIGNATURE_EX));

        /* Set the ARC Name pointer */
        ArcDiskSig->DiskSignature.ArcName = PaToVa(ArcDiskSig->ArcName);

        /* Insert into the list */
        InsertTailList(&LoaderBlock->ArcDiskInformation->DiskSignatureListHead,
                       &ArcDiskSig->DiskSignature.ListEntry);
    }

    /* Convert all list's to Virtual address */

    /* Convert the ArcDisks list to virtual address */
    List_PaToVa(&LoaderBlock->ArcDiskInformation->DiskSignatureListHead);
    LoaderBlock->ArcDiskInformation = PaToVa(LoaderBlock->ArcDiskInformation);

    /* Convert configuration entries to VA */
    ConvertConfigToVA(LoaderBlock->ConfigurationRoot);
    LoaderBlock->ConfigurationRoot = PaToVa(LoaderBlock->ConfigurationRoot);

    /* Convert all DTE into virtual addresses */
    List_PaToVa(&LoaderBlock->LoadOrderListHead);

    /* This one will be converted right before switching to virtual paging mode */
    //List_PaToVa(&LoaderBlock->MemoryDescriptorListHead);

    /* Convert list of boot drivers */
    List_PaToVa(&LoaderBlock->BootDriverListHead);

    /* Initialize Extension now */
    Extension = &WinLdrSystemBlock->Extension;
    Extension->Size = sizeof(LOADER_PARAMETER_EXTENSION);
    Extension->MajorVersion = (VersionToBoot & 0xFF00) >> 8;
    Extension->MinorVersion = VersionToBoot & 0xFF;
    Extension->Profile.Status = 2;

    /* Check if FreeLdr detected a ACPI table */
    if (AcpiPresent)
    {
        /* Set the pointer to something for compatibility */
        Extension->AcpiTable = (PVOID)1;
        // FIXME: Extension->AcpiTableSize;
    }

#ifdef _M_IX86
    /* Set headless block pointer */
    if (WinLdrTerminalConnected)
    {
        Extension->HeadlessLoaderBlock = &WinLdrSystemBlock->HeadlessLoaderBlock;
        RtlCopyMemory(Extension->HeadlessLoaderBlock,
                      &LoaderRedirectionInformation,
                      sizeof(HEADLESS_LOADER_BLOCK));
        Extension->HeadlessLoaderBlock = PaToVa(Extension->HeadlessLoaderBlock);
    }
#endif
    /* Load drivers database */
    RtlStringCbCopyA(MiscFiles, sizeof(MiscFiles), BootPath);
    RtlStringCbCatA(MiscFiles, sizeof(MiscFiles), "AppPatch\\drvmain.sdb");
    Extension->DrvDBImage = PaToVa(WinLdrLoadModule(MiscFiles,
                                                    &Extension->DrvDBSize,
                                                    LoaderRegistryData));

    /* Convert extension and setup block pointers */
    LoaderBlock->Extension = PaToVa(Extension);

    if (LoaderBlock->SetupLdrBlock)
        LoaderBlock->SetupLdrBlock = PaToVa(LoaderBlock->SetupLdrBlock);

    TRACE("WinLdrInitializePhase1() completed\n");
}

static BOOLEAN
WinLdrLoadDeviceDriver(PLIST_ENTRY LoadOrderListHead,
                       PCSTR BootPath,
                       PUNICODE_STRING FilePath,
                       ULONG Flags,
                       PLDR_DATA_TABLE_ENTRY *DriverDTE)
{
    CHAR FullPath[1024];
    CHAR DriverPath[1024];
    CHAR DllName[1024];
    PCHAR DriverNamePos;
    BOOLEAN Success;
    PVOID DriverBase = NULL;

    // Separate the path to file name and directory path
    RtlStringCbPrintfA(DriverPath, sizeof(DriverPath), "%wZ", FilePath);
    DriverNamePos = strrchr(DriverPath, '\\');
    if (DriverNamePos != NULL)
    {
        // Copy the name
        RtlStringCbCopyA(DllName, sizeof(DllName), DriverNamePos+1);

        // Cut out the name from the path
        *(DriverNamePos+1) = ANSI_NULL;
    }
    else
    {
        // There is no directory in the path
        RtlStringCbCopyA(DllName, sizeof(DllName), DriverPath);
        *DriverPath = ANSI_NULL;
    }

    TRACE("DriverPath: '%s', DllName: '%s', LPB\n", DriverPath, DllName);

    // Check if driver is already loaded
    Success = WinLdrCheckForLoadedDll(LoadOrderListHead, DllName, DriverDTE);
    if (Success)
    {
        // We've got the pointer to its DTE, just return success
        return TRUE;
    }

    // It's not loaded, we have to load it
    RtlStringCbPrintfA(FullPath, sizeof(FullPath), "%s%wZ", BootPath, FilePath);
    Success = WinLdrLoadImage(FullPath, LoaderBootDriver, &DriverBase);
    if (!Success)
        return FALSE;

    // Allocate a DTE for it
    Success = WinLdrAllocateDataTableEntry(LoadOrderListHead, DllName, DllName, DriverBase, DriverDTE);
    if (!Success)
    {
        ERR("WinLdrAllocateDataTableEntry() failed\n");
        return FALSE;
    }

    // Modify any flags, if needed
    (*DriverDTE)->Flags |= Flags;

    // Look for any dependencies it may have, and load them too
    RtlStringCbPrintfA(FullPath, sizeof(FullPath), "%s%s", BootPath, DriverPath);
    Success = WinLdrScanImportDescriptorTable(LoadOrderListHead, FullPath, *DriverDTE);
    if (!Success)
    {
        ERR("WinLdrScanImportDescriptorTable() failed for %s\n", FullPath);
        return FALSE;
    }

    return TRUE;
}

BOOLEAN
WinLdrLoadBootDrivers(PLOADER_PARAMETER_BLOCK LoaderBlock,
                      PCSTR BootPath)
{
    PLIST_ENTRY NextBd;
    PBOOT_DRIVER_LIST_ENTRY BootDriver;
    BOOLEAN Success;
    BOOLEAN ret = TRUE;

    // Walk through the boot drivers list
    NextBd = LoaderBlock->BootDriverListHead.Flink;

    while (NextBd != &LoaderBlock->BootDriverListHead)
    {
        BootDriver = CONTAINING_RECORD(NextBd, BOOT_DRIVER_LIST_ENTRY, Link);

        TRACE("BootDriver %wZ DTE %08X RegPath: %wZ\n", &BootDriver->FilePath,
            BootDriver->LdrEntry, &BootDriver->RegistryPath);

        // Paths are relative (FIXME: Are they always relative?)

        // Load it
        Success = WinLdrLoadDeviceDriver(&LoaderBlock->LoadOrderListHead,
                                         BootPath,
                                         &BootDriver->FilePath,
                                         0,
                                         &BootDriver->LdrEntry);

        if (Success)
        {
            // Convert the RegistryPath and DTE addresses to VA since we are not going to use it anymore
            BootDriver->RegistryPath.Buffer = PaToVa(BootDriver->RegistryPath.Buffer);
            BootDriver->FilePath.Buffer = PaToVa(BootDriver->FilePath.Buffer);
            BootDriver->LdrEntry = PaToVa(BootDriver->LdrEntry);
        }
        else
        {
            // Loading failed - cry loudly
            ERR("Can't load boot driver '%wZ'!\n", &BootDriver->FilePath);
            UiMessageBox("Can't load boot driver '%wZ'!", &BootDriver->FilePath);
            ret = FALSE;

            // Remove it from the list and try to continue
            RemoveEntryList(NextBd);
        }

        NextBd = BootDriver->Link.Flink;
    }

    return ret;
}

PVOID
WinLdrLoadModule(PCSTR ModuleName,
                 PULONG Size,
                 TYPE_OF_MEMORY MemoryType)
{
    ULONG FileId;
    PVOID PhysicalBase;
    FILEINFORMATION FileInfo;
    ULONG FileSize;
    ARC_STATUS Status;
    ULONG BytesRead;

    //CHAR ProgressString[256];

    /* Inform user we are loading files */
    //UiDrawBackdrop();
    //RtlStringCbPrintfA(ProgressString, sizeof(ProgressString), "Loading %s...", FileName);
    //UiDrawProgressBarCenter(1, 100, ProgressString);

    TRACE("Loading module %s\n", ModuleName);
    *Size = 0;

    /* Open the image file */
    Status = ArcOpen((PCHAR)ModuleName, OpenReadOnly, &FileId);
    if (Status != ESUCCESS)
    {
        /* In case of errors, we just return, without complaining to the user */
        WARN("Error while opening '%s', Status: %u\n", ModuleName, Status);
        return NULL;
    }

    /* Get this file's size */
    Status = ArcGetFileInformation(FileId, &FileInfo);
    if (Status != ESUCCESS)
    {
        ArcClose(FileId);
        return NULL;
    }
    FileSize = FileInfo.EndingAddress.LowPart;
    *Size = FileSize;

    /* Allocate memory */
    PhysicalBase = MmAllocateMemoryWithType(FileSize, MemoryType);
    if (PhysicalBase == NULL)
    {
        ArcClose(FileId);
        return NULL;
    }

    /* Load whole file */
    Status = ArcRead(FileId, PhysicalBase, FileSize, &BytesRead);
    ArcClose(FileId);
    if (Status != ESUCCESS)
    {
        WARN("Error while reading '%s', Status: %u\n", ModuleName, Status);
        return NULL;
    }

    TRACE("Loaded %s at 0x%x with size 0x%x\n", ModuleName, PhysicalBase, FileSize);

    return PhysicalBase;
}

USHORT
WinLdrDetectVersion(VOID)
{
    LONG rc;
    HKEY hKey;

    rc = RegOpenKey(
        NULL,
        L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Terminal Server",
        &hKey);
    if (rc != ERROR_SUCCESS)
    {
        // Key doesn't exist; assume NT 4.0
        return _WIN32_WINNT_NT4;
    }

    // We may here want to read the value of ProductVersion
    return _WIN32_WINNT_WS03;
}

static
BOOLEAN
LoadModule(
    IN OUT PLOADER_PARAMETER_BLOCK LoaderBlock,
    IN PCCH Path,
    IN PCCH File,
    IN PCCH ImportName, // BaseDllName
    IN TYPE_OF_MEMORY MemoryType,
    OUT PLDR_DATA_TABLE_ENTRY *Dte,
    IN ULONG Percentage)
{
    BOOLEAN Success;
    CHAR FullFileName[MAX_PATH];
    CHAR ProgressString[256];
    PVOID BaseAddress = NULL;

    UiDrawBackdrop();
    RtlStringCbPrintfA(ProgressString, sizeof(ProgressString), "Loading %s...", File);
    UiDrawProgressBarCenter(Percentage, 100, ProgressString);

    RtlStringCbCopyA(FullFileName, sizeof(FullFileName), Path);
    RtlStringCbCatA(FullFileName, sizeof(FullFileName), File);

    Success = WinLdrLoadImage(FullFileName, MemoryType, &BaseAddress);
    if (!Success)
    {
        TRACE("Loading %s failed\n", File);
        return FALSE;
    }
    TRACE("%s loaded successfully at %p\n", File, BaseAddress);

    /*
     * Cheat about the base DLL name if we are loading
     * the Kernel Debugger Transport DLL, to make the
     * PE loader happy.
     */
    Success = WinLdrAllocateDataTableEntry(&LoaderBlock->LoadOrderListHead,
                                           ImportName,
                                           FullFileName,
                                           BaseAddress,
                                           Dte);

    return Success;
}

static
BOOLEAN
LoadWindowsCore(IN USHORT OperatingSystemVersion,
                IN OUT PLOADER_PARAMETER_BLOCK LoaderBlock,
                IN PCSTR BootOptions,
                IN PCSTR BootPath,
                IN OUT PLDR_DATA_TABLE_ENTRY* KernelDTE)
{
    BOOLEAN Success;
    PCSTR Options;
    CHAR DirPath[MAX_PATH];
    CHAR KernelFileName[MAX_PATH];
    CHAR HalFileName[MAX_PATH];
    CHAR KdTransportDllName[MAX_PATH];
    PLDR_DATA_TABLE_ENTRY HalDTE, KdComDTE = NULL;

    if (!KernelDTE) return FALSE;

    /* Initialize SystemRoot\System32 path */
    RtlStringCbCopyA(DirPath, sizeof(DirPath), BootPath);
    RtlStringCbCatA(DirPath, sizeof(DirPath), "system32\\");

    //
    // TODO: Parse also the separate INI values "Kernel=" and "Hal="
    //

    /* Default KERNEL and HAL file names */
    RtlStringCbCopyA(KernelFileName, sizeof(KernelFileName), "ntoskrnl.exe");
    RtlStringCbCopyA(HalFileName   , sizeof(HalFileName)   , "hal.dll");

    /* Find any /KERNEL= or /HAL= switch in the boot options */
    Options = BootOptions;
    while (Options)
    {
        /* Skip possible initial whitespace */
        Options += strspn(Options, " \t");

        /* Check whether a new option starts and it is either KERNEL or HAL */
        if (*Options != '/' || (++Options,
            !(_strnicmp(Options, "KERNEL=", 7) == 0 ||
              _strnicmp(Options, "HAL=",    4) == 0)) )
        {
            /* Search for another whitespace */
            Options = strpbrk(Options, " \t");
            continue;
        }
        else
        {
            size_t i = strcspn(Options, " \t"); /* Skip whitespace */
            if (i == 0)
            {
                /* Use the default values */
                break;
            }

            /* We have found either KERNEL or HAL options */
            if (_strnicmp(Options, "KERNEL=", 7) == 0)
            {
                Options += 7; i -= 7;
                RtlStringCbCopyNA(KernelFileName, sizeof(KernelFileName), Options, i);
                _strupr(KernelFileName);
            }
            else if (_strnicmp(Options, "HAL=", 4) == 0)
            {
                Options += 4; i -= 4;
                RtlStringCbCopyNA(HalFileName, sizeof(HalFileName), Options, i);
                _strupr(HalFileName);
            }
        }
    }

    TRACE("Kernel file = '%s' ; HAL file = '%s'\n", KernelFileName, HalFileName);

    /* Load the Kernel */
    LoadModule(LoaderBlock, DirPath, KernelFileName, "ntoskrnl.exe", LoaderSystemCode, KernelDTE, 30);

    /* Load the HAL */
    LoadModule(LoaderBlock, DirPath, HalFileName, "hal.dll", LoaderHalCode, &HalDTE, 45);

    /* Load the Kernel Debugger Transport DLL */
    if (OperatingSystemVersion > _WIN32_WINNT_WIN2K)
    {
        /*
         * According to http://www.nynaeve.net/?p=173 :
         * "[...] Another enhancement that could be done Microsoft-side would be
         * a better interface for replacing KD transport modules. Right now, due
         * to the fact that ntoskrnl is static linked to KDCOM.DLL, the OS loader
         * has a hardcoded hack that interprets the KD type in the OS loader options,
         * loads one of the (hardcoded filenames) "kdcom.dll", "kd1394.dll", or
         * "kdusb2.dll" modules, and inserts them into the loaded module list under
         * the name "kdcom.dll". [...]"
         */

        /*
         * This loop replaces a dumb call to strstr(..., "DEBUGPORT=").
         * Indeed I want it to be case-insensitive to allow "debugport="
         * or "DeBuGpOrT=" or... , and I don't want it to match malformed
         * command-line options, such as:
         *
         * "...foo DEBUGPORT=xxx bar..."
         * "...foo/DEBUGPORT=xxx bar..."
         * "...foo/DEBUGPORT=bar..."
         *
         * i.e. the "DEBUGPORT=" switch must start with a slash and be separated
         * from the rest by whitespace, unless it begins the command-line, e.g.:
         *
         * "/DEBUGPORT=COM1 foo...bar..."
         * "...foo /DEBUGPORT=USB bar..."
         * or:
         * "...foo /DEBUGPORT= bar..."
         * (in that case, we default the port to COM).
         */
        Options = BootOptions;
        while (Options)
        {
            /* Skip possible initial whitespace */
            Options += strspn(Options, " \t");

            /* Check whether a new option starts and it is the DEBUGPORT one */
            if (*Options != '/' || _strnicmp(++Options, "DEBUGPORT=", 10) != 0)
            {
                /* Search for another whitespace */
                Options = strpbrk(Options, " \t");
                continue;
            }
            else
            {
                /* We found the DEBUGPORT option. Move to the port name. */
                Options += 10;
                break;
            }
        }

        if (Options)
        {
            /*
             * We have found the DEBUGPORT option. Parse the port name.
             * Format: /DEBUGPORT=COM1 or /DEBUGPORT=FILE:\Device\HarddiskX\PartitionY\debug.log or /DEBUGPORT=FOO
             * If we only have /DEBUGPORT= (i.e. without any port name), defaults it to "COM".
             */
            RtlStringCbCopyA(KdTransportDllName, sizeof(KdTransportDllName), "KD");
            if (_strnicmp(Options, "COM", 3) == 0 && '0' <= Options[3] && Options[3] <= '9')
            {
                RtlStringCbCatNA(KdTransportDllName, sizeof(KdTransportDllName), Options, 3);
            }
            else
            {
                size_t i = strcspn(Options, " \t:"); /* Skip valid separators: whitespace or colon */
                if (i == 0)
                    RtlStringCbCatA(KdTransportDllName, sizeof(KdTransportDllName), "COM");
                else
                    RtlStringCbCatNA(KdTransportDllName, sizeof(KdTransportDllName), Options, i);
            }
            RtlStringCbCatA(KdTransportDllName, sizeof(KdTransportDllName), ".DLL");
            _strupr(KdTransportDllName);

            /*
             * Load the transport DLL. Override the base DLL name of the
             * loaded transport DLL to the default "KDCOM.DLL" name.
             */
            LoadModule(LoaderBlock, DirPath, KdTransportDllName, "kdcom.dll", LoaderSystemCode, &KdComDTE, 60);
        }
    }

    /* Load all referenced DLLs for Kernel, HAL and Kernel Debugger Transport DLL */
    Success  = WinLdrScanImportDescriptorTable(&LoaderBlock->LoadOrderListHead, DirPath, *KernelDTE);
    Success &= WinLdrScanImportDescriptorTable(&LoaderBlock->LoadOrderListHead, DirPath, HalDTE);
    if (KdComDTE)
    {
        Success &= WinLdrScanImportDescriptorTable(&LoaderBlock->LoadOrderListHead, DirPath, KdComDTE);
    }

    return Success;
}

ARC_STATUS
LoadAndBootWindows(
    IN ULONG Argc,
    IN PCHAR Argv[],
    IN PCHAR Envp[])
{
    PCSTR ArgValue;
    PCHAR File;
    BOOLEAN Success;
    USHORT OperatingSystemVersion;
    PLOADER_PARAMETER_BLOCK LoaderBlock;
    CHAR  BootPath[MAX_PATH];
    CHAR  FileName[MAX_PATH];
    CHAR  BootOptions[256];

    ArgValue = GetArgumentValue(Argc, Argv, "BootType");
    if (!ArgValue)
    {
        ERR("No 'BootType' value, aborting!\n");
        return EINVAL;
    }

    if (_stricmp(ArgValue, "Windows") == 0 ||
        _stricmp(ArgValue, "Windows2003") == 0)
    {
        OperatingSystemVersion = _WIN32_WINNT_WS03;
    }
    else if (_stricmp(ArgValue, "WindowsNT40") == 0)
    {
        OperatingSystemVersion = _WIN32_WINNT_NT4;
    }
    else
    {
        ERR("Unknown 'BootType' value '%s', aborting!\n", ArgValue);
        return EINVAL;
    }

    UiDrawBackdrop();
    UiDrawProgressBarCenter(1, 100, "Loading NT...");

    /* Retrieve the system path */
    *BootPath = ANSI_NULL;
    ArgValue = GetArgumentValue(Argc, Argv, "SystemPath");
    if (ArgValue)
        RtlStringCbCopyA(BootPath, sizeof(BootPath), ArgValue);

    /*
     * Check whether BootPath is a full path
     * and if not, create a full boot path.
     *
     * See FsOpenFile for the technique used.
     */
    if (strrchr(BootPath, ')') == NULL)
    {
        /* Temporarily save the boot path */
        RtlStringCbCopyA(FileName, sizeof(FileName), BootPath);

        /* This is not a full path. Use the current (i.e. boot) device. */
        MachDiskGetBootPath(BootPath, sizeof(BootPath));

        /* Append a path separator if needed */
        if (*FileName != '\\' && *FileName != '/')
            RtlStringCbCatA(BootPath, sizeof(BootPath), "\\");

        /* Append the remaining path */
        RtlStringCbCatA(BootPath, sizeof(BootPath), FileName);
    }

    /* Append a backslash if needed */
    if (!*BootPath || BootPath[strlen(BootPath) - 1] != '\\')
        RtlStringCbCatA(BootPath, sizeof(BootPath), "\\");

    TRACE("BootPath: '%s'\n", BootPath);

    /* Retrieve the boot options */
    *BootOptions = ANSI_NULL;
    ArgValue = GetArgumentValue(Argc, Argv, "Options");
    if (ArgValue)
        RtlStringCbCopyA(BootOptions, sizeof(BootOptions), ArgValue);

    /* Append boot-time options */
    AppendBootTimeOptions(BootOptions);

    TRACE("BootOptions: '%s'\n", BootOptions);

    /* Check if a ramdisk file was given */
    File = strstr(BootOptions, "/RDPATH=");
    if (File)
    {
        /* Copy the file name and everything else after it */
        RtlStringCbCopyA(FileName, sizeof(FileName), File + 8);

        /* Null-terminate */
        *strstr(FileName, " ") = ANSI_NULL;

        /* Load the ramdisk */
        if (!RamDiskLoadVirtualFile(FileName))
        {
            UiMessageBox("Failed to load RAM disk file %s", FileName);
            return ENOENT;
        }
    }

    /* Let user know we started loading */
    //UiDrawStatusText("Loading...");

    /* Allocate and minimalist-initialize LPB */
    AllocateAndInitLPB(&LoaderBlock);

    /* Load the system hive */
    UiDrawBackdrop();
    UiDrawProgressBarCenter(15, 100, "Loading system hive...");
    Success = WinLdrInitSystemHive(LoaderBlock, BootPath, FALSE);
    TRACE("SYSTEM hive %s\n", (Success ? "loaded" : "not loaded"));
    /* Bail out if failure */
    if (!Success)
        return ENOEXEC;

    /* Load NLS data, OEM font, and prepare boot drivers list */
    Success = WinLdrScanSystemHive(LoaderBlock, BootPath);
    TRACE("SYSTEM hive %s\n", (Success ? "scanned" : "not scanned"));
    /* Bail out if failure */
    if (!Success)
        return ENOEXEC;

    /* Finish loading */
    return LoadAndBootWindowsCommon(OperatingSystemVersion,
                                    LoaderBlock,
                                    BootOptions,
                                    BootPath,
                                    FALSE);
}

ARC_STATUS
LoadAndBootWindowsCommon(
    USHORT OperatingSystemVersion,
    PLOADER_PARAMETER_BLOCK LoaderBlock,
    PCSTR BootOptions,
    PCSTR BootPath,
    BOOLEAN Setup)
{
    PLOADER_PARAMETER_BLOCK LoaderBlockVA;
    BOOLEAN Success;
    PLDR_DATA_TABLE_ENTRY KernelDTE;
    KERNEL_ENTRY_POINT KiSystemStartup;
    PCSTR SystemRoot;

    TRACE("LoadAndBootWindowsCommon()\n");

#ifdef _M_IX86
    /* Setup redirection support */
    WinLdrSetupEms((PCHAR)BootOptions);
#endif

    /* Convert BootPath to SystemRoot */
    SystemRoot = strstr(BootPath, "\\");

    /* Detect hardware */
    UiDrawBackdrop();
    UiDrawProgressBarCenter(20, 100, "Detecting hardware...");
    LoaderBlock->ConfigurationRoot = MachHwDetect();

    if (OperatingSystemVersion == 0)
        OperatingSystemVersion = WinLdrDetectVersion();

    /* Load the operating system core: the Kernel, the HAL and the Kernel Debugger Transport DLL */
    Success = LoadWindowsCore(OperatingSystemVersion,
                              LoaderBlock,
                              BootOptions,
                              BootPath,
                              &KernelDTE);
    if (!Success)
    {
        UiMessageBox("Error loading NTOS core.");
        return ENOEXEC;
    }

    /* Load boot drivers */
    UiDrawBackdrop();
    UiDrawProgressBarCenter(100, 100, "Loading boot drivers...");
    Success = WinLdrLoadBootDrivers(LoaderBlock, BootPath);
    TRACE("Boot drivers loading %s\n", Success ? "successful" : "failed");

    /* Cleanup ini file */
    IniCleanup();

    /* Initialize Phase 1 - no drivers loading anymore */
    WinLdrInitializePhase1(LoaderBlock,
                           BootOptions,
                           SystemRoot,
                           BootPath,
                           OperatingSystemVersion);

    /* Save entry-point pointer and Loader block VAs */
    KiSystemStartup = (KERNEL_ENTRY_POINT)KernelDTE->EntryPoint;
    LoaderBlockVA = PaToVa(LoaderBlock);

    /* "Stop all motors", change videomode */
    MachPrepareForReactOS();

    /* Debugging... */
    //DumpMemoryAllocMap();

    /* Do the machine specific initialization */
    WinLdrSetupMachineDependent(LoaderBlock);

    /* Map pages and create memory descriptors */
    WinLdrSetupMemoryLayout(LoaderBlock);

    /* Set processor context */
    WinLdrSetProcessorContext();

    /* Save final value of LoaderPagesSpanned */
    LoaderBlock->Extension->LoaderPagesSpanned = LoaderPagesSpanned;

    TRACE("Hello from paged mode, KiSystemStartup %p, LoaderBlockVA %p!\n",
          KiSystemStartup, LoaderBlockVA);

    // Zero KI_USER_SHARED_DATA page
    memset((PVOID)KI_USER_SHARED_DATA, 0, MM_PAGE_SIZE);

    WinLdrpDumpMemoryDescriptors(LoaderBlockVA);
    WinLdrpDumpBootDriver(LoaderBlockVA);
#ifndef _M_AMD64
    WinLdrpDumpArcDisks(LoaderBlockVA);
#endif

    /* Pass control */
    (*KiSystemStartup)(LoaderBlockVA);
    return ESUCCESS;
}

VOID
WinLdrpDumpMemoryDescriptors(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PLIST_ENTRY NextMd;
    PMEMORY_ALLOCATION_DESCRIPTOR MemoryDescriptor;

    NextMd = LoaderBlock->MemoryDescriptorListHead.Flink;

    while (NextMd != &LoaderBlock->MemoryDescriptorListHead)
    {
        MemoryDescriptor = CONTAINING_RECORD(NextMd, MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);

        TRACE("BP %08X PC %04X MT %d\n", MemoryDescriptor->BasePage,
            MemoryDescriptor->PageCount, MemoryDescriptor->MemoryType);

        NextMd = MemoryDescriptor->ListEntry.Flink;
    }
}

VOID
WinLdrpDumpBootDriver(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PLIST_ENTRY NextBd;
    PBOOT_DRIVER_LIST_ENTRY BootDriver;

    NextBd = LoaderBlock->BootDriverListHead.Flink;

    while (NextBd != &LoaderBlock->BootDriverListHead)
    {
        BootDriver = CONTAINING_RECORD(NextBd, BOOT_DRIVER_LIST_ENTRY, Link);

        TRACE("BootDriver %wZ DTE %08X RegPath: %wZ\n", &BootDriver->FilePath,
            BootDriver->LdrEntry, &BootDriver->RegistryPath);

        NextBd = BootDriver->Link.Flink;
    }
}

VOID
WinLdrpDumpArcDisks(PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PLIST_ENTRY NextBd;
    PARC_DISK_SIGNATURE ArcDisk;

    NextBd = LoaderBlock->ArcDiskInformation->DiskSignatureListHead.Flink;

    while (NextBd != &LoaderBlock->ArcDiskInformation->DiskSignatureListHead)
    {
        ArcDisk = CONTAINING_RECORD(NextBd, ARC_DISK_SIGNATURE, ListEntry);

        TRACE("ArcDisk %s checksum: 0x%X, signature: 0x%X\n",
            ArcDisk->ArcName, ArcDisk->CheckSum, ArcDisk->Signature);

        NextBd = ArcDisk->ListEntry.Flink;
    }
}
