#define TB_CURRENT_GROUP TB_G_CONFIG /* зона ответственности файла */
/* ==========================================================================
 *  src/tools/market.cpp — менеджер конфигурации рынка (CLI `market`).
 *
 *  Принцип: ИИ (и человек) НЕ правит C-код и не редактирует JSON руками,
 *  чтобы добавить пул/биржу/логотип — только эти команды. Конфиг —
 *  «таблица с ячейками»: каждое поле (url, quoter, fee, logo, note, …)
 *  адресуется отдельно; update/лого меняют ровно свою ячейку, соседние
 *  не затрагиваются (логотип и текст — разные ячейки, хоть и в одной
 *  визуальной строке).
 *
 *    market dex add NAME --url U --quoter 0x… | --router 0x…
 *           [--factory 0x…] [--type v2|v3] [--chain N] [--weth 0x…]
 *    market dex update NAME [--url U] [--quoter 0x…] [--router 0x…]
 *           [--factory 0x…] [--type v2|v3] [--chain N] [--weth 0x…]
 *           [--note S]
 *    market dex logo set NAME --file путь.{png,svg,webp,jpg}
 *    market asset add SYM --address 0x… [--chain N] [--decimals D]
 *           [--file лого] [--note S]
 *    market asset logo set SYM --file путь   (кладётся в logos/token/)
 *    market pool discover [--dex NAME] [--symbol SYM] [--add]
 *    market pool inspect 0x…                 (конфиг + фабрика на цепи)
 *    market pool enable|disable 0x…
 *    market config validate [--verify]       (--verify — сверка фабрик)
 *    market config diff [A.json B.json]      (по умолчанию: vs снапшот)
 *    market config rollback [--step N] [--list]
 *
 *  Каждая мутация сначала снимает снапшот в config-history/ — откат
 *  всегда возможен. Общий флаг --config PATH (или env MARKET_CONFIG).
 * ========================================================================== */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <strings.h>
#include <vector>

#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"
#include "tb/core/error.h"
#include "tb/core/time.h"
#include "tb/exec/backend_dex.h"
#include "tb/net/http.h"
#include "tb/user/arb.h"

/* ---- ячейки JSON: точечные присваивания ---- */

using rapidjson::Value;

static void cell_str(Value &row, const char *key, const char *v,
                     rapidjson::Document::AllocatorType &al)
{
    row.RemoveMember(key);
    Value s(v, (rapidjson::SizeType)strlen(v), al);
    Value k(key, (rapidjson::SizeType)strlen(key), al);
    row.AddMember(k, s, al);
}

static void cell_u64(Value &row, const char *key, uint64_t v,
                     rapidjson::Document::AllocatorType &al)
{
    row.RemoveMember(key);
    Value k(key, (rapidjson::SizeType)strlen(key), al);
    row.AddMember(k, Value(v), al);
}

/* ---- конфиг: чтение/запись ---- */

static bool cfg_load_doc(rapidjson::Document &d, const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "market: нет конфига %s\n", path.c_str());
        return false;
    }
    std::string buf;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        buf.append(chunk, n);
    fclose(f);
    if (d.Parse(buf.c_str(), buf.size()).HasParseError()) {
        fprintf(stderr, "market: JSON-ошибка в %s (смещение %zu)\n",
                path.c_str(), (size_t)d.GetErrorOffset());
        return false;
    }
    if (!d.IsObject()) {
        fprintf(stderr, "market: корень %s не объект\n", path.c_str());
        return false;
    }
    return true;
}

static bool cfg_save_doc(const rapidjson::Document &d,
                         const std::string &path)
{
    rapidjson::StringBuffer sb;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> w(sb);
    w.SetIndent(' ', 2);
    d.Accept(w);
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "market: не открыть на запись %s\n", tmp.c_str());
        return false;
    }
    fwrite(sb.GetString(), 1, sb.GetSize(), f);
    fputc('\n', f);
    fclose(f);
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        fprintf(stderr, "market: rename %s: %s\n", tmp.c_str(),
                strerror(errno));
        return false;
    }
    return true;
}

static Value &array_get(rapidjson::Document &d, const char *key)
{
    auto it = d.FindMember(key);
    if (it == d.MemberEnd()) {
        Value arr(rapidjson::kArrayType);
        Value k(key, (rapidjson::SizeType)strlen(key), d.GetAllocator());
        d.AddMember(k, arr, d.GetAllocator());
    }
    return d[key];
}

/* ---- снапшоты/откат ---- */

static std::string history_dir(const std::string &cfg_path)
{
    /* рядом с конфигом: config-history/ */
    size_t slash = cfg_path.find_last_of('/');
    std::string base = slash == std::string::npos ? "."
                                                  : cfg_path.substr(0, slash);
    return base + "/config-history";
}

static void snapshot(const std::string &cfg_path, const char *cmd)
{
    static unsigned seq = 0; /* мутации в одну миллисекунду: порядок
                              * лексикографический = хронологический */
    std::string dir = history_dir(cfg_path);
    mkdir(dir.c_str(), 0755); /* EEXIST не важен */
    char name[128];
    snprintf(name, sizeof(name), "%013llu-%03u-%s.json",
             (unsigned long long)tb_now_ms(), seq++, cmd);
    std::string src = cfg_path, dst = dir + "/" + name;
    FILE *in = fopen(src.c_str(), "rb");
    if (!in)
        return; /* конфига ещё нет — нечего снапшотить */
    FILE *out = fopen(dst.c_str(), "wb");
    if (!out) {
        fclose(in);
        return;
    }
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), in)) > 0)
        fwrite(chunk, 1, n, out);
    fclose(in);
    fclose(out);
    printf("market: снапшот %s\n", dst.c_str());
}

