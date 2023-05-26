#include <linux/init.h>    // Default macros for __init and __exit functions
#include <linux/module.h>  // module functions
#include <linux/device.h>  // Needed to support kernel driver module
#include <linux/kernel.h>  // Debug macros
#include <linux/fs.h>      // Linux file systems support
#include <linux/uaccess.h> // Needed for 'copy_to_user'/'copy_from_user' functions
#include <crypto/hash.h>   // Support to hashing cryptographic functions
#include <linux/slab.h>    // In-kernel dynamic memory allocation
#include <linux/types.h>   // Required for custom data types
#include <linux/mm.h>
#include <linux/crypto.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>
#include <linux/timekeeping.h>


// Define a static function named "nsm_init" that returns an integer value
static int __init rshm_init(void)
{
    ktime_t start, end;
    s64 average_time, diff;
    u64 total_time;
    int i, j;

    void *page_a, *page_b;
    dma_addr_t dma_page_a, dma_page_b;

    // Allocate memory for page A
    page_a = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!page_a)
    {
        pr_err("Failed to allocate memory for page A\n");
        return -ENOMEM;
    }

    // Allocate memory for page B
    page_b = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!page_b)
    {
        pr_err("Failed to allocate memory for page B\n");
        kfree(page_a);
        return -ENOMEM;
    }

    // Fill page A with some data
    memset(page_a, 0xAA, PAGE_SIZE);

    // Technique 1: memcpy

    for (i = 1; i <= 1000000; i *= 10)
    {
        total_time = 0;
        for (j = 0; j < i; j++)
        {
            start = ktime_get();
            memcpy(page_b, page_a, PAGE_SIZE);
            end = ktime_get();
            diff = ktime_to_ns(ktime_sub(end, start));
            total_time += diff;

        }
        average_time = total_time / i;
        pr_info("memcpy Average Time for %d Iteration: %lld ns\n", i, average_time);
    }

    memset(page_a, 0xBB, PAGE_SIZE);

    for (i = 1; i <= 1000000; i *= 10)
    {
        total_time = 0;
        for (j = 0; j < i; j++)
        {
            start = ktime_get();
            copy_page(page_b, page_a);
            end = ktime_get();
            diff = ktime_to_ns(ktime_sub(end, start));
            total_time += diff;

        }
        average_time = total_time / i;
        pr_info("Copy Page Average Time for %d Iteration: %lld ns\n", i, average_time);
    }


    kfree(page_a);
    kfree(page_b);
    
    return 0;
}

// Define a static function named "nsm_exit" that doesn't return anything
static void __exit rshm_exit(void)
{
    pr_notice("RSHM Fully Unloaded\n");
}

module_init(rshm_init);
module_exit(rshm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("AMIRMNOOHI");
MODULE_DESCRIPTION("RSHM: Transparent Distributed Shared Memory");
