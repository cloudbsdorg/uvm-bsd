/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * rm_layout_test.c - the structures shared with the resource manager in
 * nvidia.ko (sys/nvidia_uvm_rm.h) have the sizes and offsets of uvm-spec
 * r1, 02-rm-interface.md section 2.  The RM core was compiled with these
 * layouts; a mismatch would corrupt memory on the first call.  Every check
 * is a compile-time assertion.
 *
 * Red first: compiled before the header existed (missing header).
 */

#include <stddef.h>
#include <stdio.h>

#include "nvidia_uvm_rm.h"

/* CPD-OFF -- one compile-time assertion per field of the RM interface: the repetition is the layout */
#define SZ(t, n)	_Static_assert(sizeof(t) == (n), #t " size")
#define AL(t, n)	_Static_assert(_Alignof(t) == (n), #t " align")
#define OF(t, f, n)	_Static_assert(offsetof(t, f) == (n), #t "." #f)

SZ(UvmGpuInfo, 240); AL(UvmGpuInfo, 8);
OF(UvmGpuInfo, uuid, 64); OF(UvmGpuInfo, gpuArch, 80); OF(UvmGpuInfo, gpuImplementation, 84);
OF(UvmGpuInfo, hostClass, 88); OF(UvmGpuInfo, ceClass, 92); OF(UvmGpuInfo, computeClass, 96);
OF(UvmGpuInfo, gpuInTcc, 100); OF(UvmGpuInfo, subdeviceCount, 104); OF(UvmGpuInfo, virtMode, 108);
OF(UvmGpuInfo, isSimulated, 112); OF(UvmGpuInfo, gpcCount, 116); OF(UvmGpuInfo, maxGpcCount, 120);
OF(UvmGpuInfo, tpcCount, 124); OF(UvmGpuInfo, maxTpcPerGpcCount, 128);
OF(UvmGpuInfo, accessCntrBufferCount, 132); OF(UvmGpuInfo, smcEnabled, 136);
OF(UvmGpuInfo, smcSwizzId, 140); OF(UvmGpuInfo, smcUserClientInfo, 144);
OF(UvmGpuInfo, gpuConfComputeCaps, 152); OF(UvmGpuInfo, sysmemLink, 156);
OF(UvmGpuInfo, sysmemLinkRateMBps, 160); OF(UvmGpuInfo, systemMemoryWindowStart, 168);
OF(UvmGpuInfo, systemMemoryWindowSize, 176); OF(UvmGpuInfo, connectedToSwitch, 184);
OF(UvmGpuInfo, nvswitchMemoryWindowStart, 192); OF(UvmGpuInfo, egmEnabled, 200);
OF(UvmGpuInfo, egmPeerId, 201); OF(UvmGpuInfo, egmBaseAddr, 208);
OF(UvmGpuInfo, nvswitchEgmMemoryWindowStart, 216); OF(UvmGpuInfo, atsSupport, 224);
OF(UvmGpuInfo, nonPasidAtsSupport, 225); OF(UvmGpuInfo, cdmmEnabled, 226);
OF(UvmGpuInfo, nvlDirectConnect, 227); OF(UvmGpuInfo, nvlDirectConnectMemoryWindowStart, 232);

