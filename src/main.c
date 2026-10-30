#include "memory.h"
#include "replacement.h"
#include "swap.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t pid;
    uint64_t page_faults;
} process_stats_t;

typedef struct {
    uint64_t checksum;
} audit_t;

static void skip_space(char **cursor)
{
    while (isspace((unsigned char)**cursor)) {
        (*cursor)++;
    }
}

static bool at_end_or_comment(char *cursor)
{
    skip_space(&cursor);
    return *cursor == '\0' || *cursor == '#';
}

static bool parse_number(char **cursor, bool address, uint32_t *value)
{
    skip_space(cursor);
    if (**cursor == '\0' || **cursor == '#' || **cursor == '-') {
        return false;
    }

    int base = 10;
    if (address && (*cursor)[0] == '0' &&
        ((*cursor)[1] == 'x' || (*cursor)[1] == 'X')) {
        base = 16;
    }

    errno = 0;
    char *end = NULL;
    uintmax_t parsed = strtoumax(*cursor, &end, base);
    if (end == *cursor || errno == ERANGE || parsed > UINT32_MAX ||
        (*end != '\0' && *end != '#' && !isspace((unsigned char)*end))) {
        return false;
    }

    *value = (uint32_t)parsed;
    *cursor = end;
    return true;
}

/* Returns 1 for a record, 0 for blank/comment lines, and -1 for malformed input. */
static int parse_trace_line(char *line, uint32_t *timestamp, uint32_t *pid,
                            uint32_t *address, bool *is_write)
{
    char *cursor = line;
    if (at_end_or_comment(cursor)) {
        return 0;
    }
    if (!parse_number(&cursor, false, timestamp) ||
        !parse_number(&cursor, false, pid) ||
        !parse_number(&cursor, true, address)) {
        return -1;
    }

    skip_space(&cursor);
    if (*cursor == '\0' || *cursor == '#') {
        return -1;
    }
    char operation = (char)toupper((unsigned char)*cursor);
    if ((operation != 'R' && operation != 'W') ||
        (cursor[1] != '\0' && cursor[1] != '#' &&
         !isspace((unsigned char)cursor[1]))) {
        return -1;
    }
    *is_write = operation == 'W';
    cursor++;
    return at_end_or_comment(cursor) ? 1 : -1;
}

static uint64_t audit_checksum(const memory_t *mem)
{
    uint64_t checksum = 0;
    for (size_t p = 0; p < mem->process_count; p++) {
        const process_t *proc = mem->processes[p];
        for (uint32_t vpn = 0; vpn < NUM_VIRTUAL_PAGES; vpn++) {
            const page_table_entry_t *pte = &proc->page_table[vpn];
            checksum += (uint64_t)pte->present + pte->frame_number;
        }
    }
    return checksum;
}

static void audit_log(audit_t *audit, const memory_t *mem, const char *event,
                      uint32_t pid, uint32_t vpn)
{
    audit->checksum = audit_checksum(mem);
    if (pid == UINT32_MAX) {
        printf("[AUDITORIA] %s | checksum=%" PRIu64 "\n",
               event, audit->checksum);
    } else {
        printf("[AUDITORIA] %s PID=%u VPN=%u | checksum=%" PRIu64 "\n",
               event, pid, vpn, audit->checksum);
    }
}

static void observe_replacement(const memory_t *mem, replacement_event_t event,
                                uint32_t pid, uint32_t vpn, void *context)
{
    audit_t *audit = context;
    const char *name = event == REPLACEMENT_REFERENCE_CLEARED ? "R limpo" :
                       event == REPLACEMENT_PAGE_UNMAPPED ? "pagina removida" :
                       "pagina mapeada";
    audit_log(audit, mem, name, pid, vpn);
}

static void audit_mmu_changes(audit_t *audit, const memory_t *mem,
                              const process_t *proc,
                              const page_table_entry_t *before,
                              uint64_t global_access_count)
{
    bool changed = false;
    for (uint32_t vpn = 0; vpn < NUM_VIRTUAL_PAGES; vpn++) {
        const page_table_entry_t *after = &proc->page_table[vpn];
        const page_table_entry_t *old = &before[vpn];
        if (old->referenced && !after->referenced &&
            global_access_count % FADING_THRESHOLD == 0) {
            printf("[FADING] R efetivamente limpo no acesso global=%" PRIu64
                   " PID=%u VPN=%u\n",
                   global_access_count, proc->pid, vpn);
        }
        if (old->present != after->present ||
            old->frame_number != after->frame_number ||
            old->referenced != after->referenced ||
            old->modified != after->modified ||
            old->last_access_t != after->last_access_t) {
            changed = true;
        }
    }
    if (changed) {
        audit->checksum = audit_checksum(mem);
        printf("[AUDITORIA] alteracao via MMU PID=%u | checksum=%" PRIu64 "\n",
               proc->pid, audit->checksum);
    }
}

