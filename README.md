# programacaoC
Estudod de linguagem c estácio

## Agente Financeiro

Programa em C para registrar seus gastos e entender para onde vai o seu dinheiro,
com foco em reeducação financeira.

### Como compilar e rodar

```bash
make
./financas
```

(ou, sem o Makefile: `gcc financas.c -o financas && ./financas`)

### Funcionalidades

1. **Registrar gasto** — data, categoria, descrição e valor.
2. **Listar gastos** — tabela com todos os gastos registrados e o total.
3. **Resumo por categoria** — total e percentual gasto em cada categoria
   (ex: Alimentação, Transporte, Lazer), com uma dica quando uma categoria
   concentra mais de 40% dos gastos.
4. **Resumo por mês** — total gasto em cada mês.
5. **Remover gasto** — corrige lançamentos errados.
6. **Definir orçamento mensal** — define um limite de gasto para o mês.
7. **Ver situação do orçamento** — compara o gasto do mês atual com o orçamento definido.

### Dados

Os gastos ficam salvos em `gastos.csv` (formato `data;categoria;descricao;valor`)
e o orçamento em `orcamento.txt`, ambos gerados automaticamente na primeira
execução. Esses arquivos contêm dados pessoais e por isso estão no `.gitignore`
— não são versionados.
