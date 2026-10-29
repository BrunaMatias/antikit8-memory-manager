#include <stdio.h>
#include <stdlib.h>

#include "swap.h"

static int failures;

#define CHECK(cond, message) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FALHA: %s\n", message); } \
} while (0)

static void test_rajada(void)
{
    hardware_state_t hw;
    swap_t swap;
    hardware_init(&hw);
    swap_init(&swap, &hw, NULL);
    for (unsigned i = 0; i < 10; i++) {
        swap_advance_to(&swap, 200);
        CHECK(swap_page_out(&swap), "Page Out seguro recusado");
        CHECK(hw.swap_temperature < MAX_SAFE_TEMP, "Swap superaqueceu");
        CHECK(hw.total_cpu_cycles == (i + 1u) * 1000u, "houve penalidade termica");
    }
    CHECK(swap.page_outs == 10 && swap.io_cpu_cycles == 10000, "contadores de E/S");
    CHECK(swap.waiting_ms > 0 && swap.now_ms > 200, "faltou espera virtual");
    CHECK(swap.max_temperature == 64.0f, "pico termico inesperado");
}

static void test_resfriamento_unico(void)
{
    hardware_state_t hw;
    swap_t swap;
    hardware_init(&hw);
    swap_init(&swap, &hw, NULL);
    swap_advance_to(&swap, 10);
    CHECK(swap_page_out(&swap), "primeira escrita");
    CHECK(hw.swap_temperature == 37.0f, "incremento de outro ms deveria ser 12 C");
    swap_advance_to(&swap, 20);
    CHECK(swap_current_temperature(&swap) == 32.0f, "resfriamento pendente");
    CHECK(swap_page_out(&swap), "segunda escrita");
    CHECK(hw.swap_temperature == 44.0f, "resfriamento contado mais de uma vez");
    CHECK(swap.max_temperature == 44.0f, "pico nao registrado");
    CHECK(swap.waiting_ms == 0, "espera desnecessaria");

    swap_advance_to(&swap, 5);
    CHECK(swap.now_ms == 20, "relogio voltou no tempo");
    swap_advance_to(&swap, 1000);
    CHECK(swap_current_temperature(&swap) == BASE_SWAP_TEMP, "piso da temperatura");
}

static void test_inicio_em_zero_e_ciclos_da_mmu(void)
{
    hardware_state_t hw;
    swap_t swap;
    hardware_init(&hw);
    hw.total_cpu_cycles = 17; /* ciclos anteriores de acessos a MMU */
    swap_init(&swap, &hw, NULL);
    CHECK(swap_page_out(&swap), "escrita em t=0");
    CHECK(hw.swap_temperature == 40.0f, "t=0 usa incremento de 15 C");
    CHECK(swap.io_cpu_cycles == 1000 && hw.total_cpu_cycles == 1017,
          "ciclos de MMU confundidos com E/S");
    CHECK(swap_page_out(&swap), "segunda escrita em t=0");
    CHECK(swap_page_out(&swap), "terceira escrita com espera");
    CHECK(swap.now_ms > 0 && swap.max_temperature < MAX_SAFE_TEMP,
          "controle termico em t=0");
}

static void test_limite_de_timestamp(void)
{
    hardware_state_t hw;
    swap_t swap;
    hardware_init(&hw);
    swap_init(&swap, &hw, NULL);
    swap_advance_to(&swap, UINT32_MAX);
    CHECK(swap_page_out(&swap) && swap_page_out(&swap), "escritas no limite de tempo");
    CHECK(!swap_page_out(&swap), "espera com overflow deveria ser recusada");
    CHECK(swap.now_ms == UINT32_MAX && swap.page_outs == 2 &&
          hw.total_cpu_cycles == 2000 && hw.swap_temperature == 52.0f,
          "falha alterou o estado ou voltou o timestamp para zero");
}

int main(void)
{
    test_rajada();
    test_resfriamento_unico();
    test_inicio_em_zero_e_ciclos_da_mmu();
    test_limite_de_timestamp();
    printf("Swap: %s (%d falhas)\n", failures ? "REPROVADO" : "APROVADO", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
