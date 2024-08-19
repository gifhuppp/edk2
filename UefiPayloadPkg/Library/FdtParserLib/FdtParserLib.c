/** @file
  Copyright (c) 2024, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiPei.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <Library/IoLib.h>
#include <Guid/MemoryAllocationHob.h>
#include <Guid/DebugPrintErrorLevel.h>
#include <Guid/SerialPortInfoGuid.h>
#include <Guid/MemoryMapInfoGuid.h>
#include <Guid/AcpiBoardInfoGuid.h>
#include <Guid/GraphicsInfoHob.h>
#include <Guid/UniversalPayloadBase.h>
#include <UniversalPayload/SmbiosTable.h>
#include <UniversalPayload/AcpiTable.h>
#include <UniversalPayload/UniversalPayload.h>
#include <UniversalPayload/ExtraData.h>
#include <UniversalPayload/SerialPortInfo.h>
#include <UniversalPayload/DeviceTree.h>
#include <UniversalPayload/PciRootBridges.h>
#include <IndustryStandard/SmBios.h>
#include <Library/PrintLib.h>
#include <Library/FdtLib.h>
#include <Protocol/PciHostBridgeResourceAllocation.h>
#include <Protocol/PciIo.h>

typedef enum {
  ReservedMemory = 1,
  Memory,
  FrameBuffer,
  PciRootBridge,
  Options,
  DoNothing
} FDT_NODE_TYPE;

#define MEMORY_ATTRIBUTE_DEFAULT  (EFI_RESOURCE_ATTRIBUTE_PRESENT                   | \
                                     EFI_RESOURCE_ATTRIBUTE_INITIALIZED             | \
                                     EFI_RESOURCE_ATTRIBUTE_TESTED                  | \
                                     EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE             | \
                                     EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE       | \
                                     EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE | \
                                     EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE    )

#define ROOT_BRIDGE_SUPPORTS_DEFAULT  (EFI_PCI_IO_ATTRIBUTE_VGA_IO_16 | \
                              EFI_PCI_IO_ATTRIBUTE_VGA_PALETTE_IO_16 | \
                              EFI_PCI_IO_ATTRIBUTE_ISA_IO_16 | \
                              EFI_PCI_IO_ATTRIBUTE_IDE_PRIMARY_IO | \
                              EFI_PCI_IO_ATTRIBUTE_VGA_IO | \
                              EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY | \
                              EFI_PCI_IO_ATTRIBUTE_VGA_PALETTE_IO | \
                              EFI_PCI_IO_ATTRIBUTE_ISA_IO | \
                              EFI_PCI_IO_ATTRIBUTE_ISA_MOTHERBOARD_IO )

extern VOID                         *mHobList;
UNIVERSAL_PAYLOAD_PCI_ROOT_BRIDGES  *mPciRootBridgeInfo = NULL;
INT32                               mNode[0x500]        = { 0 };
UINT32                              mNodeIndex          = 0;

/**
  Build a Handoff Information Table HOB

  This function initialize a HOB region from EfiMemoryBegin to
  EfiMemoryTop. And EfiFreeMemoryBottom and EfiFreeMemoryTop should
  be inside the HOB region.

  @param[in] EfiMemoryBottom       Total memory start address
  @param[in] EfiMemoryTop          Total memory end address.
  @param[in] EfiFreeMemoryBottom   Free memory start address
  @param[in] EfiFreeMemoryTop      Free memory end address.

  @return   The pointer to the handoff HOB table.

**/
EFI_HOB_HANDOFF_INFO_TABLE *
EFIAPI
HobConstructor (
  IN VOID  *EfiMemoryBottom,
  IN VOID  *EfiMemoryTop,
  IN VOID  *EfiFreeMemoryBottom,
  IN VOID  *EfiFreeMemoryTop
  );

/**
  It will record the memory node initialized.

  @param[in]  Node           memory node is going to parsing..
**/
VOID
RecordMemoryNode (
  INT32  Node
  )
{
  DEBUG ((DEBUG_INFO, "\n RecordMemoryNode  %x , mNodeIndex :%x  \n", Node, mNodeIndex));
  mNode[mNodeIndex] = Node;
  mNodeIndex++;
}

/**
  Check the memory node if initialized.

  @param[in]  Node           memory node is going to parsing..

  @return TRUE               memory node was initialized. don't parse it again.
  @return FALSE              memory node wasn't initialized , go to parse it.
**/
BOOLEAN
CheckMemoryNodeIfInit (
  INT32  Node
  )
{
  UINT32  i;

  for (i = 0; i < mNodeIndex; i++) {
    if (mNode[i] == Node) {
      return TRUE;
    }
  }

  return FALSE;
}

/**
  It will check device node from FDT.

  @param[in]  NodeString        Device node name string.
  @param[in]  Depth             Check layer of Device node , only parse the 1st layer

  @return FDT_NODE_TYPE         what type of the device node.
**/
FDT_NODE_TYPE
CheckNodeType (
  CHAR8  *NodeString,
  INT32  Depth
  )
{
  DEBUG ((DEBUG_INFO, "\n CheckNodeType  %a   \n", NodeString));
  if (AsciiStrnCmp (NodeString, "reserved-memory", AsciiStrLen ("reserved-memory")) == 0 ) {
    return ReservedMemory;
  } else if (AsciiStrnCmp (NodeString, "memory@", AsciiStrLen ("memory@")) == 0 ) {
    return Memory;
  } else if (AsciiStrnCmp (NodeString, "framebuffer@", AsciiStrLen ("framebuffer@")) == 0) {
    return FrameBuffer;
  } else if (AsciiStrnCmp (NodeString, "pci-rb", AsciiStrLen ("pci-rb")) == 0 ) {
    return PciRootBridge;
  } else if (AsciiStrCmp (NodeString, "options") == 0) {
    return Options;
  } else {
    return DoNothing;
  }
}