static std::vector<std::string> snapshot_list(const std::string &cfg_path)
{
    std::vector<std::string> out;
    std::string dir = history_dir(cfg_path);
    DIR *d = opendir(dir.c_str());
    if (!d)
        return out;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        std::string n = e->d_name;
        if (n.size() > 5 && n.substr(n.size() - 5) == ".json")
            out.push_back(n);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

/* ---- адреса/файлы ---- */

static bool parse_addr(const char *hex, tb_addr_t *out)
{
    tb_error_t err = TB_ERROR_INIT;
    if (tb_addr_from_hex(out, hex, &err) != 0 || tb_addr_is_native(out)) {
        fprintf(stderr, "market: не адрес: '%s' (%s)\n", hex, err.message);
        return false;
    }
    return true;
}

static void addr_lower(const tb_addr_t *a, char *out /*43*/)
{
    tb_addr_to_hex(a, out, 43);
    for (char *p = out; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p += 'a' - 'A';
}

static bool mkdirs_logos(const char *kind)
{
    mkdir("logos", 0755);
    char p[64];
    snprintf(p, sizeof(p), "logos/%s", kind);
    return mkdir(p, 0755) == 0 || errno == EEXIST;
}

static bool copy_logo(const char *src_path, const std::string &dst)
{
    const char *ext = strrchr(src_path, '.');
    if (!ext || strlen(ext) > 6) {
        fprintf(stderr, "market: у логотипа нет расширения (.png/.svg/"
                        ".webp/.jpg): %s\n",
                src_path);
        return false;
    }
    FILE *in = fopen(src_path, "rb");
    if (!in) {
        fprintf(stderr, "market: нет файла %s\n", src_path);
        return false;
    }
    std::string full = dst + ext;
    FILE *out = fopen(full.c_str(), "wb");
    if (!out) {
        fprintf(stderr, "market: не записать %s\n", full.c_str());
        fclose(in);
        return false;
    }
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), in)) > 0)
        fwrite(chunk, 1, n, out);
    fclose(in);
    fclose(out);
    printf("market: логотип -> %s\n", full.c_str());
    return true;
}

/* ---- diff: каждый лист — ячейка с путём ---- */

static void flatten(const Value &v, const std::string &path,
                    std::map<std::string, std::string> &out)
{
    if (v.IsObject()) {
        for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
            std::string k =
                path + (path.empty() ? "" : ".") + it->name.GetString();
            flatten(it->value, k, out);
        }
        return;
    }
    if (v.IsArray()) {
        /* массивы строк-объектов индексируем человекочитаемым ключом:
         * rpc[uniswap-v3], pools[0x88e6…], assets[USDC] */
        for (rapidjson::SizeType i = 0; i < v.Size(); i++) {
            const Value &e = v[i];
            std::string k;
            if (e.IsObject()) {
                const char *keys[] = {"name", "pool", "symbol", "address"};
                const char *id = NULL;
                for (const char *kk : keys) {
                    auto it = e.FindMember(kk);
                    if (it != e.MemberEnd() && it->value.IsString()) {
                        id = it->value.GetString();
                        break;
                    }
                }
                k = id ? std::string("[") + id + "]"
                       : "[" + std::to_string(i) + "]";
            } else {
                k = "[" + std::to_string(i) + "]";
            }
            flatten(e, path + k, out);
        }
        return;
    }
    if (v.IsString())
        out[path] = std::string("\"") + v.GetString() + "\"";
    else if (v.IsTrue())
        out[path] = "true";
    else if (v.IsFalse())
        out[path] = "false";
    else if (v.IsNull())
        out[path] = "null";
    else {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        v.Accept(w);
        out[path] = sb.GetString();
    }
}

/* ---- поиск строк листов ---- */

/* адрес пула в конфиге может быть в любом регистре — сравниваем
 * регистронезависимо (0x88E6… == 0x88e6…) */
static Value *find_row_addr_icase(Value &arr, const char *key,
                                  const char *val)
{
    for (rapidjson::SizeType i = 0; i < arr.Size(); i++) {
        if (!arr[i].IsObject())
            continue;
        auto it = arr[i].FindMember(key);
        if (it != arr[i].MemberEnd() && it->value.IsString()
            && strcasecmp(it->value.GetString(), val) == 0)
            return &arr[i];
    }
    return NULL;
}

static Value *find_row_by_str(Value &arr, const char *key, const char *val)
{
    for (rapidjson::SizeType i = 0; i < arr.Size(); i++) {
        if (!arr[i].IsObject())
            continue;
        auto it = arr[i].FindMember(key);
        if (it != arr[i].MemberEnd() && it->value.IsString()
            && strcmp(it->value.GetString(), val) == 0)
            return &arr[i];
    }
    return NULL;
}

/* ---- аргументы: --флаг значение ---- */

static const char *arg_flag(int &i, int argc, char **argv, const char *name)
{
    if (strcmp(argv[i], name) == 0 && i + 1 < argc)
        return argv[++i];
    return NULL;
}

/* ================= команды ================= */

