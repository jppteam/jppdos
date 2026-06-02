#pragma once
/*
 * Minimal ELF32 / RISC-V types for the native app loader.
 * Only the subset needed to parse and relocate ET_DYN shared objects.
 */

#include <stdint.h>

/* ---- ELF identification -------------------------------------------------- */

#define ELF_MAG0        0x7Fu
#define ELF_MAG1        'E'
#define ELF_MAG2        'L'
#define ELF_MAG3        'F'
#define EI_NIDENT       16u

/* e_ident indices */
#define EI_CLASS        4u
#define EI_DATA         5u

/* Class / data encoding */
#define ELFCLASS32      1u
#define ELFDATA2LSB     1u   /* little-endian */

/* Object types (e_type) */
#define ET_DYN          3u   /* shared object (position-independent) */

/* Machine (e_machine) */
#define EM_RISCV        243u

/* ---- ELF32 header -------------------------------------------------------- */

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;      /* program header table offset */
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

/* ---- Program header ------------------------------------------------------ */

/* p_type values */
#define PT_NULL    0u
#define PT_LOAD    1u
#define PT_DYNAMIC 2u

/* p_flags bits */
#define PF_X 1u   /* execute */
#define PF_W 2u   /* write */
#define PF_R 4u   /* read */

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;   /* file offset of segment data */
    uint32_t p_vaddr;    /* virtual address in ELF address space */
    uint32_t p_paddr;
    uint32_t p_filesz;   /* size in file */
    uint32_t p_memsz;    /* size in memory (p_memsz >= p_filesz; tail is BSS) */
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

/* ---- Dynamic section ----------------------------------------------------- */

/* d_tag values */
#define DT_NULL       0
#define DT_PLTRELSZ   2
#define DT_HASH       4
#define DT_STRTAB     5
#define DT_SYMTAB     6
#define DT_RELA       7
#define DT_RELASZ     8
#define DT_RELAENT    9
#define DT_STRSZ     10
#define DT_SYMENT    11
#define DT_PLTREL    20
#define DT_JMPREL    23

typedef struct {
    int32_t  d_tag;
    uint32_t d_val;   /* also used as d_ptr (virtual address) */
} Elf32_Dyn;

/* ---- Symbol table -------------------------------------------------------- */

#define SHN_UNDEF 0u

/* ELF32 symbol info decomposition */
#define ELF32_ST_BIND(i)  ((uint8_t)(i) >> 4u)
#define ELF32_ST_TYPE(i)  ((uint8_t)(i) & 0x0Fu)

/* Symbol binding */
#define STB_LOCAL   0u
#define STB_GLOBAL  1u
#define STB_WEAK    2u

/* Symbol types */
#define STT_NOTYPE  0u
#define STT_FUNC    2u

typedef struct {
    uint32_t st_name;    /* index into string table */
    uint32_t st_value;   /* virtual address (if defined) */
    uint32_t st_size;
    uint8_t  st_info;    /* ELF32_ST_BIND + ELF32_ST_TYPE */
    uint8_t  st_other;
    uint16_t st_shndx;   /* SHN_UNDEF = external / undefined */
} Elf32_Sym;

/* ---- Relocation with addend (RELA) --------------------------------------- */

/* r_info decomposition */
#define ELF32_R_SYM(i)   ((uint32_t)(i) >> 8u)
#define ELF32_R_TYPE(i)  ((uint8_t)(i))

/* RISC-V 32-bit relocation types used in PIC shared objects */
#define R_RISCV_NONE      0u
#define R_RISCV_32        1u  /* S + A  (absolute 32-bit)            */
#define R_RISCV_RELATIVE  3u  /* B + A  (load-bias + addend)         */
#define R_RISCV_JUMP_SLOT 5u  /* S      (PLT/GOT slot for extern fn) */

typedef struct {
    uint32_t r_offset;   /* virtual address of location to patch */
    uint32_t r_info;     /* symbol index + relocation type        */
    int32_t  r_addend;   /* explicit addend                       */
} Elf32_Rela;

/* ---- ELF hash table (DT_HASH) — only used to get symbol count ----------- */

/*
 * Format at the DT_HASH virtual address:
 *   uint32_t nbucket;
 *   uint32_t nchain;    ← total number of symbol table entries
 *   uint32_t bucket[nbucket];
 *   uint32_t chain[nchain];
 */
typedef struct {
    uint32_t nbucket;
    uint32_t nchain;   /* symbol count */
} Elf32_Hash_Hdr;
