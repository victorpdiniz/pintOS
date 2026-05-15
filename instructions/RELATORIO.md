# Relatorio de Decisoes de Projeto
## Projeto 3 — Virtual Memory

**Centro de Informatica — Universidade Federal de Pernambuco**
Disciplina: Sistemas Operacionais — 2025 / 2026

**Integrantes:**
- Filipe Baptistella Vieira (fbv@cin.ufpe.br)
- Manoel Lira de Carvalho (mlc6@cin.ufpe.br)
- Victor Pessoa Diniz (vpd@cin.ufpe.br)
- Tulio Fernando Carvalho de Lira (tfcl@cin.ufpe.br)

---

## 1. Sumario Executivo

Este relatorio documenta as decisoes de projeto da implementacao do **Projeto 3 do PintOS — Virtual Memory**: infraestrutura completa de memoria virtual. Sao cobertas as cinco grandes areas: tabela de paginas suplementar (SPT), tabela de molduras com algoritmo de relogio, swap, crescimento dinamico da pilha, e arquivos mapeados na memoria (mmap/munmap).

**Resultado dos testes: 113/113 (100%)**

| Area | Arquivos Modificados / Criados |
|---|---|
| Tabela de paginas suplementar (SPT) | vm/page.h, vm/page.c |
| Frame table com evicao | vm/frame.h, vm/frame.c |
| Swap | vm/swap.h, vm/swap.c |
| Handler de page fault | userprog/exception.c |
| Carregamento lento (lazy loading) | userprog/process.c |
| Syscalls mmap / munmap | userprog/syscall.c |
| Inicializacao do subsistema VM | threads/init.c |
| Campos VM no struct thread | threads/thread.h |
| Correcao do loader.bin | src/Makefile.build |

---

## 2. Visao Geral da Arquitetura

Antes do Projeto 3, o kernel alocava todos os frames de um executavel de uma vez em `load_segment` e a pilha era fixa em uma pagina. Nao havia tratamento de page fault, substituicao de paginas, swap, nem mapeamento de arquivos.

O Projeto 3 adiciona **tres camadas de abstracao**:

```
Processo usuario
      |
      v  page fault
  exception.c  <-- identifica causa (stack growth / lazy load / swap reclaim)
      |
      v
  vm/page.c    <-- SPT: o que deve estar nessa pagina virtual?
      |
      v
  vm/frame.c   <-- frame table: aloca frame fisico; evicta se necessario
      |
      v
  vm/swap.c    <-- swap: leitura/escrita de paginas no disco
```

**Invariante central:** toda pagina virtual de usuario e descrita por uma entrada `sup_page_entry` no SPT da thread. O frame fisico e alocado sob demanda, na primeira falha de pagina.

---

## 3. Tabela de Paginas Suplementar (SPT)

### 3.1 struct sup_page_entry

Cada entrada descreve **onde** os dados de uma pagina virtual estao e **como** carrega-la.

**vm/page.h — sup_page_entry**
```c
enum page_location {
    PAGE_ZERO,   /* zeros puro (stack growth ou BSS) */
    PAGE_FILE,   /* arquivo (executavel ou mmap) */
    PAGE_SWAP,   /* expulso para swap */
    PAGE_FRAME,  /* atualmente em frame fisico */
};

struct sup_page_entry {
    struct hash_elem hash_elem; /* na spage_table da thread */
    void *upage;                /* endereco virtual (alinhado a pagina) */

    enum page_location location;
    bool writable;
    bool dirty;                 /* acumulado para write-back */

    bool is_mmap;               /* pagina pertence a um mmap? */
    int  mapid;                 /* ID do mmap (quando is_mmap) */
    struct file *file;          /* arquivo de origem */
    off_t file_offset;          /* offset no arquivo */
    size_t read_bytes;          /* bytes lidos do arquivo */
    size_t zero_bytes;          /* bytes zerados apos a leitura */

    size_t swap_sector;         /* setor inicial no dispositivo de swap */
    void  *kpage;               /* endereco virtual do kernel (PAGE_FRAME) */
};
```