static int cmd_dex(int argc, char **argv, const std::string &cfg_path,
                   int start)
{
    if (start >= argc) {
        fprintf(stderr,
                "market dex: add NAME… | update NAME… | logo set NAME…\n");
        return 2;
    }
    const char *op = argv[start++];

    /* market dex logo set NAME --file P */
    if (strcmp(op, "logo") == 0) {
        if (start + 3 > argc || strcmp(argv[start], "set") != 0) {
            fprintf(stderr, "market dex logo set NAME --file путь\n");
            return 2;
        }
        const char *name = argv[start + 1];
        const char *file = NULL;
        for (int i = start + 2; i < argc; i++)
            if ((file = arg_flag(i, argc, argv, "--file")))
                ;
        if (!file) {
            fprintf(stderr, "market: нужен --file\n");
            return 2;
        }
        if (!mkdirs_logos("dex"))
            return 1;
        /* конфиг не меняется (файл лого лежит рядом) — снапшот не нужен */
        if (!copy_logo(file, std::string("logos/dex/") + name))
            return 1;
        return 0;
    }

    const char *name = argv[start++];
    const char *url = NULL, *quoter = NULL, *router = NULL, *factory = NULL;
    const char *type = NULL, *chain = NULL, *weth = NULL, *note = NULL;
    for (int i = start; i < argc; i++) {
        const char *v;
        if ((v = arg_flag(i, argc, argv, "--url")))
            url = v;
        else if ((v = arg_flag(i, argc, argv, "--quoter")))
            quoter = v;
        else if ((v = arg_flag(i, argc, argv, "--router")))
            router = v;
        else if ((v = arg_flag(i, argc, argv, "--factory")))
            factory = v;
        else if ((v = arg_flag(i, argc, argv, "--type")))
            type = v;
        else if ((v = arg_flag(i, argc, argv, "--chain")))
            chain = v;
        else if ((v = arg_flag(i, argc, argv, "--weth")))
            weth = v;
        else if ((v = arg_flag(i, argc, argv, "--note")))
            note = v;
        else {
            fprintf(stderr, "market dex: неизвестный флаг %s\n", argv[i]);
            return 2;
        }
    }
    if (type && strcmp(type, "v2") != 0 && strcmp(type, "v3") != 0) {
        fprintf(stderr, "market dex: --type v2|v3\n");
        return 2;
    }

    rapidjson::Document d;
    if (!cfg_load_doc(d, cfg_path))
        return 1;
    Value &rpc = array_get(d, "rpc");

    if (strcmp(op, "add") == 0) {
        if (!name || !url) {
            fprintf(stderr, "market dex add NAME --url U --quoter 0x|"
                            "--router 0x [--factory] [--type] [--chain] "
                            "[--weth] [--note]\n");
            return 2;
        }
        if (find_row_by_str(rpc, "name", name)) {
            fprintf(stderr, "market: биржа '%s' уже есть (update?)\n", name);
            return 1;
        }
        tb_addr_t a;
        if (quoter && !parse_addr(quoter, &a))
            return 1;
        if (router && !parse_addr(router, &a))
            return 1;
        if (factory && !parse_addr(factory, &a))
            return 1;
        if (weth && !parse_addr(weth, &a))
            return 1;
        snapshot(cfg_path, "dex-add");
        Value row(rapidjson::kObjectType);
        auto &al = d.GetAllocator();
        cell_str(row, "name", name, al);
        cell_str(row, "url", url, al);
        if (quoter)
            cell_str(row, "quoter", quoter, al);
        if (router)
            cell_str(row, "router", router, al);
        if (factory)
            cell_str(row, "factory", factory, al);
        if (type && strcmp(type, "v2") == 0)
            cell_str(row, "type", "v2", al);
        if (chain)
            cell_u64(row, "chain_id", strtoull(chain, NULL, 10), al);
        if (weth)
            cell_str(row, "weth", weth, al);
        if (note)
            cell_str(row, "note", note, al);
        rpc.PushBack(row, al);
        if (!cfg_save_doc(d, cfg_path))
            return 1;
        printf("market: биржа '%s' добавлена (%u всего)\n", name,
               (unsigned)rpc.Size());
        return 0;
    }

    if (strcmp(op, "update") == 0) {
        Value *row = find_row_by_str(rpc, "name", name);
        if (!row) {
            fprintf(stderr, "market: биржи '%s' нет (add?)\n", name);
            return 1;
        }
        if (!url && !quoter && !router && !factory && !type && !chain
            && !weth && !note) {
            fprintf(stderr, "market dex update: не указано ни одной ячейки "
                            "(--url/--quoter/--router/--factory/--type/"
                            "--chain/--weth/--note)\n");
            return 2;
        }
        tb_addr_t a;
        if (quoter && !parse_addr(quoter, &a))
            return 1;
        if (router && !parse_addr(router, &a))
            return 1;
        if (factory && !parse_addr(factory, &a))
            return 1;
        if (weth && !parse_addr(weth, &a))
            return 1;
        snapshot(cfg_path, "dex-update");
        auto &al = d.GetAllocator();
        if (url)
            cell_str(*row, "url", url, al);
        if (quoter)
            cell_str(*row, "quoter", quoter, al);
        if (router)
            cell_str(*row, "router", router, al);
        if (factory)
            cell_str(*row, "factory", factory, al);
        if (type)
            cell_str(*row, "type", type, al);
        if (chain)
            cell_u64(*row, "chain_id", strtoull(chain, NULL, 10), al);
        if (weth)
            cell_str(*row, "weth", weth, al);
        if (note)
            cell_str(*row, "note", note, al);
        if (!cfg_save_doc(d, cfg_path))
            return 1;
        printf("market: биржа '%s' обновлена\n", name);
        return 0;
    }

    fprintf(stderr, "market dex: неизвестная операция '%s'\n", op);
    return 2;
}

