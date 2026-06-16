#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"
#include "threads/thread.h"
#include "threads/malloc.h"

struct block *fs_device;

static void do_format (void);

/* ---------- path resolution ---------- */

/* Splits PATH into a parent directory and a final component.
   Returns the opened parent dir (caller must close), or NULL on error.
   The last component is stored in NAME (must be NAME_MAX+1 bytes). */
static struct dir *
resolve_parent (const char *path, char name[NAME_MAX + 1])
{
  if (path == NULL || *path == '\0')
    return NULL;

  size_t len = strlen (path);
  char *buf = malloc (len + 1);
  if (buf == NULL)
    return NULL;
  strlcpy (buf, path, len + 1);

  /* Choose starting directory. */
  struct dir *dir;
  char *p = buf;
  if (*p == '/')
    {
      dir = dir_open_root ();
      p++;
    }
  else
    {
      block_sector_t cwd = thread_current ()->cwd_sector;
      dir = dir_open (inode_open (cwd));
    }

  if (dir == NULL)
    {
      free (buf);
      return NULL;
    }

  /* Walk all but the last component. */
  char *save;
  char *tok = strtok_r (p, "/", &save);
  char *next = (tok != NULL) ? strtok_r (NULL, "/", &save) : NULL;

  while (tok != NULL && next != NULL)
    {
      if (strlen (tok) > NAME_MAX)
        {
          dir_close (dir);
          free (buf);
          return NULL;
        }
      struct inode *inode = NULL;
      if (!dir_lookup (dir, tok, &inode) || !inode_is_dir (inode))
        {
          inode_close (inode);
          dir_close (dir);
          free (buf);
          return NULL;
        }
      dir_close (dir);
      dir = dir_open (inode);
      if (dir == NULL)
        {
          free (buf);
          return NULL;
        }
      tok  = next;
      next = strtok_r (NULL, "/", &save);
    }

  /* tok is now the final component. */
  if (tok == NULL || strlen (tok) == 0 || strlen (tok) > NAME_MAX)
    {
      dir_close (dir);
      free (buf);
      return NULL;
    }
  strlcpy (name, tok, NAME_MAX + 1);
  free (buf);
  return dir;
}

/* ---------- public filesystem API ---------- */

void
filesys_init (bool format)
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  cache_init ();
  inode_init ();
  free_map_init ();

  if (format)
    do_format ();

  free_map_open ();
}

void
filesys_done (void)
{
  cache_flush ();
  free_map_close ();
}

bool
filesys_create (const char *name, off_t initial_size)
{
  char fname[NAME_MAX + 1];
  struct dir *dir = resolve_parent (name, fname);
  if (dir == NULL)
    return false;

  block_sector_t inode_sector = 0;
  bool success = (free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, false)
                  && dir_add (dir, fname, inode_sector));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
  dir_close (dir);
  return success;
}

/* Open a path and return its inode (caller closes it). */
struct inode *
filesys_open_inode (const char *name)
{
  if (name == NULL || *name == '\0')
    return NULL;

  /* Special case: open root directory. */
  if (strcmp (name, "/") == 0)
    return inode_open (ROOT_DIR_SECTOR);

  char fname[NAME_MAX + 1];
  struct dir *dir = resolve_parent (name, fname);
  if (dir == NULL)
    return NULL;

  struct inode *inode = NULL;
  dir_lookup (dir, fname, &inode);
  dir_close (dir);
  return inode;
}

struct file *
filesys_open (const char *name)
{
  struct inode *inode = filesys_open_inode (name);
  if (inode == NULL || inode_is_dir (inode))
    {
      inode_close (inode);
      return NULL;
    }
  return file_open (inode);
}

bool
filesys_remove (const char *name)
{
  char fname[NAME_MAX + 1];
  struct dir *dir = resolve_parent (name, fname);
  if (dir == NULL)
    return false;

  bool success = dir_remove (dir, fname);
  dir_close (dir);
  return success;
}

bool
filesys_mkdir (const char *name)
{
  char dname[NAME_MAX + 1];
  struct dir *parent = resolve_parent (name, dname);
  if (parent == NULL)
    return false;

  block_sector_t inode_sector = 0;
  block_sector_t parent_sector = inode_get_inumber (dir_get_inode (parent));

  bool success = (free_map_allocate (1, &inode_sector)
                  && dir_create (inode_sector, parent_sector, 16)
                  && dir_add (parent, dname, inode_sector));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
  dir_close (parent);
  return success;
}

bool
filesys_chdir (const char *name)
{
  if (name == NULL || *name == '\0')
    return false;

  struct inode *inode;
  if (strcmp (name, "/") == 0)
    {
      inode = inode_open (ROOT_DIR_SECTOR);
    }
  else
    {
      char dname[NAME_MAX + 1];
      struct dir *parent = resolve_parent (name, dname);
      if (parent == NULL)
        return false;
      dir_lookup (parent, dname, &inode);
      dir_close (parent);
    }

  if (inode == NULL || !inode_is_dir (inode))
    {
      inode_close (inode);
      return false;
    }

  thread_current ()->cwd_sector = inode_get_inumber (inode);
  inode_close (inode);
  return true;
}

static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
