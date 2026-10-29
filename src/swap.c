#include "swap.h"

#include <inttypes.h>
#include <string.h>

void swap_init(swap_t *swap, hardware_state_t *hardware, FILE *log)
{
    memset(swap, 0, sizeof(*swap));
    swap->hardware = hardware;
    swap->log = log;
    if (hardware != NULL) {
        swap->now_ms = hardware->last_swap_ms;
        swap->max_temperature = hardware->swap_temperature;
    }
}

void swap_advance_to(swap_t *swap, uint32_t timestamp_ms)
{
    if (timestamp_ms > swap->now_ms) {
        swap->now_ms = timestamp_ms;
    }
}

static float cooled_temperature(float temperature, uint32_t elapsed_ms)
{
    float result = temperature - elapsed_ms * 0.5f;
    return result < BASE_SWAP_TEMP ? BASE_SWAP_TEMP : result;
}

float swap_current_temperature(const swap_t *swap)
{
    const hardware_state_t *hw = swap->hardware;
    if (hw == NULL) {
        return BASE_SWAP_TEMP;
    }
    uint32_t elapsed = swap->now_ms >= hw->last_swap_ms
                     ? swap->now_ms - hw->last_swap_ms : 0;
    return cooled_temperature(hw->swap_temperature, elapsed);
}

bool swap_page_out(swap_t *swap)
{
    hardware_state_t *hw = swap->hardware;
    if (hw == NULL || swap->now_ms < hw->last_swap_ms) {
        return false;
    }

    uint32_t scheduled_ms = swap->now_ms;
    float predicted = swap_current_temperature(swap)
                    + (scheduled_ms == hw->last_swap_ms ? 15.0f : 12.0f);

    if (predicted > SWAP_TARGET_TEMP) {
        /* Em outro ms, T_final = max(25, T_ultimo - 0.5 * dt) + 12.
         * Calculamos ceil(dt) sem exigir a biblioteca matematica (-lm). */
        double required_ms = ((double)hw->swap_temperature + 12.0
                            - SWAP_TARGET_TEMP) / 0.5;
        double available_ms = (double)(UINT32_MAX - hw->last_swap_ms);
        if (required_ms > available_ms) {
            return false;
        }
        uint32_t elapsed = required_ms > 0.0 ? (uint32_t)required_ms : 0;
        if ((double)elapsed < required_ms) {
            elapsed++;
        }
        if (elapsed == 0) {
            if (hw->last_swap_ms == UINT32_MAX) {
                return false;
            }
            elapsed = 1; /* mudar de ms reduz o incremento de 15 para 12 C */
        }
        uint32_t earliest_ms = hw->last_swap_ms + elapsed;
        if (earliest_ms > scheduled_ms) {
            scheduled_ms = earliest_ms;
        }
    }

    uint32_t wait = scheduled_ms - swap->now_ms;
    if (wait > 0 && swap->log != NULL) {
        fprintf(swap->log, "[SWAP] espera virtual de %u ms: %u -> %u ms\n",
                wait, swap->now_ms, scheduled_ms);
    }
    swap->now_ms = scheduled_ms;
    swap->waiting_ms += wait;

    /* Somente o motor do professor altera temperatura, last_swap_ms e custo.
     * Ele ja chama hardware_cool_down(diff): chamar novamente aqui contaria
     * o mesmo intervalo de resfriamento duas vezes. */
    uint64_t before_cycles = hw->total_cpu_cycles;
    hardware_trigger_page_out(hw, scheduled_ms);
    uint64_t cost = hw->total_cpu_cycles - before_cycles;
    swap->io_cpu_cycles += cost;
    swap->page_outs++;
    if (hw->swap_temperature > swap->max_temperature) {
        swap->max_temperature = hw->swap_temperature;
    }
    if (swap->log != NULL) {
        fprintf(swap->log,
                "[SWAP] Page Out #%" PRIu64 " | t=%u ms | %.1f C | "
                "custo=%" PRIu64 " ciclos\n",
                swap->page_outs, scheduled_ms, (double)hw->swap_temperature, cost);
    }
    return true;
}
