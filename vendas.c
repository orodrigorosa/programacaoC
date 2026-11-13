/*
 * vendas.c - Protótipo de controle de vendas com autenticação, papéis,
 * cancelamento autorizado e trilha de auditoria à prova de adulteração.
 *
 * Compilar: make
 * Banco:    arquivo vendas.db (ou variável de ambiente VENDAS_DB)
 *
 * Regras implementadas:
 *  1. Todo usuário tem login (nome + PIN). O PIN é guardado com salt + hash.
 *  2. A venda grava o vendedor logado; vendedor/total não podem ser alterados.
 *  3. Venda nunca é apagada: o cancelamento só muda o status.
 *  4. Cancelar exige a permissão 'venda.cancelar' OU o PIN de um supervisor
 *     (que tenha a permissão). Motivo é obrigatório.
 *  5. Toda ação (inclusive tentativas negadas e logins falhos) vai para a
 *     tabela 'auditoria', que é só-inserção e encadeada por hash (SHA-256).
 *  6. Venda refeita após cancelamento é ligada à original; se o vendedor for
 *     outro, ou se aparecer venda igual por outra pessoa em até 1 hora,
 *     a venda é marcada com ALERTA.
 *
 * AVISO: protótipo didático. Em produção use bcrypt/argon2 para o PIN,
 * um servidor de banco com usuário da aplicação sem DELETE/UPDATE na
 * auditoria, e guarde o último hash da cadeia fora do banco.
 */
#include <errno.h>
#include <math.h>
#include <openssl/sha.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define JANELA_REVENDA_SEG 3600
#define MOTIVO_MIN 5

static sqlite3 *db;

/* ------------------------------------------------------------ utilitários */

static void die(const char *msg) {
    fprintf(stderr, "ERRO: %s\n", msg);
    exit(1);
}

static void die_db(const char *ctx) {
    fprintf(stderr, "ERRO (%s): %s\n", ctx, sqlite3_errmsg(db));
    exit(1);
}

static sqlite3_stmt *prep(const char *sql) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) die_db(sql);
    return st;
}

static void exec(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "ERRO SQL: %s\n", err ? err : "?");
        sqlite3_free(err);
        exit(1);
    }
}

static const char *txt(sqlite3_stmt *st, int col) {
    const char *t = (const char *)sqlite3_column_text(st, col);
    return t ? t : "";
}

static void sha256_hex(const char *in, char out[65]) {
    unsigned char h[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)in, strlen(in), h);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) sprintf(out + 2 * i, "%02x", h[i]);
}

static void hash_pin(const char *salt, const char *pin, char out[65]) {
    char buf[256];
    unsigned char h[SHA256_DIGEST_LENGTH], t[SHA256_DIGEST_LENGTH];
    snprintf(buf, sizeof buf, "%s:%s", salt, pin);
    SHA256((const unsigned char *)buf, strlen(buf), h);
    for (int i = 0; i < 20000; i++) { /* alongamento simples */
        SHA256(h, sizeof h, t);
        memcpy(h, t, sizeof h);
    }
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) sprintf(out + 2 * i, "%02x", h[i]);
}

static void gen_salt(char out[33]) {
    unsigned char r[16];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(r, 1, sizeof r, f) != sizeof r) die("sem /dev/urandom");
    fclose(f);
    for (int i = 0; i < 16; i++) sprintf(out + 2 * i, "%02x", r[i]);
}

static int const_eq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned char d = (unsigned char)(la ^ lb);
    for (size_t i = 0; i < la && i < lb; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

/* Lê o PIN sem eco quando houver terminal; aceita pipe para testes. */
static void read_pin(const char *prompt, char *buf, size_t n) {
    struct termios old, neu;
    int tty = isatty(STDIN_FILENO);
    fprintf(stderr, "%s", prompt);
    if (tty) {
        tcgetattr(STDIN_FILENO, &old);
        neu = old;
        neu.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &neu);
    }
    if (!fgets(buf, (int)n, stdin)) buf[0] = '\0';
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        fprintf(stderr, "\n");
    }
    buf[strcspn(buf, "\r\n")] = '\0';
}

static long parse_centavos(const char *s) {
    char *end;
    double v = strtod(s, &end);
    if (*end || v <= 0 || v > 1e9) die("valor inválido");
    return lround(v * 100.0);
}

static void fmt_money(long c, char *out, size_t n) {
    snprintf(out, n, "R$ %ld.%02ld", c / 100, c % 100);
}

