/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include "opt-A3.h"

#define NUM_STACK_PAGES 12

struct core_map_entry {
   bool available; 
   size_t npages;

};

struct page_table_entry {
    paddr_t paddr;
    int status;
};

struct addrspace {
    struct page_table_entry *text;
    vaddr_t text_vbase;
    size_t text_npages;
    int text_permissions;

    struct page_table_entry *data;
    vaddr_t data_vbase;
    size_t data_npages;
    int data_permissions;

    struct page_table_entry *stack;
    size_t stack_npages;
    int stack_permissions; 

    bool load_elf_completed;
};

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
static struct core_map_entry* core_map;
static paddr_t firstpaddr, lastpaddr;
static size_t ram_npages;

static
paddr_t
getppages(size_t npages)
{
    paddr_t paddr;
    spinlock_acquire(&stealmem_lock);
    if (core_map == NULL) {
        paddr = ram_stealmem(npages);
    } else {
        size_t i;
        size_t page_start = 0;
        size_t npages_required = npages;
        for (i = 0; i < ram_npages; ++i) {
            if (core_map[i].available) {
                npages_required--;
                if (npages_required == 0) {
                    break;
                }
            } else {
                npages_required = npages; 
                page_start = i + 1;
            } 
        }        

        if (i == ram_npages) {
            spinlock_release(&stealmem_lock);
            return 0;
        } 

        core_map[page_start].npages = npages;
        for (i = page_start; i < page_start + npages; ++i) {
            core_map[i].available = false;
        } 
        paddr = firstpaddr + page_start * PAGE_SIZE;
    }
    spinlock_release(&stealmem_lock);
    return paddr;
}

static
void
freeppages(paddr_t paddr)
{
    if (core_map != NULL) {
        size_t page_start = (paddr - firstpaddr) / PAGE_SIZE;    
        size_t npages = core_map[page_start].npages;
        KASSERT(npages > 0);
        for (size_t i = page_start; i < page_start + npages; ++i) {
            KASSERT(!core_map[i].available);
            core_map[i].available = true;
            core_map[i].npages = 0;
        }
    }
}

/* Initialization function */
void 
vm_bootstrap(void)
{
    ram_getsize(&firstpaddr, &lastpaddr);
    ram_npages = (lastpaddr - firstpaddr) / PAGE_SIZE;
    
    core_map = (struct core_map_entry *) PADDR_TO_KVADDR(firstpaddr); 
    size_t core_map_size = sizeof(struct core_map_entry) * ram_npages; 
    size_t core_map_npages = ROUNDUP(core_map_size, PAGE_SIZE) / PAGE_SIZE;

    for (size_t i = 0; i < ram_npages; ++i) {
        core_map[i].available = (i < core_map_npages) ? false : true;
        core_map[i].npages = 0;
    }
    core_map[0].npages = core_map_npages;
}

/* Allocate/free kernel heap pages (called by kmalloc/kfree) */
vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void 
free_kpages(vaddr_t addr)
{
    freeppages(KVADDR_TO_PADDR(addr));
}

