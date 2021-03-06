/*	$OpenBSD: vmm.c,v 1.62 2016/04/27 15:25:36 mlarkin Exp $	*/
/*
 * Copyright (c) 2014 Mike Larkin <mlarkin@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/signalvar.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <sys/pool.h>
#include <sys/proc.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/pledge.h>
#include <sys/memrange.h>

#include <uvm/uvm_extern.h>

#include <machine/pmap.h>
#include <machine/biosvar.h>
#include <machine/segments.h>
#include <machine/cpufunc.h>
#include <machine/vmmvar.h>
#include <machine/i82489reg.h>

#include <dev/isa/isareg.h>

#ifdef VMM_DEBUG
int vmm_debug = 0;
#define DPRINTF(x...)	do { if (vmm_debug) printf(x); } while(0)
#else
#define DPRINTF(x...)
#endif /* VMM_DEBUG */

#define DEVNAME(s)  ((s)->sc_dev.dv_xname)

#define CTRL_DUMP(x,y,z) printf("     %s: Can set:%s Can clear:%s\n", #z , \
				vcpu_vmx_check_cap(x, IA32_VMX_##y ##_CTLS, \
				IA32_VMX_##z, 1) ? "Yes" : "No", \
				vcpu_vmx_check_cap(x, IA32_VMX_##y ##_CTLS, \
				IA32_VMX_##z, 0) ? "Yes" : "No");

#define VMX_EXIT_INFO_HAVE_RIP		0x1
#define VMX_EXIT_INFO_HAVE_REASON	0x2
#define VMX_EXIT_INFO_COMPLETE				\
    (VMX_EXIT_INFO_HAVE_RIP | VMX_EXIT_INFO_HAVE_REASON)

struct vm {
	vm_map_t		 vm_map;
	uint32_t		 vm_id;
	pid_t			 vm_creator_pid;
	size_t			 vm_nmemranges;
	size_t			 vm_memory_size;
	char			 vm_name[VMM_MAX_NAME_LEN];
	struct vm_mem_range	 vm_memranges[VMM_MAX_MEM_RANGES];

	struct vcpu_head	 vm_vcpu_list;
	uint32_t		 vm_vcpu_ct;
	u_int			 vm_vcpus_running;
	struct rwlock		 vm_vcpu_lock;

	SLIST_ENTRY(vm)		 vm_link;
};

SLIST_HEAD(vmlist_head, vm);

struct vmm_softc {
	struct device		sc_dev;

	/* Capabilities */
	uint32_t		nr_vmx_cpus;
	uint32_t		nr_svm_cpus;
	uint32_t		nr_rvi_cpus;
	uint32_t		nr_ept_cpus;

	/* Managed VMs */
	struct vmlist_head	vm_list;

	int			mode;

	struct rwlock		vm_lock;
	size_t			vm_ct;		/* number of in-memory VMs */
	size_t			vm_idx;		/* next unique VM index */
};

int vmm_probe(struct device *, void *, void *);
void vmm_attach(struct device *, struct device *, void *);
int vmm_activate(struct device *, int);
int vmmopen(dev_t, int, int, struct proc *);
int vmmioctl(dev_t, u_long, caddr_t, int, struct proc *);
int vmmclose(dev_t, int, int, struct proc *);
int vmm_start(void);
int vmm_stop(void);
size_t vm_create_check_mem_ranges(struct vm_create_params *);
int vm_create(struct vm_create_params *, struct proc *);
int vm_run(struct vm_run_params *);
int vm_terminate(struct vm_terminate_params *);
int vm_get_info(struct vm_info_params *);
int vm_writepage(struct vm_writepage_params *);
int vm_readpage(struct vm_readpage_params *);
int vm_resetcpu(struct vm_resetcpu_params *);
int vm_intr_pending(struct vm_intr_params *);
int vcpu_reset_regs(struct vcpu *, struct vcpu_init_state *);
int vcpu_reset_regs_vmx(struct vcpu *, struct vcpu_init_state *);
int vcpu_reset_regs_svm(struct vcpu *, struct vcpu_init_state *);
int vcpu_init(struct vcpu *);
int vcpu_init_vmx(struct vcpu *);
int vcpu_init_svm(struct vcpu *);
int vcpu_must_stop(struct vcpu *);
int vcpu_run_vmx(struct vcpu *, uint8_t, int16_t *);
int vcpu_run_svm(struct vcpu *, uint8_t);
void vcpu_deinit(struct vcpu *);
void vcpu_deinit_vmx(struct vcpu *);
void vcpu_deinit_svm(struct vcpu *);
int vm_impl_init(struct vm *);
int vm_impl_init_vmx(struct vm *);
int vm_impl_init_svm(struct vm *);
void vm_impl_deinit(struct vm *);
void vm_impl_deinit_vmx(struct vm *);
void vm_impl_deinit_svm(struct vm *);
void vm_teardown(struct vm *);
int vcpu_vmx_check_cap(struct vcpu *, uint32_t, uint32_t, int);
int vcpu_vmx_compute_ctrl(struct vcpu *, uint64_t, uint16_t, uint32_t,
    uint32_t, uint32_t *);
int vmx_get_exit_info(uint64_t *, uint64_t *);
int vmx_handle_exit(struct vcpu *);
int vmx_handle_cpuid(struct vcpu *);
int vmx_handle_cr(struct vcpu *);
int vmx_handle_inout(struct vcpu *);
int vmx_handle_hlt(struct vcpu *);
void vmx_handle_intr(struct vcpu *);
void vmx_handle_intwin(struct vcpu *);
int vmm_get_guest_memtype(struct vm *, paddr_t);
int vmm_get_guest_faulttype(void);
int vmx_get_guest_faulttype(void);
int svm_get_guest_faulttype(void);
int vmx_get_exit_qualification(uint64_t *);
int vmx_fault_page(struct vcpu *, paddr_t);
int vmx_handle_np_fault(struct vcpu *);
const char *vcpu_state_decode(u_int);
const char *vmx_exit_reason_decode(uint32_t);
const char *vmx_instruction_error_decode(uint32_t);

#ifdef VMM_DEBUG
void dump_vcpu(struct vcpu *);
void vmx_vcpu_dump_regs(struct vcpu *);
const char *msr_name_decode(uint32_t);
void vmm_segment_desc_decode(uint64_t);
void vmm_decode_cr0(uint64_t);
void vmm_decode_cr3(uint64_t);
void vmm_decode_cr4(uint64_t);
void vmm_decode_msr_value(uint64_t, uint64_t);
void vmm_decode_apicbase_msr_value(uint64_t);
void vmm_decode_ia32_fc_value(uint64_t);
void vmm_decode_mtrrcap_value(uint64_t);
void vmm_decode_perf_status_value(uint64_t);
void vmm_decode_perf_ctl_value(uint64_t);
void vmm_decode_mtrrdeftype_value(uint64_t);
void vmm_decode_efer_value(uint64_t);

extern int mtrr2mrt(int);

struct vmm_reg_debug_info {
	uint64_t	vrdi_bit;
	const char	*vrdi_present;
	const char	*vrdi_absent;
};
#endif /* VMM_DEBUG */

const char *vmm_hv_signature = VMM_HV_SIGNATURE;

struct cfdriver vmm_cd = {
	NULL, "vmm", DV_DULL
};

const struct cfattach vmm_ca = {
	sizeof(struct vmm_softc), vmm_probe, vmm_attach, NULL, vmm_activate
};

/* Pools for VMs and VCPUs */
struct pool vm_pool;
struct pool vcpu_pool;

struct vmm_softc *vmm_softc;

/* IDT information used when populating host state area */
extern vaddr_t idt_vaddr;
extern struct gate_descriptor *idt;

/* XXX Temporary hack for the PIT clock */
#define CLOCK_BIAS 8192
uint64_t vmmclk = 0;

/* Constants used in "CR access exit" */
#define CR_WRITE	0
#define CR_READ		1
#define CR_CLTS		2
#define CR_LMSW		3

/*
 * vmm_probe
 *
 * Checks if we have at least one CPU with either VMX or SVM.
 * Returns 1 if we have at least one of either type, but not both, 0 otherwise.
 */
int
vmm_probe(struct device *parent, void *match, void *aux)
{
	struct cpu_info *ci;
	CPU_INFO_ITERATOR cii;
	const char **busname = (const char **)aux;
	int found_vmx, found_svm;

	/* Check if this probe is for us */
	if (strcmp(*busname, vmm_cd.cd_name) != 0)
		return (0);

	found_vmx = 0;
	found_svm = 0;

	/* Check if we have at least one CPU with either VMX or SVM */
	CPU_INFO_FOREACH(cii, ci) {
		if (ci->ci_vmm_flags & CI_VMM_VMX)
			found_vmx = 1;
		if (ci->ci_vmm_flags & CI_VMM_SVM)
			found_svm = 1;
	}

	/* Don't support both SVM and VMX at the same time */
	if (found_vmx && found_svm)
		return (0);

	return (found_vmx || found_svm);
}

/*
 * vmm_attach
 *
 * Calculates how many of each type of CPU we have, prints this into dmesg
 * during attach. Initializes various locks, pools, and list structures for the
 * VMM.
 */
void
vmm_attach(struct device *parent, struct device *self, void *aux)
{
	struct vmm_softc *sc = (struct vmm_softc *)self;
	struct cpu_info *ci;
	CPU_INFO_ITERATOR cii;

	sc->nr_vmx_cpus = 0;
	sc->nr_svm_cpus = 0;
	sc->nr_rvi_cpus = 0;
	sc->nr_ept_cpus = 0;
	sc->vm_ct = 0;
	sc->vm_idx = 0;

	/* Calculate CPU features */
	CPU_INFO_FOREACH(cii, ci) {
		if (ci->ci_vmm_flags & CI_VMM_VMX)
			sc->nr_vmx_cpus++;
		if (ci->ci_vmm_flags & CI_VMM_SVM)
			sc->nr_svm_cpus++;
		if (ci->ci_vmm_flags & CI_VMM_RVI)
			sc->nr_rvi_cpus++;
		if (ci->ci_vmm_flags & CI_VMM_EPT)
			sc->nr_ept_cpus++;
	}

	SLIST_INIT(&sc->vm_list);
	rw_init(&sc->vm_lock, "vmlistlock");

	if (sc->nr_ept_cpus) {
		printf(": VMX/EPT\n");
		sc->mode = VMM_MODE_EPT;
	} else if (sc->nr_vmx_cpus) {
		printf(": VMX\n");
		sc->mode = VMM_MODE_VMX;
	} else if (sc->nr_rvi_cpus) {
		printf(": SVM/RVI\n");
		sc->mode = VMM_MODE_RVI;
	} else if (sc->nr_svm_cpus) {
		printf(": SVM\n");
		sc->mode = VMM_MODE_SVM;
	} else {
		printf(": unknown\n");
		sc->mode = VMM_MODE_UNKNOWN;
	}

	pool_init(&vm_pool, sizeof(struct vm), 0, 0, PR_WAITOK, "vmpool",
	    NULL);
	pool_setipl(&vm_pool, IPL_NONE);
	pool_init(&vcpu_pool, sizeof(struct vcpu), 0, 0, PR_WAITOK, "vcpupl",
	    NULL);
	pool_setipl(&vcpu_pool, IPL_NONE);

	vmm_softc = sc;
}

/*
 * vmm_activate
 *
 * Autoconf routine used during activate/deactivate.
 *
 * XXX need this for suspend/resume
 */
int
vmm_activate(struct device *self, int act)
{
	return 0;
}

/*
 * vmmopen
 *
 * Called during open of /dev/vmm. Presently unused.
 */
int
vmmopen(dev_t dev, int flag, int mode, struct proc *p)
{
	/* Don't allow open if we didn't attach */
	if (vmm_softc == NULL)
		return (ENODEV);

	/* Don't allow open if we didn't detect any supported CPUs */
	/* XXX presently this means EPT until SP and SVM are back */
	if (vmm_softc->mode != VMM_MODE_EPT)
		return (ENODEV);

	return 0;
}

/*
 * vmmioctl
 *
 * Main ioctl dispatch routine for /dev/vmm. Parses ioctl type and calls
 * appropriate lower level handler routine. Returns result to ioctl caller.
 */
int
vmmioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	int ret;

	switch (cmd) {
	case VMM_IOC_CREATE:
		if ((ret = vmm_start()) != 0) {
			vmm_stop();
			break;
		}
		ret = vm_create((struct vm_create_params *)data, p);
		break;
	case VMM_IOC_RUN:
		ret = vm_run((struct vm_run_params *)data);
		break;
	case VMM_IOC_INFO:
		ret = vm_get_info((struct vm_info_params *)data);
		break;
	case VMM_IOC_TERM:
		ret = vm_terminate((struct vm_terminate_params *)data);
		break;
	case VMM_IOC_WRITEPAGE:
		ret = vm_writepage((struct vm_writepage_params *)data);
		break;
	case VMM_IOC_READPAGE:
		ret = vm_readpage((struct vm_readpage_params *)data);
		break;
	case VMM_IOC_RESETCPU:
		ret = vm_resetcpu((struct vm_resetcpu_params *)data);
		break;
	case VMM_IOC_INTR:
		ret = vm_intr_pending((struct vm_intr_params *)data);
		break;
	default:
		DPRINTF("vmmioctl: unknown ioctl code 0x%lx\n", cmd);
		ret = ENOTTY;
	}

	return (ret);
}

/*
 * pledge_ioctl_vmm
 *
 * Restrict the allowed ioctls in a pledged process context.
 * Is called from pledge_ioctl().
 */
int
pledge_ioctl_vmm(struct proc *p, long com)
{
	switch (com) {
	case VMM_IOC_CREATE:
	case VMM_IOC_INFO:
		/* The "parent" process in vmd forks and manages VMs */
		if (p->p_p->ps_pledge & PLEDGE_PROC)
			return (0);
		break;
	case VMM_IOC_TERM:
		/* XXX VM processes should only terminate themselves */
	case VMM_IOC_RUN:
	case VMM_IOC_WRITEPAGE:
	case VMM_IOC_READPAGE:
	case VMM_IOC_RESETCPU:
		return (0);
	}

	return (EPERM);
}

/*
 * vmmclose
 *
 * Called when /dev/vmm is closed. Presently unused.
 */
int
vmmclose(dev_t dev, int flag, int mode, struct proc *p)
{
	return 0;
}

/*
 * vm_readpage
 *
 * Reads a region (PAGE_SIZE max) of guest physical memory using the parameters
 * defined in 'vrp'.
 *
 * Returns 0 if successful, or various error codes on failure:
 *  ENOENT if the VM id contained in 'vrp' refers to an unknown VM
 *  EINVAL if the memory region described by vrp is not regular memory
 *  EFAULT if the memory region described by vrp has not yet been faulted in
 *      by the guest
 */
int
vm_readpage(struct vm_readpage_params *vrp)
{
	struct vm *vm;
	paddr_t host_pa;
	void *kva;
	vaddr_t vr_page;

	/* Find the desired VM */
	rw_enter_read(&vmm_softc->vm_lock);
	SLIST_FOREACH(vm, &vmm_softc->vm_list, vm_link) {
		if (vm->vm_id == vrp->vrp_vm_id)
			break;
	}

	/* Not found? exit. */
	if (vm == NULL) {
		rw_exit_read(&vmm_softc->vm_lock);
		return (ENOENT);
	}

	/* Check that the data to be read is within a page */
	if (vrp->vrp_len > (PAGE_SIZE - (vrp->vrp_paddr & PAGE_MASK))) {
		rw_exit_read(&vmm_softc->vm_lock);
		return (EINVAL);
	}

	/* Calculate page containing vrp->vrp_paddr */
	vr_page = vrp->vrp_paddr & ~PAGE_MASK;

	/* If not regular memory, exit. */
	if (vmm_get_guest_memtype(vm, vr_page) != VMM_MEM_TYPE_REGULAR) {
		rw_exit_read(&vmm_softc->vm_lock);
		return (EINVAL);
	}

	/* Find the phys page where this guest page exists in real memory */
	if (!pmap_extract(vm->vm_map->pmap, vr_page, &host_pa)) {
		rw_exit_read(&vmm_softc->vm_lock);
		return (EFAULT);
	}

	/* Allocate temporary KVA for the guest page */
	kva = km_alloc(PAGE_SIZE, &kv_any, &kp_none, &kd_nowait);
	if (!kva) {
		DPRINTF("vm_readpage: can't alloc kva\n");
		rw_exit_read(&vmm_softc->vm_lock);
		return (EFAULT);
	}

	/* Enter the mapping in the kernel pmap and copyout */
	pmap_kenter_pa((vaddr_t)kva, host_pa, PROT_READ);

	if (copyout(kva + ((vaddr_t)vrp->vrp_paddr & PAGE_MASK),
	    vrp->vrp_data, vrp->vrp_len) == EFAULT) {
		DPRINTF("vm_readpage: can't copyout\n");
		pmap_kremove((vaddr_t)kva, PAGE_SIZE);
		km_free(kva, PAGE_SIZE, &kv_any, &kp_none);
		rw_exit_read(&vmm_softc->vm_lock);
		return (EFAULT);
	}

	/* Cleanup and exit */
	pmap_kremove((vaddr_t)kva, PAGE_SIZE);
	km_free(kva, PAGE_SIZE, &kv_any, &kp_none);

	rw_exit_read(&vmm_softc->vm_lock);

	return (0);
}

/*
 * vm_resetcpu
 *
 * Resets the vcpu defined in 'vrp' to power-on-init register state
 *
 * Parameters:
 *  vrp: ioctl structure defining the vcpu to reset (see vmmvar.h)
 *
 * Returns 0 if successful, or various error codes on failure:
 *  ENOENT if the VM id contained in 'vrp' refers to an unknown VM or
 *      if vrp describes an unknown vcpu for this VM
 *  EBUSY if the indicated VCPU is not stopped
 *  EIO if the indicated VCPU failed to reset
 */
int
vm_resetcpu(struct vm_resetcpu_params *vrp)
{
	struct vm *vm;
	struct vcpu *vcpu;

	/* Find the desired VM */
	rw_enter_read(&vmm_softc->vm_lock);
	SLIST_FOREACH(vm, &vmm_softc->vm_list, vm_link) {
		if (vm->vm_id == vrp->vrp_vm_id)
			break;
	}
	rw_exit_read(&vmm_softc->vm_lock);

	/* Not found? exit. */
	if (vm == NULL) {
		DPRINTF("vm_resetcpu: vm id %u not found\n",
		    vrp->vrp_vm_id);
		return (ENOENT);
	}

	rw_enter_read(&vm->vm_vcpu_lock);
	SLIST_FOREACH(vcpu, &vm->vm_vcpu_list, vc_vcpu_link) {
		if (vcpu->vc_id == vrp->vrp_vcpu_id)
			break;
	}
	rw_exit_read(&vm->vm_vcpu_lock);

	if (vcpu == NULL) {
		DPRINTF("vm_resetcpu: vcpu id %u of vm %u not found\n",
		    vrp->vrp_vcpu_id, vrp->vrp_vm_id);
		return (ENOENT);
	}

	if (vcpu->vc_state != VCPU_STATE_STOPPED) {
		DPRINTF("vm_resetcpu: reset of vcpu %u on vm %u attempted "
		    "while vcpu was in state %u (%s)\n", vrp->vrp_vcpu_id,
		    vrp->vrp_vm_id, vcpu->vc_state,
		    vcpu_state_decode(vcpu->vc_state));
		
		return (EBUSY);
	}

	DPRINTF("vm_resetcpu: resetting vm %d vcpu %d to power on defaults\n",
	    vm->vm_id, vcpu->vc_id);

	if (vcpu_reset_regs(vcpu, &vrp->vrp_init_state)) {
		printf("vm_resetcpu: failed\n");
#ifdef VMM_DEBUG
		dump_vcpu(vcpu);
#endif /* VMM_DEBUG */
		return (EIO);
	}

	return (0);
}

/*
 * vm_intr_pending
 *
 * IOCTL handler routine for VMM_IOC_INTR messages, sent from vmd when an
 * interrupt is pending and needs acknowledgment.
 *
 * Parameters:
 *  vip: Describes the vm/vcpu for which the interrupt is pending
 *
 * Return values:
 *  0: if successful
 *  ENOENT: if the VM/VCPU defined by 'vip' cannot be found
 */
int
vm_intr_pending(struct vm_intr_params *vip)
{
	struct vm *vm;
	struct vcpu *vcpu;
	
	/* Find the desired VM */
	rw_enter_read(&vmm_softc->vm_lock);
	SLIST_FOREACH(vm, &vmm_softc->vm_list, vm_link) {
		if (vm->vm_id == vip->vip_vm_id)
			break;
	}

	/* Not found? exit. */
	if (vm == NULL) {
		rw_exit_read(&vmm_softc->vm_lock);
		return (ENOENT);
	}

	rw_enter_read(&vm->vm_vcpu_lock);
	SLIST_FOREACH(vcpu, &vm->vm_vcpu_list, vc_vcpu_link) {
		if (vcpu->vc_id == vip->vip_vcpu_id)
			break;
	}
	rw_exit_read(&vm->vm_vcpu_lock);
	rw_exit_read(&vmm_softc->vm_lock);

	if (vcpu == NULL)
		return (ENOENT);

	vcpu->vc_intr = vip->vip_intr;

#ifdef MULTIPROCESSOR
	/*
	 * If the vcpu is running on another PCPU, attempt to force it
	 * to exit to process the pending interrupt. This could race as
	 * it could be running when we do the check but be stopped by the
	 * time we send the IPI. In this case, there is a small extra
	 * overhead to process the IPI but no other side effects.
	 *
	 * There is also a chance that the vcpu may have interrupts blocked.
	 * That's ok as that condition will be checked on exit, and we will
	 * simply re-enter the guest. This "fast notification" is done only
	 * as an optimization.
	 */
	if (vcpu->vc_state == VCPU_STATE_RUNNING) {
		x86_send_ipi(vcpu->vc_last_pcpu, X86_IPI_NOP);
	}
#endif /* MULTIPROCESSOR */

	return (0);
}

/*
 * vm_writepage
 *
 * Writes a region (PAGE_SIZE max) of guest physical memory using the parameters
 * defined in 'vrp'.
 *
 * Returns 0 if successful, or various error codes on failure:
 *  ENOENT if the VM id contained in 'vrp' refers to an unknown VM
 *  EINVAL if the memory region described by vrp is not regular memory
 *  EFAULT if the source data in vrp contains an invalid address
 *  ENOMEM if a memory allocation error occurs
 */
int
vm_writepage(struct vm_writepage_params *vwp)
{
	char *pagedata;
	struct vm *vm;
	paddr_t host_pa;
	void *kva;
	int ret;
	vaddr_t vw_page, dst;

	/* Find the desired VM */
	rw_enter_read(&vmm_softc->vm_lock);
	SLIST_FOREACH(vm, &vmm_softc->vm_list, vm_link) {
		if (vm->vm_id == vwp->vwp_vm_id)
			break;
	}

	/* Not found? exit. */
	if (vm == NULL) {
		rw_exit_read(&vmm_softc->vm_lock);
		return (ENOENT);
	}

	/* Check that the data to be written is within a page */
	if (vwp->vwp_len > (PAGE_SIZE - (vwp->vwp_paddr & PAGE_MASK))) {
		rw_exit_read(&vmm_softc->vm_lock);
		return (EINVAL);
	}

	/* Calculate page containing vwp->vwp_paddr */
	vw_page = vwp->vwp_paddr & ~PAGE_MASK;

	/* If not regular memory, exit. */
	if (vmm_get_guest_memtype(vm, vw_page) != VMM_MEM_TYPE_REGULAR) {
		rw_exit_read(&vmm_softc->vm_lock);
		return (EINVAL);
	}

	/* Allocate temporary region to copyin into */
	pagedata = malloc(PAGE_SIZE, M_DEVBUF, M_NOWAIT|M_ZERO);
	if (pagedata == NULL) {
		rw_exit_read(&vmm_softc->vm_lock);
		return (ENOMEM);
	}

	/* Copy supplied data to kernel */
	if (copyin(vwp->vwp_data, pagedata, vwp->vwp_len) == EFAULT) {
		free(pagedata, M_DEVBUF, PAGE_SIZE);
		rw_exit_read(&vmm_softc->vm_lock);
		return (EFAULT);
	}

	/* Find the phys page where this guest page exists in real memory */
	if (!pmap_extract(vm->vm_map->pmap, vw_page, &host_pa)) {
		/* page not present */
		ret = uvm_fault(vm->vm_map, vw_page,
		    VM_FAULT_INVALID, PROT_READ | PROT_WRITE | PROT_EXEC);
		if (ret) {
			free(pagedata, M_DEVBUF, PAGE_SIZE);
			rw_exit_read(&vmm_softc->vm_lock);
			return (EFAULT);
		}

		if (!pmap_extract(vm->vm_map->pmap, vw_page, &host_pa)) {
			panic("vm_writepage: still not mapped GPA 0x%llx\n",
			    (uint64_t)vwp->vwp_paddr);
		}
	}

	/* Allocate kva for guest page */
	kva = km_alloc(PAGE_SIZE, &kv_any, &kp_none, &kd_nowait);
	if (kva == NULL) {
		DPRINTF("vm_writepage: can't alloc kva\n");
		free(pagedata, M_DEVBUF, PAGE_SIZE);
		rw_exit_read(&vmm_softc->vm_lock);
		return (ENOMEM);
	}

	/* Enter mapping and copy data */
	pmap_kenter_pa((vaddr_t)kva, host_pa, PROT_READ | PROT_WRITE);
	dst = (vaddr_t)kva + ((vaddr_t)vwp->vwp_paddr & PAGE_MASK);
	memcpy((void *)dst, pagedata, vwp->vwp_len);

	/* Cleanup */
	pmap_kremove((vaddr_t)kva, PAGE_SIZE);
	km_free(kva, PAGE_SIZE, &kv_any, &kp_none);

	free(pagedata, M_DEVBUF, PAGE_SIZE);

	rw_exit_read(&vmm_softc->vm_lock);

	return (0);
}

/*
 * vmm_start
 *
 * Starts VMM mode on the system
 */
int
vmm_start(void)
{
	struct cpu_info *self = curcpu();
	int ret = 0;
#ifdef MULTIPROCESSOR
	struct cpu_info *ci;
	CPU_INFO_ITERATOR cii;
	int i;
#endif

	/* VMM is already running */
	if (self->ci_flags & CPUF_VMM)
		return (0);

#ifdef MULTIPROCESSOR
	/* Broadcast start VMM IPI */
	x86_broadcast_ipi(X86_IPI_START_VMM);

	CPU_INFO_FOREACH(cii, ci) {
		if (ci == self)
			continue;
		for (i = 100000; (!(ci->ci_flags & CPUF_VMM)) && i>0;i--)
			delay(10);
		if (!(ci->ci_flags & CPUF_VMM)) {
			printf("%s: failed to enter VMM mode\n",
				ci->ci_dev->dv_xname);
			ret = EIO;
		}
	}
#endif /* MULTIPROCESSOR */

	/* Start VMM on this CPU */
	start_vmm_on_cpu(self);
	if (!(self->ci_flags & CPUF_VMM)) {
		printf("%s: failed to enter VMM mode\n",
			self->ci_dev->dv_xname);
		ret = EIO;
	}

	return (ret);
}

/*
 * vmm_stop
 *
 * Stops VMM mode on the system
 */
int
vmm_stop(void)
{
	struct cpu_info *self = curcpu();
	int ret = 0;
#ifdef MULTIPROCESSOR
	struct cpu_info *ci;
	CPU_INFO_ITERATOR cii;
	int i;
#endif

	/* VMM is not running */
	if (!(self->ci_flags & CPUF_VMM))
		return (0);

#ifdef MULTIPROCESSOR
	/* Stop VMM on other CPUs */
	x86_broadcast_ipi(X86_IPI_STOP_VMM);

	CPU_INFO_FOREACH(cii, ci) {
		if (ci == self)
			continue;
		for (i = 100000; (ci->ci_flags & CPUF_VMM) && i>0 ;i--)
			delay(10);
		if (ci->ci_flags & CPUF_VMM) {
			printf("%s: failed to exit VMM mode\n",
				ci->ci_dev->dv_xname);
			ret = EIO;
		}
	}
#endif /* MULTIPROCESSOR */

	/* Stop VMM on this CPU */
	stop_vmm_on_cpu(self);
	if (self->ci_flags & CPUF_VMM) {
		printf("%s: failed to exit VMM mode\n",
			self->ci_dev->dv_xname);
		ret = EIO;
	}

	return (ret);
}

/*
 * start_vmm_on_cpu
 *
 * Starts VMM mode on 'ci' by executing the appropriate CPU-specific insn
 * sequence to enter VMM mode (eg, VMXON)
 */
void
start_vmm_on_cpu(struct cpu_info *ci)
{
	uint64_t msr;
	uint32_t cr4;

	/* No VMM mode? exit. */
	if ((ci->ci_vmm_flags & CI_VMM_VMX) == 0 &&
	    (ci->ci_vmm_flags & CI_VMM_SVM) == 0)
		return;

	/*
	 * AMD SVM
	 */
	if (ci->ci_vmm_flags & CI_VMM_SVM) {
		msr = rdmsr(MSR_EFER);
		msr |= EFER_SVME;
		wrmsr(MSR_EFER, msr);
	}

	/*
	 * Intel VMX
	 */
	if (ci->ci_vmm_flags & CI_VMM_VMX) {
		if (ci->ci_vmxon_region == 0)
			panic("NULL vmxon region specified\n");
		else {
			bzero(ci->ci_vmxon_region, PAGE_SIZE);
			ci->ci_vmxon_region->vr_revision =
			    ci->ci_vmm_cap.vcc_vmx.vmx_vmxon_revision;

			/* Set CR4.VMXE */
			cr4 = rcr4();
			cr4 |= CR4_VMXE;
			lcr4(cr4);

			/* Enable VMX */
			msr = rdmsr(MSR_IA32_FEATURE_CONTROL);
			if (msr & IA32_FEATURE_CONTROL_LOCK) {
				if (!(msr & IA32_FEATURE_CONTROL_VMX_EN))
					return;
			} else {
				msr |= IA32_FEATURE_CONTROL_VMX_EN |
				    IA32_FEATURE_CONTROL_LOCK;
				wrmsr(MSR_IA32_FEATURE_CONTROL, msr);
			}

			/* Enter VMX mode */
			if (vmxon((uint64_t *)&ci->ci_vmxon_region_pa))
				panic("VMXON failed\n");
		}
	}

	ci->ci_flags |= CPUF_VMM;
}

/*
 * stop_vmm_on_cpu
 *
 * Stops VMM mode on 'ci' by executing the appropriate CPU-specific insn
 * sequence to exit VMM mode (eg, VMXOFF)
 */
void
stop_vmm_on_cpu(struct cpu_info *ci)
{
	uint64_t msr;
	uint32_t cr4;

	if (!(ci->ci_flags & CPUF_VMM))
		return;

	/*
	 * AMD SVM
	 */
	if (ci->ci_vmm_flags & CI_VMM_SVM) {
		msr = rdmsr(MSR_EFER);
		msr &= ~EFER_SVME;
		wrmsr(MSR_EFER, msr);
	}

	/*
	 * Intel VMX
	 */
	if (ci->ci_vmm_flags & CI_VMM_VMX) {
		if (vmxoff())
			panic("VMXOFF failed\n");

		cr4 = rcr4();
		cr4 &= ~CR4_VMXE;
		lcr4(cr4);
	}

	ci->ci_flags &= ~CPUF_VMM;
}

/*
 * vm_create_check_mem_ranges:
 *
 * Make sure that the guest physical memory ranges given by the user process
 * do not overlap and are in ascending order.
 *
 * The last physical address may not exceed VMM_MAX_VM_MEM_SIZE.
 *
 * Return Values:
 *   The total memory size in MB if the checks were successful
 *   0: One of the memory ranges was invalid, or VMM_MAX_VM_MEM_SIZE was
 *   exceeded
 */
size_t
vm_create_check_mem_ranges(struct vm_create_params *vcp)
{
	int disjunct_range;
	size_t i, memsize = 0;
	struct vm_mem_range *vmr, *pvmr;
	const paddr_t maxgpa = (uint64_t)VMM_MAX_VM_MEM_SIZE * 1024 * 1024;

	if (vcp->vcp_nmemranges == 0 ||
	    vcp->vcp_nmemranges > VMM_MAX_MEM_RANGES)
		return (0);

	for (i = 0; i < vcp->vcp_nmemranges; i++) {
		vmr = &vcp->vcp_memranges[i];

		/* Only page-aligned addresses and sizes are permitted */
		if ((vmr->vmr_gpa & PAGE_MASK) ||
		    (vmr->vmr_size & PAGE_MASK) || vmr->vmr_size == 0)
			return (0);

		/* Make sure that VMM_MAX_VM_MEM_SIZE is not exceeded */
		if (vmr->vmr_gpa >= maxgpa ||
		    vmr->vmr_size > maxgpa - vmr->vmr_gpa)
			return (0);

		/* Specifying ranges within the PCI MMIO space is forbidden */
		disjunct_range = (vmr->vmr_gpa > VMM_PCI_MMIO_BAR_END) ||
		    (vmr->vmr_gpa + vmr->vmr_size <= VMM_PCI_MMIO_BAR_BASE);
		if (!disjunct_range)
			return (0);

		/*
		 * Make sure that guest physcal memory ranges do not overlap
		 * and that they are ascending.
		 */
		if (i > 0 && pvmr->vmr_gpa + pvmr->vmr_size > vmr->vmr_gpa)
			return (0);

		memsize += vmr->vmr_size;
		pvmr = vmr;
	}

	if (memsize % (1024 * 1024) != 0)
		return (0);
	memsize /= 1024 * 1024;
	return (memsize);
}

/*
 * vm_create
 *
 * Creates the in-memory VMM structures for the VM defined by 'vcp'. The
 * parent of this VM shall be the process defined by 'p'.
 * This function does not start the VCPU(s) - see vm_start.
 *
 * Return Values:
 *  0: the create operation was successful
 *  ENOMEM: out of memory
 *  various other errors from vcpu_init/vm_impl_init
 */
int
vm_create(struct vm_create_params *vcp, struct proc *p)
{
	int i, ret;
	size_t memsize;
	struct vm *vm;
	struct vcpu *vcpu;

	if (!(curcpu()->ci_flags & CPUF_VMM))
		return (EINVAL);

	memsize = vm_create_check_mem_ranges(vcp);
	if (memsize == 0)
		return (EINVAL);

	/* XXX - support UP only (for now) */
	if (vcp->vcp_ncpus != 1)
		return (EINVAL);

	vm = pool_get(&vm_pool, PR_WAITOK | PR_ZERO);
	SLIST_INIT(&vm->vm_vcpu_list);
	rw_init(&vm->vm_vcpu_lock, "vcpulock");

	vm->vm_creator_pid = p->p_p->ps_pid;
	vm->vm_nmemranges = vcp->vcp_nmemranges;
	memcpy(vm->vm_memranges, vcp->vcp_memranges,
	    vm->vm_nmemranges * sizeof(vm->vm_memranges[0]));
	vm->vm_memory_size = memsize;
	strncpy(vm->vm_name, vcp->vcp_name, VMM_MAX_NAME_LEN);

	if (vm_impl_init(vm)) {
		printf("failed to init arch-specific features for vm 0x%p\n",
		    vm);
		vm_teardown(vm);
		return (ENOMEM);
	}

	rw_enter_write(&vmm_softc->vm_lock);
	vmm_softc->vm_ct++;
	vmm_softc->vm_idx++;

	/*
	 * XXX we use the vm_id for the VPID/ASID, so we need to prevent
	 * wrapping around 65536/4096 entries here
	 */
	vm->vm_id = vmm_softc->vm_idx;
	vm->vm_vcpu_ct = 0;
	vm->vm_vcpus_running = 0;

	/* Initialize each VCPU defined in 'vcp' */
	for (i = 0; i < vcp->vcp_ncpus; i++) {
		vcpu = pool_get(&vcpu_pool, PR_WAITOK | PR_ZERO);
		vcpu->vc_parent = vm;
		if ((ret = vcpu_init(vcpu)) != 0) {
			printf("failed to init vcpu %d for vm 0x%p\n", i, vm);
			vm_teardown(vm);
			vmm_softc->vm_ct--;
			vmm_softc->vm_idx--;
			rw_exit_write(&vmm_softc->vm_lock);
			return (ret);
		}
		rw_enter_write(&vm->vm_vcpu_lock);
		vcpu->vc_id = vm->vm_vcpu_ct;
		vm->vm_vcpu_ct++;
		SLIST_INSERT_HEAD(&vm->vm_vcpu_list, vcpu, vc_vcpu_link);
		rw_exit_write(&vm->vm_vcpu_lock);
	}

	/* XXX init various other hardware parts (vlapic, vioapic, etc) */

	SLIST_INSERT_HEAD(&vmm_softc->vm_list, vm, vm_link);
	rw_exit_write(&vmm_softc->vm_lock);

	vcp->vcp_id = vm->vm_id;

	return (0);
}

/*
 * vm_impl_init_vmx
 *
 * Intel VMX specific VM initialization routine
 */
int
vm_impl_init_vmx(struct vm *vm)
{
	int i, ret;
	vaddr_t mingpa, maxgpa;
	vaddr_t startp;
	struct pmap *pmap;
	struct vm_mem_range *vmr;

	/* If not EPT, nothing to do here */
	if (vmm_softc->mode != VMM_MODE_EPT)
		return (0);

	/* Create a new pmap for this VM */
	pmap = pmap_create();
	if (!pmap) {
		printf("vm_impl_init_vmx: pmap_create failed\n");
		return (ENOMEM);
	}

	/*
	 * Create a new UVM map for this VM, and assign it the pmap just
	 * created.
	 */
	vmr = &vm->vm_memranges[0];
	mingpa = vmr->vmr_gpa;
	vmr = &vm->vm_memranges[vm->vm_nmemranges - 1];
	maxgpa = vmr->vmr_gpa + vmr->vmr_size;
	vm->vm_map = uvm_map_create(pmap, mingpa, maxgpa,
	    VM_MAP_ISVMSPACE | VM_MAP_PAGEABLE);

	if (!vm->vm_map) {
		printf("vm_impl_init_vmx: uvm_map_create failed\n");
		pmap_destroy(pmap);
		return (ENOMEM);
	}

	/* Map the new map with an anon */
	DPRINTF("vm_impl_init_vmx: created vm_map @ %p\n", vm->vm_map);
	for (i = 0; i < vm->vm_nmemranges; i++) {
		vmr = &vm->vm_memranges[i];
		startp = vmr->vmr_gpa;
		ret = uvm_mapanon(vm->vm_map, &startp, vmr->vmr_size, 0,
		    UVM_MAPFLAG(PROT_READ | PROT_WRITE | PROT_EXEC,
		    PROT_READ | PROT_WRITE | PROT_EXEC,
		    MAP_INHERIT_NONE,
		    MADV_NORMAL,
		    UVM_FLAG_FIXED | UVM_FLAG_COPYONW));
		if (ret) {
			printf("vm_impl_init_vmx: uvm_mapanon failed (%d)\n",
			    ret);
			/* uvm_map_deallocate calls pmap_destroy for us */
			uvm_map_deallocate(vm->vm_map);
			vm->vm_map = NULL;
			return (ENOMEM);
		}
	}

	/* Convert the low 512GB of the pmap to EPT */
	ret = pmap_convert(pmap, PMAP_TYPE_EPT);
	if (ret) {
		printf("vm_impl_init_vmx: pmap_convert failed\n");
		/* uvm_map_deallocate calls pmap_destroy for us */
		uvm_map_deallocate(vm->vm_map);
		vm->vm_map = NULL;
		return (ENOMEM);
	}

	return (0);
}

/*
 * vm_impl_init_svm
 *
 * AMD SVM specific VM initialization routine
 */
int
vm_impl_init_svm(struct vm *vm)
{
	/* XXX removed due to rot */
	return (-1);
}

/*
 * vm_impl_init
 *
 * Calls the architecture-specific VM init routine
 */
int
vm_impl_init(struct vm *vm)
{
	if (vmm_softc->mode == VMM_MODE_VMX ||
	    vmm_softc->mode == VMM_MODE_EPT)
		return vm_impl_init_vmx(vm);
	else if	(vmm_softc->mode == VMM_MODE_SVM ||
		 vmm_softc->mode == VMM_MODE_RVI)
		return vm_impl_init_svm(vm);
	else
		panic("unknown vmm mode\n");
}

/*
 * vm_impl_deinit_vmx
 *
 * Intel VMX specific VM initialization routine
 */
void
vm_impl_deinit_vmx(struct vm *vm)
{
	/* Unused */
}

/*
 * vm_impl_deinit_svm
 *
 * AMD SVM specific VM initialization routine
 */
void
vm_impl_deinit_svm(struct vm *vm)
{
	/* Unused */
}

/*
 * vm_impl_deinit
 *
 * Calls the architecture-specific VM init routine
 */
void
vm_impl_deinit(struct vm *vm)
{
	if (vmm_softc->mode == VMM_MODE_VMX ||
	    vmm_softc->mode == VMM_MODE_EPT)
		vm_impl_deinit_vmx(vm);
	else if	(vmm_softc->mode == VMM_MODE_SVM ||
		 vmm_softc->mode == VMM_MODE_RVI)
		vm_impl_deinit_svm(vm);
	else
		panic("unknown vmm mode\n");
}
/*
 * vcpu_reset_regs_svm
 *
 * XXX - unimplemented
 */
int
vcpu_reset_regs_svm(struct vcpu *vcpu, struct vcpu_init_state *vis)
{
	return (0);
}

/*
 * vcpu_reset_regs_vmx
 *
 * Initializes 'vcpu's registers to supplied state
 *
 * Parameters:
 *  vcpu: the vcpu whose register state is to be initialized
 *  vis: the register state to set
 * 
 * Return values:
 *  0: registers init'ed successfully
 *  EINVAL: an error occurred setting register state
 */
int
vcpu_reset_regs_vmx(struct vcpu *vcpu, struct vcpu_init_state *vis)
{
	int ret, ug;
	uint32_t cr0, cr4;
	uint32_t pinbased, procbased, procbased2, exit, entry;
	uint32_t want1, want0;
	uint64_t msr, ctrlval, eptp, vmcs_ptr, cr3;
	uint16_t ctrl;
	struct vmx_msr_store *msr_store;

	ret = 0;
	ug = 0;

	/* Flush any old state */
	if (!vmptrst(&vmcs_ptr)) {
		if (vmcs_ptr != 0xFFFFFFFFFFFFFFFFULL) {
			if (vmclear(&vmcs_ptr)) {
				ret = EINVAL;
				goto exit;
			}
		}
	} else {
		ret = EINVAL;
		goto exit;
	}

	/* Flush the VMCS */
	if (vmclear(&vcpu->vc_control_pa)) {
		ret = EINVAL;
		goto exit;
	}

	/*
	 * Load the VMCS onto this PCPU so we can write registers
	 */
	if (vmptrld(&vcpu->vc_control_pa)) {
		ret = EINVAL;
		goto exit;
	}

	/* Compute Basic Entry / Exit Controls */
	vcpu->vc_vmx_basic = rdmsr(IA32_VMX_BASIC);
	vcpu->vc_vmx_entry_ctls = rdmsr(IA32_VMX_ENTRY_CTLS);
	vcpu->vc_vmx_exit_ctls = rdmsr(IA32_VMX_EXIT_CTLS);
	vcpu->vc_vmx_pinbased_ctls = rdmsr(IA32_VMX_PINBASED_CTLS);
	vcpu->vc_vmx_procbased_ctls = rdmsr(IA32_VMX_PROCBASED_CTLS);

	/* Compute True Entry / Exit Controls (if applicable) */
	if (vcpu->vc_vmx_basic & IA32_VMX_TRUE_CTLS_AVAIL) {
		vcpu->vc_vmx_true_entry_ctls = rdmsr(IA32_VMX_TRUE_ENTRY_CTLS);
		vcpu->vc_vmx_true_exit_ctls = rdmsr(IA32_VMX_TRUE_EXIT_CTLS);
		vcpu->vc_vmx_true_pinbased_ctls =
		    rdmsr(IA32_VMX_TRUE_PINBASED_CTLS);
		vcpu->vc_vmx_true_procbased_ctls =
		    rdmsr(IA32_VMX_TRUE_PROCBASED_CTLS);
	}

	/* Compute Secondary Procbased Controls (if applicable) */
	if (vcpu_vmx_check_cap(vcpu, IA32_VMX_PROCBASED_CTLS,
	    IA32_VMX_ACTIVATE_SECONDARY_CONTROLS, 1))
		vcpu->vc_vmx_procbased2_ctls = rdmsr(IA32_VMX_PROCBASED2_CTLS);


# if 0
	/* XXX not needed now with MSR list */

	/* Default Guest PAT (if applicable) */
	if ((vcpu_vmx_check_cap(vcpu, IA32_VMX_ENTRY_CTLS,
	    IA32_VMX_LOAD_IA32_PAT_ON_ENTRY, 1)) ||
	    vcpu_vmx_check_cap(vcpu, IA32_VMX_EXIT_CTLS,
	    IA32_VMX_SAVE_IA32_PAT_ON_EXIT, 1)) {
		pat_default = PATENTRY(0, PAT_WB) | PATENTRY(1, PAT_WT) |
		    PATENTRY(2, PAT_UCMINUS) | PATENTRY(3, PAT_UC) |
		    PATENTRY(4, PAT_WB) | PATENTRY(5, PAT_WT) |
		    PATENTRY(6, PAT_UCMINUS) | PATENTRY(7, PAT_UC);
		if (vmwrite(VMCS_GUEST_IA32_PAT, pat_default)) {
			ret = EINVAL;
			goto exit;
		}
	}

	/* Host PAT (if applicable) */
	if (vcpu_vmx_check_cap(vcpu, IA32_VMX_EXIT_CTLS,
	    IA32_VMX_LOAD_IA32_PAT_ON_EXIT, 1)) {
		msr = rdmsr(MSR_CR_PAT);
		if (vmwrite(VMCS_HOST_IA32_PAT, msr)) {
			ret = EINVAL;
			goto exit;
		}
	}
#endif

	/*
	 * Pinbased ctrls
	 *
	 * We must be able to set the following:
	 * IA32_VMX_EXTERNAL_INT_EXITING - exit on host interrupt
	 * IA32_VMX_NMI_EXITING - exit on host NMI
	 */
	want1 = IA32_VMX_EXTERNAL_INT_EXITING |
	    IA32_VMX_NMI_EXITING;
	want0 = 0;

	if (vcpu->vc_vmx_basic & IA32_VMX_TRUE_CTLS_AVAIL) {
		ctrl = IA32_VMX_TRUE_PINBASED_CTLS;
		ctrlval = vcpu->vc_vmx_true_pinbased_ctls;
	} else {
		ctrl = IA32_VMX_PINBASED_CTLS;
		ctrlval = vcpu->vc_vmx_pinbased_ctls;
	}

	if (vcpu_vmx_compute_ctrl(vcpu, ctrlval, ctrl, want1, want0,
	    &pinbased)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_PINBASED_CTLS, pinbased)) {
		ret = EINVAL;
		goto exit;
	}

	/*
	 * Procbased ctrls
	 *
	 * We must be able to set the following:
	 * IA32_VMX_HLT_EXITING - exit on HLT instruction
	 * IA32_VMX_MWAIT_EXITING - exit on MWAIT instruction
	 * IA32_VMX_UNCONDITIONAL_IO_EXITING - exit on I/O instructions
	 * IA32_VMX_USE_MSR_BITMAPS - exit on various MSR accesses
	 * IA32_VMX_CR8_LOAD_EXITING - guest TPR access
	 * IA32_VMX_CR8_STORE_EXITING - guest TPR access
	 * IA32_VMX_USE_TPR_SHADOW - guest TPR access (shadow)
	 *
	 * If we have EPT, we must be able to clear the following
	 * IA32_VMX_CR3_LOAD_EXITING - don't care about guest CR3 accesses
	 * IA32_VMX_CR3_STORE_EXITING - don't care about guest CR3 accesses
	 */
	want1 = IA32_VMX_HLT_EXITING |
	    IA32_VMX_MWAIT_EXITING |
	    IA32_VMX_UNCONDITIONAL_IO_EXITING |
	    IA32_VMX_USE_MSR_BITMAPS |
	    IA32_VMX_CR8_LOAD_EXITING |
	    IA32_VMX_CR8_STORE_EXITING |
	    IA32_VMX_USE_TPR_SHADOW;
	want0 = 0;

	if (vmm_softc->mode == VMM_MODE_EPT) {
		want1 |= IA32_VMX_ACTIVATE_SECONDARY_CONTROLS;
		want0 |= IA32_VMX_CR3_LOAD_EXITING |
		    IA32_VMX_CR3_STORE_EXITING;
	}

	if (vcpu->vc_vmx_basic & IA32_VMX_TRUE_CTLS_AVAIL) {
		ctrl = IA32_VMX_TRUE_PROCBASED_CTLS;
		ctrlval = vcpu->vc_vmx_true_procbased_ctls;
	} else {
		ctrl = IA32_VMX_PROCBASED_CTLS;
		ctrlval = vcpu->vc_vmx_procbased_ctls;
	}

	if (vcpu_vmx_compute_ctrl(vcpu, ctrlval, ctrl, want1, want0,
	    &procbased)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_PROCBASED_CTLS, procbased)) {
		ret = EINVAL;
		goto exit;
	}

	/*
	 * Secondary Procbased ctrls
	 *
	 * We want to be able to set the following, if available:
	 * IA32_VMX_ENABLE_VPID - use VPIDs where available
	 *
	 * If we have EPT, we must be able to set the following:
	 * IA32_VMX_ENABLE_EPT - enable EPT
	 *
	 * If we have unrestricted guest capability, we must be able to set
	 * the following:
	 * IA32_VMX_UNRESTRICTED_GUEST - enable unrestricted guest
	 */
	want1 = 0;

	/* XXX checking for 2ndary controls can be combined here */
	if (vcpu_vmx_check_cap(vcpu, IA32_VMX_PROCBASED_CTLS,
	    IA32_VMX_ACTIVATE_SECONDARY_CONTROLS, 1)) {
		if (vcpu_vmx_check_cap(vcpu, IA32_VMX_PROCBASED2_CTLS,
		    IA32_VMX_ENABLE_VPID, 1))
			want1 |= IA32_VMX_ENABLE_VPID;
	}

	if (vmm_softc->mode == VMM_MODE_EPT)
		want1 |= IA32_VMX_ENABLE_EPT;

	if (vcpu_vmx_check_cap(vcpu, IA32_VMX_PROCBASED_CTLS,
	    IA32_VMX_ACTIVATE_SECONDARY_CONTROLS, 1)) {
		if (vcpu_vmx_check_cap(vcpu, IA32_VMX_PROCBASED2_CTLS,
		    IA32_VMX_UNRESTRICTED_GUEST, 1)) {
			want1 |= IA32_VMX_UNRESTRICTED_GUEST;
			ug = 1;
		}
	}

	want0 = ~want1;
	ctrlval = vcpu->vc_vmx_procbased2_ctls;
	ctrl = IA32_VMX_PROCBASED2_CTLS;

	if (vcpu_vmx_compute_ctrl(vcpu, ctrlval, ctrl, want1, want0,
	    &procbased2)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_PROCBASED2_CTLS, procbased2)) {
		ret = EINVAL;
		goto exit;
	}

	/*
	 * Exit ctrls
	 *
	 * We must be able to set the following:
	 * IA32_VMX_HOST_SPACE_ADDRESS_SIZE - exit to long mode
	 * IA32_VMX_ACKNOWLEDGE_INTERRUPT_ON_EXIT - ack interrupt on exit
	 * XXX clear save_debug_ctrls on exit ?
	 */
	want1 = IA32_VMX_HOST_SPACE_ADDRESS_SIZE |
	    IA32_VMX_ACKNOWLEDGE_INTERRUPT_ON_EXIT;
	want0 = 0;

	if (vcpu->vc_vmx_basic & IA32_VMX_TRUE_CTLS_AVAIL) {
		ctrl = IA32_VMX_TRUE_EXIT_CTLS;
		ctrlval = vcpu->vc_vmx_true_exit_ctls;
	} else {
		ctrl = IA32_VMX_EXIT_CTLS;
		ctrlval = vcpu->vc_vmx_exit_ctls;
	}

	if (vcpu_vmx_compute_ctrl(vcpu, ctrlval, ctrl, want1, want0, &exit)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_EXIT_CTLS, exit)) {
		ret = EINVAL;
		goto exit;
	}

	/*
	 * Entry ctrls
	 *
	 * We must be able to set the following:
	 * IA32_VMX_IA32E_MODE_GUEST (if no unrestricted guest)
	 * We must be able to clear the following:
	 * IA32_VMX_ENTRY_TO_SMM - enter to SMM
	 * IA32_VMX_DEACTIVATE_DUAL_MONITOR_TREATMENT
	 * XXX clear load debug_ctrls on entry ?
	 */
	if (ug == 1)
		want1 = 0;
	else
		want1 = IA32_VMX_IA32E_MODE_GUEST;
	want0 = IA32_VMX_ENTRY_TO_SMM |
	    IA32_VMX_DEACTIVATE_DUAL_MONITOR_TREATMENT;

	if (vcpu->vc_vmx_basic & IA32_VMX_TRUE_CTLS_AVAIL) {
		ctrl = IA32_VMX_TRUE_ENTRY_CTLS;
		ctrlval = vcpu->vc_vmx_true_entry_ctls;
	} else {
		ctrl = IA32_VMX_ENTRY_CTLS;
		ctrlval = vcpu->vc_vmx_entry_ctls;
	}

	if (vcpu_vmx_compute_ctrl(vcpu, ctrlval, ctrl, want1, want0, &entry)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_ENTRY_CTLS, entry)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmm_softc->mode == VMM_MODE_EPT) {
		eptp = vcpu->vc_parent->vm_map->pmap->pm_pdirpa;
		msr = rdmsr(IA32_VMX_EPT_VPID_CAP);
		if (msr & IA32_EPT_VPID_CAP_PAGE_WALK_4) {
			/* Page walk length 4 supported */
			eptp |= ((IA32_EPT_PAGE_WALK_LENGTH - 1) << 3);
		}

		if (msr & IA32_EPT_VPID_CAP_WB) {
			/* WB cache type supported */
			eptp |= IA32_EPT_PAGING_CACHE_TYPE_WB;
		}

		DPRINTF("guest eptp = 0x%llx\n", eptp);
		if (vmwrite(VMCS_GUEST_IA32_EPTP, eptp)) {
			ret = EINVAL;
			goto exit;
		}
	}

	if (vcpu_vmx_check_cap(vcpu, IA32_VMX_PROCBASED_CTLS,
	    IA32_VMX_ACTIVATE_SECONDARY_CONTROLS, 1)) {
		if (vcpu_vmx_check_cap(vcpu, IA32_VMX_PROCBASED2_CTLS,
		    IA32_VMX_ENABLE_VPID, 1))
			if (vmwrite(VMCS_GUEST_VPID,
			    (uint16_t)vcpu->vc_parent->vm_id)) {
				ret = EINVAL;
				goto exit;
			}
	}

	/*
	 * The next portion of code sets up the VMCS for the register state
	 * we want during VCPU start. This matches what the CPU state would
	 * be after a bootloader transition to 'start'.
	 */
	if (vmwrite(VMCS_GUEST_IA32_RFLAGS, vis->vis_rflags)) {
		ret = EINVAL;
		goto exit;
	}

	/*
	 * XXX -
	 * vg_rip gets special treatment here since we will rewrite
	 * it just before vmx_enter_guest, so it needs to match.
	 * we could just set vg_rip here and be done with (no vmwrite
	 * here) but that would require us to have proper resume
	 * handling (resume=1) in the exit handler, so for now we
	 * will just end up doing an extra vmwrite here.
	 */
	vcpu->vc_gueststate.vg_rip = vis->vis_rip;
	if (vmwrite(VMCS_GUEST_IA32_RIP, vis->vis_rip)) {
		ret = EINVAL;
		goto exit;
	}

	/*
	 * Determine which bits in CR0 have to be set to a fixed
	 * value as per Intel SDM A.7.
	 * CR0 bits in the vis parameter must match these.
	 */

	want1 = (curcpu()->ci_vmm_cap.vcc_vmx.vmx_cr0_fixed0) &
	    (curcpu()->ci_vmm_cap.vcc_vmx.vmx_cr0_fixed1);
	want0 = ~(curcpu()->ci_vmm_cap.vcc_vmx.vmx_cr0_fixed0) &
	    ~(curcpu()->ci_vmm_cap.vcc_vmx.vmx_cr0_fixed1);

	/*
	 * CR0_FIXED0 and CR0_FIXED1 may report the CR0_PG and CR0_PE bits as
	 * fixed to 1 even if the CPU supports the unrestricted guest
	 * feature. Update want1 and want0 accordingly to allow
	 * any value for CR0_PG and CR0_PE in vis->vis_cr0 if the CPU has
	 * the unrestricted guest capability.
	 */
	cr0 = vis->vis_cr0;

	if (ug) {
		want1 &= ~(CR0_PG | CR0_PE);
		want0 &= ~(CR0_PG | CR0_PE);
		cr0 &= ~(CR0_PG | CR0_PE);
	}


	/*
	 * VMX may require some bits to be set that userland should not have
	 * to care about. Set those here.
	 */
	if (want1 & CR0_NE)
		cr0 |= CR0_NE;

	if ((cr0 & want1) != want1) {
		ret = EINVAL;
		goto exit;
	}
	if ((~cr0 & want0) != want0) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_CR0, cr0)) {
		ret = EINVAL;
		goto exit;
	}

	if (ug)
		cr3 = 0;
	else
		cr3 = vis->vis_cr3;

	if (vmwrite(VMCS_GUEST_IA32_CR3, cr3)) {
		ret = EINVAL;
		goto exit;
	}

	/*
	 * Determine default CR4 as per Intel SDM A.8
	 * All flexible bits are set to 0
	 */
	cr4 = (curcpu()->ci_vmm_cap.vcc_vmx.vmx_cr4_fixed0) &
	    (curcpu()->ci_vmm_cap.vcc_vmx.vmx_cr4_fixed1);

	/*
	 * If we are starting in restricted guest mode, enable PAE
	 */
	if (ug == 0)
		cr4 |= CR4_PAE;

	if (vmwrite(VMCS_GUEST_IA32_CR4, cr4)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_RSP, vis->vis_rsp)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_SS_SEL, vis->vis_ss.vsi_sel)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_SS_LIMIT, vis->vis_ss.vsi_limit)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_SS_AR, vis->vis_ss.vsi_ar)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_SS_BASE, vis->vis_ss.vsi_base)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_DS_SEL, vis->vis_ds.vsi_sel)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_DS_LIMIT, vis->vis_ds.vsi_limit)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_DS_AR, vis->vis_ds.vsi_ar)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_DS_BASE, vis->vis_ds.vsi_base)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_ES_SEL, vis->vis_es.vsi_sel)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_ES_LIMIT, vis->vis_es.vsi_limit)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_ES_AR, vis->vis_es.vsi_ar)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_ES_BASE, vis->vis_es.vsi_base)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_FS_SEL, vis->vis_fs.vsi_sel)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_FS_LIMIT, vis->vis_fs.vsi_limit)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_FS_AR, vis->vis_fs.vsi_ar)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_FS_BASE, vis->vis_fs.vsi_base)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_GS_SEL, vis->vis_gs.vsi_sel)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_GS_LIMIT, vis->vis_gs.vsi_limit)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_GS_AR, vis->vis_gs.vsi_ar)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_GS_BASE, vis->vis_gs.vsi_base)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_CS_SEL, vis->vis_cs.vsi_sel)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_CS_LIMIT, vis->vis_cs.vsi_limit)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_CS_AR, vis->vis_cs.vsi_ar)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_CS_BASE, vis->vis_cs.vsi_base)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_GDTR_LIMIT, vis->vis_gdtr.vsi_limit)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_GDTR_BASE, vis->vis_gdtr.vsi_base)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_IDTR_LIMIT, vis->vis_idtr.vsi_limit)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_IDTR_BASE, vis->vis_idtr.vsi_base)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_LDTR_SEL, vis->vis_ldtr.vsi_sel)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_LDTR_LIMIT, vis->vis_ldtr.vsi_limit)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_LDTR_AR, vis->vis_ldtr.vsi_ar)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_LDTR_BASE, vis->vis_ldtr.vsi_base)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_TR_SEL, vis->vis_tr.vsi_sel)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_TR_LIMIT, vis->vis_tr.vsi_limit)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_TR_AR, vis->vis_tr.vsi_ar)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_GUEST_IA32_TR_BASE, vis->vis_tr.vsi_base)) {
		ret = EINVAL;
		goto exit;
	}

	/*
	 * Select MSRs to be loaded on exit
	 */
	msr_store = (struct vmx_msr_store *)vcpu->vc_vmx_msr_exit_load_va;
	msr_store[0].vms_index = MSR_EFER;
	msr_store[0].vms_data = rdmsr(MSR_EFER);
	msr_store[1].vms_index = MSR_CR_PAT;
	msr_store[1].vms_data = rdmsr(MSR_CR_PAT);
	msr_store[2].vms_index = MSR_STAR;
	msr_store[2].vms_data = rdmsr(MSR_STAR);
	msr_store[3].vms_index = MSR_LSTAR;
	msr_store[3].vms_data = rdmsr(MSR_LSTAR);
	msr_store[4].vms_index = MSR_CSTAR;
	msr_store[4].vms_data = rdmsr(MSR_CSTAR);
	msr_store[5].vms_index = MSR_SFMASK;
	msr_store[5].vms_data = rdmsr(MSR_SFMASK);
	msr_store[6].vms_index = MSR_KERNELGSBASE;
	msr_store[6].vms_data = rdmsr(MSR_KERNELGSBASE);

	/*
	 * Select MSRs to be loaded on entry / saved on exit
	 */
	msr_store = (struct vmx_msr_store *)vcpu->vc_vmx_msr_exit_save_va;

	/*
	 * Make sure LME is enabled in EFER if restricted guest mode is
	 * needed.
	 */
	msr_store[0].vms_index = MSR_EFER;
	if (ug == 1)
		msr_store[0].vms_data = 0ULL;	/* Initial value */
	else
		msr_store[0].vms_data = EFER_LME;

	msr_store[1].vms_index = MSR_CR_PAT;
	msr_store[1].vms_data = 0ULL;		/* Initial value */
	msr_store[2].vms_index = MSR_STAR;
	msr_store[2].vms_data = 0ULL;		/* Initial value */
	msr_store[3].vms_index = MSR_LSTAR;
	msr_store[3].vms_data = 0ULL;		/* Initial value */
	msr_store[4].vms_index = MSR_CSTAR;
	msr_store[4].vms_data = 0ULL;		/* Initial value */
	msr_store[5].vms_index = MSR_SFMASK;
	msr_store[5].vms_data = 0ULL;		/* Initial value */
	msr_store[6].vms_index = MSR_KERNELGSBASE;
	msr_store[6].vms_data = 0ULL;		/* Initial value */

	/*
	 * Currently we have the same count of entry/exit MSRs loads/stores
	 * but this is not an architectural requirement.
	 */
	if (vmwrite(VMCS_EXIT_MSR_STORE_COUNT, VMX_NUM_MSR_STORE)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_EXIT_MSR_LOAD_COUNT, VMX_NUM_MSR_STORE)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_ENTRY_MSR_LOAD_COUNT, VMX_NUM_MSR_STORE)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_EXIT_STORE_MSR_ADDRESS,
	    vcpu->vc_vmx_msr_exit_save_pa)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_EXIT_LOAD_MSR_ADDRESS,
	    vcpu->vc_vmx_msr_exit_load_pa)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_ENTRY_LOAD_MSR_ADDRESS,
	    vcpu->vc_vmx_msr_exit_save_pa)) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_MSR_BITMAP_ADDRESS,
	    vcpu->vc_msr_bitmap_pa)) {
		ret = EINVAL;
		goto exit;
	}

	/* XXX msr bitmap - set restrictions */
	/* XXX CR0 shadow */
	/* XXX CR4 shadow */

	/* Flush the VMCS */
	if (vmclear(&vcpu->vc_control_pa)) {
		ret = EINVAL;
		goto exit;
	}

