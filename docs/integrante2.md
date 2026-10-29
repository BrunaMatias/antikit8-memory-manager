# Integrante 2 — WSClock adaptado e Swap térmico

Implementação preparada sobre o commit `23176b03b0db0d4b9e1248b26ec5b40845909fa0` do repositório `BrunaMatias/antikit8-memory-manager`.

## Entrega desta parte

| Arquivo | Responsabilidade |
|---|---|
| `include/replacement.h` e `src/replacement.c` | WSClock adaptado, tratamento da falta e integração com a RAM |
| `include/swap.h` e `src/swap.c` | Relógio efetivo, escalonamento térmico e métricas de E/S |
| `tests/test_replacement.c` | Seleção de vítimas, tamanhos assimétricos, eventos de auditoria e integração |
| `tests/test_swap.c` | Resfriamento, temperatura, ciclos e limites do tempo virtual |
| `tests/test_thermal.c` | Demonstração reproduzível do Teste 2, com exportação CSV |
| `Makefile` | Compilação dos novos módulos e execução dos testes |

Os arquivos de hardware e a implementação do integrante 1 não foram alterados. O parser, o `main`, o cálculo do checksum, o mapa ASCII e as estatísticas globais/por processo continuam sendo a parte do integrante 3. O callback de auditoria é uma interface; ele não implementa o checksum.

## Como o WSClock funciona

O ponteiro circular percorre somente os quadros iniciais de cada página. Assim, uma página de 8 KB não aparece duas vezes como candidata. A janela de idade, chamada `tau`, começa em 50 ms e pode ser ajustada na inicialização.

Quando não há espaço para a página solicitada:

1. A primeira volta consulta a tabela real da MMU. Para cada página com `R=1`, limpa R e registra o instante da segunda chance em `last_access_t`.
2. A segunda volta considera a idade de cada página: `agora - last_access_t > tau` significa antiga. Uma página com R apagado pelo fading ainda pode ser recente pelo timestamp.
3. A preferência é: conjunto antigo e limpo; conjunto limpo recente; conjunto antigo com páginas sujas; conjunto recente com páginas sujas. Empates favorecem menos escritas, menos páginas recentes e menos vítimas; o ponteiro circular decide os empates restantes.
4. Cada vítima suja é gravada pelo controlador de Swap **antes** de ser removida. Vítimas limpas não geram Page Out.
5. `memory_unmap_page` libera as vítimas e `memory_load_page` conclui o acesso que falhou, respeitando leitura/escrita, offset e instante efetivo.

A busca termina após duas voltas de no máximo 64 quadros. Existe uma alternativa de seleção quando todas as páginas são recentes, evitando que o simulador entre em repetição infinita.

### Adaptações em relação ao WSClock clássico

O WSClock clássico pode iniciar a limpeza de páginas sujas e continuar a varredura enquanto a E/S ocorre. O motor fornecido oferece Page Out síncrono, sem fila de E/S assíncrona. Nesta implementação, a busca termina com um conjunto de vítimas e o controlador agenda as escritas necessárias antes de removê-las. A preferência por limpas recentes é uma adaptação explícita ao custo térmico do Antikit-8.

Essa escolha pode reduzir E/S e, ao mesmo tempo, aumentar novas faltas caso descarte páginas limpas que serão reutilizadas. Os resultados demonstram segurança térmica nos cenários testados; não demonstram que esta política minimiza faltas para qualquer carga.

### Páginas de 8 KB

Uma página ímpar precisa de dois quadros consecutivos e alinhados. Não basta haver dois quadros livres em posições separadas. O algoritmo avalia o par inteiro e pode remover:

- Uma página de 8 KB: um único Page Out, se estiver suja.
- Duas páginas de 4 KB: até dois Page Outs, um para cada vítima suja.
- Uma página de 4 KB cujo vizinho já está livre.

Isso evita despejos espalhados pela RAM e preserva a correspondência entre quadros físicos e PTEs.

## Controle do Swap sem alterar o hardware

O código do professor cobra 1.000 ciclos por Page Out antes de penalidades. Ele aquece 15 °C no mesmo milissegundo e, quando o timestamp muda, aplica resfriamento de 0,5 °C/ms seguido de aquecimento de 12 °C. A implementação usa esse comportamento real.

Antes de gravar, o controlador prevê a temperatura final. Se ela excederia **64 °C**, agenda a operação para o primeiro milissegundo seguro. A meta de 64 °C deixa uma margem de 1 °C em relação ao limite de 65 °C.

O relógio efetivo pode ficar à frente do timestamp de entrada. Por exemplo, uma requisição marcada como 200 ms pode precisar aguardar e terminar em 224 ms. As próximas requisições passam a usar o maior valor entre seu timestamp original e o relógio efetivo atual.

