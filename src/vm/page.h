#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include <list.h>
#include <stdbool.h>
#include <stddef.h>
#include "filesys/off_t.h"

struct file;    /* forward declaration */
struct thread;  /* forward declaration */

/* Describes where the data for a virtual page currently lives. */
enum page_location
{
  PAGE_ZERO,    /* zero-filled (BSS or stack growth) */
  PAGE_FILE,    /* backed by a file (executable or mmap) */
  PAGE_SWAP,    /* evicted to the swap device */
  PAGE_FRAME,   /* resident in a physical frame */
};

/* One entry per user virtual page in the per-process SPT. */
struct sup_page_entry
{
  struct hash_elem hash_elem;   /* node in thread's spage_table */
  void *upage;                  /* user virtual address (page-aligned) */

  enum page_location location;
  bool writable;
  bool dirty;                   /* accumulated dirty bit for write-back */

  bool is_mmap;                 /* true if this page belongs to an mmap */
  int  mapid;                   /* mmap ID (when is_mmap is true) */
  struct file *file;            /* source file */
  off_t file_offset;            /* byte offset into file */
  size_t read_bytes;            /* bytes to read from file */
  size_t zero_bytes;            /* bytes to zero after file data */

  size_t swap_sector;           /* starting sector in swap device */
  void  *kpage;                 /* kernel address of physical frame */
};

/* Tracks one mmap() mapping per open region. */
struct mmap_entry
{
  int mapid;
  struct file *file;     /* independent reference (file_reopen) */
  void *addr;            /* base address of mapped region */
  size_t page_count;     /* number of pages in the region */
  struct list_elem elem;
};

void spt_init    (struct hash *spt);
void spt_destroy (struct hash *spt);
struct sup_page_entry *spt_find   (struct hash *spt, void *upage);
bool  spt_insert  (struct hash *spt, struct sup_page_entry *spte);
bool  spt_load_page (struct sup_page_entry *spte);

void munmap_entry (struct thread *t, struct mmap_entry *me);
void munmap_all   (struct thread *t);

#endif /* vm/page.h */
