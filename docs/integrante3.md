# Integrante 3: Auditor e Analista de Métricas

Implementação do programa principal, leitura do trace, auditoria das tabelas de páginas, visualização ASCII da RAM e consolidação dos resultados.

## Entrega desta parte

| Arquivo | Responsabilidade |
|---|---|
| `src/main.c` | Parser, fluxo de acessos e Page Faults, auditoria, mapa de RAM e estatísticas |
| `README.md` | Formato do trace e instruções para executar o simulador |
| `docs/integrante3.md` | Descrição desta parte, critérios de medição e exemplos de uso |

O `main` integra as interfaces dos integrantes 1 e 2 sem substituir a MMU, o gerenciamento de RAM, o WSClock ou o controle térmico.

## Formato do trace

O simulador lê um registro por linha, com quatro campos separados por espaços ou tabulações:

```text
timestamp_ms pid endereco R|W
```

- `timestamp_ms` e `pid` são inteiros decimais não negativos dentro do limite de 32 bits.
- `endereco` é decimal ou hexadecimal iniciado por `0x`/`0X`.
- `R` representa leitura e `W` escrita; letras minúsculas também são aceitas.
- Linhas vazias e linhas cujo primeiro caractere não branco é `#` são ignoradas. Um comentário também pode vir após os campos.
- Registros malformados encerram a execução com indicação do arquivo e da linha.
- Endereços fora do espaço virtual são informados e ignorados pela MMU; os demais registros continuam a ser processados.

Exemplo de `memory_trace.txt`:

```text
# timestamp PID endereço operação
0 1 12287 R
1 1 0x3000 W
200 2 1048575 R
```

Execute `make` e, na raiz do projeto, `./build/simulador` para usar o arquivo padrão. Para outro arquivo, use `./build/simulador caminho/do/trace.txt`.

## Tratamento dos acessos

Para cada registro, o programa obtém ou cria a tabela do processo, avança o relógio do Swap e chama `memory_access` uma única vez. Um hit não requer ação adicional. Em um Page Fault, incrementa os contadores global e do PID e chama `replacement_handle_fault`, que completa a carga e o acesso. O `main` não repete o acesso à MMU após o tratamento, evitando duplicar a contagem global e o fading.

Uma falha na abertura/leitura do arquivo, na alocação de estruturas ou no tratamento de uma falta é reportada em `stderr` e encerra a execução com status de erro. O caminho padrão é `memory_trace.txt`; um argumento opcional substitui esse caminho.

## Auditoria

O checksum simples é a soma de `present` e `frame_number` de todas as entradas de todas as tabelas de páginas. Ele é calculado no início, após alterações observadas na MMU e em cada evento de alteração comunicado pelo callback do WSClock:

- Limpeza de R, remoção de vítima e mapeamento de página são observados via `replacement_set_observer`.
- Antes e depois de cada acesso à MMU, a tabela do processo acessado é comparada para registrar alterações diretas, incluindo o fading quando o bit R efetivamente muda.
- Uma mensagem de fading também é exibida em cada múltiplo de cinco acessos contabilizados pela MMU, indicando a execução do sorteio físico. O checksum obrigatório pode permanecer igual quando muda apenas R, pois só cobre presença e quadro.

O módulo não altera os bits auditados nem mantém cópias usadas para decidir substituições.

## Mapa e fragmentação interna

A cada dez registros de acesso processados, o terminal mostra 256 caracteres, um por unidade de 1 KB da RAM física. `U` indica bytes usados, `F` indica desperdício interno em um quadro alocado e `.` indica RAM livre. Assim, a ocupação de páginas de 4 KB e 8 KB e a sobra dentro de cada quadro ficam visíveis no mesmo mapa.

A fragmentação de um instantâneo é `bytes desperdiçados / bytes alocados` na RAM. A média final é a média aritmética desses percentuais ao fim de cada registro válido, desconsiderando instantâneos sem quadros ocupados. O cálculo usa `memory_frame_usage`, que considera como usados os bytes até o maior offset acessado em cada página.

## Estatísticas finais

O resumo apresenta:

- Total de registros de acesso válidos no trace e total de acessos contabilizados pela MMU. Os dois números diferem se houver endereço inválido.
- Page Faults global e por PID.
- Maior temperatura de Swap observada pelo controlador.
- Ciclos gastos em Page Out, sem incluir os ciclos base dos acessos MMU.
- Fragmentação interna média durante a execução e fragmentação no estado final.

## Verificações e demonstração

`make test` executa os testes existentes dos módulos de tradução, RAM, substituição e Swap. O simulador pode ser demonstrado com traces curtos contendo os limites da tradução assimétrica, sequências de Page Faults no mesmo timestamp, acessos múltiplos de cinco para observar o fading e endereços em páginas pares/ímpares com pouco uso.

Para a apresentação, mostre um registro de cada tipo, a linha de auditoria, um mapa após dez acessos e o resumo final. Compare uma leitura em uma página par com uma leitura em uma página ímpar limitada a 1 KB: a fragmentação interna da página ímpar deve ser maior.
