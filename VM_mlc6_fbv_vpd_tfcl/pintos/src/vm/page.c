#include "vm/page.h"
#include <hash.h>
#include <string.h>
#include <debug.h>
#include "filesys/file.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/swap.h"

extern struct lock filesys_lock;

static unsigned page_hash (const struct hash_elem *e, void *aux UNUSED);
static bool page_less (const struct hash_elem *a, const struct hash_elem *b,
                       void *aux UNUSED);
static void page_free_entry (struct hash_elem *e, void *aux UNUSED);

void
spt_init (struct hash *spt)
{
  hash_init (spt, page_hash, page_less, NULL);
}

static unsigned
page_hash (const struct hash_elem *e, void *aux UNUSED)
{
  const struct sup_page_entry *spte =
    hash_entry (e, struct sup_page_entry, hash_elem);
  return hash_bytes (&spte->upage, sizeof spte->upage);
}

static bool
page_less (const struct hash_elem *a, const struct hash_elem *b,
           void *aux UNUSED)
{
  return hash_entry (a, struct sup_page_entry, hash_elem)->upage
       < hash_entry (b, struct sup_page_entry, hash_elem)->upage;
}

struct sup_page_entry *
spt_find (struct hash *spt, void *upage)
{
  struct sup_page_entry key;
  key.upage = upage;
  struct hash_elem *e = hash_find (spt, &key.hash_elem);
  return e ? hash_entry (e, struct sup_page_entry, hash_elem) : NULL;
}

/* Inserts SPTE into SPT.  Returns true on success, false if upage
   already has an entry. */
bool
spt_insert (struct hash *spt, struct sup_page_entry *spte)
{
  return hash_insert (spt, &spte->hash_elem) == NULL;
}

/* Loads the page described by SPTE into a physical frame and installs
   it in the current thread's page directory.  Returns true on success. */
bool
spt_load_page (struct sup_page_entry *spte)
{
  ASSERT (spte != NULL);
  ASSERT (spte->location != PAGE_FRAME);

  enum palloc_flags flags = PAL_USER;
  if (spte->location == PAGE_ZERO)
    flags |= PAL_ZERO;

  void *kpage = frame_alloc (spte, flags);
  if (kpage == NULL)
    return false;

  switch (spte->location)
    {
    case PAGE_ZERO:
      memset (kpage, 0, PGSIZE);
      break;

    case PAGE_FILE:
      {
        int got = (int) file_read_at (spte->file, kpage,
                                      spte->read_bytes, spte->file_offset);
        if (got != (int) spte->read_bytes)
          {
            frame_free (kpage);
            return false;
          }
        memset ((uint8_t *) kpage + spte->read_bytes, 0, spte->zero_bytes);
      }
      break;

    case PAGE_SWAP:
      swap_read (spte->swap_sector, kpage);
      swap_free (spte->swap_sector);
      break;

    default:
      frame_free (kpage);
      return false;
    }

  if (!pagedir_set_page (thread_current ()->pagedir,
                         spte->upage, kpage, spte->writable))
    {
      frame_free (kpage);
      return false;
    }

  spte->kpage    = kpage;
  spte->location = PAGE_FRAME;
  frame_unpin (kpage);
  return true;
}

/* Called by hash_destroy for each SPT entry at process exit.
   PAGE_FRAME entries: remove from frame table (pagedir_destroy frees
   the physical page).  PAGE_SWAP entries: release the swap slot. */
static void
page_free_entry (struct hash_elem *e, void *aux UNUSED)
{
  struct sup_page_entry *spte =
    hash_entry (e, struct sup_page_entry, hash_elem);

  if (spte->location == PAGE_FRAME)
    frame_remove (spte->kpage);
  else if (spte->location == PAGE_SWAP)
    swap_free (spte->swap_sector);

  free (spte);
}

void
spt_destroy (struct hash *spt)
{
  hash_destroy (spt, page_free_entry);
}

/* Unmaps one mmap region, writing back dirty pages and freeing all
   resources.  The mmap_entry is removed from the thread's mmap_list. */
void
munmap_entry (struct thread *t, struct mmap_entry *me)
{
  for (size_t i = 0; i < me->page_count; i++)
    {
      void *upage = (uint8_t *) me->addr + i * PGSIZE;
      struct sup_page_entry *spte = spt_find (&t->spage_table, upage);
      if (spte == NULL)
        continue;

      if (spte->location == PAGE_FRAME)
        {
          bool dirty = pagedir_is_dirty (t->pagedir, upage) || spte->dirty;
          if (dirty)
            {
              lock_acquire (&filesys_lock);
              file_write_at (me->file, spte->kpage,
                             spte->read_bytes, spte->file_offset);
              lock_release (&filesys_lock);
            }
          pagedir_clear_page (t->pagedir, upage);
          frame_free (spte->kpage);
        }
      else if (spte->location == PAGE_SWAP)
        {
          if (spte->dirty)
            {
              void *buf = palloc_get_page (0);
              if (buf != NULL)
                {
                  swap_read (spte->swap_sector, buf);
                  lock_acquire (&filesys_lock);
                  file_write_at (me->file, buf,
                                 spte->read_bytes, spte->file_offset);
                  lock_release (&filesys_lock);
                  palloc_free_page (buf);
                }
              else
                swap_free (spte->swap_sector);
            }
          else
            swap_free (spte->swap_sector);
        }

      hash_delete (&t->spage_table, &spte->hash_elem);
      free (spte);
    }

  lock_acquire (&filesys_lock);
  file_close (me->file);
  lock_release (&filesys_lock);

  list_remove (&me->elem);
  free (me);
}

/* Unmaps all mmap regions for thread T. */
void
munmap_all (struct thread *t)
{
  while (!list_empty (&t->mmap_list))
    {
      struct mmap_entry *me =
        list_entry (list_begin (&t->mmap_list), struct mmap_entry, elem);
      munmap_entry (t, me);
    }
}