### 3.2 Hash table por processo

O SPT usa `struct hash` do PintOS, keyed pelo `upage`. A inicializacao ocorre em `start_process`, antes de qualquer possivel `thread_exit()`. O campo booleano `spt_initialized` no `struct thread` garante que `process_exit` nao destrua um SPT nao-inicializado (ex: threads de kernel).

**vm/page.c — hash functions**
```c
static unsigned page_hash(const struct hash_elem *e, void *aux UNUSED) {
    const struct sup_page_entry *spte = hash_entry(e, ...);
    return hash_bytes(&spte->upage, sizeof spte->upage);
}

static bool page_less(const struct hash_elem *a,
                      const struct hash_elem *b, void *aux UNUSED) {
    return hash_entry(a,...)->upage < hash_entry(b,...)->upage;
}
```

### 3.3 spt_load_page() — carregamento sob demanda

Chamada pelo handler de page fault. Aloca um frame, popula com os dados corretos e instala no page directory. Usa `file_read_at` (em vez de seek+read) para evitar deadlock com `filesys_lock`.

**vm/page.c — spt_load_page()**
```c
bool spt_load_page(struct sup_page_entry *spte) {
    void *kpage = frame_alloc(spte, PAL_USER | (PAGE_ZERO ? PAL_ZERO : 0));
    if (!kpage) return false;

    switch (spte->location) {
    case PAGE_ZERO:  memset(kpage, 0, PGSIZE); break;
    case PAGE_FILE:
        file_read_at(spte->file, kpage, spte->read_bytes, spte->file_offset);
        memset(kpage + spte->read_bytes, 0, spte->zero_bytes);
        break;
    case PAGE_SWAP:
        swap_read(spte->swap_sector, kpage);
        swap_free(spte->swap_sector);
        break;
    }

    pagedir_set_page(thread_current()->pagedir, spte->upage, kpage, spte->writable);
    spte->kpage = kpage;
    spte->location = PAGE_FRAME;
    frame_unpin(kpage);   /* permite evicao apos instalacao */
    return true;
}
```

> **Por que `file_read_at` sem `filesys_lock`?** A funcao `file_read_at` acessa o inode em offset explicito sem modificar a posicao interna do arquivo. O executavel tem `file_deny_write` ativo, tornando leituras concorrentes seguras. Adquirir `filesys_lock` aqui causaria deadlock: a syscall `SYS_READ` ja pode estar segurando o lock quando o page fault ocorre dentro de `file_read`.

### 3.4 Destruicao do SPT em process_exit

O destrutor `page_free_entry` e chamado por `spt_destroy` (via `hash_destroy`) para cada entrada:

- **PAGE_FRAME:** chama `frame_remove` (sem palloc_free) — `pagedir_destroy` libera o frame fisico.
- **PAGE_SWAP:** libera o slot de swap com `swap_free`.
- **PAGE_FILE / PAGE_ZERO:** nada a liberar.

> **Ordem obrigatoria:** `munmap_all` antes de `spt_destroy`. Paginas mmap com `is_mmap=true` ja terao sido tratadas (write-back + liberacao do frame) por `munmap_all`.

---

## 4. Frame Table

### 4.1 struct frame_entry

Tabela global que rastreia todos os frames de usuario alocados.

**vm/frame.h — frame_entry**
```c
struct frame_entry {
    struct list_elem elem;
    void *kpage;                 /* endereco virtual do kernel */
    struct thread *owner;        /* thread dona do frame */
    struct sup_page_entry *spte; /* entrada SPT correspondente */
    bool pinned;                 /* true = nao pode ser evicado */
};
```

### 4.2 frame_alloc() — alocacao com evicao

**vm/frame.c — frame_alloc()**
```c
void *frame_alloc(struct sup_page_entry *spte, enum palloc_flags flags) {
    lock_acquire(&frame_lock);
    void *kpage = palloc_get_page(flags);
    if (kpage != NULL) {
        /* cria frame_entry, pinned=true, adiciona a lista */
        lock_release(&frame_lock);
        return kpage;
    }
    /* memoria cheia: evicta um frame */
    kpage = frame_evict(spte, flags);
    lock_release(&frame_lock);
    return kpage;
}
```

