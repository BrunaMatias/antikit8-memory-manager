# Gerenciamento de Memória no Antikit-8

Trabalho prático da disciplina de Sistemas Operacionais

O simulador faz o papel do Sistema Operacional do chip embarcado Antikit-8. Ele processa um trace de acessos à memória, trata Page Faults com um algoritmo de substituição baseado em WSClock ou Aging e controla o aquecimento do dispositivo de Swap.

## Especificações do hardware

Os arquivos `hardware/hardware_specs.h` e `hardware/hardware_specs.c` foram fornecidos pelo professor e não são modificados.

| Característica | Valor |
|---|---|
| Espaço virtual por processo | 1 MB (1.048.576 bytes) |
| RAM física | 256 KB |
| Páginas com VPN par | 4 KB (4096 bytes) |
| Páginas com VPN ímpar | 8 KB (8192 bytes) |
| Fading do bit R | A MMU zera o bit R de uma página aleatória a cada 5 acessos globais |
| Swap térmico | +15 °C por Page Out no mesmo milissegundo virtual; resfria 0,5 °C por ms ocioso |
| Thermal Throttling | Acima de 65 °C, o custo de 1000 ciclos dobra a cada 5 °C adicionais |

## Estrutura do projeto

```
.
├── hardware/
│   ├── hardware_specs.h     especificação do hardware 
│   └── hardware_specs.c     motor da MMU e do Swap 
├── include/
│   ├── address.h            tradução de endereços virtuais
│   ├── memory.h             RAM física e tabelas de páginas
│   ├── replacement.h        WSClock adaptado e eventos para auditoria
│   └── swap.h               escalonamento térmico do Swap
├── src/
│   ├── address.c
│   ├── memory.c
│   ├── replacement.c
│   └── swap.c
├── tests/
│   ├── test_address.c       Teste 1 e validação contra a MMU
│   ├── test_memory.c        alocação, RAM cheia e fragmentação
│   ├── test_replacement.c   seleção de vítimas, integração e fading
│   ├── test_swap.c          temperatura, ciclos e limites do relógio
│   └── test_thermal.c       Teste 2 e comparação de estratégias
├── docs/
│   └── integrante2.md       funcionamento, resultados e integração
└── Makefile
```

## Como compilar e executar

### Requisitos

- Compilador C com suporte a C99 (gcc)
- GNU make

