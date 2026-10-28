#include <stdio.h>
#include <stdlib.h>

#include "memory.h"

static int failures = 0;

#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            failures++;                          \
            fprintf(stderr, "FALHA: " __VA_ARGS__); \
            fprintf(stderr, "\n");               \
        }                                        \
    } while (0)

/* Traduz va e carrega a página correspondente, retornando o quadro base obtido. */
static uint32_t load(memory_t *mem, process_t *proc, uint32_t va, bool write)
{
    translation_t t;
    addr_translate(va, &t);
    return memory_load_page(mem, proc, &t, write, 0);
}

static void test_alocacao(void)
{
    memory_t mem;
    memory_init(&mem);
    process_t *p = memory_get_process(&mem, 1);

    uint32_t even = load(&mem, p, addr_page_base(0), false);
    uint32_t odd = load(&mem, p, addr_page_base(1), true);

    CHECK(even != NO_FRAME && odd != NO_FRAME, "falha ao carregar paginas iniciais");
    CHECK(odd % 2 == 0, "pagina impar fora de par alinhado: quadro %u", odd);
    CHECK(mem.free_frames == NUM_FRAMES - 3, "quadros livres: %u", mem.free_frames);
    CHECK(p->page_table[1].modified && !p->page_table[0].modified, "bit M incorreto");
    CHECK(addr_physical(&p->page_table[1], 100) == odd * FRAME_SIZE + 100, "endereco fisico");
    CHECK(memory_check_consistency(&mem), "inconsistencia apos alocacao");

    CHECK(memory_unmap_page(&mem, p, 1), "pagina escrita deveria estar suja");
    CHECK(!memory_unmap_page(&mem, p, 0), "pagina lida deveria estar limpa");
    CHECK(mem.free_frames == NUM_FRAMES, "quadros nao devolvidos");
    CHECK(memory_check_consistency(&mem), "inconsistencia apos despejo");

    memory_destroy(&mem);
}

static void test_ram_cheia(void)
{
    memory_t mem;
    memory_init(&mem);
    process_t *a = memory_get_process(&mem, 10);
    process_t *b = memory_get_process(&mem, 20);

    // Páginas pares são agrupadas em pares já iniciados, preservando pares livres.
    for (uint32_t i = 0; i < NUM_FRAMES / 2; i++) {
        CHECK(load(&mem, a, addr_page_base(2 * i), false) == i, "par de A no quadro %u", i);
    }
    uint32_t odd = load(&mem, b, addr_page_base(1), true);
    CHECK(odd == NUM_FRAMES / 2, "impar de B esperada no quadro %u, obtido %u",
          (unsigned)NUM_FRAMES / 2, odd);
    memory_unmap_page(&mem, b, 1);

    for (uint32_t i = 0; i < NUM_FRAMES / 2; i++) {
        CHECK(load(&mem, b, addr_page_base(2 * i), true) != NO_FRAME, "par de B %u", i);
    }
    CHECK(mem.free_frames == 0, "RAM deveria estar cheia, livres %u", mem.free_frames);

    translation_t t;
    addr_translate(addr_page_base(3), &t);
    CHECK(memory_load_page(&mem, a, &t, false, 0) == NO_FRAME, "RAM cheia aceitou pagina");

    uint32_t victim = a->page_table[0].frame_number;
    CHECK(!memory_would_fit(&mem, victim, 3), "um quadro isolado nao comporta pagina impar");
    CHECK(memory_would_fit(&mem, victim, 4), "um quadro comporta pagina par");

    memory_unmap_page(&mem, a, 0);
    uint32_t buddy = memory_head_frame(&mem, victim ^ 1u);
    const frame_t *owner = &mem.frames[buddy];
    process_t *bp = memory_find_process(&mem, owner->pid);
    CHECK(memory_would_fit(&mem, buddy, 3), "despejar o vizinho libera o par");
    memory_unmap_page(&mem, bp, owner->vpn);

    CHECK(memory_load_page(&mem, a, &t, false, 0) == (victim & ~1u), "impar no par liberado");
    CHECK(memory_check_consistency(&mem), "inconsistencia com RAM cheia");
    printf("RAM de %u KB preenchida com %u quadros de %u KB.\n",
           (unsigned)PHYSICAL_RAM_SIZE / 1024, (unsigned)NUM_FRAMES, FRAME_SIZE / 1024);

    memory_destroy(&mem);
}

static void test_fragmentacao(void)
{
    memory_t mem;
    hardware_state_t hw;
    memory_init(&mem);
    hardware_init(&hw);
    process_t *p = memory_get_process(&mem, 7);

    // 1 KB usado em uma página par e em uma ímpar.
    uint32_t even = load(&mem, p, addr_page_base(0), false);
    uint32_t odd = load(&mem, p, addr_page_base(1), false);
    translation_t t;
    CHECK(memory_access(&mem, &hw, p, addr_page_base(0) + 1023, false, 1, &t) == ACCESS_HIT, "hit par");
    CHECK(memory_access(&mem, &hw, p, addr_page_base(1) + 1023, false, 1, &t) == ACCESS_HIT, "hit impar");

    uint32_t used, wasted, odd_waste = 0;
    memory_frame_usage(&mem, even, &used, &wasted);
    CHECK(used == 1024 && wasted == 3072, "par: usado %u desperdicio %u", used, wasted);
    for (uint32_t f = odd; f < odd + 2; f++) {
        memory_frame_usage(&mem, f, &used, &wasted);
        odd_waste += wasted;
    }
    CHECK(odd_waste == 7168, "impar: desperdicio %u", odd_waste);
    printf("Fragmentacao interna com 1 KB usado: par %.1f%% | impar %.1f%%\n",
           100.0 * 3072 / PAGE_SIZE_EVEN, 100.0 * odd_waste / PAGE_SIZE_ODD);

    CHECK(memory_access(&mem, &hw, p, VIRTUAL_MEMORY_SIZE, false, 2, &t) == ACCESS_INVALID, "invalido");
    CHECK(memory_access(&mem, &hw, p, addr_page_base(5), true, 2, &t) == ACCESS_FAULT, "fault");

    memory_destroy(&mem);
}

static void test_processos(void)
{
    memory_t mem;
    memory_init(&mem);

    for (uint32_t pid = 0; pid < 50; pid++) {
        CHECK(memory_get_process(&mem, pid) != NULL, "criacao do pid %u", pid);
    }
    CHECK(mem.process_count == 50, "contagem de processos %zu", mem.process_count);
    CHECK(memory_get_process(&mem, 3) == memory_find_process(&mem, 3), "pid duplicado");
    CHECK(memory_find_process(&mem, 99) == NULL, "pid inexistente encontrado");

    memory_destroy(&mem);
    CHECK(mem.processes == NULL && mem.process_count == 0, "destroy nao limpou estado");
}

int main(void)
{
    srand(42);
    test_alocacao();
    test_ram_cheia();
    test_fragmentacao();
    test_processos();

    printf("\n%s (%d falhas)\n", failures ? "REPROVADO" : "APROVADO", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