Frames sao criados com `pinned = true`. O pin so e removido apos `pagedir_set_page` em `spt_load_page`, evitando que o frame seja evicado enquanto esta sendo populado.

### 4.3 Algoritmo de Relogio (Clock / Second-Chance)

`frame_evict` percorre a lista circular ate encontrar um frame nao-pinado cujo bit accessed esteja limpo. Frames com accessed=true tem o bit zerado e sao poupados (segunda chance).

**vm/frame.c — frame_evict() (resumido)**
```c
static void *frame_evict(struct sup_page_entry *new_spte,
                          enum palloc_flags flags) {
    size_t n = list_size(&frame_table) * 2 + 2;
    for (size_t i = 0; i < n; i++) {
        struct frame_entry *fte = list_entry(clock_hand, ...);
        avanca_clock();
        if (fte->pinned) continue;
        if (pagedir_is_accessed(fte->owner->pagedir, fte->spte->upage)) {
            pagedir_set_accessed(..., false);  /* segunda chance */
            continue;
        }
        /* vitima encontrada */
        bool dirty = pagedir_is_dirty(...) || fte->spte->dirty;
        pagedir_clear_page(fte->owner->pagedir, fte->spte->upage);
        fte->spte->kpage = NULL;
        fte->spte->dirty = dirty;
        /* atualiza frame_entry para novo dono ANTES de soltar o lock */
        fte->spte = new_spte;
        fte->owner = thread_current();
        fte->pinned = true;
        lock_release(&frame_lock);
        /* I/O sem lock */
        if (old_spte->is_mmap && dirty)
            file_write_at(old_spte->file, kpage, ...);  /* write-back */
        else
            old_spte->swap_sector = swap_write(kpage);
        if (flags & PAL_ZERO) memset(kpage, 0, PGSIZE);
        lock_acquire(&frame_lock);
        return kpage;
    }
    return NULL;
}
```

> **Por que soltar o lock antes do I/O?** `swap_write` e `file_write_at` podem bloquear. Manter `frame_lock` durante I/O criaria deadlock com `filesys_lock`. A solucao e atualizar o `frame_entry` in-place antes de soltar o lock — o frame fica "reservado" para o novo dono sem que outro thread o selecione como vitima.

### 4.4 frame_remove vs frame_free

- `frame_free(kpage)`: remove da tabela + `palloc_free_page`. Usado em `munmap_entry` e em erros de `spt_load_page`.
- `frame_remove(kpage)`: remove da tabela **sem** `palloc_free_page`. Usado no destrutor do SPT — `pagedir_destroy` ja libera os frames fisicos com PTE_P=1, evitando double-free.

---

## 5. Swap

### 5.1 Estrutura

**vm/swap.c**
```c
static struct block  *swap_block;   /* dispositivo BLOCK_SWAP */
static struct bitmap *swap_bitmap;  /* 1 bit por slot de pagina */
static struct lock    swap_lock;

#define SECTORS_PER_PAGE  (PGSIZE / BLOCK_SECTOR_SIZE)  /* = 8 */
```

O bitmap rastreia **slots** (uma pagina = 8 setores de 512 bytes). `bitmap_scan_and_flip` encontra um slot livre atomicamente.

### 5.2 swap_write() e swap_read()

**vm/swap.c — swap_write()**
```c
size_t swap_write(void *kpage) {
    lock_acquire(&swap_lock);
    size_t slot = bitmap_scan_and_flip(swap_bitmap, 0, 1, false);
    lock_release(&swap_lock);
    if (slot == BITMAP_ERROR) PANIC("swap: no free slot");
    size_t base = slot * SECTORS_PER_PAGE;
    for (size_t i = 0; i < SECTORS_PER_PAGE; i++)
        block_write(swap_block, base + i,
                    (uint8_t *)kpage + i * BLOCK_SECTOR_SIZE);
    return base;   /* indice do setor inicial */
}
```