static size_t find_stats_index(const process_stats_t *stats, size_t count,
                               uint32_t pid)
{
    for (size_t i = 0; i < count; i++) {
        if (stats[i].pid == pid) {
            return i;
        }
    }
    return count;
}

static bool ensure_stats_capacity(process_stats_t **stats, size_t *capacity,
                                  size_t needed)
{
    if (needed <= *capacity) {
        return true;
    }
    size_t grown_capacity = *capacity ? *capacity * 2 : 4;
    while (grown_capacity < needed) {
        if (grown_capacity > SIZE_MAX / 2) {
            grown_capacity = needed;
            break;
        }
        grown_capacity *= 2;
    }
    if (grown_capacity > SIZE_MAX / sizeof(**stats)) {
        return false;
    }
    process_stats_t *grown = realloc(*stats, grown_capacity * sizeof(**stats));
    if (grown == NULL) {
        return false;
    }
    *stats = grown;
    *capacity = grown_capacity;
    return true;
}

static void print_ram_map(const memory_t *mem, uint64_t processed)
{
    printf("\n[RAM] Mapa apos %" PRIu64
           " linhas (cada caractere representa 1 KB):\n"
           "U=usado F=fragmentacao interna .=livre\n",
           processed);
    for (uint32_t frame = 0; frame < NUM_FRAMES; frame++) {
        uint32_t used;
        uint32_t wasted;
        memory_frame_usage(mem, frame, &used, &wasted);
        uint32_t occupied = used + wasted;
        for (uint32_t kb = 0; kb < FRAME_SIZE / 1024; kb++) {
            uint32_t start = kb * 1024;
            if (start < used) {
                putchar('U');
            } else if (start < occupied) {
                putchar('F');
            } else {
                putchar('.');
            }
        }
        if ((frame + 1) % 16 == 0) {
            putchar('\n');
        }
    }
    putchar('\n');
}

static double fragmentation_ratio(const memory_t *mem)
{
    uint64_t wasted = 0;
    for (uint32_t frame = 0; frame < NUM_FRAMES; frame++) {
        uint32_t used_bytes;
        uint32_t wasted_bytes;
        memory_frame_usage(mem, frame, &used_bytes, &wasted_bytes);
        (void)used_bytes;
        wasted += wasted_bytes;
    }
    uint64_t allocated = (uint64_t)(NUM_FRAMES - mem->free_frames) * FRAME_SIZE;
    return allocated == 0 ? 0.0 : (double)wasted / (double)allocated;
}

static void print_summary(const memory_t *mem, const hardware_state_t *hw,
                         const swap_t *swap, const process_stats_t *stats,
                         size_t stats_count, uint64_t accesses,
                         uint64_t processed, double fragmentation_sum,
                         uint64_t fragmentation_samples)
{
    uint64_t total_faults = 0;
    double final_fragmentation = fragmentation_ratio(mem);
    for (size_t i = 0; i < stats_count; i++) {
        total_faults += stats[i].page_faults;
    }
    printf("\n=== Estatisticas finais ===\n");
    printf("Acessos de memoria no trace: %" PRIu64 "\n", accesses);
    printf("Acessos contabilizados pela MMU: %" PRIu64 "\n", hw->global_access_count);
    printf("Linhas de acesso processadas: %" PRIu64 "\n", processed);
    printf("Page Faults global: %" PRIu64 "\n", total_faults);
    for (size_t i = 0; i < stats_count; i++) {
        printf("Page Faults PID=%u: %" PRIu64 "\n",
               stats[i].pid, stats[i].page_faults);
    }
    printf("Temperatura maxima do Swap: %.1f C\n",
           (double)swap->max_temperature);
    printf("Ciclos de CPU em operacoes de E/S: %" PRIu64 "\n",
           swap->io_cpu_cycles);
    printf("Fragmentacao interna media da RAM: %.2f%% (%" PRIu64
           " amostras)\n",
           fragmentation_samples == 0 ? 0.0 :
               100.0 * fragmentation_sum / (double)fragmentation_samples,
           fragmentation_samples);
    printf("Fragmentacao interna no estado final: %.2f%%\n",
           100.0 * final_fragmentation);
}

