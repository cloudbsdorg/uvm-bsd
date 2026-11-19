/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * nvidia_uvm_rm.h - the kernel interface between nvidia-uvm.ko and the
 * resource manager (RM) inside nvidia.ko: the nvUvmInterface* entry points
 * nvidia.ko exports, the structures both sides share, and the callback
 * table UVM installs.  Everything here is interface data from uvm-spec r1,
 * 02-rm-interface.md: the structure layouts are the ones the RM core was
 * compiled with (natural alignment, LP64) and tests/unit/rm_layout_test.c
 * asserts each size and offset.
 *
 * Handles are pointers to RM-internal objects and are never dereferenced
 * here.  UvmGpuPointer is a 64-bit GPU virtual address.
 */

#ifndef _NVIDIA_UVM_RM_H_
#define	_NVIDIA_UVM_RM_H_

#include "nvidia_uvm_ioctl.h"

typedef uint64_t UvmGpuPointer;

typedef struct uvmGpuSession_tag	*uvmGpuSessionHandle;
typedef struct uvmGpuDevice_tag		*uvmGpuDeviceHandle;
typedef struct uvmGpuAddressSpace_tag	*uvmGpuAddressSpaceHandle;
typedef struct uvmGpuTsg_tag		*uvmGpuTsgHandle;
typedef struct uvmGpuChannel_tag	*uvmGpuChannelHandle;
typedef void				*uvmGpuRetainedChannelHandle;

/* 16-byte RM notification record: its status half-word at byte 14 */
typedef struct {
	NvU32	timeStampLo;
	NvU32	timeStampHi;
	NvU32	info32;
	NvU16	info16;
	NvU16	status;
} UvmRmNotification;

/* UvmGpuClientInfo (8) */
typedef struct {
	NvHandle	hClient;
	NvHandle	hSmcPartRef;
} UvmGpuClientInfo;

/* UvmGpuConfComputeCaps (2) */
typedef struct {
	NvBool	bConfComputingEnabled;
	NvBool	bKeyRotationEnabled;
} UvmGpuConfComputeCaps;

/* UvmGpuInfo (240, align 8) */
#define	UVM_GPU_NAME_LENGTH	0x40
typedef struct {
	char		name[UVM_GPU_NAME_LENGTH];	/* @0 */
	NvProcessorUuid	uuid;				/* @64 */
	NvU32		gpuArch;			/* @80 */
	NvU32		gpuImplementation;		/* @84 */
	NvU32		hostClass;			/* @88 */
	NvU32		ceClass;			/* @92 */
	NvU32		computeClass;			/* @96 */
	NvBool		gpuInTcc;			/* @100 */
	NvU32		subdeviceCount;			/* @104 */
	NvU32		virtMode;			/* @108 */
	NvBool		isSimulated;			/* @112 */
	NvU32		gpcCount;			/* @116 */
	NvU32		maxGpcCount;			/* @120 */
	NvU32		tpcCount;			/* @124 */
	NvU32		maxTpcPerGpcCount;		/* @128 */
	NvU32		accessCntrBufferCount;		/* @132 */
	NvBool		smcEnabled;			/* @136 */
	NvU32		smcSwizzId;			/* @140 */
	UvmGpuClientInfo smcUserClientInfo;		/* @144 */
	UvmGpuConfComputeCaps gpuConfComputeCaps;	/* @152 */
	NvU32		sysmemLink;			/* @156 */
	NvU32		sysmemLinkRateMBps;		/* @160 */
	NvU64		systemMemoryWindowStart;	/* @168 */
	NvU64		systemMemoryWindowSize;		/* @176 */
	NvBool		connectedToSwitch;		/* @184 */
	NvU64		nvswitchMemoryWindowStart;	/* @192 */
	NvBool		egmEnabled;			/* @200 */
	NvU8		egmPeerId;			/* @201 */
	NvU64		egmBaseAddr;			/* @208 */
	NvU64		nvswitchEgmMemoryWindowStart;	/* @216 */
	NvBool		atsSupport;			/* @224 */
	NvBool		nonPasidAtsSupport;		/* @225 */
	NvBool		cdmmEnabled;			/* @226 */
	NvBool		nvlDirectConnect;		/* @227 */
	NvU64		nvlDirectConnectMemoryWindowStart; /* @232 */
} UvmGpuInfo;

