#include "replacement.h"

#include <inttypes.h>
#include <string.h>

/* Uma alocacao exige no maximo duas vitimas de 4 KB ou uma de 8 KB. */
typedef struct {
    uint32_t frames[2];
    uint32_t count;
    uint32_t dirty;
    uint32_t young;
    uint32_t next_hand;
    uint32_t priority;
    bool valid;
} eviction_plan_t;

void replacement_init(replacement_t *replacement, uint32_t tau_ms, FILE *log)
{
    memset(replacement, 0, sizeof(*replacement));
    replacement->tau_ms = tau_ms;
    replacement->log = log;
}

void replacement_set_observer(replacement_t *replacement,
                               replacement_observer_t observer, void *context)
{
    replacement->observer = observer;
    replacement->observer_context = context;
}

static void notify(replacement_t *replacement, const memory_t *mem,
                   replacement_event_t event, uint32_t pid, uint32_t vpn)
{
    if (replacement->observer != NULL) {
        replacement->observer(mem, event, pid, vpn, replacement->observer_context);
    }
}

static page_table_entry_t *frame_pte(const memory_t *mem, uint32_t frame)
{
    const frame_t *owner = &mem->frames[frame];
    process_t *proc = memory_find_process(mem, owner->pid);
    return proc != NULL ? &proc->page_table[owner->vpn] : NULL;
}

static eviction_plan_t make_plan(const memory_t *mem, uint32_t frame,
                                  uint32_t incoming_vpn, uint32_t now_ms,
                                  uint32_t tau_ms)
{
    eviction_plan_t plan = {0};
    uint32_t start = frame;
    uint32_t end = frame + 1;
    if (addr_frames_needed(incoming_vpn) == 2) {
        start = frame & ~1u;
        end = start + 2;
    }
    for (uint32_t f = start; f < end; f++) {
        uint32_t head = memory_head_frame(mem, f);
        if (head == NO_FRAME || (plan.count > 0 && head == plan.frames[0])) {
            continue;
        }
        const page_table_entry_t *pte = frame_pte(mem, head);
        if (pte == NULL || !pte->present) {
            return plan;
        }
        plan.frames[plan.count++] = head;
        plan.dirty += pte->modified ? 1u : 0u;
        uint32_t age = now_ms >= pte->last_access_t ? now_ms - pte->last_access_t : 0;
        plan.young += age <= tau_ms ? 1u : 0u;
    }
    if (plan.count == 0) {
        return plan;
    }
    plan.next_hand = addr_frames_needed(incoming_vpn) == 2 ? end
                   : frame + addr_frames_needed(mem->frames[frame].vpn);
    /* WSClock adaptado: antigo/limpo, limpo recente, antigo/sujo, sujo recente.
     * O fallback recente limita a busca mesmo quando todo o working set esta ativo. */
    plan.priority = plan.dirty == 0 ? (plan.young == 0 ? 0u : 1u)
                                  : (plan.young == 0 ? 2u : 3u);
    plan.valid = true;
    return plan;
}

static bool better_plan(const eviction_plan_t *candidate, const eviction_plan_t *best)
{
    if (!candidate->valid) {
        return false;
    }
    if (!best->valid || candidate->priority != best->priority) {
        return !best->valid || candidate->priority < best->priority;
    }
    if (candidate->dirty != best->dirty) {
        return candidate->dirty < best->dirty;
    }
    if (candidate->young != best->young) {
        return candidate->young < best->young;
    }
    return candidate->count < best->count;
}