/**
  It will ParseMemory node from FDT.

  @param[in]  Fdt               Address of the Fdt data.
  @param[in]  SubNode           first node of the PCI root bridge node.
**/
VOID
ParseMemory (
  IN VOID   *Fdt,
  IN INT32  Node
  )
{
  UINT32              Attribute;
  UINT8               ECCAttribute;
  UINT32              ECCData, ECCData2;
  INT32               Property;
  CONST FDT_PROPERTY  *PropertyPtr;
  INT32               TempLen;
  CONST CHAR8         *TempStr;
  UINT64              *Data64;
  UINT32              *Data32;
  UINT64              StartAddress;
  UINT64              NumberOfBytes;

  Attribute    = MEMORY_ATTRIBUTE_DEFAULT;
  ECCAttribute = 0;
  ECCData      = ECCData2 = 0;
  for (Property = FdtFirstPropertyOffset (Fdt, Node); Property >= 0; Property = FdtNextPropertyOffset (Fdt, Property)) {
    PropertyPtr = FdtGetPropertyByOffset (Fdt, Property, &TempLen);
    TempStr     = FdtGetString (Fdt, Fdt32ToCpu (PropertyPtr->NameOffset), NULL);
    if (AsciiStrCmp (TempStr, "reg") == 0) {
      Data64        = (UINT64 *)(PropertyPtr->Data);
      StartAddress  = Fdt64ToCpu (*Data64);
      NumberOfBytes = Fdt64ToCpu (*(Data64 + 1));
    } else if (AsciiStrCmp (TempStr, "ecc-detection-bits") == 0) {
      Data32  = (UINT32 *)(PropertyPtr->Data);
      ECCData = Fdt32ToCpu (*Data32);
    } else if (AsciiStrCmp (TempStr, "ecc-correction-bits") == 0) {
      Data32   = (UINT32 *)(PropertyPtr->Data);
      ECCData2 = Fdt32ToCpu (*Data32);
    }
  }

  if (ECCData == ECCData2) {
    if (ECCData == 1) {
      ECCAttribute = EFI_RESOURCE_ATTRIBUTE_SINGLE_BIT_ECC;
    } else if (ECCData == 2) {
      ECCAttribute = EFI_RESOURCE_ATTRIBUTE_MULTIPLE_BIT_ECC;
    }
  }

  if (ECCAttribute != 0) {
    Attribute |= ECCAttribute;
  }

  BuildResourceDescriptorHob (EFI_RESOURCE_SYSTEM_MEMORY, Attribute, StartAddress, NumberOfBytes);
}

/**
  It will ParseReservedMemory node from FDT.

  @param[in]  Fdt               Address of the Fdt data.
  @param[in]  SubNode           first node of the PCI root bridge node.
**/
VOID
ParseReservedMemory (
  IN VOID   *Fdt,
  IN INT32  Node
  )
{
  INT32                         SubNode;
  INT32                         TempLen;
  CONST CHAR8                   *TempStr;
  CONST FDT_PROPERTY            *PropertyPtr;
  UINT64                        *Data64;
  UINT64                        StartAddress;
  UINT64                        NumberOfBytes;
  UNIVERSAL_PAYLOAD_ACPI_TABLE  *PlatformAcpiTable;
  FDT_NODE_HEADER               *NodePtr;

  PlatformAcpiTable = NULL;

  for (SubNode = FdtFirstSubnode (Fdt, Node); SubNode >= 0; SubNode = FdtNextSubnode (Fdt, SubNode)) {
    NodePtr = (FDT_NODE_HEADER *)((CONST CHAR8 *)Fdt + SubNode + Fdt32ToCpu (((FDT_HEADER *)Fdt)->OffsetDtStruct));
    DEBUG ((DEBUG_INFO, "\n      SubNode(%08X)  %a", SubNode, NodePtr->Name));
    PropertyPtr = FdtGetProperty (Fdt, SubNode, "reg", &TempLen);
    ASSERT (TempLen > 0);
    TempStr = (CHAR8 *)(PropertyPtr->Data);
    if (TempLen > 0) {
      Data64        = (UINT64 *)(PropertyPtr->Data);
      StartAddress  = Fdt64ToCpu (*Data64);
      NumberOfBytes = Fdt64ToCpu (*(Data64 + 1));
      DEBUG ((DEBUG_INFO, "\n         Property  %a", TempStr));
      DEBUG ((DEBUG_INFO, "  %016lX  %016lX", StartAddress, NumberOfBytes));
    }

    RecordMemoryNode (SubNode);

    if (AsciiStrnCmp (NodePtr->Name, "mmio@", AsciiStrLen ("mmio@")) == 0) {
      DEBUG ((DEBUG_INFO, "  MemoryMappedIO"));
      BuildMemoryAllocationHob (StartAddress, NumberOfBytes, EfiMemoryMappedIO);
    } else {
      PropertyPtr = FdtGetProperty (Fdt, SubNode, "compatible", &TempLen);
      TempStr     = (CHAR8 *)(PropertyPtr->Data);
      if (AsciiStrnCmp (TempStr, "boot-code", AsciiStrLen ("boot-code")) == 0) {
        DEBUG ((DEBUG_INFO, "  boot-code"));
        BuildMemoryAllocationHob (StartAddress, NumberOfBytes, EfiBootServicesCode);
      } else if (AsciiStrnCmp (TempStr, "boot-data", AsciiStrLen ("boot-data")) == 0) {
        DEBUG ((DEBUG_INFO, "  boot-data"));
        BuildMemoryAllocationHob (StartAddress, NumberOfBytes, EfiBootServicesData);
      } else if (AsciiStrnCmp (TempStr, "runtime-code", AsciiStrLen ("runtime-code")) == 0) {
        DEBUG ((DEBUG_INFO, "  runtime-code"));
        BuildMemoryAllocationHob (StartAddress, NumberOfBytes, EfiRuntimeServicesCode);
      } else if (AsciiStrnCmp (TempStr, "runtime-data", AsciiStrLen ("runtime-data")) == 0) {
        DEBUG ((DEBUG_INFO, "  runtime-data"));
        BuildMemoryAllocationHob (StartAddress, NumberOfBytes, EfiRuntimeServicesData);
      } else if (AsciiStrnCmp (TempStr, "acpi", AsciiStrLen ("acpi")) == 0) {
        DEBUG ((DEBUG_INFO, "  acpi, StartAddress:%x, NumberOfBytes:%x", StartAddress, NumberOfBytes));
        BuildMemoryAllocationHob (StartAddress, NumberOfBytes, EfiBootServicesData);
        PlatformAcpiTable = BuildGuidHob (&gUniversalPayloadAcpiTableGuid, sizeof (UNIVERSAL_PAYLOAD_ACPI_TABLE));
        if (PlatformAcpiTable != NULL) {
          DEBUG ((DEBUG_INFO, " build gUniversalPayloadAcpiTableGuid , NumberOfBytes:%x", NumberOfBytes));
          PlatformAcpiTable->Rsdp            = (EFI_PHYSICAL_ADDRESS)(UINTN)StartAddress;
          PlatformAcpiTable->Header.Revision = UNIVERSAL_PAYLOAD_ACPI_TABLE_REVISION;
          PlatformAcpiTable->Header.Length   = sizeof (UNIVERSAL_PAYLOAD_ACPI_TABLE);
        }
      } else if (AsciiStrnCmp (TempStr, "acpi-nvs", AsciiStrLen ("acpi-nvs")) == 0) {
        DEBUG ((DEBUG_INFO, "  acpi-nvs"));
        BuildMemoryAllocationHob (StartAddress, NumberOfBytes, EfiACPIMemoryNVS);
      } else {
        BuildMemoryAllocationHob (StartAddress, NumberOfBytes, EfiReservedMemoryType);
      }
    }
  }
}