/* UvmGpuInfo.virtMode */
enum { UVM_VIRT_MODE_NONE = 0, UVM_VIRT_MODE_LEGACY = 1,
	UVM_VIRT_MODE_SRIOV_HEAVY = 2, UVM_VIRT_MODE_SRIOV_STANDARD = 3 };
/* UvmGpuInfo.sysmemLink, UvmGpuP2PCapsParams.p2pLink */
enum { UVM_LINK_TYPE_NONE = 0, UVM_LINK_TYPE_PCIE = 1, UVM_LINK_TYPE_NVLINK_1 = 2,
	UVM_LINK_TYPE_NVLINK_2 = 3, UVM_LINK_TYPE_NVLINK_3 = 4, UVM_LINK_TYPE_NVLINK_4 = 5,
	UVM_LINK_TYPE_NVLINK_5 = 6, UVM_LINK_TYPE_C2C = 7 };

/* UvmGpuCaps (8) */
typedef struct {
	NvBool	numaEnabled;
	NvU32	numaNodeId;
} UvmGpuCaps;

/* UvmGpuAddressSpaceInfo (48) */
typedef struct {
	NvU64		bigPageSize;		/* @0 */
	NvBool		atsEnabled;		/* @8 */
	volatile NvU32	*time0Offset;		/* @16 */
	volatile NvU32	*time1Offset;		/* @24 */
	NvU32		maxSubctxCount;		/* @32 */
	NvBool		smcEnabled;		/* @36 */
	NvU32		smcSwizzId;		/* @40 */
	NvU32		smcGpcCount;		/* @44 */
} UvmGpuAddressSpaceInfo;

/* UvmGpuAllocInfo (40) */
typedef struct {
	NvU64		gpuPhysOffset;		/* OUT */
	NvU64		pageSize;
	NvU64		alignment;
	NvBool		bContiguousPhysAlloc;
	NvBool		bMemGrowsDown;
	NvBool		bPersistentVidmem;
	NvHandle	hPhysHandle;
	NvBool		bUnprotected;
} UvmGpuAllocInfo;

/* UvmGpuFbInfo (64) */
typedef struct {
	NvU64		maxAllocatableAddress;	/* @0 */
	NvU32		heapSize;		/* KB @8 */
	NvU32		reservedHeapSize;	/* KB @12 */
	NvBool		bZeroFb;		/* @16 */
	NvU64		maxVidmemPageSize;	/* @24 */
	NvBool		bStaticBar1Enabled;	/* @32 */
	NvBool		bStaticBar1WriteCombined; /* @33 */
	NvU64		staticBar1StartOffset;	/* @40 */
	NvU64		staticBar1Size;		/* @48 */
	NvU32		heapStart;		/* KB @56 */
} UvmGpuFbInfo;

/* UvmGpuEccInfo (32) and UvmGpuNvlinkInfo (32) */
typedef struct {
	NvU32		eccMask;
	NvU32		eccOffset;
	void		*eccReadLocation;
	NvBool		*eccErrorNotifier;
	NvBool		bEccEnabled;
} UvmGpuEccInfo;

typedef struct {
	NvU32		nvlinkMask;
	NvU32		nvlinkOffset;
	void		*nvlinkReadLocation;
	NvBool		*nvlinkErrorNotifier;
	NvBool		bNvlinkRecoveryEnabled;
} UvmGpuNvlinkInfo;

/*
 * UvmGpuCopyEngineCaps (8): nine one-bit flags packed from bit 0 upward,
 * then the physical-engine mask.  Kept as a plain 16-bit word (no
 * bitfields) so the layout is the compiler-independent one.
 */
typedef struct {
	NvU16		flags;
	NvU16		pad;
	NvU32		cePceMask;
} UvmGpuCopyEngineCaps;

