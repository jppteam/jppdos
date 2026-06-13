#include "../include/jpp_native_loader_core.h"
#include "jpp_elf32_riscv.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"

#include "jpp_app_pool.h"

static const char *TAG = "native_loader";

/* ---- Executable memory pool --------------------------------------------- */
/*
 * Native code is loaded into the shared jpp_app_pool — a single static `.bss`
 * buffer that is contiguous (placed by the linker, never fragmented) and
 * executable (with CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT=n the whole SRAM range is
 * RWX on ESP32-C6).  A heap allocation cannot satisfy this: WiFi + BLE scatter
 * allocations leave no contiguous 50 KB+ executable block at launch time, and
 * the PMP marks the regular heap's DRAM non-executable anyway.
 *
 * The pool is shared with the MicroPython GC heap (see jpp_app_pool.h): native
 * and MicroPython apps are mutually exclusive, so one pool serves both and the
 * second pool's worth of SRAM stays in the general heap for WiFi/lwIP.
 *
 * The host app image sits at the pool base; the remaining tail can hold one
 * dynamically loaded code module (jpp_native_loader_load_module) at a time.
 */
void jpp_native_loader_preinit(void)
{
    ESP_LOGI(TAG, "EXEC pool: shared app pool, %u bytes",
             (unsigned)jpp_app_pool_size());
}

/* ---- Public symbol table (defined in jpp_native_symtab.c) --------------- */

typedef struct {
    const char *name;
    void       *addr;
} jpp_native_sym_t;

extern const jpp_native_sym_t *const jpp_native_symtab;
extern const size_t                  jpp_native_symtab_count;

/* ---- Loaded image handles ------------------------------------------------ */

struct jpp_native_loaded_app {
    void    *mem;               /* executable block for all PT_LOAD segments   */
    size_t   mem_size;
    uintptr_t load_base;        /* = (uintptr_t)mem                            */
    uintptr_t vaddr_base;       /* min PT_LOAD p_vaddr (load bias denominator) */
    void   (*entry)(void *);    /* jpp_app_entry, resolved during load         */
    void   (*task_entry)(void *, const char *);  /* jpp_app_task_entry, optional */
};

struct jpp_native_loaded_module {
    void    *mem;               /* tail region inside the pool (not released
                                   separately — owned by the pool/host app)    */
    size_t   mem_size;
    void   (*entry)(void *, void *);  /* jpp_module_entry(ctx, api)            */
};

/* Pool watermark: set while a native host app is resident, so module loads
   know where the unused tail begins. One module at a time. */
static void                        *s_pool_base;
static size_t                       s_pool_used;
static jpp_native_loaded_module_t  *s_active_module;

#define JPP_LOADER_ALIGN(sz) (((sz) + 15u) & ~(size_t)15u)

/* ---- Symbol lookup -------------------------------------------------------- */

static void *lookup_sym(const char *name)
{
    for (size_t i = 0u; i < jpp_native_symtab_count; i++) {
        if (strcmp(jpp_native_symtab[i].name, name) == 0) {
            return jpp_native_symtab[i].addr;
        }
    }
    return NULL;
}

/* ---- File helpers --------------------------------------------------------- */

/* Read exactly `n` bytes from `f` at file offset `off` into `dst`. */
static bool pread_exact(FILE *f, size_t off, void *dst, size_t n)
{
    if (fseek(f, (long)off, SEEK_SET) != 0) {
        return false;
    }
    return fread(dst, 1u, n, f) == n;
}

/* ---- ELF image loader ----------------------------------------------------- */

typedef struct {
    void     *mem;
    size_t    mem_size;
    uintptr_t load_base;
    uintptr_t vaddr_base;
    void     *entry;       /* entry_name symbol (required)        */
    void     *entry2;      /* entry2_name symbol (optional, NULL) */
} jpp_loaded_image_t;

/*
 * Load one ELF32/RISC-V ET_DYN image. Shared by app loading (image at the pool
 * base, acquired here) and module loading (image in the tail after the host
 * app; as_module = true). On any failure the pool state is exactly what it was
 * before the call: app loads release the pool, module loads never touch the
 * host app's region.
 */
