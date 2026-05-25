#include "vm/frame.h"
#include <list.h>
#include <debug.h>
#include <string.h>
#include "filesys/file.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "vm/swap.h"

extern struct lock filesys_lock;

static struct list frame_table;
static struct lock frame_lock;
static struct list_elem *clock_hand;   /* current position in clock sweep */

static void *frame_evict (struct sup_page_entry *new_spte,
                           enum palloc_flags flags);

void
frame_init (void)
{
  list_init (&frame_table);
  lock_init (&frame_lock);
  clock_hand = NULL;
}

/* Allocates a physical frame, records it in the frame table with SPTE as
   owner, and returns its kernel virtual address.  The frame is created
   with pinned=true; caller must call frame_unpin() after installing the
   page mapping to allow eviction. */
void *
frame_alloc (struct sup_page_entry *spte, enum palloc_flags flags)
{
  lock_acquire (&frame_lock);

  void *kpage = palloc_get_page (flags);
  if (kpage != NULL)
    {
      struct frame_entry *fte = malloc (sizeof *fte);
      if (fte == NULL)
        {
          palloc_free_page (kpage);
          lock_release (&frame_lock);
          return NULL;
        }
      fte->kpage  = kpage;
      fte->owner  = thread_current ();
      fte->spte   = spte;
      fte->pinned = true;
      list_push_back (&frame_table, &fte->elem);
      lock_release (&frame_lock);
      return kpage;
    }

  /* Physical memory full — evict a frame. */
  kpage = frame_evict (spte, flags);
  lock_release (&frame_lock);
  return kpage;
}

/* Clock (second-chance) eviction.  Called with frame_lock held.
   Releases and reacquires frame_lock around I/O to avoid deadlock. */
static void *
frame_evict (struct sup_page_entry *new_spte, enum palloc_flags flags)
{
  if (list_empty (&frame_table))
    return NULL;

  size_t n = list_size (&frame_table) * 2 + 2;

  for (size_t i = 0; i < n; i++)
    {
      if (clock_hand == NULL || clock_hand == list_end (&frame_table))
        clock_hand = list_begin (&frame_table);

      struct frame_entry *fte =
        list_entry (clock_hand, struct frame_entry, elem);

      clock_hand = list_next (clock_hand);
      if (clock_hand == list_end (&frame_table))
        clock_hand = list_begin (&frame_table);

      if (fte->pinned)
        continue;

      struct thread           *owner    = fte->owner;
      struct sup_page_entry   *old_spte = fte->spte;

      if (pagedir_is_accessed (owner->pagedir, old_spte->upage))
        {
          pagedir_set_accessed (owner->pagedir, old_spte->upage, false);
          continue;
        }

      /* Victim selected. */
      bool dirty = pagedir_is_dirty (owner->pagedir, old_spte->upage)
                   || old_spte->dirty;

      pagedir_clear_page (owner->pagedir, old_spte->upage);
      old_spte->kpage = NULL;
      old_spte->dirty = dirty;

      void *kpage = fte->kpage;

      /* Update frame entry in-place before releasing the lock so no
         other thread can select this frame as a victim. */
      fte->spte   = new_spte;
      fte->owner  = thread_current ();
      fte->pinned = true;

      lock_release (&frame_lock);

      /* I/O without frame_lock to avoid deadlock with filesys_lock. */
      if (old_spte->is_mmap)
        {
          if (dirty)
            {
              lock_acquire (&filesys_lock);
              file_write_at (old_spte->file, kpage,
                             old_spte->read_bytes, old_spte->file_offset);
              lock_release (&filesys_lock);
            }
          old_spte->location = PAGE_FILE;
        }
      else
        {
          old_spte->swap_sector = swap_write (kpage);
          old_spte->location   = PAGE_SWAP;
        }

      if (flags & PAL_ZERO)
        memset (kpage, 0, PGSIZE);

      lock_acquire (&frame_lock);
      return kpage;
    }

  return NULL;
}

/* Removes the frame from the table, frees the frame_entry struct, and
   returns the physical page to the user pool. */
void
frame_free (void *kpage)
{
  lock_acquire (&frame_lock);

  for (struct list_elem *e = list_begin (&frame_table);
       e != list_end (&frame_table);
       e = list_next (e))
    {
      struct frame_entry *fte = list_entry (e, struct frame_entry, elem);
      if (fte->kpage == kpage)
        {
          if (clock_hand == e)
            clock_hand = list_next (e);
          list_remove (e);
          free (fte);
          break;
        }
    }

  lock_release (&frame_lock);
  palloc_free_page (kpage);
}

/* Removes the frame from the table without freeing the physical page.
   Used by the SPT destructor; pagedir_destroy() frees the physical page. */
void
frame_remove (void *kpage)
{
  lock_acquire (&frame_lock);

  for (struct list_elem *e = list_begin (&frame_table);
       e != list_end (&frame_table);
       e = list_next (e))
    {
      struct frame_entry *fte = list_entry (e, struct frame_entry, elem);
      if (fte->kpage == kpage)
        {
          if (clock_hand == e)
            clock_hand = list_next (e);
          list_remove (e);
          free (fte);
          break;
        }
    }

  lock_release (&frame_lock);
}

/* Marks the frame as evictable (pinned=false). */
void
frame_unpin (void *kpage)
{
  lock_acquire (&frame_lock);

  for (struct list_elem *e = list_begin (&frame_table);
       e != list_end (&frame_table);
       e = list_next (e))
    {
      struct frame_entry *fte = list_entry (e, struct frame_entry, elem);
      if (fte->kpage == kpage)
        {
          fte->pinned = false;
          break;
        }
    }

  lock_release (&frame_lock);
}