Não há `sleep` real. Também não se altera manualmente a temperatura nem se apagam bits M para evitar custos. O código chama `hardware_trigger_page_out`, que aplica fisicamente o resfriamento por meio de `hardware_cool_down`. Aplicar esse mesmo resfriamento novamente fora do motor contaria o intervalo duas vezes.

Por esse motivo, `hardware.swap_temperature` guarda a temperatura da última escrita. Use `swap_current_temperature(&swap)` para consultar a temperatura após um período ocioso; o resfriamento pendente será aplicado no estado de hardware durante a próxima escrita. Não chame `hardware_cool_down` por fora desse controlador.

As métricas de E/S usam a diferença de `hardware.total_cpu_cycles` antes e depois de cada Page Out, excluindo os ciclos de acesso à MMU. A espera fica em milissegundos, separada dos ciclos, pois o enunciado não informa uma frequência para conversão. O hardware não fornece uma operação de Page In com custo próprio; o módulo não inventa esse custo.

Se a espera necessária ultrapassaria `UINT32_MAX`, `swap_page_out` falha e o tratador retorna `REPLACEMENT_SWAP_ERROR` sem descartar a vítima cuja gravação falhou. Os erros devem ser tratados pelo `main`.

## Integração pelo integrante 3

Inicializar uma vez, depois de `hardware_init` e `memory_init`:

```c
#include "replacement.h"

swap_t swap;
replacement_t clock;
swap_init(&swap, &hw, stdout);
replacement_init(&clock, WSCLOCK_DEFAULT_TAU_MS, stdout);
```

Para cada linha válida do trace, depois de obter `proc` com `memory_get_process`:

```c
/* timestamp, va e is_write vieram do parser. Verificar proc != NULL. */
swap_advance_to(&swap, timestamp);

translation_t t;
access_result_t result = memory_access(&mem, &hw, proc, va, is_write,
                                        swap.now_ms, &t);

/* Auditor: observar tambem as mudancas feitas pela propria MMU aqui,
 * inclusive o fading. Os callbacks abaixo cobrem somente o tratador. */

if (result == ACCESS_FAULT) {
    /* Incrementar os contadores de Page Fault do processo e global. */
    uint32_t frame;
    replacement_result_t status = replacement_handle_fault(
        &clock, &mem, &swap, proc, &t, is_write, timestamp, &frame);
    if (status != REPLACEMENT_OK) {
        fprintf(stderr, "Nao foi possivel resolver a falta: %d\n", (int)status);
        /* Encerrar ou propagar o erro no programa principal. */
    }
} else if (result == ACCESS_INVALID) {
    fprintf(stderr, "Endereco virtual invalido: %u\n", va);
}
```

**Não repetir `memory_access` depois do tratamento:** `memory_load_page` já concluiu o acesso. Repetir duplicaria o contador global e anteciparia o fading.

### Auditoria

Registrar a função do auditor por `replacement_set_observer(&clock, callback, contexto)`. A assinatura do callback está em `include/replacement.h`. Ele recebe a RAM atual, o tipo de evento, o PID e a VPN, imediatamente após:

- Limpeza do bit R e atualização do tempo da segunda chance.
- Remoção de cada página da RAM.
- Mapeamento da página solicitada.

O callback deve apenas ler o estado. Pode calcular e imprimir o checksum sem criar uma segunda tabela usada para decidir as vítimas. As alterações da MMU durante `memory_access`, incluindo fading, precisam ser observadas separadamente pelo integrante 3.

O checksum exigido sobre `present` e `frame_number` não muda quando apenas R muda. Nesse caso, recalculá-lo pode produzir corretamente o mesmo valor. Para auditar também R, é possível acrescentar um checksum estendido, mantendo o checksum obrigatório.

O hardware sorteia uma VPN da tabela do processo acessado a cada cinco acessos e só limpa R se essa VPN estiver presente. Portanto, o módulo de auditoria deve registrar as alterações reais, sem presumir que o sorteio sempre encontra uma página residente ou que R necessariamente mudou de 1 para 0.

### Estatísticas disponíveis

| Campo | Significado |
|---|---|
| `hw.global_access_count` | Acessos contabilizados pela MMU |
| `hw.total_cpu_cycles` | Ciclos de MMU + E/S |
| `swap.io_cpu_cycles` | Apenas ciclos gastos em Page Out |
| `swap.max_temperature` | Maior temperatura desde a inicialização do controlador |
| `swap.page_outs` | Quantidade de páginas sujas gravadas |
| `swap.waiting_ms` | Espera térmica virtual acumulada |
| `swap.now_ms` | Instante efetivo após as esperas |
| `clock.evictions` | Total de páginas removidas |
| `clock.clean_evictions` / `clock.dirty_evictions` | Remoções limpas / sujas |

