#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/sched.h>

MODULE_LICENSE("GPL");

#define VENDOR_ID 0x10ec
#define DEVICE_ID 0x8168

#define DRIVER_NAME "bbn_fpga"
#define BOARD_NAME "piecomm1"

struct DevInfo_t {
    struct pci_dev *pciDev;

    /* character device */
    dev_t cdevNum;
    struct cdev cdev;
    struct class *cls;

    /* kernel's virtual addr. for the mapped BARs */
    void __iomem *ptr_bar0;
};

static struct pci_device_id idTable[] = {
	{ PCI_DEVICE(VENDOR_ID, DEVICE_ID) },
	{ 0, },
};

MODULE_DEVICE_TABLE(pci, idTable);

struct file_operations fileOps = {
	.owner =    THIS_MODULE,
	// .read =     fpga_read,
	// .write =    fpga_write,
	// .open =     fpga_open,
	// .release =  fpga_close,
};

static int setup_chrdev(struct DevInfo_t *devInfo){
	/*
	Setup the /dev/deviceName to allow user programs to read/write to the driver.
	*/

	int devMinor = 0;
	int devMajor = 0; 
	int devNum = -1;

	int result = alloc_chrdev_region(&devInfo->cdevNum, devMinor, 1 /* one device*/, BOARD_NAME);
	if (result < 0) {
		printk(KERN_WARNING "Can't get major ID\n");
		return -1;
	}
	devMajor = MAJOR(devInfo->cdevNum);
	devNum = MKDEV(devMajor, devMinor);
	
	//Initialize and fill out the char device structure
	cdev_init(&devInfo->cdev, &fileOps);
	devInfo->cdev.owner = THIS_MODULE;
	devInfo->cdev.ops = &fileOps;
	result = cdev_add(&devInfo->cdev, devNum, 1 /* one device */);
	if (result) {
		printk(KERN_NOTICE "Error %d adding char device for BBN FPGA driver with major/minor %d / %d", result, devMajor, devMinor);
		return -1;
	}

    devInfo->cls = class_create(THIS_MODULE, BOARD_NAME);
    if(!devInfo->cls){
        printk(KERN_ERR "can't register class for fpga\n");
    }

    device_create(devInfo->cls,NULL,devInfo->cdevNum,NULL,BOARD_NAME);

	return 0;
}

static irqreturn_t ms_pci_interrupt(int irq, void* dev_id)
{
    // struct pci_dev *dev = dev_id;
    // struct DevInfo_t *devInfo = pci_get_drvdata(dev);

    return IRQ_HANDLED;
}

static int probe(struct pci_dev *dev, const struct pci_device_id *id) 
{
    struct DevInfo_t *devInfo = 0;
    int status = 0;

    printk(KERN_INFO "[BBN FPGA] Entered driver probe function.\n");
	printk(KERN_INFO "[BBN FPGA] vendor = 0x%x, device = 0x%x \n", dev->vendor, dev->device); 

    status = pci_resource_len(dev, 0);
    printk("[BBN FPGA] - BAR0 is %d bytes in size\n", status);

    printk("[BBN FPGA] - BAR0 is mapped to 0x%llx\n", pci_resource_start(dev, 0));

    status = pcim_enable_device(dev);
    if(status < 0) {
		printk("[BBN FPGA] - Could not enable device\n");
		return status;
	}

    status = pcim_iomap_regions(dev, BIT(0), KBUILD_MODNAME);
	if(status < 0) {
		printk("[BBN FPGA] - BAR0 is already in use!\n");
		return status;
	}

    devInfo = devm_kmalloc(&dev->dev, sizeof(struct DevInfo_t), GFP_KERNEL);
    if(devInfo == NULL) {
		printk("[BBN FPGA] - Error! Out of memory\n");
		return -ENOMEM;
	}

    devInfo->ptr_bar0 = pcim_iomap_table(dev)[0];
    if(devInfo->ptr_bar0 == NULL) {
		printk("[BBN FPGA] - BAR0 pointer is invalid\n");
		return -1;
	}

    devInfo->pciDev = dev;

    pci_set_drvdata(dev, devInfo);

    setup_chrdev(devInfo);

    status = pci_alloc_irq_vectors(dev, 1, 6, PCI_IRQ_MSI | PCI_IRQ_MSIX);
    if(status < 0){
		printk("[BBN FPGA] - unable to allocate irq 0x%x\n", dev->irq);
        return status;
	}
    printk("[BBN FPGA] - nvec = %d\n",status);
	printk("[BBN FPGA] - request irq:%d\n", dev->irq);

    status = request_irq(pci_irq_vector(dev, 0), ms_pci_interrupt, IRQF_SHARED, DRIVER_NAME, dev);
    if(status){
        printk("[BBN FPGA] - unable to register irq 0x%x\n", dev->irq);
        return status;
    }

    return 0;
}

static void remove(struct pci_dev *dev) 
{
	struct DevInfo_t *devInfo = pci_get_drvdata(dev);
	printk("[BBN FPGA] - Now I am in the remove function.\n");

    // free_irq(dev->irq, dev);
    // pci_disable_msix(devInfo->pciDev);
    // pci_free_irq_vectors(devInfo->pciDev);

    unregister_chrdev_region(devInfo->cdevNum, 1);

    device_destroy(devInfo->cls,devInfo->cdevNum);

    cdev_del(&devInfo->cdev);

    // kfree(devInfo);

    printk("[BBN FPGA] - remove done.\n");
}

static struct pci_driver fpgaDriver = {
	.name = DRIVER_NAME,
	.id_table = idTable,
	.probe = probe,
	.remove = remove,
};


static int fpga_init(void){
	printk(KERN_INFO "[BBN FPGA] Loading BBN FPGA driver!\n");
	return pci_register_driver(&fpgaDriver);
}

static void fpga_exit(void){
	printk(KERN_INFO "[BBN FPGA] Exiting BBN FPGA driver!\n");
	pci_unregister_driver(&fpgaDriver);
}

module_init(fpga_init);
module_exit(fpga_exit);