static int cmd_asset(int argc, char **argv, const std::string &cfg_path,
                     int start)
{
    if (start >= argc) {
        fprintf(stderr, "market asset: add SYM… | logo set SYM…\n");
        return 2;
    }
    const char *op = argv[start++];

    if (strcmp(op, "logo") == 0) {
        if (start + 3 > argc || strcmp(argv[start], "set") != 0) {
            fprintf(stderr, "market asset logo set SYM --file путь\n");
            return 2;
        }
        const char *sym = argv[start + 1];
        const char *file = NULL;
        for (int i = start + 2; i < argc; i++)
            if ((file = arg_flag(i, argc, argv, "--file")))
                ;
        if (!file) {
            fprintf(stderr, "market: нужен --file\n");
            return 2;
        }
        rapidjson::Document d;
        if (!cfg_load_doc(d, cfg_path))
            return 1;
        Value &assets = array_get(d, "assets");
        Value *row = find_row_by_str(assets, "symbol", sym);
        if (!row) {
            fprintf(stderr,
                    "market: актива '%s' нет — сначала asset add\n", sym);
            return 1;
        }
        auto it = row->FindMember("address");
        if (it == row->MemberEnd() || !it->value.IsString()) {
            fprintf(stderr, "market: у '%s' нет ячейки address\n", sym);
            return 1;
        }
        tb_addr_t addr;
        if (!parse_addr(it->value.GetString(), &addr))
            return 1;
        char low[43];
        addr_lower(&addr, low);
        if (!mkdirs_logos("token"))
            return 1;
        snapshot(cfg_path, "asset-logo-set");
        if (!copy_logo(file, std::string("logos/token/") + low))
            return 1;
        const char *ext = strrchr(file, '.');
        cell_str(*row, "logo", (std::string(low) + ext).c_str(),
                 d.GetAllocator());
        if (!cfg_save_doc(d, cfg_path))
            return 1;
        return 0;
    }

    const char *sym = argv[start++];
    const char *address = NULL, *chain = NULL, *dec = NULL, *file = NULL;
    const char *note = NULL;
    for (int i = start; i < argc; i++) {
        const char *v;
        if ((v = arg_flag(i, argc, argv, "--address")))
            address = v;
        else if ((v = arg_flag(i, argc, argv, "--chain")))
            chain = v;
        else if ((v = arg_flag(i, argc, argv, "--decimals")))
            dec = v;
        else if ((v = arg_flag(i, argc, argv, "--file")))
            file = v;
        else if ((v = arg_flag(i, argc, argv, "--note")))
            note = v;
        else {
            fprintf(stderr, "market asset: неизвестный флаг %s\n", argv[i]);
            return 2;
        }
    }

    if (strcmp(op, "add") != 0) {
        fprintf(stderr, "market asset: неизвестная операция '%s'\n", op);
        return 2;
    }
    if (!sym || !address) {
        fprintf(stderr, "market asset add SYM --address 0x… [--chain N] "
                        "[--decimals D] [--file лого] [--note S]\n");
        return 2;
    }
    tb_addr_t addr;
    if (!parse_addr(address, &addr))
        return 1;

    rapidjson::Document d;
    if (!cfg_load_doc(d, cfg_path))
        return 1;
    Value &assets = array_get(d, "assets");
    char low[43];
    addr_lower(&addr, low);
    /* дубликат: тот же адрес (+цепь) уже в листе */
    for (rapidjson::SizeType i = 0; i < assets.Size(); i++) {
        auto it = assets[i].FindMember("address");
        if (it == assets[i].MemberEnd() || !it->value.IsString())
            continue;
        tb_addr_t other;
        tb_error_t e = TB_ERROR_INIT;
        if (tb_addr_from_hex(&other, it->value.GetString(), &e) == 0
            && tb_addr_eq(&other, &addr)) {
            fprintf(stderr, "market: адрес %s уже в листе активов\n", low);
            return 1;
        }
    }
    snapshot(cfg_path, "asset-add");
    Value row(rapidjson::kObjectType);
    auto &al = d.GetAllocator();
    cell_str(row, "symbol", sym, al);
    cell_str(row, "address", low, al);
    if (chain)
        cell_u64(row, "chain", strtoull(chain, NULL, 10), al);
    if (dec)
        cell_u64(row, "decimals", strtoull(dec, NULL, 10), al);
    if (note)
        cell_str(row, "note", note, al);
    assets.PushBack(row, al);

    if (file) {
        if (!mkdirs_logos("token"))
            return 1;
        if (!copy_logo(file, std::string("logos/token/") + low))
            return 1;
        const char *ext = strrchr(file, '.');
        cell_str(assets[assets.Size() - 1], "logo",
                 (std::string(low) + ext).c_str(), al);
    }
    if (!cfg_save_doc(d, cfg_path))
        return 1;
    printf("market: актив '%s' (%s) добавлен (%u всего)\n", sym, low,
           (unsigned)assets.Size());
    return 0;
}

/* ---- pool ---- */

struct pool_row {
    std::string token, pool, dex, note;
    uint32_t fee = 0;
    bool enabled = true;
    bool has_enabled = false;
    int raw_idx = -1;
};

static std::vector<pool_row> raw_pools(const rapidjson::Document &d)
{
    std::vector<pool_row> out;
    auto it = d.FindMember("pools");
    if (it == d.MemberEnd() || !it->value.IsArray())
        return out;
    int idx = 0;
    for (const auto &v : it->value.GetArray()) {
        if (!v.IsObject())
            continue;
        pool_row r;
        r.raw_idx = idx++;
        auto g = [&](const char *k) -> const char * {
            auto f = v.FindMember(k);
            return f != v.MemberEnd() && f->value.IsString()
                       ? f->value.GetString()
                       : NULL;
        };
        if (g("token"))
            r.token = g("token");
        if (g("pool"))
            r.pool = g("pool");
        if (g("dex"))
            r.dex = g("dex");
        if (g("note"))
            r.note = g("note");
        {
            auto f = v.FindMember("fee");
            if (f != v.MemberEnd() && f->value.IsUint())
                r.fee = f->value.GetUint();
        }
        {
            auto f = v.FindMember("enabled");
            if (f != v.MemberEnd() && f->value.IsBool()) {
                r.enabled = f->value.GetBool();
                r.has_enabled = true;
            }
        }
        out.push_back(r);
    }
    return out;
}

