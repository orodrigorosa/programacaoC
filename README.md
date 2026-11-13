# programacaoC
Estudo de linguagem C (Estácio).

## vendas — controle de vendas com autenticação, cancelamento autorizado e auditoria

Protótipo em C + SQLite que impede que alguém cancele a venda de outro
funcionário e a refaça em seu próprio nome sem deixar rastro.

| Requisito | Como é atendido |
|---|---|
| Login individual | usuário + PIN (salt + SHA-256 alongado) |
| Venda registra o vendedor | `vendedor_id` gravado no login; imutável (trigger) |
| Permissão para cancelar | papéis `vendedor`, `supervisor`, `dono`; permissão `venda.cancelar` |
| Autorização por PIN do supervisor | `cancel` aceita um supervisor que digita o próprio PIN |
| Motivo obrigatório | mínimo de 5 caracteres |
| Nada é apagado | `DELETE` em vendas bloqueado; cancelamento só muda o status |
| Log de auditoria | tabela só de inserção, encadeada por hash; `verify` detecta adulteração |
| Venda "roubada" | `resell` liga à original; venda igual por outra pessoa em até 1 h gera ALERTA |

### Uso

```bash
make            # compila
make test       # roda o cenário de teste completo
./vendas init
printf '1234\n1234\n' | ./vendas user-add - ana dono          # 1º usuário (dono)
printf '1234\n1111\n1111\n' | ./vendas user-add ana bia vendedor
printf '1111\n' | ./vendas sell bia 150.00
printf '2222\n5555\n' | ./vendas cancel carla 1 "cliente desistiu" saulo
printf '1234\n' | ./vendas report ana
printf '1234\n' | ./vendas audit ana
printf '1234\n' | ./vendas verify ana
```

No terminal os PINs são pedidos sem eco; os `printf` acima só servem para automação/teste.

### Limitações (protótipo)
- Quem tem acesso ao arquivo `.db` pode remover os triggers; o `verify` detecta a adulteração
  da auditoria, mas para valer guarde o último hash fora do banco e use um SGBD em servidor.
- Use bcrypt/argon2 para PINs em produção e bloqueio após tentativas erradas.
- Sem itens de venda, estoque nem emissão fiscal (NFC-e/NF-e).