exit:
	return (ret);
}

/*
 * vcpu_init_vmx
 *
 * Intel VMX specific VCPU initialization routine.
 *
 * This function allocates various per-VCPU memory regions, sets up initial
 * VCPU VMCS controls, and sets initial register values.
 */
int
vcpu_init_vmx(struct vcpu *vcpu)
{
	struct vmcs *vmcs;
	uint32_t cr0, cr4;
	paddr_t control_pa;
	int ret;

	ret = 0;

	/* Allocate VMCS VA */
	vcpu->vc_control_va = (vaddr_t)km_alloc(PAGE_SIZE, &kv_page, &kp_zero,
	    &kd_waitok);

	if (!vcpu->vc_control_va)
		return (ENOMEM);

	/* Compute VMCS PA */
	if (!pmap_extract(pmap_kernel(), vcpu->vc_control_va, &control_pa)) {
		ret = ENOMEM;
		goto exit;
	}

	vcpu->vc_control_pa = (uint64_t)control_pa;

	/* Allocate MSR bitmap VA */
	/* XXX dont need this if no msr bitmap support */
	vcpu->vc_msr_bitmap_va = (vaddr_t)km_alloc(PAGE_SIZE, &kv_page, &kp_zero,
	    &kd_waitok);

	if (!vcpu->vc_msr_bitmap_va) {
		ret = ENOMEM;
		goto exit;
	}

	/* Compute MSR bitmap PA */
	if (!pmap_extract(pmap_kernel(), vcpu->vc_msr_bitmap_va, &control_pa)) {
		ret = ENOMEM;
		goto exit;
	}

	vcpu->vc_msr_bitmap_pa = (uint64_t)control_pa;

	/* Allocate MSR exit load area VA */
	/* XXX may not need this with MSR bitmaps */
	vcpu->vc_vmx_msr_exit_load_va = (vaddr_t)km_alloc(PAGE_SIZE, &kv_page,
	   &kp_zero, &kd_waitok);

	if (!vcpu->vc_vmx_msr_exit_load_va) {
		ret = ENOMEM;
		goto exit;
	}

	/* Compute MSR exit load area PA */
	if (!pmap_extract(pmap_kernel(), vcpu->vc_vmx_msr_exit_load_va,
	    &vcpu->vc_vmx_msr_exit_load_pa)) {
		ret = ENOMEM;
		goto exit;
	}

	/* Allocate MSR exit save area VA */
	/* XXX may not need this with MSR bitmaps */
	vcpu->vc_vmx_msr_exit_save_va = (vaddr_t)km_alloc(PAGE_SIZE, &kv_page,
	   &kp_zero, &kd_waitok);

	if (!vcpu->vc_vmx_msr_exit_save_va) {
		ret = ENOMEM;
		goto exit;
	}

	/* Compute MSR exit save area PA */
	if (!pmap_extract(pmap_kernel(), vcpu->vc_vmx_msr_exit_save_va,
	    &vcpu->vc_vmx_msr_exit_save_pa)) {
		ret = ENOMEM;
		goto exit;
	}

	/* Allocate MSR entry load area VA */
	/* XXX may not need this with MSR bitmaps */
	vcpu->vc_vmx_msr_entry_load_va = (vaddr_t)km_alloc(PAGE_SIZE, &kv_page,
	   &kp_zero, &kd_waitok);

	if (!vcpu->vc_vmx_msr_entry_load_va) {
		ret = ENOMEM;
		goto exit;
	}

	/* Compute MSR entry load area PA */
	if (!pmap_extract(pmap_kernel(), vcpu->vc_vmx_msr_entry_load_va,
	    &vcpu->vc_vmx_msr_entry_load_pa)) {
		ret = ENOMEM;
		goto exit;
	}

	vmcs = (struct vmcs *)vcpu->vc_control_va;
	vmcs->vmcs_revision = curcpu()->ci_vmm_cap.vcc_vmx.vmx_vmxon_revision;

	/* Flush the VMCS */
	if (vmclear(&vcpu->vc_control_pa)) {
		ret = EINVAL;
		goto exit;
	}

	/*
	 * Load the VMCS onto this PCPU so we can write registers
	 */
	if (vmptrld(&vcpu->vc_control_pa)) {
		ret = EINVAL;
		goto exit;
	}

	/* Host CR0 */
	cr0 = rcr0();
	if (vmwrite(VMCS_HOST_IA32_CR0, cr0)) {
		ret = EINVAL;
		goto exit;
	}

	/* Host CR4 */
	cr4 = rcr4();
	if (vmwrite(VMCS_HOST_IA32_CR4, cr4)) {
		ret = EINVAL;
		goto exit;
	}

	/* Host Segment Selectors */
	if (vmwrite(VMCS_HOST_IA32_CS_SEL, GSEL(GCODE_SEL, SEL_KPL))) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_HOST_IA32_DS_SEL, GSEL(GDATA_SEL, SEL_KPL))) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_HOST_IA32_ES_SEL, GSEL(GDATA_SEL, SEL_KPL))) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_HOST_IA32_FS_SEL, GSEL(GDATA_SEL, SEL_KPL))) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_HOST_IA32_GS_SEL, GSEL(GDATA_SEL, SEL_KPL))) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_HOST_IA32_SS_SEL, GSEL(GDATA_SEL, SEL_KPL))) {
		ret = EINVAL;
		goto exit;
	}

	if (vmwrite(VMCS_HOST_IA32_TR_SEL, GSYSSEL(GPROC0_SEL, SEL_KPL))) {
		ret = EINVAL;
		goto exit;
	}

	/* Host IDTR base */
	if (vmwrite(VMCS_HOST_IA32_IDTR_BASE, idt_vaddr)) {
		ret = EINVAL;
		goto exit;
	}

	/* VMCS link */
	if (vmwrite(VMCS_LINK_POINTER, 0xFFFFFFFFFFFFFFFF)) {
		ret = EINVAL;
		goto exit;
	}

