# Relatorio de Decisoes de Projeto
## Projeto 4 — File System

**Centro de Informatica — Universidade Federal de Pernambuco**
Disciplina: Sistemas Operacionais — 2025 / 2026

**Integrantes:**
- Filipe Baptistella Vieira (fbv@cin.ufpe.br)
- Manoel Lira de Carvalho (mlc6@cin.ufpe.br)
- Victor Pessoa Diniz (vpd@cin.ufpe.br)
- Tulio Fernando Carvalho de Lira (tfcl@cin.ufpe.br)

---

## 1. Sumario Executivo

Este relatorio documenta as decisoes de projeto do **Projeto 4 do PintOS — File System**: sistema de arquivos extensivel com buffer cache, arquivos de tamanho variavel, subdiretorios hierarquicos e persistencia. Sao cobertas as quatro grandes areas: buffer cache com evicao por relogio, inode indexado com blocos diretos e indiretos, subdiretorios com resolucao de caminhos absolutos e relativos, e cinco novas syscalls de sistema de arquivos.

| Area | Arquivos Modificados / Criados |
|---|---|
| Buffer Cache | filesys/cache.h (novo), filesys/cache.c (novo) |
| Inode indexado + crescimento | filesys/inode.h, filesys/inode.c |
| Subdiretorios + CWD | filesys/directory.h, filesys/directory.c, filesys/filesys.h, filesys/filesys.c |
| CWD por processo | threads/thread.h, userprog/process.c |
| Syscalls novas | userprog/syscall.c |
| Free map atualizado | filesys/free-map.c |
| Build | Makefile.build |

---

## 2. Visao Geral da Arquitetura

Antes do Projeto 4, o filesystem tinha:
- Inode com alocacao contigua (campo `start` + comprimento)
- Sem cache: cada leitura/escrita ia direto ao dispositivo
- Diretorio raiz unico, sem suporte a subdiretorios
- Sem crescimento de arquivo apos criacao

O Projeto 4 adiciona **quatro camadas**:

```
Processo usuario (syscall)
        |
        v
   syscall.c  <-- SYS_MKDIR, SYS_CHDIR, SYS_READDIR, SYS_ISDIR, SYS_INUMBER
        |
        v
   filesys.c  <-- resolve_parent: navega caminhos absolutos/relativos
        |
        v
   inode.c    <-- estrutura indexada; inode_write_at cresce o arquivo
        |
        v
   cache.c    <-- buffer cache: 64 slots, evicao clock
        |
        v
   block_read / block_write  (disco fisico)
```

---

## 3. Buffer Cache

### 3.1 Estrutura

```c
/* filesys/cache.c */
#define CACHE_SIZE 64

struct cache_slot {
    bool valid;
    bool dirty;
    int  clock_bit;
    block_sector_t sector;
    uint8_t data[BLOCK_SECTOR_SIZE];
};

static struct cache_slot cache[CACHE_SIZE];
static struct lock cache_lock;
static int clock_hand;
```

64 slots em um array estatico global. Um lock global serializa todas as operacoes — suficiente para passar os testes, pois o `filesys_lock` em `syscall.c` ja serializa chamadas do usuario.

### 3.2 Algoritmo de Relogio

`cache_evict` percorre o array circular ate encontrar um slot com `clock_bit == 0`. Slots com `clock_bit == 1` recebem segunda chance (bit zerado) e sao poupados. Slots sujos sao escritos em disco antes de serem reutilizados.

```c
/* filesys/cache.c — cache_evict() */
while (true) {
    struct cache_slot *s = &cache[clock_hand];
    clock_hand = (clock_hand + 1) % CACHE_SIZE;
    if (!s->valid)         return s;
    if (s->clock_bit) { s->clock_bit = 0; continue; }
    if (s->dirty)          block_write(fs_device, s->sector, s->data);
    s->valid = false;
    return s;
}
```

### 3.3 Integracao

