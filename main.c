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
#include <linux/slab.h>
#include <asm/asm.h>
#include <asm/errno.h>
#include <asm/kvm.h>
#include <asm/cpumask.h>
#include <asm/processor.h>

#define MYPAGE_SIZE 4096
#define X86_CR4_VMXE_BIT	13 /* enable VMX virtualization */
#define X86_CR4_VMXE		_BITUL(X86_CR4_VMXE_BIT)
#define FEATURE_CONTROL_VMXON_ENABLED_OUTSIDE_SMX	(1<<2)
#define FEATURE_CONTROL_LOCKED				(1<<0)
#define MSR_IA32_FEATURE_CONTROL        0x0000003a
#define MSR_IA32_VMX_BASIC              0x00000480

// for vmcs control field
#define MSR_IA32_VMX_PINBASED_CTLS		0x00000481
#define MSR_IA32_VMX_PROCBASED_CTLS		0x00000482
#define MSR_IA32_VMX_PROCBASED_CTLS2	0x0000048b
#define MSR_IA32_VMX_EXIT_CTLS			0x00000483
#define MSR_IA32_VMX_ENTRY_CTLS			0x00000484
// CH B.3.1
// Table B-8. Encodings for 32-Bit Control Fields
#define PIN_BASED_VM_EXEC_CONTROLS		0x00004000
#define PROC_BASED_VM_EXEC_CONTROLS		0x00004002
#define PROC2_BASED_VM_EXEC_CONTROLS	0x0000401e
#define VM_EXIT_CONTROLS				0x0000400c
#define VM_ENTRY_CONTROLS				0x00004012


#define MSR_IA32_VMX_CR0_FIXED0         0x00000486
#define MSR_IA32_VMX_CR0_FIXED1         0x00000487
#define MSR_IA32_VMX_CR4_FIXED0         0x00000488
#define MSR_IA32_VMX_CR4_FIXED1         0x00000489


#define VM_EXIT_REASON			 		0x00004402
#define VM_INSTRUCTION_ERROR			0x00004000  // CH 26.1, Vol 3
#define EAX_EDX_VAL(val, low, high)	((low) | (high) << 32)
#define EAX_EDX_RET(val, low, high)	"=a" (low), "=d" (high)


uint64_t *vmxonRegion = NULL;
uint64_t *vmcsRegion = NULL;
// CH 30.3, Vol 3
// VMXON instruction - Enter VMX operation
static inline int _vmxon(uint64_t phys)
{
	uint8_t ret;

	__asm__ __volatile__ ("vmxon %[pa]; setna %[ret]"
		: [ret]"=rm"(ret)
		: [pa]"m"(phys)
		: "cc", "memory");
	return ret;
}

// CH 24.11.2, Vol 3
static inline int vmread(uint64_t encoding, uint64_t *value)
{
	uint64_t tmp;
	uint8_t ret;
	/*
	if (enable_evmcs)
		return evmcs_vmread(encoding, value);
	*/
	__asm__ __volatile__("vmread %[encoding], %[value]; setna %[ret]"
		: [value]"=rm"(tmp), [ret]"=rm"(ret)
		: [encoding]"r"(encoding)
		: "cc", "memory");

	*value = tmp;
	return ret;
}

/*
 * A wrapper around vmread that ignores errors and returns zero if the
 * vmread instruction fails.
 */
static inline uint64_t vmreadz(uint64_t encoding)
{
	uint64_t value = 0;
	vmread(encoding, &value);
	return value;
}

static inline int vmwrite(uint64_t encoding, uint64_t value)
{
	uint8_t ret;
	__asm__ __volatile__ ("vmwrite %[value], %[encoding]; setna %[ret]"
		: [ret]"=rm"(ret)
		: [value]"rm"(value), [encoding]"r"(encoding)
		: "cc", "memory");

	return ret;
}

uint32_t vmExit_reason(void) {
	uint32_t exit_reason = vmreadz(VM_EXIT_REASON);
	return exit_reason;
}

/* Dealloc vmxon region*/
bool deallocate_vmxon_region(void) {
	if(vmxonRegion){
	    kfree(vmxonRegion);
		return true;
   	}
   	return false;
}

/* Dealloc vmcs guest region*/
bool deallocate_vmcs_region(void) {
	if(vmcsRegion){
    	printk(KERN_INFO "Freeing allocated vmcs region!\n");
    	kfree(vmcsRegion);
		return true;
	}
	return false;
}