exit:
	if (ret) {
		if (vcpu->vc_control_va)
			km_free((void *)vcpu->vc_control_va, PAGE_SIZE,
			    &kv_page, &kp_zero);
		if (vcpu->vc_msr_bitmap_va)
			km_free((void *)vcpu->vc_msr_bitmap_va, PAGE_SIZE,
			    &kv_page, &kp_zero);
		if (vcpu->vc_vmx_msr_exit_save_va)
			km_free((void *)vcpu->vc_vmx_msr_exit_save_va,
			    PAGE_SIZE, &kv_page, &kp_zero);
		if (vcpu->vc_vmx_msr_exit_load_va)
			km_free((void *)vcpu->vc_vmx_msr_exit_load_va,
			    PAGE_SIZE, &kv_page, &kp_zero);
		if (vcpu->vc_vmx_msr_entry_load_va)
			km_free((void *)vcpu->vc_vmx_msr_entry_load_va,
			    PAGE_SIZE, &kv_page, &kp_zero);
	}

	return (ret);
}

/*
 * vcpu_reset_regs
 *
 * Resets a vcpu's registers to the provided state
 *
 * Parameters:
 *  vcpu: the vcpu whose registers shall be reset
 *  vis: the desired register state
 *
 * Return values:
 *  0: the vcpu's registers were successfully reset
 *  !0: the vcpu's registers could not be reset (see arch-specific reset
 *      function for various values that can be returned here)
 */
