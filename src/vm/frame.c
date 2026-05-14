#include "vm/frame.h"
#include <list.h>
#include <stdio.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* Global frame table: one entry per physical user-pool frame in use. */
static struct list frame_table;
static struct lock frame_lock;

/* Initializes the frame table. Must be called once during kernel boot,
   after palloc_init() and before any user process runs. */
void
frame_init (void)
{
  list_init (&frame_table);
  lock_init (&frame_lock);
}

/* Allocates a physical frame from the user pool, records it in the frame
   table, and returns its kernel virtual address.
   FLAGS are passed to palloc_get_page (do not include PAL_USER — it is
   added automatically).  UPAGE is the user virtual address that will be
   mapped to this frame; stored for future eviction bookkeeping.
   Returns NULL if no frame is available. */
void *
frame_alloc (enum palloc_flags flags, void *upage)
{
  void *kpage = palloc_get_page (PAL_USER | flags);
  if (kpage == NULL)
    return NULL;

  struct frame_entry *f = malloc (sizeof *f);
  if (f == NULL)
    {
      palloc_free_page (kpage);
      return NULL;
    }

  f->kpage  = kpage;
  f->upage  = upage;
  f->owner  = thread_current ();

  lock_acquire (&frame_lock);
  list_push_back (&frame_table, &f->elem);
  lock_release (&frame_lock);

  return kpage;
}

/* Removes the frame whose kernel virtual address is KPAGE from the frame
   table and frees the underlying physical page back to the user pool.
   Panics if KPAGE is not found in the table (double-free guard). */
void
frame_free (void *kpage)
{
  lock_acquire (&frame_lock);

  struct frame_entry *target = NULL;
  for (struct list_elem *e = list_begin (&frame_table);
       e != list_end (&frame_table);
       e = list_next (e))
    {
      struct frame_entry *f = list_entry (e, struct frame_entry, elem);
      if (f->kpage == kpage)
        {
          target = f;
          list_remove (e);
          break;
        }
    }

  lock_release (&frame_lock);

  /* If not found, the page was not allocated through frame_alloc — treat it
     as a direct palloc free so pagedir_destroy still works correctly. */
  if (target != NULL)
    free (target);

  palloc_free_page (kpage);
}
