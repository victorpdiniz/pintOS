# Relatorio de Decisoes de Projeto
## Projeto 3 — Virtual Memory Completo (SPT, Swap, Evicao, Stack Growth, mmap)

Centro de Informatica — Universidade Federal de Pernambuco
Disciplina: Sistemas Operacionais — 2025 / 2026

---

## 1. Sumario Executivo

Este relatorio documenta a **conclusao** do Projeto 3 de Memoria Virtual do PintOS, construida sobre a Frame Table entregue anteriormente. Ao final desta etapa, o kernel possui suporte completo a memoria virtual sob demanda: tabela de paginas suplementar por processo, algoritmo de relogio para evicao de frames, swap em disco, crescimento dinamico da pilha, carregamento lento de executaveis e arquivos mapeados na memoria (mmap/munmap).

**Resultado dos testes: 113/113 (100%)**

| Arquivo | Status | Funcao |
|---|---|---|
| vm/page.h | Novo | Define `sup_page_entry`, `mmap_entry` e prototipos publicos do SPT |
| vm/page.c | Novo | Implementa SPT (hash por processo), `spt_load_page`, `munmap_entry`, `spt_destroy` |
| vm/swap.h | Novo | Define `swap_init`, `swap_write`, `swap_read`, `swap_free` |
| vm/swap.c | Novo | Implementa bitmap de slots e I/O com BLOCK_SWAP |
| vm/frame.h | Modificado | Adiciona campos `spte`, `pinned`; novos prototipos `frame_remove`, `frame_unpin` |
| vm/frame.c | Modificado | Adiciona algoritmo de relogio, evicao, pinning, `frame_remove`, `frame_unpin` |
| userprog/exception.c | Modificado | Handler de page fault: lazy load, stack growth, recuperacao em modo kernel |
| userprog/process.c | Modificado | `load_segment` com SPT lazy, `setup_stack` via SPT, `spt_destroy` em `process_exit` |
| userprog/syscall.c | Modificado | `SYS_MMAP`, `SYS_MUNMAP`, `esp_saved`, `put_user` em `validate_buffer` |
| threads/thread.h | Modificado | Campos VM: `spt_initialized`, `spage_table`, `mmap_list`, `next_mapid`, `esp_saved` |
| threads/init.c | Modificado | Chama `frame_init()` e `swap_init()` apos `filesys_init()` sob `#ifdef VM` |
| src/Makefile.build | Modificado | `vm_SRC` com page.c e swap.c; correcao do `loader.bin` para GNU ld 2.45 |

---

## 2. Contexto — Estado apos a Frame Table

Ao final do relatorio anterior, a frame table rastreava frames fisicos alocados mas:

- `palloc_get_page` retornava NULL quando a memoria se esgotava — nenhum frame era evictado
- `load_segment` alocava frames imediatamente para cada pagina do executavel, mesmo que nunca fossem acessadas
- Page faults terminavam o processo incondicionalmente (nao havia handler de recuperacao)
- Nao existia representacao do que uma pagina virtual *deveria* conter quando nao estava em RAM
- Nao havia crescimento de pilha, swap nem mmap

Esta etapa adiciona as **tres camadas** que fecham o subsistema:

```
Processo usuario
      |
      v  page fault
  exception.c  <-- identifica causa: stack growth / lazy load / swap reclaim
      |
      v
  vm/page.c    <-- SPT: o que deve estar nessa pagina virtual?
      |
      v
  vm/frame.c   <-- frame table: aloca frame fisico; evicta via clock se necessario
      |
      v
  vm/swap.c    <-- swap: leitura/escrita de paginas no disco
```

**Invariante central:** toda pagina virtual de usuario e descrita por uma entrada `sup_page_entry` no SPT da thread. O frame fisico e alocado sob demanda, na primeira falha de pagina.

---

## 3. vm/page.h — Estruturas de Dados

O header define as duas estruturas centrais do subsistema e os prototipos publicos.

**vm/page.h — enum page_location**
```c
enum page_location {
    PAGE_ZERO,   /* zeros puros (stack growth ou BSS) */
    PAGE_FILE,   /* arquivo (executavel ou mmap) */
    PAGE_SWAP,   /* expulso para swap */
    PAGE_FRAME,  /* atualmente em frame fisico */
};
```