`swap_free` usa `bitmap_set(swap_bitmap, sector/SECTORS_PER_PAGE, false)` para marcar o slot como livre.

---

## 6. Crescimento da Pilha (Stack Growth)

### 6.1 Deteccao no handler de page fault

O crescimento da pilha e detectado quando a pagina faltante nao esta no SPT mas satisfaz:

```c
/* userprog/exception.c — page_fault() */
if (spte == NULL
    && user_esp != NULL
    && (uintptr_t)fault_addr < (uintptr_t)PHYS_BASE
    && (uintptr_t)fault_addr >= (uintptr_t)PHYS_BASE - 8*1024*1024
    && (uintptr_t)fault_addr >= (uintptr_t)user_esp - 32)
{
    /* valido: cria entrada PAGE_ZERO e carrega */
}
```

| Condicao | Razao |
|---|---|
| `fault_addr < PHYS_BASE` | Endereco de usuario |
| `>= PHYS_BASE - 8 MB` | Pilha limitada a 8 MB |
| `>= user_esp - 32` | Cobre a instrucao PUSHA (empurra 32 bytes abaixo de esp) |

### 6.2 esp_saved — ESP do usuario em contexto de kernel

Quando o page fault ocorre em modo kernel (syscall acessando buffer do usuario), `f->esp` aponta para a pilha do kernel. O ESP real do usuario e salvo no inicio do syscall_handler:

**userprog/syscall.c**
```c
void syscall_handler(struct intr_frame *f) {
    thread_current()->esp_saved = f->esp;   /* salva ESP do usuario */
    ...
    thread_current()->esp_saved = NULL;     /* limpa ao sair */
}
```

**userprog/exception.c**
```c
void *user_esp = user ? f->esp : cur->esp_saved;
```

### 6.3 Recuperacao de page fault em modo kernel (get_user/put_user pattern)

Quando o kernel acessa um endereco invalido de usuario, a assembly inline salva o endereco de recuperacao em `%eax`. Se o page fault nao puder ser resolvido:

```c
/* userprog/exception.c — fault em modo kernel */
f->eip = (void (*)(void)) f->eax;   /* salta para label de recuperacao */
f->eax = 0xffffffff;                /* get_user retorna -1, put_user retorna false */
return;
```

### 6.4 put_user — validacao de escrita antes do lock

Para o syscall `SYS_READ`, o buffer do usuario precisa ser **gravavel**. A funcao `validate_buffer(..., true)` chama `put_user` para verificar permissao de escrita **antes** de adquirir `filesys_lock`. Isso garante que o processo seja encerrado com exit(-1) antes de entrar na regiao critica, evitando deadlock.

**userprog/syscall.c — put_user()**
```c
static bool put_user(uint8_t *udst, uint8_t byte) {
    if (!is_user_vaddr(udst)) return false;
    int error_code;
    asm ("movl $1f, %0; movb %b2, %1; 1:"
         : "=&a" (error_code), "=m" (*udst) : "q" (byte));
    return error_code != (int) 0xffffffff;
}
```

> **Por que put_user e necessario para pt-write-code2?** Sem ele, o kernel tenta escrever em uma pagina read-only (codigo) enquanto segura `filesys_lock`. O page fault resultante nao tem endereco de recuperacao valido em `%eax`, causando comportamento indefinido. Com `put_user` validando antes do lock, o processo e encerrado corretamente com exit(-1).

---

## 7. Arquivos Mapeados na Memoria (mmap / munmap)

### 7.1 struct mmap_entry

Cada chamada `mmap` bem-sucedida cria uma entrada na lista `mmap_list` da thread:

**vm/page.h — mmap_entry**
```c
struct mmap_entry {
    int mapid;
    struct file *file;    /* referencia independente (file_reopen) */
    void *addr;           /* inicio da regiao mapeada */
    size_t page_count;    /* numero de paginas */
    struct list_elem elem;
};
```

### 7.2 sys_mmap() — validacao e criacao lazy

