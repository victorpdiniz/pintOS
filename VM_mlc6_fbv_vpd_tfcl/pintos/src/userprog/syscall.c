#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/malloc.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "lib/kernel/stdio.h"
#ifdef VM
#include "vm/page.h"
#endif

typedef uint32_t pid_t;
typedef int mapid_t;

struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);
static void exit_with_status (int status);
static int memread_user (const void *src, void *dst, size_t bytes);
static int32_t get_user (const uint8_t *uaddr);
static bool put_user (uint8_t *udst, uint8_t byte);
static struct file *get_file_from_fd (int fd);
static void validate_string (const char *str);
static void validate_buffer (const void *buf, unsigned size, bool writable);
static pid_t sys_exec (const char *cmdline);
#ifdef VM
static mapid_t sys_mmap (int fd, void *addr);
static void    sys_munmap (mapid_t mapid);
#endif

void
syscall_init (void)
{
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f)
{
#ifdef VM
  thread_current ()->esp_saved = f->esp;
#endif

  int syscall_num;

  if (memread_user (f->esp, &syscall_num, sizeof (syscall_num)) == -1)
    exit_with_status (-1);

  switch (syscall_num)
    {
    case SYS_HALT:
      shutdown_power_off ();
      break;

    case SYS_EXIT:
      {
        int status;
        if (memread_user (f->esp + 4, &status, sizeof (status)) == -1)
          exit_with_status (-1);
        exit_with_status (status);
        break;
      }

    case SYS_EXEC:
      {
        const char *cmdline;
        if (memread_user (f->esp + 4, &cmdline, sizeof (cmdline)) == -1)
          exit_with_status (-1);
        f->eax = sys_exec (cmdline);
        break;
      }

    case SYS_WAIT:
      {
        tid_t tid;
        if (memread_user (f->esp + 4, &tid, sizeof (tid)) == -1)
          exit_with_status (-1);
        f->eax = process_wait (tid);
        break;
      }

    case SYS_CREATE:
      {
        const char *file;
        unsigned initial_size;
        if (memread_user (f->esp + 4, &file, sizeof (file)) == -1 ||
            memread_user (f->esp + 8, &initial_size, sizeof (initial_size)) == -1)
          exit_with_status (-1);
        if (file == NULL)
          exit_with_status (-1);
        validate_string (file);
        lock_acquire (&filesys_lock);
        bool ok = filesys_create (file, initial_size);
        lock_release (&filesys_lock);
        f->eax = ok;
        break;
      }

    case SYS_REMOVE:
      {
        const char *file;
        if (memread_user (f->esp + 4, &file, sizeof (file)) == -1)
          exit_with_status (-1);
        if (file == NULL)
          exit_with_status (-1);
        validate_string (file);
        lock_acquire (&filesys_lock);
        bool ok = filesys_remove (file);
        lock_release (&filesys_lock);
        f->eax = ok;
        break;
      }

    case SYS_OPEN:
      {
        const char *filename;
        if (memread_user (f->esp + 4, &filename, sizeof (filename)) == -1)
          exit_with_status (-1);
        if (filename == NULL)
          exit_with_status (-1);
        validate_string (filename);

        lock_acquire (&filesys_lock);
        struct file *opened = filesys_open (filename);
        lock_release (&filesys_lock);

        if (opened == NULL)
          {
            f->eax = -1;
            break;
          }

        struct thread *cur = thread_current ();
        int fd = -1;
        for (int i = 2; i < MAX_FDS; i++)
          {
            if (cur->fd_table[i] == NULL)
              {
                cur->fd_table[i] = opened;
                fd = i;
                break;
              }
          }
        if (fd == -1)
          {
            lock_acquire (&filesys_lock);
            file_close (opened);
            lock_release (&filesys_lock);
          }
        f->eax = fd;
        break;
      }

    case SYS_FILESIZE:
      {
        int fd;
        if (memread_user (f->esp + 4, &fd, sizeof (fd)) == -1)
          exit_with_status (-1);
        struct file *file = get_file_from_fd (fd);
        if (file == NULL)
          {
            f->eax = -1;
            break;
          }
        lock_acquire (&filesys_lock);
        f->eax = file_length (file);
        lock_release (&filesys_lock);
        break;
      }

    case SYS_READ:
      {
        int fd;
        void *buffer;
        unsigned size;
        if (memread_user (f->esp + 4, &fd, sizeof (fd)) == -1 ||
            memread_user (f->esp + 8, &buffer, sizeof (buffer)) == -1 ||
            memread_user (f->esp + 12, &size, sizeof (size)) == -1)
          exit_with_status (-1);
        validate_buffer (buffer, size, true);  /* buffer must be writable */

        if (fd == 0)
          {
            uint8_t *buf = buffer;
            for (unsigned i = 0; i < size; i++)
              buf[i] = input_getc ();
            f->eax = size;
          }
        else
          {
            struct file *file = get_file_from_fd (fd);
            if (file == NULL)
              {
                f->eax = -1;
                break;
              }
            lock_acquire (&filesys_lock);
            f->eax = file_read (file, buffer, size);
            lock_release (&filesys_lock);
          }
        break;
      }

    case SYS_WRITE:
      {
        int fd;
        const void *buffer;
        unsigned size;
        if (memread_user (f->esp + 4, &fd, sizeof (fd)) == -1 ||
            memread_user (f->esp + 8, &buffer, sizeof (buffer)) == -1 ||
            memread_user (f->esp + 12, &size, sizeof (size)) == -1)
          exit_with_status (-1);
        validate_buffer (buffer, size, false);  /* only needs to be readable */

        if (fd == 1)
          {
            putbuf (buffer, size);
            f->eax = size;
          }
        else
          {
            struct file *file = get_file_from_fd (fd);
            if (file == NULL)
              {
                f->eax = -1;
                break;
              }
            lock_acquire (&filesys_lock);
            f->eax = file_write (file, buffer, size);
            lock_release (&filesys_lock);
          }
        break;
      }

    case SYS_SEEK:
      {
        int fd;
        unsigned position;
        if (memread_user (f->esp + 4, &fd, sizeof (fd)) == -1 ||
            memread_user (f->esp + 8, &position, sizeof (position)) == -1)
          exit_with_status (-1);
        struct file *file = get_file_from_fd (fd);
        if (file != NULL)
          {
            lock_acquire (&filesys_lock);
            file_seek (file, position);
            lock_release (&filesys_lock);
          }
        break;
      }

    case SYS_TELL:
      {
        int fd;
        if (memread_user (f->esp + 4, &fd, sizeof (fd)) == -1)
          exit_with_status (-1);
        struct file *file = get_file_from_fd (fd);
        if (file == NULL)
          {
            f->eax = -1;
            break;
          }
        lock_acquire (&filesys_lock);
        f->eax = file_tell (file);
        lock_release (&filesys_lock);
        break;
      }

    case SYS_CLOSE:
      {
        int fd;
        if (memread_user (f->esp + 4, &fd, sizeof (fd)) == -1)
          exit_with_status (-1);
        struct file *file = get_file_from_fd (fd);
        if (file == NULL)
          exit_with_status (-1);
        lock_acquire (&filesys_lock);
        file_close (file);
        lock_release (&filesys_lock);
        thread_current ()->fd_table[fd] = NULL;
        break;
      }

#ifdef VM
    case SYS_MMAP:
      {
        int fd;
        void *addr;
        if (memread_user (f->esp + 4, &fd, sizeof (fd)) == -1 ||
            memread_user (f->esp + 8, &addr, sizeof (addr)) == -1)
          exit_with_status (-1);
        f->eax = (uint32_t) sys_mmap (fd, addr);
        break;
      }

    case SYS_MUNMAP:
      {
        mapid_t mapid;
        if (memread_user (f->esp + 4, &mapid, sizeof (mapid)) == -1)
          exit_with_status (-1);
        sys_munmap (mapid);
        break;
      }
#endif

    default:
      exit_with_status (-1);
    }

#ifdef VM
  thread_current ()->esp_saved = NULL;
#endif
}