**vm/page.h — struct sup_page_entry**
```c
struct sup_page_entry {
    struct hash_elem hash_elem;
    void *upage;              /* endereco virtual (alinhado a pagina) */

    enum page_location location;
    bool writable;
    bool dirty;               /* acumulado para write-back durante evicao */

    bool is_mmap;             /* pagina pertence a um mmap? */
    int  mapid;
    struct file *file;
    off_t file_offset;
    size_t read_bytes;
    size_t zero_bytes;

    size_t swap_sector;       /* setor inicial no dispositivo de swap */
    void  *kpage;             /* endereco kernel quando PAGE_FRAME */
};
```

**vm/page.h — struct mmap_entry**
```c
struct mmap_entry {
    int mapid;
    struct file *file;    /* referencia independente via file_reopen */
    void *addr;           /* inicio da regiao mapeada */
    size_t page_count;
    struct list_elem elem;
};
```

| Campo | Tipo | Finalidade |
|---|---|---|
| `upage` | `void *` | Chave do hash. Identifica univocamente a pagina virtual no espaco do processo |
| `location` | `enum` | Diz onde estao os dados: RAM, arquivo, swap ou zero |
| `dirty` | `bool` | Preserva o bit dirty entre evicoes — necessario para write-back correto de mmap |
| `is_mmap` | `bool` | Distingue paginas de mmap (write-back para arquivo) de paginas normais (vai para swap) |
| `swap_sector` | `size_t` | Indice do setor inicial no bitmap de swap quando `location == PAGE_SWAP` |
| `kpage` | `void *` | Endereco kernel valido somente quando `location == PAGE_FRAME` |

---

## 4. vm/page.c — Implementacao do SPT

### 4.1 Hash table por processo

O SPT usa `struct hash` do PintOS, indexado por `upage`. As funcoes de hash e comparacao:

**vm/page.c — funcoes de hash**
```c
static unsigned
page_hash (const struct hash_elem *e, void *aux UNUSED) {
    const struct sup_page_entry *spte =
        hash_entry (e, struct sup_page_entry, hash_elem);
    return hash_bytes (&spte->upage, sizeof spte->upage);
}

static bool
page_less (const struct hash_elem *a,
           const struct hash_elem *b, void *aux UNUSED) {
    return hash_entry (a, struct sup_page_entry, hash_elem)->upage
         < hash_entry (b, struct sup_page_entry, hash_elem)->upage;
}
```

`spt_init` chama `hash_init` com essas funcoes. `spt_find` usa `hash_find` diretamente, sem lock — o SPT e privado por thread, entao nao ha corrida.

### 4.2 spt_load_page() — carregamento sob demanda

Chamada pelo handler de page fault apos identificar a entrada SPT correspondente.

**vm/page.c — spt_load_page()**
```c
bool
spt_load_page (struct sup_page_entry *spte) {
    void *kpage = frame_alloc (spte, PAL_USER |
                               (spte->location == PAGE_ZERO ? PAL_ZERO : 0));
    if (!kpage) return false;

    switch (spte->location) {
    case PAGE_ZERO:
        memset (kpage, 0, PGSIZE);
        break;
    case PAGE_FILE:
        file_read_at (spte->file, kpage,
                      spte->read_bytes, spte->file_offset);
        memset (kpage + spte->read_bytes, 0, spte->zero_bytes);
        break;
    case PAGE_SWAP:
        swap_read (spte->swap_sector, kpage);
        swap_free (spte->swap_sector);
        break;
    default:
        frame_free (kpage);
        return false;
    }

    pagedir_set_page (thread_current ()->pagedir,
                      spte->upage, kpage, spte->writable);
    spte->kpage     = kpage;
    spte->location  = PAGE_FRAME;
    frame_unpin (kpage);   /* permite evicao apos instalacao */
    return true;
}
```

**Por que `file_read_at` sem `filesys_lock`?**
`file_read_at` acessa o inode em offset explicito sem modificar o estado interno do arquivo. O executavel tem `file_deny_write` ativo, tornando leituras concorrentes seguras. Adquirir `filesys_lock` aqui causaria deadlock: `SYS_READ` pode estar segurando o lock quando o page fault ocorre dentro de `file_read`.

### 4.3 munmap_entry() — write-back e liberacao