/* Fault handling function called by trap code */
int 
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t text_vbase, text_vtop, data_vbase, data_vtop, stack_vbase, stack_vtop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;
    bool is_text_segment = false;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "smartvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
        return EFAULT;
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->text != NULL);
	KASSERT(as->text_vbase != 0);
	KASSERT(as->text_npages != 0);
	KASSERT(as->data != NULL);
	KASSERT(as->data_vbase != 0);
	KASSERT(as->data_npages != 0);
	KASSERT(as->stack != NULL);
	KASSERT((as->text_vbase & PAGE_FRAME) == as->text_vbase);
	KASSERT((as->data_vbase & PAGE_FRAME) == as->data_vbase);

	text_vbase = as->text_vbase;
	text_vtop = text_vbase + as->text_npages * PAGE_SIZE;
	data_vbase = as->data_vbase;
	data_vtop = data_vbase + as->data_npages * PAGE_SIZE;
	stack_vbase = USERSTACK - NUM_STACK_PAGES * PAGE_SIZE;
	stack_vtop = USERSTACK;

	if (faultaddress >= text_vbase && faultaddress < text_vtop) {
        size_t i = (faultaddress - text_vbase) / PAGE_SIZE;
		paddr = as->text[i].paddr;
        is_text_segment = true;
	}
	else if (faultaddress >= data_vbase && faultaddress < data_vtop) {
        size_t i = (faultaddress - data_vbase) / PAGE_SIZE;
		paddr = as->data[i].paddr;
	}
	else if (faultaddress >= stack_vbase && faultaddress < stack_vtop) {
        size_t i = (faultaddress - stack_vbase) / PAGE_SIZE;
		paddr = as->stack[i].paddr;
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
        if (as->load_elf_completed && is_text_segment) {
            elo &= ~TLBLO_DIRTY;
        }
		DEBUG(DB_VM, "smartvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

    ehi = faultaddress;
    elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
    if (as->load_elf_completed && is_text_segment) {
        elo &= ~TLBLO_DIRTY;
    }
    DEBUG(DB_VM, "smartvm: 0x%x -> 0x%x\n", faultaddress, paddr);
    tlb_random(ehi, elo);
    splx(spl);
    return 0;
}

/* TLB shootdown handling called from interprocessor_interrupt */
void 
vm_tlbshootdown_all(void)
{
    panic("smartvm tried to do tlb shootdown?!\n");
}

void 
vm_tlbshootdown(const struct tlbshootdown * ts)
{
    (void) ts;
    panic("smartvm tried to do tlb shootdown?!\n");
}

static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

struct addrspace *
as_create(void)
{
    struct addrspace *as = kmalloc(sizeof(struct addrspace));
    if (as == NULL) {
        return NULL;
    }

    as->text = NULL;
    as->text_vbase = 0;
    as->text_npages = 0;
    as->text_permissions = 0;

    as->data = NULL;
    as->data_vbase = 0;
    as->data_npages = 0;
    as->data_permissions = 0;

    as->stack = NULL;
    as->stack_permissions = 0;

    as->load_elf_completed = false;

    return as;
}

void
as_destroy(struct addrspace *as)
{
    if (as != NULL) {
        if (as->text != NULL && as->text_vbase != 0 && as->text_npages != 0) {
            for (size_t i = 0; i < as->text_npages; ++i) {
                freeppages(as->text[i].paddr);             
            }
            kfree(as->text);
        } 
        if (as->data != NULL && as->data_vbase != 0 && as->data_npages != 0) {
            for (size_t i = 0; i < as->data_npages; ++i) {
                freeppages(as->data[i].paddr);             
            }
            kfree(as->data);
        } 
        if (as->stack != NULL) {
            for (size_t i = 0; i < NUM_STACK_PAGES; ++i) {
                freeppages(as->stack[i].paddr);             
            }
            kfree(as->stack);
        } 
	    kfree(as);
    }
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
    /* nothing */
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	if (as->text_vbase == 0) {
		as->text_vbase = vaddr;
		as->text_npages = npages;
        as->text_permissions = readable | writeable | executable;
		return 0;
	}

	if (as->data_vbase == 0) {
		as->data_vbase = vaddr;
		as->data_npages = npages;
        as->data_permissions = readable | writeable | executable;
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("smartvm: Warning: too many regions\n");
	return EUNIMP;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	KASSERT(as->stack != NULL);

	*stackptr = USERSTACK;
	return 0;
}

int
as_prepare_load(struct addrspace *as)
{
	KASSERT(as->text == NULL);
	KASSERT(as->data == NULL);
    KASSERT(as->stack == NULL);

    as->text = (struct page_table_entry *) 
        kmalloc(sizeof(struct page_table_entry) * as->text_npages);
    for (size_t i = 0; i < as->text_npages; ++i) {
        as->text[i].paddr = getppages(1);
        if (as->text[i].paddr == 0) {
            return ENOMEM;
        } else {
            as_zero_region(as->text[i].paddr, 1);
        }
    }

    as->data = (struct page_table_entry *) 
        kmalloc(sizeof(struct page_table_entry) * as->data_npages);
    for (size_t i = 0; i < as->data_npages; ++i) {
        as->data[i].paddr = getppages(1);
        if (as->data[i].paddr == 0) {
            return ENOMEM;
        } else {
            as_zero_region(as->data[i].paddr, 1);
        }
    }

    as->stack = (struct page_table_entry *) 
        kmalloc(sizeof(struct page_table_entry) * NUM_STACK_PAGES);
    for (size_t i = 0; i < NUM_STACK_PAGES; ++i) {
        as->stack[i].paddr = getppages(1);
        if (as->stack[i].paddr == 0) {
            return ENOMEM;
        } else {
            as_zero_region(as->stack[i].paddr, 1);
        }
    }

	KASSERT(as->text != NULL);
	KASSERT(as->data != NULL);
    KASSERT(as->stack != NULL);

    as->load_elf_completed = false;

	return 0;
}


int
as_complete_load(struct addrspace *as)
{
	int i, spl;

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
    as->load_elf_completed = true;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

    new->text_vbase = old->text_vbase;
    new->text_npages = old->text_npages;
    new->text_permissions = old->text_permissions;

    new->data_vbase = old->data_vbase;
    new->data_npages = old->data_npages;
    new->data_permissions = old->data_permissions;

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

	KASSERT(new->text != NULL);
	KASSERT(new->data != NULL);
	KASSERT(new->stack != NULL);

    for (size_t i = 0; i < new->text_npages; ++i) {
        memmove((void *)PADDR_TO_KVADDR(new->text[i].paddr),
            (const void *)PADDR_TO_KVADDR(old->text[i].paddr),
            PAGE_SIZE);
    }

    for (size_t i = 0; i < new->data_npages; ++i) {
        memmove((void *)PADDR_TO_KVADDR(new->data[i].paddr),
            (const void *)PADDR_TO_KVADDR(old->data[i].paddr),
            PAGE_SIZE);
    }

    for (size_t i = 0; i < NUM_STACK_PAGES; ++i) {
        memmove((void *)PADDR_TO_KVADDR(new->stack[i].paddr),
            (const void *)PADDR_TO_KVADDR(old->stack[i].paddr),
            PAGE_SIZE);
    }
	
	*ret = new;
	return 0;
}

