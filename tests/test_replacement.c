#include <stdio.h>
#include <stdlib.h>

#include "replacement.h"

static int failures;

#define CHECK(cond, message) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FALHA: %s\n", message); } \
} while (0)

typedef struct {
    memory_t mem;
    hardware_state_t hw;
    swap_t swap;
    replacement_t clock;
    process_t *owner;
    process_t *incoming;
    unsigned mapped;
    unsigned unmapped;
    unsigned cleared;
} fixture_t;

static void observe(const memory_t *mem, replacement_event_t event,
                     uint32_t pid, uint32_t vpn, void *context)
{
    fixture_t *f = context;
    process_t *proc = memory_find_process(mem, pid);
    CHECK(memory_check_consistency(mem), "auditor recebeu RAM inconsistente");
    CHECK(proc != NULL && vpn < NUM_VIRTUAL_PAGES, "evento de pagina inexistente");
    if (proc == NULL || vpn >= NUM_VIRTUAL_PAGES) {
        return;
    }
    if (event == REPLACEMENT_PAGE_MAPPED) {
        f->mapped++;
        CHECK(proc->page_table[vpn].present, "aviso de carga antes de present=1");
    } else if (event == REPLACEMENT_PAGE_UNMAPPED) {
        f->unmapped++;
        CHECK(!proc->page_table[vpn].present, "aviso de despejo antes de present=0");
    } else {
        f->cleared++;
        CHECK(!proc->page_table[vpn].referenced, "aviso antes de limpar R");
    }
}

static void init(fixture_t *f)
{
    memory_init(&f->mem);
    hardware_init(&f->hw);
    swap_init(&f->swap, &f->hw, NULL);
    replacement_init(&f->clock, WSCLOCK_DEFAULT_TAU_MS, NULL);
    f->mapped = f->unmapped = f->cleared = 0;
    replacement_set_observer(&f->clock, observe, f);
    f->owner = memory_get_process(&f->mem, 1);
    f->incoming = memory_get_process(&f->mem, 2);
    if (f->owner == NULL || f->incoming == NULL) {
        fprintf(stderr, "Sem memoria para os testes\n");
        exit(EXIT_FAILURE);
    }
}

static void fill_even(fixture_t *f, bool dirty)
{
    for (uint32_t i = 0; i < NUM_FRAMES; i++) {
        translation_t t;
        addr_translate(addr_page_base(2 * i), &t);
        CHECK(memory_load_page(&f->mem, f->owner, &t, dirty, 0) == i, "preenchimento par");
        f->owner->page_table[2 * i].referenced = false;
    }
}

static replacement_result_t fault(fixture_t *f, uint32_t vpn, bool write,
                                    uint32_t timestamp, uint32_t *frame)
{
    translation_t t;
    swap_advance_to(&f->swap, timestamp);
    access_result_t result = memory_access(&f->mem, &f->hw, f->incoming,
                                            addr_page_base(vpn), write,
                                            f->swap.now_ms, &t);
    CHECK(result == ACCESS_FAULT, "acesso deveria gerar falta");
    uint64_t accesses = f->hw.global_access_count;
    replacement_result_t handled = replacement_handle_fault(&f->clock, &f->mem, &f->swap,
                                                              f->incoming, &t, write,
                                                              timestamp, frame);
    CHECK(f->hw.global_access_count == accesses, "tratamento repetiu o acesso a MMU");
    CHECK(memory_check_consistency(&f->mem), "RAM inconsistente apos falta");
    return handled;
}

static void test_sem_despejo(void)
{
    fixture_t f;
    uint32_t frame;
    init(&f);
    CHECK(fault(&f, 1, true, 10, &frame) == REPLACEMENT_OK, "falta com RAM livre");
    CHECK(frame % 2 == 0 && f.mem.free_frames == NUM_FRAMES - 2, "alinhamento da carga");
    CHECK(f.incoming->page_table[1].modified && f.incoming->page_table[1].referenced,
          "escrita que gerou falta nao foi concluida");
    CHECK(f.clock.evictions == 0 && f.swap.page_outs == 0 && f.mapped == 1,
          "despejo ou E/S desnecessarios");
    translation_t t;
    addr_translate(addr_page_base(1), &t);
    CHECK(replacement_handle_fault(&f.clock, &f.mem, &f.swap, f.incoming,
                                     &t, false, 10, &frame) == REPLACEMENT_INVALID,
          "handler aceitou pagina ja presente");
    t.vpn = NUM_VIRTUAL_PAGES;
    CHECK(replacement_handle_fault(&f.clock, &f.mem, &f.swap, f.incoming,
                                     &t, false, 10, NULL) == REPLACEMENT_INVALID,
          "handler aceitou traducao invalida");
    memory_destroy(&f.mem);
}

