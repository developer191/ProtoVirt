#include <asm/processor.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/const.h>
#include <linux/errno.h>
#include <linux/fs.h>   /* Needed for KERN_INFO */
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/smp.h>
#include <linux/major.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/uaccess.h>
#include <linux/kvm.h>
#include <linux/gfp.h>
#include <asm/asm.h>
#include <asm/errno.h>
#include <asm/kvm.h>
#include <asm/cpumask.h>
#include <asm/processor.h>

#define X86_CR4_VMXE_BIT	13 /* enable VMX virtualization */
#define X86_CR4_VMXE		_BITUL(X86_CR4_VMXE_BIT)
#define FEATURE_CONTROL_VMXON_ENABLED_OUTSIDE_SMX	(1<<2)
#define FEATURE_CONTROL_LOCKED				(1<<0)
#define MSR_IA32_FEATURE_CONTROL        0x0000003a
#define MSR_IA32_VMX_BASIC              0x00000480
#define MSR_IA32_VMX_CR0_FIXED0         0x00000486
#define MSR_IA32_VMX_CR0_FIXED1         0x00000487
#define MSR_IA32_VMX_CR4_FIXED0         0x00000488
#define MSR_IA32_VMX_CR4_FIXED1         0x00000489
#define EAX_EDX_VAL(val, low, high)	((low) | (high) << 32)
#define EAX_EDX_RET(val, low, high)	"=a" (low), "=d" (high)


// CH 24.2, Vol 3
// getting vmcs revision identifier
/*
static inline uint32_t vmcs_revision_id(void)
{
	return rdmsr(MSR_IA32_VMX_BASIC);
}
*/

static inline int vmxon(uint64_t phys)
{
	uint8_t ret;

	__asm__ __volatile__ ("vmxon %[pa]; setna %[ret]"
		: [ret]"=rm"(ret)
		: [pa]"m"(phys)
		: "cc", "memory");

	return ret;
}

static inline unsigned long long notrace __rdmsr1(unsigned int msr)
{
	DECLARE_ARGS(val, low, high);

	asm volatile("1: rdmsr\n"
		     "2:\n"
		     _ASM_EXTABLE_HANDLE(1b, 2b, ex_handler_rdmsr_unsafe)
		     : EAX_EDX_RET(val, low, high) : "c" (msr));

	return EAX_EDX_VAL(val, low, high);
}

static inline uint32_t vmcs_revision_id(void)
{
	return __rdmsr1(MSR_IA32_VMX_BASIC);
}


// CH 23.7, Vol 3
// Enter in VMX mode
bool getVmxOperation(void) {
    //unsigned long cr0;
	unsigned long cr4;
	unsigned long cr0;
    uint64_t feature_control;
	uint64_t required;
    // setting CR4.VMXE[bit 13] = 1
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4) : : "memory");
    cr4 |= X86_CR4_VMXE;
    __asm__ __volatile__("mov %0, %%cr4" : : "r"(cr4) : "memory");

    /*
	 * Configure IA32_FEATURE_CONTROL MSR to allow VMXON:
	 *  Bit 0: Lock bit. If clear, VMXON causes a #GP.
	 *  Bit 2: Enables VMXON outside of SMX operation. If clear, VMXON
	 *    outside of SMX causes a #GP.
	 */

	 u32 low1 = 0;
	required = FEATURE_CONTROL_VMXON_ENABLED_OUTSIDE_SMX;
	required |= FEATURE_CONTROL_LOCKED;
	feature_control = __rdmsr1(MSR_IA32_FEATURE_CONTROL);
	printk(KERN_INFO "RDMS output is %ld", (long)feature_control);

	if ((feature_control & required) != required) {
		wrmsr(MSR_IA32_FEATURE_CONTROL, feature_control | required, low1);
	}

	/*
	 * Ensure bits in CR0 and CR4 are valid in VMX operation:
	 * - Bit X is 1 in _FIXED0: bit X is fixed to 1 in CRx.
	 * - Bit X is 0 in _FIXED1: bit X is fixed to 0 in CRx.
	 */
	__asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0) : : "memory");
	cr0 &= __rdmsr1(MSR_IA32_VMX_CR0_FIXED1);
	cr0 |= __rdmsr1(MSR_IA32_VMX_CR0_FIXED0);
	__asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0) : "memory");

	__asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4) : : "memory");
	cr4 &= __rdmsr1(MSR_IA32_VMX_CR4_FIXED1);
	cr4 |= __rdmsr1(MSR_IA32_VMX_CR4_FIXED0);

	// allocating 4kib((4096 bytes) of memory for vmxon region
	//vmcs_revision_id();
	printk(KERN_INFO "VMX revision id is %d",vmcs_revision_id() );
	/*
	struct vmx_pages *vmx = addr_gva2hva(vm, vmx_gva);
	struct kvm_vm *vm;
	vmx->vmxon = (void *)vm_vaddr_alloc(vm, getpagesize(), 0x10000, 0, 0);
	vmx->vmxon_hva = addr_gva2hva(vm, (uintptr_t)vmx->vmxon);
	vmx->vmxon_gpa = addr_gva2gpa(vm, (uintptr_t)vmx->vmxon);
	// putting vmcs revision data in allocated memory
	*(uint32_t *)(vmx->vmxon) = vmcs_revision();


	if (vmxon(vmx->vmxon_gpa))
		return false;
		*/
	return true;
}

// CH 23.6, Vol 3
// Checking the support of VMX
bool vmxSupport(void)
{

    int getVmxSupport, vmxBit;
    __asm__("mov $1, %rax");
    __asm__("cpuid");
    __asm__("mov %%ecx , %0\n\t":"=r" (getVmxSupport));
    vmxBit = (getVmxSupport >> 5) & 1;
    if (vmxBit == 1){
        return true;
    }
    else {
        return false;
    }
    return false;

}

int __init start_init(void)
{
    int vmxSupportPresent;

    if (vmxSupport()){
        if (getVmxOperation()) {
			printk(KERN_INFO "VMX operation successfull");
		}
		else {
			printk(KERN_INFO "VMX opperation failed!!\n");
		}
    }
    else {
        printk(KERN_INFO "VMX support not present\n");
    }
    return 0;
}

static void __exit end_exit(void)
{
    printk(KERN_INFO "Bye Bye\n");
}







module_init(start_init);
module_exit(end_exit);


MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Shubham Dubey");
MODULE_DESCRIPTION("Lightweight Hypervisior ");