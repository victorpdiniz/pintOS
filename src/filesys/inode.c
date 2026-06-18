#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/cache.h"
#include "threads/malloc.h"
#include "threads/synch.h"

#define INODE_MAGIC      0x494e4f44
#define INODE_DIRECT_CNT 12
#define INDIRECT_CNT     (BLOCK_SECTOR_SIZE / sizeof (block_sector_t))  /* 128 */

/* On-disk inode — must be exactly BLOCK_SECTOR_SIZE bytes. */
struct inode_disk
  {
    off_t length;                              /* File size in bytes. */
    unsigned magic;                            /* Magic number. */
    uint32_t is_dir;                           /* 1 if directory, 0 if file. */
    block_sector_t direct[INODE_DIRECT_CNT];   /* 12 direct data blocks. */
    block_sector_t indirect;                   /* Singly-indirect block. */
    block_sector_t doubly_indirect;            /* Doubly-indirect block. */
    uint32_t unused[111];                      /* Padding to 512 bytes. */
  };

static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode
  {
    struct list_elem elem;
    block_sector_t sector;
    int open_cnt;
    bool removed;
    int deny_write_cnt;
    struct lock lock;          /* serializes inode_write_at growth */
    struct inode_disk data;
  };

static struct list open_inodes;

void
inode_init (void)
{
  list_init (&open_inodes);
}

/* ---------- block index helpers ---------- */

/* Map logical block index IDX to its on-disk sector using DISK.
   Returns (block_sector_t)-1 if IDX is beyond the allocated range. */
static block_sector_t
index_to_sector (const struct inode_disk *disk, size_t idx)
{
  /* Direct */
  if (idx < INODE_DIRECT_CNT)
    return disk->direct[idx];
  idx -= INODE_DIRECT_CNT;

  /* Singly-indirect */
  if (idx < INDIRECT_CNT)
    {
      if (disk->indirect == 0)
        return (block_sector_t) -1;
      block_sector_t buf[INDIRECT_CNT];
      cache_read (disk->indirect, buf);
      return buf[idx];
    }
  idx -= INDIRECT_CNT;

  /* Doubly-indirect */
  if (idx < INDIRECT_CNT * INDIRECT_CNT)
    {
      if (disk->doubly_indirect == 0)
        return (block_sector_t) -1;
      block_sector_t dbl[INDIRECT_CNT];
      cache_read (disk->doubly_indirect, dbl);
      size_t l1 = idx / INDIRECT_CNT;
      size_t l2 = idx % INDIRECT_CNT;
      if (dbl[l1] == 0)
        return (block_sector_t) -1;
      block_sector_t indir[INDIRECT_CNT];
      cache_read (dbl[l1], indir);
      return indir[l2];
    }

  return (block_sector_t) -1;
}

static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);
  if (pos < 0 || pos >= inode->data.length)
    return (block_sector_t) -1;
  return index_to_sector (&inode->data, (size_t) (pos / BLOCK_SECTOR_SIZE));
}

/* ---------- block allocation ---------- */

/* Allocate and zero one sector, returning its number or 0 on failure. */
static block_sector_t
alloc_zero_sector (void)
{
  block_sector_t sec;
  if (!free_map_allocate (1, &sec))
    return 0;
  static char zeros[BLOCK_SECTOR_SIZE];
  cache_write (sec, zeros);
  return sec;
}

/* Write sector pointer IDX into DISK, allocating intermediate blocks
   as needed.  Returns true on success. */
static bool
inode_set_block (struct inode_disk *disk, size_t idx, block_sector_t sec)
{
  /* Direct */
  if (idx < INODE_DIRECT_CNT)
    {
      disk->direct[idx] = sec;
      return true;
    }
  idx -= INODE_DIRECT_CNT;

  /* Singly-indirect */
  if (idx < INDIRECT_CNT)
    {
      if (disk->indirect == 0)
        {
          disk->indirect = alloc_zero_sector ();
          if (disk->indirect == 0)
            return false;
        }
      block_sector_t buf[INDIRECT_CNT];
      cache_read (disk->indirect, buf);
      buf[idx] = sec;
      cache_write (disk->indirect, buf);
      return true;
    }
  idx -= INDIRECT_CNT;

  /* Doubly-indirect */
  if (idx < INDIRECT_CNT * INDIRECT_CNT)
    {
      if (disk->doubly_indirect == 0)
        {
          disk->doubly_indirect = alloc_zero_sector ();
          if (disk->doubly_indirect == 0)
            return false;
        }
      block_sector_t dbl[INDIRECT_CNT];
      cache_read (disk->doubly_indirect, dbl);
      size_t l1 = idx / INDIRECT_CNT;
      size_t l2 = idx % INDIRECT_CNT;
      if (dbl[l1] == 0)
        {
          dbl[l1] = alloc_zero_sector ();
          if (dbl[l1] == 0)
            return false;
          cache_write (disk->doubly_indirect, dbl);
        }
      block_sector_t indir[INDIRECT_CNT];
      cache_read (dbl[l1], indir);
      indir[l2] = sec;
      cache_write (dbl[l1], indir);
      return true;
    }

  return false;  /* file too large */
}

/* Grow DISK to hold NEW_LENGTH bytes, allocating new sectors as needed.
   Does NOT write DISK back to the block device — caller does that. */