static void test_preferencia_limpa_e_fading(void)
{
    fixture_t f;
    uint32_t frame;
    init(&f);
    fill_even(&f, true);
    f.owner->page_table[20].modified = false;
    CHECK(fault(&f, 0, false, 200, &frame) == REPLACEMENT_OK, "selecao limpa");
    CHECK(frame == 10 && !f.owner->page_table[20].present, "nao preferiu pagina limpa");
    CHECK(f.swap.page_outs == 0 && f.clock.clean_evictions == 1, "limpa gerou Page Out");
    memory_destroy(&f.mem);

    init(&f);
    fill_even(&f, true);
    /* R=0 nao basta para declarar uma pagina antiga: pode ser fading. */
    f.owner->page_table[0].modified = false;
    f.owner->page_table[0].last_access_t = 190;
    f.owner->page_table[2].modified = false;
    CHECK(fault(&f, 0, false, 200, &frame) == REPLACEMENT_OK, "fading e idade");
    CHECK(frame == 1 && f.owner->page_table[0].present, "fading ignorou acesso recente");
    memory_destroy(&f.mem);
}

static void test_segunda_chance_e_ponteiro(void)
{
    fixture_t f;
    uint32_t frame;
    init(&f);
    fill_even(&f, true);
    f.owner->page_table[0].modified = false;
    f.owner->page_table[0].referenced = true;
    f.owner->page_table[2].modified = false;
    CHECK(fault(&f, 0, false, 200, &frame) == REPLACEMENT_OK, "segunda chance");
    CHECK(frame == 1 && f.owner->page_table[0].last_access_t == 200 && f.cleared == 1,
          "segunda chance nao protegeu R=1");
    memory_destroy(&f.mem);

    init(&f);
    fill_even(&f, false);
    for (uint32_t i = 0; i < NUM_FRAMES; i++) {
        f.owner->page_table[2 * i].referenced = true;
    }
    CHECK(fault(&f, 0, false, 200, &frame) == REPLACEMENT_OK && frame == 0,
          "fallback nao terminou com todas as paginas referenciadas");
    CHECK(fault(&f, 2, false, 201, &frame) == REPLACEMENT_OK && frame == 1,
          "ponteiro circular nao avancou");
    CHECK(f.clock.hand == 2, "ponteiro circular incorreto");
    memory_destroy(&f.mem);
}

static void test_pares_alinhados(void)
{
    fixture_t f;
    uint32_t frame;
    init(&f);
    fill_even(&f, false);
    CHECK(fault(&f, 1, false, 200, &frame) == REPLACEMENT_OK, "duas vitimas de 4 KB");
    CHECK(frame == 0 && f.clock.evictions == 2 && f.mem.free_frames == 0,
          "pagina de 8 KB nao liberou exatamente um par");
    CHECK(f.unmapped == 2 && f.mapped == 1, "auditor perdeu alteracao no par");
    memory_destroy(&f.mem);

    init(&f);
    fill_even(&f, false);
    memory_unmap_page(&f.mem, f.owner, 14); /* quadro 7 livre, vizinho 6 ocupado */
    CHECK(fault(&f, 1, false, 200, &frame) == REPLACEMENT_OK, "par parcialmente livre");
    CHECK(frame == 6 && f.clock.evictions == 1, "despejou paginas a mais para formar par");
    memory_destroy(&f.mem);

    init(&f);
    fill_even(&f, true);
    f.owner->page_table[4].modified = false;
    f.owner->page_table[6].modified = false;
    CHECK(fault(&f, 1, false, 200, &frame) == REPLACEMENT_OK && frame == 2,
          "nao escolheu o par de paginas limpas");
    CHECK(f.swap.page_outs == 0, "par limpo gerou escrita");
    memory_destroy(&f.mem);

    init(&f);
    fill_even(&f, true);
    CHECK(fault(&f, 1, true, 200, &frame) == REPLACEMENT_OK, "par de paginas sujas");
    CHECK(f.swap.page_outs == 2 && f.clock.dirty_evictions == 2 && f.swap.io_cpu_cycles == 2000,
          "nao gravou exatamente as duas paginas sujas");
    memory_destroy(&f.mem);
}