int 
vcpu_reset_regs(struct vcpu *vcpu, struct vcpu_init_state *vis)
{
	int ret;

	if (vmm_softc->mode == VMM_MODE_VMX ||
	    vmm_softc->mode == VMM_MODE_EPT)
		ret = vcpu_reset_regs_vmx(vcpu, vis);
	else if (vmm_softc->mode == VMM_MODE_SVM ||
		 vmm_softc->mode == VMM_MODE_RVI)
		ret = vcpu_reset_regs_svm(vcpu, vis);
	else
		panic("unknown vmm mode\n");

	return (ret);
}

/*
 * vcpu_init_svm
 *
 * AMD SVM specific VCPU initialization routine.
 */
int
vcpu_init_svm(struct vcpu *vcpu)
{
	/* XXX removed due to rot */
	return (0);
}

/*
 * vcpu_init
 *
 * Calls the architecture-specific VCPU init routine
 */
int
vcpu_init(struct vcpu *vcpu)
{
	int ret = 0;

	vcpu->vc_hsa_stack_va = (vaddr_t)malloc(PAGE_SIZE, M_DEVBUF,
	    M_NOWAIT|M_ZERO);
	if (!vcpu->vc_hsa_stack_va)
		return (ENOMEM);

	vcpu->vc_virt_mode = vmm_softc->mode;
	vcpu->vc_state = VCPU_STATE_STOPPED;
	if (vmm_softc->mode == VMM_MODE_VMX ||
	    vmm_softc->mode == VMM_MODE_EPT)
		ret = vcpu_init_vmx(vcpu);
	else if (vmm_softc->mode == VMM_MODE_SVM ||
		 vmm_softc->mode == VMM_MODE_RVI)
		ret = vcpu_init_svm(vcpu);
	else
		panic("unknown vmm mode\n");

	if (ret)
		free((void *)vcpu->vc_hsa_stack_va, M_DEVBUF, PAGE_SIZE);

	return (ret);
}

/*
 * vcpu_deinit_vmx
 *
 * Deinitializes the vcpu described by 'vcpu'
 */
void
vcpu_deinit_vmx(struct vcpu *vcpu)
{
	if (vcpu->vc_control_va)
		km_free((void *)vcpu->vc_control_va, PAGE_SIZE,
		    &kv_page, &kp_zero);
	if (vcpu->vc_vmx_msr_exit_save_va)
		km_free((void *)vcpu->vc_vmx_msr_exit_save_va,
		    PAGE_SIZE, &kv_page, &kp_zero);
	if (vcpu->vc_vmx_msr_exit_load_va)
		km_free((void *)vcpu->vc_vmx_msr_exit_load_va,
		    PAGE_SIZE, &kv_page, &kp_zero);
	if (vcpu->vc_vmx_msr_entry_load_va)
		km_free((void *)vcpu->vc_vmx_msr_entry_load_va,
		    PAGE_SIZE, &kv_page, &kp_zero);
	if (vcpu->vc_hsa_stack_va)
		free((void *)vcpu->vc_hsa_stack_va, M_DEVBUF, PAGE_SIZE);
}

/*
 * vcpu_deinit_svm
 *
 * Deinitializes the vcpu described by 'vcpu'
 */
void
vcpu_deinit_svm(struct vcpu *vcpu)
{
	/* Unused */
}

/*
 * vcpu_deinit
 *
 * Calls the architecture-specific VCPU deinit routine
 */
void
vcpu_deinit(struct vcpu *vcpu)
{
	if (vmm_softc->mode == VMM_MODE_VMX ||
	    vmm_softc->mode == VMM_MODE_EPT)
		vcpu_deinit_vmx(vcpu);
	else if	(vmm_softc->mode == VMM_MODE_SVM ||
		 vmm_softc->mode == VMM_MODE_RVI)
		vcpu_deinit_svm(vcpu);
	else
		panic("unknown vmm mode\n");
}

/*
 * vm_teardown
 *
 * Tears down (destroys) the vm indicated by 'vm'.
 */
void
vm_teardown(struct vm *vm)
{
	struct vcpu *vcpu, *tmp;

	/* Free VCPUs */
	rw_enter_write(&vm->vm_vcpu_lock);
	SLIST_FOREACH_SAFE(vcpu, &vm->vm_vcpu_list, vc_vcpu_link, tmp) {
		SLIST_REMOVE(&vm->vm_vcpu_list, vcpu, vcpu, vc_vcpu_link);
		vcpu_deinit(vcpu);
		pool_put(&vcpu_pool, vcpu);
	}

	vm_impl_deinit(vm);

	/* teardown guest vmspace */
	if (vm->vm_map != NULL)
		uvm_map_deallocate(vm->vm_map);

	vmm_softc->vm_ct--;
	if (vmm_softc->vm_ct < 1)
		vmm_stop();
	rw_exit_write(&vm->vm_vcpu_lock);
	pool_put(&vm_pool, vm);
}

/*
 * vcpu_vmx_check_cap
 *
 * Checks if the 'cap' bit in the 'msr' MSR can be set or cleared (set = 1
 * or set = 0, respectively).
 *
 * When considering 'msr', we check to see if true controls are available,
 * and use those if so.
 *
 * Returns 1 of 'cap' can be set/cleared as requested, 0 otherwise.
 */
int
vcpu_vmx_check_cap(struct vcpu *vcpu, uint32_t msr, uint32_t cap, int set)
{
	uint64_t ctl;

	if (vcpu->vc_vmx_basic & IA32_VMX_TRUE_CTLS_AVAIL) {
		switch (msr) {
		case IA32_VMX_PINBASED_CTLS:
			ctl = vcpu->vc_vmx_true_pinbased_ctls;
			break;
		case IA32_VMX_PROCBASED_CTLS:
			ctl = vcpu->vc_vmx_true_procbased_ctls;
			break;
		case IA32_VMX_PROCBASED2_CTLS:
			ctl = vcpu->vc_vmx_procbased2_ctls;
			break;
		case IA32_VMX_ENTRY_CTLS:
			ctl = vcpu->vc_vmx_true_entry_ctls;
			break;
		case IA32_VMX_EXIT_CTLS:
			ctl = vcpu->vc_vmx_true_exit_ctls;
			break;
		default:
			return (0);
		}
	} else {
		switch (msr) {
		case IA32_VMX_PINBASED_CTLS:
			ctl = vcpu->vc_vmx_pinbased_ctls;
			break;
		case IA32_VMX_PROCBASED_CTLS:
			ctl = vcpu->vc_vmx_procbased_ctls;
			break;
		case IA32_VMX_PROCBASED2_CTLS:
			ctl = vcpu->vc_vmx_procbased2_ctls;
			break;
		case IA32_VMX_ENTRY_CTLS:
			ctl = vcpu->vc_vmx_entry_ctls;
			break;
		case IA32_VMX_EXIT_CTLS:
			ctl = vcpu->vc_vmx_exit_ctls;
			break;
		default:
			return (0);
		}
	}

	if (set) {
		/* Check bit 'cap << 32', must be !0 */
		return (ctl & ((uint64_t)cap << 32)) != 0;
	} else {
		/* Check bit 'cap', must be 0 */
		return (ctl & cap) == 0;
	}
}

/*
 * vcpu_vmx_compute_ctrl
 *
 * Computes the appropriate control value, given the supplied parameters
 * and CPU capabilities.
 *
 * Intel has made somewhat of a mess of this computation - it is described
 * using no fewer than three different approaches, spread across many
 * pages of the SDM. Further compounding the problem is the fact that now
 * we have "true controls" for each type of "control", and each needs to
 * be examined to get the calculation right, but only if "true" controls
 * are present on the CPU we're on.
 *
 * Parameters:
 *  vcpu: the vcpu for which controls are to be computed. (XXX now unused)
 *  ctrlval: the control value, as read from the CPU MSR
 *  ctrl: which control is being set (eg, pinbased, procbased, etc)
 *  want0: the set of desired 0 bits
 *  want1: the set of desired 1 bits
 *  out: (out) the correct value to write into the VMCS for this VCPU,
 *      for the 'ctrl' desired.
 *
 * Returns 0 if successful, or EINVAL if the supplied parameters define
 *     an unworkable control setup.
 */
