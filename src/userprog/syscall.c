#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"
#include "lib/kernel/stdio.h"
#include "userprog/process.h"

typedef uint32_t pid_t;
static void syscall_handler (struct intr_frame *);
static void exit_with_status (int status);
static int memread_user (const void *src, void *dst, size_t bytes);
static int32_t get_user (const uint8_t *uaddr);
pid_t sys_exec (const char *cmdline);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  int syscall_num;

  /* Safely read the syscall number from the user stack. */
  if (memread_user (f->esp, &syscall_num, sizeof (syscall_num)) == -1)
    exit_with_status (-1);

  switch (syscall_num) 
  {
    case SYS_HALT:
    {
      shutdown_power_off (); /* */
      break;
    }

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

    case SYS_REMOVE:
    
    case SYS_OPEN:
    
    case SYS_FILESIZE:
    
    case SYS_READ:

    case SYS_WRITE:
    {
      int fd;
      const void *buffer;
      unsigned size;

      /* Extract arguments from stack. */
      if (memread_user (f->esp + 4, &fd, sizeof (fd)) == -1 ||
          memread_user (f->esp + 8, &buffer, sizeof (buffer)) == -1 ||
          memread_user (f->esp + 12, &size, sizeof (size)) == -1)
        exit_with_status (-1);

      /* Validate the entire buffer range before using putbuf. */
      for (unsigned i = 0; i < size; i++)
      {
        if (get_user ((const uint8_t *)buffer + i) == -1)
          exit_with_status (-1);
      }

      if (fd == 1) /* STDOUT_FILENO */
      {
        putbuf (buffer, size);
        f->eax = size;
      }
      else
        f->eax = 0;
      break;
    }

    case SYS_SEEK:
    
    case SYS_TELL:
    
    case SYS_CLOSE:

    default:
      exit_with_status (-1);
  }
}

/* Helper to handle process termination and print the required message. */
static void
exit_with_status (int status)
{
  printf ("%s: exit(%d)\n", thread_current ()->name, status);
  thread_exit ();
}

/* Helper to handle process execution. */
pid_t
sys_exec (const char *cmdline)
{
  if (cmdline == NULL)
    exit_with_status (-1);

  /* To be "bulletproof," the kernel must check every byte 
     of the string to ensure the user isn't passing a string 
     that partially overlaps into kernel memory or unmapped pages. */
  for (const char *p = cmdline; ; p++)
    {
      if (get_user ((const uint8_t *) p) == -1)
        exit_with_status (-1);
      if (*p == '\0')
        break;
    }

  tid_t child_tid = process_execute (cmdline);

  return (pid_t) child_tid;
}

/****************** Robust Memory Access Helpers ********************/

/* Reads a single byte from user address uaddr.
   Returns the byte value or -1 if a segfault occurred. */
static int32_t
get_user (const uint8_t *uaddr) 
{
  if (!is_user_vaddr (uaddr)) /* Ensure address is below PHYS_BASE. */
    return -1;

  int result;
  /* Inline assembly to catch page faults in the kernel. */
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}

/* Safely reads 'bytes' from user memory starting at 'src' into 'dst'.
   Returns 'bytes' if successful, or -1 on error. */
static int
memread_user (const void *src, void *dst, size_t bytes)
{
  int32_t value;
  for (size_t i = 0; i < bytes; i++) 
    {
      value = get_user ((const uint8_t *)src + i);
      if (value == -1) 
        return -1;
      *(uint8_t *)(dst + i) = value & 0xff;
    }
  return (int)bytes;
}