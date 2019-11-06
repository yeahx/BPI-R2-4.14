/* Our First Driver code */
// https://sysplay.github.io/books/LinuxDrivers/book/Content/Part02.html

#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/sched.h> //added

static int __init ofd_init(void) /* Constructor */
{
	printk(KERN_INFO "ofd registered");
	printk(KERN_INFO "The Process is \"%s\" (pid %i)\n",
		current->comm, current->pid);
	return 0;
}

static void __exit ofd_exit(void) /* Destructor */
{
    printk(KERN_INFO "ofd unregistered");
}

module_init(ofd_init);
module_exit(ofd_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("name <email@email.com>");
MODULE_DESCRIPTION("Our First Driver");
