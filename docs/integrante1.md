# Integrante 1: Arquiteto da MMU

Tradução de endereços virtuais com páginas assimétricas e gerenciamento das estruturas de dados da RAM física.

## Entrega desta parte

| Arquivo | Responsabilidade |
|---|---|
| `include/address.h` e `src/address.c` | Cálculo de VPN e offset, endereço físico e limites do espaço virtual |
| `include/memory.h` e `src/memory.c` | Quadros da RAM, tabelas de páginas por processo, alocação, despejo e fragmentação |
| `tests/test_address.c` | Teste 1 do enunciado, varredura exaustiva e validação contra a MMU |
| `tests/test_memory.c` | Alocação, RAM cheia, fragmentação interna e gerenciamento de processos |

## Tradução assimétrica

Páginas pares têm 4 KB e ímpares têm 8 KB. Cada par formado por uma página par e a ímpar seguinte ocupa sempre **12 KB (12288 bytes)**, o que permite calcular a VPN em tempo constante, sem percorrer a tabela:

```
bloco  = endereço / 12288
resto  = endereço % 12288
VPN    = 2 × bloco + (resto ≥ 4096 ? 1 : 0)
offset = resto ≥ 4096 ? resto − 4096 : resto
```

1 MB contém 85 blocos completos (1.044.480 bytes) e uma página par final de 4 KB. O sistema tem **171 páginas, da VPN 0 à VPN 170**.

Endereços a partir de 1.048.576 são recusados antes de chegar à MMU, para não serem contados como Page Fault.

## Teste 1: limites das páginas

| Endereço virtual | VPN | Offset | Significado |
|---|---:|---:|---|
| 12287 | 1 | 8191 | Último byte da página de 8 KB |
| 12288 | 2 | 0 | Primeiro byte da página de 4 KB |
| 1048575 | 170 | 4095 | Último byte de 1 MB, sem estouro |
| 1048576 | inválido | inválido | Fora do espaço virtual |

Saída de `make test`:

```
[MMU] VA   12287 -> VPN   1 | Offset 8191 | Pagina de 8 KB (impar)
[MMU] VA   12288 -> VPN   2 | Offset    0 | Pagina de 4 KB (par)
[MMU] VA 1048575 -> VPN 170 | Offset 4095 | Pagina de 4 KB (par)
[MMU] VA 1048576 -> INVALIDO (limite de 1048576 bytes)
```

Além dos três endereços do enunciado, o cálculo foi comparado com uma soma acumulada em todos os 1.048.576 endereços. O início e o fim das 171 páginas também foram conferidos diretamente em `mmu_access_memory()`, do professor.

## Organização da RAM física

Os 256 KB foram divididos em **64 quadros de 4 KB**. O endereço físico é `frame_number × 4096 + offset`.

| Página | Tamanho | Quadros ocupados |
|---|---|---|
| Par | 4 KB | 1 quadro |
| Ímpar | 8 KB | 2 quadros vizinhos, começando em um quadro par |

- Páginas pares são colocadas de preferência ao lado de um quadro já ocupado, preservando pares livres para as páginas ímpares.
- Cada processo tem sua própria tabela de 171 entradas, criada no primeiro acesso do PID e liberada por `memory_destroy`.
- `memory_check_consistency` confirma que cada PTE presente aponta para o quadro certo e que cada quadro ocupado pertence a uma PTE presente.

## Fragmentação interna

Cada página registra o maior offset acessado, o que mostra quantos bytes estão realmente em uso. O restante é desperdício interno.

| Página | Tamanho | Bytes usados | Desperdício |
|---|---:|---:|---:|
| Par | 4 KB | 1 KB | 75,0% |
| Ímpar | 8 KB | 1 KB | 87,5% |

Com o mesmo uso real, a página de 8 KB desperdiça mais memória, que é o efeito pedido no Teste 4.

## Interface usada pelos outros módulos

| Função | Uso |
|---|---|
| `memory_access` | Traduz e consulta a MMU, retornando `ACCESS_HIT`, `ACCESS_FAULT` ou `ACCESS_INVALID` |
| `memory_load_page` | Carrega a página em quadros livres ou retorna `NO_FRAME` quando a RAM está cheia |
| `memory_unmap_page` | Libera a página e informa se ela estava suja, exigindo Page Out |
| `memory_would_fit` | Indica se despejar uma vítima abre espaço para a página desejada |
| `memory_frame_usage` | Bytes usados e desperdiçados por quadro, para o mapa ASCII |

## Verificações realizadas

- Compilação C99 com `-Wall -Wextra -pedantic`, sem avisos.
- Teste 1 e varredura de todos os endereços do espaço virtual, sem divergências.
- Preenchimento total da RAM, despejo e abertura de espaço para página de 8 KB.
- Criação de 50 processos e liberação completa da memória ao final.