- `cache_init()` e chamado dentro de `filesys_init()`, antes de `inode_init()` e `free_map_init()`.
- `cache_flush()` e chamado em `filesys_done()` para escrever todos os slots sujos em disco — garantindo persistencia.
- Todas as chamadas `block_read(fs_device, ...)` e `block_write(fs_device, ...)` em `inode.c` foram substituidas por `cache_read(...)` e `cache_write(...)`.

> **Por que nao cache assincrono?** Os testes do Projeto 4 validam corretude, nao throughput. O cache sincrono e suficiente para passar todos os testes e evita a complexidade de threads de background com deadlocks potenciais.

---

## 4. Inode Indexado (Arquivos Extensiveis)

### 4.1 Nova estrutura inode_disk

```c
/* filesys/inode.c */
#define INODE_DIRECT_CNT 12
#define INDIRECT_CNT     (BLOCK_SECTOR_SIZE / sizeof(block_sector_t))  /* 128 */

struct inode_disk {
    off_t length;                              /* 4 bytes */
    unsigned magic;                            /* 4 bytes */
    uint32_t is_dir;                           /* 4 bytes — 1 se diretorio */
    block_sector_t direct[INODE_DIRECT_CNT];   /* 48 bytes */
    block_sector_t indirect;                   /* 4 bytes */
    block_sector_t doubly_indirect;            /* 4 bytes */
    uint32_t unused[111];                      /* 444 bytes — padding ate 512 */
};
/* Total: 4+4+4+48+4+4+444 = 512 bytes exatos */
```

Capacidade maxima:
- Direto: 12 × 512 B = 6 KB
- Indireto: 128 × 512 B = 64 KB
- Duplo-indireto: 128 × 128 × 512 B ≈ 8 MB

### 4.2 byte_to_sector com indice

```c
static block_sector_t
index_to_sector(const struct inode_disk *disk, size_t idx)
{
    if (idx < INODE_DIRECT_CNT)
        return disk->direct[idx];
    idx -= INODE_DIRECT_CNT;

    if (idx < INDIRECT_CNT) {
        block_sector_t buf[INDIRECT_CNT];
        cache_read(disk->indirect, buf);
        return buf[idx];
    }
    idx -= INDIRECT_CNT;

    /* duplo-indireto */
    block_sector_t dbl[INDIRECT_CNT];
    cache_read(disk->doubly_indirect, dbl);
    block_sector_t indir[INDIRECT_CNT];
    cache_read(dbl[idx / INDIRECT_CNT], indir);
    return indir[idx % INDIRECT_CNT];
}
```

### 4.3 Crescimento do arquivo

`inode_write_at` detecta quando `offset + size > inode->data.length` e chama `inode_alloc` para alocar os setores necessarios. A funcao `inode_set_block` aloca blocos intermediarios (indireto, duplo-indireto) conforme necessario.

```c
/* filesys/inode.c — inode_write_at() */
if (offset + size > inode->data.length) {
    lock_acquire(&inode->lock);
    if (offset + size > inode->data.length) {
        if (!inode_alloc(&inode->data, offset + size)) {
            lock_release(&inode->lock);
            return 0;
        }
        cache_write(inode->sector, &inode->data);
    }
    lock_release(&inode->lock);
}
```

O padrao de double-checked locking evita que duas threads cresam o mesmo inode concorrentemente.

### 4.4 Liberacao de blocos em inode_close

Quando `inode->removed == true` e `open_cnt` chega a zero, `inode_free_blocks` percorre a estrutura indexada e libera cada setor individualmente no free-map:

```c
/* Ordem: direto → indireto → duplo-indireto */
static void
inode_free_blocks(struct inode_disk *disk) {
    /* libera blocos diretos */
    /* le bloco indireto, libera suas entradas, libera o proprio bloco */
    /* le bloco duplo-indireto, para cada sub-indireto: repete */
}
```

> **Mudanca de assinatura:** `inode_create` recebeu um terceiro parametro `bool is_dir`. Todos os callers foram atualizados: `filesys_create` (false), `dir_create` (true via inode interno), `free_map_create` (false).

---

## 5. Subdiretorios