static void
exit_with_status (int status)
{
  struct thread *cur = thread_current ();
  printf ("%s: exit(%d)\n", cur->name, status);
  cur->exit_status = status;
  thread_exit ();
}

static pid_t
sys_exec (const char *cmdline)
{
  if (cmdline == NULL)
    exit_with_status (-1);
  validate_string (cmdline);

  tid_t tid = process_execute (cmdline);
  if (tid == TID_ERROR)
    return -1;

  /* Find the child's entry and wait for it to finish loading. */
  struct thread *cur = thread_current ();
  struct child_process *cp = NULL;
  for (struct list_elem *e = list_begin (&cur->children);
       e != list_end (&cur->children);
       e = list_next (e))
    {
      struct child_process *c = list_entry (e, struct child_process, elem);
      if (c->tid == tid)
        {
          cp = c;
          break;
        }
    }

  if (cp == NULL)
    return -1;

  sema_down (&cp->load_sema);

  if (!cp->load_success)
    return -1;

  return (pid_t) tid;
}

#ifdef VM
static mapid_t
sys_mmap (int fd, void *addr)
{
  /* Basic validation. */
  if (addr == NULL || pg_ofs (addr) != 0 || fd < 2 || fd >= MAX_FDS)
    return (mapid_t) -1;

  struct thread *cur = thread_current ();
  struct file *orig = get_file_from_fd (fd);
  if (orig == NULL)
    return (mapid_t) -1;

  lock_acquire (&filesys_lock);
  struct file *mmap_file = file_reopen (orig);
  off_t file_len = (mmap_file != NULL) ? file_length (mmap_file) : 0;
  lock_release (&filesys_lock);

  if (mmap_file == NULL || file_len == 0)
    {
      if (mmap_file != NULL)
        {
          lock_acquire (&filesys_lock);
          file_close (mmap_file);
          lock_release (&filesys_lock);
        }
      return (mapid_t) -1;
    }

  size_t page_count = ((size_t) file_len + PGSIZE - 1) / PGSIZE;

  /* Reject if any page would overlap an existing mapping. */
  for (size_t i = 0; i < page_count; i++)
    {
      void *upage = (uint8_t *) addr + i * PGSIZE;
      if (!is_user_vaddr (upage)
          || spt_find (&cur->spage_table, upage) != NULL)
        {
          lock_acquire (&filesys_lock);
          file_close (mmap_file);
          lock_release (&filesys_lock);
          return (mapid_t) -1;
        }
    }

  struct mmap_entry *me = malloc (sizeof *me);
  if (me == NULL)
    {
      lock_acquire (&filesys_lock);
      file_close (mmap_file);
      lock_release (&filesys_lock);
      return (mapid_t) -1;
    }

  me->mapid      = cur->next_mapid++;
  me->file       = mmap_file;
  me->addr       = addr;
  me->page_count = page_count;
  list_push_back (&cur->mmap_list, &me->elem);

  /* Create lazy SPT entries for each page. */
  off_t offset = 0;
  for (size_t i = 0; i < page_count; i++)
    {
      void *upage = (uint8_t *) addr + i * PGSIZE;
      size_t read_bytes = ((size_t) (file_len - offset) >= PGSIZE)
                          ? PGSIZE
                          : (size_t) (file_len - offset);
      size_t zero_bytes = PGSIZE - read_bytes;

      struct sup_page_entry *spte = malloc (sizeof *spte);
      if (spte == NULL)
        {
          /* Unwind: remove already-inserted entries and the mmap_entry.
             Use munmap_entry which handles cleanup correctly. */
          munmap_entry (cur, me);
          return (mapid_t) -1;
        }

      spte->upage       = upage;
      spte->location    = PAGE_FILE;
      spte->writable    = true;
      spte->dirty       = false;
      spte->is_mmap     = true;
      spte->mapid       = me->mapid;
      spte->file        = mmap_file;
      spte->file_offset = offset;
      spte->read_bytes  = read_bytes;
      spte->zero_bytes  = zero_bytes;
      spte->swap_sector = 0;
      spte->kpage       = NULL;

      spt_insert (&cur->spage_table, spte);
      offset += PGSIZE;
    }

  return (mapid_t) me->mapid;
}

