cat > src/sbull.c << 'EOF'
/*
 * sbull.c: Simple Block device for Linux
 * 
 * 这是一个简单的模拟块设备驱动程序
 * 在内存中创建一个虚拟的磁盘设备
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/kernel.h>   /* printk() */
#include <linux/slab.h>     /* kmalloc() */
#include <linux/fs.h>       /* 文件系统相关 */
#include <linux/errno.h>    /* 错误码 */
#include <linux/types.h>    /* size_t */
#include <linux/proc_fs.h>
#include <linux/fcntl.h>    /* O_ACCMODE */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/hdreg.h>
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/version.h>

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Driver Student");
MODULE_DESCRIPTION("A Simple Block Device Driver");

/* 设备配置参数 */
static int nsbulls = 1;           /* 设备数量，默认1个 */
static int nqueues = 1;           /* 队列数量 */
static int hardsect_size = 512;   /* 硬件扇区大小 */
static int logical_block_size = 512; /* 逻辑块大小 */
static int nsectors = 1024;       /* 每个设备的扇区数，默认512KB */
static int max_transfer = 255;    /* 最大传输扇区数 */

module_param(nsbulls, int, 0444);
MODULE_PARM_DESC(nsbulls, "Number of sbull devices to create");
module_param(hardsect_size, int, 0444);
MODULE_PARM_DESC(hardsect_size, "Hardware sector size");
module_param(logical_block_size, int, 0444);
MODULE_PARM_DESC(logical_block_size, "Logical block size");
module_param(nsectors, int, 0444);
MODULE_PARM_DESC(nsectors, "Number of 512-byte sectors per device");
module_param(max_transfer, int, 0444);
MODULE_PARM_DESC(max_transfer, "Maximum number of sectors per transfer");

#define SBULL_MAX_DEVICES 8        /* 最大设备数 */
#define SBULL_SECTOR_SIZE 512      /* 标准扇区大小 */

/* 设备数据结构 */
struct sbull_dev {
    int size;                      /* 设备大小（扇区数） */
    u8 *data;                     /* 数据数组 */
    short users;                  /* 用户计数 */
    short media_change;           /* 介质改变标志 */
    spinlock_t lock;              /* 自旋锁 */
    struct request_queue *queue;  /* 设备请求队列 */
    struct gendisk *gd;           /* 通用磁盘结构 */
    struct blk_mq_tag_set tag_set; /* BLK-MQ标签集 */
};

static struct sbull_dev *devices = NULL; /* 设备数组 */

/* 
 * 处理函数：处理块设备的I/O请求
 * 这是驱动的核心函数，处理所有读写操作
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
static blk_status_t sbull_xfer_request(struct request *req)
{
    struct sbull_dev *dev = req->q->queuedata;
    struct bio_vec bvec;
    struct req_iterator iter;
    sector_t sector = blk_rq_pos(req);
    unsigned int nsect = 0;
    blk_status_t status = BLK_STS_OK;
    
    /* 遍历请求中的所有段 */
    rq_for_each_segment(bvec, req, iter) {
        void *buffer = page_address(bvec.bv_page) + bvec.bv_offset;
        unsigned int len = bvec.bv_len;
        unsigned long offset = (sector << 9) + nsect * 512;
        
        /* 检查是否超出设备范围 */
        if (offset + len > dev->size * 512) {
            status = BLK_STS_IOERR;
            break;
        }
        
        /* 执行读写操作 */
        if (rq_data_dir(req) == WRITE) {
            /* 写操作：从缓冲区复制数据到设备 */
            memcpy(dev->data + offset, buffer, len);
            printk(KERN_DEBUG "sbull: Write %u bytes at sector %llu\n", 
                   len, sector + nsect);
        } else {
            /* 读操作：从设备复制数据到缓冲区 */
            memcpy(buffer, dev->data + offset, len);
            printk(KERN_DEBUG "sbull: Read %u bytes from sector %llu\n", 
                   len, sector + nsect);
        }
        
        nsect += len / 512;
    }
    
    return status;
}

static blk_status_t sbull_queue_rq(struct blk_mq_hw_ctx *hctx,
                                  const struct blk_mq_queue_data *bd)
{
    struct request *req = bd->rq;
    blk_status_t status;
    
    /* 处理请求 */
    status = sbull_xfer_request(req);
    
    /* 完成请求 */
    blk_mq_end_request(req, status);
    
    return BLK_STS_OK;
}
#else
/* 旧版本内核的处理函数 */
static void sbull_request(struct request_queue *q)
{
    struct request *req;
    struct sbull_dev *dev = q->queuedata;
    
    /* 获取下一个请求 */
    req = blk_fetch_request(q);
    while (req != NULL) {
        // 这里需要你补充完整的老版本内核请求处理逻辑
        // 提示：使用__rq_for_each_bio遍历bio，处理每个段
        
        /* 处理完成后，通知内核 */
        __blk_end_request_all(req, 0);
        
        /* 获取下一个请求 */
        req = blk_fetch_request(q);
    }
}
#endif

