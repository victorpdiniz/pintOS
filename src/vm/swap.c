#include "vm/swap.h"
#include <bitmap.h>
#include <debug.h>
#include "devices/block.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

#define SECTORS_PER_PAGE  (PGSIZE / BLOCK_SECTOR_SIZE)

static struct block  *swap_block;
static struct bitmap *swap_bitmap;   /* one bit per page slot */
static struct lock    swap_lock;

void
swap_init (void)
{
  swap_block = block_get_role (BLOCK_SWAP);
  if (swap_block == NULL)
    return;
  size_t num_slots = block_size (swap_block) / SECTORS_PER_PAGE;
  swap_bitmap = bitmap_create (num_slots);
  ASSERT (swap_bitmap != NULL);
  lock_init (&swap_lock);
}

/* Writes KPAGE to a free swap slot; returns the starting sector number. */
size_t
swap_write (void *kpage)
{
  ASSERT (swap_block != NULL);
  ASSERT (swap_bitmap != NULL);

  lock_acquire (&swap_lock);
  size_t slot = bitmap_scan_and_flip (swap_bitmap, 0, 1, false);
  lock_release (&swap_lock);

  if (slot == BITMAP_ERROR)
    PANIC ("swap: no free slot");

  size_t base = slot * SECTORS_PER_PAGE;
  for (size_t i = 0; i < SECTORS_PER_PAGE; i++)
    block_write (swap_block, base + i,
                 (uint8_t *) kpage + i * BLOCK_SECTOR_SIZE);
  return base;
}

/* Reads the page at SECTOR into KPAGE. */
void
swap_read (size_t sector, void *kpage)
{
  ASSERT (swap_block != NULL);
  for (size_t i = 0; i < SECTORS_PER_PAGE; i++)
    block_read (swap_block, sector + i,
                (uint8_t *) kpage + i * BLOCK_SECTOR_SIZE);
}

/* Marks the page slot starting at SECTOR as free. */
void
swap_free (size_t sector)
{
  if (swap_bitmap == NULL)
    return;
  size_t slot = sector / SECTORS_PER_PAGE;
  lock_acquire (&swap_lock);
  bitmap_set (swap_bitmap, slot, false);
  lock_release (&swap_lock);
}