static jpp_native_loader_result_t load_image(
    const char         *path,
    bool                as_module,
    const char         *entry_name,
    const char         *entry2_name,
    jpp_loaded_image_t *out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "LOAD_FAILED open: %s", path);
        return JPP_NATIVE_LOADER_READ_FAILED;
    }

    /* --- 1. Read and validate ELF header ---------------------------------- */
    Elf32_Ehdr ehdr;
    if (!pread_exact(f, 0u, &ehdr, sizeof(ehdr))) {
        fclose(f);
        ESP_LOGE(TAG, "LOAD_FAILED read ehdr");
        return JPP_NATIVE_LOADER_READ_FAILED;
    }

    if (ehdr.e_ident[0] != ELF_MAG0 || ehdr.e_ident[1] != ELF_MAG1 ||
        ehdr.e_ident[2] != ELF_MAG2 || ehdr.e_ident[3] != ELF_MAG3) {
        fclose(f);
        ESP_LOGE(TAG, "LOAD_FAILED bad ELF magic");
        return JPP_NATIVE_LOADER_INVALID_ELF;
    }
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS32 ||
        ehdr.e_ident[EI_DATA]  != ELFDATA2LSB) {
        fclose(f);
        ESP_LOGE(TAG, "LOAD_FAILED not ELF32-LE");
        return JPP_NATIVE_LOADER_INVALID_ELF;
    }
    if (ehdr.e_machine != EM_RISCV || ehdr.e_type != ET_DYN) {
        fclose(f);
        ESP_LOGE(TAG, "LOAD_FAILED not RISC-V ET_DYN (machine=%u type=%u)",
                 ehdr.e_machine, ehdr.e_type);
        return JPP_NATIVE_LOADER_INVALID_ELF;
    }
    if (ehdr.e_phentsize < sizeof(Elf32_Phdr) || ehdr.e_phnum == 0u) {
        fclose(f);
        ESP_LOGE(TAG, "LOAD_FAILED no program headers");
        return JPP_NATIVE_LOADER_INVALID_ELF;
    }

    /* --- 2. Read program headers ------------------------------------------ */
    Elf32_Phdr phdrs[8];
    if (ehdr.e_phnum > 8u) {
        fclose(f);
        ESP_LOGE(TAG, "LOAD_FAILED too many phdrs (%u)", ehdr.e_phnum);
        return JPP_NATIVE_LOADER_INVALID_ELF;
    }
    for (uint16_t i = 0u; i < ehdr.e_phnum; i++) {
        if (!pread_exact(f, ehdr.e_phoff + (size_t)i * ehdr.e_phentsize,
                         &phdrs[i], sizeof(Elf32_Phdr))) {
            fclose(f);
            ESP_LOGE(TAG, "LOAD_FAILED read phdr %u", i);
            return JPP_NATIVE_LOADER_READ_FAILED;
        }
    }

    /* --- 3. Compute contiguous load range ---------------------------------- */
    uint32_t vaddr_min = UINT32_MAX;
    uint32_t vaddr_max = 0u;
    for (uint16_t i = 0u; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_memsz > 0u) {
            if (phdrs[i].p_vaddr < vaddr_min) {
                vaddr_min = phdrs[i].p_vaddr;
            }
            uint32_t seg_end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
            if (seg_end > vaddr_max) {
                vaddr_max = seg_end;
            }
        }
    }
    if (vaddr_max <= vaddr_min) {
        fclose(f);
        ESP_LOGE(TAG, "LOAD_FAILED no PT_LOAD segments");
        return JPP_NATIVE_LOADER_INVALID_ELF;
    }
    size_t total_size = (size_t)(vaddr_max - vaddr_min);

    /* --- 4. Place the image in executable memory --------------------------- */
    void *mem;
    if (!as_module) {
        ESP_LOGI(TAG, "LOAD %s: %zu bytes (pool=%u bytes, used=%s)",
                 path, total_size, (unsigned)jpp_app_pool_size(),
                 jpp_app_pool_in_use() ? "yes" : "no");
        mem = jpp_app_pool_acquire(total_size, NULL);
        if (mem == NULL) {
            fclose(f);
            ESP_LOGE(TAG, "LOAD_FAILED %s: app pool unavailable (busy or binary "
                     "%zu bytes exceeds pool %u bytes)",
                     path, total_size, (unsigned)jpp_app_pool_size());
            return JPP_NATIVE_LOADER_NO_MEMORY;
        }
    } else {
        size_t tail_off = JPP_LOADER_ALIGN(s_pool_used);
        ESP_LOGI(TAG, "LOAD MODULE %s: %zu bytes (tail %zu of %u free)",
                 path, total_size,
                 (size_t)(jpp_app_pool_size() - tail_off),
                 (unsigned)jpp_app_pool_size());
        if (tail_off + total_size > jpp_app_pool_size()) {
            fclose(f);
            ESP_LOGE(TAG, "LOAD_FAILED module %s: %zu bytes exceeds free pool "
                     "tail (%zu bytes)",
                     path, total_size, (size_t)(jpp_app_pool_size() - tail_off));
            return JPP_NATIVE_LOADER_NO_MEMORY;
        }
        mem = (uint8_t *)s_pool_base + tail_off;
    }
    memset(mem, 0, total_size);

    uintptr_t load_base = (uintptr_t)mem;
    /* delta: add to any ELF vaddr to get the runtime memory address */
    uintptr_t delta = load_base - (uintptr_t)vaddr_min;