/* строка venue из rpc[] для цепных операций */
static bool venue_of(const rapidjson::Document &d, const char *name,
                     tb_exec_dex_venue_t *v)
{
    memset(v, 0, sizeof(*v));
    snprintf(v->name, sizeof(v->name), "%s", name);
    auto rpc = d.FindMember("rpc");
    if (rpc == d.MemberEnd() || !rpc->value.IsArray())
        return false;
    for (const auto &row : rpc->value.GetArray()) {
        auto n = row.FindMember("name");
        if (n == row.MemberEnd() || !n->value.IsString()
            || strcmp(n->value.GetString(), name) != 0)
            continue;
        auto gs = [&](const char *k, char *dst, size_t cap) {
            auto f = row.FindMember(k);
            if (f != row.MemberEnd() && f->value.IsString())
                snprintf(dst, cap, "%s", f->value.GetString());
        };
        gs("url", v->rpc_url, sizeof(v->rpc_url));
        {
            tb_error_t e = TB_ERROR_INIT;
            auto f = row.FindMember("quoter");
            if (f != row.MemberEnd() && f->value.IsString())
                tb_addr_from_hex(&v->quoter, f->value.GetString(), &e);
            f = row.FindMember("router");
            if (f != row.MemberEnd() && f->value.IsString())
                tb_addr_from_hex(&v->router, f->value.GetString(), &e);
            f = row.FindMember("factory");
            if (f != row.MemberEnd() && f->value.IsString())
                tb_addr_from_hex(&v->factory, f->value.GetString(), &e);
            f = row.FindMember("weth");
            if (f != row.MemberEnd() && f->value.IsString())
                tb_addr_from_hex(&v->weth, f->value.GetString(), &e);
        }
        {
            auto f = row.FindMember("type");
            if (f != row.MemberEnd() && f->value.IsString()
                && strcmp(f->value.GetString(), "v2") == 0)
                v->kind = TB_DEX_V2;
        }
        {
            auto f = row.FindMember("chain_id");
            if (f != row.MemberEnd() && f->value.IsUint64())
                v->chain_id = f->value.GetUint64();
        }
        return v->rpc_url[0] != '\0';
    }
    return false;
}

static uint64_t cfg_chain_default(const rapidjson::Document &d)
{
    auto net = d.FindMember("network");
    if (net != d.MemberEnd() && net->value.IsObject()) {
        auto f = net->value.FindMember("chain_id");
        if (f != net->value.MemberEnd() && f->value.IsUint64())
            return f->value.GetUint64();
    }
    return 1;
}

static tb_addr_t cfg_weth(const rapidjson::Document &d)
{
    tb_addr_t w;
    memset(&w, 0, sizeof(w));
    auto f = d.FindMember("weth");
    if (f != d.MemberEnd() && f->value.IsString()) {
        tb_error_t e = TB_ERROR_INIT;
        tb_addr_from_hex(&w, f->value.GetString(), &e);
    }
    return w;
}

static std::string lower_str(const std::string &s)
{
    std::string r = s;
    for (char &c : r)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return r;
}

