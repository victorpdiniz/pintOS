#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"

struct dir
  {
    struct inode *inode;
    off_t pos;
  };

struct dir_entry
  {
    block_sector_t inode_sector;
    char name[NAME_MAX + 1];
    bool in_use;
  };

bool
dir_create (block_sector_t sector, block_sector_t parent_sector,
            size_t entry_cnt)
{
  if (!inode_create (sector, entry_cnt * sizeof (struct dir_entry), true))
    return false;

  struct dir *dir = dir_open (inode_open (sector));
  if (dir == NULL)
    return false;

  bool ok = (dir_add (dir, ".", sector)
             && dir_add (dir, "..", parent_sector));
  dir_close (dir);
  return ok;
}

struct dir *
dir_open (struct inode *inode)
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos   = 0;
      return dir;
    }
  inode_close (inode);
  free (dir);
  return NULL;
}

struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

struct dir *
dir_reopen (struct dir *dir)
{
  return dir_open (inode_reopen (dir->inode));
}

void
dir_close (struct dir *dir)
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

struct inode *
dir_get_inode (struct dir *dir)
{
  return dir->inode;
}

static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp)
{
  struct dir_entry e;
  size_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0;
       inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e)
    if (e.in_use && !strcmp (name, e.name))
      {
        if (ep   != NULL) *ep   = e;
        if (ofsp != NULL) *ofsp = ofs;
        return true;
      }
  return false;
}

bool
dir_lookup (const struct dir *dir, const char *name, struct inode **inode)
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;

  return *inode != NULL;
}

bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  if (lookup (dir, name, NULL, NULL))
    goto done;

  for (ofs = 0;
       inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e)
    if (!e.in_use)
      break;

  e.in_use       = true;
  e.inode_sector = inode_sector;
  strlcpy (e.name, name, sizeof e.name);
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

 done:
  return success;
}

bool
dir_remove (struct dir *dir, const char *name)
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (!lookup (dir, name, &e, &ofs))
    goto done;

  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  /* Do not remove a non-empty directory. */
  if (inode_is_dir (inode))
    {
      struct dir *sub = dir_open (inode_reopen (inode));
      bool empty = sub != NULL && dir_is_empty (sub);
      dir_close (sub);
      if (!empty)
        goto done;
    }

  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e)
    goto done;

  inode_remove (inode);
  success = true;

 done:
  inode_close (inode);
  return success;
}

/* Skip "." and ".." when reading directory entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e)
    {
      dir->pos += sizeof e;
      if (e.in_use
          && strcmp (e.name, ".") != 0
          && strcmp (e.name, "..") != 0)
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          return true;
        }
    }
  return false;
}

bool
dir_is_empty (struct dir *dir)
{
  struct dir_entry e;
  off_t ofs;

  for (ofs = 0;
       inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e)
    if (e.in_use
        && strcmp (e.name, ".") != 0
        && strcmp (e.name, "..") != 0)
      return false;

  return true;
}