/* App loads own the pool acquisition; module loads borrow the tail and must
   leave the host app's region untouched on failure. */
#define FREE_MEM() do { \
    if (!as_module) { jpp_app_pool_release(); } \
} while (0)

    /* --- 5. Load PT_LOAD segments ----------------------------------------- */
    for (uint16_t i = 0u; i < ehdr.e_phnum; i++) {
        const Elf32_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0u) {
            continue;
        }
        uint8_t *dest = (uint8_t *)(delta + ph->p_vaddr);
        if (ph->p_filesz > 0u) {
            if (!pread_exact(f, ph->p_offset, dest, ph->p_filesz)) {
                fclose(f);
                FREE_MEM();
                ESP_LOGE(TAG, "LOAD_FAILED read segment %u", i);
                return JPP_NATIVE_LOADER_READ_FAILED;
            }
        }
        /* BSS: already zeroed by the memset above */
    }
    fclose(f);
    f = NULL;

    /* --- 6. Parse PT_DYNAMIC ---------------------------------------------- */
    uint32_t dyn_vaddr  = 0u;
    uint32_t dyn_count  = 0u;
    for (uint16_t i = 0u; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dyn_vaddr = phdrs[i].p_vaddr;
            dyn_count = phdrs[i].p_filesz / sizeof(Elf32_Dyn);
            break;
        }
    }
    if (dyn_vaddr == 0u) {
        FREE_MEM();
        ESP_LOGE(TAG, "LOAD_FAILED no PT_DYNAMIC");
        return JPP_NATIVE_LOADER_INVALID_ELF;
    }
    const Elf32_Dyn *dyn = (const Elf32_Dyn *)(delta + dyn_vaddr);

    /* Extract dynamic entries we care about. */
    uint32_t dt_strtab   = 0u, dt_strsz   = 0u;
    uint32_t dt_symtab   = 0u, dt_syment  = 0u;
    uint32_t dt_hash     = 0u;
    uint32_t dt_rela     = 0u, dt_relasz  = 0u, dt_relaent = 0u;
    uint32_t dt_jmprel   = 0u, dt_pltrelsz= 0u;

    for (uint32_t i = 0u; i < dyn_count; i++) {
        switch (dyn[i].d_tag) {
        case DT_STRTAB:   dt_strtab    = dyn[i].d_val; break;
        case DT_STRSZ:    dt_strsz     = dyn[i].d_val; break;
        case DT_SYMTAB:   dt_symtab    = dyn[i].d_val; break;
        case DT_SYMENT:   dt_syment    = dyn[i].d_val; break;
        case DT_HASH:     dt_hash      = dyn[i].d_val; break;
        case DT_RELA:     dt_rela      = dyn[i].d_val; break;
        case DT_RELASZ:   dt_relasz    = dyn[i].d_val; break;
        case DT_RELAENT:  dt_relaent   = dyn[i].d_val; break;
        case DT_JMPREL:   dt_jmprel    = dyn[i].d_val; break;
        case DT_PLTRELSZ: dt_pltrelsz  = dyn[i].d_val; break;
        case DT_NULL:     goto dyn_done;
        default:          break;
        }
    }
dyn_done:;
    (void)dt_strsz; /* parsed but not used for bounds checking */

    if (dt_strtab == 0u || dt_symtab == 0u) {
        FREE_MEM();
        ESP_LOGE(TAG, "LOAD_FAILED missing DT_STRTAB/DT_SYMTAB");
        return JPP_NATIVE_LOADER_INVALID_ELF;
    }

    /* Pointers into the loaded image. */
    const char    *strtab = (const char    *)(delta + dt_strtab);
    const Elf32_Sym *symtab = (const Elf32_Sym *)(delta + dt_symtab);
    size_t sym_ent = (dt_syment > 0u) ? dt_syment : sizeof(Elf32_Sym);

    /* Symbol count from DT_HASH (uint32 nchain at offset 4). */
    size_t sym_count = 0u;
    if (dt_hash != 0u) {
        const uint32_t *hash = (const uint32_t *)(delta + dt_hash);
        sym_count = (size_t)hash[1]; /* nchain = total symbol entries */
    }

    /* ---- Helper: resolve symbol address ----------------------------------- */
    /* Returns the runtime address of symbol `sym`, or 0 on failure. */