static int cmd_pool_discover(int argc, char **argv,
                             const std::string &cfg_path, int start)
{
    const char *dex_f = NULL, *sym_f = NULL;
    bool add = false;
    for (int i = start; i < argc; i++) {
        const char *v;
        if ((v = arg_flag(i, argc, argv, "--dex")))
            dex_f = v;
        else if ((v = arg_flag(i, argc, argv, "--symbol")))
            sym_f = v;
        else if (strcmp(argv[i], "--add") == 0)
            add = true;
        else {
            fprintf(stderr, "market pool discover: флаг %s\n", argv[i]);
            return 2;
        }
    }
    rapidjson::Document d;
    if (!cfg_load_doc(d, cfg_path))
        return 1;

    /* кандидаты-активы: лист активов + токены watchlist (уникальные) */
    struct cand {
        std::string sym;
        tb_addr_t addr;
        uint64_t chain;
    };
    std::vector<cand> cands;
    {
        auto assets = d.FindMember("assets");
        if (assets != d.MemberEnd() && assets->value.IsArray())
            for (const auto &a : assets->value.GetArray()) {
                auto s = a.FindMember("symbol");
                auto ad = a.FindMember("address");
                if (!s->value.IsString() || !ad->value.IsString())
                    continue;
                cand c;
                c.sym = s->value.GetString();
                tb_error_t e = TB_ERROR_INIT;
                if (tb_addr_from_hex(&c.addr, ad->value.GetString(), &e)
                    != 0)
                    continue;
                auto ch = a.FindMember("chain");
                c.chain = ch != a.MemberEnd() && ch->value.IsUint64()
                              ? ch->value.GetUint64()
                              : cfg_chain_default(d);
                auto en = a.FindMember("enabled");
                if (en != a.MemberEnd() && en->value.IsBool()
                    && !en->value.GetBool())
                    continue;
                cands.push_back(c);
            }
    }
    for (const pool_row &p : raw_pools(d)) {
        tb_addr_t t;
        tb_error_t e = TB_ERROR_INIT;
        if (tb_addr_from_hex(&t, p.token.c_str(), &e) != 0)
            continue;
        bool dup = false;
        for (const cand &c : cands)
            if (tb_addr_eq(&c.addr, &t))
                dup = true;
        if (!dup) {
            cand c;
            c.sym = p.token; /* символа нет — покажем адрес */
            c.addr = t;
            c.chain = cfg_chain_default(d);
            cands.push_back(c);
        }
    }
    if (sym_f) {
        std::vector<cand> f;
        for (const cand &c : cands)
            if (c.sym == sym_f || c.sym.find(sym_f) != std::string::npos)
                f.push_back(c);
        cands = f;
    }

    tb_error_t err = TB_ERROR_INIT;
    tb_http_t *http = tb_http_net_create("market-cli/0.1", NULL, &err);
    if (!http) {
        fprintf(stderr, "market: транспорт: %s\n", err.message);
        return 1;
    }

    /* биржи: из rpc[] (фильтр --dex); тир для v3 {100,500,3000,10000} */
    static const uint32_t v3_tiers[] = {100, 500, 3000, 10000};
    struct new_pool {
        std::string dex, token, pool;
        uint32_t fee;
    };
    std::vector<new_pool> to_add;
    int found_n = 0;
    auto rpc = d.FindMember("rpc");
    if (rpc == d.MemberEnd() || !rpc->value.IsArray()) {
        fprintf(stderr, "market: в конфиге нет rpc[]\n");
        tb_http_destroy(http);
        return 1;
    }
    for (const auto &row : rpc->value.GetArray()) {
        auto n = row.FindMember("name");
        if (!n->value.IsString())
            continue;
        std::string name = n->value.GetString();
        if (dex_f && name != dex_f)
            continue;
        tb_exec_dex_venue_t v;
        if (!venue_of(d, name.c_str(), &v)) {
            fprintf(stderr, "market: биржа %s без url — пропуск\n",
                    name.c_str());
            continue;
        }
        if (tb_addr_is_native(&v.factory)) {
            printf("  %s: нет factory — discover невозможен\n", name.c_str());
            continue;
        }
        uint64_t chain = v.chain_id ? v.chain_id : cfg_chain_default(d);
        if (tb_addr_is_native(&v.weth))
            v.weth = cfg_weth(d);
        for (const cand &c : cands) {
            if (c.chain != chain)
                continue;
            if (v.kind == TB_DEX_V2) {
                bool found = false;
                tb_addr_t pool;
                if (tb_exec_dex_find_pool(http, &v, &v.weth, &c.addr, 3000,
                                          &found, &pool,
                                          &err) != 0) {
                    printf("  %s %s: ошибка: %s\n", name.c_str(),
                           c.sym.c_str(), err.message);
                    tb_error_clear(&err);
                    continue;
                }
                if (found) {
                    found_n++;
                    char ph[43];
                    tb_addr_to_hex(&pool, ph, sizeof(ph));
                    bool watched = false;
                    for (const pool_row &p : raw_pools(d))
                        if (strcasecmp(p.pool.c_str(), ph) == 0)
                            watched = true;
                    printf("  %s %s v2 %s%s\n", name.c_str(),
                           c.sym.c_str(), ph,
                           watched ? "  [в watchlist]" : "");
                    if (add && !watched) {
                        new_pool np;
                        np.dex = name;
                        char tl[43];
                        tb_addr_to_hex(&c.addr, tl, sizeof(tl));
                        for (char *p = tl; *p; p++)
                            if (*p >= 'A' && *p <= 'Z')
                                *p += 'a' - 'A';
                        np.token = tl;
                        np.pool = ph;
                        np.fee = 3000;
                        to_add.push_back(np);
                    }
                }
            } else {
                for (uint32_t fee : v3_tiers) {
                    bool found = false;
                    tb_addr_t pool;
                    if (tb_exec_dex_find_pool(http, &v, &v.weth, &c.addr,
                                              fee, &found, &pool,
                                              &err) != 0) {
                        printf("  %s %s fee=%u: ошибка: %s\n",
                               name.c_str(), c.sym.c_str(), fee,
                               err.message);
                        tb_error_clear(&err);
                        break;
                    }
                    if (!found)
                        continue;
                    found_n++;
                    char ph[43];
                    tb_addr_to_hex(&pool, ph, sizeof(ph));
                    /* в watchlist? */
                    bool watched = false;
                    for (const pool_row &p : raw_pools(d))
                        if (strcasecmp(p.pool.c_str(), ph) == 0)
                            watched = true;
                    printf("  %s %s fee=%u %s%s\n", name.c_str(),
                           c.sym.c_str(), fee, ph,
                           watched ? "  [в watchlist]" : "");
                    if (add && !watched) {
                        new_pool np;
                        np.dex = name;
                        /* токен — адрес (не символ): нижний регистр */
                        char tl[43];
                        tb_addr_to_hex(&c.addr, tl, sizeof(tl));
                        for (char *p = tl; *p; p++)
                            if (*p >= 'A' && *p <= 'Z')
                                *p += 'a' - 'A';
                        np.token = tl;
                        np.pool = ph;
                        np.fee = fee;
                        to_add.push_back(np);
                    }
                }
            }
        }
    }
    tb_http_destroy(http);
    printf("market: найдено пулов: %d\n", found_n);

    if (add && !to_add.empty()) {
        snapshot(cfg_path, "pool-discover-add");
        Value &pools = array_get(d, "pools");
        auto &al = d.GetAllocator();
        for (const new_pool &np : to_add) {
            if (find_row_addr_icase(pools, "pool", np.pool.c_str()))
                continue;
            Value row(rapidjson::kObjectType);
            cell_str(row, "token", np.token.c_str(), al);
            cell_str(row, "pool", np.pool.c_str(), al);
            cell_u64(row, "fee", np.fee, al);
            cell_str(row, "dex", np.dex.c_str(), al);
            Value k("enabled", (rapidjson::SizeType)strlen("enabled"), al);
            row.AddMember(k, Value(false), al); /* включать явно */
            pools.PushBack(row, al);
        }
        if (!cfg_save_doc(d, cfg_path))
            return 1;
        printf("market: добавлено пулов (выключены, включай явно): %zu\n",
               to_add.size());
    }
    return 0;
}