### 5.1 Entradas . e ..

`dir_create(sector, parent_sector, entry_cnt)` agora recebe o setor do diretorio pai e insere imediatamente as entradas `.` e `..`:

```c
bool
dir_create(block_sector_t sector, block_sector_t parent_sector, size_t entry_cnt)
{
    if (!inode_create(sector, entry_cnt * sizeof(struct dir_entry), true))
        return false;
    struct dir *dir = dir_open(inode_open(sector));
    bool ok = dir_add(dir, ".", sector) && dir_add(dir, "..", parent_sector);
    dir_close(dir);
    return ok;
}
```

O diretorio raiz e criado com `dir_create(ROOT_DIR_SECTOR, ROOT_DIR_SECTOR, 16)` — o pai do raiz e ele proprio.

### 5.2 Resolucao de caminhos

`resolve_parent(path, name)` em `filesys.c` recebe um caminho (absoluto ou relativo) e retorna o diretorio pai aberto, deixando o ultimo componente em `name`.

```c
/* filesys/filesys.c — resolve_parent() */
static struct dir *
resolve_parent(const char *path, char name[NAME_MAX + 1])
{
    /* 1. escolhe ponto de partida: raiz se '/', CWD se relativo */
    /* 2. tokeniza com strtok_r por '/' */
    /* 3. percorre todos os componentes menos o ultimo (tok com next != NULL) */
    /* 4. retorna o diretorio pai e o ultimo componente em name */
}
```

Casos cobertos:
- `/a/b/c` → abre raiz, percorre `a/b/`, retorna dir de `b`, name = `c`
- `a/b` → abre CWD, percorre `a/`, retorna dir de `a`, name = `b`
- `file` → retorna CWD, name = `file`
- `/` → retorna NULL (nao ha pai para o raiz)

### 5.3 CWD por processo

Campo `block_sector_t cwd_sector` adicionado ao `struct thread` sob `#ifdef FILESYS`:

```c
/* threads/thread.h */
#ifdef FILESYS
    block_sector_t cwd_sector;       /* setor do diretorio de trabalho */
    struct dir *dir_table[MAX_FDS];  /* fds de diretorios abertos */
#endif
```

Inicializacao em `start_process` (userprog/process.c):

```c
/* herda do processo pai; usa raiz se pai nao tem CWD valido */
cur->cwd_sector = (parent_cwd != 0) ? parent_cwd : ROOT_DIR_SECTOR;
```

### 5.4 dir_readdir e dir_is_empty

`dir_readdir` pula `.` e `..` para que o programa de usuario nao os veja:

```c
if (e.in_use
    && strcmp(e.name, ".") != 0
    && strcmp(e.name, "..") != 0)
{
    strlcpy(name, e.name, NAME_MAX + 1);
    return true;
}
```

`dir_is_empty` usa a mesma logica para verificar se um diretorio pode ser removido.

### 5.5 dir_remove com verificacao de diretorio vazio

`dir_remove` verifica se a entrada e um diretorio e, em caso positivo, chama `dir_is_empty`. Diretorios nao-vazios nao sao removidos:

```c
if (inode_is_dir(inode)) {
    struct dir *sub = dir_open(inode_reopen(inode));
    bool empty = sub != NULL && dir_is_empty(sub);
    dir_close(sub);
    if (!empty) goto done;
}
```

---

## 6. Descritores de Arquivo para Diretorios

### 6.1 dir_table por processo

`fd_table[MAX_FDS]` continua armazenando `struct file *` para arquivos regulares. Um segundo array `dir_table[MAX_FDS]` armazena `struct dir *` para fds de diretorios. O invariante e: para um fd valido, exatamente um dos dois e nao-NULL.

```c
/* threads/thread.h */
struct file *fd_table[MAX_FDS];   /* arquivos regulares */
struct dir  *dir_table[MAX_FDS];  /* diretorios abertos */
```

### 6.2 SYS_OPEN com deteccao de tipo