Faltas globais/por processo e média de fragmentação devem ser consolidadas pelo integrante 3.

## Compilação e testes

Na raiz do repositório:

```bash
make
make test
make thermal
```

Como ainda não existe `src/main.c`, `make` gera a biblioteca estática `build/libantikit8.a`. Assim que o grupo adicionar esse arquivo, o mesmo comando passará a gerar `build/simulador`. O teste térmico é executável independentemente do parser.

`make thermal` gera `build/thermal_results.csv`, com uma linha por requisição e colunas de temperatura, pico, ciclos acumulados de E/S, Page Outs e tempo efetivo. O integrante 3 pode usar esse CSV para gerar os gráficos do relatório. Os executáveis são compilados localmente; não há binários de Linux para copiar para Windows.

### Resultados obtidos

Todos os cenários começam com 32 páginas de 8 KB, preenchendo os 256 KB. Depois, dez páginas novas são acessadas consecutivamente com timestamp original 200. As três primeiras linhas da tabela usam leituras nas novas páginas. A última usa escritas para forçar dez vítimas sujas e exercitar a espera térmica.

| Cenário | Faltas na rajada | Page Outs | Pico | Ciclos de E/S | Espera virtual |
|---|---:|---:|---:|---:|---:|
| Referência ineficiente, RAM inicialmente suja | 10 | 10 | 172 °C | 4.793.492.000 | 0 ms |
| WSClock, vítimas inicialmente limpas | 10 | 0 | 25 °C | 0 | 0 ms |
| WSClock, leituras após RAM inicialmente suja | 10 | 1 | 37 °C | 1.000 | 0 ms |
| WSClock, estresse com escritas e RAM suja | 10 | 10 | 64 °C | 10.000 | 168 ms |

No cenário de leituras após RAM suja, o WSClock grava uma primeira vítima e, nas faltas seguintes, reaproveita a página recém-carregada que está limpa. Isso reduz a E/S nesta rajada, mas pode prejudicar uma carga que reutilize essas páginas; o teste não mede esse possível aumento de faltas futuras.

No estresse com escritas, os instantes efetivos dos dez Page Outs foram `200, 200, 201, 224, 248, 272, 296, 320, 344, 368` ms. O custo permaneceu em 1.000 ciclos por operação. O adiamento explícito explica a redução térmica; não houve alteração dos timestamps originais da carga de teste.

A referência ineficiente existe apenas no teste comparativo. Ela escolhe deliberadamente páginas sujas originais, mesmo quando há alternativas limpas; não é usada pelo tratador implementado.

### Verificações realizadas

- Testes originais de tradução, incluindo todos os 1.048.576 endereços, e de gerenciamento da RAM.
- Testes de preferência por limpas, segunda chance, idade após fading e avanço do ponteiro circular.
- Páginas de 4/8 KB, pares inicialmente ocupados e pares parcialmente livres.
- Preservação de página suja se a gravação não puder ser agendada.
- 5.000 acessos em cinco processos, verificando a consistência após cada operação e nos callbacks.
- Compilação C99 com `-Wall -Wextra -Werror -pedantic` e execução com AddressSanitizer/UndefinedBehaviorSanitizer, sem erros detectados.

O verificador de vazamentos do LeakSanitizer não pôde executar neste ambiente por restrição de acesso a `/proc`; a execução de AddressSanitizer/UBSan foi concluída com `detect_leaks=0`. Os novos módulos não fazem alocação dinâmica; as estruturas de processos são liberadas por `memory_destroy` nos testes.

## Incorporar ao Git do grupo

Copiar os novos arquivos e os dois arquivos atualizados (`Makefile` e `README.md`) para o clone local do grupo, preservando os caminhos. Executar os testes antes do commit. O autor do commit deve ser a própria pessoa responsável por esta parte.

```bash
git add include/replacement.h include/swap.h src/replacement.c src/swap.c
git add tests/test_replacement.c tests/test_swap.c tests/test_thermal.c
git add Makefile README.md docs/integrante2.md
git commit -m "Implementa WSClock adaptado e controle termico do Swap"
```

Se o repositório avançou desde o commit de base, conferir as diferenças antes de substituir `README.md` e `Makefile`. Os módulos novos usam somente a interface atual do integrante 1.

## Apoio para a apresentação de 1 a 2 minutos

Explicar o ponteiro circular, os bits R/M e a janela de idade; mostrar que uma vítima limpa evita E/S. Em seguida, executar `make thermal` e destacar a comparação entre a referência ineficiente e o cenário de escritas: 172 °C contra 64 °C. Explicar que o SO pagou 168 ms de espera virtual para manter as dez escritas em 10.000 ciclos de E/S. Por fim, mostrar que uma página de 8 KB pode exigir dois quadros e que nenhuma vítima suja é removida antes de ser gravada.
