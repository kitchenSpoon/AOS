/*
 * Copyright 2014, General Dynamics C4 Systems
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(GD_GPL)
 */

#include <config.h>

#ifdef CONFIG_VTX

#include <stdint.h>
#include <machine/io.h>
#include <kernel/boot.h>
#include <kernel/faulthandler.h>
#include <kernel/thread.h>
#include <arch/machine.h>
#include <arch/machine/cpu_registers.h>
#include <api/failures.h>
#include <api/syscall.h>
#include <model/statedata.h>
#include <arch/machine/registerset.h>
#include <arch/object/vtx.h>
#include <arch/object/vcpu.h>
#include <arch/machine/fpu.h>

#define MSR_FEATURE_CONTROL 0x3A
#define MSR_VM_BASIC 0x480
#define FEATURE_CONTROL_MASK (BIT(0) | BIT(2))

uint32_t vtx_enabled = 0;
/* Address of the current VMCS: the one loaded by the CPU. */
void *current_vmcs = NULL;
uint32_t null_ept_space = 0;
uint32_t vmcs_revision = 0;
uint32_t* msr_bitmap = NULL;
uint64_t vpid_capability = 0;
uint32_t vtx_memory[3];

static int is_vtx_supported(void)
{
    uint32_t reg_ecx, reg_edx;

    asm volatile (
        "cpuid\n"
        : "=c"(reg_ecx), "=d"(reg_edx)
        : "a"(1)
        : "ebx"
    );
    return (reg_ecx & reg_edx & BIT(5));
}

BOOT_CODE bool_t vtx_allocate(void)
{
    vtx_memory[0] = alloc_region(PAGE_BITS);
    vtx_memory[1] = alloc_region(PAGE_BITS);
    vtx_memory[2] = alloc_region(PAGE_BITS);
    return vtx_memory[0] && vtx_memory[1] && vtx_memory[2];
}

BOOT_CODE void vtx_enable(void)
{
    if (is_vtx_supported()) {
        uint64_t feature_control = ((uint64_t)ia32_rdmsr_high(MSR_FEATURE_CONTROL)) << 32 | (uint64_t)ia32_rdmsr_low(MSR_FEATURE_CONTROL);
        uint64_t vm_basic = ((uint64_t)ia32_rdmsr_high(MSR_VM_BASIC)) << 32 | (uint64_t)ia32_rdmsr_low(MSR_VM_BASIC);
        vmcs_revision = vm_basic;
        if ((feature_control & FEATURE_CONTROL_MASK) == FEATURE_CONTROL_MASK) {
            uint32_t vm_basic_high = vm_basic >> 32;
            if (init_vtx_fixed_values((vm_basic_high & BIT(55 - 32)) == BIT(55 - 32))) {
                uint32_t *vmxon_region;
                uint64_t vmxon_region_arg;
                uint8_t error = 0;
                vmxon_region = (uint32_t *)vtx_memory[0];
                vmxon_region[0] = vmcs_revision;
                vmxon_region_arg = (uint64_t)(
                                       pptr_to_paddr(vmxon_region) & 0xFFFFF000L);
                write_cr4(read_cr4() | CR4_VMXE);
                asm volatile(
                    "vmxon %1; setnae %0"
                    : "=g"(error)
                    : "m"(vmxon_region_arg)
                    : "memory", "cc");
                if (error) {
                    printf("vt-x: vmxon failure\n");
                } else {
                    void *null_ept_space_ptr;
                    printf("vt-x: on!\n");
                    msr_bitmap = (uint32_t*)vtx_memory[1];
                    memset(msr_bitmap, ~0, BIT(PAGE_BITS));
                    /* Set sysenter MSRs to writeable and readable. */
                    msr_bitmap[11] = 0xff8fffff;
                    msr_bitmap[512 + 11] = 0xff8fffff;
                    null_ept_space_ptr = (void*)vtx_memory[2];
                    memset(null_ept_space_ptr, 0, BIT(PAGE_BITS));
                    null_ept_space = pptr_to_paddr(null_ept_space_ptr);
                    null_ept_space |= (3 << 3) | 6;
                    vtx_enabled = 1;
                }
                vpid_capability = ((uint64_t)ia32_rdmsr_high(MSR_VMX_EPT_VPID_CAP)) << 32 | (uint64_t)ia32_rdmsr_low(MSR_VMX_EPT_VPID_CAP);
            } else {
                printf("vt-x: disabled due to lack of required features\n");
            }
        } else if (!(feature_control & 1)) {
            printf("vt-x: feature control not locked\n");
        } else if (!(feature_control & (1 << 2))) {
            printf("vt-x: disabled by feature control\n");
        }
    } else {
        printf("vt-x: not supported\n");
    }
}

