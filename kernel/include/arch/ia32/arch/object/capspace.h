/*
 * Copyright 2014, General Dynamics C4 Systems
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(GD_GPL)
 */

#ifndef __ARCH_OBJECT_CAPSPACE_H
#define __ARCH_OBJECT_CAPSPACE_H

enum capSpaceType {
    capSpaceUntypedMemory,
    capSpaceTypedMemory,
    capSpaceReply,
    capSpaceIRQ,
    capSpaceIOPort,
#ifdef CONFIG_IOMMU
    capSpaceIOSpace,
#endif
    capSpaceIPI,
    capSpaceDomain,
};

static inline int CONST
cap_get_capSpaceType(cap_t cap)
{
    switch (cap_get_capType(cap)) {
    case cap_null_cap:
        fail("null cap has no type");
    case cap_endpoint_cap:
    case cap_async_endpoint_cap:
    case cap_cnode_cap:
    case cap_thread_cap:
    case cap_frame_cap:
    case cap_page_table_cap:
    case cap_page_directory_cap:
    case cap_zombie_cap:
        return capSpaceTypedMemory;

    case cap_domain_cap:
        return capSpaceDomain;

    case cap_untyped_cap:
        return capSpaceUntypedMemory;

    case cap_irq_control_cap:
    case cap_irq_handler_cap:
        return capSpaceIRQ;
    case cap_reply_cap:
        return capSpaceReply;
#ifdef CONFIG_VTX
    case cap_vcpu_cap:
        return capSpaceTypedMemory;
#endif
    case cap_io_port_cap:
        return capSpaceIOPort;
#ifdef CONFIG_IOMMU
    case cap_io_space_cap:
        return capSpaceIOSpace;
    case cap_io_page_table_cap:
        return capSpaceTypedMemory;
#endif
#ifdef CONFIG_VTX
    case cap_ept_page_directory_pointer_table_cap:
    case cap_ept_page_directory_cap:
    case cap_ept_page_table_cap:
        return capSpaceTypedMemory;
#endif
    case cap_ipi_cap:
        return capSpaceIPI;

    default:
        fail("Invalid arch cap type");
    }
}

static inline void * CONST
cap_get_capSpacePtr(cap_t cap)
{
    switch (cap_get_capType(cap)) {
    case cap_domain_cap:
        return (void*)0;
    case cap_irq_control_cap:
        return (void*)0;
    case cap_irq_handler_cap:
        return (void*)cap_irq_handler_cap_get_capIRQ(cap);
    case cap_reply_cap:
        return (void*)cap_reply_cap_get_capTCBPtr(cap);
    case cap_io_port_cap:
        return (void*)cap_io_port_cap_get_capIOPortFirstPort(cap);
#ifdef CONFIG_IOMMU
    case cap_io_space_cap:
        return (void*)cap_io_space_cap_get_capPCIDevice(cap);
#endif
    case cap_ipi_cap:
        return (void*)0;
    default:
        return cap_get_capPtr(cap);
    }
}

static inline unsigned int CONST
cap_get_capSpaceSize(cap_t cap)
{
    switch (cap_get_capType(cap)) {
    case cap_irq_control_cap:
        return 0xff;
    case cap_irq_handler_cap:
        return 1;
    case cap_reply_cap:
        return 1;
    case cap_domain_cap:
        return 1;
    case cap_io_port_cap:
        return cap_io_port_cap_get_capIOPortLastPort(cap) - cap_io_port_cap_get_capIOPortFirstPort(cap);
#ifdef CONFIG_IOMMU
    case cap_io_space_cap:
        return 1;
#endif
    case cap_ipi_cap:
        return 1;
    default:
        return BIT(cap_get_capSizeBits(cap));
    }
}