#define	UVM_CE_CAP_SUPPORTED	(1u << 0)
#define	UVM_CE_CAP_GRCE		(1u << 1)
#define	UVM_CE_CAP_SHARED	(1u << 2)
#define	UVM_CE_CAP_SYSMEM_READ	(1u << 3)
#define	UVM_CE_CAP_SYSMEM_WRITE	(1u << 4)
#define	UVM_CE_CAP_SYSMEM	(1u << 5)
#define	UVM_CE_CAP_NVLINK_P2P	(1u << 6)
#define	UVM_CE_CAP_P2P		(1u << 7)
#define	UVM_CE_CAP_SECURE	(1u << 8)
#define	UVM_CE_CAP_HAS(c, bit)	(((c)->flags & UVM_CE_CAP_##bit) != 0)

#define	UVM_COPY_ENGINE_COUNT_MAX	64
typedef struct {
	UvmGpuCopyEngineCaps copyEngineCaps[UVM_COPY_ENGINE_COUNT_MAX];
} UvmGpuCopyEnginesCaps;

/* UvmGpuChannelInfo (120) */
typedef struct {
	volatile NvU32	*gpGet;			/* @0 */
	volatile NvU32	*gpPut;			/* @8 */
	UvmGpuPointer	*gpFifoEntries;		/* @16 */
	NvU32		numGpFifoEntries;	/* @24 */
	NvU32		channelClassNum;	/* @28 */
	UvmRmNotification *errorNotifier;	/* @32 */
	UvmRmNotification *keyRotationNotifier;	/* @40 */
	NvU32		hwRunlistId;		/* @48 */
	NvU32		hwChannelId;		/* @52 */
	volatile NvU32	*dummyBar1Mapping;	/* @56 */
	volatile NvU32	*workSubmissionOffset;	/* @64 */
	NvU32		workSubmissionToken;	/* @72 */
	volatile NvU32	*pWorkSubmissionToken;	/* @80 */
	NvU64		gpFifoGpuVa;		/* @88 */
	NvU64		gpPutGpuVa;		/* @96 */
	NvU64		gpGetGpuVa;		/* @104 */
	NvU64		workSubmissionOffsetGpuVa; /* @112 */
} UvmGpuChannelInfo;

/* UvmGpuTsgAllocParams (8), UvmGpuChannelAllocParams (12) */
enum { UVM_GPU_CHANNEL_ENGINE_TYPE_GR = 1, UVM_GPU_CHANNEL_ENGINE_TYPE_CE = 2,
	UVM_GPU_CHANNEL_ENGINE_TYPE_SEC2 = 3 };
enum { UVM_BUFFER_LOCATION_DEFAULT = 0, UVM_BUFFER_LOCATION_SYS = 1,
	UVM_BUFFER_LOCATION_VID = 2 };

typedef struct {
	NvU32	engineType;
	NvU32	engineIndex;
} UvmGpuTsgAllocParams;

typedef struct {
	NvU32	numGpFifoEntries;
	NvU32	gpFifoLoc;
	NvU32	gpPutLoc;
} UvmGpuChannelAllocParams;

/* UvmGpuMemoryInfo (56) */
typedef struct {
	NvU32		kind;			/* @0 */
	NvBool		sysmem;			/* @4 */
	NvBool		egm;			/* @5 */
	NvBool		deviceDescendant;	/* @6 */
	NvU64		pageSize;		/* @8 */
	NvBool		contig;			/* @16 */
	NvU64		physAddr;		/* @24 */
	NvU64		size;			/* @32 */
	NvProcessorUuid	uuid;			/* @40 */
} UvmGpuMemoryInfo;

/* UvmGpuChannelResourceInfo (80) */
typedef struct {
	NvP64		resourceDescriptor;	/* @0, opaque RM descriptor */
	NvU32		resourceId;		/* @8 */
	NvU64		alignment;		/* @16 */
	UvmGpuMemoryInfo resourceInfo;		/* @24 */
} UvmGpuChannelResourceInfo;