```c
/* userprog/syscall.c — SYS_OPEN */
struct inode *inode = filesys_open_inode(filename);

if (inode_is_dir(inode))
    cur->dir_table[fd] = dir_open(inode);
else
    cur->fd_table[fd]  = file_open(inode);
```

`get_file_from_fd` retorna NULL para fds de diretorio, evitando que `SYS_READ`/`SYS_WRITE` operem sobre eles.

---

## 7. Novas Syscalls

| Syscall | Implementacao |
|---|---|
| `SYS_CHDIR` | `filesys_chdir(path)` atualiza `thread->cwd_sector` |
| `SYS_MKDIR` | `filesys_mkdir(path)` cria inode + `.`/`..` + entrada no pai |
| `SYS_READDIR` | `dir_readdir(dir, name)` sobre `dir_table[fd]`, pula `.`/`..` |
| `SYS_ISDIR` | verifica `dir_table[fd] != NULL` |
| `SYS_INUMBER` | `inode_get_inumber` sobre o inode do fd (arquivo ou diretorio) |

Todos os novos casos estao sob `#ifdef FILESYS` em `syscall.c`.

---

## 8. Persistencia

A persistencia e garantida por dois mecanismos:

1. **cache_flush() em filesys_done():** todos os slots sujos do cache sao escritos ao disco antes do sistema desligar.
2. **free-map escrito a cada alocacao/liberacao:** `free_map_allocate` e `free_map_release` ja chamavam `bitmap_write` — continua funcionando, agora com blocos individuais em vez de contiguos.

Os testes de persistencia (`*-persistence.ck`) reinicializam o sistema e verificam que as mudancas sobreviveram. O flush do cache garante que nao ha dados perdidos no buffer.

---

## 9. Invariantes do Sistema

| Invariante | Como e Garantido |
|---|---|
| `sizeof(inode_disk) == 512` | ASSERT em `inode_create`; campos dimensionados para exatamente 512 bytes |
| Arquivo cresce monotonicamente | `inode_alloc` so aumenta `length`; `inode_write_at` usa double-checked lock |
| Diretorio sempre tem `.` e `..` | `dir_create` insere ambos antes de retornar |
| Diretorio nao-vazio nao e removido | `dir_remove` chama `dir_is_empty` se `inode_is_dir` |
| `readdir` nunca retorna `.` ou `..` | `dir_readdir` pula explicitamente os dois |
| Cache flush antes do desligamento | `filesys_done` chama `cache_flush` |
| Blocos intermediarios zerados | `alloc_zero_sector` zera cada novo bloco via `cache_write(sec, zeros)` |
| CWD herdado pelo filho | `start_args.cwd_sector` propagado de pai para filho em `process_execute` |
| fd de diretorio nao aceita read/write | `get_file_from_fd` retorna NULL se `dir_table[fd] != NULL` |
| Descritores de diretorio fechados na saida | `process_exit` itera `dir_table` e chama `dir_close` |

---

## 10. Modificacoes por Arquivo

### 10.1 filesys/cache.h e filesys/cache.c (novos)

API publica: `cache_init`, `cache_read`, `cache_write`, `cache_flush`. Implementacao interna: array `cache[64]`, lock global, ponteiro de relogio.

### 10.2 filesys/inode.h

- `inode_create(sector, length, is_dir)` — novo parametro `is_dir`
- `inode_is_dir(inode)` — nova funcao

### 10.3 filesys/inode.c

Reescrito completamente:
- `struct inode_disk`: 12 diretos + indireto + duplo-indireto + `is_dir`
- `index_to_sector`, `inode_set_block`, `inode_alloc`, `inode_free_blocks`
- `inode_write_at` com crescimento automatico
- Lock por inode para proteger crescimento concorrente
- Todas as operacoes de bloco passam pelo cache

### 10.4 filesys/filesys.h e filesys/filesys.c

Novas funcoes: `filesys_mkdir`, `filesys_chdir`, `filesys_open_inode`.
Funcao interna: `resolve_parent` para resolucao de caminhos.
`filesys_init` chama `cache_init`; `filesys_done` chama `cache_flush`.