/**
  It will ParseFrameBuffer node from FDT.

  @param[in]  Fdt               Address of the Fdt data.
  @param[in]  SubNode           first Sub node of the PCI root bridge node.

  @return     GmaStr            Graphic device node name string.
**/
CHAR8 *
ParseFrameBuffer (
  IN VOID   *Fdt,
  IN INT32  Node
  )
{
  INT32                      Property;
  INT32                      TempLen;
  CONST FDT_PROPERTY         *PropertyPtr;
  CONST CHAR8                *TempStr;
  UINT32                     *Data32;
  UINT64                     FrameBufferBase;
  UINT32                     FrameBufferSize;
  EFI_PEI_GRAPHICS_INFO_HOB  *GraphicsInfo;
  CHAR8                      *GmaStr;

  GmaStr = "Gma";
  //
  // Create GraphicInfo HOB.
  //
  GraphicsInfo = BuildGuidHob (&gEfiGraphicsInfoHobGuid, sizeof (EFI_PEI_GRAPHICS_INFO_HOB));
  ASSERT (GraphicsInfo != NULL);
  if (GraphicsInfo == NULL) {
    return GmaStr;
  }

  ZeroMem (GraphicsInfo, sizeof (EFI_PEI_GRAPHICS_INFO_HOB));

  for (Property = FdtFirstPropertyOffset (Fdt, Node); Property >= 0; Property = FdtNextPropertyOffset (Fdt, Property)) {
    PropertyPtr = FdtGetPropertyByOffset (Fdt, Property, &TempLen);
    TempStr     = FdtGetString (Fdt, Fdt32ToCpu (PropertyPtr->NameOffset), NULL);
    if (AsciiStrCmp (TempStr, "reg") == 0) {
      Data32                        = (UINT32 *)(PropertyPtr->Data);
      FrameBufferBase               = Fdt32ToCpu (*(Data32 + 0));
      FrameBufferSize               = Fdt32ToCpu (*(Data32 + 1));
      GraphicsInfo->FrameBufferBase = FrameBufferBase;
      GraphicsInfo->FrameBufferSize = (UINT32)FrameBufferSize;
    } else if (AsciiStrCmp (TempStr, "width") == 0) {
      Data32                                          = (UINT32 *)(PropertyPtr->Data);
      GraphicsInfo->GraphicsMode.HorizontalResolution = Fdt32ToCpu (*Data32);
    } else if (AsciiStrCmp (TempStr, "height") == 0) {
      Data32                                        = (UINT32 *)(PropertyPtr->Data);
      GraphicsInfo->GraphicsMode.VerticalResolution = Fdt32ToCpu (*Data32);
    } else if (AsciiStrCmp (TempStr, "format") == 0) {
      TempStr = (CHAR8 *)(PropertyPtr->Data);
      if (AsciiStrCmp (TempStr, "a8r8g8b8") == 0) {
        GraphicsInfo->GraphicsMode.PixelFormat = PixelRedGreenBlueReserved8BitPerColor;
      } else if (AsciiStrCmp (TempStr, "a8b8g8r8") == 0) {
        GraphicsInfo->GraphicsMode.PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
      } else {
        GraphicsInfo->GraphicsMode.PixelFormat = PixelFormatMax;
      }
    } else if (AsciiStrCmp (TempStr, "display") == 0) {
      GmaStr = (CHAR8 *)(PropertyPtr->Data);
      GmaStr++;
      DEBUG ((DEBUG_INFO, "  display (%s)", GmaStr));
    }
  }

  return GmaStr;
}

