#include "BBN_FPGA.h"

// #include "buffer.h"

MODULE_LICENSE("GPL");

#define VENDOR_ID 0x10ec
#define DEVICE_ID 0x8168

#define DRIVER_NAME "bbn_fpga"
#define BOARD_NAME "piecomm1"

#define BUFFER_SIZE (32*1024)

struct DevInfo_t {
    struct pci_dev *pciDev;

    /* character device */
    dev_t cdevNum;
    struct cdev cdev;
    struct class *cls;

    /* Mutex for this device. */
    struct semaphore rx_sem;
    wait_queue_head_t rxq; 

    struct kfifo rx_fifo;

    void *buffer;

    uint32_t interrupt_count;
    uint32_t read_count;

    /* kernel's virtual addr. for the mapped BARs */
    void __iomem *ptr_bar0;
};

static struct pci_device_id idTable[] = {
	{ PCI_DEVICE(VENDOR_ID, DEVICE_ID) },
	{ 0, },
};

MODULE_DEVICE_TABLE(pci, idTable);

int fpga_open(struct inode *inode, struct file *filePtr) 
{
    struct DevInfo_t *devInfo = NULL;

    devInfo = container_of(inode->i_cdev, struct DevInfo_t, cdev);

    // if (down_interruptible(&devInfo->sem)) {
	// 	printk(KERN_WARNING "[BBN FPGA] fpga_open: Unable to get semaphore!\n");
	// 	return -ERESTARTSYS;
	// }

    filePtr->private_data = devInfo;

    // up(&devInfo->sem);

    return 0;
}

ssize_t fpga_read(struct file *filePtr, char __user *buf, size_t count, loff_t *pos)
{
    struct DevInfo_t * devInfo = (struct DevInfo_t *) filePtr->private_data;
    ssize_t bytesDone = 0;

    if (down_interruptible(&devInfo->rx_sem)) {
		printk(KERN_WARNING "[BBN FPGA] fpga_open: Unable to get semaphore!\n");
		return -ERESTARTSYS;
	}

    // while(kfifo_len(&devInfo->rx_fifo) < count){
    //     up(&devInfo->rx_sem);
    //     if(wait_event_interruptible(devInfo->rxq, kfifo_len(&devInfo->rx_fifo) >= count)){
    //         return -ERESTARTSYS;
    //     }
    //     if(down_interruptible(&devInfo->rx_sem)){
    //         return -ERESTARTSYS;
    //     }
    // }

    while(devInfo->read_count == devInfo->interrupt_count){
        up(&devInfo->rx_sem);
        if(wait_event_interruptible(devInfo->rxq, devInfo->read_count != devInfo->interrupt_count)){
            return -ERESTARTSYS;
        }
        if(down_interruptible(&devInfo->rx_sem)){
            return -ERESTARTSYS;
        }
    }

    memcpy_fromio(devInfo->buffer, devInfo->ptr_bar0, BUFFER_SIZE);

    bytesDone = copy_to_user(buf, devInfo->buffer, BUFFER_SIZE);

    devInfo->read_count ++;

    up(&devInfo->rx_sem);

    return bytesDone;
}

struct file_operations fileOps = {
	.owner =    THIS_MODULE,
	.read =     fpga_read,
	// .write =    fpga_write,
	.open =     fpga_open,
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
    struct pci_dev *dev = dev_id;
    struct DevInfo_t *devInfo = pci_get_drvdata(dev);

    devInfo->interrupt_count ++;

    wake_up_interruptible(&devInfo->rxq);

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

    status = kfifo_alloc(&devInfo->rx_fifo, 1024, GFP_KERNEL);
    devInfo->buffer = kmalloc (BUFFER_SIZE * sizeof(char), GFP_KERNEL);

    init_waitqueue_head(&devInfo->rxq);

    sema_init(&devInfo->rx_sem, 1);

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
