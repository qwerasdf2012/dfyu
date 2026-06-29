#include <linux/module.h>
#include <linux/tty.h>
#include <linux/miscdevice.h>
#include <linux/proc_fs.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include "comm.h"
#include "memory.h"
#include "process.h"
char *devicename;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
	MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver); 
#endif

/* 内核版本兼容性 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
	#define mmap_read_lock(mm)   down_read(&(mm)->mmap_lock)
	#define mmap_read_unlock(mm) up_read(&(mm)->mmap_lock)
	#define mmap_write_lock(mm)  down_write(&(mm)->mmap_lock)
	#define mmap_write_unlock(mm) up_write(&(mm)->mmap_lock)
#else
	#define mmap_read_lock(mm)   down_read(&(mm)->mmap_sem)
	#define mmap_read_unlock(mm) up_read(&(mm)->mmap_sem)
	#define mmap_write_lock(mm)  down_write(&(mm)->mmap_sem)
	#define mmap_write_unlock(mm) up_write(&(mm)->mmap_sem)
#endif

struct mem_tool_device {
	struct cdev cdev;
	struct device *dev;
	int max;
};
static struct mem_tool_device *memdev;
static struct list_head *prev_module;
static struct list_head prev_list_saved;  /* 保存原始链表，用于正确恢复 */
static dev_t mem_tool_dev_t;
static struct class *mem_tool_class;
static bool module_hidden = false;
static DEFINE_MUTEX(hide_mutex);

static int dispatch_open(struct inode *node, struct file *file);
static int dispatch_close(struct inode *node, struct file *file);
static long dispatch_ioctl(struct file* const file, unsigned int const cmd, unsigned long const arg);

struct file_operations dispatch_functions = {
	.owner          = THIS_MODULE,
	.open           = dispatch_open,
	.release        = dispatch_close,
	.unlocked_ioctl = dispatch_ioctl,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)
	.ioctl          = dispatch_ioctl,  /* 兼容老内核 */
#endif
};

long dispatch_ioctl(struct file* const file, unsigned int const cmd, unsigned long const arg)
{
	static COPY_MEMORY cm;
	static MODULE_BASE mb;
	static char name[0x100] = {0};
	/* 验证代码保留注释状态，需要时取消注释 */
	/*static char key[0x100] = {0};
	static bool is_verified = false;
	if(cmd == OP_INIT_KEY && !is_verified) {
		if (copy_from_user(key, (void __user*)arg, sizeof(key)-1) != 0) {
			return -1;
		}
		is_verified = init_key(key, sizeof(key));
	}
	if(is_verified == false) {
		return -1;
	}*/
	switch (cmd) {
		case OP_READ_MEM:
			{
				if (copy_from_user(&cm, (void __user*)arg, sizeof(cm)) != 0) {
					return -EFAULT;
				}
				if (read_process_memory(cm.pid, cm.addr, cm.buffer, cm.size) == false) {
					return -EINVAL;
				}
			}
			break;
		case OP_WRITE_MEM:
			{
				if (copy_from_user(&cm, (void __user*)arg, sizeof(cm)) != 0) {
					return -EFAULT;
				}
				if (write_process_memory(cm.pid, cm.addr, cm.buffer, cm.size) == false) {
					return -EINVAL;
				}
			}
			break;
		case OP_MODULE_BASE:
			{
				if (copy_from_user(&mb, (void __user*)arg, sizeof(mb)) != 0 
				|| copy_from_user(name, (void __user*)mb.name, sizeof(name)-1) != 0) {
					return -EFAULT;
				}
				name[sizeof(name)-1] = '\0';  /* 确保字符串终止 */
				mb.base = get_module_base(mb.pid, name);
				if (copy_to_user((void __user*)arg, &mb, sizeof(mb)) != 0) {
					return -EFAULT;
				}
			}
			break;
		default:
			return -ENOTTY;
	}
	return 0;
}

int dispatch_open(struct inode *node, struct file *file)
{
	mutex_lock(&hide_mutex);
	
	file->private_data = memdev;
	
	/* 防止重复隐藏 */
	if (!module_hidden) {
		prev_module = __this_module.list.prev;
		if (prev_module) {
			prev_list_saved = *prev_module;
		}
		list_del_init(&__this_module.list);  /* 摘除链表，/proc/modules 中不可见 */
		module_hidden = true;
	}
	/* 安全删除设备文件和类，避免重复删除 */
	if (mem_tool_class && !IS_ERR(mem_tool_class)) {
		if (memdev && memdev->dev && !IS_ERR(memdev->dev)) {
			device_destroy(mem_tool_class, mem_tool_dev_t);
			memdev->dev = NULL;
		}
		class_destroy(mem_tool_class);
		mem_tool_class = NULL;
	}
	
	mutex_unlock(&hide_mutex);
	
	printk(KERN_INFO "打开文件成功\n");
	return 0;
}