/* UvmGpuChannelInstanceInfo (1120) */
#define	UVM_GPU_CHANNEL_MAX_RESOURCES	13
typedef struct {
	NvU64		base;			/* @0 instance block physical address */
	NvBool		sysmem;			/* @8 */
	NvU32		runlistId;		/* @12 */
	NvU32		chId;			/* @16 */
	NvBool		bInSubctx;		/* @20 */
	NvU32		subctxId;		/* @24 */
	NvBool		bTsgChannel;		/* @28 */
	NvU32		tsgId;			/* @32 */
	NvU32		tsgMaxSubctxCount;	/* @36 */
	UvmGpuChannelResourceInfo resourceInfo[UVM_GPU_CHANNEL_MAX_RESOURCES]; /* @40 */
	NvU32		resourceCount;		/* @1080 */
	NvU32		channelEngineType;	/* @1084 */
	NvU32		clearFaultedToken;	/* @1088 */
	volatile NvU32	*pChramChannelRegister;	/* @1096 */
	volatile NvU32	*pRunlistPRIBaseRegister; /* @1104 */
	NvU32		smcEngineId;		/* @1112 */
	NvU32		smcEngineVeIdOffset;	/* @1116 */
} UvmGpuChannelInstanceInfo;

/* UvmGpuChannelResourceBindParams (16) */
typedef struct {
	NvU32	resourceId;
	NvU64	resourceVa;
} UvmGpuChannelResourceBindParams;

/* UvmGpuExternalMappingInfo (72) */
typedef struct {
	NvU32		cachingType;		/* IN @0 */
	NvU32		mappingType;		/* IN @4 */
	NvU32		formatType;		/* IN @8 */
	NvU32		elementBits;		/* IN @12 */
	NvU32		compressionType;	/* IN @16 */
	NvU64		pteBufferSize;		/* IN @24, bytes */
	NvU64		mappingPageSize;	/* IN @32 */
	NvU64		*pteBuffer;		/* IN @40 */
	NvU64		numWrittenPtes;		/* OUT @48 */
	NvU64		numRemainingPtes;	/* OUT @56 */
	NvU32		pteSize;		/* OUT @64 */
	NvBool		bNeedL2InvalidateAtUnmap; /* OUT @68 */
} UvmGpuExternalMappingInfo;

/* UvmGpuFaultInfo (184) */
typedef struct {
	struct {
		volatile NvU32	*pFaultBufferGet;	/* @0 */
		volatile NvU32	*pFaultBufferPut;	/* @8 */
		volatile NvU32	*pFaultBufferInfo;	/* @16, deprecated */
		volatile NvU32	*pPmcIntr;		/* @24 */
		volatile NvU32	*pPmcIntrEnSet;		/* @32 */
		volatile NvU32	*pPmcIntrEnClear;	/* @40 */
		volatile NvU32	*pPrefetchCtrl;		/* @48 */
		NvU32		replayableFaultMask;	/* @56 */
		void		*bufferAddress;		/* @64 */
		NvU32		bufferSize;		/* @72, bytes */
		void		*bufferMetadata;	/* @80, CC only */
		NvU8		cslCtx[16];		/* @88, CC only */
	} replayable;
	struct {
		void		*shadowBufferAddress;	/* @104 */
		void		*shadowBufferContext;	/* @112 */
		NvU32		bufferSize;		/* @120 */
		void		*isr_sp;		/* @128, glue-allocated RM stack */
		void		*isr_bh_sp;		/* @136, glue-allocated RM stack */
		volatile NvU32	*pFaultBufferPut;	/* @144, CC only */
		NvU32		shadowBufferGet;	/* @152, CC only */
		void		*shadowBufferMetadata;	/* @160, CC only */
	} nonReplayable;
	NvHandle	faultBufferHandle;		/* @168, non-zero = initialised */
	void		*pDevice;			/* @176 */
} UvmGpuFaultInfo;

/* UvmPmaAllocationOptions (56) */
typedef struct {
	NvU32	flags;			/* @0 */
	NvU32	minimumSpeed;		/* @4 */
	NvU64	physBegin;		/* @8 */
	NvU64	physEnd;		/* @16 */
	NvU32	regionId;		/* @24 */
	NvU64	alignment;		/* @32 */
	NvU64	numPagesAllocated;	/* OUT @40 */
	NvU32	resultFlags;		/* OUT @48 */
} UvmPmaAllocationOptions;

