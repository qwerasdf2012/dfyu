#include <linux/sched.h>
#include <linux/pid.h>
#include <asm/ptrace.h>
#include <asm/hw_breakpoint.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <linux/err.h>          // 新增：提供 IS_ERR、PTR_ERR
#include <linux/module.h>       // 如果作为模块可加，非必须

static pid_t target_pid = 0;
static unsigned long target_va = 0;
static struct perf_event *bp_event = NULL;

static void breakpoint_handler(struct perf_event *event,
                               struct perf_sample_data *data,
                               struct pt_regs *regs)
{
    struct task_struct *tsk = current;

    if (tsk->pid != target_pid || !user_mode(regs))
        return;

    if (regs->pc == target_va) {
        regs->regs[0] = 0;          // 将 x0 置 0
        regs->pc = regs->regs[30];  // 跳转到 x30（LR）

        // 禁用并重新启用，以维持事件状态（实际不会重复触发，因 PC 已改）
        perf_event_disable(event);
        perf_event_enable(event);
    }
}

int hook_attach(pid_t pid, unsigned long user_va)
{
    struct perf_event_attr attr = {0};
    struct task_struct *tsk;
    struct pid *pid_ptr;

    if (bp_event)
        return -EEXIST;

    pid_ptr = find_vpid(pid);
    if (!pid_ptr)
        return -ESRCH;

    tsk = pid_task(pid_ptr, PIDTYPE_PID);
    if (!tsk || !tsk->mm)
        return -EINVAL;

    target_pid = pid;
    target_va = user_va;

    attr.type = PERF_TYPE_BREAKPOINT;
    attr.size = sizeof(attr);
    attr.bp_type = HW_BREAKPOINT_X;
    attr.bp_addr = user_va;
    attr.bp_len = HW_BREAKPOINT_LEN_4;

    bp_event = perf_event_create_kernel_counter(&attr, -1, tsk,
                                                breakpoint_handler, NULL);
    if (IS_ERR(bp_event)) {
        int err = PTR_ERR(bp_event);
        bp_event = NULL;
        target_pid = 0;
        target_va = 0;
        return err;
    }
    return 0;
}

void hook_detach(void)
{
    if (bp_event) {
        perf_event_release_kernel(bp_event);
        bp_event = NULL;
    }
    target_pid = 0;
    target_va = 0;
}