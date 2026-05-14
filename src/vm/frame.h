#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include "threads/palloc.h"

/* Entry in the global frame table. Tracks one physical frame allocated
   from the user pool, which thread owns it, and its user virtual address. */
struct frame_entry
  {
    void *kpage;               /* Kernel virtual address of this frame. */
    void *upage;               /* User virtual address mapped to this frame. */
    struct thread *owner;      /* Thread that owns this frame. */
    struct list_elem elem;     /* Node in the global frame_table list. */
  };

void frame_init (void);
void *frame_alloc (enum palloc_flags flags, void *upage);
void frame_free (void *kpage);

#endif /* vm/frame.h */