int
vcpu_vmx_compute_ctrl(struct vcpu *vcpu, uint64_t ctrlval, uint16_t ctrl,
    uint32_t want1, uint32_t want0, uint32_t *out)
{
	int i, set, clear;

	/*
	 * The Intel SDM gives three formulae for determining which bits to
	 * set/clear for a given control and desired functionality. Formula
	 * 1 is the simplest but disallows use of newer features that are
	 * enabled by functionality in later CPUs.
	 *
	 * Formulas 2 and 3 allow such extra functionality. We use formula
	 * 2 - this requires us to know the identity of controls in the
	 * "default1" class for each control register, but allows us to not
	 * have to pass along and/or query both sets of capability MSRs for
	 * each control lookup. This makes the code slightly longer,
	 * however.
	 */
	for (i = 0; i < 32; i++) {
		/* Figure out if we can set and / or clear this bit */
		set = (ctrlval & (1ULL << (i + 32))) != 0;
		clear = ((1ULL << i) & ((uint64_t)ctrlval)) == 0;

		/* If the bit can't be set nor cleared, something's wrong */
		if (!set && !clear)
			return (EINVAL);

		/*
		 * Formula 2.c.i - "If the relevant VMX capability MSR
		 * reports that a control has a single setting, use that
		 * setting."
		 */
		if (set && !clear) {
			if (want0 & (1ULL << i))
				return (EINVAL);
			else 
				*out |= (1ULL << i);
		} else if (clear && !set) {
			if (want1 & (1ULL << i))
				return (EINVAL);
			else
				*out &= ~(1ULL << i);
		} else {
			/*
			 * 2.c.ii - "If the relevant VMX capability MSR
			 * reports that a control can be set to 0 or 1
			 * and that control's meaning is known to the VMM,
			 * set the control based on the functionality desired."
			 */
			if (want1 & (1ULL << i))
				*out |= (1ULL << i);
			else if (want0 & (1 << i))
				*out &= ~(1ULL << i);
			else {
				/*
				 * ... assuming the control's meaning is not
				 * known to the VMM ...
				 *
				 * 2.c.iii - "If the relevant VMX capability
				 * MSR reports that a control can be set to 0
			 	 * or 1 and the control is not in the default1
				 * class, set the control to 0."
				 *
				 * 2.c.iv - "If the relevant VMX capability
				 * MSR reports that a control can be set to 0
				 * or 1 and the control is in the default1
				 * class, set the control to 1."
				 */
				switch (ctrl) {
				case IA32_VMX_PINBASED_CTLS:
				case IA32_VMX_TRUE_PINBASED_CTLS:
					/*
					 * A.3.1 - default1 class of pinbased
					 * controls comprises bits 1,2,4
					 */
					switch (i) {
						case 1:
						case 2:
						case 4:
							*out |= (1ULL << i);
							break;
						default:
							*out &= ~(1ULL << i);
							break;
					}
					break;
				case IA32_VMX_PROCBASED_CTLS:
				case IA32_VMX_TRUE_PROCBASED_CTLS:
					/*
					 * A.3.2 - default1 class of procbased
					 * controls comprises bits 1, 4-6, 8,
					 * 13-16, 26
					 */
					switch (i) {
						case 1:
						case 4 ... 6:
						case 8:
						case 13 ... 16:
						case 26:
							*out |= (1ULL << i);
							break;
						default:
							*out &= ~(1ULL << i);
							break;
					}
					break;
					/*
					 * Unknown secondary procbased controls
					 * can always be set to 0
					 */
				case IA32_VMX_PROCBASED2_CTLS:
					*out &= ~(1ULL << i);
					break;
				case IA32_VMX_EXIT_CTLS:
				case IA32_VMX_TRUE_EXIT_CTLS:
					/*
					 * A.4 - default1 class of exit
					 * controls comprises bits 0-8, 10,
					 * 11, 13, 14, 16, 17
					 */
					switch (i) {
						case 0 ... 8:
						case 10 ... 11:
						case 13 ... 14:
						case 16 ... 17:
							*out |= (1ULL << i);
							break;
						default:
							*out &= ~(1ULL << i);
							break;
					}
					break;
				case IA32_VMX_ENTRY_CTLS:
				case IA32_VMX_TRUE_ENTRY_CTLS:
					/*
					 * A.5 - default1 class of entry
					 * controls comprises bits 0-8, 12
					 */
					switch (i) {
						case 0 ... 8:
						case 12:
							*out |= (1ULL << i);
							break;
						default:
							*out &= ~(1ULL << i);
							break;
					}
					break;
				}
			}
		}
	}

	return (0);
}

/*
 * vm_get_info
 *
 * Returns information about the VM indicated by 'vip'.
 */
int
vm_get_info(struct vm_info_params *vip)
{
	struct vm_info_result *out;
	struct vm *vm;
	struct vcpu *vcpu;
	int i, j;
	size_t need;

	rw_enter_read(&vmm_softc->vm_lock);
	need = vmm_softc->vm_ct * sizeof(struct vm_info_result);
	if (vip->vip_size < need) {
		vip->vip_info_ct = 0;
		vip->vip_size = need;
		rw_exit_read(&vmm_softc->vm_lock);
		return (0);
	}

	out = malloc(need, M_DEVBUF, M_NOWAIT|M_ZERO);
	if (out == NULL) {
		vip->vip_info_ct = 0;
		rw_exit_read(&vmm_softc->vm_lock);
		return (ENOMEM);
	}

	i = 0;
	vip->vip_info_ct = vmm_softc->vm_ct;
	SLIST_FOREACH(vm, &vmm_softc->vm_list, vm_link) {
		out[i].vir_memory_size = vm->vm_memory_size;
		out[i].vir_used_size =
		    pmap_resident_count(vm->vm_map->pmap) * PAGE_SIZE;
		out[i].vir_ncpus = vm->vm_vcpu_ct;
		out[i].vir_id = vm->vm_id;
		out[i].vir_creator_pid = vm->vm_creator_pid;
		strncpy(out[i].vir_name, vm->vm_name, VMM_MAX_NAME_LEN);
		rw_enter_read(&vm->vm_vcpu_lock);
		for (j = 0; j < vm->vm_vcpu_ct; j++) {
			out[i].vir_vcpu_state[j] = VCPU_STATE_UNKNOWN;
			SLIST_FOREACH(vcpu, &vm->vm_vcpu_list,
			    vc_vcpu_link) {
				if (vcpu->vc_id == j)
					out[i].vir_vcpu_state[j] =
					    vcpu->vc_state;
			}
		}
		rw_exit_read(&vm->vm_vcpu_lock);
		i++;
	}
	rw_exit_read(&vmm_softc->vm_lock);
	if (copyout(out, vip->vip_info, need) == EFAULT) {
		free(out, M_DEVBUF, need);
		return (EFAULT);
	}

	free(out, M_DEVBUF, need);
	return (0);
}

/*
 * vm_terminate
 *
 * Terminates the VM indicated by 'vtp'.
 */
int
vm_terminate(struct vm_terminate_params *vtp)
{
	struct vm *vm;
	struct vcpu *vcpu;
	u_int old, next;

	/*
	 * Find desired VM
	 */
	rw_enter_read(&vmm_softc->vm_lock);
	SLIST_FOREACH(vm, &vmm_softc->vm_list, vm_link) {
		if (vm->vm_id == vtp->vtp_vm_id)
			break;
	}

	if (vm != NULL) {
		rw_enter_read(&vm->vm_vcpu_lock);
		SLIST_FOREACH(vcpu, &vm->vm_vcpu_list, vc_vcpu_link) {
			do {
				old = vcpu->vc_state;
				if (old == VCPU_STATE_RUNNING)
					next = VCPU_STATE_REQTERM;
				else if (old == VCPU_STATE_STOPPED)
					next = VCPU_STATE_TERMINATED;
				else /* must be REQTERM or TERMINATED */
					break;
			} while (old != atomic_cas_uint(&vcpu->vc_state,
			    old, next));
		}
		rw_exit_read(&vm->vm_vcpu_lock);
	}
	rw_exit_read(&vmm_softc->vm_lock);

	if (vm == NULL)
		return (ENOENT);

	/* XXX possible race here two threads terminating the same vm? */
	rw_enter_write(&vmm_softc->vm_lock);
	SLIST_REMOVE(&vmm_softc->vm_list, vm, vm, vm_link);
	rw_exit_write(&vmm_softc->vm_lock);
	if (vm->vm_vcpus_running == 0)
		vm_teardown(vm);

	return (0);
}

/*
 * vm_run
 *
 * Run the vm / vcpu specified by 'vrp'
 */
int
vm_run(struct vm_run_params *vrp)
{
	struct vm *vm;
	struct vcpu *vcpu;
	int ret = 0;
	u_int old, next;

	/*
	 * Find desired VM
	 */
	rw_enter_read(&vmm_softc->vm_lock);

	SLIST_FOREACH(vm, &vmm_softc->vm_list, vm_link) {
		if (vm->vm_id == vrp->vrp_vm_id)
			break;
	}

	/*
	 * Attempt to locate the requested VCPU. If found, attempt to
	 * to transition from VCPU_STATE_STOPPED -> VCPU_STATE_RUNNING.
	 * Failure to make the transition indicates the VCPU is busy.
	 */
	if (vm != NULL) {
		rw_enter_read(&vm->vm_vcpu_lock);
		SLIST_FOREACH(vcpu, &vm->vm_vcpu_list, vc_vcpu_link) {
			if (vcpu->vc_id == vrp->vrp_vcpu_id)
				break;
		}

		if (vcpu != NULL) {
			old = VCPU_STATE_STOPPED;
			next = VCPU_STATE_RUNNING;

			if (atomic_cas_uint(&vcpu->vc_state, old, next) != old)
				ret = EBUSY;
			else
				atomic_inc_int(&vm->vm_vcpus_running);
		}
		rw_exit_read(&vm->vm_vcpu_lock);

		if (vcpu == NULL)
			ret = ENOENT;
	}
	rw_exit_read(&vmm_softc->vm_lock);

	if (vm == NULL)
		ret = ENOENT;

	/* Bail if errors detected in the previous steps */
	if (ret)
		return (ret);

	/*
	 * We may be returning from userland helping us from the last exit.
	 * If so (vrp_continue == 1), copy in the exit data from vmd. The
	 * exit data will be consumed before the next entry (this typically
	 * comprises VCPU register changes as the result of vmd(8)'s actions).
	 */
	if (vrp->vrp_continue) {
		if (copyin(vrp->vrp_exit, &vcpu->vc_exit,
		    sizeof(union vm_exit)) == EFAULT) {
			return (EFAULT);
		}
	}

	/* Run the VCPU specified in vrp */
	if (vcpu->vc_virt_mode == VMM_MODE_VMX ||
	    vcpu->vc_virt_mode == VMM_MODE_EPT) {
		ret = vcpu_run_vmx(vcpu, vrp->vrp_continue, &vrp->vrp_injint);
	} else if (vcpu->vc_virt_mode == VMM_MODE_SVM ||
		   vcpu->vc_virt_mode == VMM_MODE_RVI) {
		ret = vcpu_run_svm(vcpu, vrp->vrp_continue);
	}

	/*
	 * We can set the VCPU states here without CAS because once
	 * a VCPU is in state RUNNING or REQTERM, only the VCPU itself
	 * can switch the state.
	 */
	atomic_dec_int(&vm->vm_vcpus_running);
	if (vcpu->vc_state == VCPU_STATE_REQTERM) {
		vrp->vrp_exit_reason = VM_EXIT_TERMINATED;
		vcpu->vc_state = VCPU_STATE_TERMINATED;
		if (vm->vm_vcpus_running == 0)
			vm_teardown(vm);
		ret = 0;
	} else if (ret == EAGAIN) {
		/* If we are exiting, populate exit data so vmd can help. */
		vrp->vrp_exit_reason = vcpu->vc_gueststate.vg_exit_reason;
		vcpu->vc_state = VCPU_STATE_STOPPED;

		if (copyout(&vcpu->vc_exit, vrp->vrp_exit,
		    sizeof(union vm_exit)) == EFAULT) {
			ret = EFAULT;
		} else
			ret = 0;
	} else if (ret == 0) {
		vrp->vrp_exit_reason = VM_EXIT_NONE;
		vcpu->vc_state = VCPU_STATE_STOPPED;
	} else {
		vrp->vrp_exit_reason = VM_EXIT_TERMINATED;
		vcpu->vc_state = VCPU_STATE_TERMINATED;
	}

	return (ret);
}

/*
 * vcpu_must_stop
 *
 * Check if we need to (temporarily) stop running the VCPU for some reason,
 * such as:
 * - the VM was requested to terminate
 * - the proc running this VCPU has pending signals
 */
int
vcpu_must_stop(struct vcpu *vcpu)
{
	struct proc *p = curproc;

	if (vcpu->vc_state == VCPU_STATE_REQTERM)
		return (1);
	if (CURSIG(p) != 0)
		return (1);
	return (0);
}

/*
 * vcpu_run_vmx
 *
 * VMM main loop used to run a VCPU.
 *
 * Parameters:
 *  vcpu: The VCPU to run
 *  from_exit: 1 if returning directly from an exit to vmd during the
 *      previous run, or 0 if we exited last time without needing to
 *      exit to vmd.
 *  injint: Interrupt that should be injected during this run, or -1 if
 *      no interrupt should be injected.
 *
 * Return values:
 *  0: The run loop exited and no help is needed from vmd
 *  EAGAIN: The run loop exited and help from vmd is needed
 *  EINVAL: an error occured
 */
int
vcpu_run_vmx(struct vcpu *vcpu, uint8_t from_exit, int16_t *injint)
{
	int ret = 0, resume, locked, exitinfo;
	struct region_descriptor gdt;
	struct cpu_info *ci;
	uint64_t exit_reason, cr3, vmcs_ptr;
	struct schedstate_percpu *spc;
	struct vmx_invvpid_descriptor vid;
	uint64_t rflags, eii;

	resume = 0;

	while (ret == 0) {
		if (!resume) {
			/*
			 * We are launching for the first time, or we are
			 * resuming from a different pcpu, so we need to
			 * reset certain pcpu-specific values.
			 */
			ci = curcpu();
			setregion(&gdt, ci->ci_gdt, GDT_SIZE - 1);

			vcpu->vc_last_pcpu = ci;

			if (vmptrld(&vcpu->vc_control_pa)) {
				ret = EINVAL;
				break;
			}

			if (gdt.rd_base == 0) {
				ret = EINVAL;
				break;
			}

			/* Host GDTR base */
			if (vmwrite(VMCS_HOST_IA32_GDTR_BASE, gdt.rd_base)) {
				ret = EINVAL;
				break;
			}

			/* Host TR base */
			if (vmwrite(VMCS_HOST_IA32_TR_BASE,
			    (uint64_t)curcpu()->ci_tss)) {
				ret = EINVAL;
				break;
			}

			/* Host CR3 */
			cr3 = rcr3();
			if (vmwrite(VMCS_HOST_IA32_CR3, cr3)) {
				ret = EINVAL;
				break;
			}
		}

		/*
		 * If we are returning from userspace (vmd) because we exited
		 * last time, fix up any needed vcpu state first.
		 */
		if (from_exit) {
			from_exit = 0;
			switch (vcpu->vc_gueststate.vg_exit_reason) {
			case VMX_EXIT_IO:
				vcpu->vc_gueststate.vg_rax =
				    vcpu->vc_exit.vei.vei_data;
				break;
			case VMX_EXIT_HLT:
				break;
			case VMX_EXIT_TRIPLE_FAULT:
				break;
			default:
				printf("vcpu_run_vmx: returning from exit "
				    "with unknown reason %d (%s)\n",
				    vcpu->vc_gueststate.vg_exit_reason,
				    vmx_exit_reason_decode(
					vcpu->vc_gueststate.vg_exit_reason));
				break;
			}
		}

		/*
		 * XXX - clock hack. We don't track host clocks while not
		 * running inside a VM, and thus we lose many clocks while
		 * the OS is running other processes. For now, approximate
		 * when a clock should be injected by injecting one clock
		 * per CLOCK_BIAS exits.
		 *
		 * This should be changed to track host clocks to know if
		 * a clock tick was missed, and "catch up" clock interrupt
		 * injections later as needed.
		 *
		 * Note that checking injint here and not injecting the
		 * clock interrupt if injint is set also violates interrupt
		 * priority, until this hack is fixed.
		 */
		vmmclk++;
		eii = 0xFFFFFFFFFFFFFFFFULL;

		if (vmmclk % CLOCK_BIAS == 0)
			eii = 0x20;

		if (*injint != -1)
			eii = *injint + 0x20;

		if (eii != 0xFFFFFFFFFFFFFFFFULL) {
			if (vmread(VMCS_GUEST_IA32_RFLAGS, &rflags)) {
				printf("intr: can't read guest rflags\n");
				rflags = 0;
			}

			if (rflags & PSL_I) {
				eii |= (1ULL << 31);	/* Valid */
				eii |= (0ULL << 8);	/* Hardware Interrupt */
				if (vmwrite(VMCS_ENTRY_INTERRUPTION_INFO, eii)) {
					printf("intr: can't vector clock "
					    "interrupt to guest\n");
				}
				if (*injint != -1)
					*injint = -1;
			}
		}

		/* XXX end clock hack */

		/* Invalidate old TLB mappings */
		vid.vid_vpid = vcpu->vc_parent->vm_id;
		vid.vid_addr = 0;
		invvpid(IA32_VMX_INVVPID_SINGLE_CTX_GLB, &vid);

		/* Start / resume the VM / VCPU */
		KERNEL_ASSERT_LOCKED();
		KERNEL_UNLOCK();
		ret = vmx_enter_guest(&vcpu->vc_control_pa,
		    &vcpu->vc_gueststate, resume);

		exit_reason = VM_EXIT_NONE;
		if (ret == 0)
			exitinfo = vmx_get_exit_info(
			    &vcpu->vc_gueststate.vg_rip, &exit_reason);

		if (ret || exitinfo != VMX_EXIT_INFO_COMPLETE ||
		    exit_reason != VMX_EXIT_EXTINT) {
			KERNEL_LOCK();
			locked = 1;
		} else
			locked = 0;

		/* If we exited successfully ... */
		if (ret == 0) {
			resume = 1;
			if (!(exitinfo & VMX_EXIT_INFO_HAVE_RIP)) {
				printf("vcpu_run_vmx: cannot read guest rip\n");
				ret = EINVAL;
				break;
			}

			if (!(exitinfo & VMX_EXIT_INFO_HAVE_REASON)) {
				printf("vcpu_run_vmx: cant read exit reason\n");
				ret = EINVAL;
				break;
			}

			/*
			 * Handle the exit. This will alter "ret" to EAGAIN if
			 * the exit handler determines help from vmd is needed.
			 */
			vcpu->vc_gueststate.vg_exit_reason = exit_reason;
			ret = vmx_handle_exit(vcpu);

			/*
			 * When the guest exited due to an external interrupt,
			 * we do not yet hold the kernel lock: we need to
			 * handle interrupts first before grabbing the lock:
			 * the interrupt handler might do work that
			 * another CPU holding the kernel lock waits for.
			 *
			 * Example: the TLB shootdown code in the pmap module
			 * sends an IPI to all other CPUs and busy-waits for
			 * them to decrement tlb_shoot_wait to zero. While
			 * busy-waiting, the kernel lock is held.
			 *
			 * If this code here attempted to grab the kernel lock
			 * before handling the interrupt, it would block
			 * forever.
			 */
			if (!locked)
				KERNEL_LOCK();

			/* Exit to vmd if we are terminating or failed enter */
			if (ret || vcpu_must_stop(vcpu))
				break;

			/* Check if we should yield - don't hog the cpu */
			spc = &ci->ci_schedstate;
			if (spc->spc_schedflags & SPCF_SHOULDYIELD) {
				resume = 0;
				if (vmclear(&vcpu->vc_control_pa)) {
					ret = EINVAL;
					break;
				}
				yield();
			}
		} else if (ret == VMX_FAIL_LAUNCH_INVALID_VMCS) {
			printf("vcpu_run_vmx: failed launch with invalid "
			    "vmcs\n");
			ret = EINVAL;
		} else if (ret == VMX_FAIL_LAUNCH_VALID_VMCS) {
			exit_reason = vcpu->vc_gueststate.vg_exit_reason;
			printf("vcpu_run_vmx: failed launch with valid "
			    "vmcs, code=%lld (%s)\n", exit_reason,
			    vmx_instruction_error_decode(exit_reason));
			ret = EINVAL;
		} else {
			printf("vcpu_run_vmx: failed launch for unknown "
			    "reason\n");
			ret = EINVAL;
		}

	}

	/*
	 * We are heading back to userspace (vmd), either because we need help
	 * handling an exit, or we failed in some way to enter the guest.
	 * Clear any current VMCS pointer as we may end up coming back on
	 * a different CPU.
	 */
	if (!vmptrst(&vmcs_ptr)) {
		if (vmcs_ptr != 0xFFFFFFFFFFFFFFFFULL)
			if (vmclear(&vcpu->vc_control_pa))
				ret = EINVAL;
	} else
		ret = EINVAL;

	return (ret);
}

