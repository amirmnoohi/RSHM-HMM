#include "mem.h"

__memory_t *memory;

void memory_init(void){
    size_t i;

    memory = kmalloc(sizeof(__memory_t), GFP_KERNEL);
    memory->pages = kmalloc(sizeof(__page_t) * DIV_ROUND_UP(BUF_LEN, PAGE_SIZE), GFP_KERNEL);
    memory->size = DIV_ROUND_UP(BUF_LEN, PAGE_SIZE);

    
    // for each page allocate page
    for (i = 0; i < memory->size; i++)
    {
        memory->pages[i].__page = alloc_page(GFP_KERNEL);
        memory->pages[i].state = __INVALID;
    }
}

void memory_destroy(void){
    size_t i;
    
    for (i = 0; i < memory->size; i++)
    {
        __free_page(memory->pages[i].__page);
    }
    kfree(memory->pages);
    kfree(memory);
}
