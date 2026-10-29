#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "replacement.h"

static int failures;

#define CHECK(cond, message) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FALHA: %s\n", message); } \
} while (0)

static void run_scenario(const char *name, bool inefficient, bool initial_dirty,
                          bool incoming_write, FILE *csv)
{
    memory_t mem;
    hardware_state_t hw;
    swap_t swap;
    replacement_t clock;
    memory_init(&mem);
    hardware_init(&hw);
    swap_init(&swap, &hw, NULL);
    replacement_init(&clock, WSCLOCK_DEFAULT_TAU_MS, NULL);
    process_t *proc = memory_get_process(&mem, 1);
    if (proc == NULL) {
        fprintf(stderr, "Sem memoria para o teste termico\n");
        exit(EXIT_FAILURE);
    }

    /* 32 paginas de 8 KB ocupam todos os 256 KB. Cada carga passa pela MMU. */
    for (uint32_t i = 0; i < NUM_FRAMES / 2; i++) {
        translation_t t;
        CHECK(memory_access(&mem, &hw, proc, addr_page_base(2 * i + 1), initial_dirty,
                              i, &t) == ACCESS_FAULT, "preenchimento deveria gerar falta");
        CHECK(replacement_handle_fault(&clock, &mem, &swap, proc, &t,
                                         initial_dirty, i, NULL) == REPLACEMENT_OK,
              "falha no preenchimento da RAM");
    }
    CHECK(mem.free_frames == 0, "RAM nao ficou cheia");

    float max_temperature = BASE_SWAP_TEMP;
    uint64_t io_cycles = 0;
    uint64_t page_outs = 0;
    printf("\n%s\n", name);
    printf("pedido | trace(ms) | efetivo(ms) | Page Outs | temperatura(C) | E/S(ciclos)\n");
    for (uint32_t i = 0; i < 10; i++) {
        uint32_t vpn = 65 + 2 * i;
        translation_t t;
        swap_advance_to(&swap, 200);
        CHECK(memory_access(&mem, &hw, proc, addr_page_base(vpn), incoming_write,
                              swap.now_ms, &t) == ACCESS_FAULT, "nova pagina nao gerou falta");
        if (inefficient) {
            /* Referencia propositalmente ruim: sempre escolhe outra pagina suja
             * original, sem considerar R, idade, paginas limpas ou temperatura. */
            uint32_t victim_vpn = 2 * i + 1;
            uint64_t before = hw.total_cpu_cycles;
            hardware_trigger_page_out(&hw, 200);
            io_cycles += hw.total_cpu_cycles - before;
            page_outs++;
            CHECK(memory_unmap_page(&mem, proc, victim_vpn), "vitima de referencia nao suja");
            CHECK(memory_load_page(&mem, proc, &t, incoming_write, 200) != NO_FRAME,
                  "carga no cenario de referencia");
        } else {
            CHECK(replacement_handle_fault(&clock, &mem, &swap, proc, &t,
                                             incoming_write, 200, NULL) == REPLACEMENT_OK,
                  "falta nao resolvida no cenario seguro");
            io_cycles = swap.io_cpu_cycles;
            page_outs = swap.page_outs;
            CHECK(hw.swap_temperature < MAX_SAFE_TEMP, "WSClock superaqueceu o Swap");
        }
        if (hw.swap_temperature > max_temperature) {
            max_temperature = hw.swap_temperature;
        }
        printf("%6u | %9u | %11u | %9" PRIu64 " | %14.1f | %11" PRIu64 "\n",
               i + 1, 200u, swap.now_ms, page_outs, (double)hw.swap_temperature, io_cycles);
        if (csv != NULL) {
            fprintf(csv, "%s,%u,200,%u,%u,%" PRIu64 ",%" PRIu64 ",%.1f,%.1f,%" PRIu64 "\n",
                    name, i + 1, swap.now_ms, i + 1, page_outs, io_cycles,
                    (double)hw.swap_temperature, (double)max_temperature, swap.waiting_ms);
        }
        CHECK(memory_check_consistency(&mem), "RAM inconsistente no teste termico");
    }
    CHECK(hw.global_access_count == 42, "deveriam ser 32 acessos iniciais + 10 faltas");
    if (inefficient) {
        CHECK(max_temperature > MAX_SAFE_TEMP && io_cycles > 16000,
              "referencia nao demonstrou throttling");
    } else if (!initial_dirty) {
        CHECK(page_outs == 0 && io_cycles == 0, "paginas limpas geraram E/S");
    } else if (incoming_write) {
        CHECK(page_outs == 10 && io_cycles == 10000 && swap.waiting_ms > 0,
              "rajada suja nao foi escalonada sem throttling");
    } else {
        CHECK(page_outs == 1 && io_cycles == 1000,
              "WSClock nao aproveitou pagina limpa na rajada de leituras");
    }
    printf("Resumo: 10 Page Faults na rajada | pico %.1f C | E/S %" PRIu64
           " ciclos | espera virtual %" PRIu64 " ms\n",
           (double)max_temperature, io_cycles, swap.waiting_ms);
    memory_destroy(&mem);
}

int main(int argc, char **argv)
{
    FILE *csv = NULL;
    if (argc > 2) {
        fprintf(stderr, "Uso: %s [arquivo.csv]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        csv = fopen(argv[1], "w");
        if (csv == NULL) {
            perror(argv[1]);
            return EXIT_FAILURE;
        }
        fprintf(csv, "cenario,pedido,trace_ms,efetivo_ms,page_faults_rajada,page_outs,"
                     "ciclos_es,temperatura_c,pico_c,espera_ms\n");
    }
    srand(42);
    run_scenario("Referencia ineficiente - leituras", true, true, false, csv);
    run_scenario("WSClock - vitimas limpas", false, false, false, csv);
    run_scenario("WSClock - leituras apos RAM suja", false, true, false, csv);
    run_scenario("WSClock - estresse com escritas", false, true, true, csv);
    if (csv != NULL && fclose(csv) != 0) {
        perror("Erro ao salvar CSV");
        return EXIT_FAILURE;
    }
    printf("\nTeste termico: %s (%d falhas)\n", failures ? "REPROVADO" : "APROVADO", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