#define	UVM_PMA_ALLOCATE_DONT_EVICT		(1u << 0)
#define	UVM_PMA_ALLOCATE_PINNED			(1u << 1)
#define	UVM_PMA_ALLOCATE_SPECIFY_MINIMUM_SPEED	(1u << 2)
#define	UVM_PMA_ALLOCATE_SPECIFY_ADDRESS_RANGE	(1u << 3)
#define	UVM_PMA_ALLOCATE_SPECIFY_REGION_ID	(1u << 4)
#define	UVM_PMA_ALLOCATE_PREFER_SLOWEST		(1u << 5)
#define	UVM_PMA_ALLOCATE_CONTIGUOUS		(1u << 6)
#define	UVM_PMA_ALLOCATE_PERSISTENT		(1u << 7)
#define	UVM_PMA_ALLOCATE_PROTECTED_REGION	(1u << 8)
#define	UVM_PMA_ALLOCATE_FORCE_ALIGNMENT	(1u << 9)
#define	UVM_PMA_ALLOCATE_NO_ZERO		(1u << 10)
#define	UVM_PMA_ALLOCATE_TURN_BLACKLIST_OFF	(1u << 11)
#define	UVM_PMA_ALLOCATE_ALLOW_PARTIAL		(1u << 12)
#define	UVM_PMA_ALLOCATE_RESULT_IS_ZERO		(1u << 0)
#define	UVM_PMA_FREE_IS_ZERO			(1u << 0)
#define	UVM_PMA_CALLED_FROM_PMA_EVICTION	16384

/* UvmPmaStatistics (48): read-only to UVM */
typedef struct {
	volatile NvU64	numPages2m;
	volatile NvU64	numFreePages64k;
	volatile NvU64	numFreePages2m;
	volatile NvU64	numPages2mProtected;
	volatile NvU64	numFreePages64kProtected;
	volatile NvU64	numFreePages2mProtected;
} UvmPmaStatistics;

/* UvmPlatformInfo (2) */
typedef struct {
	NvBool	atsSupported;
	NvBool	confComputingEnabled;
} UvmPlatformInfo;

/*
 * UvmGpuPlatformInfo (24): the FreeBSD glue's output of
 * nvUvmInterfaceRegisterGpu.  It never crosses into the RM core, so its
 * shape is this port's own: the PCI device_t and the DMA window RM
 * programmed for the GPU.
 */
typedef struct {
	void	*dev;				/* device_t */
	NvU64	dma_addressable_start;
	NvU64	dma_addressable_limit;
} UvmGpuPlatformInfo;

/* UvmGpuP2PCapsParams (64) */
typedef struct {
	NvU32	peerIds[2];		/* @0 */
	NvU32	egmPeerIds[2];		/* @8 */
	NvU32	p2pLink;		/* @16 */
	NvU32	optimalNvlinkWriteCEs[2]; /* @20 */
	NvU32	totalLinkLineRateMBps;	/* @28 */
	NvU64	bar1DmaAddress[2];	/* @32 */
	NvU64	bar1DmaSize[2];		/* @48 */
} UvmGpuP2PCapsParams;

/* UvmGpuAccessCntrInfo (72), UvmGpuAccessCntrConfig (8) */
typedef struct {
	volatile NvU32	*pAccessCntrBufferGet;	/* @0 */
	volatile NvU32	*pAccessCntrBufferPut;	/* @8 */
	volatile NvU32	*pAccessCntrBufferFull;	/* @16 */
	volatile NvU32	*pHubIntr;		/* @24 */
	volatile NvU32	*pHubIntrEnSet;		/* @32 */
	volatile NvU32	*pHubIntrEnClear;	/* @40 */
	NvU32		accessCounterMask;	/* @48 */
	void		*bufferAddress;		/* @56 */
	NvU32		bufferSize;		/* @64 */
	NvHandle	accessCntrBufferHandle;	/* @68 */
} UvmGpuAccessCntrInfo;

typedef struct {
	NvU32	granularity;	/* 0 64K, 1 2M, 2 16M, 3 16G */
	NvU32	threshold;	/* 1..65535 */
} UvmGpuAccessCntrConfig;