**userprog/syscall.c — sys_mmap() (resumido)**
```c
static mapid_t sys_mmap(int fd, void *addr) {
    /* rejeita: addr NULL, nao alinhado, fd invalido, arquivo vazio */
    struct file *mmap_file = file_reopen(orig); /* referencia propria */
    off_t file_len = file_length(mmap_file);
    size_t page_count = (file_len + PGSIZE - 1) / PGSIZE;

    /* verifica que nenhuma pagina sobrepoe mapeamento existente */
    for (size_t i = 0; i < page_count; i++)
        if (spt_find(&cur->spage_table, addr + i*PGSIZE) != NULL)
            return -1;

    /* cria mmap_entry e SPT entries (PAGE_FILE, is_mmap=true) */
    for (size_t i = 0; i < page_count; i++) {
        spte->location   = PAGE_FILE;
        spte->is_mmap    = true;
        spte->file       = mmap_file;
        spte->file_offset = i * PGSIZE;
        /* ... insere no SPT ... */
    }
    return me->mapid;
}
```

Paginas mmap sao carregadas **lazily**: somente quando o processo acessa o endereco ocorre um page fault, que resolve via SPT (PAGE_FILE).

### 7.3 munmap_entry() — write-back e liberacao

**vm/page.c — munmap_entry() (resumido)**
```c
void munmap_entry(struct thread *t, struct mmap_entry *me) {
    for (size_t i = 0; i < me->page_count; i++) {
        struct sup_page_entry *spte = spt_find(&t->spage_table, upage);

        if (spte->location == PAGE_FRAME) {
            bool dirty = pagedir_is_dirty(t->pagedir, upage) || spte->dirty;
            if (dirty) {  /* write-back para arquivo */
                lock_acquire(&filesys_lock);
                file_write_at(me->file, spte->kpage,
                              spte->read_bytes, spte->file_offset);
                lock_release(&filesys_lock);
            }
            pagedir_clear_page(t->pagedir, upage);
            frame_free(spte->kpage);
        } else if (spte->location == PAGE_SWAP && spte->dirty) {
            /* le do swap e escreve no arquivo */
            void *buf = palloc_get_page(0);
            swap_read(spte->swap_sector, buf);
            file_write_at(me->file, buf, spte->read_bytes, spte->file_offset);
            palloc_free_page(buf);
        } else if (spte->location == PAGE_SWAP) {
            swap_free(spte->swap_sector);
        }
        hash_delete(&t->spage_table, &spte->hash_elem);
        free(spte);
    }
    file_close(me->file);
    list_remove(&me->elem);
    free(me);
}
```

### 7.4 Ordem de liberacao em process_exit

```c
void process_exit(void) {
    /* 1. fecha fd_table e executable */
    lock_acquire(&filesys_lock);
    /* fecha fd_table[2..MAX_FDS] e executable */
    lock_release(&filesys_lock);

    /* 2. write-back de todas as paginas mmap sujas */
    munmap_all(cur);

    /* 3. destroi SPT (libera slots de swap, remove do frame table) */
    spt_destroy(&cur->spage_table);

    /* 4. sinaliza pai, libera filhos */
    ...

    /* 5. destroi page directory (libera frames fisicos) */
    pagedir_destroy(pd);
}
```

> **Por que essa ordem?** `munmap_all` precisa do `pagedir` para verificar bits dirty e de `file` para o write-back. `spt_destroy` precisa que as entradas mmap ja tenham sido removidas. `pagedir_destroy` libera os frames fisicos — por isso `frame_remove` (sem `palloc_free_page`) e chamado no destrutor do SPT para evitar double-free.

---

## 8. Carregamento Lento do Executavel (Lazy Loading)

### 8.1 load_segment() com SPT

Antes: `load_segment` alocava um frame e lia o arquivo imediatamente para cada pagina.

Agora: cria entradas SPT (PAGE_FILE ou PAGE_ZERO) sem alocar frames.