static bool
inode_alloc (struct inode_disk *disk, off_t new_length)
{
  size_t old_blocks = bytes_to_sectors (disk->length);
  size_t new_blocks = bytes_to_sectors (new_length);

  for (size_t i = old_blocks; i < new_blocks; i++)
    {
      block_sector_t sec = alloc_zero_sector ();
      if (sec == 0 || !inode_set_block (disk, i, sec))
        {
          if (sec != 0)
            free_map_release (sec, 1);
          return false;
        }
    }
  disk->length = new_length;
  return true;
}

/* ---------- inode release ---------- */

static void
inode_free_blocks (struct inode_disk *disk)
{
  size_t n = bytes_to_sectors (disk->length);

  /* Direct */
  for (size_t i = 0; i < INODE_DIRECT_CNT && i < n; i++)
    if (disk->direct[i])
      free_map_release (disk->direct[i], 1);

  /* Singly-indirect */
  if (n > INODE_DIRECT_CNT && disk->indirect)
    {
      block_sector_t buf[INDIRECT_CNT];
      cache_read (disk->indirect, buf);
      size_t cnt = n - INODE_DIRECT_CNT;
      if (cnt > INDIRECT_CNT) cnt = INDIRECT_CNT;
      for (size_t i = 0; i < cnt; i++)
        if (buf[i])
          free_map_release (buf[i], 1);
      free_map_release (disk->indirect, 1);
    }

  /* Doubly-indirect */
  if (n > INODE_DIRECT_CNT + INDIRECT_CNT && disk->doubly_indirect)
    {
      block_sector_t dbl[INDIRECT_CNT];
      cache_read (disk->doubly_indirect, dbl);
      size_t rem = n - INODE_DIRECT_CNT - INDIRECT_CNT;
      for (size_t l1 = 0; l1 < INDIRECT_CNT && rem > 0; l1++)
        {
          if (dbl[l1])
            {
              block_sector_t indir[INDIRECT_CNT];
              cache_read (dbl[l1], indir);
              size_t cnt = rem < INDIRECT_CNT ? rem : INDIRECT_CNT;
              for (size_t l2 = 0; l2 < cnt; l2++)
                if (indir[l2])
                  free_map_release (indir[l2], 1);
              free_map_release (dbl[l1], 1);
              rem -= cnt;
            }
        }
      free_map_release (disk->doubly_indirect, 1);
    }
}

/* ---------- public API ---------- */

bool
inode_create (block_sector_t sector, off_t length, bool is_dir)
{
  ASSERT (length >= 0);
  ASSERT (sizeof (struct inode_disk) == BLOCK_SECTOR_SIZE);

  struct inode_disk *disk = calloc (1, sizeof *disk);
  if (disk == NULL)
    return false;

  disk->magic  = INODE_MAGIC;
  disk->is_dir = is_dir ? 1 : 0;

  bool ok = inode_alloc (disk, length);
  if (ok)
    cache_write (sector, disk);

  free (disk);
  return ok;
}

struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector)
        {
          inode_reopen (inode);
          return inode;
        }
    }

  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  list_push_front (&open_inodes, &inode->elem);
  inode->sector        = sector;
  inode->open_cnt      = 1;
  inode->deny_write_cnt = 0;
  inode->removed       = false;
  lock_init (&inode->lock);
  cache_read (inode->sector, &inode->data);
  return inode;
}

struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

void
inode_close (struct inode *inode)
{
  if (inode == NULL)
    return;

  if (--inode->open_cnt == 0)
    {
      list_remove (&inode->elem);

      if (inode->removed)
        {
          inode_free_blocks (&inode->data);
          free_map_release (inode->sector, 1);
        }
      free (inode);
    }
}

void
inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0)
    {
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      if (sector_idx == (block_sector_t) -1)
        break;

      int sector_ofs  = offset % BLOCK_SECTOR_SIZE;
      off_t inode_left = inode_length (inode) - offset;
      int sector_left  = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left     = inode_left < sector_left ? (int) inode_left : sector_left;
      int chunk_size   = size < min_left ? (int) size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          cache_read (sector_idx, buffer + bytes_read);
        }
      else
        {
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          cache_read (sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }

      size       -= chunk_size;
      offset     += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);
  return bytes_read;
}

off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  /* Grow if necessary. */
  if (offset + size > inode->data.length)
    {
      lock_acquire (&inode->lock);
      if (offset + size > inode->data.length)
        {
          if (!inode_alloc (&inode->data, offset + size))
            {
              lock_release (&inode->lock);
              return 0;
            }
          cache_write (inode->sector, &inode->data);
        }
      lock_release (&inode->lock);
    }

  while (size > 0)
    {
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      if (sector_idx == (block_sector_t) -1)
        break;

      int sector_ofs  = offset % BLOCK_SECTOR_SIZE;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int chunk_size  = size < sector_left ? (int) size : sector_left;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          cache_write (sector_idx, buffer + bytes_written);
        }
      else
        {
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          if (sector_ofs > 0 || chunk_size < sector_left)
            cache_read (sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          cache_write (sector_idx, bounce);
        }

      size           -= chunk_size;
      offset         += chunk_size;
      bytes_written  += chunk_size;
    }
  free (bounce);
  return bytes_written;
}

void
inode_deny_write (struct inode *inode)
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

void
inode_allow_write (struct inode *inode)
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}

bool
inode_is_dir (const struct inode *inode)
{
  return inode != NULL && inode->data.is_dir != 0;
}
