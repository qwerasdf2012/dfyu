#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <asm/pgtable.h>
#include <asm/io.h>
#include <asm/processor.h>
#if(LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,83))
#include <linux/sched/mm.h>
#endif

/*
 * 最底层：直接通过 CR3 读取物理内存
 * 不依赖任何内核函数，纯硬件级别操作
 */

/* 获取 CR3 寄存器值（页表基地址） */
static inline unsigned long read_cr3(void)
{
	unsigned long cr3;
	asm volatile("mov %%cr3, %0" : "=r"(cr3));
	return cr3;
}

/* 直接通过物理地址读取内存（MMIO 方式） */
static void* direct_phys_read(phys_addr_t pa)
{
	void* mapped;
	struct vm_struct *vm;
	/* 检查物理地址是否有效 */
	if (!pfn_valid(__phys_to_pfn(pa)))
		return NULL;
	
	/* 使用 ioremap_nocache 直接映射物理内存 */
	mapped = ioremap_cache(pa, PAGE_SIZE);
	/* 找到对应的 vm_struct */
	
	return mapped;
}

/* 
 * 软件页表遍历 - 最原始的方式
 * 不通过内核页表API，直接计算地址
 */
static phys_addr_t raw_va_to_pa(unsigned long pgd_base, uintptr_t va)
{
	unsigned long pgd_idx, p4d_idx, pud_idx, pmd_idx, pte_idx;
	unsigned long pgd_addr, p4d_addr, pud_addr, pmd_addr, pte_addr;
	unsigned long pgd_val, p4d_val, pud_val, pmd_val, pte_val;
	unsigned long pfn;

	/* 计算各级页表索引 */
	pgd_idx = (va >> 39) & 0x1FF;
	p4d_idx = (va >> 30) & 0x1FF;
	pud_idx = (va >> 21) & 0x1FF;
	pmd_idx = (va >> 12) & 0x1FF;
	pte_idx = (va >> 0) & 0x1FF;

	/* 读取 PGD */
	pgd_addr = pgd_base + pgd_idx * 8;
	pgd_val = *(unsigned long*)phys_to_virt(pgd_addr);
	if (!(pgd_val & 1)) return 0;

	/* 读取 P4D (如果支持) */
#if CONFIG_PGTABLE_LEVELS >= 5
	p4d_addr = (pgd_val & 0xFFFFFFFFF000) + p4d_idx * 8;
	p4d_val = *(unsigned long*)phys_to_virt(p4d_addr);
	if (!(p4d_val & 1)) return 0;
	pud_addr = (p4d_val & 0xFFFFFFFFF000) + pud_idx * 8;
#else
	pud_addr = (pgd_val & 0xFFFFFFFFF000) + pud_idx * 8;
#endif

	/* 读取 PUD */
	pud_val = *(unsigned long*)phys_to_virt(pud_addr);
	if (!(pud_val & 1)) return 0;

	/* 检查是否为 1GB 大页 */
	if (pud_val & (1 << 7)) {
		pfn = (pud_val & 0xFFFFFFFFC0000000) >> 12;
		return (pfn << 12) + (va & 0x3FFFFFFF);
	}

	/* 读取 PMD */
	pmd_addr = (pud_val & 0xFFFFFFFFF000) + pmd_idx * 8;
	pmd_val = *(unsigned long*)phys_to_virt(pmd_addr);
	if (!(pmd_val & 1)) return 0;

	/* 检查是否为 2MB 大页 */
	if (pmd_val & (1 << 7)) {
		pfn = (pmd_val & 0xFFFFFFFFFFE00000) >> 12;
		return (pfn << 12) + (va & 0x1FFFFF);
	}

	/* 读取 PTE */
	pte_addr = (pmd_val & 0xFFFFFFFFF000) + pte_idx * 8;
	pte_val = *(unsigned long*)phys_to_virt(pte_addr);
	if (!(pte_val & 1)) return 0;

	pfn = (pte_val & 0xFFFFFFFFF000) >> 12;
	return (pfn << 12) + (va & 0xFFF);
}

/*
 * 直接读取进程内存 - 纯底层实现
 */
bool read_process_memory(pid_t pid, uintptr_t addr, void* buffer, size_t size)
{
	struct task_struct* task;
	struct mm_struct* mm;
	unsigned long pgd_base;
	phys_addr_t pa;
	void* mapped;
	size_t offset, bytes, total = 0;

	task = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (!task) return false;
	
	mm = get_task_mm(task);
	if (!mm) return false;

	/* 获取目标进程的页表基地址 */
	pgd_base = virt_to_phys(mm->pgd);

	while (size > 0) {
		offset = addr & (PAGE_SIZE - 1);
		bytes = min((size_t)(PAGE_SIZE - offset), size);

		/* 直接用原始页表遍历获取物理地址 */
		pa = raw_va_to_pa(pgd_base, addr);
		if (!pa) {
			mmput(mm);
			return false;
		}

		/* 直接映射物理内存 */
		mapped = direct_phys_read(pa);
		if (!mapped) {
			mmput(mm);
			return false;
		}

		/* 直接拷贝数据 */
		if (copy_to_user(buffer + total, mapped + offset, bytes)) {
			iounmap(mapped);
			mmput(mm);
			return false;
		}

		iounmap(mapped);
		total += bytes;
		addr += bytes;
		size -= bytes;
	}

	mmput(mm);
	return total > 0;
}

/*
 * 直接写入进程内存 - 纯底层实现
 */
bool write_process_memory(pid_t pid, uintptr_t addr, void* buffer, size_t size)
{
	struct task_struct* task;
	struct mm_struct* mm;
	unsigned long pgd_base;
	phys_addr_t pa;
	void* mapped;
	size_t offset, bytes, total = 0;

	task = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (!task) return false;
	
	mm = get_task_mm(task);
	if (!mm) return false;

	/* 获取目标进程的页表基地址 */
	pgd_base = virt_to_phys(mm->pgd);

	while (size > 0) {
		offset = addr & (PAGE_SIZE - 1);
		bytes = min((size_t)(PAGE_SIZE - offset), size);

		/* 直接用原始页表遍历获取物理地址 */
		pa = raw_va_to_pa(pgd_base, addr);
		if (!pa) {
			mmput(mm);
			return false;
		}

		/* 直接映射物理内存 */
		mapped = direct_phys_read(pa);
		if (!mapped) {
			mmput(mm);
			return false;
		}

		/* 直接写入数据 */
		if (copy_from_user(mapped + offset, buffer + total, bytes)) {
			iounmap(mapped);
			mmput(mm);
			return false;
		}

		iounmap(mapped);
		total += bytes;
		addr += bytes;
		size -= bytes;
	}

	mmput(mm);
	return total > 0;
}