#define SYM_AT(idx) ((const Elf32_Sym *)((const uint8_t *)symtab + (idx) * sym_ent))

    /* --- 7. Apply .rela.dyn relocations ----------------------------------- */
    if (dt_rela != 0u && dt_relasz > 0u) {
        size_t rela_ent  = (dt_relaent > 0u) ? dt_relaent : sizeof(Elf32_Rela);
        size_t rela_count = dt_relasz / rela_ent;
        const Elf32_Rela *rela = (const Elf32_Rela *)(delta + dt_rela);

        for (size_t i = 0u; i < rela_count; i++) {
            const Elf32_Rela *r = (const Elf32_Rela *)
                                  ((const uint8_t *)rela + i * rela_ent);
            uint32_t r_type = ELF32_R_TYPE(r->r_info);
            uint32_t r_sym  = ELF32_R_SYM(r->r_info);
            uint32_t *patch = (uint32_t *)(delta + r->r_offset);

            switch (r_type) {
            case R_RISCV_RELATIVE:
                /* *patch = load_bias + addend; load_bias = delta (when vaddr_min=0)
                   or delta + vaddr_min if vaddr_min != 0. For ET_DYN, vaddr_min is
                   the intended base, so B = delta + vaddr_min (= load_base). */
                *patch = (uint32_t)(load_base + (uint32_t)r->r_addend);
                break;

            case R_RISCV_32: {
                uint32_t sym_val = 0u;
                if (r_sym != 0u && r_sym < sym_count) {
                    const Elf32_Sym *s = SYM_AT(r_sym);
                    if (s->st_shndx == SHN_UNDEF) {
                        void *faddr = lookup_sym(strtab + s->st_name);
                        if (faddr == NULL) {
                            ESP_LOGE(TAG, "LOAD_FAILED unresolved: %s",
                                     strtab + s->st_name);
                            FREE_MEM();
                            return JPP_NATIVE_LOADER_UNRESOLVED_SYM;
                        }
                        sym_val = (uint32_t)(uintptr_t)faddr;
                    } else {
                        sym_val = (uint32_t)(delta + s->st_value);
                    }
                }
                *patch = sym_val + (uint32_t)r->r_addend;
                break;
            }

            case R_RISCV_JUMP_SLOT: {
                /* PLT/GOT slot for an external function. */
                if (r_sym == 0u || r_sym >= sym_count) {
                    break;
                }
                const Elf32_Sym *s = SYM_AT(r_sym);
                if (s->st_shndx == SHN_UNDEF) {
                    void *faddr = lookup_sym(strtab + s->st_name);
                    if (faddr == NULL) {
                        ESP_LOGE(TAG, "LOAD_FAILED unresolved: %s",
                                 strtab + s->st_name);
                        FREE_MEM();
                        return JPP_NATIVE_LOADER_UNRESOLVED_SYM;
                    }
                    *patch = (uint32_t)(uintptr_t)faddr;
                } else {
                    *patch = (uint32_t)(delta + s->st_value);
                }
                break;
            }

            case R_RISCV_NONE:
                break;

            default:
                ESP_LOGE(TAG, "LOAD_FAILED unsupported rela type %u at 0x%x",
                         r_type, r->r_offset);
                FREE_MEM();
                return JPP_NATIVE_LOADER_UNSUPPORTED;
            }
        }
    }

    /* --- 8. Apply .rela.plt relocations ----------------------------------- */
    if (dt_jmprel != 0u && dt_pltrelsz > 0u) {
        size_t rela_ent   = (dt_relaent > 0u) ? dt_relaent : sizeof(Elf32_Rela);
        size_t rela_count = dt_pltrelsz / rela_ent;
        const Elf32_Rela *rela = (const Elf32_Rela *)(delta + dt_jmprel);

        for (size_t i = 0u; i < rela_count; i++) {
            const Elf32_Rela *r = (const Elf32_Rela *)
                                  ((const uint8_t *)rela + i * rela_ent);
            uint32_t r_type = ELF32_R_TYPE(r->r_info);
            uint32_t r_sym  = ELF32_R_SYM(r->r_info);
            uint32_t *patch = (uint32_t *)(delta + r->r_offset);

            if (r_type != R_RISCV_JUMP_SLOT) {
                ESP_LOGE(TAG, "LOAD_FAILED unexpected plt rela type %u", r_type);
                FREE_MEM();
                return JPP_NATIVE_LOADER_UNSUPPORTED;
            }
            if (r_sym == 0u || r_sym >= sym_count) {
                continue;
            }
            const Elf32_Sym *s = SYM_AT(r_sym);
            if (s->st_shndx == SHN_UNDEF) {
                void *faddr = lookup_sym(strtab + s->st_name);
                if (faddr == NULL) {
                    ESP_LOGE(TAG, "LOAD_FAILED unresolved plt: %s",
                             strtab + s->st_name);
                    FREE_MEM();
                    return JPP_NATIVE_LOADER_UNRESOLVED_SYM;
                }
                *patch = (uint32_t)(uintptr_t)faddr;
            } else {
                *patch = (uint32_t)(delta + s->st_value);
            }
        }
    }