**vm/page.c — munmap_entry() (resumido)**
```c
void
munmap_entry (struct thread *t, struct mmap_entry *me) {
    for (size_t i = 0; i < me->page_count; i++) {
        void *upage = me->addr + i * PGSIZE;
        struct sup_page_entry *spte = spt_find (&t->spage_table, upage);

        if (spte->location == PAGE_FRAME) {
            bool dirty = pagedir_is_dirty (t->pagedir, upage) || spte->dirty;
            if (dirty) {
                lock_acquire (&filesys_lock);
                file_write_at (me->file, spte->kpage,
                               spte->read_bytes, spte->file_offset);
                lock_release (&filesys_lock);
            }
            pagedir_clear_page (t->pagedir, upage);
            frame_free (spte->kpage);
        } else if (spte->location == PAGE_SWAP) {
            if (spte->dirty) {
                void *buf = palloc_get_page (0);
                swap_read (spte->swap_sector, buf);
                lock_acquire (&filesys_lock);
                file_write_at (me->file, buf,
                               spte->read_bytes, spte->file_offset);
                lock_release (&filesys_lock);
                palloc_free_page (buf);
            } else {
                swap_free (spte->swap_sector);
            }
        }
        hash_delete (&t->spage_table, &spte->hash_elem);
        free (spte);
    }
    file_close (me->file);
    list_remove (&me->elem);
    free (me);
}
```

O campo `spte->dirty` e essencial: quando uma pagina mmap e evictada para swap, o bit dirty do pagedir e perdido (`pagedir_clear_page`). O campo acumula esse estado para que `munmap_entry` possa decidir corretamente se escreve de volta ao arquivo.

### 4.4 spt_destroy() — destruicao em process_exit

O destrutor `page_free_entry` e chamado por `hash_destroy` para cada entrada:

| `location` | Acao |
|---|---|
| `PAGE_FRAME` | `frame_remove(kpage)` — remove da tabela sem `palloc_free`; `pagedir_destroy` libera o frame fisico |
| `PAGE_SWAP` | `swap_free(swap_sector)` — libera o slot |
| `PAGE_FILE` / `PAGE_ZERO` | Nenhuma acao de I/O necessaria |

**Por que `frame_remove` e nao `frame_free`?**
`pagedir_destroy`, chamado logo apos `spt_destroy`, percorre todas as PTEs com `PTE_P=1` e devolve os frames ao palloc. Chamar `palloc_free_page` no destrutor do SPT causaria double-free. `frame_remove` retira a entrada da tabela sem liberar a pagina fisica.

---

## 5. vm/swap.h + vm/swap.c

### 5.1 Estado global

**vm/swap.c — variaveis estaticas**
```c
static struct block  *swap_block;
static struct bitmap *swap_bitmap;   /* 1 bit por slot; 1 slot = 1 pagina */
static struct lock    swap_lock;

#define SECTORS_PER_PAGE  (PGSIZE / BLOCK_SECTOR_SIZE)   /* = 8 */
```

`swap_init` obtem o dispositivo `BLOCK_SWAP`, calcula o numero de slots (`block_size / SECTORS_PER_PAGE`) e aloca o bitmap.

### 5.2 swap_write() e swap_read()

**vm/swap.c — swap_write()**
```c
size_t
swap_write (void *kpage) {
    lock_acquire (&swap_lock);
    size_t slot = bitmap_scan_and_flip (swap_bitmap, 0, 1, false);
    lock_release (&swap_lock);
    if (slot == BITMAP_ERROR)
        PANIC ("swap: no free slot");
    size_t base = slot * SECTORS_PER_PAGE;
    for (size_t i = 0; i < SECTORS_PER_PAGE; i++)
        block_write (swap_block, base + i,
                     (uint8_t *) kpage + i * BLOCK_SECTOR_SIZE);
    return base;   /* indice do setor inicial — armazenado em spte->swap_sector */
}
```

**vm/swap.c — swap_read()**
```c
void
swap_read (size_t sector, void *kpage) {
    for (size_t i = 0; i < SECTORS_PER_PAGE; i++)
        block_read (swap_block, sector + i,
                    (uint8_t *) kpage + i * BLOCK_SECTOR_SIZE);
}
```

`swap_free` usa `bitmap_set (swap_bitmap, sector / SECTORS_PER_PAGE, false)` para marcar o slot livre.

