/* Our Second Driver code */
// https://sysplay.github.io/books/LinuxDrivers/book/Content/Part02.html

#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/sched.h> //added
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

static int my_second_probe(struct platform_device *pdev)
{
	//const struct of_device_id *of_id =of_match_device(second_driver_dt_ids, &pdev->dev);
	//if (of_id) {/* Use of_id->data here */}
	//printk(KERN_INFO "osd probed");
	dev_info(&pdev->dev,"osd probed");
	return 0;
}

static int my_second_remove(struct platform_device *pdev)
{
	//printk(KERN_INFO "osd removed");
	dev_info(&pdev->dev,"osd removed");
	return 0;
}

//DTS-binding
/*
seconddriver {
	compatible = "our,second-driver";
	reg = <0x50100000 0x500>;
	status = "okay";
};
*/

static struct of_device_id second_driver_dt_ids[] = {
	{.compatible = "our,second-driver", .data = NULL},
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, second_driver_dt_ids);

static struct platform_driver my_second_driver = {
	.probe = my_second_probe,
	.remove = my_second_remove,
	.driver = {
		.name = "my second driver",
		.of_match_table = second_driver_dt_ids,
	},
};

static int __init osd_init(void)
{
	printk(KERN_INFO "osd registering");
	printk(KERN_INFO "The Process is \"%s\" (pid %i)\n",
		current->comm, current->pid);

	return platform_driver_register(&my_second_driver);
}

static void __exit osd_exit(void)
{
	printk(KERN_INFO "osd unregistering");
	platform_driver_unregister(&my_second_driver);
}

module_init(osd_init);
module_exit(osd_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("name <email@email.com>");
MODULE_DESCRIPTION("Our Second Driver");