#undef SYM_AT

    /* --- 9. Find the entry symbol(s) in .dynsym ---------------------------- */
    void *entry_fn  = NULL;
    void *entry2_fn = NULL;
    if (sym_count > 0u) {
        for (size_t i = 1u; i < sym_count; i++) {
            const Elf32_Sym *s = (const Elf32_Sym *)
                                 ((const uint8_t *)symtab + i * sym_ent);
            if (s->st_shndx == SHN_UNDEF) {
                continue;
            }
            if (ELF32_ST_BIND(s->st_info) != STB_GLOBAL) {
                continue;
            }
            const char *sname = strtab + s->st_name;
            if (strcmp(sname, entry_name) == 0) {
                entry_fn = (void *)(delta + s->st_value);
            } else if (entry2_name != NULL && strcmp(sname, entry2_name) == 0) {
                entry2_fn = (void *)(delta + s->st_value);
            }
            if (entry_fn != NULL && (entry2_name == NULL || entry2_fn != NULL)) {
                break;
            }
        }
    }
    if (entry_fn == NULL) {
        FREE_MEM();
        ESP_LOGE(TAG, "LOAD_FAILED %s not found", entry_name);
        return JPP_NATIVE_LOADER_NO_ENTRY;
    }

#undef FREE_MEM

    /* --- 10. Flush instruction cache -------------------------------------- */
    /*
     * After writing executable code to RAM, the I-cache may hold stale lines.
     * The GCC __builtin___clear_cache hint flushes the relevant cache range
     * without needing an extra ESP-IDF header dependency.
     */
    __builtin___clear_cache((char *)mem, (char *)mem + total_size);

    out->mem        = mem;
    out->mem_size   = total_size;
    out->load_base  = load_base;
    out->vaddr_base = (uintptr_t)vaddr_min;
    out->entry      = entry_fn;
    out->entry2     = entry2_fn;
    return JPP_NATIVE_LOADER_OK;
}

/* ---- App load / run / free ------------------------------------------------ */

jpp_native_loader_result_t jpp_native_loader_load(
    const char               *path,
    jpp_native_loaded_app_t **out_app)
{
    if (path == NULL || out_app == NULL) {
        return JPP_NATIVE_LOADER_INVALID_ELF;
    }
    *out_app = NULL;

    jpp_loaded_image_t img;
    jpp_native_loader_result_t r = load_image(
        path, false, "jpp_app_entry", "jpp_app_task_entry", &img);
    if (r != JPP_NATIVE_LOADER_OK) {
        return r;
    }

    jpp_native_loaded_app_t *app =
        (jpp_native_loaded_app_t *)malloc(sizeof(jpp_native_loaded_app_t));
    if (app == NULL) {
        jpp_app_pool_release();
        return JPP_NATIVE_LOADER_NO_MEMORY;
    }
    app->mem        = img.mem;
    app->mem_size   = img.mem_size;
    app->load_base  = img.load_base;
    app->vaddr_base = img.vaddr_base;
    app->entry      = (void (*)(void *))img.entry;
    app->task_entry = (void (*)(void *, const char *))img.entry2;

    s_pool_base = img.mem;
    s_pool_used = img.mem_size;

    ESP_LOGI(TAG, "LOADED %s base=0x%08x size=%zu entry=0x%08x",
             path, (unsigned)img.load_base, img.mem_size,
             (unsigned)(uintptr_t)img.entry);
    *out_app = app;
    return JPP_NATIVE_LOADER_OK;
}