/**
  It will ParseOptions node from FDT.

  @param[in]  Fdt               Address of the Fdt data.
  @param[in]  SubNode           first Sub node of the PCI root bridge node.
  @param[out] PciEnumDone       Init ParsePciRootBridge node for ParsePciRootBridge.
  @param[out] BootMode          Init the system boot mode
**/
VOID
ParseOptions (
  IN VOID            *Fdt,
  IN INT32           Node,
  OUT UINT8          *PciEnumDone,
  OUT EFI_BOOT_MODE  *BootMode
  )
{
  INT32                   SubNode;
  FDT_NODE_HEADER         *NodePtr;
  UNIVERSAL_PAYLOAD_BASE  *PayloadBase;
  CONST FDT_PROPERTY      *PropertyPtr;
  CONST CHAR8             *TempStr;
  INT32                   TempLen;
  UINT32                  *Data32;
  UINT64                  *Data64;
  UINT64                  StartAddress;
  UINT8                   SizeOfMemorySpace;

  for (SubNode = FdtFirstSubnode (Fdt, Node); SubNode >= 0; SubNode = FdtNextSubnode (Fdt, SubNode)) {
    NodePtr = (FDT_NODE_HEADER *)((CONST CHAR8 *)Fdt + SubNode + Fdt32ToCpu (((FDT_HEADER *)Fdt)->OffsetDtStruct));
    DEBUG ((DEBUG_INFO, "\n      SubNode(%08X)  %a", SubNode, NodePtr->Name));

    if (AsciiStrnCmp (NodePtr->Name, "upl-images@", AsciiStrLen ("upl-images@")) == 0) {
      DEBUG ((DEBUG_INFO, "  Found image@ node \n"));
      //
      // Build PayloadBase HOB .
      //
      PayloadBase = BuildGuidHob (&gUniversalPayloadBaseGuid, sizeof (UNIVERSAL_PAYLOAD_BASE));
      ASSERT (PayloadBase != NULL);
      if (PayloadBase == NULL) {
        return;
      }

      PayloadBase->Header.Revision = UNIVERSAL_PAYLOAD_BASE_REVISION;
      PayloadBase->Header.Length   = sizeof (UNIVERSAL_PAYLOAD_BASE);

      PropertyPtr = FdtGetProperty (Fdt, SubNode, "addr", &TempLen);

      ASSERT (TempLen > 0);
      if (TempLen > 0) {
        Data64       = (UINT64 *)(PropertyPtr->Data);
        StartAddress = Fdt64ToCpu (*Data64);
        DEBUG ((DEBUG_INFO, "\n         Property(00000000)  entry"));
        DEBUG ((DEBUG_INFO, "  %016lX\n", StartAddress));

        PayloadBase->Entry = (EFI_PHYSICAL_ADDRESS)StartAddress;
      }
    }

    if (AsciiStrnCmp (NodePtr->Name, "upl-params", AsciiStrLen ("upl-params")) == 0) {
      PropertyPtr = FdtGetProperty (Fdt, SubNode, "addr-width", &TempLen);
      if (TempLen > 0) {
        Data32 = (UINT32 *)(PropertyPtr->Data);
        DEBUG ((DEBUG_INFO, "\n         Property(00000000)  address_width"));
        DEBUG ((DEBUG_INFO, "  %X", Fdt32ToCpu (*Data32)));
        SizeOfMemorySpace = (UINT8)Fdt32ToCpu (*Data32);
        BuildCpuHob (SizeOfMemorySpace, PcdGet8 (SizeOfIoSpace));
      }

      PropertyPtr = FdtGetProperty (Fdt, SubNode, "pci-enum-done", &TempLen);
      if (TempLen > 0) {
        *PciEnumDone = 1;
        DEBUG ((DEBUG_INFO, "  Found PciEnumDone (%08X)\n", *PciEnumDone));
      } else {
        *PciEnumDone = 0;
        DEBUG ((DEBUG_INFO, "  Not Found PciEnumDone \n"));
      }

      PropertyPtr = FdtGetProperty (Fdt, SubNode, "boot-mode", &TempLen);
      if (TempLen > 0) {
        TempStr = (CHAR8 *)(PropertyPtr->Data);
        if (AsciiStrCmp (TempStr, "normal") == 0) {
          *BootMode = BOOT_WITH_FULL_CONFIGURATION;
        } else if (AsciiStrCmp (TempStr, "fast") == 0) {
          *BootMode = BOOT_WITH_MINIMAL_CONFIGURATION;
        } else if (AsciiStrCmp (TempStr, "full") == 0) {
          *BootMode = BOOT_WITH_FULL_CONFIGURATION_PLUS_DIAGNOSTICS;
        } else if (AsciiStrCmp (TempStr, "default") == 0) {
          *BootMode = BOOT_WITH_DEFAULT_SETTINGS;
        } else if (AsciiStrCmp (TempStr, "s4") == 0) {
          *BootMode = BOOT_ON_S4_RESUME;
        } else if (AsciiStrCmp (TempStr, "s3") == 0) {
          *BootMode = BOOT_ON_S3_RESUME;
        }
      }
    }
  }
}

/**
  It will Parsegraphic node from FDT.

  @param[in]  Fdt               Address of the Fdt data.
  @param[in]  SubNode           first Sub node of the PCI root bridge node.
**/
VOID
ParsegraphicNode (
  IN VOID   *Fdt,
  IN INT32  SubNode
  )
{
  EFI_PEI_GRAPHICS_DEVICE_INFO_HOB  *GraphicsDev;
  CONST FDT_PROPERTY                *PropertyPtr;
  UINT16                            GmaID;
  UINT32                            *Data32;
  INT32                             TempLen;

  DEBUG ((DEBUG_INFO, "  Found gma@ node \n"));
  GraphicsDev = NULL;
  //
  // Build Graphic info HOB .
  //
  GraphicsDev = BuildGuidHob (&gEfiGraphicsDeviceInfoHobGuid, sizeof (EFI_PEI_GRAPHICS_DEVICE_INFO_HOB));
  ASSERT (GraphicsDev != NULL);
  if (GraphicsDev == NULL) {
    return;
  }

  SetMem (GraphicsDev, sizeof (EFI_PEI_GRAPHICS_DEVICE_INFO_HOB), 0xFF);
  PropertyPtr = FdtGetProperty (Fdt, SubNode, "vendor-id", &TempLen);
  ASSERT (TempLen > 0);
  if (TempLen > 0) {
    Data32 = (UINT32 *)(PropertyPtr->Data);
    GmaID  = (UINT16)Fdt32ToCpu (*Data32);
    DEBUG ((DEBUG_INFO, "\n   vendor-id"));
    DEBUG ((DEBUG_INFO, "  %016lX\n", GmaID));
    GraphicsDev->VendorId = GmaID;
  }

  PropertyPtr = FdtGetProperty (Fdt, SubNode, "device-id", &TempLen);
  ASSERT (TempLen > 0);
  if (TempLen > 0) {
    Data32 = (UINT32 *)(PropertyPtr->Data);
    GmaID  = (UINT16)Fdt32ToCpu (*Data32);
    DEBUG ((DEBUG_INFO, "\n   device-id"));
    DEBUG ((DEBUG_INFO, "  %016lX\n", GmaID));
    GraphicsDev->DeviceId = GmaID;
  }

  PropertyPtr = FdtGetProperty (Fdt, SubNode, "revision-id", &TempLen);
  ASSERT (TempLen > 0);
  if (TempLen > 0) {
    Data32 = (UINT32 *)(PropertyPtr->Data);
    GmaID  = (UINT16)Fdt32ToCpu (*Data32);
    DEBUG ((DEBUG_INFO, "\n   revision-id"));
    DEBUG ((DEBUG_INFO, "  %016lX\n", GmaID));
    GraphicsDev->RevisionId = (UINT8)GmaID;
  }

  PropertyPtr = FdtGetProperty (Fdt, SubNode, "subsystem-vendor-id", &TempLen);
  ASSERT (TempLen > 0);
  if (TempLen > 0) {
    Data32 = (UINT32 *)(PropertyPtr->Data);
    GmaID  = (UINT16)Fdt32ToCpu (*Data32);
    DEBUG ((DEBUG_INFO, "\n   subsystem-vendor-id"));
    DEBUG ((DEBUG_INFO, "  %016lX\n", GmaID));
    GraphicsDev->SubsystemVendorId = GmaID;
  }

  PropertyPtr = FdtGetProperty (Fdt, SubNode, "subsystem-id", &TempLen);
  ASSERT (TempLen > 0);
  if (TempLen > 0) {
    Data32 = (UINT32 *)(PropertyPtr->Data);
    GmaID  = (UINT16)Fdt32ToCpu (*Data32);
    DEBUG ((DEBUG_INFO, "\n   subsystem-id"));
    DEBUG ((DEBUG_INFO, "  %016lX\n", GmaID));
    GraphicsDev->SubsystemId = GmaID;
  }
}