static inline unsigned int CONST
cap_get_capExtraComp(cap_t cap)
{
    switch (cap_get_capType(cap)) {
    case cap_frame_cap:
        if (!cap_frame_cap_get_capFMappedObject(cap)) {
            return 0;
        }
        switch (cap_frame_cap_get_capFMappedType(cap)) {
        case IA32_MAPPING_PD:
            switch (cap_frame_cap_get_capFSize(cap)) {
            case IA32_4K:
                return PTE_REF(PTE_PTR(cap_frame_cap_get_capFMappedObject(cap)) + cap_frame_cap_get_capFMappedIndex(cap));
            case IA32_4M:
                return PDE_REF(PDE_PTR(cap_frame_cap_get_capFMappedObject(cap)) + cap_frame_cap_get_capFMappedIndex(cap));
            default:
                fail ("Unknown frame size");
            }
#ifdef CONFIG_VTX
        case IA32_MAPPING_EPT:
            switch (cap_frame_cap_get_capFSize(cap)) {
            case IA32_4K:
                return EPT_PTE_REF(EPT_PTE_PTR(cap_frame_cap_get_capFMappedObject(cap)) + cap_frame_cap_get_capFMappedIndex(cap));
            case IA32_4M:
                return EPT_PDE_REF(EPT_PDE_PTR(cap_frame_cap_get_capFMappedObject(cap)) + cap_frame_cap_get_capFMappedIndex(cap));
            default:
                fail ("Unknown frame size");
            }
#endif
#ifdef CONFIG_IOMMU
        case IA32_MAPPING_IO:
            switch (cap_frame_cap_get_capFSize(cap)) {
            case IA32_4K:
                return VTD_PTE_REF(VTD_PTE_PTR(cap_frame_cap_get_capFMappedObject(cap)) + cap_frame_cap_get_capFMappedIndex(cap));
            case IA32_4M:
                return VTD_PTE_REF(VTD_PTE_PTR(cap_frame_cap_get_capFMappedObject(cap)) + cap_frame_cap_get_capFMappedIndex(cap));
            default:
                fail ("Unknown frame size");
            }
#endif
        default:
            fail("Unknown mapping type for frame");
        }
    case cap_page_table_cap:
        if (!cap_page_table_cap_get_capPTMappedObject(cap)) {
            return 0;
        }
        return PDE_REF(PDE_PTR(cap_page_table_cap_get_capPTMappedObject(cap)) + cap_page_table_cap_get_capPTMappedIndex(cap));
#ifdef CONFIG_VTX
    case cap_ept_page_directory_cap:
        if (!cap_ept_page_directory_cap_get_capPDMappedObject(cap)) {
            return 0;
        }
        return EPT_PDPTE_REF(EPT_PDPTE_PTR(cap_ept_page_directory_cap_get_capPDMappedObject(cap))
                             + cap_ept_page_directory_cap_get_capPDMappedIndex(cap));
    case cap_ept_page_table_cap:
        if (!cap_ept_page_table_cap_get_capPTMappedObject(cap)) {
            return 0;
        }
        return EPT_PDE_REF(EPT_PDE_PTR(cap_ept_page_table_cap_get_capPTMappedObject(cap))
                           + cap_ept_page_table_cap_get_capPTMappedIndex(cap));
#endif
#ifdef CONFIG_IOMMU
    case cap_io_space_cap:
        return cap_io_space_cap_get_capDomainID(cap);
    case cap_io_page_table_cap:
        if (!cap_io_page_table_cap_get_capIOPTMappedObject(cap)) {
            return 0;
        }
        if (cap_io_page_table_cap_get_capIOPTLevel(cap) == 0) {
            return VTD_CTE_REF(VTD_CTE_PTR(cap_io_page_table_cap_get_capIOPTMappedObject(cap))
                               + cap_io_page_table_cap_get_capIOPTMappedIndex(cap));
        } else {
            return VTD_PTE_REF(VTD_PTE_PTR(cap_io_page_table_cap_get_capIOPTMappedObject(cap))
                               + cap_io_page_table_cap_get_capIOPTMappedIndex(cap));
        }
#endif
    default:
        return 0;
    }
}

static inline unsigned int const
cte_depth_bits_type(cap_tag_t ctag)
{

    switch (ctag) {
#ifdef CONFIG_VTX
    case cap_ept_page_directory_cap:
    case cap_ept_page_table_cap:
        return 3;
#endif /* CONFIG_VTX */
#ifdef CONFIG_IOMMU
    case cap_io_page_table_cap:
        return 3;
#endif /* CONFIG_IOMMU */
    case cap_page_table_cap:
        return 3;
    case cap_frame_cap:
#ifdef CONFIG_VTX
        /* EPT paging strucutures only have 3 bits free, and we do not
         * know in advance where this will get mapped */
        return 2;
#else
        return 3;
#endif /* CONFIG_VTX */
    default:
        return CTE_DEPTH_BITS;

    }

}

static inline unsigned int const
cte_depth_bits_cap(cap_t cap)
{
    return cte_depth_bits_type(cap_get_capType(cap));
}

#endif /* __ARCH_OBJECT_STRUCTURES_H */