void jpp_native_loader_run(jpp_native_loaded_app_t *app,
                            jpp_sdk_context_t       *ctx)
{
    if (app == NULL || app->entry == NULL || ctx == NULL) {
        return;
    }
    app->entry(ctx);
}

bool jpp_native_loader_run_task(jpp_native_loaded_app_t *app,
                                jpp_sdk_context_t       *ctx,
                                const char              *task_name)
{
    if (app == NULL || app->task_entry == NULL || ctx == NULL ||
        task_name == NULL) {
        return false;
    }
    app->task_entry(ctx, task_name);
    return true;
}

void jpp_native_loader_free(jpp_native_loaded_app_t *app)
{
    if (app == NULL) {
        return;
    }
    /* Reclaim any module the app left loaded — its memory is inside the pool,
       but the handle is heap-allocated. */
    if (s_active_module != NULL) {
        ESP_LOGW(TAG, "app exited with a module still loaded — reclaiming");
        jpp_native_loader_module_free(s_active_module);
    }
    if (app->mem != NULL) {
        jpp_app_pool_release();  /* return pool to available state */
        app->mem = NULL;
    }
    s_pool_base = NULL;
    s_pool_used = 0u;
    free(app);
}

/* ---- Module load / run / free --------------------------------------------- */

jpp_native_loader_result_t jpp_native_loader_load_module(
    const char                  *path,
    jpp_native_loaded_module_t **out_module)
{
    if (path == NULL || out_module == NULL) {
        return JPP_NATIVE_LOADER_INVALID_ELF;
    }
    *out_module = NULL;

    if (s_pool_base == NULL) {
        ESP_LOGE(TAG, "MODULE_FAILED no native host app loaded");
        return JPP_NATIVE_LOADER_BAD_STATE;
    }
    if (s_active_module != NULL) {
        ESP_LOGE(TAG, "MODULE_FAILED a module is already loaded");
        return JPP_NATIVE_LOADER_BAD_STATE;
    }

    jpp_loaded_image_t img;
    jpp_native_loader_result_t r = load_image(
        path, true, "jpp_module_entry", NULL, &img);
    if (r != JPP_NATIVE_LOADER_OK) {
        return r;
    }

    jpp_native_loaded_module_t *module = (jpp_native_loaded_module_t *)
        malloc(sizeof(jpp_native_loaded_module_t));
    if (module == NULL) {
        return JPP_NATIVE_LOADER_NO_MEMORY;
    }
    module->mem      = img.mem;
    module->mem_size = img.mem_size;
    module->entry    = (void (*)(void *, void *))img.entry;

    s_active_module = module;
    ESP_LOGI(TAG, "LOADED MODULE %s base=0x%08x size=%zu entry=0x%08x",
             path, (unsigned)img.load_base, img.mem_size,
             (unsigned)(uintptr_t)img.entry);
    *out_module = module;
    return JPP_NATIVE_LOADER_OK;
}

void jpp_native_loader_module_run(jpp_native_loaded_module_t *module,
                                  jpp_sdk_context_t          *ctx,
                                  void                       *api)
{
    if (module == NULL || module->entry == NULL || ctx == NULL) {
        return;
    }
    module->entry(ctx, api);
}

void jpp_native_loader_module_free(jpp_native_loaded_module_t *module)
{
    if (module == NULL) {
        return;
    }
    if (module == s_active_module) {
        s_active_module = NULL;
    }
    free(module);
}

const char *jpp_native_loader_result_name(jpp_native_loader_result_t r)
{
    switch (r) {
    case JPP_NATIVE_LOADER_OK:             return "OK";
    case JPP_NATIVE_LOADER_READ_FAILED:    return "READ_FAILED";
    case JPP_NATIVE_LOADER_INVALID_ELF:    return "INVALID_ELF";
    case JPP_NATIVE_LOADER_UNSUPPORTED:    return "UNSUPPORTED";
    case JPP_NATIVE_LOADER_NO_MEMORY:      return "NO_MEMORY";
    case JPP_NATIVE_LOADER_UNRESOLVED_SYM: return "UNRESOLVED_SYM";
    case JPP_NATIVE_LOADER_NO_ENTRY:       return "NO_ENTRY";
    case JPP_NATIVE_LOADER_BAD_STATE:      return "BAD_STATE";
    }
    return "UNKNOWN";
}