static inline int _vmptrld(uint64_t vmcs_pa)
{
	uint8_t ret;

	__asm__ __volatile__ ("vmptrld %[pa]; setna %[ret]"
		: [ret]"=rm"(ret)
		: [pa]"m"(vmcs_pa)
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

// CH 24.2, Vol 3
// getting vmcs revision identifier
static inline uint32_t vmcs_revision_id(void)
{
	return __rdmsr1(MSR_IA32_VMX_BASIC);
}
// CH 23.7, Vol 3
// Enter in VMX mode
bool allocVmcsRegion(void) {
	vmcsRegion = kzalloc(MYPAGE_SIZE,GFP_KERNEL);
   	if(vmcsRegion==NULL){
		printk(KERN_INFO "Error allocating vmcs region\n");
      	return false;
   	}
	return true;
}
// CH 24.2, Vol 3
// VMCS region
bool vmcsOperations(void) {
	long int vmcsPhyRegion = 0;
	if (allocVmcsRegion()){
		vmcsPhyRegion = __pa(vmcsRegion);
		*(uint32_t *)vmcsRegion = vmcs_revision_id();
	}
	else {
		return false;
	}

	//making the vmcs active and current
	if (_vmptrld(vmcsPhyRegion))
		return false;
	return true;
}
// CH 23.7, Vol 3
// Enter in VMX mode
bool getVmxOperation(void) {
    //unsigned long cr0;
	unsigned long cr4;
	unsigned long cr0;
    uint64_t feature_control;
	uint64_t required;
	long int vmxon_phy_region = 0;
	u32 low1 = 0;
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
	__asm__ __volatile__("mov %0, %%cr4" : : "r"(cr4) : "memory");

	// allocating 4kib((4096 bytes) of memory for vmxon region
	vmxonRegion = kzalloc(MYPAGE_SIZE,GFP_KERNEL);
   	if(vmxonRegion==NULL){
		printk(KERN_INFO "Error allocating vmxon region\n");
      	return false;
   	}
	vmxon_phy_region = __pa(vmxonRegion);
	*(uint32_t *)vmxonRegion = vmcs_revision_id();
	if (_vmxon(vmxon_phy_region))
		return false;
	return true;
}
// Ch A.2, Vol 3
// indicate whether any of the default1 controls may be 0
// if return 0, all the default1 controls are reserved and must be 1.
// if return 1,not all the default1 controls are reserved, and
// some (but not necessarily all) may be 0.
unsigned long long default1_controls(void){
	unsigned long long check_default1_controls = (unsigned long long)((__rdmsr1(MSR_IA32_VMX_BASIC) << 55) & 1);
	//printk(KERN_INFO "default1 controls value!---%llu\n", check_default1_controls);
	return check_default1_controls;
}

// CH 26.2.1, Vol 3
// Initializing VMCS control field
bool initVmcsControlField(void) {
	// checking of any of the default1 controls may be 0:
	//not doing it for now.
	// CH A.3.1, Vol 3
	// setting pin based controls
	uint32_t pinbased_control0 = __rdmsr1(MSR_IA32_VMX_PINBASED_CTLS);
	uint32_t pinbased_control1 = __rdmsr1(MSR_IA32_VMX_PINBASED_CTLS) >> 32;
	uint32_t procbased_control0 = __rdmsr1(MSR_IA32_VMX_PROCBASED_CTLS);
	uint32_t procbased_control1 = __rdmsr1(MSR_IA32_VMX_PROCBASED_CTLS) >> 32;
	uint32_t procbased_secondary_control0 = __rdmsr1(MSR_IA32_VMX_PROCBASED_CTLS2);
	uint32_t procbased_secondary_control1 = __rdmsr1(MSR_IA32_VMX_PROCBASED_CTLS2) >> 32;
	uint32_t vm_exit_control0 = __rdmsr1(MSR_IA32_VMX_EXIT_CTLS);
	uint32_t vm_exit_control1 = __rdmsr1(MSR_IA32_VMX_EXIT_CTLS) >> 32;
	uint32_t vm_entry_control0 = __rdmsr1(MSR_IA32_VMX_ENTRY_CTLS);
	uint32_t vm_entry_control1 = __rdmsr1(MSR_IA32_VMX_ENTRY_CTLS) >> 32;
	// setting final value to write to control fields
	uint32_t pinbased_control_final = (pinbased_control0 & pinbased_control1);
	uint32_t procbased_control_final = (procbased_control0 & procbased_control1);
	uint32_t procbased_secondary_control_final = (procbased_secondary_control0 & procbased_secondary_control0);
	uint32_t vm_exit_control_final = (vm_exit_control0 & vm_exit_control1);
	uint32_t vm_entry_control_final = (vm_entry_control0 & vm_entry_control1);
	// writing the value to control field
	vmwrite(PIN_BASED_VM_EXEC_CONTROLS, pinbased_control_final);
	vmwrite(PROC_BASED_VM_EXEC_CONTROLS, procbased_control_final);
	vmwrite(PROC2_BASED_VM_EXEC_CONTROLS, procbased_secondary_control_final);
	vmwrite(VM_EXIT_CONTROLS, vm_exit_control_final);
	vmwrite(VM_ENTRY_CONTROLS, vm_entry_control_final);


	return true;
}

bool vmxoffOperation(void)
{
	if (deallocate_vmxon_region()) {
		printk(KERN_INFO "Successfully freed allocated vmxon region!\n");
	}
	else {
		printk(KERN_INFO "Error freeing allocated vmxon region!\n");
	}
	if (deallocate_vmcs_region()) {
		printk(KERN_INFO "Successfully freed allocated vmcs region!\n");
	}
	else {
		printk(KERN_INFO "Error freeing allocated vmcs region!\n");
	}
	asm volatile ("vmxoff\n" : : : "cc");
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
    if (!vmxSupport()){
		printk(KERN_INFO "VMX support not present! EXITING");
		return 0;
	}
	else {
		printk(KERN_INFO "VMX support present! CONTINUING");
	}
	if (!getVmxOperation()) {
		printk(KERN_INFO "VMX Operation failed! EXITING");
		return 0;
	}
	else {
		printk(KERN_INFO "VMX Operation succeeded! CONTINUING");
	}
	if (!vmcsOperations()) {
		printk(KERN_INFO "VMCS Operation failed! EXITING");
		return 0;
	}
	else {
		printk(KERN_INFO "VMX Operation succeeded! CONTINUING");
	}
	if (!initVmcsControlField()) {
		printk(KERN_INFO "Initialization of VMCS Control field failed! EXITING");
		return 0;
	}
	else {
		printk(KERN_INFO "Initializing of control fields to the most basic settings succeeded! CONTINUING");
	}
	if (!vmxoffOperation()) {
		printk(KERN_INFO "VMXOFF operation failed! EXITING");
		return 0;
	}
	else {
		printk(KERN_INFO "VMXOFF Operation succeeded! CONTINUING");
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