/**
  It will ParseSerialPort node from FDT.

  @param[in]  Fdt               Address of the Fdt data.
  @param[in]  SubNode           first Sub node of the PCI root bridge node.
**/
VOID
ParseSerialPort (
  IN VOID   *Fdt,
  IN INT32  SubNode
  )
{
  UNIVERSAL_PAYLOAD_SERIAL_PORT_INFO  *Serial;
  CONST FDT_PROPERTY                  *PropertyPtr;
  INT32                               TempLen;
  CONST CHAR8                         *TempStr;
  UINT32                              *Data32;
  UINT32                              Attribute;

  //
  // Create SerialPortInfo HOB.
  //
  Serial = BuildGuidHob (&gUniversalPayloadSerialPortInfoGuid, sizeof (UNIVERSAL_PAYLOAD_SERIAL_PORT_INFO));
  ASSERT (Serial != NULL);
  if (Serial == NULL) {
    return;
  }

  Serial->Header.Revision = UNIVERSAL_PAYLOAD_SERIAL_PORT_INFO_REVISION;
  Serial->Header.Length   = sizeof (UNIVERSAL_PAYLOAD_SERIAL_PORT_INFO);
  Serial->RegisterStride  = 1;
  Serial->UseMmio         = 1;

  PropertyPtr = FdtGetProperty (Fdt, SubNode, "current-speed", &TempLen);
  ASSERT (TempLen > 0);
  if (TempLen > 0) {
    Data32 = (UINT32 *)(PropertyPtr->Data);
    DEBUG ((DEBUG_INFO, "  %X", Fdt32ToCpu (*Data32)));
    Serial->BaudRate = Fdt32ToCpu (*Data32);
  }

  PropertyPtr = FdtGetProperty (Fdt, SubNode, "compatible", &TempLen);
  TempStr     = (CHAR8 *)(PropertyPtr->Data);
  if (AsciiStrnCmp (TempStr, "isa", AsciiStrLen ("isa")) == 0) {
    DEBUG ((DEBUG_INFO, " find serial compatible isa \n"));
    Serial->UseMmio = 0;
    PropertyPtr     = FdtGetProperty (Fdt, SubNode, "reg", &TempLen);
    ASSERT (TempLen > 0);
    if (TempLen > 0) {
      Data32               = (UINT32 *)(PropertyPtr->Data);
      Attribute            = Fdt32ToCpu (*(Data32 + 0));
      Serial->RegisterBase = Fdt32ToCpu (*(Data32 + 1));
      Serial->UseMmio      = Attribute == 1 ? FALSE : TRUE;
      DEBUG ((DEBUG_INFO, "\n in espi serial  Property()  %a", TempStr));
      DEBUG ((DEBUG_INFO, " StartAddress   %016lX\n", Serial->RegisterBase));
      DEBUG ((DEBUG_INFO, " Attribute      %016lX\n", Attribute));
    }
  } else {
    DEBUG ((DEBUG_INFO, " NOT  serial compatible isa \n"));
    PropertyPtr = FdtGetProperty (Fdt, SubNode, "reg", &TempLen);
    ASSERT (TempLen > 0);
    if (TempLen > 0) {
      Data32               = (UINT32 *)(PropertyPtr->Data);
      Serial->RegisterBase = Fdt32ToCpu (*Data32);
    }
  }
}

