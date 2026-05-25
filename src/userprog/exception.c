#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#ifdef VM
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "vm/frame.h"
#endif

/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

void
exception_init (void)
{
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill,
                     "#BR BOUND Range Exceeded Exception");

  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill,
                     "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill,
                     "#XF SIMD Floating-Point Exception");

  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

void
exception_print_stats (void)
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

static void
kill (struct intr_frame *f)
{
  switch (f->cs)
    {
    case SEL_UCSEG:
      printf ("%s: dying due to interrupt %#04x (%s).\n",
              thread_name (), f->vec_no, intr_name (f->vec_no));
      intr_dump_frame (f);
      thread_exit ();

    case SEL_KCSEG:
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel");

    default:
      printf ("Interrupt %#04x (%s) in unknown segment %04x\n",
             f->vec_no, intr_name (f->vec_no), f->cs);
      thread_exit ();
    }
}

static void
page_fault (struct intr_frame *f)
{
  bool not_present;
  bool write;
  bool user;
  void *fault_addr;

  asm ("movl %%cr2, %0" : "=r" (fault_addr));
  intr_enable ();
  page_fault_cnt++;

  not_present = (f->error_code & PF_P) == 0;
  write       = (f->error_code & PF_W) != 0;
  user        = (f->error_code & PF_U) != 0;

  struct thread *cur = thread_current ();

#ifdef VM
  /* Determine the user-mode stack pointer (needed for stack growth). */
  void *user_esp = user ? f->esp : cur->esp_saved;

  /* Only attempt VM recovery for user-space addresses. */
  if (fault_addr != NULL && is_user_vaddr (fault_addr))
    {
      void *fault_page = pg_round_down (fault_addr);

      if (not_present)
        {
          /* Look up the page in the SPT. */
          struct sup_page_entry *spte =
            spt_find (&cur->spage_table, fault_page);

          if (spte != NULL)
            {
              /* Rights violation: write to a read-only page. */
              if (write && !spte->writable)
                goto cannot_handle;

              if (spt_load_page (spte))
                return;

              goto cannot_handle;
            }

          /* Stack growth: page not in SPT but within the allowed range. */
          if (user_esp != NULL
              && (uintptr_t) fault_addr >= (uintptr_t) PHYS_BASE - 8 * 1024 * 1024
              && (uintptr_t) fault_addr >= (uintptr_t) user_esp - 32)
            {
              struct sup_page_entry *new_spte = malloc (sizeof *new_spte);
              if (new_spte == NULL)
                goto cannot_handle;

              memset (new_spte, 0, sizeof *new_spte);
              new_spte->upage      = fault_page;
              new_spte->location   = PAGE_ZERO;
              new_spte->writable   = true;
              new_spte->zero_bytes = PGSIZE;

              if (!spt_insert (&cur->spage_table, new_spte))
                {
                  free (new_spte);
                  goto cannot_handle;
                }

              if (spt_load_page (new_spte))
                return;

              hash_delete (&cur->spage_table, &new_spte->hash_elem);
              free (new_spte);
              goto cannot_handle;
            }
        }
    }

cannot_handle:
#endif /* VM */

  if (!user)
    {
      /* Kernel-mode fault: recover using the get_user assembly pattern.
         The recovery address was stored in %eax before the faulting load. */
      f->eip = (void (*) (void)) f->eax;
      f->eax = 0xffffffff;
      return;
    }

  /* User-mode fault that could not be resolved. */
  printf ("%s: exit(%d)\n", cur->name, -1);
  cur->exit_status = -1;
  thread_exit ();
}