/*
 * vmx_handle_intr
 *
 * Handle host (external) interrupts. We read which interrupt fired by
 * extracting the vector from the VMCS and dispatch the interrupt directly
 * to the host using vmm_dispatch_intr.
 */
void
vmx_handle_intr(struct vcpu *vcpu)
{
	uint8_t vec;
	uint64_t eii;
	struct gate_descriptor *idte;
	vaddr_t handler;

	if (vmread(VMCS_EXIT_INTERRUPTION_INFO, &eii)) {
		printf("vmx_handle_intr: can't obtain intr info\n");
		return;
	}

	vec = eii & 0xFF;

	/* XXX check "error valid" code in eii, abort if 0 */
	idte=&idt[vec];
	handler = idte->gd_looffset + ((uint64_t)idte->gd_hioffset << 16);
	vmm_dispatch_intr(handler);

}

/*
 * vmx_handle_hlt
 *
 * Handle HLT exits
 */
int
vmx_handle_hlt(struct vcpu *vcpu)
{
	uint64_t insn_length;

	if (vmread(VMCS_INSTRUCTION_LENGTH, &insn_length)) {
		printf("vmx_handle_hlt: can't obtain instruction length\n");
		return (EINVAL);
	}

	vcpu->vc_gueststate.vg_rip += insn_length;
	return (EAGAIN);
}

/*
 * vmx_get_exit_info
 *
 * Returns exit information containing the current guest RIP and exit reason
 * in rip and exit_reason. The return value is a bitmask indicating whether
 * reading the RIP and exit reason was successful.
 */
int
vmx_get_exit_info(uint64_t *rip, uint64_t *exit_reason)
{
	int rv = 0;

	if (vmread(VMCS_GUEST_IA32_RIP, rip) == 0) {
		rv |= VMX_EXIT_INFO_HAVE_RIP;
		if (vmread(VMCS_EXIT_REASON, exit_reason) == 0)
			rv |= VMX_EXIT_INFO_HAVE_REASON;
	}
	return (rv);
}

/*
 * vmx_handle_exit
 *
 * Handle exits from the VM by decoding the exit reason and calling various
 * subhandlers as needed.
 */
int
vmx_handle_exit(struct vcpu *vcpu)
{
	uint64_t exit_reason;
	int update_rip, ret = 0;

	update_rip = 0;
	exit_reason = vcpu->vc_gueststate.vg_exit_reason;

	switch (exit_reason) {
	case VMX_EXIT_EPT_VIOLATION:
		ret = vmx_handle_np_fault(vcpu);
		break;
	case VMX_EXIT_CPUID:
		ret = vmx_handle_cpuid(vcpu);
		update_rip = 1;
		break;
	case VMX_EXIT_IO:
		ret = vmx_handle_inout(vcpu);
		update_rip = 1;
		break;
	case VMX_EXIT_EXTINT:
		vmx_handle_intr(vcpu);
		update_rip = 0;
		break;
	case VMX_EXIT_CR_ACCESS:
		ret = vmx_handle_cr(vcpu);
		update_rip = 1;
		break;
	case VMX_EXIT_HLT:
		ret = vmx_handle_hlt(vcpu);
		update_rip = 1;
		break;
	case VMX_EXIT_TRIPLE_FAULT:
#ifdef VMM_DEBUG
		DPRINTF("vmx_handle_exit: vm %d vcpu %d triple fault\n",
		    vcpu->vc_parent->vm_id, vcpu->vc_id);
		vmx_vcpu_dump_regs(vcpu);
		dump_vcpu(vcpu);
#endif /* VMM_DEBUG */
		ret = EAGAIN;
		update_rip = 0;
		break;
	default:
		DPRINTF("vmx_handle_exit: unhandled exit %lld (%s)\n",
		    exit_reason, vmx_exit_reason_decode(exit_reason));
		return (EINVAL);
	}

	if (update_rip) {
		if (vmwrite(VMCS_GUEST_IA32_RIP,
		    vcpu->vc_gueststate.vg_rip)) {
			printf("vmx_handle_exit: can't advance rip\n");
			return (EINVAL);
		}
	}

	return (ret);
}

/*
 * vmm_get_guest_memtype
 *
 * Returns the type of memory 'gpa' refers to in the context of vm 'vm'
 */
int
vmm_get_guest_memtype(struct vm *vm, paddr_t gpa)
{
	int i;
	struct vm_mem_range *vmr;

	if (gpa >= VMM_PCI_MMIO_BAR_BASE && gpa <= VMM_PCI_MMIO_BAR_END) {
		DPRINTF("guest mmio access @ 0x%llx\n", (uint64_t)gpa);
		return (VMM_MEM_TYPE_REGULAR);
	}

	/* XXX Use binary search? */
	for (i = 0; i < vm->vm_nmemranges; i++) {
		vmr = &vm->vm_memranges[i];

		/*
		 * vm_memranges are ascending. gpa can no longer be in one of
		 * the memranges
		 */
		if (gpa < vmr->vmr_gpa)
			break;

		if (gpa < vmr->vmr_gpa + vmr->vmr_size)
			return (VMM_MEM_TYPE_REGULAR);
	}

	DPRINTF("guest memtype @ 0x%llx unknown\n", (uint64_t)gpa);
	return (VMM_MEM_TYPE_UNKNOWN);
}

/*
 * vmm_get_guest_faulttype
 *
 * Determines the type (R/W/X) of the last fault on the VCPU last run on
 * this PCPU. Calls the appropriate architecture-specific subroutine.
 */
int
vmm_get_guest_faulttype(void)
{
	if (vmm_softc->mode == VMM_MODE_EPT)
		return vmx_get_guest_faulttype();
	else if (vmm_softc->mode == VMM_MODE_RVI)
		return vmx_get_guest_faulttype();
	else
		panic("unknown vmm mode\n");
}

/*
 * vmx_get_exit_qualification
 *
 * Return the current VMCS' exit qualification information
 */
int
vmx_get_exit_qualification(uint64_t *exit_qualification)
{
	if (vmread(VMCS_GUEST_EXIT_QUALIFICATION, exit_qualification)) {
		printf("vmm_get_exit_qualification: cant extract exit qual\n");
		return (EINVAL);
	}

	return (0);
}

/*
 * vmx_get_guest_faulttype
 *
 * Determines the type (R/W/X) of the last fault on the VCPU last run on
 * this PCPU.
 */
int
vmx_get_guest_faulttype(void)
{
	uint64_t exit_qualification;
	uint64_t presentmask = IA32_VMX_EPT_FAULT_WAS_READABLE |
	    IA32_VMX_EPT_FAULT_WAS_WRITABLE | IA32_VMX_EPT_FAULT_WAS_EXECABLE;
	uint64_t protmask = IA32_VMX_EPT_FAULT_READ |
	    IA32_VMX_EPT_FAULT_WRITE | IA32_VMX_EPT_FAULT_EXEC;

	if (vmx_get_exit_qualification(&exit_qualification))
		return (-1);

	if ((exit_qualification & presentmask) == 0)
		return VM_FAULT_INVALID;
	if (exit_qualification & protmask)
		return VM_FAULT_PROTECT;
	return (-1);
}

/*
 * svm_get_guest_faulttype
 *
 * Determines the type (R/W/X) of the last fault on the VCPU last run on
 * this PCPU.
 */
int
svm_get_guest_faulttype(void)
{
	/* XXX removed due to rot */
	return (-1);
}

/*
 * vmx_fault_page
 *
 * Request a new page to be faulted into the UVM map of the VM owning 'vcpu'
 * at address 'gpa'.
 */
int
vmx_fault_page(struct vcpu *vcpu, paddr_t gpa)
{
	int fault_type, ret;

	fault_type = vmx_get_guest_faulttype();
	if (fault_type == -1) {
		printf("vmx_fault_page: invalid fault type\n");
		return (EINVAL);
	}

	ret = uvm_fault(vcpu->vc_parent->vm_map, gpa, fault_type,
	    PROT_READ | PROT_WRITE | PROT_EXEC);
	if (ret)
		printf("vmx_fault_page: uvm_fault returns %d\n", ret);

	return (ret);
}

/*
 * vmx_handle_np_fault
 *
 * High level nested paging handler for VMX. Verifies that a fault is for a
 * valid memory region, then faults a page, or aborts otherwise.
 */
int
vmx_handle_np_fault(struct vcpu *vcpu)
{
	uint64_t gpa;
	int gpa_memtype, ret;

	ret = 0;
	if (vmread(VMCS_GUEST_PHYSICAL_ADDRESS, &gpa)) {
		printf("vmm_handle_np_fault: cannot extract faulting pa\n");
		return (EINVAL);
	}

	gpa_memtype = vmm_get_guest_memtype(vcpu->vc_parent, gpa);
	switch (gpa_memtype) {
	case VMM_MEM_TYPE_REGULAR:
		ret = vmx_fault_page(vcpu, gpa);
		break;
	default:
		printf("unknown memory type %d for GPA 0x%llx\n",
		    gpa_memtype, gpa);
		return (EINVAL);
	}

	return (ret);
}

/*
 * vmx_handle_inout
 *
 * Exit handler for IN/OUT instructions.
 *
 * The vmm can handle certain IN/OUTS without exiting to vmd, but most of these
 * will be passed to vmd for completion.
 */
int
vmx_handle_inout(struct vcpu *vcpu)
{
	uint64_t insn_length, exit_qual;
	int ret;

	if (vmread(VMCS_INSTRUCTION_LENGTH, &insn_length)) {
		printf("vmx_handle_inout: can't obtain instruction length\n");
		return (EINVAL);
	}

	if (vmx_get_exit_qualification(&exit_qual)) {
		printf("vmx_handle_inout: can't get exit qual\n");
		return (EINVAL);
	}

	/* Bits 0:2 - size of exit */
	vcpu->vc_exit.vei.vei_size = (exit_qual & 0x7) + 1;
	/* Bit 3 - direction */
	vcpu->vc_exit.vei.vei_dir = (exit_qual & 0x8) >> 3;
	/* Bit 4 - string instruction? */
	vcpu->vc_exit.vei.vei_string = (exit_qual & 0x10) >> 4;
	/* Bit 5 - REP prefix? */
	vcpu->vc_exit.vei.vei_rep = (exit_qual & 0x20) >> 5;
	/* Bit 6 - Operand encoding */
	vcpu->vc_exit.vei.vei_encoding = (exit_qual & 0x40) >> 6;
	/* Bit 16:31 - port */
	vcpu->vc_exit.vei.vei_port = (exit_qual & 0xFFFF0000) >> 16;
	/* Data */
	vcpu->vc_exit.vei.vei_data = (uint32_t)vcpu->vc_gueststate.vg_rax;

	vcpu->vc_gueststate.vg_rip += insn_length;

	/*
	 * The following ports usually belong to devices owned by vmd.
	 * Return EAGAIN to signal help needed from userspace (vmd).
	 * Return 0 to indicate we don't care about this port.
	 *
	 * XXX something better than a hardcoded list here, maybe
	 * configure via vmd via the device list in vm create params?
	 *
	 * XXX handle not eax target
	 */
	switch (vcpu->vc_exit.vei.vei_port) {
	case 0x40 ... 0x43:
	case 0x3f8 ... 0x3ff:
	case 0xcf8:
	case 0xcfc:
	case VMM_PCI_IO_BAR_BASE ... VMM_PCI_IO_BAR_END:
		ret = EAGAIN;
		break;
	case IO_RTC ... IO_RTC + 1:
		/* We can directly read the RTC on behalf of the guest */
		if (vcpu->vc_exit.vei.vei_dir == 1) {
			vcpu->vc_gueststate.vg_rax =
			    inb(vcpu->vc_exit.vei.vei_port);
		}
		ret = 0;
		break;
	default:
		/* Read from unsupported ports returns FFs */
		if (vcpu->vc_exit.vei.vei_dir == 1)
			vcpu->vc_gueststate.vg_rax = 0xFFFFFFFF;
		ret = 0;
	}

	return (ret);
}

/*
 * vmx_handle_cr
 *
 * Handle reads/writes to control registers (except CR3)
 */
int
vmx_handle_cr(struct vcpu *vcpu)
{
	uint64_t insn_length, exit_qual;
	uint8_t crnum, dir;

	if (vmread(VMCS_INSTRUCTION_LENGTH, &insn_length)) {
		printf("vmx_handle_cr: can't obtain instruction length\n");
		return (EINVAL);
	}

	if (vmx_get_exit_qualification(&exit_qual)) {
		printf("vmx_handle_cr: can't get exit qual\n");
		return (EINVAL);
	}

	/* Low 4 bits of exit_qual represent the CR number */
	crnum = exit_qual & 0xf;

	dir = (exit_qual & 0x30) >> 4;

	switch (dir) {
	case CR_WRITE:
		DPRINTF("vmx_handle_cr: mov to cr%d @ %llx\n",
	    	    crnum, vcpu->vc_gueststate.vg_rip);
		break;
	case CR_READ:
		DPRINTF("vmx_handle_cr: mov from cr%d @ %llx\n",
		    crnum, vcpu->vc_gueststate.vg_rip);
		break;
	case CR_CLTS:
		DPRINTF("vmx_handle_cr: clts instruction @ %llx\n",
		    vcpu->vc_gueststate.vg_rip);
		break;
	case CR_LMSW:
		DPRINTF("vmx_handle_cr: lmsw instruction @ %llx\n",
		    vcpu->vc_gueststate.vg_rip);
		break;
	default:
		DPRINTF("vmx_handle_cr: unknown cr access @ %llx\n",
		    vcpu->vc_gueststate.vg_rip);
	}

	vcpu->vc_gueststate.vg_rip += insn_length;

	return (0);
}

/*
 * vmx_handle_cpuid
 *
 * Exit handler for CPUID instruction
 */
int
vmx_handle_cpuid(struct vcpu *vcpu)
{
	uint64_t insn_length;
	uint64_t *rax, *rbx, *rcx, *rdx;

	if (vmread(VMCS_INSTRUCTION_LENGTH, &insn_length)) {
		printf("vmx_handle_cpuid: can't obtain instruction length\n");
		return (EINVAL);
	}

	/* All CPUID instructions are 0x0F 0xA2 */
	KASSERT(insn_length == 2);

	rax = &vcpu->vc_gueststate.vg_rax;
	rbx = &vcpu->vc_gueststate.vg_rbx;
	rcx = &vcpu->vc_gueststate.vg_rcx;
	rdx = &vcpu->vc_gueststate.vg_rdx;

	switch (*rax) {
	case 0x00:	/* Max level and vendor ID */
		*rax = 0x07; /* cpuid_level */
		*rbx = *((uint32_t *)&cpu_vendor);
		*rcx = *((uint32_t *)&cpu_vendor + 1);
		*rdx = *((uint32_t *)&cpu_vendor + 2);
		break;
	case 0x01:	/* Version, brand, feature info */
		*rax = cpu_id;
		/* mask off host's APIC ID, reset to vcpu id */
		*rbx = cpu_ebxfeature & 0x00FFFFFF;
		*rbx &= (vcpu->vc_id & 0xFF) << 24;
		/*
		 * clone host capabilities minus:
		 *  speedstep (CPUIDECX_EST)
		 *  vmx (CPUIDECX_VMX)
		 *  xsave (CPUIDECX_XSAVE)
		 *  thermal (CPUIDECX_TM2, CPUID_ACPI, CPUID_TM)
		 *  XXX - timestamp (CPUID_TSC)
		 *  monitor/mwait (CPUIDECX_MWAIT)
		 *  performance monitoring (CPUIDECX_PDCM)
		 * plus:
		 *  hypervisor (CPUIDECX_HV)
		 */
		*rcx = (cpu_ecxfeature | CPUIDECX_HV) &
		    ~(CPUIDECX_EST | CPUIDECX_TM2 |
		    CPUIDECX_MWAIT | CPUIDECX_PDCM |
		    CPUIDECX_VMX | CPUIDECX_XSAVE);
		*rdx = curcpu()->ci_feature_flags &
		    ~(CPUID_ACPI | CPUID_TM | CPUID_TSC);
		break;
	case 0x02:	/* Cache and TLB information */
		DPRINTF("vmx_handle_cpuid: function 0x02 (cache/TLB) not"
		    " supported\n");
		break;
	case 0x03:	/* Processor serial number (not supported) */
		*rax = 0;
		*rbx = 0;
		*rcx = 0;
		*rdx = 0;
		break;
	case 0x04:
		DPRINTF("vmx_handle_cpuid: function 0x04 (deterministic "
		    "cache info) not supported\n");
		break;
	case 0x05:	/* MONITOR/MWAIT (not supported) */
		*rax = 0;
		*rbx = 0;
		*rcx = 0;
		*rdx = 0;
		break;
	case 0x06:	/* Thermal / Power management */
		/* Only ARAT is exposed in function 0x06 */
		*rax = TPM_ARAT;
		*rbx = 0;
		*rcx = 0;
		*rdx = 0;
		break;
	case 0x07:	/* SEFF */
		if (*rcx == 0) {
			*rax = 0;	/* Highest subleaf supported */
			*rbx = curcpu()->ci_feature_sefflags_ebx;
			*rcx = curcpu()->ci_feature_sefflags_ecx;
			*rdx = 0;
		} else {
			/* Unsupported subleaf */
			*rax = 0;
			*rbx = 0;
			*rcx = 0;
			*rdx = 0;
		}
		break;
	case 0x09:	/* Direct Cache Access (not supported) */
		DPRINTF("vmx_handle_cpuid: function 0x09 (direct cache access)"
		    " not supported\n");
		break;
	case 0x0a:	/* Architectural performance monitoring */
		*rax = 0;
		*rbx = 0;
		*rcx = 0;
		*rdx = 0;
		break;
	case 0x0b:	/* Extended topology enumeration (not supported) */
		DPRINTF("vmx_handle_cpuid: function 0x0b (topology enumeration)"
		    " not supported\n");
		break;
	case 0x0d:	/* Processor ext. state information (not supported) */
		DPRINTF("vmx_handle_cpuid: function 0x0d (ext. state info)"
		    " not supported\n");
		break;
	case 0x0f:	/* QoS info (not supported) */
		DPRINTF("vmx_handle_cpuid: function 0x0f (QoS info)"
		    " not supported\n");
		break;
	case 0x14:	/* Processor Trace info (not supported) */
		DPRINTF("vmx_handle_cpuid: function 0x14 (processor trace info)"
		    " not supported\n");
		break;
	case 0x15:	/* TSC / Core Crystal Clock info (not supported) */
		DPRINTF("vmx_handle_cpuid: function 0x15 (TSC / CCC info)"
		    " not supported\n");
		break;
	case 0x16:	/* Processor frequency info (not supported) */
		DPRINTF("vmx_handle_cpuid: function 0x16 (frequency info)"
		    " not supported\n");
		break;
	case 0x40000000:	/* Hypervisor information */
		*rax = 0;
		*rbx = *((uint32_t *)&vmm_hv_signature[0]);
		*rcx = *((uint32_t *)&vmm_hv_signature[4]);
		*rdx = *((uint32_t *)&vmm_hv_signature[8]);
		break;
	case 0x80000000:	/* Extended function level */
		*rax = 0x80000007; /* curcpu()->ci_pnfeatset */
		*rbx = 0;
		*rcx = 0;
		*rdx = 0;
	case 0x80000001: 	/* Extended function info */
		*rax = curcpu()->ci_efeature_eax;
		*rbx = 0;	/* Reserved */
		*rcx = curcpu()->ci_efeature_ecx;
		*rdx = curcpu()->ci_feature_eflags;
		break;
	case 0x80000002:	/* Brand string */
		*rax = curcpu()->ci_brand[0];
		*rbx = curcpu()->ci_brand[1];
		*rcx = curcpu()->ci_brand[2];
		*rdx = curcpu()->ci_brand[3];
		break;
	case 0x80000003:	/* Brand string */
		*rax = curcpu()->ci_brand[4];
		*rbx = curcpu()->ci_brand[5];
		*rcx = curcpu()->ci_brand[6];
		*rdx = curcpu()->ci_brand[7];
		break;
	case 0x80000004:	/* Brand string */
		*rax = curcpu()->ci_brand[8];
		*rbx = curcpu()->ci_brand[9];
		*rcx = curcpu()->ci_brand[10];
		*rdx = curcpu()->ci_brand[11];
		break;
	case 0x80000005:	/* Reserved (Intel), cacheinfo (AMD) */
		*rax = curcpu()->ci_amdcacheinfo[0];
		*rbx = curcpu()->ci_amdcacheinfo[1];
		*rcx = curcpu()->ci_amdcacheinfo[2];
		*rdx = curcpu()->ci_amdcacheinfo[3];
		break;
	case 0x80000006:	/* ext. cache info */
		*rax = curcpu()->ci_extcacheinfo[0];
		*rbx = curcpu()->ci_extcacheinfo[1];
		*rcx = curcpu()->ci_extcacheinfo[2];
		*rdx = curcpu()->ci_extcacheinfo[3];
		break;
	case 0x80000007:	/* apmi */
		*rax = 0;	/* Reserved */
		*rbx = 0;	/* Reserved */
		*rcx = 0;	/* Reserved */
		*rdx = cpu_apmi_edx;
		break;
	case 0x80000008:	/* Phys bits info and topology (AMD) */
		DPRINTF("vmx_handle_cpuid: function 0x80000008 (phys bits info)"
		    " not supported\n");
		break;
	default:
		DPRINTF("vmx_handle_cpuid: unsupported rax=0x%llx\n", *rax);
	}

	vcpu->vc_gueststate.vg_rip += insn_length;

	return (0);
}

