#include "filesys/cache.h"
#include <string.h>
#include "filesys/filesys.h"
#include "threads/synch.h"

#define CACHE_SIZE 64

struct cache_slot
  {
    bool valid;
    bool dirty;
    int  clock_bit;
    block_sector_t sector;
    uint8_t data[BLOCK_SECTOR_SIZE];
  };

static struct cache_slot cache[CACHE_SIZE];
static struct lock cache_lock;
static int clock_hand;

void
cache_init (void)
{
  lock_init (&cache_lock);
  clock_hand = 0;
  memset (cache, 0, sizeof cache);
}

static struct cache_slot *
cache_find (block_sector_t sector)
{
  for (int i = 0; i < CACHE_SIZE; i++)
    if (cache[i].valid && cache[i].sector == sector)
      return &cache[i];
  return NULL;
}

/* Clock eviction: find an unreferenced slot, flush if dirty. */
static struct cache_slot *
cache_evict (void)
{
  while (true)
    {
      struct cache_slot *s = &cache[clock_hand];
      clock_hand = (clock_hand + 1) % CACHE_SIZE;

      if (!s->valid)
        return s;

      if (s->clock_bit)
        {
          s->clock_bit = 0;
          continue;
        }

      if (s->dirty)
        {
          block_write (fs_device, s->sector, s->data);
          s->dirty = false;
        }
      s->valid = false;
      return s;
    }
}

void
cache_read (block_sector_t sector, void *buffer)
{
  lock_acquire (&cache_lock);
  struct cache_slot *s = cache_find (sector);
  if (s == NULL)
    {
      s = cache_evict ();
      s->sector    = sector;
      s->valid     = true;
      s->dirty     = false;
      s->clock_bit = 1;
      block_read (fs_device, sector, s->data);
    }
  else
    s->clock_bit = 1;
  memcpy (buffer, s->data, BLOCK_SECTOR_SIZE);
  lock_release (&cache_lock);
}

void
cache_write (block_sector_t sector, const void *buffer)
{
  lock_acquire (&cache_lock);
  struct cache_slot *s = cache_find (sector);
  if (s == NULL)
    {
      s = cache_evict ();
      s->sector    = sector;
      s->valid     = true;
      s->clock_bit = 1;
    }
  else
    s->clock_bit = 1;
  memcpy (s->data, buffer, BLOCK_SECTOR_SIZE);
  s->dirty = true;
  lock_release (&cache_lock);
}

void
cache_flush (void)
{
  lock_acquire (&cache_lock);
  for (int i = 0; i < CACHE_SIZE; i++)
    if (cache[i].valid && cache[i].dirty)
      {
        block_write (fs_device, cache[i].sector, cache[i].data);
        cache[i].dirty = false;
      }
  lock_release (&cache_lock);
}
