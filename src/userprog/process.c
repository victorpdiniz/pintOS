#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <string.h>
#include "threads/malloc.h"
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#ifdef VM
#include "vm/frame.h"
#include "vm/page.h"
#endif

extern struct lock filesys_lock;

/* Bundled arguments passed from process_execute to start_process. */
struct start_args
  {
    char *cmd_line;              /* palloc'd copy of the command line */
    struct child_process *cp;    /* parent-owned child_process entry */
#ifdef FILESYS
    block_sector_t cwd_sector;   /* parent's working directory */
#endif
  };

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/* Atomically decrement cp->ref_count; free when it reaches zero. */
static void
cp_release (struct child_process *cp)
{
  enum intr_level old_level = intr_disable ();
  int refs = --cp->ref_count;
  intr_set_level (old_level);
  if (refs == 0)
    free (cp);
}

/* Starts a new thread running a user program loaded from CMD_LINE.
   The new thread may be scheduled (and may even exit)
   before process_execute() returns. Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *cmd_line)
{
  char *cmd_copy;
  tid_t tid;

  cmd_copy = palloc_get_page (0);
  if (cmd_copy == NULL)
    return TID_ERROR;
  strlcpy (cmd_copy, cmd_line, PGSIZE);

  /* Extract the executable name for thread_create (stack-local). */
  char name_buf[16];
  strlcpy (name_buf, cmd_line, sizeof name_buf);
  char *save_ptr;
  char *file_name = strtok_r (name_buf, " ", &save_ptr);

  /* Allocate the child tracking entry. */
  struct child_process *cp = malloc (sizeof *cp);
  if (cp == NULL)
    {
      palloc_free_page (cmd_copy);
      return TID_ERROR;
    }
  cp->tid = TID_ERROR;
  cp->exit_status = -1;
  cp->waited = false;
  cp->load_success = false;
  cp->ref_count = 2;
  sema_init (&cp->sema, 0);
  sema_init (&cp->load_sema, 0);

  /* Bundle args for the child thread. */
  struct start_args *args = malloc (sizeof *args);
  if (args == NULL)
    {
      free (cp);
      palloc_free_page (cmd_copy);
      return TID_ERROR;
    }
  args->cmd_line = cmd_copy;
  args->cp = cp;
#ifdef FILESYS
  args->cwd_sector = thread_current ()->cwd_sector;
#endif

  tid = thread_create (file_name, PRI_DEFAULT, start_process, args);
  if (tid == TID_ERROR)
    {
      free (args);
      free (cp);
      palloc_free_page (cmd_copy);
      return TID_ERROR;
    }

  cp->tid = tid;
  list_push_back (&thread_current ()->children, &cp->elem);
  return tid;
}