/*
 * vcpu_run_svm
 *
 * VMM main loop used to run a VCPU.
 */
int
vcpu_run_svm(struct vcpu *vcpu, uint8_t from_exit)
{
	/* XXX removed due to rot */
	return (0);
}

/*
 * vmx_exit_reason_decode
 *
 * Returns a human readable string describing exit type 'code'
 */
const char *
vmx_exit_reason_decode(uint32_t code)
{
	switch (code) {
	case VMX_EXIT_NMI: return "NMI";
	case VMX_EXIT_EXTINT: return "external interrupt";
	case VMX_EXIT_TRIPLE_FAULT: return "triple fault";
	case VMX_EXIT_INIT: return "INIT signal";
	case VMX_EXIT_SIPI: return "SIPI signal";
	case VMX_EXIT_IO_SMI: return "I/O SMI";
	case VMX_EXIT_OTHER_SMI: return "other SMI";
	case VMX_EXIT_INT_WINDOW: return "interrupt window";
	case VMX_EXIT_NMI_WINDOW: return "NMI window";
	case VMX_EXIT_TASK_SWITCH: return "task switch";
	case VMX_EXIT_CPUID: return "CPUID instruction";
	case VMX_EXIT_GETSEC: return "GETSEC instruction";
	case VMX_EXIT_HLT: return "HLT instruction";
	case VMX_EXIT_INVD: return "INVD instruction";
	case VMX_EXIT_INVLPG: return "INVLPG instruction";
	case VMX_EXIT_RDPMC: return "RDPMC instruction";
	case VMX_EXIT_RDTSC: return "RDTSC instruction";
	case VMX_EXIT_RSM: return "RSM instruction";
	case VMX_EXIT_VMCALL: return "VMCALL instruction";
	case VMX_EXIT_VMCLEAR: return "VMCLEAR instruction";
	case VMX_EXIT_VMLAUNCH: return "VMLAUNCH instruction";
	case VMX_EXIT_VMPTRLD: return "VMPTRLD instruction";
	case VMX_EXIT_VMPTRST: return "VMPTRST instruction";
	case VMX_EXIT_VMREAD: return "VMREAD instruction";
	case VMX_EXIT_VMRESUME: return "VMRESUME instruction";
	case VMX_EXIT_VMWRITE: return "VMWRITE instruction";
	case VMX_EXIT_VMXOFF: return "VMXOFF instruction";
	case VMX_EXIT_VMXON: return "VMXON instruction";
	case VMX_EXIT_CR_ACCESS: return "CR access";
	case VMX_EXIT_MOV_DR: return "MOV DR instruction";
	case VMX_EXIT_IO: return "I/O instruction";
	case VMX_EXIT_RDMSR: return "RDMSR instruction";
	case VMX_EXIT_WRMSR: return "WRMSR instruction";
	case VMX_EXIT_ENTRY_FAILED_GUEST_STATE: return "guest state invalid";
	case VMX_EXIT_ENTRY_FAILED_MSR_LOAD: return "MSR load failed";
	case VMX_EXIT_MWAIT: return "MWAIT instruction";
	case VMX_EXIT_MTF: return "monitor trap flag";
	case VMX_EXIT_MONITOR: return "MONITOR instruction";
	case VMX_EXIT_PAUSE: return "PAUSE instruction";
	case VMX_EXIT_ENTRY_FAILED_MCE: return "MCE during entry";
	case VMX_EXIT_TPR_BELOW_THRESHOLD: return "TPR below threshold";
	case VMX_EXIT_APIC_ACCESS: return "APIC access";
	case VMX_EXIT_VIRTUALIZED_EOI: return "virtualized EOI";
	case VMX_EXIT_GDTR_IDTR: return "GDTR/IDTR access";
	case VMX_EXIT_LDTR_TR: return "LDTR/TR access";
	case VMX_EXIT_EPT_VIOLATION: return "EPT violation";
	case VMX_EXIT_EPT_MISCONFIGURATION: return "EPT misconfiguration";
	case VMX_EXIT_INVEPT: return "INVEPT instruction";
	case VMX_EXIT_RDTSCP: return "RDTSCP instruction";
	case VMX_EXIT_VMX_PREEMPTION_TIMER_EXPIRED:
	    return "preemption timer expired";
	case VMX_EXIT_INVVPID: return "INVVPID instruction";
	case VMX_EXIT_WBINVD: return "WBINVD instruction";
	case VMX_EXIT_XSETBV: return "XSETBV instruction";
	case VMX_EXIT_APIC_WRITE: return "APIC write";
	case VMX_EXIT_RDRAND: return "RDRAND instruction";
	case VMX_EXIT_INVPCID: return "INVPCID instruction";
	case VMX_EXIT_VMFUNC: return "VMFUNC instruction";
	case VMX_EXIT_RDSEED: return "RDSEED instruction";
	case VMX_EXIT_XSAVES: return "XSAVES instruction";
	case VMX_EXIT_XRSTORS: return "XRSTORS instruction";
	default: return "unknown";
	}
}

/*
 * vmx_instruction_error_decode
 *
 * Returns a human readable string describing the instruction error in 'code'
 */
const char *
vmx_instruction_error_decode(uint32_t code)
{
	switch (code) {
	case 1: return "VMCALL: unsupported in VMX root";
	case 2: return "VMCLEAR: invalid paddr";
	case 3: return "VMCLEAR: VMXON pointer";
	case 4: return "VMLAUNCH: non-clear VMCS";
	case 5: return "VMRESUME: non-launched VMCS";
	case 6: return "VMRESUME: executed after VMXOFF";
	case 7: return "VM entry: invalid control field(s)";
	case 8: return "VM entry: invalid host state field(s)";
	case 9: return "VMPTRLD: invalid paddr";
	case 10: return "VMPTRLD: VMXON pointer";
	case 11: return "VMPTRLD: incorrect VMCS revid";
	case 12: return "VMREAD/VMWRITE: unsupported VMCS field";
	case 13: return "VMWRITE: RO VMCS field";
	case 15: return "VMXON: unsupported in VMX root";
	case 20: return "VMCALL: invalid VM exit control fields";
	case 26: return "VM entry: blocked by MOV SS";
	case 28: return "Invalid operand to INVEPT/INVVPID";
	default: return "unknown";
	}
}

/*
 * vcpu_state_decode
 *
 * Returns a human readable string describing the vcpu state in 'state'.
 */
const char *
vcpu_state_decode(u_int state)
{
	switch (state) {
	case VCPU_STATE_STOPPED: return "stopped";
	case VCPU_STATE_RUNNING: return "running";
	case VCPU_STATE_REQTERM: return "requesting termination";
	case VCPU_STATE_TERMINATED: return "terminated";
	case VCPU_STATE_UNKNOWN: return "unknown";
	default: return "invalid";
	}
}

#ifdef VMM_DEBUG
/*
 * dump_vcpu
 *
 * Dumps the VMX capabilites of vcpu 'vcpu'
 */
void
dump_vcpu(struct vcpu *vcpu)
{
	printf("vcpu @ 0x%llx\n", (uint64_t)vcpu);
	printf("    parent vm @ 0x%llx\n", (uint64_t)vcpu->vc_parent);
	printf("    mode: ");
	if (vcpu->vc_virt_mode == VMM_MODE_VMX ||
	    vcpu->vc_virt_mode == VMM_MODE_EPT) {
		printf("VMX\n");
		printf("    pinbased ctls: 0x%llx\n",
		    vcpu->vc_vmx_pinbased_ctls);
		printf("    true pinbased ctls: 0x%llx\n",
		    vcpu->vc_vmx_true_pinbased_ctls);
		CTRL_DUMP(vcpu, PINBASED, EXTERNAL_INT_EXITING);
		CTRL_DUMP(vcpu, PINBASED, NMI_EXITING);
		CTRL_DUMP(vcpu, PINBASED, VIRTUAL_NMIS);
		CTRL_DUMP(vcpu, PINBASED, ACTIVATE_VMX_PREEMPTION_TIMER);
		CTRL_DUMP(vcpu, PINBASED, PROCESS_POSTED_INTERRUPTS);
		printf("    procbased ctls: 0x%llx\n",
		    vcpu->vc_vmx_procbased_ctls);
		printf("    true procbased ctls: 0x%llx\n",
		    vcpu->vc_vmx_true_procbased_ctls);
		CTRL_DUMP(vcpu, PROCBASED, INTERRUPT_WINDOW_EXITING);
		CTRL_DUMP(vcpu, PROCBASED, USE_TSC_OFFSETTING);
		CTRL_DUMP(vcpu, PROCBASED, HLT_EXITING);
		CTRL_DUMP(vcpu, PROCBASED, INVLPG_EXITING);
		CTRL_DUMP(vcpu, PROCBASED, MWAIT_EXITING);
		CTRL_DUMP(vcpu, PROCBASED, RDPMC_EXITING);
		CTRL_DUMP(vcpu, PROCBASED, RDTSC_EXITING);
		CTRL_DUMP(vcpu, PROCBASED, CR3_LOAD_EXITING);
		CTRL_DUMP(vcpu, PROCBASED, CR3_STORE_EXITING);
		CTRL_DUMP(vcpu, PROCBASED, CR8_LOAD_EXITING);
		CTRL_DUMP(vcpu, PROCBASED, CR8_STORE_EXITING);
		CTRL_DUMP(vcpu, PROCBASED, USE_TPR_SHADOW);
		CTRL_DUMP(vcpu, PROCBASED, NMI_WINDOW_EXITING);
		CTRL_DUMP(vcpu, PROCBASED, MOV_DR_EXITING);
		CTRL_DUMP(vcpu, PROCBASED, UNCONDITIONAL_IO_EXITING);
		CTRL_DUMP(vcpu, PROCBASED, USE_IO_BITMAPS);
		if (vcpu_vmx_check_cap(vcpu, IA32_VMX_PROCBASED_CTLS,
		    IA32_VMX_ACTIVATE_SECONDARY_CONTROLS, 1)) {
			printf("    procbased2 ctls: 0x%llx\n",
			    vcpu->vc_vmx_procbased2_ctls);
			CTRL_DUMP(vcpu, PROCBASED2, VIRTUALIZE_APIC);
			CTRL_DUMP(vcpu, PROCBASED2, ENABLE_EPT);
			CTRL_DUMP(vcpu, PROCBASED2, DESCRIPTOR_TABLE_EXITING);
			CTRL_DUMP(vcpu, PROCBASED2, ENABLE_RDTSCP);
			CTRL_DUMP(vcpu, PROCBASED2, VIRTUALIZE_X2APIC_MODE);
			CTRL_DUMP(vcpu, PROCBASED2, ENABLE_VPID);
			CTRL_DUMP(vcpu, PROCBASED2, WBINVD_EXITING);
			CTRL_DUMP(vcpu, PROCBASED2, UNRESTRICTED_GUEST);
			CTRL_DUMP(vcpu, PROCBASED2,
			    APIC_REGISTER_VIRTUALIZATION);
			CTRL_DUMP(vcpu, PROCBASED2,
			    VIRTUAL_INTERRUPT_DELIVERY);
			CTRL_DUMP(vcpu, PROCBASED2, PAUSE_LOOP_EXITING);
			CTRL_DUMP(vcpu, PROCBASED2, RDRAND_EXITING);
			CTRL_DUMP(vcpu, PROCBASED2, ENABLE_INVPCID);
			CTRL_DUMP(vcpu, PROCBASED2, ENABLE_VM_FUNCTIONS);
			CTRL_DUMP(vcpu, PROCBASED2, VMCS_SHADOWING);
			CTRL_DUMP(vcpu, PROCBASED2, EPT_VIOLATION_VE);
		}
		printf("    entry ctls: 0x%llx\n",
		    vcpu->vc_vmx_entry_ctls);
		printf("    true entry ctls: 0x%llx\n",
		    vcpu->vc_vmx_true_procbased_ctls);
		CTRL_DUMP(vcpu, ENTRY, LOAD_DEBUG_CONTROLS);
		CTRL_DUMP(vcpu, ENTRY, IA32E_MODE_GUEST);
		CTRL_DUMP(vcpu, ENTRY, ENTRY_TO_SMM);
		CTRL_DUMP(vcpu, ENTRY, DEACTIVATE_DUAL_MONITOR_TREATMENT);
		CTRL_DUMP(vcpu, ENTRY, LOAD_IA32_PERF_GLOBAL_CTRL_ON_ENTRY);
		CTRL_DUMP(vcpu, ENTRY, LOAD_IA32_PAT_ON_ENTRY);
		CTRL_DUMP(vcpu, ENTRY, LOAD_IA32_EFER_ON_ENTRY);
		printf("    exit ctls: 0x%llx\n",
		    vcpu->vc_vmx_exit_ctls);
		printf("    true exit ctls: 0x%llx\n",
		    vcpu->vc_vmx_true_exit_ctls);
		CTRL_DUMP(vcpu, EXIT, SAVE_DEBUG_CONTROLS);
		CTRL_DUMP(vcpu, EXIT, HOST_SPACE_ADDRESS_SIZE);
		CTRL_DUMP(vcpu, EXIT, LOAD_IA32_PERF_GLOBAL_CTRL_ON_EXIT);
		CTRL_DUMP(vcpu, EXIT, ACKNOWLEDGE_INTERRUPT_ON_EXIT);
		CTRL_DUMP(vcpu, EXIT, SAVE_IA32_PAT_ON_EXIT);
		CTRL_DUMP(vcpu, EXIT, LOAD_IA32_PAT_ON_EXIT);
		CTRL_DUMP(vcpu, EXIT, SAVE_IA32_EFER_ON_EXIT);
		CTRL_DUMP(vcpu, EXIT, LOAD_IA32_EFER_ON_EXIT);
		CTRL_DUMP(vcpu, EXIT, SAVE_VMX_PREEMPTION_TIMER);
	}
}

/*
 * vmx_vcpu_dump_regs
 *
 * Debug function to print vcpu regs from the current vcpu
 *  note - vmcs for 'vcpu' must be on this pcpu.
 *
 * Parameters:
 *  vcpu - vcpu whose registers should be dumped
 */