/**
  It will ParsePciRootBridge node from FDT.

  @param[in]  Fdt               Address of the Fdt data.
  @param[in]  Node              first node of the Fdt data.
  @param[in]  PciEnumDone       To use ParsePciRootBridge node.
  @param[in]  RootBridgeCount   Number of pci RootBridge.
  @param[in]  GmaStr            Graphic device node name string.
  @param[in]  index             Index of ParsePciRootBridge node.
**/
VOID
ParsePciRootBridge (
  IN VOID   *Fdt,
  IN INT32  Node,
  IN UINT8  PciEnumDone,
  IN UINT8  RootBridgeCount,
  IN CHAR8  *GmaStr,
  IN UINT8  *index
  )
{
  INT32               SubNode;
  INT32               Property;
  INT32               SSubNode;
  FDT_NODE_HEADER     *NodePtr;
  CONST FDT_PROPERTY  *PropertyPtr;
  INT32               TempLen;
  UINT32              *Data32;
  UINT32              MemType;
  CONST CHAR8         *TempStr;
  UINT8               RbIndex;
  UINTN               HobDataSize;
  UINT8               Base;

  RbIndex     = *index;
  HobDataSize = sizeof (UNIVERSAL_PAYLOAD_PCI_ROOT_BRIDGES) + RootBridgeCount *sizeof (UNIVERSAL_PAYLOAD_PCI_ROOT_BRIDGE);
  //
  // Create PCI Root Bridge Info Hob.
  //
  if (mPciRootBridgeInfo == NULL) {
    mPciRootBridgeInfo = BuildGuidHob (&gUniversalPayloadPciRootBridgeInfoGuid, HobDataSize);
    ASSERT (mPciRootBridgeInfo != NULL);
    if (mPciRootBridgeInfo == NULL) {
      return;
    }

    ZeroMem (mPciRootBridgeInfo, HobDataSize);
    mPciRootBridgeInfo->Header.Length    = sizeof (UNIVERSAL_PAYLOAD_PCI_ROOT_BRIDGES);
    mPciRootBridgeInfo->Header.Revision  = UNIVERSAL_PAYLOAD_PCI_ROOT_BRIDGES_REVISION;
    mPciRootBridgeInfo->Count            = RootBridgeCount;
    mPciRootBridgeInfo->ResourceAssigned = (BOOLEAN)PciEnumDone;
  }

  for (SubNode = FdtFirstSubnode (Fdt, Node); SubNode >= 0; SubNode = FdtNextSubnode (Fdt, SubNode)) {
    NodePtr = (FDT_NODE_HEADER *)((CONST CHAR8 *)Fdt + SubNode + Fdt32ToCpu (((FDT_HEADER *)Fdt)->OffsetDtStruct));
    DEBUG ((DEBUG_INFO, "\n      SubNode(%08X)  %a", SubNode, NodePtr->Name));

    if (AsciiStrnCmp (NodePtr->Name, GmaStr, AsciiStrLen (GmaStr)) == 0) {
      DEBUG ((DEBUG_INFO, "  Found gma@ node \n"));
      ParsegraphicNode (Fdt, SubNode);
    }

    if (AsciiStrnCmp (NodePtr->Name, "isa", AsciiStrLen ("isa")) == 0) {
      SSubNode = FdtFirstSubnode (Fdt, SubNode); // serial
      ParseSerialPort (Fdt, SSubNode);
    }

    if (AsciiStrnCmp (NodePtr->Name, "serial@", AsciiStrLen ("serial@")) == 0) {
      ParseSerialPort (Fdt, SubNode);
    }
  }

  DEBUG ((DEBUG_INFO, " RbIndex :%x \n", RbIndex));

  for (Property = FdtFirstPropertyOffset (Fdt, Node); Property >= 0; Property = FdtNextPropertyOffset (Fdt, Property)) {
    PropertyPtr = FdtGetPropertyByOffset (Fdt, Property, &TempLen);
    TempStr     = FdtGetString (Fdt, Fdt32ToCpu (PropertyPtr->NameOffset), NULL);

    if (AsciiStrCmp (TempStr, "ranges") == 0) {
      DEBUG ((DEBUG_INFO, "  Found ranges Property TempLen (%08X), limit %x\n", TempLen, TempLen/sizeof (UINT32)));

      mPciRootBridgeInfo->RootBridge[RbIndex].AllocationAttributes = EFI_PCI_HOST_BRIDGE_COMBINE_MEM_PMEM | EFI_PCI_HOST_BRIDGE_MEM64_DECODE;
      mPciRootBridgeInfo->RootBridge[RbIndex].Supports             = ROOT_BRIDGE_SUPPORTS_DEFAULT;
      mPciRootBridgeInfo->RootBridge[RbIndex].PMemAbove4G.Base     = PcdGet64 (PcdPciReservedPMemAbove4GBBase);
      mPciRootBridgeInfo->RootBridge[RbIndex].PMemAbove4G.Limit    = PcdGet64 (PcdPciReservedPMemAbove4GBLimit);
      mPciRootBridgeInfo->RootBridge[RbIndex].PMem.Base            = PcdGet32 (PcdPciReservedPMemBase);
      mPciRootBridgeInfo->RootBridge[RbIndex].PMem.Limit           = PcdGet32 (PcdPciReservedPMemLimit);
      mPciRootBridgeInfo->RootBridge[RbIndex].UID                  = RbIndex;
      mPciRootBridgeInfo->RootBridge[RbIndex].HID                  = EISA_PNP_ID (0x0A03);

      Data32 = (UINT32 *)(PropertyPtr->Data);
      for (Base = 0; Base < TempLen/sizeof (UINT32); Base = Base + DWORDS_TO_NEXT_ADDR_TYPE) {
        DEBUG ((DEBUG_INFO, "  Base :%x \n", Base));
        MemType = Fdt32ToCpu (*(Data32 + Base));
        if (((MemType) & (SS_64BIT_MEMORY_SPACE)) == SS_64BIT_MEMORY_SPACE) {
          DEBUG ((DEBUG_INFO, "  To program 64 mm \n"));
          mPciRootBridgeInfo->RootBridge[RbIndex].MemAbove4G.Base  = Fdt32ToCpu (*(Data32 + Base + 2)) + LShiftU64 (Fdt32ToCpu (*(Data32 + Base + 1)), 32);
          mPciRootBridgeInfo->RootBridge[RbIndex].MemAbove4G.Limit = mPciRootBridgeInfo->RootBridge[RbIndex].MemAbove4G.Base + LShiftU64 (Fdt32ToCpu (*(Data32 + Base + 5)), 32) +  Fdt32ToCpu (*(Data32 + Base + 6)) -1;
        } else if (((MemType) & (SS_32BIT_MEMORY_SPACE)) == SS_32BIT_MEMORY_SPACE) {
          DEBUG ((DEBUG_INFO, "  To program 32 mem \n"));
          mPciRootBridgeInfo->RootBridge[RbIndex].Mem.Base  = Fdt32ToCpu (*(Data32 + Base + 2));
          mPciRootBridgeInfo->RootBridge[RbIndex].Mem.Limit = mPciRootBridgeInfo->RootBridge[RbIndex].Mem.Base + Fdt32ToCpu (*(Data32 + Base + 6)) -1;
        } else if (((MemType) & (SS_IO_SPACE)) == SS_IO_SPACE) {
          DEBUG ((DEBUG_INFO, "  To program Io\n"));
          mPciRootBridgeInfo->RootBridge[RbIndex].Io.Base  = Fdt32ToCpu (*(Data32 + Base + 2));
          mPciRootBridgeInfo->RootBridge[RbIndex].Io.Limit = mPciRootBridgeInfo->RootBridge[RbIndex].Io.Base + Fdt32ToCpu (*(Data32 +  Base + 6)) -1;
        }
      }

      DEBUG ((DEBUG_INFO, "RootBridgeCount %x, index :%x\n", RootBridgeCount, RbIndex));

      DEBUG ((DEBUG_INFO, "PciRootBridge->Mem.Base %x, \n", mPciRootBridgeInfo->RootBridge[RbIndex].Mem.Base));
      DEBUG ((DEBUG_INFO, "PciRootBridge->Mem.limit %x, \n", mPciRootBridgeInfo->RootBridge[RbIndex].Mem.Limit));

      DEBUG ((DEBUG_INFO, "PciRootBridge->MemAbove4G.Base %llx, \n", mPciRootBridgeInfo->RootBridge[RbIndex].MemAbove4G.Base));
      DEBUG ((DEBUG_INFO, "PciRootBridge->MemAbove4G.limit %llx, \n", mPciRootBridgeInfo->RootBridge[RbIndex].MemAbove4G.Limit));

      DEBUG ((DEBUG_INFO, "PciRootBridge->Io.Base %llx, \n", mPciRootBridgeInfo->RootBridge[RbIndex].Io.Base));
      DEBUG ((DEBUG_INFO, "PciRootBridge->Io.limit %llx, \n", mPciRootBridgeInfo->RootBridge[RbIndex].Io.Limit));
    }

    if (AsciiStrCmp (TempStr, "bus-range") == 0) {
      DEBUG ((DEBUG_INFO, "  Found bus-range Property TempLen (%08X)\n", TempLen));

      Data32                                                  = (UINT32 *)(PropertyPtr->Data);
      mPciRootBridgeInfo->RootBridge[RbIndex].Bus.Base        = Fdt32ToCpu (*Data32) & 0xFF;
      mPciRootBridgeInfo->RootBridge[RbIndex].Bus.Limit       = Fdt32ToCpu (*(Data32 + 1)) & 0xFF;
      mPciRootBridgeInfo->RootBridge[RbIndex].Bus.Translation = 0;

      DEBUG ((DEBUG_INFO, "PciRootBridge->Bus.Base %x, index %x\n", mPciRootBridgeInfo->RootBridge[RbIndex].Bus.Base, RbIndex));
      DEBUG ((DEBUG_INFO, "PciRootBridge->Bus.limit %x, index %x\n", mPciRootBridgeInfo->RootBridge[RbIndex].Bus.Limit, RbIndex));
    }
  }

  if (RbIndex > 0) {
    RbIndex--;
  }

  DEBUG ((DEBUG_INFO, "After updated RbIndex :%x \n", RbIndex));
  *index = RbIndex;
}

