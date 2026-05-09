#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <asm/cacheflush.h>
#if(LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,83))
#include <linux/sched/mm.h>
#endif
#include <asm/cpu.h>
#include <asm/io.h>
#include <asm/page.h>
#include <asm/pgtable.h>

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/notifier.h>
#include <asm/ptrace.h>


static struct notifier_block bp_notifier;

static int bp_handler(struct pt_regs *regs)
{
static COPY_MEMORY cm;
    uint64_t pc = regs->pc;

    // 只处理我们的断点
if (pc != cm.addr)
        return 0;
    // 1. 打印命中日志（证明断点触发了）
    pr_info("[SAFE-BP] 命中成功！PC=0x%llx\n", pc);

    // 2. 修复 PC，跳过这条指令，防止死循环
    regs->pc += 4;

    // 3. 不再重新写断点寄存器，也不清零！4.x 内核这么做会崩溃

    return 1;
}

static int die_notify(struct notifier_block *nb, unsigned long action, void *data)
{
    struct pt_regs *regs = (struct pt_regs *)data;

    if (bp_handler(regs)) {
        // 关键：告诉内核“异常已处理，不要panic”
        return NOTIFY_STOP;
    }

    return NOTIFY_DONE;
}

static inline void set_bp(uint64_t addrn)
{
    uint64_t addr = addrn;
    __asm__ volatile (
        "msr DBGBVR0_EL1, %0\n"
        "mov x1, #1\n"
        "orr x1, x1, #(1 << 9)\n" // EL0 用户态
        "msr DBGBCR0_EL1, x1\n"
        :: "r"(addr) : "x1"
    );
}

static inline void clear_bp(void)
{
    uint64_t zero = 0;
    __asm__ volatile (
        "msr DBGBVR0_EL1, %0\n"
        "msr DBGBCR0_EL1, %0\n"
        :: "r"(zero)
    );
}
















#if(LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 61))
phys_addr_t translate_linear_address(struct mm_struct* mm, uintptr_t va) {

	pgd_t *pgd;
	p4d_t *p4d;
	pmd_t *pmd;
	pte_t *pte;
	pud_t *pud;

	phys_addr_t page_addr;
	uintptr_t page_offset;

	pgd = pgd_offset(mm, va);
	if(pgd_none(*pgd) || pgd_bad(*pgd)) {
		return 0;
	}
	p4d = p4d_offset(pgd, va);
	if (p4d_none(*p4d) || p4d_bad(*p4d)) {
		return 0;
	}
	pud = pud_offset(p4d,va);
	if(pud_none(*pud) || pud_bad(*pud)) {
		return 0;
	}
	pmd = pmd_offset(pud,va);
	if(pmd_none(*pmd)) {
		return 0;
	}
	pte = pte_offset_kernel(pmd,va);
	if(pte_none(*pte)) {
		return 0;
	}
	if(!pte_present(*pte)) {
		return 0;
	}
	//页物理地址
	page_addr = (phys_addr_t)(pte_pfn(*pte) << PAGE_SHIFT);
	//页内偏移
	page_offset = va & (PAGE_SIZE-1);

	return page_addr + page_offset;
}
#else
phys_addr_t translate_linear_address(struct mm_struct* mm, uintptr_t va) {

	pgd_t *pgd;
	pmd_t *pmd;
	pte_t *pte;
	pud_t *pud;

	phys_addr_t page_addr;
	uintptr_t page_offset;

	pgd = pgd_offset(mm, va);
	if(pgd_none(*pgd) || pgd_bad(*pgd)) {
		return 0;
	}
	pud = pud_offset(pgd,va);
	if(pud_none(*pud) || pud_bad(*pud)) {
		return 0;
	}
	pmd = pmd_offset(pud,va);
	if(pmd_none(*pmd)) {
		return 0;
	}
	pte = pte_offset_kernel(pmd,va);
	if(pte_none(*pte)) {
		return 0;
	}
	if(!pte_present(*pte)) {
		return 0;
	}
	//页物理地址
	page_addr = (phys_addr_t)(pte_pfn(*pte) << PAGE_SHIFT);
	//页内偏移
	page_offset = va & (PAGE_SIZE-1);

	return page_addr + page_offset;
}
#endif

#ifdef ARCH_HAS_VALID_PHYS_ADDR_RANGE
#define my_valid_phys_addr_range(addr, count) (addr + count <= __pa(high_memory))
#else
#define my_valid_phys_addr_range(addr, count) true
#endif


bool write_physical_address(phys_addr_t pa, void* buffer, size_t size) {
	void* mapped;

	if (!pfn_valid(__phys_to_pfn(pa))) {
		return false;
	}
	if (!my_valid_phys_addr_range(pa, size)) {
		return false;
	}
	mapped =  ioremap_cache(pa, size);
	if (!mapped) {
		return false;
	}
	if(copy_from_user(mapped, buffer, size)) {
		iounmap(mapped);
		return false;
	}
	iounmap(mapped);
	return true;
}

size_t read_physical_address(phys_addr_t pa, void *buffer, size_t size)
{
  void* mapped;
if (!pfn_valid(__phys_to_pfn(pa))) {
		return false;
	}
	if (!my_valid_phys_addr_range(pa, size)) {
		return false;
	}
	
	mapped = ioremap_cache(pa, size);
	if (!mapped) {
		return 0;
	}
flush_cache_range(NULL, (unsigned long)mapped, (unsigned long)mapped + size);
	if(copy_to_user(buffer, mapped, size)) {
		iounmap(mapped);
		return 0;
	}
	iounmap(mapped);
	return size;
}

size_t read_process_memory(pid_t pid, uintptr_t addr, void* buffer, size_t size)
{
	struct task_struct* task;
	struct mm_struct* mm;
	phys_addr_t pa;
	size_t max;
	size_t count = 0;

	task = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (!task) {
		return false;
	}
	mm = get_task_mm(task);
	if (!mm) {
		return false;
	}
	while (size > 0) {
		pa = translate_linear_address(mm, addr);
		max = min(PAGE_SIZE - (addr & (PAGE_SIZE - 1)), min(size, PAGE_SIZE));
		if (!pa) {
			goto none_phy_addr;
		}
		
		count = read_physical_address(pa, buffer, max);
		//printk("[*]pid %d address = %llx  addr %lx   count %zu  size %zu",pid,pa,addr,count,size);
	none_phy_addr:
		size -= max;
		buffer += max;
		addr += max;
	}
	mmput(mm);
	return count;
}

bool write_process_memory(pid_t pid, uintptr_t addr, void* buffer, size_t size)
{
	struct task_struct* task;
	struct mm_struct* mm;
	phys_addr_t pa;
	bool count = false;

	task = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (!task) {
		return false;
	}
	mm = get_task_mm(task);
	if (!mm) {
		return false;
	}
	pa = translate_linear_address(mm, addr);
	if (!pa) {
		goto none_phy_addr;
	}
	count = write_physical_address(pa,buffer,size);
	none_phy_addr:
		mmput(mm);
	return count;
}
// 作用：远程替换进程里的目标函数为你的hook函数
 