**Por que o lock so cobre `bitmap_scan_and_flip` e nao o I/O?**
`block_write` e `block_read` ja sao thread-safe internamente. Segurar `swap_lock` durante o I/O de disco bloquearia desnecessariamente outras threads que precisam apenas de um slot diferente.

---

## 6. vm/frame.c — Extensoes para Evicao

### 6.1 Novos campos em struct frame_entry

**vm/frame.h — frame_entry (versao final)**
```c
struct frame_entry {
    struct list_elem elem;
    void *kpage;
    struct thread *owner;
    struct sup_page_entry *spte;   /* NOVO: entrada SPT correspondente */
    bool pinned;                   /* NOVO: true = nao elegivel para evicao */
};
```

`spte` substitui o antigo campo `upage` — agora o frame conhece toda a descricao da pagina, nao so o endereco virtual. `pinned` garante que um frame em processo de populacao nao seja evictado por outra thread.

### 6.2 frame_alloc() — alocacao com evicao

**vm/frame.c — frame_alloc()**
```c
void *
frame_alloc (struct sup_page_entry *spte, enum palloc_flags flags) {
    lock_acquire (&frame_lock);

    void *kpage = palloc_get_page (flags);
    if (kpage != NULL) {
        struct frame_entry *fte = malloc (sizeof *fte);
        fte->kpage  = kpage;
        fte->owner  = thread_current ();
        fte->spte   = spte;
        fte->pinned = true;         /* pinado ate frame_unpin apos pagedir_set_page */
        list_push_back (&frame_table, &fte->elem);
        lock_release (&frame_lock);
        return kpage;
    }

    /* Memoria fisica esgotada — evicta. */
    kpage = frame_evict (spte, flags);
    lock_release (&frame_lock);
    return kpage;
}
```

| Passo | Decisao | Motivo |
|---|---|---|
| `pinned = true` na criacao | Frame nao pode ser evictado imediatamente | O frame ainda nao tem mapeamento no pagedir; evicta-lo antes causaria inconsistencia |
| `frame_unpin` apos `pagedir_set_page` | Libera para evicao | So apos o mapeamento estar instalado o algoritmo de relogio pode acessar os bits do pagedir |
| `spte` no lugar de `upage` | Frame conhece toda a entrada SPT | Evicao precisa chamar `swap_write` ou `file_write_at` usando os metadados da pagina |

### 6.3 Algoritmo de Relogio (Clock / Second-Chance)

**vm/frame.c — frame_evict() (resumido)**
```c
static void *
frame_evict (struct sup_page_entry *new_spte, enum palloc_flags flags) {
    size_t n = list_size (&frame_table) * 2 + 2;

    for (size_t i = 0; i < n; i++) {
        if (clock_hand == NULL || clock_hand == list_end (&frame_table))
            clock_hand = list_begin (&frame_table);

        struct frame_entry *fte =
            list_entry (clock_hand, struct frame_entry, elem);
        clock_hand = list_next (clock_hand);

        if (fte->pinned) continue;

        if (pagedir_is_accessed (fte->owner->pagedir, fte->spte->upage)) {
            pagedir_set_accessed (fte->owner->pagedir,
                                  fte->spte->upage, false);  /* segunda chance */
            continue;
        }

        /* Vitima selecionada. */
        bool dirty = pagedir_is_dirty (fte->owner->pagedir, fte->spte->upage)
                     || fte->spte->dirty;
        void *kpage     = fte->kpage;
        struct sup_page_entry *old_spte = fte->spte;

        pagedir_clear_page (fte->owner->pagedir, old_spte->upage);
        old_spte->kpage = NULL;
        old_spte->dirty = dirty;

        /* Atualiza frame_entry in-place antes de soltar o lock. */
        fte->spte   = new_spte;
        fte->owner  = thread_current ();
        fte->pinned = true;
        lock_release (&frame_lock);

        /* I/O sem frame_lock para evitar deadlock com filesys_lock. */
        if (old_spte->is_mmap) {
            if (dirty) {
                lock_acquire (&filesys_lock);
                file_write_at (old_spte->file, kpage,
                               old_spte->read_bytes, old_spte->file_offset);
                lock_release (&filesys_lock);
            }
            old_spte->location = PAGE_FILE;
        } else {
            old_spte->swap_sector = swap_write (kpage);
            old_spte->location   = PAGE_SWAP;
        }

        if (flags & PAL_ZERO) memset (kpage, 0, PGSIZE);
        lock_acquire (&frame_lock);
        return kpage;
    }
    return NULL;
}
```