exception_t
handleVmexit(void)
{
    enum exit_reasons reason;
    uint32_t qualification, interrupt;
    finishVmexitSaving();
    reason = vmread(VMX_DATA_EXIT_REASON) & MASK(16);
    if (reason == EXTERNAL_INTERRUPT) {
        interrupt = vmread(VMX_DATA_EXIT_INTERRUPT_INFO);
        ia32KScurInterrupt = interrupt & 0xff;
        return handleInterruptEntry();
    } else if (ksCurThread != ia32KSfpuOwner) {
        if (reason == EXCEPTION_OR_NMI && !(ksCurThread->tcbArch.vcpu->exception_mask & BIT(7))) {
            interrupt = vmread(VMX_DATA_EXIT_INTERRUPT_INFO);
            if ((interrupt & 0xff) == 0x7) {
                return handleUnimplementedDevice();
            }
        } else if (reason == CONTROL_REGISTER && !(ksCurThread->tcbArch.vcpu->cr0_mask & BIT(3))) {
            qualification = vmread(VMX_DATA_EXIT_QUALIFICATION);
            if ((qualification & 0xF) == 0) {
                switch ((qualification >> 4) & 0x3) {
                case 0: { /* mov to CR0 */
                    register_t source = crExitRegs[(qualification >> 8) & 0x7];
                    uint32_t value;
                    value = getRegister(ksCurThread, source);
                    ksCurThread->tcbArch.vcpu->cr0 = (ksCurThread->tcbArch.vcpu->cr0 & ~BIT(0x3)) |
                                                     (value & BIT(0x3));
                    if (!((value ^ ksCurThread->tcbArch.vcpu->cr0_shadow) &
                            ksCurThread->tcbArch.vcpu->cr0_mask)) {
                        return EXCEPTION_NONE;
                    }
                    break;
                }
                case 2: { /* CLTS */
                    ksCurThread->tcbArch.vcpu->cr0 = (ksCurThread->tcbArch.vcpu->cr0 & ~BIT(0x3));
                    return EXCEPTION_NONE;
                }
                case 3: { /* LMSW */
                    uint32_t value = (qualification >> 16) & MASK(16);
                    ksCurThread->tcbArch.vcpu->cr0 = (ksCurThread->tcbArch.vcpu->cr0 & ~BIT(0x3)) | value;
                    if (!((value ^ ksCurThread->tcbArch.vcpu->cr0_shadow) &
                            ksCurThread->tcbArch.vcpu->cr0_mask & MASK(16))) {
                        return EXCEPTION_NONE;
                    }
                    break;
                }
                }
            }
        }
    }
    switch (reason) {
    case EXCEPTION_OR_NMI:
        ksCurThread->tcbArch.vcpu->interrupt_info = vmread(VMX_DATA_EXIT_INTERRUPT_INFO);
    case MOV_DR:
    case TASK_SWITCH:
    case CONTROL_REGISTER:
    case IO:
    case MWAIT:
    case SIPI:
    case INVLPG:
    case INVEPT:
    case INVVPID:
    case VMCLEAR:
    case VMPTRLD:
    case VMPTRST:
    case VMREAD:
    case VMWRITE:
    case VMXON:
    case EPT_VIOLATION:
    case GDTR_OR_IDTR:
    case LDTR_OR_TR:
    case TPR_BELOW_THRESHOLD:
    case APIC_ACCESS:
        qualification = vmread(VMX_DATA_EXIT_QUALIFICATION);
        break;
    default:
        qualification = 0;
    }
    current_fault = fault_vmx_fault_new(qualification, reason);
    handleFault(ksCurThread);

    schedule();
    activateThread();

    return EXCEPTION_NONE;
}


