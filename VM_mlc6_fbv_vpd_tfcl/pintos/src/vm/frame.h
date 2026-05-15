#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include "threads/palloc.h"

struct sup_page_entry;  /* forward declaration */
struct thread;          /* forward declaration */

/* One entry per physical user-pool frame currently in use. */
struct frame_entry
{
  struct list_elem elem;
  void *kpage;                    /* kernel virtual address of the frame */
  struct thread *owner;           /* thread that owns this frame */
  struct sup_page_entry *spte;    /* SPT entry mapped to this frame */
  bool pinned;                    /* true = not eligible for eviction */
};

void  frame_init   (void);
void *frame_alloc  (struct sup_page_entry *spte, enum palloc_flags flags);
void  frame_free   (void *kpage);    /* remove + palloc_free_page */
void  frame_remove (void *kpage);    /* remove only, no palloc_free_page */
void  frame_unpin  (void *kpage);    /* allow future eviction */

#endif /* vm/frame.h */
