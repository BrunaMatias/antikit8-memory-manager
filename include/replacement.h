#ifndef REPLACEMENT_H
#define REPLACEMENT_H

#include "memory.h"
#include "swap.h"

#define WSCLOCK_DEFAULT_TAU_MS 50u

typedef enum {
    REPLACEMENT_REFERENCE_CLEARED,
    REPLACEMENT_PAGE_UNMAPPED,
    REPLACEMENT_PAGE_MAPPED
} replacement_event_t;

/* Ponto de integracao do auditor: chamado imediatamente apos cada mudanca
 * de PTE feita por este modulo. O callback pode observar, mas nao mutar mem. */
typedef void (*replacement_observer_t)(const memory_t *mem,
                                        replacement_event_t event,
                                        uint32_t pid, uint32_t vpn, void *context);

typedef struct {
    uint32_t hand;
    uint32_t tau_ms;
    uint64_t evictions;
    uint64_t clean_evictions;
    uint64_t dirty_evictions;
    replacement_observer_t observer;
    void *observer_context;
    FILE *log;
} replacement_t;

typedef enum {
    REPLACEMENT_OK,
    REPLACEMENT_INVALID,
    REPLACEMENT_NO_VICTIM,
    REPLACEMENT_SWAP_ERROR
} replacement_result_t;

void replacement_init(replacement_t *replacement, uint32_t tau_ms, FILE *log);

void replacement_set_observer(replacement_t *replacement,
                               replacement_observer_t observer, void *context);

/* Chamar somente depois de memory_access() retornar ACCESS_FAULT.
 * Resolve a falta e conclui o acesso por memory_load_page(), sem chamar a MMU
 * de novo e sem duplicar contagem de acessos/fading. O frame de saida e opcional.
 * Para os proximos acessos, usar swap.now_ms como timestamp efetivo da MMU. */
replacement_result_t replacement_handle_fault(replacement_t *replacement,
                                                memory_t *mem, swap_t *swap,
                                                process_t *proc,
                                                const translation_t *translation,
                                                bool is_write,
                                                uint32_t timestamp_ms,
                                                uint32_t *frame_out);

#endif
