#ifndef SWAP_H
#define SWAP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware_specs.h"

/* O hardware aquece 12 C quando muda o ms e 15 C no mesmo ms.
 * Mantemos uma margem de 1 C em relacao ao limite de throttling. */
#define SWAP_TARGET_TEMP (MAX_SAFE_TEMP - 1.0f)

typedef struct {
    hardware_state_t *hardware;
    uint32_t now_ms;           /* relogio efetivo: inclui esperas virtuais */
    uint64_t page_outs;
    uint64_t io_cpu_cycles;    /* apenas E/S; exclui os ciclos de acesso da MMU */
    uint64_t waiting_ms;
    float max_temperature;
    FILE *log;                /* NULL desabilita os logs */
} swap_t;

/* O hardware deve ter sido inicializado com hardware_init(). */
void swap_init(swap_t *swap, hardware_state_t *hardware, FILE *log);

/* Avanca ate o timestamp do trace sem voltar no tempo apos uma espera. */
void swap_advance_to(swap_t *swap, uint32_t timestamp_ms);

/* Temperatura no instante efetivo, incluindo o resfriamento pendente.
 * O hardware aplica esse resfriamento na proxima operacao de Page Out. */
float swap_current_temperature(const swap_t *swap);

/* Grava UMA pagina suja, esperando em tempo virtual quando necessario.
 * Retorna false se a espera ultrapassaria o limite uint32_t do hardware.
 * Nao altera bits da PTE; a pagina so pode ser removida apos o sucesso. */
bool swap_page_out(swap_t *swap);

#endif