void
vmx_vcpu_dump_regs(struct vcpu *vcpu)
{
	uint64_t r;
	int i;
	struct vmx_msr_store *msr_store;

	/* XXX reformat this for 32 bit guest as needed */
	DPRINTF("vcpu @ %p\n", vcpu);
	DPRINTF(" rax=0x%016llx rbx=0x%016llx rcx=0x%016llx\n",
	    vcpu->vc_gueststate.vg_rax, vcpu->vc_gueststate.vg_rbx,
	    vcpu->vc_gueststate.vg_rcx);
	DPRINTF(" rdx=0x%016llx rbp=0x%016llx rdi=0x%016llx\n",
	    vcpu->vc_gueststate.vg_rdx, vcpu->vc_gueststate.vg_rbp,
	    vcpu->vc_gueststate.vg_rdi);
	DPRINTF(" rsi=0x%016llx  r8=0x%016llx  r9=0x%016llx\n",
	    vcpu->vc_gueststate.vg_rsi, vcpu->vc_gueststate.vg_r8,
	    vcpu->vc_gueststate.vg_r9);
	DPRINTF(" r10=0x%016llx r11=0x%016llx r12=0x%016llx\n",
	    vcpu->vc_gueststate.vg_r10, vcpu->vc_gueststate.vg_r11,
	    vcpu->vc_gueststate.vg_r12);
	DPRINTF(" r13=0x%016llx r14=0x%016llx r15=0x%016llx\n",
	    vcpu->vc_gueststate.vg_r13, vcpu->vc_gueststate.vg_r14,
	    vcpu->vc_gueststate.vg_r15);

	DPRINTF(" rip=0x%016llx rsp=", vcpu->vc_gueststate.vg_rip);
	if (vmread(VMCS_GUEST_IA32_RSP, &r))
		DPRINTF("(error reading)\n");
	else
		DPRINTF("0x%016llx\n", r);

	DPRINTF(" cr0=");
	if (vmread(VMCS_GUEST_IA32_CR0, &r))
		DPRINTF("(error reading)\n");
	else {
		DPRINTF("0x%016llx ", r);
		vmm_decode_cr0(r);
	}

	DPRINTF(" cr2=0x%016llx\n", vcpu->vc_gueststate.vg_cr2);

	DPRINTF(" cr3=");
	if (vmread(VMCS_GUEST_IA32_CR3, &r))
		DPRINTF("(error reading)\n");
	else {
		DPRINTF("0x%016llx ", r);
		vmm_decode_cr3(r);
	}

	DPRINTF(" cr4=");
	if (vmread(VMCS_GUEST_IA32_CR4, &r))
		DPRINTF("(error reading)\n");
	else {
		DPRINTF("0x%016llx ", r);
		vmm_decode_cr4(r);
	}

	DPRINTF(" --Guest Segment Info--\n");

	DPRINTF(" cs=");
	if (vmread(VMCS_GUEST_IA32_CS_SEL, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%04llx rpl=%lld", r, r & 0x3);

	DPRINTF(" base=");
	if (vmread(VMCS_GUEST_IA32_CS_BASE, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" limit=");
	if (vmread(VMCS_GUEST_IA32_CS_LIMIT, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" a/r=");
	if (vmread(VMCS_GUEST_IA32_CS_AR, &r))
		DPRINTF("(error reading)\n");
	else {
		DPRINTF("0x%04llx\n  ", r);
		vmm_segment_desc_decode(r);
	}	

	DPRINTF(" ds=");
	if (vmread(VMCS_GUEST_IA32_DS_SEL, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%04llx rpl=%lld", r, r & 0x3);

	DPRINTF(" base=");
	if (vmread(VMCS_GUEST_IA32_DS_BASE, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" limit=");
	if (vmread(VMCS_GUEST_IA32_DS_LIMIT, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" a/r=");
	if (vmread(VMCS_GUEST_IA32_DS_AR, &r))
		DPRINTF("(error reading)\n");
	else {
		DPRINTF("0x%04llx\n  ", r);
		vmm_segment_desc_decode(r);
	}	

	DPRINTF(" es=");
	if (vmread(VMCS_GUEST_IA32_ES_SEL, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%04llx rpl=%lld", r, r & 0x3);

	DPRINTF(" base=");
	if (vmread(VMCS_GUEST_IA32_ES_BASE, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" limit=");
	if (vmread(VMCS_GUEST_IA32_ES_LIMIT, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" a/r=");
	if (vmread(VMCS_GUEST_IA32_ES_AR, &r))
		DPRINTF("(error reading)\n");
	else {
		DPRINTF("0x%04llx\n  ", r);
		vmm_segment_desc_decode(r);
	}	

	DPRINTF(" fs=");
	if (vmread(VMCS_GUEST_IA32_FS_SEL, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%04llx rpl=%lld", r, r & 0x3);

	DPRINTF(" base=");
	if (vmread(VMCS_GUEST_IA32_FS_BASE, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" limit=");
	if (vmread(VMCS_GUEST_IA32_FS_LIMIT, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" a/r=");
	if (vmread(VMCS_GUEST_IA32_FS_AR, &r))
		DPRINTF("(error reading)\n");
	else {
		DPRINTF("0x%04llx\n  ", r);
		vmm_segment_desc_decode(r);
	}

	DPRINTF(" gs=");
	if (vmread(VMCS_GUEST_IA32_GS_SEL, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%04llx rpl=%lld", r, r & 0x3);

	DPRINTF(" base=");
	if (vmread(VMCS_GUEST_IA32_GS_BASE, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" limit=");
	if (vmread(VMCS_GUEST_IA32_GS_LIMIT, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" a/r=");
	if (vmread(VMCS_GUEST_IA32_GS_AR, &r))
		DPRINTF("(error reading)\n");
	else {
		DPRINTF("0x%04llx\n  ", r);
		vmm_segment_desc_decode(r);
	}	

	DPRINTF(" ss=");
	if (vmread(VMCS_GUEST_IA32_SS_SEL, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%04llx rpl=%lld", r, r & 0x3);

	DPRINTF(" base=");
	if (vmread(VMCS_GUEST_IA32_SS_BASE, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" limit=");
	if (vmread(VMCS_GUEST_IA32_SS_LIMIT, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" a/r=");
	if (vmread(VMCS_GUEST_IA32_SS_AR, &r))
		DPRINTF("(error reading)\n");
	else {
		DPRINTF("0x%04llx\n  ", r);
		vmm_segment_desc_decode(r);
	}	

	DPRINTF(" tr=");
	if (vmread(VMCS_GUEST_IA32_TR_SEL, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%04llx", r);

	DPRINTF(" base=");
	if (vmread(VMCS_GUEST_IA32_TR_BASE, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" limit=");
	if (vmread(VMCS_GUEST_IA32_TR_LIMIT, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" a/r=");
	if (vmread(VMCS_GUEST_IA32_TR_AR, &r))
		DPRINTF("(error reading)\n");
	else {
		DPRINTF("0x%04llx\n  ", r);
		vmm_segment_desc_decode(r);
	}	
		
	DPRINTF(" gdtr base=");
	if (vmread(VMCS_GUEST_IA32_GDTR_BASE, &r))
		DPRINTF("(error reading)   ");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" limit=");
	if (vmread(VMCS_GUEST_IA32_GDTR_LIMIT, &r))
		DPRINTF("(error reading)\n");
	else
		DPRINTF("0x%016llx\n", r);

	DPRINTF(" idtr base=");
	if (vmread(VMCS_GUEST_IA32_IDTR_BASE, &r))
		DPRINTF("(error reading)   ");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" limit=");
	if (vmread(VMCS_GUEST_IA32_IDTR_LIMIT, &r))
		DPRINTF("(error reading)\n");
	else
		DPRINTF("0x%016llx\n", r);
	
	DPRINTF(" ldtr=");
	if (vmread(VMCS_GUEST_IA32_LDTR_SEL, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%04llx", r);

	DPRINTF(" base=");
	if (vmread(VMCS_GUEST_IA32_LDTR_BASE, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" limit=");
	if (vmread(VMCS_GUEST_IA32_LDTR_LIMIT, &r))
		DPRINTF("(error reading)");
	else
		DPRINTF("0x%016llx", r);

	DPRINTF(" a/r=");
	if (vmread(VMCS_GUEST_IA32_LDTR_AR, &r))
		DPRINTF("(error reading)\n");
	else {
		DPRINTF("0x%04llx\n  ", r);
		vmm_segment_desc_decode(r);
	}	

	DPRINTF(" --Guest MSRs @ 0x%016llx (paddr: 0x%016llx)--\n",
	    (uint64_t)vcpu->vc_vmx_msr_exit_save_va,
	    (uint64_t)vcpu->vc_vmx_msr_exit_save_pa);

	msr_store = (struct vmx_msr_store *)vcpu->vc_vmx_msr_exit_save_va;

	for (i = 0; i < VMX_NUM_MSR_STORE; i++) {
		DPRINTF("  MSR %d @ %p : 0x%08x (%s), "
		    "value=0x%016llx ",
		    i, &msr_store[i], msr_store[i].vms_index,
		    msr_name_decode(msr_store[i].vms_index),
		    msr_store[i].vms_data); 
		vmm_decode_msr_value(msr_store[i].vms_index,
		    msr_store[i].vms_data);
	}

	DPRINTF(" last PIC irq=%d\n", vcpu->vc_intr);
}

/*
 * msr_name_decode
 *
 * Returns a human-readable name for the MSR supplied in 'msr'.
 *
 * Parameters:
 *  msr - The MSR to decode
 *
 * Return value:
 *  NULL-terminated character string containing the name of the MSR requested
 */
const char *
msr_name_decode(uint32_t msr)
{
	/*
	 * Add as needed. Also consider adding a decode function when
	 * adding to this table.
	 */

	switch (msr) {
	case MSR_TSC: return "TSC";
	case MSR_APICBASE: return "APIC base";
	case MSR_IA32_FEATURE_CONTROL: return "IA32 feature control";
	case MSR_PERFCTR0: return "perf counter 0";
	case MSR_PERFCTR1: return "perf counter 1";
	case MSR_TEMPERATURE_TARGET: return "temperature target";
	case MSR_MTRRcap: return "MTRR cap";
	case MSR_PERF_STATUS: return "perf status";
	case MSR_PERF_CTL: return "perf control";
	case MSR_MTRRvarBase: return "MTRR variable base";
	case MSR_MTRRfix64K_00000: return "MTRR fixed 64K";
	case MSR_MTRRfix16K_80000: return "MTRR fixed 16K";
	case MSR_MTRRfix4K_C0000: return "MTRR fixed 4K";
	case MSR_CR_PAT: return "PAT";
	case MSR_MTRRdefType: return "MTRR default type";
	case MSR_EFER: return "EFER";
	case MSR_STAR: return "STAR";
	case MSR_LSTAR: return "LSTAR";
	case MSR_CSTAR: return "CSTAR";
	case MSR_SFMASK: return "SFMASK";
	case MSR_FSBASE: return "FSBASE";
	case MSR_GSBASE: return "GSBASE";
	case MSR_KERNELGSBASE: return "KGSBASE";
	default: return "Unknown MSR";
	}
}

/*
 * vmm_segment_desc_decode
 *
 * Debug function to print segment information for supplied descriptor
 *
 * Parameters:
 *  val - The A/R bytes for the segment descriptor to decode
 */
void
vmm_segment_desc_decode(uint64_t val)
{
	uint16_t ar;
	uint8_t g, type, s, dpl, p, dib, l;
	uint32_t unusable;

	/* Exit early on unusable descriptors */
	unusable = val & 0x10000;
	if (unusable) {
		DPRINTF("(unusable)\n");
		return;
	}

	ar = (uint16_t)val;

	g = (ar & 0x8000) >> 15;
	dib = (ar & 0x4000) >> 14;
	l = (ar & 0x2000) >> 13;
	p = (ar & 0x80) >> 7;
	dpl = (ar & 0x60) >> 5;
	s = (ar & 0x10) >> 4;
	type = (ar & 0xf);

	DPRINTF("granularity=%d dib=%d l(64 bit)=%d present=%d sys=%d ",
	    g, dib, l, p, s);

	DPRINTF("type=");
	if (!s) {
		switch (type) {
		case SDT_SYSLDT: DPRINTF("ldt\n"); break;
		case SDT_SYS386TSS: DPRINTF("tss (available)\n"); break;
		case SDT_SYS386BSY: DPRINTF("tss (busy)\n"); break;
		case SDT_SYS386CGT: DPRINTF("call gate\n"); break;
		case SDT_SYS386IGT: DPRINTF("interrupt gate\n"); break;
		case SDT_SYS386TGT: DPRINTF("trap gate\n"); break;
		/* XXX handle 32 bit segment types by inspecting mode */
		default: DPRINTF("unknown");
		}
	} else {
		switch (type + 16) {
		case SDT_MEMRO: DPRINTF("data, r/o\n"); break;
		case SDT_MEMROA: DPRINTF("data, r/o, accessed\n"); break;
		case SDT_MEMRW: DPRINTF("data, r/w\n"); break;
		case SDT_MEMRWA: DPRINTF("data, r/w, accessed\n"); break;
		case SDT_MEMROD: DPRINTF("data, r/o, expand down\n"); break;
		case SDT_MEMRODA: DPRINTF("data, r/o, expand down, "
		    "accessed\n");
			break;
		case SDT_MEMRWD: DPRINTF("data, r/w, expand down\n"); break;
		case SDT_MEMRWDA: DPRINTF("data, r/w, expand down, "
		    "accessed\n");
			break;
		case SDT_MEME: DPRINTF("code, x only\n"); break;
		case SDT_MEMEA: DPRINTF("code, x only, accessed\n");
		case SDT_MEMER: DPRINTF("code, r/x\n"); break;
		case SDT_MEMERA: DPRINTF("code, r/x, accessed\n"); break;
		case SDT_MEMEC: DPRINTF("code, x only, conforming\n"); break;
		case SDT_MEMEAC: DPRINTF("code, x only, conforming, "
		    "accessed\n");
			break;
		case SDT_MEMERC: DPRINTF("code, r/x, conforming\n"); break;
		case SDT_MEMERAC: DPRINTF("code, r/x, conforming, accessed\n");
			break;
		}
	}
}

void
vmm_decode_cr0(uint64_t cr0)
{
	struct vmm_reg_debug_info cr0_info[11] = {
		{ CR0_PG, "PG ", "pg " },
		{ CR0_CD, "CD ", "cd " },
		{ CR0_NW, "NW ", "nw " },
		{ CR0_AM, "AM ", "am " },
		{ CR0_WP, "WP ", "wp " },
		{ CR0_NE, "NE ", "ne " },
		{ CR0_ET, "ET ", "et " },
		{ CR0_TS, "TS ", "ts " },
		{ CR0_EM, "EM ", "em " },
		{ CR0_MP, "MP ", "mp " },
		{ CR0_PE, "PE", "pe" }
	};

	uint8_t i;

	DPRINTF("(");
	for (i = 0; i < 11; i++)
		if (cr0 & cr0_info[i].vrdi_bit)
			DPRINTF(cr0_info[i].vrdi_present);
		else
			DPRINTF(cr0_info[i].vrdi_absent);
	
	DPRINTF(")\n");
}

void
vmm_decode_cr3(uint64_t cr3)
{
	struct vmm_reg_debug_info cr3_info[2] = {
		{ CR3_PWT, "PWT ", "pwt "},
		{ CR3_PCD, "PCD", "pcd"}
	};

	uint64_t cr4;
	uint8_t i;

	if (vmread(VMCS_GUEST_IA32_CR4, &cr4)) {
		DPRINTF("(error)\n");
		return;
	}

	/* If CR4.PCIDE = 0, interpret CR3.PWT and CR3.PCD */
	if ((cr4 & CR4_PCIDE) == 0) {
		DPRINTF("(");
		for (i = 0 ; i < 2 ; i++)
			if (cr3 & cr3_info[i].vrdi_bit)
				DPRINTF(cr3_info[i].vrdi_present);
			else
				DPRINTF(cr3_info[i].vrdi_absent);

		DPRINTF(")\n");
	} else {
		DPRINTF("(pcid=0x%llx)\n", cr3 & 0xFFF);
	}
}

void
vmm_decode_cr4(uint64_t cr4)
{
	struct vmm_reg_debug_info cr4_info[19] = {
		{ CR4_PKE, "PKE ", "pke "},
		{ CR4_SMAP, "SMAP ", "smap "},
		{ CR4_SMEP, "SMEP ", "smep "},
		{ CR4_OSXSAVE, "OSXSAVE ", "osxsave "},
		{ CR4_PCIDE, "PCIDE ", "pcide "},
		{ CR4_FSGSBASE, "FSGSBASE ", "fsgsbase "},
		{ CR4_SMXE, "SMXE ", "smxe "},
		{ CR4_VMXE, "VMXE ", "vmxe "},
		{ CR4_OSXMMEXCPT, "OSXMMEXCPT ", "osxmmexcpt "},
		{ CR4_OSFXSR, "OSFXSR ", "osfxsr "},
		{ CR4_PCE, "PCE ", "pce "},
		{ CR4_PGE, "PGE ", "pge "},
		{ CR4_MCE, "MCE ", "mce "},
		{ CR4_PAE, "PAE ", "pae "},
		{ CR4_PSE, "PSE ", "pse "},
		{ CR4_DE, "DE ", "de "},
		{ CR4_TSD, "TSD ", "tsd "},
		{ CR4_PVI, "PVI ", "pvi "},
		{ CR4_VME, "VME", "vme"}
	};

	uint8_t i;

	DPRINTF("(");
	for (i = 0; i < 19; i++)
		if (cr4 & cr4_info[i].vrdi_bit)
			DPRINTF(cr4_info[i].vrdi_present);
		else
			DPRINTF(cr4_info[i].vrdi_absent);
	
	DPRINTF(")\n");
}

void
vmm_decode_apicbase_msr_value(uint64_t apicbase)
{
	struct vmm_reg_debug_info apicbase_info[3] = {
		{ APICBASE_BSP, "BSP ", "bsp "},
		{ APICBASE_ENABLE_X2APIC, "X2APIC ", "x2apic "},
		{ APICBASE_GLOBAL_ENABLE, "GLB_EN", "glb_en"}
	};

	uint8_t i;

	DPRINTF("(");
	for (i = 0; i < 3; i++)
		if (apicbase & apicbase_info[i].vrdi_bit)
			DPRINTF(apicbase_info[i].vrdi_present);
		else
			DPRINTF(apicbase_info[i].vrdi_absent);
	
	DPRINTF(")\n");
}

void
vmm_decode_ia32_fc_value(uint64_t fcr)
{
	struct vmm_reg_debug_info fcr_info[4] = {
		{ IA32_FEATURE_CONTROL_LOCK, "LOCK ", "lock "},
		{ IA32_FEATURE_CONTROL_SMX_EN, "SMX ", "smx "},
		{ IA32_FEATURE_CONTROL_VMX_EN, "VMX ", "vmx "},
		{ IA32_FEATURE_CONTROL_SENTER_EN, "SENTER ", "senter "}
	};

	uint8_t i;

	DPRINTF("(");
	for (i = 0; i < 4; i++)
		if (fcr & fcr_info[i].vrdi_bit)
			DPRINTF(fcr_info[i].vrdi_present);
		else
			DPRINTF(fcr_info[i].vrdi_absent);

	if (fcr & IA32_FEATURE_CONTROL_SENTER_EN)
		DPRINTF(" [SENTER param = 0x%llx]",
		    (fcr & IA32_FEATURE_CONTROL_SENTER_PARAM_MASK) >> 8);

	DPRINTF(")\n");
}

void
vmm_decode_mtrrcap_value(uint64_t val)
{
	struct vmm_reg_debug_info mtrrcap_info[3] = {
		{ MTRRcap_FIXED, "FIXED ", "fixed "},
		{ MTRRcap_WC, "WC ", "wc "},
		{ MTRRcap_SMRR, "SMRR ", "smrr "}
	};

	uint8_t i;

	DPRINTF("(");
	for (i = 0; i < 3; i++)
		if (val & mtrrcap_info[i].vrdi_bit)
			DPRINTF(mtrrcap_info[i].vrdi_present);
		else
			DPRINTF(mtrrcap_info[i].vrdi_absent);

	if (val & MTRRcap_FIXED)
		DPRINTF(" [nr fixed ranges = 0x%llx]",
		    (val & 0xff));

	DPRINTF(")\n");
}

void
vmm_decode_perf_status_value(uint64_t val)
{
	DPRINTF("(pstate ratio = 0x%llx)\n", (val & 0xffff));
}

void vmm_decode_perf_ctl_value(uint64_t val)
{
	DPRINTF("(%s ", (val & PERF_CTL_TURBO) ? "TURBO" : "turbo");
	DPRINTF("pstate req = 0x%llx)\n", (val & 0xfffF));
}

void
vmm_decode_mtrrdeftype_value(uint64_t mtrrdeftype)
{
	struct vmm_reg_debug_info mtrrdeftype_info[2] = {
		{ MTRRdefType_FIXED_ENABLE, "FIXED ", "fixed "},
		{ MTRRdefType_ENABLE, "ENABLED ", "enabled "},
	};

	uint8_t i;
	int type;

	DPRINTF("(");
	for (i = 0; i < 2; i++)
		if (mtrrdeftype & mtrrdeftype_info[i].vrdi_bit)
			DPRINTF(mtrrdeftype_info[i].vrdi_present);
		else
			DPRINTF(mtrrdeftype_info[i].vrdi_absent);

	DPRINTF("type = ");
	type = mtrr2mrt(mtrrdeftype & 0xff);
	switch (type) {
	case MDF_UNCACHEABLE: DPRINTF("UC"); break;
	case MDF_WRITECOMBINE: DPRINTF("WC"); break;
	case MDF_WRITETHROUGH: DPRINTF("WT"); break;
	case MDF_WRITEPROTECT: DPRINTF("RO"); break;
	case MDF_WRITEBACK: DPRINTF("WB"); break;
	case MDF_UNKNOWN:
	default:
		DPRINTF("??");
		break;
	}

	DPRINTF(")\n");
}

void
vmm_decode_efer_value(uint64_t efer)
{
	struct vmm_reg_debug_info efer_info[4] = {
		{ EFER_SCE, "SCE ", "sce "},
		{ EFER_LME, "LME ", "lme "},
		{ EFER_LMA, "LMA ", "lma "},
		{ EFER_NXE, "NXE", "nxe"},
	};

	uint8_t i;

	DPRINTF("(");
	for (i = 0; i < 4; i++)
		if (efer & efer_info[i].vrdi_bit)
			DPRINTF(efer_info[i].vrdi_present);
		else
			DPRINTF(efer_info[i].vrdi_absent);

	DPRINTF(")\n");
}

void
vmm_decode_msr_value(uint64_t msr, uint64_t val)
{
	switch (msr) {
	case MSR_APICBASE: vmm_decode_apicbase_msr_value(val); break;
	case MSR_IA32_FEATURE_CONTROL: vmm_decode_ia32_fc_value(val); break;
	case MSR_MTRRcap: vmm_decode_mtrrcap_value(val); break;
	case MSR_PERF_STATUS: vmm_decode_perf_status_value(val); break;
	case MSR_PERF_CTL: vmm_decode_perf_ctl_value(val); break;
	case MSR_MTRRdefType: vmm_decode_mtrrdeftype_value(val); break;
	case MSR_EFER: vmm_decode_efer_value(val); break;
	default: DPRINTF("\n");
	}
}
#endif /* VMM_DEBUG */
