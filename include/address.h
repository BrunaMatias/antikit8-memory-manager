#ifndef ADDRESS_H
#define ADDRESS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware_specs.h"

#define PAGE_PAIR_SIZE (PAGE_SIZE_EVEN + PAGE_SIZE_ODD)

// 85 pares completos de 12 KB e uma página par final de 4 KB totalizam 171 páginas.
#define NUM_VIRTUAL_PAGES                                    \
    (2u * (VIRTUAL_MEMORY_SIZE / PAGE_PAIR_SIZE) +           \
     (VIRTUAL_MEMORY_SIZE % PAGE_PAIR_SIZE > 0) +            \
     (VIRTUAL_MEMORY_SIZE % PAGE_PAIR_SIZE > PAGE_SIZE_EVEN))

// Unidade de alocação da RAM: uma página par ocupa 1 quadro e uma ímpar ocupa 2.
#define FRAME_SIZE PAGE_SIZE_EVEN

typedef struct {
    uint32_t virtual_address;
    uint32_t vpn;
    uint32_t offset;
    uint32_t page_size;
} translation_t;

/* Retorna o tamanho em bytes da página virtual vpn. */
uint32_t addr_page_size(uint32_t vpn);

/* Retorna o primeiro endereço virtual da página vpn. */
uint32_t addr_page_base(uint32_t vpn);

/* Retorna quantos quadros físicos consecutivos a página vpn ocupa. */
uint32_t addr_frames_needed(uint32_t vpn);

/* Decompõe o endereço virtual em VPN e offset em tempo constante.
 * Retorna false se o endereço estiver fora do espaço virtual de 1 MB. */
bool addr_translate(uint32_t virtual_address, translation_t *out);

/* Calcula o endereço físico a partir do quadro base da PTE e do offset. */
uint32_t addr_physical(const page_table_entry_t *pte, uint32_t offset);

/* Imprime o resultado da tradução de virtual_address no formato do log da MMU. */
void addr_log(FILE *out, uint32_t virtual_address);

#endif