int dispatch_close(struct inode *node, struct file *file)
{
	mutex_lock(&hide_mutex);
	
	/* 仅在已隐藏时恢复 */
	if (module_hidden && prev_module) {
		*prev_module = prev_list_saved;
		list_add(&__this_module.list, prev_module);
		module_hidden = false;
	}
	
	/* 避免重复创建，判空 */
	if (!mem_tool_class) {
		mem_tool_class = class_create(THIS_MODULE, devicename);
		if (!IS_ERR(mem_tool_class) && memdev && !memdev->dev) {
			memdev->dev = device_create(mem_tool_class, NULL, mem_tool_dev_t, NULL, "%s", devicename);
		}
	}
	
	mutex_unlock(&hide_mutex);
	
	printk(KERN_INFO "关闭文件成功\n");
	return 0;
}

static int __init driver_entry(void)
{

	int ret;
	devicename = DEVICE_NAME;
	devicename = get_rand_str();  /* 注释此行关闭随机驱动名 */

	/* 1. 动态申请设备号 */
	ret = alloc_chrdev_region(&mem_tool_dev_t, 0, 1, devicename);
	if (ret < 0) {
		printk(KERN_ERR "设备编号分配失败: %d\n", ret);
		return ret;
	}

	/* 2. 动态申请设备结构体的内存 */
	memdev = kzalloc(sizeof(struct mem_tool_device), GFP_KERNEL);
	if (!memdev) {
		ret = -ENOMEM;
		printk(KERN_ERR "内存分配失败\n");
		goto fail_region;
	}

	/* 3. 初始化并且添加cdev结构体 */
	cdev_init(&memdev->cdev, &dispatch_functions);
	memdev->cdev.owner = THIS_MODULE;
	/* cdev_init 已经设置了 ops，这行是冗余的，删除 */
	/* memdev->cdev.ops = &dispatch_functions; */

	ret = cdev_add(&memdev->cdev, mem_tool_dev_t, 1);
	if (ret) {
		printk(KERN_ERR "注册cdev失败: %d\n", ret);
		goto fail_cdev;
	}
		/* 4. 创建设备文件 */
	mem_tool_class = class_create(THIS_MODULE, devicename);
	if (IS_ERR(mem_tool_class)) {
		ret = PTR_ERR(mem_tool_class);
		printk(KERN_ERR "创建设备类失败: %d\n", ret);
		goto fail_cdev_add;
	}

	memdev->dev = device_create(mem_tool_class, NULL, mem_tool_dev_t, NULL, "%s", devicename);
	if (IS_ERR(memdev->dev)) {
		ret = PTR_ERR(memdev->dev);
		printk(KERN_ERR "创建设备文件失败: %d\n", ret);
		goto fail_class;
	}

	/* 隐蔽措施：删除可疑的proc文件 */
	if (!IS_ERR(filp_open("/proc/sched_debug", O_RDONLY, 0))) {
		remove_proc_subtree("sched_debug", NULL);
	}
	if (!IS_ERR(filp_open("/proc/uevents_records", O_RDONLY, 0))) {
		remove_proc_entry("uevents_records", NULL);
	}

	/* 释放设备号，/proc/devices 中不可见 */
	unregister_chrdev_region(mem_tool_dev_t, 1);
	
	/* 保存链表状态后摘除，/proc/modules 中不可见 */
	prev_module = __this_module.list.prev;
	if (prev_module) {
		prev_list_saved = *prev_module;
	}
	list_del_init(&__this_module.list);
	module_hidden = true;
	
	/* 摘除kobj，/sys/modules/中不可见 */
	kobject_del(&THIS_MODULE->mkobj.kobj);

	printk(KERN_INFO "设备创建成功 %s\n", devicename);
	return 0;

fail_class:
	class_destroy(mem_tool_class);
fail_cdev_add:
	cdev_del(&memdev->cdev);
fail_cdev:
	kfree(memdev);
	memdev = NULL;
fail_region:
	unregister_chrdev_region(mem_tool_dev_t, 1);
	return ret;
}

static void __exit driver_unload(void)
{
	/* 如果模块仍处于隐藏状态，先恢复 */
	if (module_hidden && prev_module) {
		*prev_module = prev_list_saved;
		list_add(&__this_module.list, prev_module);
		module_hidden = false;
	}

	/* 删除设备文件 */
	if (mem_tool_class && !IS_ERR(mem_tool_class)) {
		if (memdev && memdev->dev && !IS_ERR(memdev->dev)) {
			device_destroy(mem_tool_class, mem_tool_dev_t);
		}
		class_destroy(mem_tool_class);
	}

	/* 注销cdev */
	if (memdev) {
		cdev_del(&memdev->cdev);
		kfree(memdev);
		memdev = NULL;
	}

	/* 释放设备号 */
	unregister_chrdev_region(mem_tool_dev_t, 1);

	printk(KERN_INFO "设备删除成功 %s\n", devicename);
}

module_init(driver_entry);
module_exit(driver_unload);

MODULE_LICENSE("GPL");