/* -------------------------------------------------------------- auditoria */

/* Cada linha guarda o hash da anterior: alterar qualquer linha quebra a cadeia. */
static void audit_hash(const char *prev, long uid, const char *acao, const char *ent,
                       long eid, const char *det, long ts, char out[65]) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s|%ld|%s|%s|%ld|%s|%ld", prev, uid, acao, ent, eid, det, ts);
    sha256_hex(buf, out);
}

static void audit(long uid, const char *acao, const char *ent, long eid, const char *det) {
    char prev[65] = "GENESIS", h[65];
    sqlite3_stmt *st = prep("SELECT hash FROM auditoria ORDER BY id DESC LIMIT 1");
    if (sqlite3_step(st) == SQLITE_ROW) snprintf(prev, sizeof prev, "%s", txt(st, 0));
    sqlite3_finalize(st);

    long ts = (long)time(NULL);
    audit_hash(prev, uid, acao, ent, eid, det, ts, h);

    st = prep("INSERT INTO auditoria(usuario_id,acao,entidade,entidade_id,detalhe,ts,hash_ant,hash)"
              " VALUES(?,?,?,?,?,?,?,?)");
    sqlite3_bind_int64(st, 1, uid);
    sqlite3_bind_text(st, 2, acao, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, ent, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, eid);
    sqlite3_bind_text(st, 5, det, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 6, ts);
    sqlite3_bind_text(st, 7, prev, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 8, h, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) die_db("auditoria");
    sqlite3_finalize(st);
}

