/*
 * The MIT License (MIT)
 *
 * Copyright © 2016 Franklin "Snaipe" Mathieu <http://snai.pe/>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "mimick/assert.h"

#include "config.h"
#include "plt.h"
#include "trampoline.h"
#include "vitals.h"

#if MMK_BITS == 32
typedef Elf32_Word ElfWord;
typedef Elf32_Sword ElfSWord;
# ifndef ELF_R_SYM
#  define ELF_R_SYM(i) ELF32_R_SYM(i)
# endif
#elif MMK_BITS == 64
typedef Elf64_Xword ElfWord;
typedef Elf64_Sxword ElfSWord;
# ifndef ELF_R_SYM
#  define ELF_R_SYM(i) ELF64_R_SYM(i)
# endif
#else
# error Unsupported architecture
#endif

#if defined HAVE_ELF_AUXV_T
typedef ElfW(auxv_t) ElfAux;
#elif defined HAVE_ELF_AUXINFO
typedef ElfW(Auxinfo) ElfAux;
#else
# error Unsupported platform
#endif

extern char **environ;

static plt_fn **plt_get_offset(plt_lib lib, const char *name);

static void *lib_dt_lookup(plt_lib lib, ElfSWord tag)
{
    ElfW(Addr) base =(ElfW(Addr)) lib->l_addr;
    for (const ElfW(Dyn) *dyn = lib->l_ld; dyn->d_tag != DT_NULL; ++dyn) {
        if (dyn->d_tag == tag) {
            if (dyn->d_un.d_ptr >= base
                    && (dyn->d_un.d_ptr >> (MMK_BITS - 8)) ^ 0xff)
                return (void*) dyn->d_un.d_ptr;
            else
                return (char*) base + dyn->d_un.d_ptr;
        }
    }
    return NULL;
}

static ElfWord lib_dt_lookup_val(plt_lib lib, ElfSWord tag)
{
    for (const ElfW(Dyn) *dyn = lib->l_ld; dyn->d_tag != DT_NULL; ++dyn) {
        if (dyn->d_tag == tag) {
            return dyn->d_un.d_val;
        }
    }
    return 0;
}

#if !defined HAVE__R_DEBUG
static ElfW(Addr) get_auxval(ElfAux *auxv, ElfW(Off) tag)
{
    for (; auxv->a_type != AT_NULL; auxv++) {
        if (auxv->a_type == tag)
            return auxv->a_un.a_val;
    }
    return (ElfW(Addr)) -1;
}

static ElfW(Addr) find_dynamic(ElfW(Phdr) *phdr, ElfW(Off) phent)
{
    for (ElfW(Off) i = 0; i < phent; ++i) {
        if (phdr[i].p_type == PT_DYNAMIC)
            return phdr[i].p_vaddr;
    }
    return 0;
}

static struct r_debug *r_debug_from_dynamic(void *dynamic)
{
    for (const ElfW(Dyn) *dyn = dynamic; dyn->d_tag != DT_NULL; ++dyn) {
        if (dyn->d_tag == DT_DEBUG)
            return (struct r_debug *) dyn->d_un.d_ptr;
    }
    return NULL;
}
#endif

static struct r_debug *get_r_debug(void)
{
    // Find our own r_debug
    struct r_debug *dbg = NULL;

    // First use some known shortcuts
#if defined HAVE__R_DEBUG
    dbg = &_r_debug;
#elif defined HAVE__DYNAMIC
    dbg = r_debug_from_dynamic(_DYNAMIC);
#endif

#if !defined HAVE__R_DEBUG
    // If there are no available shortcuts, we manually query our own phdrs
# if defined HAVE__DYNAMIC
    if (!dbg) {
# endif
        char **envp = environ;
        while (*envp++ != NULL);
        ElfAux *auxv = (ElfAux*) envp;
        ElfW(Addr) phdr = get_auxval(auxv, AT_PHDR);
        ElfW(Addr) phent = get_auxval(auxv, AT_PHENT);
        if (phdr != (ElfW(Addr)) -1 && phent != (ElfW(Addr)) -1) {
            ElfW(Addr) dynamic = find_dynamic((void*) phdr, phent);
            dbg = r_debug_from_dynamic((void*) dynamic);
        }
# if defined HAVE__DYNAMIC
    }
# endif
#endif

    return dbg;
}

plt_ctx plt_init_ctx(void)
{
    static struct r_debug *dbg = (void*) -1;
    if (dbg == (void*) -1)
        dbg = get_r_debug();
    return dbg;
}

plt_lib plt_get_lib(plt_ctx ctx, const char *name)
{
    if (!name)
        return ctx->r_map;

    const char *val = NULL;
    enum plt_selector sel = plt_get_selector(name, &val);
    size_t val_len = strlen(val);
    int libc = !strcmp(val, "c");

    for (struct link_map *lm = ctx->r_map; lm != NULL; lm = lm->l_next) {
        if (sel == PLT_SEL_LIB) {
            if (libc) {
                if (strstr(lm->l_name, "/libc.so")
                 || strstr(lm->l_name, "/musl.so"))
                    return lm;
            } else {
                size_t len = val_len + 8;
                char pattern[len];
                snprintf(pattern, len, "/lib%s.so", val);
                if (strstr(lm->l_name, pattern))
                    return lm;
            }
        } else if (sel == PLT_SEL_NONE || sel == PLT_SEL_FILE) {
            if (!strcmp(name, lm->l_name))
                return lm;
        } else if (sel == PLT_SEL_SYM) {
            plt_fn **sym = plt_get_offset(lm, val);
            if (sym)
                return lm;
        }
    }
    return NULL;
}

struct rel_info {
    ElfW(Rel) *tab;
    ElfWord size;
    ElfWord entry_sz;
};

static uintptr_t get_offset(struct rel_info *info, ElfW(Sym) *symtab,
        const char *strtab, const char *name)
{
    ElfW(Rel) *rel = info->tab;
    for (ElfWord i = 0; i < info->size / info->entry_sz;
            ++i, rel = (void*)(((char *) rel) + info->entry_sz))
    {
        ElfW(Sym) *sym = &symtab[ELF_R_SYM(rel->r_info)];

        if (!strcmp(strtab + sym->st_name, name))
            return (uintptr_t) rel->r_offset;
    }
    return 0;
}

static plt_fn **plt_get_offset(plt_lib lib, const char *name)
{
    ElfW(Sym) *symtab   = (ElfW(Sym)*)  lib_dt_lookup(lib, DT_SYMTAB);
    const char *strtab  = (const char*) lib_dt_lookup(lib, DT_STRTAB);
    ElfSWord type = (ElfSWord) lib_dt_lookup_val(lib, DT_PLTREL);

    ElfW(Rel)   *rel = lib_dt_lookup(lib, DT_JMPREL);
    ElfWord rel_sz = lib_dt_lookup_val(lib, DT_PLTRELSZ);
    ElfWord relent_sz = lib_dt_lookup_val(lib, type + 2);

    if (!symtab || !strtab || !type || !rel || !rel_sz || !relent_sz)
        return NULL;

    struct rel_info info = {
        .tab = rel,
        .size = rel_sz,
        .entry_sz = relent_sz,
    };

    ElfW(Addr) base = (ElfW(Addr)) lib->l_addr;
#ifdef __FreeBSD__
    if (base == 0x400000)
        base = 0;
#endif

    uintptr_t off = get_offset(&info, symtab, strtab, name);
    if (off)
        return (plt_fn **) (off + base);
    return NULL;
}

plt_offset *plt_get_offsets(plt_lib lib, const char *name, size_t *n)
{
    plt_fn **off = plt_get_offset(lib, name);
    if (off) {
        plt_offset *ot = mmk_malloc(sizeof (plt_offset));
        *n = 1;
        *ot = (plt_offset) { .offset = off };
        return ot;
    }
    return NULL;
}

void plt_set_offsets(plt_offset *offset, size_t nb_off, plt_fn *newval)
{
    for (size_t i = 0; i < nb_off; ++i) {
        if (!offset[i].oldval)
            offset[i].oldval = *offset[i].offset;
        *offset[i].offset = newval;
    }
}

void plt_reset_offsets(plt_offset *offset, size_t nb_off)
{
    for (size_t i = 0; i < nb_off; ++i) {
        *offset[i].offset = offset[i].oldval;
    }
}

static unsigned long elf_hash (const char *s)
{
    unsigned long h = 0, high;
    while (*s) {
        h = (h << 4) + (unsigned char) *s++;
        if ((high = h & 0xf0000000))
            h ^= high >> 24;
        h &= ~high;
    }
    return h;
}

static ElfW(Sym) *elf_hash_find(ElfW(Word) *hash, ElfW(Sym) *symtab,
    const char *strtab, const char *name)
{
    struct {
        ElfW(Word) nb_buckets;
        ElfW(Word) nb_chains;
    } *h_info = (void*) hash;

    ElfW(Word) *buckets = (ElfW(Word)*) (h_info + 1);
    ElfW(Word) *chains = (ElfW(Word)*) (h_info + 1) + h_info->nb_buckets;

    unsigned long idx = elf_hash(name) % h_info->nb_buckets;

    for (ElfW(Word) si = buckets[idx]; si != STN_UNDEF; si = chains[si]) {
        if (mmk_streq(&strtab[symtab[si].st_name], name))
            return &symtab[si];
    }
    return NULL;
}

static ElfW(Sym) *sym_lookup_dyn(plt_lib lib, const char *name)
{
    ElfW(Word) *hash    = (ElfW(Word)*) lib_dt_lookup(lib, DT_HASH);
    ElfW(Sym) *symtab   = (ElfW(Sym)*)  lib_dt_lookup(lib, DT_SYMTAB);
    const char *strtab  = (const char*) lib_dt_lookup(lib, DT_STRTAB);

    if (!hash || !symtab || !strtab)
        return NULL;
    return elf_hash_find (hash, symtab, strtab, name);
}

plt_fn *plt_get_real_fn(plt_ctx ctx, const char *name)
{
    for (struct link_map *lm = ctx->r_map; lm != NULL; lm = lm->l_next) {
        ElfW(Sym) *sym = sym_lookup_dyn(lm, name);
        if (sym) {
            /* Some compilers (e.g. ICC) put unresolved symbols into the
               symbol table with a size of 0. We ignore them to avoid
               getting the address of the PLT stub. */
            if (sym->st_size == 0)
                continue;

            return (void *) (sym->st_value + lm->l_addr);
        }
    }
    return NULL;
}