/* PMA eviction callbacks UVM registers (02 section 3.9) */
typedef NV_STATUS (*uvmPmaEvictPagesCallback)(void *callbackData, NvU64 pageSize,
    NvU64 *pPages, NvU32 count, NvU64 physBegin, NvU64 physEnd, NvU32 memType);
typedef NV_STATUS (*uvmPmaEvictRangeCallback)(void *callbackData, NvU64 physBegin,
    NvU64 physEnd, NvU32 memType);

/*
 * The callback table UVM installs with nvUvmInterfaceRegisterUvmCallbacks
 * (02 section 5).  isrTopHalf is called from nvidia.ko's interrupt path
 * before RM's own servicing; it returns NV_OK when it scheduled servicing,
 * NV_WARN_MORE_PROCESSING_REQUIRED when UVM work was pending but could not
 * be taken, NV_ERR_NO_INTR_PENDING when there was no UVM work.
 */
struct UvmOpsUvmEvents {
	NV_STATUS (*suspend)(void);				/* @0 */
	NV_STATUS (*resume)(void);				/* @8 */
	NV_STATUS (*startDevice)(const NvProcessorUuid *);	/* @16, may be NULL */
	NV_STATUS (*stopDevice)(const NvProcessorUuid *);	/* @24, may be NULL */
	NV_STATUS (*isrTopHalf)(const NvProcessorUuid *);	/* @32 */
	NV_STATUS (*drainP2P)(const NvProcessorUuid *);		/* @40 */
	NV_STATUS (*resumeP2P)(const NvProcessorUuid *);	/* @48 */
};

/* module lifetime (02 section 3.1) */
NV_STATUS nvUvmInterfaceSessionCreate(uvmGpuSessionHandle *session, UvmPlatformInfo *platformInfo);
void nvUvmInterfaceSessionDestroy(uvmGpuSessionHandle session);
NV_STATUS nvUvmInterfaceRegisterUvmCallbacks(const struct UvmOpsUvmEvents *ops);
void nvUvmInterfaceDeRegisterUvmOps(void);

/* GPU lifetime (02 section 3.2) */
NV_STATUS nvUvmInterfaceRegisterGpu(const NvProcessorUuid *uuid, UvmGpuPlatformInfo *platformInfo);
void nvUvmInterfaceUnregisterGpu(const NvProcessorUuid *uuid);
NV_STATUS nvUvmInterfaceGetGpuInfo(const NvProcessorUuid *uuid,
    const UvmGpuClientInfo *clientInfo, UvmGpuInfo *gpuInfo);
NV_STATUS nvUvmInterfaceDeviceCreate(uvmGpuSessionHandle session, const UvmGpuInfo *gpuInfo,
    const NvProcessorUuid *uuid, uvmGpuDeviceHandle *device, NvBool bCreateSmcPartition);
void nvUvmInterfaceDeviceDestroy(uvmGpuDeviceHandle device);
NV_STATUS nvUvmInterfaceGetFbInfo(uvmGpuDeviceHandle device, UvmGpuFbInfo *fbInfo);
NV_STATUS nvUvmInterfaceQueryCaps(uvmGpuDeviceHandle device, UvmGpuCaps *caps);
NV_STATUS nvUvmInterfaceAddressSpaceCreate(uvmGpuDeviceHandle device, NvU64 vaBase, NvU64 vaSize,
    NvBool enableAts, uvmGpuAddressSpaceHandle *vaSpace, UvmGpuAddressSpaceInfo *vaSpaceInfo);
void nvUvmInterfaceAddressSpaceDestroy(uvmGpuAddressSpaceHandle vaSpace);
NV_STATUS nvUvmInterfaceGetEccInfo(uvmGpuDeviceHandle device, UvmGpuEccInfo *eccInfo);
NV_STATUS nvUvmInterfaceGetNvlinkInfo(uvmGpuDeviceHandle device, UvmGpuNvlinkInfo *nvlinkInfo);
NV_STATUS nvUvmInterfaceGetPmaObject(uvmGpuDeviceHandle device, void **pPma,
    const UvmPmaStatistics **pPmaPubStats);
NV_STATUS nvUvmInterfacePmaRegisterEvictionCallbacks(void *pPma, uvmPmaEvictPagesCallback evictPages,
    uvmPmaEvictRangeCallback evictRange, void *callbackData);