/* ------------------------------------------------------------------ banco */

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS usuarios("
    " id INTEGER PRIMARY KEY, nome TEXT UNIQUE NOT NULL, papel TEXT NOT NULL,"
    " salt TEXT NOT NULL, pin_hash TEXT NOT NULL, ativo INTEGER NOT NULL DEFAULT 1);"
    "CREATE TABLE IF NOT EXISTS permissoes(papel TEXT NOT NULL, permissao TEXT NOT NULL,"
    " PRIMARY KEY(papel,permissao));"
    "CREATE TABLE IF NOT EXISTS vendas("
    " id INTEGER PRIMARY KEY, vendedor_id INTEGER NOT NULL REFERENCES usuarios(id),"
    " total INTEGER NOT NULL, status TEXT NOT NULL DEFAULT 'ATIVA',"
    " criada_em INTEGER NOT NULL, venda_original_id INTEGER REFERENCES vendas(id),"
    " alerta TEXT, cancelado_por INTEGER REFERENCES usuarios(id),"
    " autorizado_por INTEGER REFERENCES usuarios(id), motivo_cancelamento TEXT,"
    " cancelada_em INTEGER);"
    "CREATE TABLE IF NOT EXISTS auditoria("
    " id INTEGER PRIMARY KEY, usuario_id INTEGER NOT NULL, acao TEXT NOT NULL,"
    " entidade TEXT NOT NULL, entidade_id INTEGER NOT NULL, detalhe TEXT NOT NULL,"
    " ts INTEGER NOT NULL, hash_ant TEXT NOT NULL, hash TEXT NOT NULL);"
    /* Imutabilidade imposta pelo próprio banco */
    "CREATE TRIGGER IF NOT EXISTS aud_no_update BEFORE UPDATE ON auditoria"
    " BEGIN SELECT RAISE(ABORT,'auditoria é imutável'); END;"
    "CREATE TRIGGER IF NOT EXISTS aud_no_delete BEFORE DELETE ON auditoria"
    " BEGIN SELECT RAISE(ABORT,'auditoria é imutável'); END;"
    "CREATE TRIGGER IF NOT EXISTS venda_no_delete BEFORE DELETE ON vendas"
    " BEGIN SELECT RAISE(ABORT,'venda não pode ser apagada; cancele'); END;"
    "CREATE TRIGGER IF NOT EXISTS venda_no_edit BEFORE UPDATE ON vendas"
    " WHEN OLD.status='CANCELADA' OR OLD.vendedor_id<>NEW.vendedor_id"
    "   OR OLD.total<>NEW.total OR OLD.criada_em<>NEW.criada_em"
    " BEGIN SELECT RAISE(ABORT,'venda é imutável (só pode ser cancelada uma vez)'); END;";

static const char *PERMS[][2] = {
    {"vendedor", "venda.criar"},
    {"supervisor", "venda.criar"},   {"supervisor", "venda.cancelar"},
    {"supervisor", "auditoria.ver"}, {"supervisor", "relatorio.ver"},
    {"dono", "venda.criar"},         {"dono", "venda.cancelar"},
    {"dono", "auditoria.ver"},       {"dono", "relatorio.ver"},
    {"dono", "usuario.criar"},
};

static void open_db(void) {
    const char *path = getenv("VENDAS_DB");
    if (sqlite3_open(path ? path : "vendas.db", &db) != SQLITE_OK) die_db("abrir banco");
    exec("PRAGMA foreign_keys=ON;");
}

static void init_db(void) {
    exec(SCHEMA);
    sqlite3_stmt *st = prep("INSERT OR IGNORE INTO permissoes(papel,permissao) VALUES(?,?)");
    for (size_t i = 0; i < sizeof PERMS / sizeof PERMS[0]; i++) {
        sqlite3_bind_text(st, 1, PERMS[i][0], -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, PERMS[i][1], -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
}

/* ------------------------------------------------------ login e permissões */

static int has_perm(const char *papel, const char *perm) {
    sqlite3_stmt *st = prep("SELECT 1 FROM permissoes WHERE papel=? AND permissao=?");
    sqlite3_bind_text(st, 1, papel, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, perm, -1, SQLITE_STATIC);
    int ok = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return ok;
}

/* Autentica; devolve id (>0) ou 0. Falhas também são auditadas. */
static long login(const char *nome, char *papel, size_t psz) {
    char pin[64], h[65];
    char prompt[128];
    snprintf(prompt, sizeof prompt, "PIN de %s: ", nome);
    read_pin(prompt, pin, sizeof pin);

    sqlite3_stmt *st = prep("SELECT id,papel,salt,pin_hash,ativo FROM usuarios WHERE nome=?");
    sqlite3_bind_text(st, 1, nome, -1, SQLITE_STATIC);
    long uid = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        hash_pin(txt(st, 2), pin, h);
        if (sqlite3_column_int(st, 4) && const_eq(h, txt(st, 3))) {
            uid = sqlite3_column_int64(st, 0);
            snprintf(papel, psz, "%s", txt(st, 1));
        }
    }
    sqlite3_finalize(st);
    memset(pin, 0, sizeof pin);
    if (!uid) audit(0, "LOGIN_FALHA", "usuario", 0, nome);
    return uid;
}

static long need_login(const char *nome, char *papel, size_t psz) {
    long uid = login(nome, papel, psz);
    if (!uid) die("usuário ou PIN inválido");
    return uid;
}

static void need_perm(long uid, const char *papel, const char *perm) {
    if (has_perm(papel, perm)) return;
    audit(uid, "ACESSO_NEGADO", "permissao", 0, perm);
    fprintf(stderr, "ERRO: seu papel (%s) não tem a permissão '%s'\n", papel, perm);
    exit(2);
}

/* --------------------------------------------------------------- comandos */

static void cmd_init(void) {
    init_db();
    printf("Banco inicializado.\n");
}

static void cmd_user_add(int argc, char **argv) {
    /* user-add <quem-cria|-> <novo> <papel> */
    if (argc != 5) die("uso: user-add <admin|-> <novo-usuario> <vendedor|supervisor|dono>");
    const char *admin = argv[2], *novo = argv[3], *papel = argv[4];
    if (strcmp(papel, "vendedor") && strcmp(papel, "supervisor") && strcmp(papel, "dono"))
        die("papel deve ser vendedor, supervisor ou dono");

    sqlite3_stmt *st = prep("SELECT COUNT(*) FROM usuarios");
    sqlite3_step(st);
    int vazio = sqlite3_column_int(st, 0) == 0;
    sqlite3_finalize(st);

    long aid = 0;
    if (vazio) {
        if (strcmp(papel, "dono")) die("o primeiro usuário precisa ser 'dono'");
        if (strcmp(admin, "-")) die("banco vazio: use '-' como admin");
    } else {
        char ap[32];
        aid = need_login(admin, ap, sizeof ap);
        need_perm(aid, ap, "usuario.criar");
    }

    char p1[64], p2[64], salt[33], h[65];
    read_pin("Novo PIN (mín. 4): ", p1, sizeof p1);
    read_pin("Repita o PIN: ", p2, sizeof p2);
    if (strlen(p1) < 4 || strcmp(p1, p2)) die("PIN inválido ou diferente");
    gen_salt(salt);
    hash_pin(salt, p1, h);

    st = prep("INSERT INTO usuarios(nome,papel,salt,pin_hash) VALUES(?,?,?,?)");
    sqlite3_bind_text(st, 1, novo, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, papel, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, salt, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, h, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) die("usuário já existe");
    long id = (long)sqlite3_last_insert_rowid(db);
    sqlite3_finalize(st);
    char det[96];
    snprintf(det, sizeof det, "%s papel=%s", novo, papel);
    audit(aid, "USUARIO_CRIADO", "usuario", id, det);
    printf("Usuário %s (%s) criado.\n", novo, papel);
}

/* Cria venda; detecta indícios de "roubo" de venda e grava o alerta. */
static long create_sale(long uid, long total, long orig) {
    char alerta[128] = "";
    if (orig > 0) {
        sqlite3_stmt *st = prep("SELECT vendedor_id,status,total FROM vendas WHERE id=?");
        sqlite3_bind_int64(st, 1, orig);
        if (sqlite3_step(st) != SQLITE_ROW) die("venda original não existe");
        if (strcmp(txt(st, 1), "CANCELADA")) die("a venda original não está cancelada");
        if (sqlite3_column_int64(st, 0) != uid) snprintf(alerta, sizeof alerta, "OUTRO_VENDEDOR(orig #%ld)", orig);
        sqlite3_finalize(st);
    } else {
        sqlite3_stmt *st = prep("SELECT id FROM vendas WHERE status='CANCELADA' AND total=?"
                                " AND vendedor_id<>? AND cancelada_em>=? ORDER BY cancelada_em DESC LIMIT 1");
        sqlite3_bind_int64(st, 1, total);
        sqlite3_bind_int64(st, 2, uid);
        sqlite3_bind_int64(st, 3, (long)time(NULL) - JANELA_REVENDA_SEG);
        if (sqlite3_step(st) == SQLITE_ROW)
            snprintf(alerta, sizeof alerta, "POSSIVEL_REFEITA(#%lld)", sqlite3_column_int64(st, 0));
        sqlite3_finalize(st);
    }

    sqlite3_stmt *st = prep("INSERT INTO vendas(vendedor_id,total,criada_em,venda_original_id,alerta)"
                            " VALUES(?,?,?,?,?)");
    sqlite3_bind_int64(st, 1, uid);
    sqlite3_bind_int64(st, 2, total);
    sqlite3_bind_int64(st, 3, (long)time(NULL));
    if (orig > 0) sqlite3_bind_int64(st, 4, orig); else sqlite3_bind_null(st, 4);
    if (*alerta) sqlite3_bind_text(st, 5, alerta, -1, SQLITE_STATIC); else sqlite3_bind_null(st, 5);
    if (sqlite3_step(st) != SQLITE_DONE) die_db("inserir venda");
    long id = (long)sqlite3_last_insert_rowid(db);
    sqlite3_finalize(st);

    char det[200], m[32];
    fmt_money(total, m, sizeof m);
    snprintf(det, sizeof det, "total=%s%s%s", m, *alerta ? " ALERTA=" : "", alerta);
    audit(uid, "VENDA_CRIADA", "venda", id, det);
    if (*alerta) audit(uid, "ALERTA_REVENDA", "venda", id, alerta);
    printf("Venda #%ld registrada (%s)%s%s\n", id, m, *alerta ? "  ** ALERTA: " : "", alerta);
    return id;
}

static void cmd_sell(int argc, char **argv, int resell) {
    /* sell <user> <valor>   |   resell <user> <venda-original> <valor> */
    if (argc != (resell ? 5 : 4)) die(resell ? "uso: resell <user> <venda-original> <valor>" : "uso: sell <user> <valor>");
    char papel[32];
    long uid = need_login(argv[2], papel, sizeof papel);
    need_perm(uid, papel, "venda.criar");
    create_sale(uid, parse_centavos(argv[resell ? 4 : 3]), resell ? atol(argv[3]) : 0);
}

static void cmd_cancel(int argc, char **argv) {
    /* cancel <user> <venda-id> "<motivo>" [supervisor] */
    if (argc < 5 || argc > 6) die("uso: cancel <user> <venda-id> \"<motivo>\" [supervisor]");
    long vid = atol(argv[3]);
    const char *motivo = argv[4], *supname = argc == 6 ? argv[5] : NULL;
    if (strlen(motivo) < MOTIVO_MIN) die("motivo obrigatório (mínimo 5 caracteres)");

    char papel[32];
    long uid = need_login(argv[2], papel, sizeof papel);

    sqlite3_stmt *st = prep("SELECT vendedor_id,status FROM vendas WHERE id=?");
    sqlite3_bind_int64(st, 1, vid);
    if (sqlite3_step(st) != SQLITE_ROW) die("venda não encontrada");
    long vendedor = sqlite3_column_int64(st, 0);
    int ativa = !strcmp(txt(st, 1), "ATIVA");
    sqlite3_finalize(st);
    if (!ativa) die("a venda já está cancelada");

    long auth = 0;
    if (has_perm(papel, "venda.cancelar")) {
        auth = uid;
    } else {
        char det[160];
        snprintf(det, sizeof det, "sem permissão; motivo informado: %s", motivo);
        audit(uid, "CANCELAMENTO_SEM_PERMISSAO", "venda", vid, det);
        if (!supname) {
            fprintf(stderr, "ERRO: seu papel (%s) não pode cancelar. Peça a autorização de um supervisor.\n", papel);
            exit(2);
        }
        char sp[32];
        long sid = need_login(supname, sp, sizeof sp);
        if (sid == uid) die("o supervisor precisa ser outra pessoa");
        if (!has_perm(sp, "venda.cancelar")) {
            audit(sid, "AUTORIZACAO_NEGADA", "venda", vid, "autorizador sem permissão");
            die("o autorizador não tem permissão de cancelar");
        }
        auth = sid;
    }

    exec("BEGIN IMMEDIATE");
    st = prep("UPDATE vendas SET status='CANCELADA', cancelado_por=?, autorizado_por=?,"
              " motivo_cancelamento=?, cancelada_em=? WHERE id=? AND status='ATIVA'");
    sqlite3_bind_int64(st, 1, uid);
    sqlite3_bind_int64(st, 2, auth);
    sqlite3_bind_text(st, 3, motivo, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, (long)time(NULL));
    sqlite3_bind_int64(st, 5, vid);
    if (sqlite3_step(st) != SQLITE_DONE || sqlite3_changes(db) != 1) { exec("ROLLBACK"); die("não foi possível cancelar"); }
    sqlite3_finalize(st);
    char det[300];
    snprintf(det, sizeof det, "vendedor_original=%ld autorizado_por=%ld motivo=%s", vendedor, auth, motivo);
    audit(uid, "VENDA_CANCELADA", "venda", vid, det);
    exec("COMMIT");
    printf("Venda #%ld cancelada (por usuário %ld, autorizada por %ld).\n", vid, uid, auth);
}

static void cmd_audit(int argc, char **argv) {
    if (argc < 3) die("uso: audit <user> [quantidade]");
    char papel[32];
    long uid = need_login(argv[2], papel, sizeof papel);
    need_perm(uid, papel, "auditoria.ver");
    int n = argc > 3 ? atoi(argv[3]) : 50;
    sqlite3_stmt *st = prep("SELECT a.id, datetime(a.ts,'unixepoch','localtime'), COALESCE(u.nome,'(desconhecido)'),"
                            " a.acao, a.entidade, a.entidade_id, a.detalhe FROM auditoria a"
                            " LEFT JOIN usuarios u ON u.id=a.usuario_id ORDER BY a.id DESC LIMIT ?");
    sqlite3_bind_int(st, 1, n);
    while (sqlite3_step(st) == SQLITE_ROW)
        printf("#%-4d %s  %-12s %-26s %s#%lld  %s\n", sqlite3_column_int(st, 0), txt(st, 1), txt(st, 2),
               txt(st, 3), txt(st, 4), sqlite3_column_int64(st, 5), txt(st, 6));
    sqlite3_finalize(st);
}

static void cmd_verify(int argc, char **argv) {
    if (argc != 3) die("uso: verify <user>");
    char papel[32];
    long uid = need_login(argv[2], papel, sizeof papel);
    need_perm(uid, papel, "auditoria.ver");
    sqlite3_stmt *st = prep("SELECT id,usuario_id,acao,entidade,entidade_id,detalhe,ts,hash_ant,hash"
                            " FROM auditoria ORDER BY id");
    char prev[65] = "GENESIS", h[65];
    long n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        audit_hash(prev, sqlite3_column_int64(st, 1), txt(st, 2), txt(st, 3), sqlite3_column_int64(st, 4),
                   txt(st, 5), sqlite3_column_int64(st, 6), h);
        if (strcmp(prev, txt(st, 7)) || strcmp(h, txt(st, 8))) {
            printf("ADULTERAÇÃO DETECTADA na linha de auditoria #%lld\n", sqlite3_column_int64(st, 0));
            sqlite3_finalize(st);
            exit(3);
        }
        snprintf(prev, sizeof prev, "%s", h);
        n++;
    }
    sqlite3_finalize(st);
    printf("Cadeia íntegra: %ld registros verificados. Último hash: %s\n", n, prev);
}

static void cmd_report(int argc, char **argv) {
    if (argc != 3) die("uso: report <user>");
    char papel[32];
    long uid = need_login(argv[2], papel, sizeof papel);
    need_perm(uid, papel, "relatorio.ver");

    printf("== Cancelamentos por usuário ==\n");
    sqlite3_stmt *st = prep("SELECT u.nome, COUNT(*), SUM(v.total) FROM vendas v"
                            " JOIN usuarios u ON u.id=v.cancelado_por WHERE v.status='CANCELADA'"
                            " GROUP BY u.id ORDER BY 2 DESC");
    while (sqlite3_step(st) == SQLITE_ROW) {
        char m[32];
        fmt_money(sqlite3_column_int64(st, 2), m, sizeof m);
        printf("  %-12s %d cancelamento(s)  %s\n", txt(st, 0), sqlite3_column_int(st, 1), m);
    }
    sqlite3_finalize(st);

    printf("== Vendas canceladas (detalhe) ==\n");
    st = prep("SELECT v.id, vend.nome, canc.nome, COALESCE(aut.nome,'-'), v.motivo_cancelamento, v.total"
              " FROM vendas v JOIN usuarios vend ON vend.id=v.vendedor_id"
              " JOIN usuarios canc ON canc.id=v.cancelado_por LEFT JOIN usuarios aut ON aut.id=v.autorizado_por"
              " WHERE v.status='CANCELADA' ORDER BY v.id");
    while (sqlite3_step(st) == SQLITE_ROW) {
        char m[32];
        fmt_money(sqlite3_column_int64(st, 5), m, sizeof m);
        printf("  venda #%d (%s) vendedor=%s cancelou=%s autorizou=%s motivo=\"%s\"\n", sqlite3_column_int(st, 0), m,
               txt(st, 1), txt(st, 2), txt(st, 3), txt(st, 4));
    }
    sqlite3_finalize(st);

    printf("== ALERTAS: possível venda refeita por outra pessoa ==\n");
    st = prep("SELECT v.id, u.nome, v.alerta, v.total FROM vendas v JOIN usuarios u ON u.id=v.vendedor_id"
              " WHERE v.alerta IS NOT NULL ORDER BY v.id");
    int any = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        char m[32];
        fmt_money(sqlite3_column_int64(st, 3), m, sizeof m);
        printf("  venda #%d (%s) feita por %s -> %s\n", sqlite3_column_int(st, 0), m, txt(st, 1), txt(st, 2));
        any = 1;
    }
    if (!any) printf("  nenhum\n");
    sqlite3_finalize(st);
}