int main(int argc, char **argv)
{
    const char *trace_path = argc == 1 ? "memory_trace.txt" :
                             argc == 2 ? argv[1] : NULL;
    if (trace_path == NULL) {
        fprintf(stderr, "Uso: %s [arquivo_trace]\n", argv[0]);
        return EXIT_FAILURE;
    }

    FILE *trace = fopen(trace_path, "r");
    if (trace == NULL) {
        perror(trace_path);
        return EXIT_FAILURE;
    }

    memory_t mem;
    hardware_state_t hw;
    swap_t swap;
    replacement_t replacement;
    process_stats_t *stats = NULL;
    size_t stats_count = 0;
    size_t stats_capacity = 0;
    uint64_t accesses = 0;
    uint64_t processed = 0;
    double fragmentation_sum = 0.0;
    uint64_t fragmentation_samples = 0;
    audit_t audit = {0};
    char line[512];
    unsigned long line_number = 0;
    int exit_status = EXIT_SUCCESS;

    memory_init(&mem);
    hardware_init(&hw);
    swap_init(&swap, &hw, stdout);
    replacement_init(&replacement, WSCLOCK_DEFAULT_TAU_MS, stdout);
    replacement_set_observer(&replacement, observe_replacement, &audit);
    audit_log(&audit, &mem, "estado inicial", UINT32_MAX, UINT32_MAX);

    while (fgets(line, sizeof(line), trace) != NULL) {
        line_number++;
        if (strchr(line, '\n') == NULL && !feof(trace)) {
            fprintf(stderr, "%s:%lu: linha excede o limite de %zu caracteres\n",
                    trace_path, line_number, sizeof(line) - 1);
            exit_status = EXIT_FAILURE;
            break;
        }

        uint32_t timestamp;
        uint32_t pid;
        uint32_t address;
        bool is_write;
        int parsed = parse_trace_line(line, &timestamp, &pid, &address, &is_write);
        if (parsed == 0) {
            continue;
        }
        if (parsed < 0) {
            fprintf(stderr, "%s:%lu: registro invalido; esperado: "
                    "<timestamp_ms> <pid> <endereco_decimal|0xhex> <R|W>\n",
                    trace_path, line_number);
            exit_status = EXIT_FAILURE;
            break;
        }

        processed++;
        accesses++;
        process_t *proc = memory_get_process(&mem, pid);
        if (proc == NULL || !ensure_stats_capacity(&stats, &stats_capacity,
                                                    mem.process_count)) {
            fprintf(stderr, "Falha de memoria ao criar estatisticas do PID %u\n", pid);
            exit_status = EXIT_FAILURE;
            break;
        }
        if (stats_count < mem.process_count) {
            stats[stats_count].pid = pid;
            stats[stats_count].page_faults = 0;
            stats_count++;
        }

        swap_advance_to(&swap, timestamp);
        if (!addr_translate(address, &(translation_t){0})) {
            fprintf(stderr, "[MMU] VA %u invalido (limite de %u bytes)\n",
                    address, VIRTUAL_MEMORY_SIZE);
        } else {
            page_table_entry_t before[NUM_VIRTUAL_PAGES];
            memcpy(before, proc->page_table, sizeof(before));

            translation_t translation;
            access_result_t result = memory_access(&mem, &hw, proc, address,
                                                   is_write, swap.now_ms,
                                                   &translation);
            audit_mmu_changes(&audit, &mem, proc, before, hw.global_access_count);
            if (hw.global_access_count != 0 &&
                hw.global_access_count % FADING_THRESHOLD == 0) {
                printf("[FADING] acesso global=%" PRIu64
                       ": MMU executou sorteio eletrico\n",
                       hw.global_access_count);
            }
            if (result == ACCESS_HIT) {
                printf("[ACESSO] t=%u ms PID=%u VA=%u %c HIT VPN=%u quadro=%u\n",
                       timestamp, pid, address, is_write ? 'W' : 'R',
                       translation.vpn,
                       proc->page_table[translation.vpn].frame_number);
            } else if (result == ACCESS_FAULT) {
                size_t stats_index = find_stats_index(stats, stats_count, pid);
                if (stats_index == stats_count) {
                    fprintf(stderr, "Estatisticas ausentes para PID %u\n", pid);
                    exit_status = EXIT_FAILURE;
                    break;
                }
                stats[stats_index].page_faults++;
                printf("[PAGE FAULT] t=%u ms PID=%u VA=%u VPN=%u\n",
                       timestamp, pid, address, translation.vpn);
                replacement_result_t handled = replacement_handle_fault(
                    &replacement, &mem, &swap, proc, &translation, is_write,
                    timestamp, NULL);
                if (handled != REPLACEMENT_OK) {
                    fprintf(stderr, "Falha ao tratar Page Fault (codigo %d)\n",
                            (int)handled);
                    exit_status = EXIT_FAILURE;
                    break;
                }
            }
        }

        if (mem.free_frames < NUM_FRAMES) {
            fragmentation_sum += fragmentation_ratio(&mem);
            fragmentation_samples++;
        }
        if (processed % 10 == 0) {
            print_ram_map(&mem, processed);
        }
    }

    if (ferror(trace)) {
        perror(trace_path);
        exit_status = EXIT_FAILURE;
    }
    if (fclose(trace) != 0) {
        perror(trace_path);
        exit_status = EXIT_FAILURE;
    }

    if (exit_status == EXIT_SUCCESS) {
        print_summary(&mem, &hw, &swap, stats, stats_count, accesses,
                      processed, fragmentation_sum, fragmentation_samples);
    }
    free(stats);
    memory_destroy(&mem);
    return exit_status;
}