**Por que soltar o lock antes do I/O?**
`swap_write` e `file_write_at` podem bloquear na fila de disco. Manter `frame_lock` durante esse bloqueio causaria deadlock: outra thread em page fault chamaria `frame_alloc`, tentaria adquirir `frame_lock` e ficaria presa enquanto a primeira thread espera o disco. A solucao e atualizar `fte->spte` e `fte->owner` in-place antes de soltar o lock — o frame fica "reservado" para o novo dono sem que outro thread o selecione como vitima.

### 6.4 frame_remove vs frame_free

| Funcao | Remove da tabela | `palloc_free_page` | Uso |
|---|---|---|---|
| `frame_free` | Sim | Sim | `munmap_entry`, erros em `spt_load_page` |
| `frame_remove` | Sim | Nao | Destrutor SPT — `pagedir_destroy` libera o frame |

---

## 7. userprog/exception.c — Handler de Page Fault

### 7.1 Fluxo completo em page_fault()

**userprog/exception.c — page_fault() (estrutura)**
```c
void
page_fault (struct intr_frame *f) {
    void *fault_addr = /* cr2 */;
    bool user  = (f->error_code & PF_U) != 0;
    bool write = (f->error_code & PF_W) != 0;

    /* (1) Recuperacao em modo kernel (get_user/put_user) */
    if (!user) {
        if (f->eax != 0) {         /* existe label de recuperacao em %eax */
            f->eip = (void (*)(void)) f->eax;
            f->eax = 0xffffffff;   /* get_user retorna -1 */
            return;
        }
        /* Nao ha recuperacao — encerra o processo. */
    }

    void *upage = pg_round_down (fault_addr);
    struct thread *cur = thread_current ();
    struct sup_page_entry *spte = spt_find (&cur->spage_table, upage);

    /* (2) Lazy load ou reclaim de swap */
    if (spte != NULL) {
        if (!spt_load_page (spte))
            exit_with_status (-1);
        return;
    }

    /* (3) Stack growth */
    void *user_esp = user ? f->esp : cur->esp_saved;
    if (user_esp != NULL
        && (uintptr_t) fault_addr < (uintptr_t) PHYS_BASE
        && (uintptr_t) fault_addr >= (uintptr_t) PHYS_BASE - 8 * 1024 * 1024
        && (uintptr_t) fault_addr >= (uintptr_t) user_esp - 32)
    {
        struct sup_page_entry *spte = malloc (sizeof *spte);
        spte->upage    = upage;
        spte->location = PAGE_ZERO;
        spte->writable = true;
        spte->is_mmap  = false;
        spte->dirty    = false;
        spt_insert (&cur->spage_table, spte);
        if (!spt_load_page (spte))
            exit_with_status (-1);
        return;
    }

    exit_with_status (-1);
}
```

### 7.2 Condicoes de stack growth

| Condicao | Razao |
|---|---|
| `fault_addr < PHYS_BASE` | Endereco de usuario |
| `>= PHYS_BASE - 8 MB` | Pilha limitada a 8 MB pelo enunciado |
| `>= user_esp - 32` | Cobre a instrucao PUSHA (empurra 32 bytes abaixo de esp) |

### 7.3 esp_saved — ESP do usuario em contexto de kernel

Quando o page fault ocorre dentro de uma syscall (modo kernel), `f->esp` aponta para a pilha do kernel. O ESP real do usuario e salvo em `thread->esp_saved` no inicio do syscall_handler e limpo ao sair.

**userprog/syscall.c**
```c
void
syscall_handler (struct intr_frame *f) {
    thread_current ()->esp_saved = f->esp;
    /* ... despacho ... */
    thread_current ()->esp_saved = NULL;
}
```

**userprog/exception.c**
```c
void *user_esp = user ? f->esp : cur->esp_saved;
```

---

## 8. userprog/process.c — Lazy Loading

### 8.1 load_segment() com SPT

Antes: alocava um frame e lia o arquivo imediatamente para cada pagina.
Agora: cria apenas entradas SPT sem alocar frames.