/*
 * 获取磁盘信息
 */
static int sbull_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
    struct sbull_dev *dev = bdev->bd_disk->private_data;
    long size;
    
    /* 计算柱面、磁头、扇区 */
    size = dev->size * (hardsect_size / SBULL_SECTOR_SIZE);
    geo->cylinders = (size & ~0x3f) >> 6;
    geo->heads = 4;
    geo->sectors = 16;
    geo->start = 0;
    
    return 0;
}

/*
 * 介质改变检查
 */
static int sbull_media_changed(struct gendisk *gd)
{
    struct sbull_dev *dev = gd->private_data;
    
    return dev->media_change;
}

/*
 * 重新验证介质
 */
static int sbull_revalidate(struct gendisk *gd)
{
    struct sbull_dev *dev = gd->private_data;
    
    dev->media_change = 0;
    return 0;
}

/* 块设备操作结构 */
static struct block_device_operations sbull_ops = {
    .owner           = THIS_MODULE,
    .getgeo          = sbull_getgeo,
    .media_changed   = sbull_media_changed,
    .revalidate_disk = sbull_revalidate,
};

/*
 * 设置设备：初始化一个sbull设备
 */
static void setup_device(struct sbull_dev *dev, int which)
{
    /* 初始化自旋锁 */
    spin_lock_init(&dev->lock);
    
    /* 分配设备内存 */
    dev->size = nsectors * hardsect_size;
    dev->data = vmalloc(dev->size);
    if (dev->data == NULL) {
        printk(KERN_ERR "sbull: vmalloc failure.\n");
        return;
    }
    
    /* 初始化数据（可选：填充一些初始数据） */
    memset(dev->data, 0, dev->size);
    
    /* 初始化其他字段 */
    dev->users = 0;
    dev->media_change = 0;
    
    /* 打印设备信息 */
    printk(KERN_INFO "sbull: Device %d initialized, size = %d KB\n", 
           which, dev->size / 1024);
}

/*
 * 模块初始化函数
 */
static int __init sbull_init(void)
{
    int i;
    int ret = 0;
    
    printk(KERN_INFO "sbull: Initializing module\n");
    
    /* 检查参数有效性 */
    if (nsbulls < 1 || nsbulls > SBULL_MAX_DEVICES) {
        printk(KERN_ERR "sbull: Invalid number of devices\n");
        return -EINVAL;
    }
    
    /* 分配设备数组 */
    devices = kmalloc(nsbulls * sizeof(struct sbull_dev), GFP_KERNEL);
    if (!devices) {
        printk(KERN_ERR "sbull: kmalloc failure\n");
        return -ENOMEM;
    }
    memset(devices, 0, nsbulls * sizeof(struct sbull_dev));
    
    /* 初始化每个设备 */
    for (i = 0; i < nsbulls; i++) {
        struct sbull_dev *dev = &devices[i];
        
        /* 设置设备 */
        setup_device(dev, i);
        
        /* 
         * 任务一：这里需要补充请求队列的初始化代码
         * 提示：根据内核版本使用blk_init_queue或blk_mq_init_sq_queue
         */
        
        /* 
         * 任务二：这里需要补充gendisk结构的初始化和注册代码
         * 提示：使用alloc_disk和add_disk函数
         */
        
        if (ret) {
            /* 错误处理 */
            while (i > 0) {
                // 清理已创建的设备
            }
            kfree(devices);
            return ret;
        }
    }
    
    printk(KERN_INFO "sbull: Module loaded with %d device(s)\n", nsbulls);
    return 0;
}

/*
 * 模块清理函数
 */
static void __exit sbull_cleanup(void)
{
    int i;
    
    printk(KERN_INFO "sbull: Unloading module\n");
    
    if (devices) {
        for (i = 0; i < nsbulls; i++) {
            struct sbull_dev *dev = &devices[i];
            
            if (dev->gd) {
                /* 
                 * 任务三：这里需要补充设备卸载的清理代码
                 * 提示：需要删除gendisk，停止请求队列，释放资源
                 */
            }
        }
        kfree(devices);
    }
    
    printk(KERN_INFO "sbull: Module unloaded\n");
}

module_init(sbull_init);
module_exit(sbull_cleanup);
