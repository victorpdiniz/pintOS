#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"
#include "lib/kernel/stdio.h"

static void syscall_handler (struct intr_frame *);
static void validate_ptr (const void *vaddr);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  /* Valida o ponteiro do número da syscall. */
  validate_ptr(f->esp);
  int syscall_num = *(int *)f->esp;

  switch (syscall_num) 
  {
    case SYS_EXIT:
      {
        validate_ptr(f->esp + 4);
        int status = *(int *)(f->esp + 4);
        
        /* Regra (iii) do SYS_EXIT: printf específico. */
        printf("%s: exit(%d)\n", thread_current()->name, status);
        
        /* Libera o pai que está no wait (se houver semáforo real). */
        // sema_up(&thread_current()->wait_sema); 
        
        thread_exit();
        break;
      }

    case SYS_WRITE:
      {
        /* Valida argumentos. */
        validate_ptr(f->esp + 4); // fd
        validate_ptr(f->esp + 8); // buffer
        validate_ptr(f->esp + 12); // size

        int fd = *(int *)(f->esp + 4);
        const void *buffer = *(char **)(f->esp + 8);
        unsigned size = *(unsigned *)(f->esp + 12);

        validate_ptr(buffer);

        if (fd == 1) /* STDOUT */
          {
            putbuf(buffer, size);
            f->eax = size;
          }
        break;
      }

    case SYS_WAIT:
      {
        validate_ptr(f->esp + 4);
        tid_t tid = *(tid_t *)(f->esp + 4);
        /* Regra (ii) do SYS_WAIT: uso simples do wait. */
        f->eax = process_wait(tid);
        break;
      }

    default:
      thread_exit();
  }
}

/* Validação conforme as orientações de endereços válidos. */
static void
validate_ptr (const void *vaddr) 
{
  if (!vaddr || !is_user_vaddr(vaddr) || pagedir_get_page(thread_current()->pagedir, vaddr) == NULL)
    {
      /* Se acesso inválido, termina o processo conforme regras. */
      printf("%s: exit(-1)\n", thread_current()->name);
      thread_exit();
    }
}