static void
sys_munmap (mapid_t mapid)
{
  struct thread *cur = thread_current ();

  for (struct list_elem *e = list_begin (&cur->mmap_list);
       e != list_end (&cur->mmap_list);
       e = list_next (e))
    {
      struct mmap_entry *me = list_entry (e, struct mmap_entry, elem);
      if (me->mapid == mapid)
        {
          munmap_entry (cur, me);
          return;
        }
    }
}
#endif /* VM */

/* Returns the struct file* for fd, or NULL if fd is invalid. */
static struct file *
get_file_from_fd (int fd)
{
  if (fd < 2 || fd >= MAX_FDS)
    return NULL;
  return thread_current ()->fd_table[fd];
}

/* Terminates the process if any byte of the string is in kernel space
   or unmapped user memory. */
static void
validate_string (const char *str)
{
  for (const char *p = str; ; p++)
    {
      if (get_user ((const uint8_t *) p) == -1)
        exit_with_status (-1);
      if (*p == '\0')
        break;
    }
}

/* Terminates the process if any byte of the buffer is invalid.
   If WRITABLE is true, also verifies that each byte can be written
   (catches SYS_READ into a read-only page before we hold filesys_lock). */
static void
validate_buffer (const void *buf, unsigned size, bool writable)
{
  for (unsigned i = 0; i < size; i++)
    {
      const uint8_t *p = (const uint8_t *) buf + i;
      int val = get_user (p);
      if (val == -1)
        exit_with_status (-1);
      if (writable && !put_user ((uint8_t *) p, (uint8_t) val))
        exit_with_status (-1);
    }
}

/****************** Robust Memory Access Helpers ********************/

static int32_t
get_user (const uint8_t *uaddr)
{
  if (!is_user_vaddr (uaddr))
    return -1;

  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}

/* Attempts to write BYTE to user address UDST.  Returns true on success,
   false if the address is invalid or read-only.  Uses the same assembly
   recovery pattern as get_user so the page-fault handler can recover. */
static bool
put_user (uint8_t *udst, uint8_t byte)
{
  if (!is_user_vaddr (udst))
    return false;
  int error_code;
  asm ("movl $1f, %0; movb %b2, %1; 1:"
       : "=&a" (error_code), "=m" (*udst) : "q" (byte));
  return error_code != (int) 0xffffffff;
}

static int
memread_user (const void *src, void *dst, size_t bytes)
{
  int32_t value;
  for (size_t i = 0; i < bytes; i++)
    {
      value = get_user ((const uint8_t *) src + i);
      if (value == -1)
        return -1;
      *(uint8_t *) (dst + i) = value & 0xff;
    }
  return (int) bytes;
}