**userprog/process.c — load_segment() com VM**
```c
static bool
load_segment (...) {
    off_t file_offset = ofs;
    while (read_bytes > 0 || zero_bytes > 0) {
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        struct sup_page_entry *spte = malloc (sizeof *spte);
        spte->upage        = upage;
        spte->location     = (page_read_bytes == 0) ? PAGE_ZERO : PAGE_FILE;
        spte->file         = file;
        spte->file_offset  = file_offset;
        spte->read_bytes   = page_read_bytes;
        spte->zero_bytes   = page_zero_bytes;
        spte->writable     = writable;
        spte->is_mmap      = false;
        spte->dirty        = false;
        spt_insert (&thread_current ()->spage_table, spte);

        read_bytes  -= page_read_bytes;
        zero_bytes  -= page_zero_bytes;
        upage       += PGSIZE;
        file_offset += page_read_bytes;
    }
    return true;
}
```

### 8.2 setup_stack() via SPT

```c
static bool
setup_stack (void **esp) {
    struct sup_page_entry *spte = malloc (sizeof *spte);
    spte->upage    = ((uint8_t *) PHYS_BASE) - PGSIZE;
    spte->location = PAGE_ZERO;
    spte->writable = true;
    spte->is_mmap  = false;
    spte->dirty    = false;
    spt_insert (&thread_current ()->spage_table, spte);

    if (!spt_load_page (spte)) return false;
    *esp = PHYS_BASE;
    return true;
}
```

A pilha inicial e carregada imediatamente via `spt_load_page` — ao contrario das paginas do executavel, a pilha precisa existir antes de executar qualquer instrucao.

### 8.3 Ordem de liberacao em process_exit()

```c
void
process_exit (void) {
    /* 1. Fecha fds e executavel. */
    lock_acquire (&filesys_lock);
    /* ... fecha fd_table[2..MAX_FDS] e executable ... */
    lock_release (&filesys_lock);

    /* 2. Write-back de todas as paginas mmap sujas. */
    munmap_all (cur);

    /* 3. Destroi SPT (libera slots de swap, remove entradas do frame table). */
    if (cur->spt_initialized)
        spt_destroy (&cur->spage_table);

    /* 4. Sinaliza pai, libera filhos. */
    /* ... */

    /* 5. Destroi page directory (libera frames fisicos com PTE_P=1). */
    pagedir_destroy (pd);
}
```

**Por que essa ordem?** `munmap_all` precisa do `pagedir` (para verificar dirty) e do ponteiro `file` (para write-back). `spt_destroy` usa `frame_remove` sem `palloc_free`. `pagedir_destroy` libera os frames fisicos — por isso o destrutor SPT nao chama `palloc_free_page`.

---

## 9. userprog/syscall.c — mmap, munmap e put_user

### 9.1 sys_mmap() — validacao e criacao lazy

**userprog/syscall.c — sys_mmap() (resumido)**
```c
static mapid_t
sys_mmap (int fd, void *addr) {
    /* Rejeita: addr NULL, nao alinhado, fd 0/1, arquivo vazio. */
    if (addr == NULL || pg_ofs (addr) != 0 || fd < 2) return -1;

    struct file *orig = get_file_from_fd (fd);
    if (orig == NULL) return -1;

    lock_acquire (&filesys_lock);
    struct file *mmap_file = file_reopen (orig);
    off_t file_len = file_length (mmap_file);
    lock_release (&filesys_lock);

    if (file_len == 0) { file_close (mmap_file); return -1; }

    size_t page_count = (file_len + PGSIZE - 1) / PGSIZE;

    /* Verifica que nenhuma pagina sobrepoe mapeamento existente. */
    for (size_t i = 0; i < page_count; i++)
        if (spt_find (&cur->spage_table, addr + i * PGSIZE) != NULL) {
            file_close (mmap_file);
            return -1;
        }

    /* Cria mmap_entry e SPT entries (PAGE_FILE, is_mmap=true). */
    struct mmap_entry *me = malloc (sizeof *me);
    me->mapid      = cur->next_mapid++;
    me->file       = mmap_file;
    me->addr       = addr;
    me->page_count = page_count;
    list_push_back (&cur->mmap_list, &me->elem);

    for (size_t i = 0; i < page_count; i++) {
        struct sup_page_entry *spte = malloc (sizeof *spte);
        spte->upage       = addr + i * PGSIZE;
        spte->location    = PAGE_FILE;
        spte->is_mmap     = true;
        spte->mapid       = me->mapid;
        spte->file        = mmap_file;
        spte->file_offset = i * PGSIZE;
        spte->read_bytes  = MIN (PGSIZE, file_len - i * PGSIZE);
        spte->zero_bytes  = PGSIZE - spte->read_bytes;
        spte->writable    = true;
        spte->dirty       = false;
        spt_insert (&cur->spage_table, spte);
    }
    return me->mapid;
}
```

