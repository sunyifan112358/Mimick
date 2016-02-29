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
#include <assert.h>
#include <stdint.h>

#include "plt-pe.h"

#define IDIR_IMPORT 1 // Index of the import directory entry

plt_ctx plt_init_ctx (void) {
    return NULL;
}

plt_lib plt_get_lib (plt_ctx ctx, const char *name)
{
    (void) ctx;
    return GetModuleHandle(name);
}

static inline PIMAGE_NT_HEADERS nt_header_from_lib (plt_lib lib)
{
    PIMAGE_DOS_HEADER dos_hdr = (PIMAGE_DOS_HEADER) lib;
    return (PIMAGE_NT_HEADERS) ((char *) dos_hdr + dos_hdr->e_lfanew);
}

static inline PIMAGE_IMPORT_DESCRIPTOR get_first_import_descriptor (plt_lib lib)
{
    PIMAGE_NT_HEADERS nthdr = nt_header_from_lib (lib);
    DWORD off = nthdr->OptionalHeader.DataDirectory[IDIR_IMPORT].VirtualAddress;
    assert (off != 0);
    return (PIMAGE_IMPORT_DESCRIPTOR) ((char *) lib + off);
}

plt_fn **plt_get_offset (plt_lib lib, const char *name)
{
    char *base = lib;
    for (PIMAGE_IMPORT_DESCRIPTOR entry = get_first_import_descriptor (lib);
            entry->Name;
            entry++)
    {
        uintptr_t *thunk = (uintptr_t *) (base + entry->FirstThunk);
        PIMAGE_THUNK_DATA thunk_data = (PIMAGE_THUNK_DATA) (base
                + (entry->OriginalFirstThunk
                    ? entry->OriginalFirstThunk
                    : entry->FirstThunk));
        for (; thunk_data->u1.AddressOfData != 0; ++thunk, ++thunk_data)
        {
            PIMAGE_IMPORT_BY_NAME ibn = (void *) thunk_data->u1.AddressOfData;
            if (!strcmp(name, base + (uintptr_t) ibn->Name))
                return (plt_fn **) thunk;
        }
    }
    return NULL;
}