#ifndef MEMORY_H
#define MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "address.h"
#include "hardware_specs.h"

#define NUM_FRAMES (PHYSICAL_RAM_SIZE / FRAME_SIZE)
#define NO_FRAME   UINT32_MAX

typedef enum {
    ACCESS_HIT,
    ACCESS_FAULT,
    ACCESS_INVALID
} access_result_t;

typedef struct {
    bool used;
    bool head;           // primeiro quadro da página; páginas ímpares têm um segundo quadro
    uint32_t pid;
    uint32_t vpn;
    uint32_t bytes_used; // maior offset acessado + 1, mantido apenas no quadro head
} frame_t;

typedef struct {
    uint32_t pid;
    page_table_entry_t page_table[NUM_VIRTUAL_PAGES];
} process_t;

typedef struct {
    frame_t frames[NUM_FRAMES];
    uint32_t free_frames;
    process_t **processes;
    size_t process_count;
    size_t process_capacity;
} memory_t;

/* Inicializa a RAM com todos os quadros livres e sem processos. */
void memory_init(memory_t *mem);

/* Libera as tabelas de páginas de todos os processos. */
void memory_destroy(memory_t *mem);

/* Retorna o processo pid, criando sua tabela de páginas vazia se ainda não existir.
 * Retorna NULL em caso de falha de alocação. */
process_t *memory_get_process(memory_t *mem, uint32_t pid);

/* Retorna o processo pid ou NULL se ele nunca acessou a memória. */
process_t *memory_find_process(const memory_t *mem, uint32_t pid);

/* Traduz o endereço e consulta a MMU, registrando o uso de bytes em caso de hit.
 * Endereços fora de 1 MB retornam ACCESS_INVALID sem tocar no hardware. */
access_result_t memory_access(memory_t *mem, hardware_state_t *hw, process_t *proc,
                              uint32_t virtual_address, bool is_write,
                              uint32_t timestamp, translation_t *out);

/* Procura quadros livres para a página vpn: 1 quadro se par, 2 alinhados se ímpar.
 * Retorna o quadro base ou NO_FRAME se a RAM não comportar a página. */
uint32_t memory_find_free(const memory_t *mem, uint32_t vpn);

/* Carrega a página da tradução t em quadros livres e conclui o acesso que falhou.
 * Retorna o quadro base ou NO_FRAME se for preciso liberar espaço antes. */
uint32_t memory_load_page(memory_t *mem, process_t *proc, const translation_t *t,
                          bool is_write, uint32_t timestamp);

/* Remove a página vpn da RAM, liberando seus quadros e limpando a PTE.
 * Retorna true se a página estava suja e exige Page Out. */
bool memory_unmap_page(memory_t *mem, process_t *proc, uint32_t vpn);

/* Indica se despejar a página que ocupa victim_frame abre espaço para vpn. */
bool memory_would_fit(const memory_t *mem, uint32_t victim_frame, uint32_t vpn);

/* Retorna o quadro base da página que ocupa frame, ou NO_FRAME se estiver livre. */
uint32_t memory_head_frame(const memory_t *mem, uint32_t frame);

/* Informa quantos bytes do quadro estão em uso e quantos são desperdício interno.
 * Quadros livres retornam zero nos dois campos. */
void memory_frame_usage(const memory_t *mem, uint32_t frame,
                        uint32_t *used, uint32_t *wasted);

/* Confere se cada PTE presente e cada quadro ocupado apontam um para o outro. */
bool memory_check_consistency(const memory_t *mem);

#endif