static int cmd_pool(int argc, char **argv, const std::string &cfg_path,
                    int start)
{
    if (start >= argc) {
        fprintf(stderr,
                "market pool: discover… | inspect 0x… | enable 0x… | "
                "disable 0x…\n");
        return 2;
    }
    const char *op = argv[start];

    if (strcmp(op, "discover") == 0)
        return cmd_pool_discover(argc, argv, cfg_path, start + 1);

    if (start + 1 >= argc) {
        fprintf(stderr, "market pool %s 0xАДРЕС\n", op);
        return 2;
    }
    const char *addr_s = argv[start + 1];
    tb_addr_t addr;
    if (!parse_addr(addr_s, &addr))
        return 1;
    char low[43];
    addr_lower(&addr, low);

    rapidjson::Document d;
    if (!cfg_load_doc(d, cfg_path))
        return 1;
    Value &pools = array_get(d, "pools");
    Value *row = find_row_addr_icase(pools, "pool", low);

    if (strcmp(op, "inspect") == 0) {
        if (!row) {
            printf("market: пул %s в конфиге не значится\n", low);
            return 1;
        }
        auto gs = [&](const char *k) -> std::string {
            auto f = row->FindMember(k);
            return f != row->MemberEnd() && f->value.IsString()
                       ? f->value.GetString()
                       : "-";
        };
        auto gf = row->FindMember("fee");
        uint32_t fee = gf != row->MemberEnd() && gf->value.IsUint()
                           ? gf->value.GetUint()
                           : 0;
        auto ge = row->FindMember("enabled");
        bool en = !(ge != row->MemberEnd() && ge->value.IsBool()
                    && !ge->value.GetBool());
        printf("пул     : %s\nбиржа   : %s\nтокен   : %s\nfee     : %u "
               "(%u.%04u%%)\nактивен : %s\nприм.   : %s\n",
               low, gs("dex").c_str(), gs("token").c_str(), fee,
               fee / 10000, fee % 10000, en ? "да" : "нет",
               gs("note").c_str());

        /* цепная проверка: фабрика знает пул? */
        tb_exec_dex_venue_t v;
        if (venue_of(d, gs("dex").c_str(), &v)) {
            if (tb_addr_is_native(&v.factory)) {
                printf("фабрика : у биржи не задана — не проверяем\n");
                return 0;
            }
            uint64_t chain = v.chain_id ? v.chain_id : cfg_chain_default(d);
            if (tb_addr_is_native(&v.weth))
                v.weth = cfg_weth(d);
            tb_addr_t token;
            tb_error_t e = TB_ERROR_INIT;
            if (tb_addr_from_hex(&token, gs("token").c_str(), &e) != 0) {
                printf("токен   : не разобрать адрес\n");
                return 0;
            }
            tb_error_t err = TB_ERROR_INIT;
            tb_http_t *http =
                tb_http_net_create("market-cli/0.1", NULL, &err);
            if (!http) {
                printf("фабрика : RPC недоступен (%s)\n", err.message);
                return 0;
            }
            bool found = false;
            tb_addr_t got;
            int rc = tb_exec_dex_find_pool(http, &v, &v.weth, &token, fee,
                                           &found, &got, &err);
            tb_http_destroy(http);
            if (rc != 0) {
                printf("фабрика : ошибка %s\n", err.message);
                return 0;
            }
            if (!found) {
                printf("фабрика : пула НЕТ — запись устарела "
                       "(getPool вернул 0x0)\n");
                return 0;
            }
            char gh[43];
            tb_addr_to_hex(&got, gh, sizeof(gh));
            printf("фабрика : %s %s\n", gh,
                   strcasecmp(gh, low) == 0 ? "— совпало" : "— РАСХОЖДЕНИЕ");
        } else {
            printf("фабрика : биржа '%s' без url — не проверяем\n",
                   gs("dex").c_str());
        }
        return 0;
    }

    if (strcmp(op, "enable") == 0 || strcmp(op, "disable") == 0) {
        if (!row) {
            fprintf(stderr, "market: пула %s нет в конфиге\n", low);
            return 1;
        }
        snapshot(cfg_path, op);
        bool en = op[0] == 'e';
        row->RemoveMember("enabled");
        Value k("enabled", (rapidjson::SizeType)strlen("enabled"),
                d.GetAllocator());
        row->AddMember(k, Value(en), d.GetAllocator());
        if (!cfg_save_doc(d, cfg_path))
            return 1;
        printf("market: пул %s %s\n", low,
               en ? "включён (в watchlist)" : "выключен (в конфиге, бот "
                                              "не смотрит)");
        return 0;
    }

    fprintf(stderr, "market pool: неизвестная операция '%s'\n", op);
    return 2;
}

