#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/fs.h>			// file_operations structure - open, read, write, close
#include <linux/cdev.h>			// This is char driver; makes cdev available
#include <linux/semaphore.h>	// Synchronization
#include <linux/uaccess.h>		// copy_to_user; copy_from_user


struct semaphore sem;	// Synchronization
int ret;				// Used to hold return values of functions; Safe on kernel stack space !

#define MBED_PAGE_SIZE 32
#define MBED_BUFFER_SIZE 34			// 1 for COMMAND and 1 for page address
char buffer[MBED_BUFFER_SIZE];		// Simple buffer

// To later register our device we need a cdev instance and some other variables
struct cdev *mcdev;
int major_number;	// Extracted from dev_t using macro - mknod /dev/<file> c major minor
dev_t dev_num;		// Will hold major number that the kernel gives us

// name -> appears in /proc/devices
#define DEVICE_NAME		"mbed"

// Points to the i2c client device
struct i2c_client *i2cclient;

static const struct i2c_device_id mbed_id[] = {
	{ "mbed", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mbed_id);

#ifdef CONFIG_OF
static const struct of_device_id mbed_dt_ids[] = {
	{ .compatible = "bioboost,mbed", },
	{ }
};
MODULE_DEVICE_TABLE(of, mbed_dt_ids);
#endif

/*
	Called on device_file open
	inode: reference to the file on disk and contains info about that file
	struct file: represents and abstract open file
*/	
int device_open(struct inode *inode, struct file *filep)
{
	// Only allow one process to open this device by using a semaphore as mutual exclusive lock - mutex
	if (down_interruptible(&sem) != 0)
	{
		pr_alert("mbed: could not lock device during open\n");
		return -1;
	}

	pr_alert("mbed: opened device\n");
	return 0;
}

/*
	Called when the user closes the device
	inode: reference to the file on disk and contains info about that file
	struct file: represents and abstract open file
*/	
int device_close(struct inode* inode, struct file* filep)
{
	// By calling up (opposite of down for semaphore) we release the mutex that we optained when
	// we opened the device. This has the effect of allowing another process to use the device now
	up(&sem);
	pr_alert("mbed: closed device\n");
	return 0;
}

/*
	Called when the user want to write information to the device
	char* buffSourceData: source in userspace from where to get the data
	size_t buffCount: number of bytes to fetch and store in our device
	loff_t* curOffset: something about current open file offset (??)
*/
ssize_t device_write(struct file* filep, const char* buffSourceData, size_t buffCount, loff_t* currOffset)
{
	int i;

	// buffCount cannot exceed PAGE_SIZE
	if (buffCount > MBED_PAGE_SIZE) {
		pr_alert("mbed: buffCount to big (max 32)\n");
		return -1;
	}

	// Fetch data from user space to kernel space
	// copy_from_user(destination, source, sizeToTransfer)
	pr_alert("mbed: writing to device\n");
	ret = copy_from_user((buffer+2), buffSourceData, buffCount);
	if (ret != 0) {
		pr_alert("mbed: could not copy data from user to kernel space\n");
		return -1;
	}

	// Clear remaining data (may not be good solution but for the moment we use this)
	for (i = buffCount+2; i < MBED_BUFFER_SIZE; i++) {
		buffer[i] = 0;
	}

	// Send the data to the mbed device (currently we only use first page)
	buffer[0] = 0x01;		// Write page command
	buffer[1] = 0;			// Page address
	ret = i2c_master_send(i2cclient, buffer, MBED_BUFFER_SIZE);
	if (ret != MBED_BUFFER_SIZE) {
		pr_alert("mbed: could not send data to device\n");
	}

	return ret;
}

/*
	Called when the user wants to get information from the device
	char* buffStoreData: destination in user space here to store the data
	size_t buffCount: space available in buffStoreData
	loff_t* curOffset: something about current open file offset (??)
*/
ssize_t device_read(struct file* filep, char* buffStoreData, size_t buffCount, loff_t* curOffset)
{
	// buffCount cannot exceed MBED_PAGE_SIZE
	if (buffCount > MBED_PAGE_SIZE) {
		pr_alert("mbed: buffCount to big (max 32)\n");
		return -1;
	}

	// First we need to send page address to device
	buffer[0] = 0x00;		// Page address command
	buffer[1] = 0;			// Page address
	ret = i2c_master_send(i2cclient, buffer, 2);
	if (ret != 2) {
		pr_alert("mbed: could not send page address to device\n");
		return -1;
	}

	// Add some delay
	udelay(1000);

	// Next we fetch the data from the device (full page!)
	ret = i2c_master_recv(i2cclient, buffer, MBED_PAGE_SIZE);
	if (ret != MBED_PAGE_SIZE) {
		pr_alert("mbed: page could not be retrieved\n");
		return -1;
	}

	// Take data from kernel space (device) to user space (process accessing our device)
	// copy_to_user(destination, source, sizeToTransfer)
	pr_alert("mbed: reading data from the device\n");
	ret = copy_to_user(buffStoreData, buffer, buffCount);
	return ret;
}

// Tell the kernel which functions to call when the user operates on our device file
struct file_operations fops = {
	// List of initializers
	.owner = THIS_MODULE,		// prevent unloading of module when operations are in use
	.open = device_open,		// method to call when opening the device
	.release = device_close,	// method to call when closing the device
	.write = device_write,		// method to call when writing to the device
	.read = device_read			// method to call when reading from the device
};


/*
	Initializes the device and registers it with the appropriate kernel framework
	struct i2c_client *client: represents the i2c device itself
	struct i2c_device_id *id: device ID entry that matched the device probed
*/
static int mbed_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	pr_alert("mbed: probe function called\n");

	// Use dynamic allocation to assign our device a major number
	// -- alloc_chrdev_region(dev_t*, uint fminor, uint count, char* name)

	ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);		//dev_num will hold maj number after this call
	if (ret < 0) {
		pr_alert("mbed: failed to allocate a major number\n");
		return ret;		// propage error code
	}
	pr_alert("mbed: module loaded\n");

	// Extract the major number
	major_number = MAJOR(dev_num);		// Macro to extract the major number and store it in our var
	pr_alert("mbed: major number is %d\n", major_number);
	pr_alert("mbed: Use mknod /dev/%s c %d 0 for device file\n", DEVICE_NAME, major_number);

	// Allocate character device structure and initialize
	mcdev = cdev_alloc();			// Create our cdev structure, initialize our cdev structure
	mcdev->ops = &fops;				// Pass pointer of struct file_operations
	mcdev->owner = THIS_MODULE;

	// Now that we create cdev, we have to add it to the kernel
	// int cdev_add(struct cdev* dev, dev_t num, unsigned int count)
	ret = cdev_add(mcdev, dev_num, 1);
	if (ret < 0) {
		pr_alert("mbed: unable to add cdev to kernel\n");
		return ret;
	}

	// Initialize semaphore
	sema_init(&sem, 1);		// initial value of 1

	// Save pointer to client device
	i2cclient = client;

	return 0;
}

/*
	Responsible for unregistering from the kernel framework and to shut it down
	struct i2c_client *client: represents the i2c device itself
*/
static int mbed_remove(struct i2c_client *client)
{
	pr_alert("mbed: remove function called\n");

	// Remove character device from the system
	cdev_del(mcdev);

	// Free the device numbers that we requested in probe
	unregister_chrdev_region(dev_num, 1);
	pr_alert("mbed: unloaded module\n");

	i2cclient = NULL;

	return 0;
}

/*
	struct i2c_driver - represents an I2C device driver
	Tells the kernel what functions to map on device driver actions
*/
static struct i2c_driver mbed_driver = {
	.probe = mbed_probe,
	.remove = mbed_remove,
	.id_table = mbed_id,
	.driver = {
		.name = "mbed",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(mbed_dt_ids),
	},
};

module_i2c_driver(mbed_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simple i2c driver for mbed");
MODULE_AUTHOR("Nico De Witte");