static eviction_plan_t select_plan(replacement_t *replacement, memory_t *mem,
                                   uint32_t incoming_vpn, uint32_t now_ms)
{
    /* Primeira volta: segunda chance, consultando os bits R reais da MMU.
     * O last_access_t recente tambem protege paginas cujo R sofreu fading. */
    for (uint32_t step = 0; step < NUM_FRAMES; step++) {
        uint32_t f = (replacement->hand + step) % NUM_FRAMES;
        if (!mem->frames[f].used || !mem->frames[f].head) {
            continue;
        }
        page_table_entry_t *pte = frame_pte(mem, f);
        if (pte != NULL && pte->referenced) {
            pte->referenced = false;
            pte->last_access_t = now_ms;
            notify(replacement, mem, REPLACEMENT_REFERENCE_CLEARED,
                   mem->frames[f].pid, mem->frames[f].vpn);
        }
    }

    /* Segunda volta: escolhe um conjunto que realmente libera espaco alinhado.
     * Assim nao ha loop infinito nem despejos dispersos ao pedir uma pagina de 8 KB. */
    eviction_plan_t best = {0};
    for (uint32_t step = 0; step < NUM_FRAMES; step++) {
        uint32_t f = (replacement->hand + step) % NUM_FRAMES;
        if (!mem->frames[f].used || !mem->frames[f].head) {
            continue;
        }
        eviction_plan_t candidate = make_plan(mem, f, incoming_vpn,
                                               now_ms, replacement->tau_ms);
        if (better_plan(&candidate, &best)) {
            best = candidate;
        }
    }
    return best;
}

replacement_result_t replacement_handle_fault(replacement_t *replacement,
                                                memory_t *mem, swap_t *swap,
                                                process_t *proc,
                                                const translation_t *translation,
                                                bool is_write,
                                                uint32_t timestamp_ms,
                                                uint32_t *frame_out)
{
    if (frame_out != NULL) {
        *frame_out = NO_FRAME;
    }
    if (replacement == NULL || mem == NULL || swap == NULL || swap->hardware == NULL ||
        proc == NULL || translation == NULL ||
        memory_find_process(mem, proc->pid) != proc) {
        return REPLACEMENT_INVALID;
    }
    translation_t checked;
    if (!addr_translate(translation->virtual_address, &checked) ||
        checked.vpn != translation->vpn || checked.offset != translation->offset ||
        checked.page_size != translation->page_size ||
        proc->page_table[checked.vpn].present) {
        return REPLACEMENT_INVALID;
    }
    swap_advance_to(swap, timestamp_ms);

    if (memory_find_free(mem, checked.vpn) == NO_FRAME) {
        eviction_plan_t plan = select_plan(replacement, mem, checked.vpn, swap->now_ms);
        if (!plan.valid) {
            return REPLACEMENT_NO_VICTIM;
        }
        for (uint32_t i = 0; i < plan.count; i++) {
            uint32_t frame = plan.frames[i];
            uint32_t pid = mem->frames[frame].pid;
            uint32_t vpn = mem->frames[frame].vpn;
            process_t *victim = memory_find_process(mem, pid);
            bool dirty = victim->page_table[vpn].modified;
            if (dirty && !swap_page_out(swap)) {
                return REPLACEMENT_SWAP_ERROR; /* a pagina nao gravada permanece mapeada */
            }
            memory_unmap_page(mem, victim, vpn);
            replacement->evictions++;
            if (dirty) {
                replacement->dirty_evictions++;
            } else {
                replacement->clean_evictions++;
            }
            notify(replacement, mem, REPLACEMENT_PAGE_UNMAPPED, pid, vpn);
            if (replacement->log != NULL) {
                fprintf(replacement->log,
                        "[WSCLOCK] vitima PID=%u VPN=%u quadro=%u M=%u | t=%u ms\n",
                        pid, vpn, frame, dirty ? 1u : 0u, swap->now_ms);
            }
        }
        replacement->hand = plan.next_hand % NUM_FRAMES;
    }

    uint32_t frame = memory_load_page(mem, proc, &checked, is_write, swap->now_ms);
    if (frame == NO_FRAME) {
        return REPLACEMENT_NO_VICTIM;
    }
    notify(replacement, mem, REPLACEMENT_PAGE_MAPPED, proc->pid, checked.vpn);
    if (replacement->log != NULL) {
        fprintf(replacement->log, "[WSCLOCK] carga PID=%u VPN=%u quadro=%u | t=%u ms\n",
                proc->pid, checked.vpn, frame, swap->now_ms);
    }
    if (frame_out != NULL) {
        *frame_out = frame;
    }
    return REPLACEMENT_OK;
}
