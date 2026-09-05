/*
 * Agente Financeiro - controle de gastos para reeducacao financeira.
 * Registra gastos em um arquivo CSV e gera resumos por categoria e por mes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ARQUIVO_GASTOS "gastos.csv"
#define ARQUIVO_ORCAMENTO "orcamento.txt"
#define MAX_LINHA 256
#define MAX_GRUPOS 50

typedef struct {
    char data[11];
    char categoria[30];
    char descricao[100];
    double valor;
} Gasto;

int carregarGastos(Gasto **lista);
void imprimirGastos(Gasto *lista, int n);
void adicionarGasto(void);
void listarGastos(void);
void resumoPorCategoria(void);
void resumoPorMes(void);
void removerGasto(void);
void definirOrcamento(void);
void verificarOrcamento(void);
void limparBufferEntrada(void);

int main(void) {
    int opcao;

    do {
        printf("=========================================\n");
        printf(" AGENTE FINANCEIRO - Reeducacao Financeira\n");
        printf("=========================================\n");
        printf("1. Registrar gasto\n");
        printf("2. Listar gastos\n");
        printf("3. Resumo por categoria\n");
        printf("4. Resumo por mes\n");
        printf("5. Remover gasto\n");
        printf("6. Definir orcamento mensal\n");
        printf("7. Ver situacao do orcamento\n");
        printf("0. Sair\n");
        printf("Escolha uma opcao: ");

        if (scanf("%d", &opcao) != 1) {
            limparBufferEntrada();
            opcao = -1;
        }

        switch (opcao) {
            case 1: adicionarGasto(); break;
            case 2: listarGastos(); break;
            case 3: resumoPorCategoria(); break;
            case 4: resumoPorMes(); break;
            case 5: removerGasto(); break;
            case 6: definirOrcamento(); break;
            case 7: verificarOrcamento(); break;
            case 0: printf("Ate logo!\n"); break;
            default: printf("Opcao invalida.\n\n");
        }
    } while (opcao != 0);

    return 0;
}

void limparBufferEntrada(void) {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

int carregarGastos(Gasto **lista) {
    FILE *f = fopen(ARQUIVO_GASTOS, "r");
    if (!f) {
        *lista = NULL;
        return 0;
    }

    int capacidade = 16, total = 0;
    Gasto *arr = malloc(capacidade * sizeof(Gasto));
    char linha[MAX_LINHA];

    while (fgets(linha, sizeof(linha), f)) {
        linha[strcspn(linha, "\n")] = '\0';
        if (strlen(linha) == 0) continue;

        Gasto g;
        if (sscanf(linha, "%10[^;];%29[^;];%99[^;];%lf",
                   g.data, g.categoria, g.descricao, &g.valor) == 4) {
            if (total == capacidade) {
                capacidade *= 2;
                arr = realloc(arr, capacidade * sizeof(Gasto));
            }
            arr[total++] = g;
        }
    }

    fclose(f);
    *lista = arr;
    return total;
}

void adicionarGasto(void) {
    Gasto g;
    limparBufferEntrada();

    printf("Data (dd/mm/aaaa): ");
    scanf("%10[^\n]", g.data);
    limparBufferEntrada();

    printf("Categoria (ex: Alimentacao, Transporte, Lazer, Moradia, Saude, Outros): ");
    scanf("%29[^\n]", g.categoria);
    limparBufferEntrada();

    printf("Descricao: ");
    scanf("%99[^\n]", g.descricao);
    limparBufferEntrada();

    printf("Valor gasto (R$): ");
    while (scanf("%lf", &g.valor) != 1 || g.valor < 0) {
        printf("Valor invalido. Digite novamente: ");
        limparBufferEntrada();
    }

    FILE *f = fopen(ARQUIVO_GASTOS, "a");
    if (!f) {
        printf("Erro ao abrir arquivo de gastos.\n\n");
        return;
    }
    fprintf(f, "%s;%s;%s;%.2f\n", g.data, g.categoria, g.descricao, g.valor);
    fclose(f);
    printf("Gasto registrado!\n\n");
}

void imprimirGastos(Gasto *lista, int n) {
    double total = 0;
    printf("\n%-4s %-12s %-15s %-30s %10s\n", "#", "Data", "Categoria", "Descricao", "Valor");
    printf("---------------------------------------------------------------------------\n");
    for (int i = 0; i < n; i++) {
        printf("%-4d %-12s %-15s %-30s %10.2f\n",
               i + 1, lista[i].data, lista[i].categoria, lista[i].descricao, lista[i].valor);
        total += lista[i].valor;
    }
    printf("---------------------------------------------------------------------------\n");
    printf("Total gasto: R$ %.2f\n\n", total);
}

void listarGastos(void) {
    Gasto *lista;
    int n = carregarGastos(&lista);
    if (n == 0) {
        printf("Nenhum gasto registrado ainda.\n\n");
        return;
    }
    imprimirGastos(lista, n);
    free(lista);
}

void resumoPorCategoria(void) {
    Gasto *lista;
    int n = carregarGastos(&lista);
    if (n == 0) {
        printf("Nenhum gasto registrado ainda.\n\n");
        return;
    }

    char categorias[MAX_GRUPOS][30];
    double totais[MAX_GRUPOS];
    int numCategorias = 0;
    double totalGeral = 0;

    for (int i = 0; i < n; i++) {
        totalGeral += lista[i].valor;
        int achou = 0;
        for (int j = 0; j < numCategorias; j++) {
            if (strcmp(categorias[j], lista[i].categoria) == 0) {
                totais[j] += lista[i].valor;
                achou = 1;
                break;
            }
        }
        if (!achou && numCategorias < MAX_GRUPOS) {
            strcpy(categorias[numCategorias], lista[i].categoria);
            totais[numCategorias] = lista[i].valor;
            numCategorias++;
        }
    }

    for (int i = 0; i < numCategorias - 1; i++) {
        for (int j = 0; j < numCategorias - 1 - i; j++) {
            if (totais[j] < totais[j + 1]) {
                double tt = totais[j]; totais[j] = totais[j + 1]; totais[j + 1] = tt;
                char tc[30];
                strcpy(tc, categorias[j]);
                strcpy(categorias[j], categorias[j + 1]);
                strcpy(categorias[j + 1], tc);
            }
        }
    }

    printf("\n=== Resumo por categoria ===\n");
    for (int i = 0; i < numCategorias; i++) {
        double pct = (totais[i] / totalGeral) * 100.0;
        printf("%-15s R$ %8.2f  (%5.1f%%)\n", categorias[i], totais[i], pct);
    }
    printf("-----------------------------\n");
    printf("Total geral: R$ %.2f\n\n", totalGeral);

    if (numCategorias > 0) {
        double pctMaior = (totais[0] / totalGeral) * 100.0;
        printf("Dica: sua maior categoria de gasto e \"%s\", representando %.1f%% do total.\n",
               categorias[0], pctMaior);
        if (pctMaior > 40.0) {
            printf("Isso e mais de 40%% dos seus gastos concentrados em um unico grupo. Vale revisar se da pra reduzir.\n");
        }
    }
    printf("\n");
    free(lista);
}

void resumoPorMes(void) {
    Gasto *lista;
    int n = carregarGastos(&lista);
    if (n == 0) {
        printf("Nenhum gasto registrado ainda.\n\n");
        return;
    }

    char meses[MAX_GRUPOS][8];
    double totais[MAX_GRUPOS];
    int numMeses = 0;

    for (int i = 0; i < n; i++) {
        char chave[8] = "00/0000";
        if (strlen(lista[i].data) >= 10) {
            strncpy(chave, lista[i].data + 3, 7);
            chave[7] = '\0';
        }

        int achou = 0;
        for (int j = 0; j < numMeses; j++) {
            if (strcmp(meses[j], chave) == 0) {
                totais[j] += lista[i].valor;
                achou = 1;
                break;
            }
        }
        if (!achou && numMeses < MAX_GRUPOS) {
            strcpy(meses[numMeses], chave);
            totais[numMeses] = lista[i].valor;
            numMeses++;
        }
    }

    printf("\n=== Resumo por mes ===\n");
    for (int i = 0; i < numMeses; i++) {
        printf("%-8s R$ %8.2f\n", meses[i], totais[i]);
    }
    printf("\n");
    free(lista);
}

void removerGasto(void) {
    Gasto *lista;
    int n = carregarGastos(&lista);
    if (n == 0) {
        printf("Nenhum gasto registrado ainda.\n\n");
        return;
    }

    imprimirGastos(lista, n);
    printf("Digite o numero do gasto a remover (0 para cancelar): ");
    int idx;
    if (scanf("%d", &idx) != 1) {
        limparBufferEntrada();
        idx = 0;
    }

    if (idx < 1 || idx > n) {
        printf("Operacao cancelada.\n\n");
        free(lista);
        return;
    }

    FILE *f = fopen(ARQUIVO_GASTOS, "w");
    if (!f) {
        printf("Erro ao abrir arquivo de gastos.\n\n");
        free(lista);
        return;
    }
    for (int i = 0; i < n; i++) {
        if (i == idx - 1) continue;
        fprintf(f, "%s;%s;%s;%.2f\n", lista[i].data, lista[i].categoria, lista[i].descricao, lista[i].valor);
    }
    fclose(f);
    printf("Gasto removido.\n\n");
    free(lista);
}

void definirOrcamento(void) {
    double valor;
    printf("Digite o orcamento mensal (R$): ");
    while (scanf("%lf", &valor) != 1 || valor < 0) {
        printf("Valor invalido. Tente novamente: ");
        limparBufferEntrada();
    }

    FILE *f = fopen(ARQUIVO_ORCAMENTO, "w");
    if (!f) {
        printf("Erro ao salvar orcamento.\n\n");
        return;
    }
    fprintf(f, "%.2f\n", valor);
    fclose(f);
    printf("Orcamento definido!\n\n");
}

void verificarOrcamento(void) {
    FILE *fo = fopen(ARQUIVO_ORCAMENTO, "r");
    if (!fo) {
        printf("Nenhum orcamento definido ainda. Use a opcao 6 para definir um.\n\n");
        return;
    }
    double orcamento = 0;
    fscanf(fo, "%lf", &orcamento);
    fclose(fo);

    time_t agora = time(NULL);
    struct tm *tmAgora = localtime(&agora);
    char mesAtual[32];
    snprintf(mesAtual, sizeof(mesAtual), "%02d/%04d", tmAgora->tm_mon + 1, tmAgora->tm_year + 1900);

    Gasto *lista;
    int n = carregarGastos(&lista);
    double totalMes = 0;
    for (int i = 0; i < n; i++) {
        if (strlen(lista[i].data) >= 10) {
            char chave[8];
            strncpy(chave, lista[i].data + 3, 7);
            chave[7] = '\0';
            if (strcmp(chave, mesAtual) == 0) totalMes += lista[i].valor;
        }
    }
    free(lista);

    printf("\n=== Orcamento do mes %s ===\n", mesAtual);
    printf("Orcamento definido: R$ %.2f\n", orcamento);
    printf("Gasto ate agora:    R$ %.2f\n", totalMes);

    double restante = orcamento - totalMes;
    if (restante >= 0) {
        printf("Voce ainda tem R$ %.2f disponivel neste mes.\n\n", restante);
    } else {
        printf("Atencao: voce ultrapassou o orcamento em R$ %.2f!\n\n", -restante);
    }
}