void nvUvmInterfacePmaUnregisterEvictionCallbacks(void *pPma);
NV_STATUS nvUvmInterfaceQueryCopyEnginesCaps(uvmGpuDeviceHandle device, UvmGpuCopyEnginesCaps *caps);

/* RM-backed memory for UVM-internal buffers (02 section 3.3) */
NV_STATUS nvUvmInterfaceMemoryAllocSys(uvmGpuAddressSpaceHandle vaSpace, NvLength length,
    UvmGpuPointer *gpuPointer, UvmGpuAllocInfo *allocInfo);
NV_STATUS nvUvmInterfaceMemoryAllocFB(uvmGpuAddressSpaceHandle vaSpace, NvLength length,
    UvmGpuPointer *gpuPointer, UvmGpuAllocInfo *allocInfo);
void nvUvmInterfaceMemoryFree(uvmGpuAddressSpaceHandle vaSpace, UvmGpuPointer gpuPointer);
NV_STATUS nvUvmInterfaceMemoryCpuMap(uvmGpuAddressSpaceHandle vaSpace, UvmGpuPointer gpuPointer,
    NvLength length, void **cpuPtr, NvU64 pageSize);
void nvUvmInterfaceMemoryCpuUnMap(uvmGpuAddressSpaceHandle vaSpace, void *cpuPtr);
NV_STATUS nvUvmInterfaceDupAllocation(uvmGpuAddressSpaceHandle srcVaSpace, NvU64 srcAddress,
    uvmGpuAddressSpaceHandle dstVaSpace, NvU64 dstVaAlignment, NvU64 *dstAddress);

/* channels (02 section 3.4) */
NV_STATUS nvUvmInterfaceTsgAllocate(uvmGpuAddressSpaceHandle vaSpace,
    const UvmGpuTsgAllocParams *allocParams, uvmGpuTsgHandle *tsg);
void nvUvmInterfaceTsgDestroy(uvmGpuTsgHandle tsg);
NV_STATUS nvUvmInterfaceChannelAllocate(uvmGpuTsgHandle tsg, const UvmGpuChannelAllocParams *allocParams,
    uvmGpuChannelHandle *channel, UvmGpuChannelInfo *channelInfo);
void nvUvmInterfaceChannelDestroy(uvmGpuChannelHandle channel);

/* page directory hand-over (02 section 3.5) */
NV_STATUS nvUvmInterfaceSetPageDirectory(uvmGpuAddressSpaceHandle vaSpace, NvU64 physAddress,
    unsigned numEntries, NvBool bVidMemAperture, NvU32 pasid, NvU64 *dmaAddress);
NV_STATUS nvUvmInterfaceUnsetPageDirectory(uvmGpuAddressSpaceHandle vaSpace);
NV_STATUS nvUvmInterfaceDupAddressSpace(uvmGpuDeviceHandle device, NvHandle hUserClient,
    NvHandle hUserVASpace, uvmGpuAddressSpaceHandle *vaSpace, UvmGpuAddressSpaceInfo *vaSpaceInfo);

/* fault buffers (02 section 3.6) */
NV_STATUS nvUvmInterfaceInitFaultInfo(uvmGpuDeviceHandle device, UvmGpuFaultInfo *faultInfo);
NV_STATUS nvUvmInterfaceDestroyFaultInfo(uvmGpuDeviceHandle device, UvmGpuFaultInfo *faultInfo);
NV_STATUS nvUvmInterfaceHasPendingNonReplayableFaults(UvmGpuFaultInfo *faultInfo, NvBool *hasPending);
NV_STATUS nvUvmInterfaceGetNonReplayableFaults(UvmGpuFaultInfo *faultInfo, void *buffer, NvU32 *numFaults);
NV_STATUS nvUvmInterfaceFlushReplayableFaultBuffer(UvmGpuFaultInfo *faultInfo, NvBool bCopyAndFlush);
NV_STATUS nvUvmInterfaceTogglePrefetchFaults(UvmGpuFaultInfo *faultInfo, NvBool bEnable);
NV_STATUS nvUvmInterfaceReportNonReplayableFault(uvmGpuDeviceHandle device, const void *faultPacket);