exception_t
handleVmEntryFail(void)
{
    current_fault = fault_vmx_fault_new(-1, -1);
    handleFault(ksCurThread);

    schedule();
    activateThread();

    return EXCEPTION_NONE;
}

void
finishVmexitSaving(void)
{
    setRegister(ksCurThread, FaultEIP, vmread(VMX_GUEST_RIP));
    setRegister(ksCurThread, Error, -2);
    setRegister(ksCurThread, NextEIP, vmread(VMX_GUEST_RIP) + vmread(VMX_DATA_EXIT_INSTRUCTION_LENGTH));
    setRegister(ksCurThread, ESP, vmread(VMX_GUEST_RSP));
    ksCurThread->tcbArch.vcpu->launched = true;
    if (ksCurThread != ia32KSfpuOwner) {
        ksCurThread->tcbArch.vcpu->cr0 = (vmread(VMX_GUEST_CR0) & ~BIT(3)) | (ksCurThread->tcbArch.vcpu->cr0 & BIT(3));
    } else {
        ksCurThread->tcbArch.vcpu->cr0 = vmread(VMX_GUEST_CR0);
    }
}

static void
setIOPort(vcpu_t* vcpu)
{
    uint32_t high, low;
    if (cap_get_capType(vcpu->io_port) == cap_io_port_cap) {
        low = cap_io_port_cap_get_capIOPortFirstPort(vcpu->io_port);
        high = cap_io_port_cap_get_capIOPortLastPort(vcpu->io_port) + 1;
    } else {
        low = -1;
        high = -1;
    }
    /* Has the range changed at all */
    if (low != vcpu->io_min || high != vcpu->io_max) {
        /* We previously had some mappings, and now our range has changed
           Just knock all the ports out */
        if (vcpu->io_min != -1) {
            memset(&vcpu->io[vcpu->io_min / 32], ~0,
                   (1 + ((vcpu->io_max - 1) / 32) - (vcpu->io_min / 32)) * 4);
        }
        vcpu->io_min = low;
        vcpu->io_max = high;
    }
}

static void
setEPTRoot(cap_t vmxSpace, vcpu_t* vcpu)
{
    if (cap_get_capType(vmxSpace) != cap_ept_page_directory_pointer_table_cap) {
        vmwrite(VMX_CONTROL_EPT_POINTER, null_ept_space);
        vmwrite(VMX_CONTROL_VPID, (null_ept_space) >> 13);
    } else {
        vmwrite(VMX_CONTROL_EPT_POINTER, (pptr_to_paddr((void*)cap_ept_page_directory_pointer_table_cap_get_capPDPTBasePtr(vmxSpace)) - EPT_PDPT_OFFSET) | (3 << 3) | 6);
        vmwrite(VMX_CONTROL_VPID, (pptr_to_paddr((void*)cap_ept_page_directory_pointer_table_cap_get_capPDPTBasePtr(vmxSpace)) - EPT_PDPT_OFFSET) >> 13);
    }
}