SZ(UvmGpuCaps, 8); OF(UvmGpuCaps, numaNodeId, 4);
SZ(UvmGpuAddressSpaceInfo, 48); OF(UvmGpuAddressSpaceInfo, atsEnabled, 8);
OF(UvmGpuAddressSpaceInfo, time0Offset, 16); OF(UvmGpuAddressSpaceInfo, time1Offset, 24);
OF(UvmGpuAddressSpaceInfo, maxSubctxCount, 32); OF(UvmGpuAddressSpaceInfo, smcEnabled, 36);
OF(UvmGpuAddressSpaceInfo, smcSwizzId, 40); OF(UvmGpuAddressSpaceInfo, smcGpcCount, 44);
SZ(UvmGpuAllocInfo, 40); OF(UvmGpuAllocInfo, pageSize, 8); OF(UvmGpuAllocInfo, alignment, 16);
OF(UvmGpuAllocInfo, bContiguousPhysAlloc, 24); OF(UvmGpuAllocInfo, bMemGrowsDown, 25);
OF(UvmGpuAllocInfo, bPersistentVidmem, 26); OF(UvmGpuAllocInfo, hPhysHandle, 28);
OF(UvmGpuAllocInfo, bUnprotected, 32);
SZ(UvmGpuFbInfo, 64); OF(UvmGpuFbInfo, heapSize, 8); OF(UvmGpuFbInfo, reservedHeapSize, 12);
OF(UvmGpuFbInfo, bZeroFb, 16); OF(UvmGpuFbInfo, maxVidmemPageSize, 24);
OF(UvmGpuFbInfo, bStaticBar1Enabled, 32); OF(UvmGpuFbInfo, bStaticBar1WriteCombined, 33);
OF(UvmGpuFbInfo, staticBar1StartOffset, 40); OF(UvmGpuFbInfo, staticBar1Size, 48);
OF(UvmGpuFbInfo, heapStart, 56);
SZ(UvmGpuEccInfo, 32); OF(UvmGpuEccInfo, eccOffset, 4); OF(UvmGpuEccInfo, eccReadLocation, 8);
OF(UvmGpuEccInfo, eccErrorNotifier, 16); OF(UvmGpuEccInfo, bEccEnabled, 24);
SZ(UvmGpuNvlinkInfo, 32); OF(UvmGpuNvlinkInfo, bNvlinkRecoveryEnabled, 24);
SZ(UvmGpuCopyEngineCaps, 8); OF(UvmGpuCopyEngineCaps, cePceMask, 4);
SZ(UvmGpuCopyEnginesCaps, 512);
SZ(UvmGpuChannelInfo, 120); OF(UvmGpuChannelInfo, gpPut, 8); OF(UvmGpuChannelInfo, gpFifoEntries, 16);
OF(UvmGpuChannelInfo, numGpFifoEntries, 24); OF(UvmGpuChannelInfo, channelClassNum, 28);
OF(UvmGpuChannelInfo, errorNotifier, 32); OF(UvmGpuChannelInfo, keyRotationNotifier, 40);
OF(UvmGpuChannelInfo, hwRunlistId, 48); OF(UvmGpuChannelInfo, hwChannelId, 52);
OF(UvmGpuChannelInfo, dummyBar1Mapping, 56); OF(UvmGpuChannelInfo, workSubmissionOffset, 64);
OF(UvmGpuChannelInfo, workSubmissionToken, 72); OF(UvmGpuChannelInfo, pWorkSubmissionToken, 80);
OF(UvmGpuChannelInfo, gpFifoGpuVa, 88); OF(UvmGpuChannelInfo, gpPutGpuVa, 96);
OF(UvmGpuChannelInfo, gpGetGpuVa, 104); OF(UvmGpuChannelInfo, workSubmissionOffsetGpuVa, 112);
SZ(UvmGpuTsgAllocParams, 8); OF(UvmGpuTsgAllocParams, engineIndex, 4);
SZ(UvmGpuChannelAllocParams, 12); OF(UvmGpuChannelAllocParams, gpPutLoc, 8);
SZ(UvmGpuMemoryInfo, 56); OF(UvmGpuMemoryInfo, sysmem, 4); OF(UvmGpuMemoryInfo, egm, 5);
OF(UvmGpuMemoryInfo, deviceDescendant, 6); OF(UvmGpuMemoryInfo, pageSize, 8);
OF(UvmGpuMemoryInfo, contig, 16); OF(UvmGpuMemoryInfo, physAddr, 24); OF(UvmGpuMemoryInfo, size, 32);
OF(UvmGpuMemoryInfo, uuid, 40);
SZ(UvmGpuChannelResourceInfo, 80); OF(UvmGpuChannelResourceInfo, resourceId, 8);
OF(UvmGpuChannelResourceInfo, alignment, 16); OF(UvmGpuChannelResourceInfo, resourceInfo, 24);
SZ(UvmGpuChannelInstanceInfo, 1120); OF(UvmGpuChannelInstanceInfo, sysmem, 8);
OF(UvmGpuChannelInstanceInfo, runlistId, 12); OF(UvmGpuChannelInstanceInfo, chId, 16);
OF(UvmGpuChannelInstanceInfo, bInSubctx, 20); OF(UvmGpuChannelInstanceInfo, subctxId, 24);
OF(UvmGpuChannelInstanceInfo, bTsgChannel, 28); OF(UvmGpuChannelInstanceInfo, tsgId, 32);
OF(UvmGpuChannelInstanceInfo, tsgMaxSubctxCount, 36); OF(UvmGpuChannelInstanceInfo, resourceInfo, 40);
OF(UvmGpuChannelInstanceInfo, resourceCount, 1080); OF(UvmGpuChannelInstanceInfo, channelEngineType, 1084);
OF(UvmGpuChannelInstanceInfo, clearFaultedToken, 1088);
OF(UvmGpuChannelInstanceInfo, pChramChannelRegister, 1096);
OF(UvmGpuChannelInstanceInfo, pRunlistPRIBaseRegister, 1104);
OF(UvmGpuChannelInstanceInfo, smcEngineId, 1112); OF(UvmGpuChannelInstanceInfo, smcEngineVeIdOffset, 1116);
SZ(UvmGpuChannelResourceBindParams, 16); OF(UvmGpuChannelResourceBindParams, resourceVa, 8);
SZ(UvmGpuExternalMappingInfo, 72); OF(UvmGpuExternalMappingInfo, compressionType, 16);
OF(UvmGpuExternalMappingInfo, pteBufferSize, 24); OF(UvmGpuExternalMappingInfo, mappingPageSize, 32);
OF(UvmGpuExternalMappingInfo, pteBuffer, 40); OF(UvmGpuExternalMappingInfo, numWrittenPtes, 48);
OF(UvmGpuExternalMappingInfo, numRemainingPtes, 56); OF(UvmGpuExternalMappingInfo, pteSize, 64);
OF(UvmGpuExternalMappingInfo, bNeedL2InvalidateAtUnmap, 68);
SZ(UvmGpuFaultInfo, 184); OF(UvmGpuFaultInfo, replayable, 0);
OF(UvmGpuFaultInfo, replayable.pPmcIntr, 24); OF(UvmGpuFaultInfo, replayable.pPmcIntrEnSet, 32);
OF(UvmGpuFaultInfo, replayable.pPmcIntrEnClear, 40); OF(UvmGpuFaultInfo, replayable.pPrefetchCtrl, 48);
OF(UvmGpuFaultInfo, replayable.replayableFaultMask, 56); OF(UvmGpuFaultInfo, replayable.bufferAddress, 64);
OF(UvmGpuFaultInfo, replayable.bufferSize, 72); OF(UvmGpuFaultInfo, replayable.bufferMetadata, 80);
OF(UvmGpuFaultInfo, replayable.cslCtx, 88);
OF(UvmGpuFaultInfo, nonReplayable, 104); OF(UvmGpuFaultInfo, nonReplayable.shadowBufferContext, 112);
OF(UvmGpuFaultInfo, nonReplayable.bufferSize, 120); OF(UvmGpuFaultInfo, nonReplayable.isr_sp, 128);
OF(UvmGpuFaultInfo, nonReplayable.isr_bh_sp, 136); OF(UvmGpuFaultInfo, nonReplayable.pFaultBufferPut, 144);
OF(UvmGpuFaultInfo, nonReplayable.shadowBufferGet, 152);
OF(UvmGpuFaultInfo, nonReplayable.shadowBufferMetadata, 160);
OF(UvmGpuFaultInfo, faultBufferHandle, 168); OF(UvmGpuFaultInfo, pDevice, 176);
SZ(UvmPmaAllocationOptions, 56); OF(UvmPmaAllocationOptions, physBegin, 8);
OF(UvmPmaAllocationOptions, physEnd, 16); OF(UvmPmaAllocationOptions, regionId, 24);
OF(UvmPmaAllocationOptions, alignment, 32); OF(UvmPmaAllocationOptions, numPagesAllocated, 40);
OF(UvmPmaAllocationOptions, resultFlags, 48);
SZ(UvmPmaStatistics, 48);
SZ(UvmPlatformInfo, 2); OF(UvmPlatformInfo, confComputingEnabled, 1);
SZ(UvmGpuPlatformInfo, 24); OF(UvmGpuPlatformInfo, dma_addressable_start, 8);
OF(UvmGpuPlatformInfo, dma_addressable_limit, 16);
SZ(struct UvmOpsUvmEvents, 56); OF(struct UvmOpsUvmEvents, isrTopHalf, 32);
OF(struct UvmOpsUvmEvents, drainP2P, 40); OF(struct UvmOpsUvmEvents, resumeP2P, 48);
SZ(UvmGpuP2PCapsParams, 64); OF(UvmGpuP2PCapsParams, p2pLink, 16);
OF(UvmGpuP2PCapsParams, optimalNvlinkWriteCEs, 20); OF(UvmGpuP2PCapsParams, totalLinkLineRateMBps, 28);
OF(UvmGpuP2PCapsParams, bar1DmaAddress, 32); OF(UvmGpuP2PCapsParams, bar1DmaSize, 48);
SZ(UvmGpuAccessCntrInfo, 72); OF(UvmGpuAccessCntrInfo, accessCounterMask, 48);
OF(UvmGpuAccessCntrInfo, bufferAddress, 56); OF(UvmGpuAccessCntrInfo, bufferSize, 64);
OF(UvmGpuAccessCntrInfo, accessCntrBufferHandle, 68);
SZ(UvmGpuAccessCntrConfig, 8);
SZ(UvmGpuClientInfo, 8);