**userprog/process.c — load_segment() com VM**
```c
static bool load_segment(...) {
    off_t file_offset = ofs;
    while (read_bytes > 0 || zero_bytes > 0) {
        struct sup_page_entry *spte = malloc(sizeof *spte);
        spte->upage       = upage;
        spte->location    = (page_read_bytes == 0) ? PAGE_ZERO : PAGE_FILE;
        spte->file        = file;
        spte->file_offset = file_offset;
        spte->read_bytes  = page_read_bytes;
        spte->zero_bytes  = page_zero_bytes;
        spte->writable    = writable;
        spte->is_mmap     = false;
        spt_insert(&thread_current()->spage_table, spte);

        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
        file_offset += page_read_bytes;
    }
    return true;
}
```

> **Vantagem:** processos que nunca acessam certas paginas do codigo nunca incorrem no custo de carregar essas paginas.

### 8.2 setup_stack() com SPT

A pilha inicial usa o SPT — cria uma entrada PAGE_ZERO e chama `spt_load_page` imediatamente (a pilha deve existir antes de executar qualquer instrucao).

---

## 9. Modificacoes em threads/thread.h

Campos adicionados ao `struct thread` sob `#ifdef VM`:

```c
#ifdef VM
    bool spt_initialized;      /* true apos spt_init() em start_process */
    struct hash spage_table;   /* tabela de paginas suplementar */
    struct list mmap_list;     /* entradas de mmap ativas */
    int next_mapid;            /* proximo ID de mmap a atribuir */
    void *esp_saved;           /* ESP do usuario salvo no syscall_handler */
#endif
```

O campo `spt_initialized` (inicializado como `false` pelo palloc com PAL_ZERO) garante que threads de kernel e processos que falham antes de `spt_init` nao tenham `spt_destroy` chamado sobre estruturas invalidas.

Inicializados em `start_process` (antes de qualquer `thread_exit()`):
```c
spt_init(&cur->spage_table);
list_init(&cur->mmap_list);
cur->next_mapid = 1;
cur->esp_saved  = NULL;
cur->spt_initialized = true;
```

---

## 10. Inicializacao do Subsistema VM (threads/init.c)

`frame_init` e `swap_init` sao chamados apos a inicializacao do filesystem, dentro de `#ifdef VM`:

```c
#ifdef VM
  frame_init();   /* inicializa lista e lock do frame table */
  swap_init();    /* obtem BLOCK_SWAP e cria bitmap */
#endif
```

---

## 11. Correcao do loader.bin (Makefile.build)

O GNU ld 2.45 (Fedora 43) insere uma secao `.note.gnu.property` com LMA `0x080480b4`, fazendo o binario bruto ter 134 MB em vez de 512 bytes. A correcao usa `objcopy` para remover a secao antes da linkedicao:

**src/Makefile.build**
```makefile
loader.bin: threads/loader.o
    $(OBJCOPY) --remove-section=.note.gnu.property $< threads/loader_stripped.o
    $(LD) -N -e 0 -Ttext 0x7c00 --oformat binary -o $@ threads/loader_stripped.o
    rm -f threads/loader_stripped.o
```

Resultado: `loader.bin` com 512 bytes exatos, compativel com o script `pintos`.

---

## 12. Invariantes do Sistema

| Invariante | Como e Garantido |
|---|---|
| Toda pagina virtual de usuario tem uma entrada SPT | `load_segment`, `setup_stack` e stack growth criam entradas antes de qualquer acesso |
| Frame fisico nunca e double-freed | `frame_remove` (sem palloc_free) no destrutor SPT; `pagedir_destroy` libera os frames com PTE_P=1 |
| Pagina mmap suja sempre e escrita de volta | `munmap_entry` verifica `pagedir_is_dirty` OR `spte->dirty` antes de escrever |
| Evicao nao causa deadlock com filesys_lock | `frame_lock` e solto antes de qualquer I/O de arquivo ou swap |
| Kernel nunca crasha por ponteiro invalido de usuario | `get_user`/`put_user` com recuperacao assembly; page fault em modo kernel redireciona para label de recuperacao |
| Pilha nao cresce alem de 8 MB | Condicao `fault_addr >= PHYS_BASE - 8MB` verificada em cada stack growth |
| Frame pinado nao e evicado | `pinned=true` ate `frame_unpin` apos `pagedir_set_page` |
| Swap nao estoura silenciosamente | `PANIC("swap: no free slot")` se `bitmap_scan_and_flip` retorna `BITMAP_ERROR` |
| SYS_READ em pagina read-only encerra o processo antes do lock | `put_user` em `validate_buffer(..., true)` detecta falta de permissao de escrita antes de `filesys_lock` |
| threads de kernel nao chamam spt_destroy | Campo `spt_initialized` (false por default) protege `process_exit` |