/* Thread function that loads and runs a user process. */
static void
start_process (void *args_)
{
  struct thread *cur = thread_current ();

#ifdef VM
  /* Initialize VM state before any possible thread_exit(). */
  spt_init (&cur->spage_table);
  list_init (&cur->mmap_list);
  cur->next_mapid   = 1;
  cur->esp_saved    = NULL;
  cur->spt_initialized = true;
#endif

  struct start_args *args = args_;
  char *file_name = args->cmd_line;
  struct child_process *cp = args->cp;
#ifdef FILESYS
  block_sector_t parent_cwd = args->cwd_sector;
#endif
  free (args);

#ifdef FILESYS
  cur->cwd_sector = (parent_cwd != 0) ? parent_cwd : ROOT_DIR_SECTOR;
#endif

  cur->cp = cp;

  struct intr_frame if_;
  bool success;

  /* Pre-parse to count argc. */
  char *fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    {
      cp->load_success = false;
      sema_up (&cp->load_sema);
      thread_exit ();
    }
  strlcpy (fn_copy, file_name, PGSIZE);

  int argc = 0;
  char *token, *save_ptr;
  for (token = strtok_r (fn_copy, " ", &save_ptr); token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
    argc++;

  char *argv[argc];
  uint32_t argv_addresses[argc];

  /* Re-tokenize original string. */
  char *executable_name = strtok_r (file_name, " ", &save_ptr);
  argv[0] = executable_name;
  for (int i = 1; i < argc; i++)
    argv[i] = strtok_r (NULL, " ", &save_ptr);

  /* Initialize interrupt frame. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  /* Load the executable (protected by filesys lock). */
  lock_acquire (&filesys_lock);
  success = load (executable_name, &if_.eip, &if_.esp);
  lock_release (&filesys_lock);

  cp->load_success = success;
  sema_up (&cp->load_sema);

  if (!success)
    {
      palloc_free_page (fn_copy);
      palloc_free_page (file_name);
      thread_exit ();
    }

  /* Build user stack (80x86 calling convention). */
  void *esp = if_.esp;

  for (int i = argc - 1; i >= 0; i--)
    {
      size_t len = strlen (argv[i]) + 1;
      esp -= len;
      memcpy (esp, argv[i], len);
      argv_addresses[i] = (uint32_t) esp;
    }

  esp = (void *) ((uint32_t) esp & ~3);  /* word-align */

  esp -= 4;
  *(uint32_t *) esp = 0;  /* argv[argc] null sentinel */

  for (int i = argc - 1; i >= 0; i--)
    {
      esp -= 4;
      *(uint32_t *) esp = argv_addresses[i];
    }

  uint32_t argv_ptr = (uint32_t) esp;
  esp -= 4;
  *(uint32_t *) esp = argv_ptr;  /* argv */

  esp -= 4;
  *(int *) esp = argc;  /* argc */

  esp -= 4;
  *(uint32_t *) esp = 0;  /* fake return address */

  if_.esp = esp;

  palloc_free_page (fn_copy);
  palloc_free_page (file_name);

  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.
   Returns -1 if TID is not a direct child or was already waited on. */
int
process_wait (tid_t child_tid)
{
  struct thread *cur = thread_current ();
  struct child_process *cp = NULL;

  for (struct list_elem *e = list_begin (&cur->children);
       e != list_end (&cur->children);
       e = list_next (e))
    {
      struct child_process *c = list_entry (e, struct child_process, elem);
      if (c->tid == child_tid)
        {
          cp = c;
          break;
        }
    }

  if (cp == NULL || cp->waited)
    return -1;

  cp->waited = true;
  sema_down (&cp->sema);

  int status = cp->exit_status;
  list_remove (&cp->elem);
  cp_release (cp);
  return status;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  /* Close all open file descriptors and directory fds. */
  lock_acquire (&filesys_lock);
  for (int i = 2; i < MAX_FDS; i++)
    {
      if (cur->fd_table[i] != NULL)
        {
          file_close (cur->fd_table[i]);
          cur->fd_table[i] = NULL;
        }
#ifdef FILESYS
      if (cur->dir_table[i] != NULL)
        {
          dir_close (cur->dir_table[i]);
          cur->dir_table[i] = NULL;
        }
#endif
    }
  /* Re-allow writes to and close the executable. */
  if (cur->executable != NULL)
    {
      file_close (cur->executable);
      cur->executable = NULL;
    }
  lock_release (&filesys_lock);

#ifdef VM
  if (cur->spt_initialized)
    {
      /* Write back all dirty mmap pages, then tear down the SPT. */
      munmap_all (cur);
      spt_destroy (&cur->spage_table);
    }
#endif

  /* Signal parent that this process has exited. */
  if (cur->cp != NULL)
    {
      cur->cp->exit_status = cur->exit_status;
      sema_up (&cur->cp->sema);
      cp_release (cur->cp);
    }

  /* Release all children's tracking entries. */
  while (!list_empty (&cur->children))
    {
      struct child_process *cp =
        list_entry (list_pop_front (&cur->children),
                    struct child_process, elem);
      cp_release (cp);
    }

  /* Destroy the current process's page directory. */
  pd = cur->pagedir;
  if (pd != NULL)
    {
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current thread. */
void
process_activate (void)
{
  struct thread *t = thread_current ();
  pagedir_activate (t->pagedir);
  tss_update ();
}

/* ELF types. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

#define PE32Wx PRIx32
#define PE32Ax PRIx32
#define PE32Ox PRIx32
#define PE32Hx PRIx16

struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_STACK   0x6474e551

#define PF_X 1
#define PF_W 2
#define PF_R 4

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores entry point into *EIP and initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp)
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL)
    goto done;
  process_activate ();

  file = filesys_open (file_name);
  if (file == NULL)
    {
      printf ("load: %s: open failed\n", file_name);
      goto done;
    }

  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024)
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done;
    }

  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++)
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type)
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file))
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else
                {
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  if (!setup_stack (esp))
    goto done;

  *eip = (void (*) (void)) ehdr.e_entry;
  success = true;

 done:
  if (success)
    {
      /* Keep the file open and deny writes to protect the running executable. */
      file_deny_write (file);
      t->executable = file;
    }
  else
    file_close (file);

  return success;
}

static bool install_page (void *upage, void *kpage, bool writable);

static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;
  if (phdr->p_offset > (Elf32_Off) file_length (file))
    return false;
  if (phdr->p_memsz < phdr->p_filesz)
    return false;
  if (phdr->p_memsz == 0)
    return false;
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;
  if (phdr->p_vaddr < PGSIZE)
    return false;
  return true;
}

static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

#ifdef VM
  off_t file_offset = ofs;
  while (read_bytes > 0 || zero_bytes > 0)
    {
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      struct sup_page_entry *spte = malloc (sizeof *spte);
      if (spte == NULL)
        return false;

      spte->upage       = upage;
      spte->location    = (page_read_bytes == 0) ? PAGE_ZERO : PAGE_FILE;
      spte->writable    = writable;
      spte->dirty       = false;
      spte->is_mmap     = false;
      spte->mapid       = -1;
      spte->file        = file;
      spte->file_offset = file_offset;
      spte->read_bytes  = page_read_bytes;
      spte->zero_bytes  = page_zero_bytes;
      spte->swap_sector = 0;
      spte->kpage       = NULL;

      if (!spt_insert (&thread_current ()->spage_table, spte))
        {
          free (spte);
          return false;
        }

      read_bytes  -= page_read_bytes;
      zero_bytes  -= page_zero_bytes;
      upage       += PGSIZE;
      file_offset += page_read_bytes;
    }
  return true;
#else
  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0)
    {
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false;
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      if (!install_page (upage, kpage, writable))
        {
          palloc_free_page (kpage);
          return false;
        }

      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
#endif
}

static bool
setup_stack (void **esp)
{
#ifdef VM
  void *upage = ((uint8_t *) PHYS_BASE) - PGSIZE;

  struct sup_page_entry *spte = malloc (sizeof *spte);
  if (spte == NULL)
    return false;

  spte->upage       = upage;
  spte->location    = PAGE_ZERO;
  spte->writable    = true;
  spte->dirty       = false;
  spte->is_mmap     = false;
  spte->mapid       = -1;
  spte->file        = NULL;
  spte->file_offset = 0;
  spte->read_bytes  = 0;
  spte->zero_bytes  = PGSIZE;
  spte->swap_sector = 0;
  spte->kpage       = NULL;

  if (!spt_insert (&thread_current ()->spage_table, spte))
    {
      free (spte);
      return false;
    }

  if (!spt_load_page (spte))
    return false;

  *esp = PHYS_BASE;
  return true;
#else
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL)
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
#endif
}

static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