### 10.5 filesys/directory.h e filesys/directory.c

- `dir_create(sector, parent_sector, entry_cnt)` — novo parametro `parent_sector`
- `dir_is_empty(dir)` — nova funcao
- `dir_readdir` pula `.` e `..`
- `dir_remove` verifica `dir_is_empty` para diretorios

### 10.6 filesys/free-map.c

`inode_create(FREE_MAP_SECTOR, ..., false)` — atualizado para nova assinatura.

### 10.7 threads/thread.h

Campos adicionados sob `#ifdef FILESYS`:
```c
block_sector_t cwd_sector;
struct dir *dir_table[MAX_FDS];
```
Include de `devices/block.h` para `block_sector_t`.
Forward declaration `struct dir;`.

### 10.8 userprog/process.c

- `start_args` recebe `cwd_sector` para heranca do CWD
- `start_process` inicializa `cwd_sector` a partir do pai
- `process_exit` fecha entradas de `dir_table`

### 10.9 userprog/syscall.c

- `SYS_OPEN` usa `filesys_open_inode` + detecta diretorio
- `SYS_CLOSE` trata `fd_table` e `dir_table`
- Novos casos: `SYS_CHDIR`, `SYS_MKDIR`, `SYS_READDIR`, `SYS_ISDIR`, `SYS_INUMBER`
- `get_file_from_fd` retorna NULL para fds de diretorio
- Nova funcao `get_dir_from_fd`

---

## 11. Como Executar os Testes

```bash
cd src/filesys
make
cd build

# Teste individual:
pintos -v -k -T 60 --qemu --filesys-size=2 \
    -p tests/filesys/extended/dir-mkdir -a dir-mkdir \
    -- -q -f run dir-mkdir

# Suite completa:
make check
```

O ambiente de compilacao requer `i386-elf-gcc` (cross-compiler x86). Em Mac ARM64, usar Docker ou VM Linux x86.

---

## 12. Tabela de Testes Esperados

### 12.1 Testes de Funcionalidade

| Teste | O que valida |
|---|---|
| `grow-create` | Cria arquivo e cresce com escritas |
| `grow-seq-sm` / `grow-seq-lg` | Crescimento sequencial de arquivo |
| `grow-sparse` | Arquivo esparso com offset grande |
| `grow-two-files` | Dois arquivos crescendo simultaneamente |
| `grow-tell` | `tell` retorna posicao correta apos crescimento |
| `grow-file-size` | `filesize` reflete o crescimento |
| `grow-dir-lg` | Diretorio cresce alem de 16 entradas iniciais |
| `grow-root-sm` / `grow-root-lg` | Raiz cresce |
| `dir-mkdir` | Cria subdiretorio simples |
| `dir-mk-tree` | Cria arvore de diretorios aninhados |
| `dir-rmdir` | Remove diretorio vazio |
| `dir-rm-tree` | Remove arvore de diretorios |
| `dir-vine` | Estrutura profundamente aninhada |
| `dir-open` | Abre diretorio com `open()` |
| `dir-rm-cwd` | Remove diretorio de trabalho atual |
| `dir-rm-parent` | Remove diretorio pai com filho ainda aberto |
| `dir-rm-root` | Tentativa de remover raiz (deve falhar) |
| `dir-over-file` | Cria diretorio com nome ja usado por arquivo |
| `dir-under-file` | Cria arquivo com nome ja usado por diretorio |
| `dir-empty-name` | Nome vazio retorna falha |
| `syn-rw` | Escrita sincronizada de multiplos processos |

### 12.2 Testes de Persistencia

Cada teste acima tem uma variante `-persistence` que reinicia o sistema e verifica que o estado foi preservado.

### 12.3 Testes de Robustez

`dir-empty-name`, `dir-open`, `dir-over-file`, `dir-under-file`, `dir-rm-cwd`, `dir-rm-parent`, `dir-rm-root` — validam comportamento correto em casos extremos.

---

*CIn/UFPE · Sistemas Operacionais · Projeto 4 — File System · 2025/2026*
