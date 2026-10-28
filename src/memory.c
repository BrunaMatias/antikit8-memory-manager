#include "memory.h"

#include <stdlib.h>
#include <string.h>

void memory_init(memory_t *mem)
{
    memset(mem, 0, sizeof(*mem));
    mem->free_frames = NUM_FRAMES;
}

void memory_destroy(memory_t *mem)
{
    for (size_t i = 0; i < mem->process_count; i++) {
        free(mem->processes[i]);
    }
    free(mem->processes);
    memory_init(mem);
}

process_t *memory_find_process(const memory_t *mem, uint32_t pid)
{
    for (size_t i = 0; i < mem->process_count; i++) {
        if (mem->processes[i]->pid == pid) {
            return mem->processes[i];
        }
    }
    return NULL;
}

process_t *memory_get_process(memory_t *mem, uint32_t pid)
{
    process_t *proc = memory_find_process(mem, pid);
    if (proc != NULL) {
        return proc;
    }

    if (mem->process_count == mem->process_capacity) {
        size_t capacity = mem->process_capacity ? mem->process_capacity * 2 : 4;
        process_t **grown = realloc(mem->processes, capacity * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        mem->processes = grown;
        mem->process_capacity = capacity;
    }

    // calloc zera a tabela, deixando todas as páginas com present = 0.
    proc = calloc(1, sizeof(*proc));
    if (proc == NULL) {
        return NULL;
    }
    proc->pid = pid;
    mem->processes[mem->process_count++] = proc;
    return proc;
}

access_result_t memory_access(memory_t *mem, hardware_state_t *hw, process_t *proc,
                              uint32_t virtual_address, bool is_write,
                              uint32_t timestamp, translation_t *out)
{
    if (!addr_translate(virtual_address, out)) {
        return ACCESS_INVALID;
    }

    if (!mmu_access_memory(hw, proc->page_table, NUM_VIRTUAL_PAGES,
                           virtual_address, is_write, timestamp)) {
        return ACCESS_FAULT;
    }

    frame_t *head = &mem->frames[proc->page_table[out->vpn].frame_number];
    if (out->offset + 1 > head->bytes_used) {
        head->bytes_used = out->offset + 1;
    }
    return ACCESS_HIT;
}

uint32_t memory_find_free(const memory_t *mem, uint32_t vpn)
{
    if (addr_frames_needed(vpn) == 2) {
        for (uint32_t f = 0; f + 1 < NUM_FRAMES; f += 2) {
            if (!mem->frames[f].used && !mem->frames[f + 1].used) {
                return f;
            }
        }
        return NO_FRAME;
    }

    // Prioriza quadros cujo vizinho já está ocupado para preservar pares livres.
    uint32_t fallback = NO_FRAME;
    for (uint32_t f = 0; f < NUM_FRAMES; f++) {
        if (mem->frames[f].used) {
            continue;
        }
        if (mem->frames[f ^ 1u].used) {
            return f;
        }
        if (fallback == NO_FRAME) {
            fallback = f;
        }
    }
    return fallback;
}

uint32_t memory_load_page(memory_t *mem, process_t *proc, const translation_t *t,
                          bool is_write, uint32_t timestamp)
{
    page_table_entry_t *pte = &proc->page_table[t->vpn];
    if (pte->present) {
        return pte->frame_number;
    }

    uint32_t base = memory_find_free(mem, t->vpn);
    if (base == NO_FRAME) {
        return NO_FRAME;
    }

    uint32_t count = addr_frames_needed(t->vpn);
    for (uint32_t i = 0; i < count; i++) {
        frame_t *frame = &mem->frames[base + i];
        frame->used = true;
        frame->head = (i == 0);
        frame->pid = proc->pid;
        frame->vpn = t->vpn;
        frame->bytes_used = (i == 0) ? t->offset + 1 : 0;
    }
    mem->free_frames -= count;

    pte->frame_number = base;
    pte->present = true;
    pte->referenced = true;
    pte->modified = is_write;
    pte->last_access_t = timestamp;
    return base;
}

bool memory_unmap_page(memory_t *mem, process_t *proc, uint32_t vpn)
{
    page_table_entry_t *pte = &proc->page_table[vpn];
    if (!pte->present) {
        return false;
    }

    bool dirty = pte->modified;
    uint32_t count = addr_frames_needed(vpn);
    for (uint32_t i = 0; i < count; i++) {
        memset(&mem->frames[pte->frame_number + i], 0, sizeof(frame_t));
    }
    mem->free_frames += count;

    memset(pte, 0, sizeof(*pte));
    return dirty;
}

uint32_t memory_head_frame(const memory_t *mem, uint32_t frame)
{
    if (frame >= NUM_FRAMES || !mem->frames[frame].used) {
        return NO_FRAME;
    }
    return mem->frames[frame].head ? frame : frame - 1;
}

bool memory_would_fit(const memory_t *mem, uint32_t victim_frame, uint32_t vpn)
{
    if (victim_frame >= NUM_FRAMES) {
        return false;
    }
    if (addr_frames_needed(vpn) == 1) {
        return true;
    }

    // A página ímpar precisa do par alinhado inteiro livre após o despejo.
    uint32_t victim = memory_head_frame(mem, victim_frame);
    uint32_t pair = (victim == NO_FRAME ? victim_frame : victim) & ~1u;
    for (uint32_t f = pair; f < pair + 2; f++) {
        uint32_t owner = memory_head_frame(mem, f);
        if (owner != NO_FRAME && owner != victim) {
            return false;
        }
    }
    return true;
}

void memory_frame_usage(const memory_t *mem, uint32_t frame,
                        uint32_t *used, uint32_t *wasted)
{
    uint32_t head = memory_head_frame(mem, frame);
    if (head == NO_FRAME) {
        *used = 0;
        *wasted = 0;
        return;
    }

    uint32_t touched = mem->frames[head].bytes_used;
    uint32_t start = (frame - head) * FRAME_SIZE;
    uint32_t in_frame = touched > start ? touched - start : 0;

    *used = in_frame < FRAME_SIZE ? in_frame : FRAME_SIZE;
    *wasted = FRAME_SIZE - *used;
}

bool memory_check_consistency(const memory_t *mem)
{
    uint32_t mapped = 0;

    for (size_t p = 0; p < mem->process_count; p++) {
        const process_t *proc = mem->processes[p];
        for (uint32_t vpn = 0; vpn < NUM_VIRTUAL_PAGES; vpn++) {
            const page_table_entry_t *pte = &proc->page_table[vpn];
            if (!pte->present) {
                continue;
            }
            uint32_t count = addr_frames_needed(vpn);
            uint32_t base = pte->frame_number;
            if (base + count > NUM_FRAMES || (count == 2 && base % 2 != 0)) {
                return false;
            }
            for (uint32_t i = 0; i < count; i++) {
                const frame_t *frame = &mem->frames[base + i];
                if (!frame->used || frame->head != (i == 0) ||
                    frame->pid != proc->pid || frame->vpn != vpn) {
                    return false;
                }
            }
            mapped += count;
        }
    }

    uint32_t used = 0;
    for (uint32_t f = 0; f < NUM_FRAMES; f++) {
        used += mem->frames[f].used ? 1 : 0;
    }
    return mapped == used && used + mem->free_frames == NUM_FRAMES;
}