**Windows:** instale o [MSYS2](https://www.msys2.org) e, no terminal **MSYS2 UCRT64**, execute:

```bash
pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-gcc make
```

Para usar os comandos no terminal do VS Code ou no PowerShell, adicione `C:\msys64\ucrt64\bin` e `C:\msys64\usr\bin` ao PATH do Windows.

**Linux:** `sudo apt install build-essential`

### Comandos

| Comando | Ação |
|---|---|
| `make test` | Compila e executa os testes |
| `make` | Compila os módulos em `build/libantikit8.a`; quando existir `src/main.c`, compila `build/simulador` |
| `make thermal` | Executa o estresse térmico e salva `build/thermal_results.csv` |
| `make clean` | Remove a pasta `build/` |

O programa principal e o parser são responsabilidade do integrante 3 e ainda não estão presentes nesta versão. Os módulos dos integrantes 1 e 2 já podem ser compilados e testados. Depois que o grupo adicionar `src/main.c`, coloque o arquivo `memory_trace.txt` na raiz do projeto e rode:

```bash
./build/simulador
```

### Saída esperada de `make test`

```
== Teste 1: limites da paginacao assimetrica ==
[MMU] VA   12287 -> VPN   1 | Offset 8191 | Pagina de 8 KB (impar)
[MMU] VA   12288 -> VPN   2 | Offset    0 | Pagina de 4 KB (par)
[MMU] VA 1048575 -> VPN 170 | Offset 4095 | Pagina de 4 KB (par)
[MMU] VA 1048576 -> INVALIDO (limite de 1048576 bytes)
Ultima VPN valida: 170

Varredura exaustiva de 1048576 enderecos concluida.
Limites de 171 paginas confirmados pela MMU.

APROVADO (0 falhas)
RAM de 256 KB preenchida com 64 quadros de 4 KB.
Fragmentacao interna com 1 KB usado: par 75.0% | impar 87.5%

APROVADO (0 falhas)
```

## Tradução de endereços assimétrica

Uma página par (4 KB) seguida de uma ímpar (8 KB) forma um par de 12 KB. A VPN e o offset são obtidos em tempo constante:

```
par       = endereço / 12288
resto     = endereço % 12288
VPN       = 2 × par + (resto ≥ 4096 ? 1 : 0)
offset    = resto ≥ 4096 ? resto − 4096 : resto
```

1 MB contém 85 pares completos (1.044.480 bytes) e uma página par final de 4 KB. O sistema tem, portanto, **171 páginas (VPN 0 a 170)**, e o último byte do espaço virtual fica dentro da VPN 170, sem estouro.

| Endereço | VPN | Offset | Observação |
|---|---|---|---|
| 12287 | 1 | 8191 | Último byte da página de 8 KB |
| 12288 | 2 | 0 | Primeiro byte da página de 4 KB |
| 1048575 | 170 | 4095 | Último byte de 1 MB |
| 1048576 | — | — | Rejeitado como inválido |

Endereços fora de 1 MB são recusados antes de chegar à MMU, para não serem contados como Page Fault.

## Organização da RAM física

A RAM de 256 KB é dividida em **64 quadros de 4 KB**:

- Uma página par ocupa 1 quadro.
- Uma página ímpar ocupa 2 quadros consecutivos, começando em um quadro de índice par.
- O campo `frame_number` da PTE guarda o quadro inicial, e o endereço físico é `frame_number × 4096 + offset`.
- Ao alocar uma página par, o sistema dá preferência a quadros cujo vizinho já está ocupado, para manter pares livres disponíveis para as páginas ímpares.
- Cada processo tem sua própria tabela com 171 entradas, criada no primeiro acesso do PID e liberada ao final da execução.

### Fragmentação interna

Cada página registra o maior offset acessado, o que mostra quantos bytes estão realmente em uso. O restante da página é desperdício interno:

| Página | Tamanho | Bytes usados | Desperdício |
|---|---|---|---|
| Par | 4 KB | 1 KB | 75,0% |
| Ímpar | 8 KB | 1 KB | 87,5% |

## Interface dos módulos

| Função | Descrição |
|---|---|
| `addr_translate` | Decompõe um endereço virtual em VPN, offset e tamanho de página |
| `addr_physical` | Calcula o endereço físico a partir da PTE |
| `memory_get_process` | Retorna a tabela de páginas do processo, criando-a se necessário |
| `memory_access` | Traduz e consulta a MMU, retornando `ACCESS_HIT`, `ACCESS_FAULT` ou `ACCESS_INVALID` |
| `memory_load_page` | Carrega a página em quadros livres ou retorna `NO_FRAME` se a RAM estiver cheia |
| `memory_unmap_page` | Remove a página da RAM e informa se ela estava suja (exige Page Out) |
| `memory_would_fit` | Indica se despejar uma vítima abre espaço para a página desejada |
| `memory_frame_usage` | Informa bytes usados e desperdiçados em um quadro |
| `memory_check_consistency` | Verifica a correspondência entre PTEs e quadros físicos |
| `replacement_init` | Inicializa o ponteiro do WSClock e a janela de idade `tau` |
| `replacement_handle_fault` | Seleciona vítimas, grava as sujas e conclui o acesso que falhou |
| `replacement_set_observer` | Registra o callback do auditor para alterações feitas pelo WSClock |
| `swap_advance_to` | Avança o relógio efetivo, preservando esperas virtuais anteriores |
| `swap_page_out` | Aciona o hardware e espera em tempo virtual quando necessário |
| `swap_current_temperature` | Consulta a temperatura considerando o resfriamento pendente |

## WSClock e controle térmico — integrante 2

O WSClock usa os bits `referenced` e `modified` e o campo `last_access_t` da própria tabela usada pela MMU. A janela de idade padrão é de 50 ms, configurável por `replacement_init`. A busca circular dá segunda chance às páginas referenciadas e prefere páginas limpas para evitar E/S. Se uma página de 8 KB exigir duas vítimas de 4 KB, escolhe um par alinhado e grava cada vítima suja separadamente.

Quando uma escrita ultrapassaria a meta de 64 °C, o SO adia a operação no relógio virtual. O hardware original aplica o resfriamento e cobra os ciclos de E/S; seus arquivos permanecem intactos. O tempo de espera é registrado separadamente, sem inventar uma frequência de CPU para convertê-lo em ciclos.

O comando `make thermal` enche os 256 KB de RAM e provoca dez novas faltas com timestamp de entrada 200. Compara uma referência ineficiente com o WSClock em rajadas de leitura e de escrita, gerando os dados para o relatório. A implementação, as limitações da estratégia e o trecho de integração com o futuro `main` estão em [docs/integrante2.md](docs/integrante2.md).

## Divisão do trabalho

| Integrante | Papel | Responsabilidades |
|---|---|---|
| 1 | Arquiteto da MMU | Tradução de endereços virtuais (VPN e offset assimétricos) e estruturas de dados da RAM |
| 2 | Engenheiro de Algoritmos | Algoritmo de substituição (WSClock/Aging) e controle térmico do Swap |
| 3 | Auditor e Analista de Métricas | Parser do trace, auditoria por checksum, mapa ASCII de fragmentação e estatísticas finais |

## Testes

| Teste | Objetivo | Onde |
|---|---|---|
| 1 | Validação da tradução assimétrica nos limites das páginas | `tests/test_address.c` |
| 2 | Estresse térmico do Swap com RAM cheia no timestamp 200 | `tests/test_thermal.c` e `make thermal` |
| 3 | Auditoria do checksum após o fading do bit R | Log do simulador |
| 4 | Mapa ASCII de fragmentação a cada 10 linhas do trace | Log do simulador |

`tests/test_address.c` também compara a tradução com uma soma acumulada em todos os 1.048.576 endereços e confirma, para cada limite de página, que `mmu_access_memory()` reconhece a mesma VPN. `tests/test_memory.c` cobre alocação, despejo, preenchimento total da RAM, fragmentação interna e gerenciamento de processos.

`tests/test_replacement.c` cobre páginas limpas e sujas, segunda chance, fading, pares alinhados, erros e 5.000 acessos entre cinco processos, verificando a consistência da RAM após cada operação. `tests/test_swap.c` verifica o pico térmico, o resfriamento sem duplicação, a separação dos ciclos de MMU/E/S e o limite de timestamps.

## Saída do simulador

Ao fim do processamento do trace, o simulador exibe:

- Total de acessos à memória
- Total de Page Faults (por processo e global)
- Temperatura máxima alcançada pelo Swap
- Ciclos de clock gastos em operações de E/S
- Porcentagem média de fragmentação interna da RAM