/* interrupt ownership (02 section 3.7) */
NV_STATUS nvUvmInterfaceOwnPageFaultIntr(uvmGpuDeviceHandle device, NvBool bOwnInterrupts);
NV_STATUS nvUvmInterfaceServiceDeviceInterruptsRM(uvmGpuDeviceHandle device);

/* user objects: channels and external memory (02 section 3.8) */
NV_STATUS nvUvmInterfaceRetainChannel(uvmGpuAddressSpaceHandle vaSpace, NvHandle hClient,
    NvHandle hChannel, uvmGpuRetainedChannelHandle *retainedChannel,
    UvmGpuChannelInstanceInfo *instanceInfo);
void nvUvmInterfaceReleaseChannel(uvmGpuRetainedChannelHandle retainedChannel);
NV_STATUS nvUvmInterfaceGetChannelResourcePtes(uvmGpuAddressSpaceHandle vaSpace,
    NvP64 resourceDescriptor, NvU64 offset, NvU64 size, UvmGpuExternalMappingInfo *mappingInfo);
NV_STATUS nvUvmInterfaceBindChannelResources(uvmGpuRetainedChannelHandle retainedChannel,
    UvmGpuChannelResourceBindParams *bindParams);
void nvUvmInterfaceStopChannel(uvmGpuRetainedChannelHandle retainedChannel, NvBool bImmediate);
NV_STATUS nvUvmInterfaceDupMemory(uvmGpuDeviceHandle device, NvHandle hClient, NvHandle hPhysMemory,
    NvHandle *hDupMemory, UvmGpuMemoryInfo *memoryInfo);
NV_STATUS nvUvmInterfaceFreeDupedHandle(uvmGpuDeviceHandle device, NvHandle hDupMemory);
NV_STATUS nvUvmInterfaceGetExternalAllocPtes(uvmGpuAddressSpaceHandle vaSpace, NvHandle hDupedMemory,
    NvU64 offset, NvU64 size, UvmGpuExternalMappingInfo *mappingInfo);

/* the physical memory allocator (02 section 3.9) */
NV_STATUS nvUvmInterfacePmaAllocPages(void *pPma, NvLength pageCount, NvU64 pageSize,
    UvmPmaAllocationOptions *options, NvU64 *pPages);
NV_STATUS nvUvmInterfacePmaPinPages(void *pPma, NvU64 *pPages, NvLength pageCount, NvU64 pageSize,
    NvU32 flags);
void nvUvmInterfacePmaFreePages(void *pPma, NvU64 *pPages, NvLength pageCount, NvU64 pageSize,
    NvU32 flags);

/* peers and access counters (02 section 3.10) */
NV_STATUS nvUvmInterfaceP2pObjectCreate(uvmGpuDeviceHandle device1, uvmGpuDeviceHandle device2,
    NvHandle *hP2pObject);
void nvUvmInterfaceP2pObjectDestroy(uvmGpuSessionHandle session, NvHandle hP2pObject);
NV_STATUS nvUvmInterfaceGetP2PCaps(uvmGpuDeviceHandle device1, uvmGpuDeviceHandle device2,
    UvmGpuP2PCapsParams *params);
NV_STATUS nvUvmInterfaceInitAccessCntrInfo(uvmGpuDeviceHandle device, UvmGpuAccessCntrInfo *info,
    NvU32 index);
NV_STATUS nvUvmInterfaceDestroyAccessCntrInfo(uvmGpuDeviceHandle device, UvmGpuAccessCntrInfo *info);
NV_STATUS nvUvmInterfaceEnableAccessCntr(uvmGpuDeviceHandle device, UvmGpuAccessCntrInfo *info,
    const UvmGpuAccessCntrConfig *config);
NV_STATUS nvUvmInterfaceDisableAccessCntr(uvmGpuDeviceHandle device, UvmGpuAccessCntrInfo *info);

/* optional: tells RM an unrecoverable error happened (02 section 3.11) */
void nvUvmInterfaceReportFatalError(NV_STATUS error);

#endif /* _NVIDIA_UVM_RM_H_ */
