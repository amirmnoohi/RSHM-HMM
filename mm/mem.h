#ifndef __NSM_PAGES_H__
#define __NSM_PAGES_H__

#include <stddef.h>
#include <linux/slab.h>


#define BUF_LEN 1024000 /* 1MB = Max length of the message from the device */

typedef enum
{
    __INVALID,
    __SHARED,
    __MODIFIED,
    __EXCLUSIVE
} __state_t;

typedef struct
{
    __state_t state;
    struct page *__page;
} __page_t;

typedef struct
{
    __page_t *pages;
    size_t size;
} __memory_t;

extern __memory_t *memory;

void memory_init(void);
void memory_destroy(void);

#endif