static void test_vitima_de_8kb(void)
{
    fixture_t f;
    uint32_t frame;
    init(&f);
    for (uint32_t i = 0; i < NUM_FRAMES / 2; i++) {
        translation_t t;
        addr_translate(addr_page_base(2 * i + 1), &t);
        CHECK(memory_load_page(&f.mem, f.owner, &t, true, 0) != NO_FRAME, "carga impar");
    }
    CHECK(fault(&f, 0, false, 200, &frame) == REPLACEMENT_OK, "vitima de 8 KB");
    CHECK(f.clock.evictions == 1 && f.swap.page_outs == 1 && f.mem.free_frames == 1,
          "pagina de 8 KB foi gravada duas vezes");
    CHECK(fault(&f, 2, false, 200, &frame) == REPLACEMENT_OK && f.clock.evictions == 1,
          "nao aproveitou quadro restante da pagina de 8 KB");
    memory_destroy(&f.mem);
}

static void test_falha_de_swap_preserva_pagina(void)
{
    fixture_t f;
    uint32_t frame;
    init(&f);
    fill_even(&f, true);
    swap_advance_to(&f.swap, UINT32_MAX);
    CHECK(swap_page_out(&f.swap) && swap_page_out(&f.swap), "preparacao de limite termico");
    CHECK(fault(&f, 0, false, UINT32_MAX, &frame) == REPLACEMENT_SWAP_ERROR,
          "limite de timestamp nao gerou erro");
    CHECK(f.clock.evictions == 0 && f.mem.free_frames == 0 && frame == NO_FRAME,
          "pagina suja removida antes de gravar no Swap");
    memory_destroy(&f.mem);
}

static void test_varios_processos(void)
{
    fixture_t f;
    init(&f);
    uint32_t random_state = 1234567;
    uint64_t faults = 0;
    for (uint32_t i = 0; i < 5000; i++) {
        random_state = random_state * 1664525u + 1013904223u;
        uint32_t pid = 1 + ((random_state >> 24) % 5);
        uint32_t vpn = (random_state >> 8) % NUM_VIRTUAL_PAGES;
        uint32_t offset = (random_state >> 16) % addr_page_size(vpn);
        bool write = (random_state & 1u) != 0;
        process_t *proc = memory_get_process(&f.mem, pid);
        if (proc == NULL) {
            CHECK(false, "alocacao do processo");
            break;
        }
        swap_advance_to(&f.swap, i / 7);
        translation_t t;
        access_result_t result = memory_access(&f.mem, &f.hw, proc,
                                                addr_page_base(vpn) + offset,
                                                write, f.swap.now_ms, &t);
        if (result == ACCESS_FAULT) {
            faults++;
            CHECK(replacement_handle_fault(&f.clock, &f.mem, &f.swap, proc, &t,
                                             write, i / 7, NULL) == REPLACEMENT_OK,
                  "falta nao resolvida em teste multiprocesso");
        } else {
            CHECK(result == ACCESS_HIT, "endereco valido rejeitado");
        }
        CHECK(memory_check_consistency(&f.mem), "inconsistencia multiprocesso");
    }
    CHECK(f.hw.global_access_count == 5000 && f.mapped == faults, "acessos/faltas duplicados");
    CHECK(f.swap.page_outs == f.clock.dirty_evictions, "Page Outs e vitimas sujas divergem");
    CHECK(f.hw.total_cpu_cycles == 5000 + f.swap.io_cpu_cycles, "ciclos totais divergentes");
    CHECK(f.swap.max_temperature < MAX_SAFE_TEMP && f.swap.io_cpu_cycles == f.swap.page_outs * 1000,
          "penalidade termica em teste multiprocesso");
    memory_destroy(&f.mem);
}

int main(void)
{
    srand(42);
    test_sem_despejo();
    test_preferencia_limpa_e_fading();
    test_segunda_chance_e_ponteiro();
    test_pares_alinhados();
    test_vitima_de_8kb();
    test_falha_de_swap_preserva_pagina();
    test_varios_processos();
    printf("WSClock: %s (%d falhas)\n", failures ? "REPROVADO" : "APROVADO", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
