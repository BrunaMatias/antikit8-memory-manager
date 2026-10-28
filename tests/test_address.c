#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "address.h"
#include "hardware_specs.h"

static int failures = 0;

#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            failures++;                          \
            fprintf(stderr, "FALHA: " __VA_ARGS__); \
            fprintf(stderr, "\n");               \
        }                                        \
    } while (0)

/* Confere VPN e offset de um endereço contra os valores esperados pelo enunciado. */
static void expect(uint32_t va, uint32_t vpn, uint32_t offset)
{
    translation_t t;
    addr_log(stdout, va);
    CHECK(addr_translate(va, &t), "VA %u deveria ser valido", va);
    CHECK(t.vpn == vpn && t.offset == offset,
          "VA %u: obtido VPN %u Offset %u, esperado VPN %u Offset %u",
          va, t.vpn, t.offset, vpn, offset);
}

static void test_enunciado(void)
{
    printf("== Teste 1: limites da paginacao assimetrica ==\n");
    expect(12287, 1, 8191);
    expect(12288, 2, 0);
    expect(VIRTUAL_MEMORY_SIZE - 1, NUM_VIRTUAL_PAGES - 1, PAGE_SIZE_EVEN - 1);

    translation_t t;
    addr_log(stdout, VIRTUAL_MEMORY_SIZE);
    CHECK(!addr_translate(VIRTUAL_MEMORY_SIZE, &t), "VA 1048576 deveria ser invalido");
    CHECK(NUM_VIRTUAL_PAGES == 171, "esperadas 171 paginas, obtidas %u", NUM_VIRTUAL_PAGES);
    printf("Ultima VPN valida: %u\n\n", NUM_VIRTUAL_PAGES - 1);
}

/* Varre o espaço virtual inteiro comparando o cálculo direto com a soma acumulada. */
static void test_exaustivo(void)
{
    uint32_t vpn = 0;
    uint32_t base = 0;

    for (uint32_t va = 0; va < VIRTUAL_MEMORY_SIZE; va++) {
        if (va == base + addr_page_size(vpn)) {
            base += addr_page_size(vpn);
            vpn++;
        }
        translation_t t;
        if (!addr_translate(va, &t) || t.vpn != vpn || t.offset != va - base) {
            CHECK(0, "VA %u divergente da referencia iterativa", va);
            return;
        }
        CHECK(addr_page_base(t.vpn) + t.offset == va, "base da VPN %u incorreta", t.vpn);
    }
    CHECK(vpn == NUM_VIRTUAL_PAGES - 1, "varredura terminou na VPN %u", vpn);
    printf("Varredura exaustiva de %u enderecos concluida.\n", (unsigned)VIRTUAL_MEMORY_SIZE);
}

/* Garante que a MMU do professor resolve cada limite de página para a mesma VPN. */
static void test_contra_mmu(void)
{
    hardware_state_t hw;
    page_table_entry_t table[NUM_VIRTUAL_PAGES];
    hardware_init(&hw);

    for (uint32_t vpn = 0; vpn < NUM_VIRTUAL_PAGES; vpn++) {
        uint32_t limits[2] = { addr_page_base(vpn), addr_page_base(vpn) + addr_page_size(vpn) - 1 };
        for (int i = 0; i < 2; i++) {
            translation_t t;
            addr_translate(limits[i], &t);

            memset(table, 0, sizeof(table));
            CHECK(!mmu_access_memory(&hw, table, NUM_VIRTUAL_PAGES, limits[i], false, 0),
                  "VA %u deveria gerar page fault", limits[i]);

            table[t.vpn].present = true;
            CHECK(mmu_access_memory(&hw, table, NUM_VIRTUAL_PAGES, limits[i], false, 0),
                  "MMU nao reconheceu VA %u na VPN %u", limits[i], t.vpn);
        }
    }
    printf("Limites de %u paginas confirmados pela MMU.\n", NUM_VIRTUAL_PAGES);
}

int main(void)
{
    srand(42);
    test_enunciado();
    test_exaustivo();
    test_contra_mmu();

    printf("\n%s (%d falhas)\n", failures ? "REPROVADO" : "APROVADO", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
