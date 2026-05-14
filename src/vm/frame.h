#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include "threads/palloc.h"

struct thread;

struct frame_table_entry
  {
    void *kpage;              /* Kernel virtual address of the frame. */
    void *upage;              /* User virtual address mapped to this frame. */
    struct thread *owner;     /* Thread that owns this frame. */
    struct list_elem elem;    /* Element in the global frame_table list. */
  };

void frame_table_init (void);
void *frame_alloc (enum palloc_flags flags, void *upage);
void frame_free (void *kpage);
void frame_table_remove_process (struct thread *t);

#endif /* vm/frame.h */