**Por que `file_reopen`?** O fd do processo pode ser fechado a qualquer momento. `file_reopen` cria uma referencia independente para o mapeamento, garantindo que o arquivo permaneca acessivel durante toda a vida do mmap — mesmo apos `close(fd)`.

Paginas mmap sao carregadas **lazily**: somente ao primeiro acesso ocorre um page fault que resolve via SPT (`PAGE_FILE`).

### 9.2 sys_munmap()

```c
static void
sys_munmap (mapid_t mapid) {
    struct mmap_entry *me = find_mmap_entry (cur, mapid);
    if (me == NULL) return;
    munmap_entry (cur, me);
}
```

`munmap_all` percorre `mmap_list` e chama `munmap_entry` para cada entrada — chamado em `process_exit` para garantir write-back mesmo que o processo termine sem chamar `munmap` explicitamente.

### 9.3 put_user — validacao de escrita antes do lock

**userprog/syscall.c — put_user()**
```c
static bool
put_user (uint8_t *udst, uint8_t byte) {
    if (!is_user_vaddr (udst)) return false;
    int error_code;
    asm ("movl $1f, %0; movb %b2, %1; 1:"
         : "=&a" (error_code), "=m" (*udst) : "q" (byte));
    return error_code != (int) 0xffffffff;
}
```

`validate_buffer(..., writable=true)` chama `put_user` para verificar permissao de escrita **antes** de adquirir `filesys_lock`. Isso resolve o teste `pt-write-code2`:

| Sem `put_user` | Com `put_user` |
|---|---|
| `validate_buffer` carrega a pagina read-only | `put_user` tenta escrever na pagina read-only |
| `lock_acquire(&filesys_lock)` adquirido | Page fault na instrucao de escrita |
| `file_read` tenta escrever na pagina → page fault em modo kernel | Handler encontra `%eax` com label de recuperacao |
| Sem label de recuperacao → comportamento indefinido (timeout) | `put_user` retorna `false` → `exit(-1)` antes do lock |

---

## 10. threads/thread.h — Campos VM

```c
#ifdef VM
#include <hash.h>
#endif

/* Em struct thread, sob #ifdef VM: */
bool spt_initialized;               /* true apos spt_init() em start_process */
struct hash spage_table;            /* tabela de paginas suplementar */
struct list mmap_list;              /* entradas de mmap ativas */
int next_mapid;                     /* proximo ID de mmap a atribuir */
void *esp_saved;                    /* ESP do usuario salvo no syscall_handler */
```

Inicializados em `start_process` antes de qualquer `thread_exit()`:

```c
spt_init (&cur->spage_table);
list_init (&cur->mmap_list);
cur->next_mapid      = 1;
cur->esp_saved       = NULL;
cur->spt_initialized = true;
```

O campo `spt_initialized` (inicializado como `false` pelo palloc com `PAL_ZERO`) garante que threads de kernel e processos que falham antes de `spt_init` nao tenham `spt_destroy` chamado sobre estruturas invalidas.

---

## 11. threads/init.c e Makefile.build

### 11.1 threads/init.c — Inicializacao do subsistema VM

```c
#ifdef VM
  frame_init ();   /* inicializa lista e lock da frame table */
  swap_init ();    /* obtem BLOCK_SWAP e cria bitmap */
#endif
```

Chamadas apos `filesys_init()` e antes do primeiro processo de usuario. A ordem e obrigatoria: `swap_init` precisa que o sistema de blocos esteja pronto.

### 11.2 Makefile.build — vm_SRC e correcao do loader.bin

```makefile
# Ativa a compilacao dos tres modulos VM:
vm_SRC  = vm/frame.c
vm_SRC += vm/page.c
vm_SRC += vm/swap.c

# Correcao do loader.bin para GNU ld 2.45 (Fedora 43):
loader.bin: threads/loader.o
	$(OBJCOPY) --remove-section=.note.gnu.property $< threads/loader_stripped.o
	$(LD) -N -e 0 -Ttext 0x7c00 --oformat binary -o $@ threads/loader_stripped.o
	rm -f threads/loader_stripped.o
```