static int cmd_config(int argc, char **argv, const std::string &cfg_path,
                      int start)
{
    if (start >= argc) {
        fprintf(stderr,
                "market config: validate [--verify] | diff [A B] | "
                "rollback [--step N] [--list]\n");
        return 2;
    }
    const char *op = argv[start++];

    if (strcmp(op, "validate") == 0) {
        bool verify = false;
        for (int i = start; i < argc; i++)
            if (strcmp(argv[i], "--verify") == 0)
                verify = true;
        tb_arb_cfg_t cfg;
        tb_error_t err = TB_ERROR_INIT;
        if (tb_arb_cfg_load(&cfg, cfg_path.c_str(), &err) != 0) {
            fprintf(stderr, "market: конфиг не грузится: %s\n",
                    err.message);
            return 1;
        }
        tb_arb_defaults(&cfg);
        if (tb_arb_cfg_validate(&cfg, &err) != 0) {
            fprintf(stderr, "market: конфиг невалиден: %s\n", err.message);
            return 1;
        }
        printf("market: конфиг валиден (%u пулов, %u бирж, %u активов)\n",
               cfg.pools_n, cfg.dex.venues_n, cfg.assets_n);
        if (verify) {
            tb_error_t verr = TB_ERROR_INIT;
            tb_http_t *http = tb_http_net_create("market-cli/0.1", NULL,
                                                 &verr);
            if (!http) {
                fprintf(stderr, "market: транспорт: %s\n", verr.message);
                return 1;
            }
            printf("market: сверка пулов с фабриками на цепи…\n");
            int rc = tb_arb_cfg_verify(&cfg, http, &verr);
            tb_http_destroy(http);
            if (rc != 0) {
                fprintf(stderr, "market: %s\n", verr.message);
                return 1;
            }
            printf("market: все пулы подтверждены фабриками\n");
        }
        return 0;
    }

    if (strcmp(op, "diff") == 0) {
        std::string a = cfg_path, b;
        if (start < argc)
            a = argv[start++];
        if (start < argc)
            b = argv[start++];
        else {
            /* по умолчанию: текущий vs последний снапшот */
            auto snaps = snapshot_list(cfg_path);
            if (snaps.empty()) {
                printf("market: снапшотов нет — сравнить не с чем\n");
                return 0;
            }
            b = history_dir(cfg_path) + "/" + snaps.back();
        }
        rapidjson::Document da, db;
        if (!cfg_load_doc(da, a) || !cfg_load_doc(db, b))
            return 1;
        std::map<std::string, std::string> ma, mb;
        flatten(da, "", ma);
        flatten(db, "", mb);
        int diffs = 0;
        for (const auto &kv : ma) {
            auto it = mb.find(kv.first);
            if (it == mb.end()) {
                printf("+ %s = %s (в '%s' отсутствует)\n", kv.first.c_str(),
                       kv.second.c_str(), b.c_str());
                diffs++;
            } else if (it->second != kv.second) {
                printf("~ %s: %s -> %s\n", kv.first.c_str(),
                       it->second.c_str(), kv.second.c_str());
                diffs++;
            }
        }
        for (const auto &kv : mb)
            if (ma.find(kv.first) == ma.end()) {
                printf("- %s = %s (было в '%s')\n", kv.first.c_str(),
                       kv.second.c_str(), b.c_str());
                diffs++;
            }
        printf("market: ячеек изменено: %d (%s vs %s)\n", diffs, a.c_str(),
               b.c_str());
        return 0;
    }

    if (strcmp(op, "rollback") == 0) {
        int step = 1;
        bool list = false;
        for (int i = start; i < argc; i++) {
            const char *v;
            if ((v = arg_flag(i, argc, argv, "--step")))
                step = (int)strtoul(v, NULL, 10);
            else if (strcmp(argv[i], "--list") == 0)
                list = true;
        }
        auto snaps = snapshot_list(cfg_path);
        if (list) {
            for (const auto &s : snaps)
                printf("  %s\n", s.c_str());
            printf("market: снапшотов: %zu\n", snaps.size());
            return 0;
        }
        if (snaps.size() < (size_t)step || step < 1) {
            fprintf(stderr, "market: нет снапшота с --step %d\n", step);
            return 1;
        }
        const std::string &pick = snaps[snaps.size() - (size_t)step];
        std::string src = history_dir(cfg_path) + "/" + pick;
        snapshot(cfg_path, "rollback"); /* текущее состояние — тоже в историю */
        rapidjson::Document d;
        if (!cfg_load_doc(d, src) || !cfg_save_doc(d, cfg_path))
            return 1;
        printf("market: конфиг восстановлен из %s\n", pick.c_str());
        return 0;
    }

    fprintf(stderr, "market config: неизвестная операция '%s'\n", op);
    return 2;
}

/* ================= точка входа ================= */

int market_main(int argc, char **argv)
{
    std::string cfg_path = getenv("MARKET_CONFIG") ? getenv("MARKET_CONFIG")
                                                   : "arb_config.json";
    int start = 1;
    if (start < argc && strcmp(argv[start], "--config") == 0
        && start + 1 < argc) {
        cfg_path = argv[start + 1];
        start += 2;
    }
    if (start >= argc) {
        printf("market — менеджер конфигурации рынка (ячейки, снапшоты)\n"
               "  market dex    add|update|logo set\n"
               "  market asset  add|logo set\n"
               "  market pool   discover|inspect|enable|disable\n"
               "  market config validate|diff|rollback\n"
               "  --config PATH (или env MARKET_CONFIG)\n");
        return 0;
    }
    const char *cmd = argv[start++];
    if (strcmp(cmd, "dex") == 0)
        return cmd_dex(argc, argv, cfg_path, start);
    if (strcmp(cmd, "asset") == 0)
        return cmd_asset(argc, argv, cfg_path, start);
    if (strcmp(cmd, "pool") == 0)
        return cmd_pool(argc, argv, cfg_path, start);
    if (strcmp(cmd, "config") == 0)
        return cmd_config(argc, argv, cfg_path, start);
    fprintf(stderr, "market: неизвестная команда '%s'\n", cmd);
    return 2;
}

#ifndef MARKET_NO_MAIN
int main(int argc, char **argv)
{
    return market_main(argc, argv);
}
#endif
