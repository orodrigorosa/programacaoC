#!/usr/bin/env bash
# Cenário: a vendedora Carla tenta "roubar" a venda da vendedora Bia.
set -u
export VENDAS_DB="$(mktemp -u)/x.db"; mkdir -p "$(dirname "$VENDAS_DB")"
V=./vendas
# Cliente SQL mínimo (não depende do binário sqlite3)
sqlite3() { python3 -c "import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); c.executescript(sys.argv[2]); c.commit()" "$1" "$2"; }
fail=0
ok()   { echo "  ok   - $1"; }
bad()  { echo "  FALHA - $1"; fail=1; }
# espera_sucesso <desc> <comando...> ; espera_erro <desc> <comando...>
espera_sucesso() { local d="$1"; shift; if "$@" >/tmp/out.$$ 2>&1; then ok "$d"; else bad "$d"; cat /tmp/out.$$; fi; }
espera_erro()    { local d="$1"; shift; if "$@" >/tmp/out.$$ 2>&1; then bad "$d (deveria falhar)"; else ok "$d"; fi; }

echo "== Usuários =="
espera_sucesso "cria dono"        bash -c "printf '1234\n1234\n' | $V user-add - ana dono"
espera_sucesso "cria supervisor"  bash -c "printf '1234\n5555\n5555\n' | $V user-add ana saulo supervisor"
espera_sucesso "cria vendedora Bia"   bash -c "printf '1234\n1111\n1111\n' | $V user-add ana bia vendedor"
espera_sucesso "cria vendedora Carla" bash -c "printf '1234\n2222\n2222\n' | $V user-add ana carla vendedor"
espera_erro "vendedor não cria usuário" bash -c "printf '1111\n9999\n9999\n' | $V user-add bia mallory dono"

echo "== Venda e tentativa de cancelamento =="
espera_sucesso "Bia vende R\$ 150"  bash -c "printf '1111\n' | $V sell bia 150.00"
espera_erro "PIN errado é recusado" bash -c "printf '0000\n' | $V sell bia 10"
espera_erro "Carla não cancela sem supervisor" bash -c "printf '2222\n' | $V cancel carla 1 'cliente desistiu'"
espera_erro "motivo vazio é recusado" bash -c "printf '5555\n' | $V cancel saulo 1 'x'"
espera_erro "Carla com PIN de supervisor errado" bash -c "printf '2222\n0000\n' | $V cancel carla 1 'cliente desistiu' saulo"
espera_erro "Carla não pode se autorizar" bash -c "printf '2222\n2222\n' | $V cancel carla 1 'cliente desistiu' carla"
espera_erro "vendedor não é autorizador" bash -c "printf '2222\n1111\n' | $V cancel carla 1 'cliente desistiu' bia"
espera_sucesso "Carla cancela COM autorização do Saulo" bash -c "printf '2222\n5555\n' | $V cancel carla 1 'cliente desistiu' saulo"
espera_erro "não cancela duas vezes" bash -c "printf '5555\n' | $V cancel saulo 1 'cancelando de novo'"

echo "== Carla refaz a venda no nome dela =="
out=$(printf '2222\n' | $V sell carla 150.00 2>&1); echo "$out" | grep -q ALERTA && ok "venda igual por outra pessoa gera ALERTA" || bad "sem alerta ($out)"
out=$(printf '2222\n' | $V resell carla 1 150.00 2>&1); echo "$out" | grep -q OUTRO_VENDEDOR && ok "resell ligado à original gera ALERTA" || bad "sem alerta resell ($out)"

echo "== Relatório e auditoria =="
rel=$(printf '1234\n' | $V report ana 2>&1)
echo "$rel" | grep -q "cancelou=carla autorizou=saulo" && ok "relatório mostra vendedor, quem cancelou e quem autorizou" || bad "relatório"
echo "$rel" | grep -q "OUTRO_VENDEDOR" && ok "relatório lista os alertas" || bad "relatório alertas"
espera_erro "vendedor não vê auditoria" bash -c "printf '1111\n' | $V audit bia"
aud=$(printf '1234\n' | $V audit ana 100 2>&1)
for a in LOGIN_FALHA CANCELAMENTO_SEM_PERMISSAO VENDA_CANCELADA ALERTA_REVENDA ACESSO_NEGADO; do
  echo "$aud" | grep -q "$a" && ok "auditoria contém $a" || bad "auditoria sem $a"
done
espera_sucesso "cadeia de hashes íntegra" bash -c "printf '1234\n' | $V verify ana"

echo "== Tentativas de fraude no banco =="
espera_erro "UPDATE na auditoria bloqueado" sqlite3 "$VENDAS_DB" "UPDATE auditoria SET acao='X' WHERE id=1"
espera_erro "DELETE na auditoria bloqueado" sqlite3 "$VENDAS_DB" "DELETE FROM auditoria WHERE id=1"
espera_erro "DELETE de venda bloqueado"     sqlite3 "$VENDAS_DB" "DELETE FROM vendas WHERE id=1"
espera_erro "trocar vendedor bloqueado"     sqlite3 "$VENDAS_DB" "UPDATE vendas SET vendedor_id=3 WHERE id=2"
# Quem tem acesso ao arquivo pode derrubar o gatilho: a verificação de hash pega.
sqlite3 "$VENDAS_DB" "DROP TRIGGER aud_no_update; UPDATE auditoria SET detalhe='adulterado' WHERE id=6"
printf '1234\n' | $V verify ana 2>&1 | grep -q ADULTERA && ok "verify detecta adulteração direta no arquivo" || bad "verify não detectou"

rm -f /tmp/out.$$
[ $fail -eq 0 ] && echo "TODOS OS TESTES PASSARAM" || { echo "HÁ FALHAS"; exit 1; }