static void
handleLazyFpu(void)
{
    if (ksCurThread != ia32KSfpuOwner) {
        vmwrite(VMX_GUEST_CR0, ksCurThread->tcbArch.vcpu->cr0 | BIT(3));
        vmwrite(VMX_CONTROL_EXCEPTION_BITMAP, ksCurThread->tcbArch.vcpu->exception_mask | BIT(0x7));
        if (ksCurThread->tcbArch.vcpu->cr0_mask & BIT(3)) {
            /* Don't replace the userland read shadow value if userland is
             * masking CR0.TS. The important thing is that it is masked so the
             * guest can't modify it. We don't care about the read shadow
             * value. */
            vmwrite(VMX_CONTROL_CR0_MASK, ksCurThread->tcbArch.vcpu->cr0_mask);
            vmwrite(VMX_CONTROL_CR0_READ_SHADOW, ksCurThread->tcbArch.vcpu->cr0_shadow);
        } else {
            vmwrite(VMX_CONTROL_CR0_MASK, ksCurThread->tcbArch.vcpu->cr0_mask | BIT(3));
            vmwrite(VMX_CONTROL_CR0_READ_SHADOW, (ksCurThread->tcbArch.vcpu->cr0 & BIT(3)) |
                    (ksCurThread->tcbArch.vcpu->cr0_shadow & ~BIT(3)));
        }
    } else {
        vmwrite(VMX_GUEST_CR0, ksCurThread->tcbArch.vcpu->cr0);
        vmwrite(VMX_CONTROL_EXCEPTION_BITMAP, ksCurThread->tcbArch.vcpu->exception_mask);
        vmwrite(VMX_CONTROL_CR0_MASK, ksCurThread->tcbArch.vcpu->cr0_mask);
        vmwrite(VMX_CONTROL_CR0_READ_SHADOW, ksCurThread->tcbArch.vcpu->cr0_shadow);
    }
}

void
restoreVMCS(void)
{
    vcpu_t *expected_vmcs = ksCurThread->tcbArch.vcpu;

    /* Check that the right VMCS is active and current. */
    if (current_vmcs != expected_vmcs) {
        vmptrld(expected_vmcs);
    }

    /* Copy registers back from the TCB. */
    vmwrite(VMX_GUEST_RIP, getRegister(ksCurThread, FaultEIP));
    vmwrite(VMX_GUEST_RSP, getRegister(ksCurThread, ESP));

    /* Set host SP to point just beyond the first field to be stored on exit. */
    vmwrite(VMX_HOST_RSP, (uint32_t)&ksCurThread->tcbArch.tcbContext.registers[DS]);
    vmwrite(VMX_HOST_CR3, get_cr3());
    setEPTRoot(TCB_PTR_CTE_PTR(ksCurThread, tcbVTable)->cap, ksCurThread->tcbArch.vcpu);
    setIOPort(ksCurThread->tcbArch.vcpu);
    handleLazyFpu();
}

uint32_t
vmread(uint32_t field)
{
    uint32_t value;
    asm volatile (
        "vmread %%eax, %%ecx"
        : "=c"(value)
        : "a"(field)
        : "cc"
    );
    return value;
}

void
vmwrite(uint32_t field, uint32_t value)
{
    uint8_t error = 0;
    asm volatile (
        "vmwrite %%eax, %%edx; setna %0"
        : "=q"(error)
        : "a"(value), "d"(field)
        : "cc"
    );
    if (error) {
        printf("error setting field %x\n", field);
    }
}

uint32_t
get_cr3(void)
{
    uint32_t reg_eax;

    asm volatile (
        "mov %%cr3, %%eax\n"
        : "=a"(reg_eax)
        :
        : "cc"
    );
    return reg_eax;
}

int
vmptrld(void *vmcs_ptr)
{
    uint64_t physical_address;
    uint8_t error;
    current_vmcs = vmcs_ptr;
    physical_address = pptr_to_paddr(vmcs_ptr);
    asm volatile (
        "vmptrld (%%eax); setna %0"
        : "=g"(error)
        : "a"(&physical_address), "m"(physical_address)
        : "cc"
    );
    return error;
}

void
vmclear(void *vmcs_ptr)
{
    uint64_t physical_address;
    physical_address = pptr_to_paddr((void*)vmcs_ptr);
    asm volatile (
        "vmclear (%%eax)"
        :
        : "a"(&physical_address), "m"(physical_address)
        : "cc"
    );
}

void
invept(void* ept_pml4)
{
    if (vpid_capability & BIT(20) && vpid_capability & (BIT(25) | BIT(26))) {
        uint64_t physical_address[2];
        uint32_t type = 2;
        if (vpid_capability & BIT(25)) {
            type = 1;
        }

        physical_address[0] = pptr_to_paddr((void*)ept_pml4);
        physical_address[1] = 0;
        asm volatile (
            INVEPT_OPCODE
            :
            : "a"(&physical_address),  "c"(type)
            : "memory"
        );
    }
}

#endif