static void usage(void) {
    fprintf(stderr,
            "uso: vendas <comando> ...\n"
            "  init\n"
            "  user-add <admin|-> <novo> <vendedor|supervisor|dono>\n"
            "  sell     <user> <valor>\n"
            "  resell   <user> <venda-original> <valor>\n"
            "  cancel   <user> <venda-id> \"<motivo>\" [supervisor]\n"
            "  audit    <user> [n]      (log de auditoria)\n"
            "  verify   <user>          (confere a cadeia de hashes)\n"
            "  report   <user>          (cancelamentos e alertas)\n"
            "PINs são lidos da entrada padrão (sem eco no terminal).\n");
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 2) usage();
    open_db();
    init_db();
    const char *c = argv[1];
    if (!strcmp(c, "init")) cmd_init();
    else if (!strcmp(c, "user-add")) cmd_user_add(argc, argv);
    else if (!strcmp(c, "sell")) cmd_sell(argc, argv, 0);
    else if (!strcmp(c, "resell")) cmd_sell(argc, argv, 1);
    else if (!strcmp(c, "cancel")) cmd_cancel(argc, argv);
    else if (!strcmp(c, "audit")) cmd_audit(argc, argv);
    else if (!strcmp(c, "verify")) cmd_verify(argc, argv);
    else if (!strcmp(c, "report")) cmd_report(argc, argv);
    else usage();
    sqlite3_close(db);
    return 0;
}