O GNU ld 2.45 insere automaticamente uma secao `.note.gnu.property` com LMA `0x080480b4`, fazendo o binario bruto ter 134 MB em vez de 512 bytes. `objcopy --remove-section` remove a secao antes da linkedicao, produzindo um `loader.bin` de exatamente 512 bytes, compativel com o script `pintos`.

---

## 12. Decisoes de Projeto

| Decisao | Alternativa Descartada | Justificativa |
|---|---|---|
| Hash table no SPT indexada por `upage` | Array linear ou lista encadeada | O(1) esperado para lookup no page fault; um processo pode ter centenas de paginas |
| `spt_initialized` como flag booleana | Verificar se `spage_table.bucket` e NULL | `palloc` com `PAL_ZERO` garante que `false` seja o estado inicial sem inicializacao extra |
| `file_read_at` sem `filesys_lock` em `spt_load_page` | Adquirir `filesys_lock` antes da leitura | Deadlock inevitavel: `SYS_READ` pode estar com o lock quando o page fault ocorre |
| Frame atualizado in-place antes de soltar `frame_lock` | Alocar novo `frame_entry` para o novo dono | Evita janela de corrida onde outro thread seleciona o mesmo frame como vitima |
| `pinned=true` ate `frame_unpin` apos `pagedir_set_page` | Nao pinnar frames | Sem pinning, a evicao poderia selecionar um frame que ainda nao tem mapeamento instalado |
| `munmap_all` antes de `spt_destroy` em `process_exit` | Destruir SPT e depois fazer write-back | `munmap_entry` precisa do `pagedir` para verificar dirty e de `spte->kpage` para copiar os dados |
| `frame_remove` sem `palloc_free` no destrutor SPT | Chamar `palloc_free_page` no destrutor | `pagedir_destroy` ja libera frames com `PTE_P=1`; `palloc_free_page` causaria double-free |
| `esp_saved` no `struct thread` | Passar ESP como argumento para o handler | O handler de page fault nao tem acesso ao frame de interrupcao da syscall; o campo e acessivel a qualquer momento |
| `put_user` antes de `filesys_lock` em SYS_READ | Verificar permissao de escrita dentro do lock | Sem isso, page fault em pagina read-only ocorre com o lock adquirido, causando deadlock |
| `file_reopen` para mmap | Reutilizar o ponteiro do fd | O fd pode ser fechado apos `mmap`; a referencia deve ser independente |
| Bitmap para slots de swap | Lista de slots livres | `bitmap_scan_and_flip` e O(n/word) e atomica; lista exigiria lock e malloc separados |

---

## 13. Invariantes do Sistema

| Invariante | Como e Garantido |
|---|---|
| Toda pagina virtual de usuario tem uma entrada SPT | `load_segment`, `setup_stack` e stack growth criam entradas antes de qualquer acesso |
| Frame fisico nunca e double-freed | `frame_remove` (sem `palloc_free`) no destrutor SPT; `pagedir_destroy` libera os frames |
| Pagina mmap suja sempre e escrita de volta | `munmap_entry` verifica `pagedir_is_dirty` OR `spte->dirty` |
| Evicao nao causa deadlock com `filesys_lock` | `frame_lock` e solto antes de qualquer I/O de arquivo ou swap |
| Frame pinado nao e evictado | `pinned=true` ate `frame_unpin` apos `pagedir_set_page` |
| Kernel nunca crasha por ponteiro invalido de usuario | `get_user`/`put_user` com recuperacao assembly; page fault em modo kernel usa label em `%eax` |
| Pilha nao cresce alem de 8 MB | Condicao `fault_addr >= PHYS_BASE - 8MB` verificada em cada stack growth |
| Swap nao estoura silenciosamente | `PANIC("swap: no free slot")` se `bitmap_scan_and_flip` retorna `BITMAP_ERROR` |
| `SYS_READ` em pagina read-only encerra antes do lock | `put_user` em `validate_buffer(..., true)` detecta falta de permissao antes de `filesys_lock` |
| Threads de kernel nao chamam `spt_destroy` | Campo `spt_initialized` (false por default via PAL_ZERO) protege `process_exit` |

---

*CIn/UFPE · Sistemas Operacionais · Projeto 3 — Virtual Memory (Conclusao) · 2025/2026*