/**
  It will parse FDT based on DTB from bootloaders.

  @param[in]  FdtBase               Address of the Fdt data.

  @return   The address to the new hob list
**/
UINTN
EFIAPI
ParseDtb (
  IN VOID  *FdtBase
  )
{
  VOID                  *Fdt;
  INT32                 Node;
  INT32                 Property;
  INT32                 Depth;
  FDT_NODE_HEADER       *NodePtr;
  CONST FDT_PROPERTY    *PropertyPtr;
  CONST CHAR8           *TempStr;
  INT32                 TempLen;
  UINT64                *Data64;
  UINT64                StartAddress;
  UINT64                NumberOfBytes;
  UINTN                 MinimalNeededSize;
  EFI_PHYSICAL_ADDRESS  FreeMemoryBottom;
  EFI_PHYSICAL_ADDRESS  FreeMemoryTop;
  EFI_PHYSICAL_ADDRESS  MemoryBottom;
  EFI_PHYSICAL_ADDRESS  MemoryTop;
  BOOLEAN               IsHobConstructed;
  UINTN                 NewHobList;
  UINT8                 RootBridgeCount;
  UINT8                 index;
  UINTN                 HobDataSize;
  UINT8                 PciEnumDone;
  UINT8                 NodeType;
  EFI_BOOT_MODE         BootMode;
  CHAR8                 *GmaStr;

  Fdt               = FdtBase;
  Depth             = 0;
  MinimalNeededSize = FixedPcdGet32 (PcdSystemMemoryUefiRegionSize);
  IsHobConstructed  = FALSE;
  NewHobList        = 0;
  RootBridgeCount   = 0;
  index             = 0;
  HobDataSize       = 0;
  PciEnumDone       = 0;
  BootMode          = 0;
  NodeType          = 0;

  DEBUG ((DEBUG_INFO, "FDT = 0x%x  %x\n", Fdt, Fdt32ToCpu (*((UINT32 *)Fdt))));
  DEBUG ((DEBUG_INFO, "Start parsing DTB data\n"));
  DEBUG ((DEBUG_INFO, "MinimalNeededSize :%x\n", MinimalNeededSize));

  for (Node = FdtNextNode (Fdt, 0, &Depth); Node >= 0; Node = FdtNextNode (Fdt, Node, &Depth)) {
    NodePtr = (FDT_NODE_HEADER *)((CONST CHAR8 *)Fdt + Node + Fdt32ToCpu (((FDT_HEADER *)Fdt)->OffsetDtStruct));
    DEBUG ((DEBUG_INFO, "\n   Node(%08x)  %a   Depth %x", Node, NodePtr->Name, Depth));
    // memory node
    if (AsciiStrnCmp (NodePtr->Name, "memory@", AsciiStrLen ("memory@")) == 0) {
      for (Property = FdtFirstPropertyOffset (Fdt, Node); Property >= 0; Property = FdtNextPropertyOffset (Fdt, Property)) {
        PropertyPtr = FdtGetPropertyByOffset (Fdt, Property, &TempLen);
        TempStr     = FdtGetString (Fdt, Fdt32ToCpu (PropertyPtr->NameOffset), NULL);
        if (AsciiStrCmp (TempStr, "reg") == 0) {
          Data64        = (UINT64 *)(PropertyPtr->Data);
          StartAddress  = Fdt64ToCpu (*Data64);
          NumberOfBytes = Fdt64ToCpu (*(Data64 + 1));
          DEBUG ((DEBUG_INFO, "\n         Property(%08X)  %a", Property, TempStr));
          DEBUG ((DEBUG_INFO, "  %016lX  %016lX", StartAddress, NumberOfBytes));
          if (!IsHobConstructed) {
            if ((NumberOfBytes > MinimalNeededSize) && (StartAddress < BASE_4GB)) {
              MemoryBottom     = StartAddress + NumberOfBytes - MinimalNeededSize;
              FreeMemoryBottom = MemoryBottom;
              FreeMemoryTop    = StartAddress + NumberOfBytes;
              MemoryTop        = FreeMemoryTop;

              DEBUG ((DEBUG_INFO, "MemoryBottom :0x%llx\n", MemoryBottom));
              DEBUG ((DEBUG_INFO, "FreeMemoryBottom :0x%llx\n", FreeMemoryBottom));
              DEBUG ((DEBUG_INFO, "FreeMemoryTop :0x%llx\n", FreeMemoryTop));
              DEBUG ((DEBUG_INFO, "MemoryTop :0x%llx\n", MemoryTop));
              mHobList         =  HobConstructor ((VOID *)(UINTN)MemoryBottom, (VOID *)(UINTN)MemoryTop, (VOID *)(UINTN)FreeMemoryBottom, (VOID *)(UINTN)FreeMemoryTop);
              IsHobConstructed = TRUE;
              NewHobList       = (UINTN)mHobList;
              break;
            }
          }
        }
      }
    } // end of memory node
    else {
      PropertyPtr = FdtGetProperty (Fdt, Node, "compatible", &TempLen);
      TempStr     = (CHAR8 *)(PropertyPtr->Data);
      if (AsciiStrnCmp (TempStr, "pci-rb", AsciiStrLen ("pci-rb")) == 0) {
        RootBridgeCount++;
      }
    }
  }

  index = RootBridgeCount - 1;
  Depth = 0;
  for (Node = FdtNextNode (Fdt, 0, &Depth); Node >= 0; Node = FdtNextNode (Fdt, Node, &Depth)) {
    NodePtr = (FDT_NODE_HEADER *)((CONST CHAR8 *)Fdt + Node + Fdt32ToCpu (((FDT_HEADER *)Fdt)->OffsetDtStruct));
    DEBUG ((DEBUG_INFO, "\n   Node(%08x)  %a   Depth %x", Node, NodePtr->Name, Depth));

    NodeType = CheckNodeType (NodePtr->Name, Depth);
    DEBUG ((DEBUG_INFO, "NodeType :0x%x\n", NodeType));
    switch (NodeType) {
      case ReservedMemory:
        DEBUG ((DEBUG_INFO, "ParseReservedMemory\n"));
        ParseReservedMemory (Fdt, Node);
        break;
      case Memory:
        DEBUG ((DEBUG_INFO, "ParseMemory\n"));
        if (!CheckMemoryNodeIfInit (Node)) {
          ParseMemory (Fdt, Node);
        } else {
          DEBUG ((DEBUG_INFO, "Memory has initialized\n"));
        }

        break;
      case FrameBuffer:
        DEBUG ((DEBUG_INFO, "ParseFrameBuffer\n"));
        GmaStr = ParseFrameBuffer (Fdt, Node);
        break;
      case PciRootBridge:
        DEBUG ((DEBUG_INFO, "ParsePciRootBridge, index :%x\n", index));
        ParsePciRootBridge (Fdt, Node, PciEnumDone, RootBridgeCount, GmaStr, &index);
        DEBUG ((DEBUG_INFO, "After ParsePciRootBridge, index :%x\n", index));
        break;
      case Options:
        DEBUG ((DEBUG_INFO, "ParseOptions\n"));
        ParseOptions (Fdt, Node, &PciEnumDone, &BootMode);
        break;
      default:
        DEBUG ((DEBUG_INFO, "ParseNothing\n"));
        break;
    }
  }

  ((EFI_HOB_HANDOFF_INFO_TABLE *)(mHobList))->BootMode = BootMode;
  DEBUG ((DEBUG_INFO, "\n"));

  return NewHobList;
}

/**
  It will Parse FDT -node based on information from bootloaders.
  @param[in]  FdtBase   The starting memory address of FdtBase
  @retval HobList   The base address of Hoblist.

**/
UINTN
EFIAPI
FdtNodeParser (
  IN VOID  *FdtBase
  )
{
  return ParseDtb (FdtBase);
}

/**
  It will initialize HOBs for UPL.

  @param[in]  FdtBase        Address of the Fdt data.

  @retval EFI_SUCCESS        If it completed successfully.
  @retval Others             If it failed to initialize HOBs.
**/
UINTN
EFIAPI
UplInitHob (
  IN VOID  *FdtBase
  )
{
  UINTN  NHobAddress;

  NHobAddress = 0;
  //
  // Check parameter type(
  //
  if (FdtCheckHeader (FdtBase) == 0) {
    DEBUG ((DEBUG_INFO, "%a() FDT blob\n", __func__));
    NHobAddress = FdtNodeParser ((VOID *)FdtBase);
  } else {
    DEBUG ((DEBUG_INFO, "%a() HOb list\n", __func__));
    mHobList = FdtBase;

    return (UINTN)(mHobList);
  }

  return NHobAddress;
}