/* CPD-ON */

/* PMA flag bits (02 section 2) */
_Static_assert(UVM_PMA_ALLOCATE_DONT_EVICT == 1 && UVM_PMA_ALLOCATE_PINNED == 2 &&
    UVM_PMA_ALLOCATE_NO_ZERO == (1 << 10) && UVM_PMA_ALLOCATE_ALLOW_PARTIAL == (1 << 12) &&
    UVM_PMA_CALLED_FROM_PMA_EVICTION == 16384, "PMA flags");

int
main(void)
{
	UvmGpuCopyEngineCaps caps;

	/* copy-engine capability bits: bit 0 supported ... bit 8 secure */
	caps.flags = 0;
	caps.cePceMask = 0;
	caps.flags |= UVM_CE_CAP_SUPPORTED | UVM_CE_CAP_SYSMEM | UVM_CE_CAP_P2P;
	if (!UVM_CE_CAP_HAS(&caps, SUPPORTED) || UVM_CE_CAP_HAS(&caps, GRCE) ||
	    !UVM_CE_CAP_HAS(&caps, SYSMEM) || !UVM_CE_CAP_HAS(&caps, P2P) ||
	    UVM_CE_CAP_HAS(&caps, SECURE) || UVM_CE_CAP_SECURE != (1u << 8)) {
		printf("FAIL: copy engine capability bits\n");
		return (1);
	}
	printf("rm_layout_test: PASS\n");
	return (0);
}
