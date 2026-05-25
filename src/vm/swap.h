#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stddef.h>

void   swap_init  (void);
size_t swap_write (void *kpage);
void   swap_read  (size_t sector, void *kpage);
void   swap_free  (size_t sector);

#endif /* vm/swap.h */
