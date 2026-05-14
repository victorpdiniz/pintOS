#include "vm/frame.h"
#include <list.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"

static struct list frame_table;
static struct lock frame_table_lock;

void
frame_table_init (void)
{
  list_init (&frame_table);
  lock_init (&frame_table_lock);
}

/* Allocates a user frame, registers it in the frame table, and
   returns its kernel virtual address. Returns NULL on failure. */
void *
frame_alloc (enum palloc_flags flags, void *upage)
{
  void *kpage = palloc_get_page (flags | PAL_USER);
  if (kpage == NULL)
    return NULL;

  struct frame_table_entry *fte = malloc (sizeof *fte);
  if (fte == NULL)
    {
      palloc_free_page (kpage);
      return NULL;
    }

  fte->kpage = kpage;
  fte->upage = upage;
  fte->owner = thread_current ();

  lock_acquire (&frame_table_lock);
  list_push_back (&frame_table, &fte->elem);
  lock_release (&frame_table_lock);

  return kpage;
}

/* Removes kpage's entry from the frame table and frees the page. */
void
frame_free (void *kpage)
{
  struct frame_table_entry *fte = NULL;

  lock_acquire (&frame_table_lock);
  for (struct list_elem *e = list_begin (&frame_table);
       e != list_end (&frame_table);
       e = list_next (e))
    {
      struct frame_table_entry *f = list_entry (e, struct frame_table_entry, elem);
      if (f->kpage == kpage)
        {
          list_remove (e);
          fte = f;
          break;
        }
    }
  lock_release (&frame_table_lock);

  if (fte != NULL)
    free (fte);
  palloc_free_page (kpage);
}

/* Removes all frame table entries owned by thread T without freeing
   the underlying pages (the caller, typically pagedir_destroy, handles that). */
void
frame_table_remove_process (struct thread *t)
{
  lock_acquire (&frame_table_lock);
  struct list_elem *e = list_begin (&frame_table);
  while (e != list_end (&frame_table))
    {
      struct frame_table_entry *fte = list_entry (e, struct frame_table_entry, elem);
      struct list_elem *next = list_next (e);
      if (fte->owner == t)
        {
          list_remove (e);
          free (fte);
        }
      e = next;
    }
  lock_release (&frame_table_lock);
}