---

## 13. Testes

### 13.1 Resultados Finais

**113/113 testes passaram (100%)**

| Categoria | Passaram | Falharam |
|---|---|---|
| userprog (todos) | 80 | 0 |
| vm/pt-* (page table / stack) | 10 | 0 |
| vm/page-* (swap / eviction) | 7 | 0 |
| vm/mmap-* | 16 | 0 |
| filesys/base/* | 13 | 0 |

### 13.2 Testes Selecionados

| Teste | O que Valida |
|---|---|
| `pt-grow-stack` | Crescimento basico da pilha por page fault |
| `pt-grow-pusha` | Stack growth com PUSHA (32 bytes abaixo de esp) |
| `pt-grow-bad` | Acesso invalido abaixo do limite da pilha retorna -1 |
| `pt-big-stk-obj` | Objeto grande na pilha — multiplos pages de crescimento |
| `pt-bad-addr` | Acesso a endereco invalido mata o processo |
| `pt-write-code` | Escrita no segmento de codigo e rejeitada |
| `pt-write-code2` | Escrita no codigo via syscall read() detectada com put_user |
| `pt-grow-stk-sc` | Stack growth disparado dentro de syscall (usa esp_saved) |
| `page-linear` | Lazy loading + leitura linear de paginas do executavel |
| `page-merge-seq` | Multiplos processos compartilhando paginas via swap/eviction |
| `page-shuffle` | Acesso aleatorio a muitas paginas sob pressao de memoria |
| `mmap-read` | Leitura de arquivo via mmap |
| `mmap-write` | Escrita em mmap e write-back correto ao arquivo |
| `mmap-unmap` | munmap explicito com write-back |
| `mmap-exit` | write-back implicito ao encerrar processo sem munmap |
| `mmap-overlap` | mmap em regiao ja mapeada retorna -1 |
| `mmap-over-code` | mmap sobre segmento de codigo e rejeitado |
| `mmap-remove` | Arquivo removido enquanto mapeado continua acessivel |
| `mmap-close` | Fechar fd nao afeta mapeamento em vigor |

### 13.3 Decisao de Projeto: pt-write-code2

Este teste falhou inicialmente por um problema sutil de deadlock:

1. `SYS_READ(handle, test_main, 1)` — buffer aponta para o segmento de codigo (read-only)
2. `validate_buffer` carregava a pagina como read-only (via page fault + SPT)
3. `lock_acquire(&filesys_lock)` era adquirido
4. `file_read` tentava **escrever** na pagina read-only → page fault em modo kernel
5. Sem endereco de recuperacao valido em `%eax` → comportamento indefinido (timeout)

**Solucao:** `validate_buffer(..., writable=true)` chama `put_user` para verificar permissao de escrita **antes** de adquirir `filesys_lock`. `put_user` usa o mesmo padrao de recuperacao assembly do `get_user`, portanto o page fault e tratado corretamente (retorna `false`), e o processo e encerrado com `exit(-1)` antes do lock ser adquirido.

### 13.4 Executar os testes

```bash
cd src/vm
make
export PATH="/path/to/pintos/src/utils:$PATH"
cd build

# teste individual:
pintos -v -k -T 60 --qemu --filesys-size=2 \
    -p tests/vm/mmap-read -a mmap-read \
    --swap-size=4 -- -q -f run mmap-read

# suite completa:
make check
```

---

*CIn/UFPE · Sistemas Operacionais · Projeto 3 — Virtual Memory · 2025/2026*
