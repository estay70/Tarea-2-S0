
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using std::string;
using std::vector;


static std::atomic<int> g_stop{0};           
static std::atomic_flag g_sigint_anunciado = ATOMIC_FLAG_INIT;

static void on_sigint(int) { g_stop.store(1); }

static long now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}


static void init_cond(pthread_cond_t* cv) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(cv, &a);
    pthread_condattr_destroy(&a);
}


static void cond_wait_ms(pthread_cond_t* cv, pthread_mutex_t* mx, long ms) {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(cv, mx, &ts);
}


class Logger {
public:
    void abrir(const string& path, bool consola) {
        consola_ = consola;
        t0_ = now_ms();
        f_ = fopen(path.c_str(), "w");
        if (!f_) perror("No se pudo abrir el archivo de log");
    }
    void cerrar() {
        if (f_) fclose(f_);
        f_ = nullptr;
    }
    void msg(int mesa, const char* fmt, ...) __attribute__((format(printf, 3, 4))) {
        char buf[768];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        char pre[64];
        if (mesa >= 0)
            snprintf(pre, sizeof pre, "[%6ld ms] [Mesa %2d] ", now_ms() - t0_, mesa);
        else
            snprintf(pre, sizeof pre, "[%6ld ms] [Torneo]  ", now_ms() - t0_);
        pthread_mutex_lock(&mx_);
        if (consola_) {
            fprintf(stdout, "%s%s\n", pre, buf);
            fflush(stdout);
        }
        if (f_) {
            fprintf(f_, "%s%s\n", pre, buf);
            fflush(f_);
        }
        pthread_mutex_unlock(&mx_);
    }
    void bloque(const string& s) {
        pthread_mutex_lock(&mx_);
        if (consola_) {
            fputs(s.c_str(), stdout);
            fflush(stdout);
        }
        if (f_) {
            fputs(s.c_str(), f_);
            fflush(f_);
        }
        pthread_mutex_unlock(&mx_);
    }

private:
    pthread_mutex_t mx_ = PTHREAD_MUTEX_INITIALIZER;
    FILE* f_ = nullptr;
    bool consola_ = true;
    long t0_ = 0;
};
static Logger LOG;

static void anunciar_sigint() {
    if (g_stop.load() && !g_sigint_anunciado.test_and_set())
        LOG.msg(-1, "SIGINT recibido: no entran mas jugadores; las partidas activas se cierran ordenadamente");
}


constexpr int NG = 5;             
constexpr int APEX = 25;          
constexpr int NNODOS = 26;
constexpr int PUMA_ID = 12;       
constexpr int MAX_PERROS = 12;
constexpr int NPIEZAS = 13;
constexpr int8_t LIBRE = -1;

struct Topologia {
    vector<int> adj[NNODOS];
    vector<std::pair<int, int>> saltos[NNODOS];  
    int fila[NNODOS];
    int col[NNODOS];
};
static Topologia TOPO;

static void construir_topologia(bool diagonales) {
    for (int n = 0; n < NNODOS; ++n) {
        TOPO.adj[n].clear();
        TOPO.saltos[n].clear();
    }
    for (int r = 0; r < NG; ++r)
        for (int c = 0; c < NG; ++c) {
            TOPO.fila[r * NG + c] = r;
            TOPO.col[r * NG + c] = c;
        }
    TOPO.fila[APEX] = -1;
    TOPO.col[APEX] = 2;

    for (int r = 0; r < NG; ++r)
        for (int c = 0; c < NG; ++c) {
            const int n = r * NG + c;
            vector<std::pair<int, int>> dirs = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            if (diagonales && (r + c) % 2 == 0) {
                dirs.push_back({-1, -1});
                dirs.push_back({-1, 1});
                dirs.push_back({1, -1});
                dirs.push_back({1, 1});
            }
            for (auto d : dirs) {
                const int r1 = r + d.first, c1 = c + d.second;
                if (r1 < 0 || r1 >= NG || c1 < 0 || c1 >= NG) continue;
                const int m = r1 * NG + c1;
                TOPO.adj[n].push_back(m);
                const int r2 = r1 + d.first, c2 = c1 + d.second;
                if (r2 >= 0 && r2 < NG && c2 >= 0 && c2 < NG)
                    TOPO.saltos[n].push_back({m, r2 * NG + c2});
            }
        }
    
    for (int c : {0, 2, 4}) {
        TOPO.adj[APEX].push_back(c);
        TOPO.adj[c].push_back(APEX);
    }

    TOPO.saltos[APEX].push_back({2, 7});
    TOPO.saltos[7].push_back({2, APEX});
}

static int dist(int a, int b) {
    return std::abs(TOPO.fila[a] - TOPO.fila[b]) + std::abs(TOPO.col[a] - TOPO.col[b]);
}

static string nombre_nodo(int n) {
    if (n == APEX) return "T";
    char b[16];
    snprintf(b, sizeof b, "(%d,%d)", TOPO.fila[n], TOPO.col[n]);
    return b;
}


struct Config {
    unsigned seed = 42;
    int retardo_min = 50;
    int retardo_max = 300;
    bool diagonales = true;
    int puma_pos = APEX;
    vector<int> perros = {15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 10, 14};  
    int capturas = 6;
    int max_movs = 1500;            
    int imprimir_cada = 10;         
    int timeout_abandono = 1500;    
    int llegada_min = 0;
    int llegada_max = 150;
    vector<int> fallo_jugadores;    
    int fallo_tras = 5;             
    bool fallo_silencio = false;    
    bool consola = true;
    string log_archivo = "torneo.log";
};

static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool parse_pos(const string& tok, int& out) {
    if (tok == "T" || tok == "t") {
        out = APEX;
        return true;
    }
    int r, c;
    char extra;
    if (sscanf(tok.c_str(), "%d,%d%c", &r, &c, &extra) == 2 && r >= 0 && r < NG && c >= 0 && c < NG) {
        out = r * NG + c;
        return true;
    }
    return false;
}

static bool cargar_config(const string& path, Config& cfg, string& err) {
    std::ifstream in(path);
    if (!in) {
        err = "no se puede abrir '" + path + "'";
        return false;
    }
    string linea;
    int nl = 0;
    while (std::getline(in, linea)) {
        ++nl;
        size_t h = linea.find('#');
        if (h != string::npos) linea.erase(h);
        linea = trim(linea);
        if (linea.empty()) continue;
        size_t eq = linea.find('=');
        if (eq == string::npos) {
            err = "linea " + std::to_string(nl) + ": se esperaba clave = valor";
            return false;
        }
        const string k = trim(linea.substr(0, eq));
        const string v = trim(linea.substr(eq + 1));
        auto num = [&](int& dst) -> bool {
            char* end = nullptr;
            long x = strtol(v.c_str(), &end, 10);
            if (v.empty() || *end != '\0') return false;
            dst = static_cast<int>(x);
            return true;
        };
        bool ok = true;
        if (k == "seed") {
            int x = 0;
            ok = num(x);
            cfg.seed = static_cast<unsigned>(x);
        } else if (k == "retardo_min_ms") ok = num(cfg.retardo_min);
        else if (k == "retardo_max_ms") ok = num(cfg.retardo_max);
        else if (k == "diagonales") {
            int x = 1;
            ok = num(x);
            cfg.diagonales = (x != 0);
        } else if (k == "puma") ok = parse_pos(v, cfg.puma_pos);
        else if (k == "perros") {
            cfg.perros.clear();
            std::istringstream ss(v);
            string tok;
            while (ss >> tok) {
                int p;
                if (!parse_pos(tok, p)) {
                    ok = false;
                    break;
                }
                cfg.perros.push_back(p);
            }
        } else if (k == "capturas_victoria") ok = num(cfg.capturas);
        else if (k == "max_movimientos") ok = num(cfg.max_movs);
        else if (k == "imprimir_cada") ok = num(cfg.imprimir_cada);
        else if (k == "timeout_abandono_ms") ok = num(cfg.timeout_abandono);
        else if (k == "llegada_min_ms") ok = num(cfg.llegada_min);
        else if (k == "llegada_max_ms") ok = num(cfg.llegada_max);
        else if (k == "fallo_tras_movs") ok = num(cfg.fallo_tras);
        else if (k == "fallo_modo") {
            if (v == "muerte") cfg.fallo_silencio = false;
            else if (v == "silencio") cfg.fallo_silencio = true;
            else ok = false;
        } else if (k == "fallo_jugadores") {
            cfg.fallo_jugadores.clear();
            string t = v;
            std::replace(t.begin(), t.end(), ',', ' ');
            std::istringstream ss(t);
            int x;
            while (ss >> x) cfg.fallo_jugadores.push_back(x);
        } else if (k == "consola") {
            int x = 1;
            ok = num(x);
            cfg.consola = (x != 0);
        } else if (k == "log_archivo") cfg.log_archivo = v;
        else {
            err = "linea " + std::to_string(nl) + ": clave desconocida '" + k + "'";
            return false;
        }
        if (!ok) {
            err = "linea " + std::to_string(nl) + ": valor invalido para '" + k + "'";
            return false;
        }
    }
    return true;
}

static bool validar_config(const Config& c, string& err) {
    if (c.retardo_min < 0 || c.retardo_max < c.retardo_min) {
        err = "retardos invalidos (0 <= min <= max)";
        return false;
    }
    if (c.perros.empty() || c.perros.size() > static_cast<size_t>(MAX_PERROS)) {
        err = "debe haber entre 1 y 12 perros";
        return false;
    }
    vector<int> usados = c.perros;
    usados.push_back(c.puma_pos);
    std::sort(usados.begin(), usados.end());
    if (std::adjacent_find(usados.begin(), usados.end()) != usados.end()) {
        err = "dos fichas comparten casilla inicial";
        return false;
    }
    if (c.capturas < 1 || c.capturas > static_cast<int>(c.perros.size())) {
        err = "capturas_victoria debe estar entre 1 y la cantidad de perros";
        return false;
    }
    if (c.max_movs < 1 || c.imprimir_cada < 1) {
        err = "max_movimientos e imprimir_cada deben ser >= 1";
        return false;
    }
    if (c.llegada_min < 0 || c.llegada_max < c.llegada_min) {
        err = "llegadas invalidas (0 <= min <= max)";
        return false;
    }
    if (c.timeout_abandono < 2 * c.retardo_max + 200) {
        err = "timeout_abandono_ms debe ser >= 2*retardo_max_ms + 200 (evita falsos abandonos)";
        return false;
    }
    if (c.fallo_tras < 1) {
        err = "fallo_tras_movs debe ser >= 1";
        return false;
    }
    return true;
}



struct Estado {
    int8_t cell[NNODOS];  
    int pos[NPIEZAS];
    bool viva[NPIEZAS];
    int capturas = 0;
    int movs = 0;
};

struct Mov {
    int to;
    int over;  
};

enum Resultado {
    EN_CURSO = 0,
    GANA_PUMA,
    GANAN_PERROS,
    ABANDONO_PUMA,    
    ABANDONO_PERROS,  
    INTERRUMPIDA,     
    TABLAS,           
    N_RESULTADOS
};

static string texto_resultado(int r) {
    switch (r) {
        case GANA_PUMA: return "GANA EL PUMA (capturas suficientes)";
        case GANAN_PERROS: return "GANAN LOS PERROS (puma acorralado)";
        case ABANDONO_PUMA: return "ABANDONO del Puma -> ganan los Perros por abandono";
        case ABANDONO_PERROS: return "ABANDONO de los Perros -> gana el Puma por abandono";
        case INTERRUMPIDA: return "PARTIDA INTERRUMPIDA (SIGINT)";
        case TABLAS: return "TABLAS (tope de movimientos)";
        default: return "EN CURSO";
    }
}

static void estado_inicial(Estado& s, const Config& cfg) {
    for (int i = 0; i < NNODOS; ++i) s.cell[i] = LIBRE;
    for (int i = 0; i < NPIEZAS; ++i) {
        s.pos[i] = -1;
        s.viva[i] = false;
    }
    for (size_t i = 0; i < cfg.perros.size(); ++i) {
        s.pos[i] = cfg.perros[i];
        s.viva[i] = true;
        s.cell[cfg.perros[i]] = static_cast<int8_t>(i);
    }
    s.pos[PUMA_ID] = cfg.puma_pos;
    s.viva[PUMA_ID] = true;
    s.cell[cfg.puma_pos] = PUMA_ID;
    s.capturas = 0;
    s.movs = 0;
}

static void movs_validos(const Estado& s, int pieza, vector<Mov>& out) {
    out.clear();
    const int p = s.pos[pieza];
    for (int n : TOPO.adj[p])
        if (s.cell[n] == LIBRE) out.push_back({n, -1});
    if (pieza == PUMA_ID) {
        for (const auto& sj : TOPO.saltos[p]) {
            const int c = s.cell[sj.first];
            if (c >= 0 && c < PUMA_ID && s.cell[sj.second] == LIBRE) out.push_back({sj.second, sj.first});
        }
    }
}

static string tablero_str(const Estado& s) {
    auto sim = [&](int n) -> char {
        const int c = s.cell[n];
        if (c == LIBRE) return '.';
        return c == PUMA_ID ? 'P' : 'D';
    };
    string o;
    o += string("                T[") + sim(APEX) + "]\n";
    o += "            c0 c1 c2 c3 c4\n";
    for (int r = 0; r < NG; ++r) {
        char b[64];
        snprintf(b, sizeof b, "        f%d   %c  %c  %c  %c  %c\n", r, sim(r * NG), sim(r * NG + 1),
                 sim(r * NG + 2), sim(r * NG + 3), sim(r * NG + 4));
        o += b;
    }
    return o;
}


struct Servidor;
struct Mesa;

struct Cliente {
    int id = 0;
    int rol = 0;  
    Servidor* srv = nullptr;
    pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv;
    Mesa* mesa = nullptr;
    bool cancelado = false;
    Cliente() { init_cond(&cv); }
    ~Cliente() {
        pthread_cond_destroy(&cv);
        pthread_mutex_destroy(&mx);
    }
};

struct Mesa {
    int id = 0;
    const Config* cfg = nullptr;
    Servidor* srv = nullptr;
    Cliente* jug[2] = {nullptr, nullptr};  

    Estado st;                  
    pthread_mutex_t board_mx;   
    pthread_mutex_t meta_mx;    
    pthread_cond_t meta_cv;     
    int activas = 0;            

    std::atomic<int> resultado{EN_CURSO};
    std::atomic<long> beat[2];            
    std::atomic<int> movs_lado[2];
    std::atomic<bool> muerto[2];          
    std::atomic<bool> caida[2];           
    std::atomic<int> colisiones{0};
};

struct Servidor {
    const Config* cfg = nullptr;
    int N = 0, M = 0;
    sem_t mesas;                          
    pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv;
    std::deque<Cliente*> cola[2];
    bool cerrado = false;                 
    int pares_totales = 0;
    std::atomic<int> pares_formados{0};
    std::atomic<int> mesas_en_uso{0};
    std::atomic<int> max_mesas{0};
    std::atomic<int> mesa_seq{0};
    std::atomic<int> res[N_RESULTADOS];
    std::atomic<long> movs_total{0};
    std::atomic<long> colisiones_total{0};
};
static void lock_board(Mesa& m) {
    int r = pthread_mutex_lock(&m.board_mx);
    if (r == EOWNERDEAD) {
        pthread_mutex_consistent(&m.board_mx);
        LOG.msg(m.id, "Mutex del tablero RECUPERADO tras la caida de su dueno (EOWNERDEAD)");
    } else if (r != 0) {
        LOG.msg(m.id, "ERROR fatal al tomar el tablero: %s", strerror(r));
        abort();
    }
}
struct BoardLock {
    Mesa& m;
    explicit BoardLock(Mesa& mm) : m(mm) { lock_board(m); }
    ~BoardLock() { pthread_mutex_unlock(&m.board_mx); }
    BoardLock(const BoardLock&) = delete;
    BoardLock& operator=(const BoardLock&) = delete;
};
static void mesa_init(Mesa& m) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&m.board_mx, &a);
    pthread_mutexattr_destroy(&a);
    pthread_mutex_init(&m.meta_mx, nullptr);
    init_cond(&m.meta_cv);
    estado_inicial(m.st, *m.cfg);
    m.activas = 1 + static_cast<int>(m.cfg->perros.size());
    m.beat[0] = m.beat[1] = now_ms();
    m.movs_lado[0] = m.movs_lado[1] = 0;
    m.muerto[0] = m.muerto[1] = false;
    m.caida[0] = m.caida[1] = false;
}
static void mesa_destroy(Mesa& m) {
    pthread_cond_destroy(&m.meta_cv);
    pthread_mutex_destroy(&m.meta_mx);
    pthread_mutex_destroy(&m.board_mx);
}
static bool finalizar(Mesa& m, int res) {
    int esperado = EN_CURSO;
    if (!m.resultado.compare_exchange_strong(esperado, res)) return false;
    pthread_mutex_lock(&m.meta_mx);  // orden: board_mx -> meta_mx
    pthread_cond_broadcast(&m.meta_cv);
    pthread_mutex_unlock(&m.meta_mx);
    return true;
}
static void esperar(Mesa& m, long ms) {
    const long limite = now_ms() + ms;
    pthread_mutex_lock(&m.meta_mx);
    while (m.resultado.load() == EN_CURSO) {
        const long resto = limite - now_ms();
        if (resto <= 0) break;
        cond_wait_ms(&m.meta_cv, &m.meta_mx, resto);
    }
    pthread_mutex_unlock(&m.meta_mx);
}
static void pieza_termina(Mesa& m) {
    pthread_mutex_lock(&m.meta_mx);
    --m.activas;
    pthread_cond_broadcast(&m.meta_cv);
    pthread_mutex_unlock(&m.meta_mx);
}
enum { MOV_OK, MOV_CHOQUE, MOV_FIN };
static string nombre_pieza(int p) { return p == PUMA_ID ? string("Puma") : "Perro " + std::to_string(p + 1); }
static int intentar(Mesa& m, int pieza, Mov mv) {
    BoardLock g(m);
    if (m.resultado.load() != EN_CURSO) return MOV_FIN;
    Estado& s = m.st;
    if (!s.viva[pieza]) return MOV_FIN;
    vector<Mov> v;
    movs_validos(s, pieza, v);  
    const bool legal = std::any_of(v.begin(), v.end(), [&](const Mov& x) { return x.to == mv.to && x.over == mv.over; });
    if (!legal) {
        m.colisiones.fetch_add(1);
        LOG.msg(m.id, "%s pierde la carrera por %s (casilla ya ocupada) -> colision", nombre_pieza(pieza).c_str(),
                nombre_nodo(mv.to).c_str());
        return MOV_CHOQUE;
    }
    const int from = s.pos[pieza];
    s.cell[from] = LIBRE;
    s.cell[mv.to] = static_cast<int8_t>(pieza);
    s.pos[pieza] = mv.to;
    ++s.movs;
    if (mv.over >= 0) {
        const int dog = s.cell[mv.over];
        s.cell[mv.over] = LIBRE;
        s.viva[dog] = false;
        ++s.capturas;
        LOG.msg(m.id, "Puma salta %s -> %s y captura a %s en %s [capturas=%d/%d]", nombre_nodo(from).c_str(),
                nombre_nodo(mv.to).c_str(), nombre_pieza(dog).c_str(), nombre_nodo(mv.over).c_str(), s.capturas,
                m.cfg->capturas);
    } else {
        LOG.msg(m.id, "%s: %s -> %s", nombre_pieza(pieza).c_str(), nombre_nodo(from).c_str(),
                nombre_nodo(mv.to).c_str());
    }
    bool terminal = false;
    if (s.capturas >= m.cfg->capturas) {
        terminal = finalizar(m, GANA_PUMA);
    } else {
        vector<Mov> vp;
        movs_validos(s, PUMA_ID, vp);
        if (vp.empty()) terminal = finalizar(m, GANAN_PERROS);
        else if (s.movs >= m.cfg->max_movs) terminal = finalizar(m, TABLAS);
    }
    if (!terminal && s.movs % m.cfg->imprimir_cada == 0)
        LOG.bloque("Tablero (mov " + std::to_string(s.movs) + "):\n" + tablero_str(s));
    return MOV_OK;
}
static bool expuesto(const Estado& t, int nodo) {
    const int pp = t.pos[PUMA_ID];
    for (const auto& sj : TOPO.saltos[pp])
        if (sj.first == nodo && t.cell[sj.second] == LIBRE) return true;
    return false;
}
static Mov elegir_puma(const Estado& s, const vector<Mov>& mv, std::mt19937& rng) {
    vector<Mov> saltos;
    for (const Mov& x : mv)
        if (x.over >= 0) saltos.push_back(x);
    if (!saltos.empty()) return saltos[rng() % saltos.size()];
    if (rng() % 100 < 70) {
        int mejor = 1 << 30;
        vector<Mov> emp;
        for (const Mov& x : mv) {
            int d = 1 << 30;
            for (int i = 0; i < MAX_PERROS; ++i)
                if (s.viva[i]) d = std::min(d, dist(x.to, s.pos[i]));
            if (d < mejor) {
                mejor = d;
                emp.clear();
            }
            if (d == mejor) emp.push_back(x);
        }
        if (!emp.empty()) return emp[rng() % emp.size()];
    }
    return mv[rng() % mv.size()];
}
static Mov elegir_perro(const Estado& s, int pieza, const vector<Mov>& mv, std::mt19937& rng) {
    vector<Mov> seguros;
    for (const Mov& x : mv) {
        Estado t = s;
        t.cell[t.pos[pieza]] = LIBRE;
        t.cell[x.to] = static_cast<int8_t>(pieza);
        t.pos[pieza] = x.to;
        if (!expuesto(t, x.to)) seguros.push_back(x);
    }
    const vector<Mov>& pool = seguros.empty() ? mv : seguros;
    if (rng() % 100 < 75) {
        int mejor = 1 << 30;
        vector<Mov> emp;
        for (const Mov& x : pool) {
            const int d = dist(x.to, s.pos[PUMA_ID]);
            if (d < mejor) {
                mejor = d;
                emp.clear();
            }
            if (d == mejor) emp.push_back(x);
        }
        return emp[rng() % emp.size()];
    }
    return pool[rng() % pool.size()];
}
struct PiezaArg {
    Mesa* m;
    int pieza;
    int lado;      
    bool inyectar; 
    unsigned seed;
};
static void* pieza_main(void* p) {
    PiezaArg* a = static_cast<PiezaArg*>(p);
    Mesa& m = *a->m;
    const Config& cfg = *m.cfg;
    std::mt19937 rng(a->seed);
    std::uniform_int_distribution<int> retardo(cfg.retardo_min, cfg.retardo_max);
    vector<Mov> mv;
    while (true) {
        esperar(m, retardo(rng));  
        if (m.resultado.load() != EN_CURSO) break;
        if (m.muerto[a->lado].load()) {
            while (m.resultado.load() == EN_CURSO) esperar(m, 200);
            break;
        }
        m.beat[a->lado].store(now_ms());

        Estado s;
        {
            BoardLock g(m);  
            s = m.st;
        }
        if (!s.viva[a->pieza]) break;  
        movs_validos(s, a->pieza, mv);
        if (mv.empty()) continue;
        const Mov eleg = (a->pieza == PUMA_ID) ? elegir_puma(s, mv, rng) : elegir_perro(s, a->pieza, mv, rng);

        const int r = intentar(m, a->pieza, eleg);
        if (r == MOV_FIN) {
            if (m.resultado.load() != EN_CURSO) break;
            continue;
        }
        if (r == MOV_OK) {
            const int k = m.movs_lado[a->lado].fetch_add(1) + 1;
            if (a->inyectar && k == cfg.fallo_tras) {
                m.muerto[a->lado].store(true);
                if (cfg.fallo_silencio) {
                    LOG.msg(m.id, "!! %s (jugador cliente) se CUELGA: deja de responder sin morir",
                            a->lado == 0 ? "Puma" : "Perros");
                    continue;
                }
                lock_board(m);
                LOG.msg(m.id, "!! %s (jugador cliente) FALLA y muere reteniendo el mutex del tablero",
                        a->lado == 0 ? "Puma" : "Perros");
                m.caida[a->lado].store(true);  
                pieza_termina(m);
                return nullptr;  
            }
        }
    }
    pieza_termina(m);
    return nullptr;
}
static void asignar(Cliente* c, Mesa* m) {
    pthread_mutex_lock(&c->mx);
    c->mesa = m;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mx);
}
static void cancelar(Cliente* c) {
    pthread_mutex_lock(&c->mx);
    if (!c->mesa) c->cancelado = true;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mx);
}
static void* juez_main(void* arg) {
    Mesa* mp = static_cast<Mesa*>(arg);
    Mesa& m = *mp;
    Servidor* S = m.srv;
    const Config& cfg = *m.cfg;
    mesa_init(m);
    LOG.msg(m.id, "Juez: mesa abierta. Puma=Cliente %d, Perros=Cliente %d (mesas en uso: %d/%d)", m.jug[0]->id,
            m.jug[1]->id, S->mesas_en_uso.load(), S->M);
    {
        BoardLock g(m);
        LOG.bloque("Tablero inicial:\n" + tablero_str(m.st));
    }
    m.beat[0] = m.beat[1] = now_ms();
    asignar(m.jug[0], mp);  
    asignar(m.jug[1], mp);
    while (m.resultado.load() == EN_CURSO) {
        esperar(m, 100);
        if (m.resultado.load() != EN_CURSO) break;
        if (g_stop.load()) {
            anunciar_sigint();
            finalizar(m, INTERRUMPIDA);
            break;
        }
        const long t = now_ms();
        for (int lado = 0; lado < 2; ++lado) {
            if (m.caida[lado].load()) {
                LOG.msg(m.id, "Juez: detecta la CAIDA del jugador %s (hilo terminado de forma anomala) -> abandono",
                        lado == 0 ? "Puma" : "Perros");
                finalizar(m, lado == 0 ? ABANDONO_PUMA : ABANDONO_PERROS);
                break;
            }
            if (t - m.beat[lado].load() > cfg.timeout_abandono) {
                LOG.msg(m.id, "Juez: el bando %s no da senales de vida hace %ld ms -> abandono",
                        lado == 0 ? "Puma" : "Perros", t - m.beat[lado].load());
                finalizar(m, lado == 0 ? ABANDONO_PUMA : ABANDONO_PERROS);
                break;
            }
        }
    }
    pthread_mutex_lock(&m.meta_mx);
    while (m.activas > 0) cond_wait_ms(&m.meta_cv, &m.meta_mx, 100);
    pthread_mutex_unlock(&m.meta_mx);
    const int res = m.resultado.load();
    string fin;
    int movs, caps;
    {
        BoardLock g(m);
        fin = tablero_str(m.st);
        movs = m.st.movs;
        caps = m.st.capturas;
    }
    const int col = m.colisiones.load();
    LOG.bloque("Tablero final:\n" + fin);
    LOG.msg(m.id, "FIN: %s | movimientos=%d capturas=%d colisiones=%d", texto_resultado(res).c_str(), movs, caps, col);
    const int mid = m.id;
    mesa_destroy(m);
    S->res[res].fetch_add(1);
    S->movs_total.fetch_add(movs);
    S->colisiones_total.fetch_add(col);
    S->mesas_en_uso.fetch_sub(1);
    delete mp;
    sem_post(&S->mesas);
    LOG.msg(mid, "Mesa liberada");
    return nullptr;
}
static void* matchmaker_main(void* arg) {
    Servidor& S = *static_cast<Servidor*>(arg);
    vector<pthread_t> jueces;
    while (!g_stop.load() && S.pares_formados.load() < S.pares_totales) {
        Cliente* par[2] = {nullptr, nullptr};
        pthread_mutex_lock(&S.mx);
        while (!g_stop.load() && !(!S.cola[0].empty() && !S.cola[1].empty())) cond_wait_ms(&S.cv, &S.mx, 100);
        if (g_stop.load()) {
            pthread_mutex_unlock(&S.mx);
            break;
        }
        par[0] = S.cola[0].front();
        S.cola[0].pop_front();
        par[1] = S.cola[1].front();
        S.cola[1].pop_front();
        pthread_mutex_unlock(&S.mx);
        LOG.msg(-1, "Emparejados: Cliente %d (Puma) vs Cliente %d (Perros)", par[0]->id, par[1]->id);
        bool ok = false;
        bool avisado = false;
        while (!g_stop.load()) {
            timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            if (sem_timedwait(&S.mesas, &ts) == 0) {
                ok = true;
                break;
            }
            if (!avisado && S.mesas_en_uso.load() >= S.M) {
                LOG.msg(-1, "Las %d mesas estan ocupadas: el par (%d,%d) espera pasivamente", S.M, par[0]->id,
                        par[1]->id);
                avisado = true;
            }
        }
        if (!ok) {
            anunciar_sigint();
            cancelar(par[0]);
            cancelar(par[1]);
            break;
        }
        const int en_uso = S.mesas_en_uso.fetch_add(1) + 1;
        int prev = S.max_mesas.load();
        while (en_uso > prev && !S.max_mesas.compare_exchange_weak(prev, en_uso)) {
        }
        S.pares_formados.fetch_add(1);
        Mesa* m = new Mesa();
        m->id = S.mesa_seq.fetch_add(1) + 1;
        m->cfg = S.cfg;
        m->srv = &S;
        m->jug[0] = par[0];
        m->jug[1] = par[1];
        pthread_t tid;
        if (pthread_create(&tid, nullptr, juez_main, m) != 0) {
            LOG.msg(-1, "ERROR: no se pudo crear el juez de la mesa %d", m->id);
            S.mesas_en_uso.fetch_sub(1);
            delete m;
            sem_post(&S.mesas);
            cancelar(par[0]);
            cancelar(par[1]);
            continue;
        }
        jueces.push_back(tid);
    }
    pthread_mutex_lock(&S.mx);
    S.cerrado = true;
    vector<Cliente*> huerfanos;
    for (int r = 0; r < 2; ++r) {
        for (Cliente* c : S.cola[r]) huerfanos.push_back(c);
        S.cola[r].clear();
    }
    pthread_mutex_unlock(&S.mx);
    for (Cliente* c : huerfanos) cancelar(c);

    for (pthread_t t : jueces) pthread_join(t, nullptr);
    return nullptr;
}
static bool encolar(Servidor& S, Cliente* c) {
    pthread_mutex_lock(&S.mx);
    const bool ok = !S.cerrado;
    if (ok) {
        S.cola[c->rol].push_back(c);
        pthread_cond_broadcast(&S.cv);
    }
    pthread_mutex_unlock(&S.mx);
    return ok;
}
static void* cliente_main(void* arg) {
    Cliente* c = static_cast<Cliente*>(arg);
    Servidor& S = *c->srv;
    const Config& cfg = *S.cfg;
    c->rol = c->id % 2;  
    LOG.msg(-1, "Cliente %d llega y pide jugar como %s", c->id, c->rol == 0 ? "Puma" : "Perros");

    if (!encolar(S, c)) {
        LOG.msg(-1, "Cliente %d: el torneo ya no acepta jugadores", c->id);
        return nullptr;
    }
    pthread_mutex_lock(&c->mx);
    while (!c->mesa && !c->cancelado) cond_wait_ms(&c->cv, &c->mx, 100);
    Mesa* m = c->mesa;
    pthread_mutex_unlock(&c->mx);
    if (!m) {
        LOG.msg(-1, "Cliente %d se va sin jugar (sin pareja o torneo detenido)", c->id);
        return nullptr;
    }
    const int mid = m->id;
    const int lado = c->rol;
    const bool inyectar = std::find(cfg.fallo_jugadores.begin(), cfg.fallo_jugadores.end(), c->id) != cfg.fallo_jugadores.end();
    const int n = (lado == 0) ? 1 : static_cast<int>(cfg.perros.size());
    vector<PiezaArg> args(n);
    vector<pthread_t> th(n);
    vector<bool> lanzado(n, false);
    for (int i = 0; i < n; ++i) {
        args[i].m = m;
        args[i].pieza = (lado == 0) ? PUMA_ID : i;
        args[i].lado = lado;
        args[i].inyectar = inyectar;
        args[i].seed = cfg.seed + static_cast<unsigned>(c->id) * 7919u + static_cast<unsigned>(i) * 104729u;
        if (pthread_create(&th[i], nullptr, pieza_main, &args[i]) == 0) lanzado[i] = true;
        else pieza_termina(*m);  // no arranco: descontarla para no bloquear al juez
    }
    for (int i = 0; i < n; ++i)
        if (lanzado[i]) pthread_join(th[i], nullptr);
    LOG.msg(-1, "Cliente %d (%s) termina su participacion en la mesa %d", c->id, lado == 0 ? "Puma" : "Perros", mid);
    return nullptr;
}
static void uso(const char* prog) {
    fprintf(stderr, "Uso: %s N M [config.txt]\n  N: total de clientes, M: maximo de mesas simultaneas\n", prog);
}
int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        uso(argv[0]);
        return 1;
    }
    char *e1 = nullptr, *e2 = nullptr;
    const long N = strtol(argv[1], &e1, 10);
    const long M = strtol(argv[2], &e2, 10);
    if (*argv[1] == '\0' || *e1 != '\0' || *argv[2] == '\0' || *e2 != '\0' || N < 1 || M < 1 || N > 100000 || M > 10000) {
        fprintf(stderr, "Error: N y M deben ser enteros positivos\n");
        uso(argv[0]);
        return 1;
    }
    Config cfg;
    string err;
    if (argc == 4 && !cargar_config(argv[3], cfg, err)) {
        fprintf(stderr, "Error de configuracion: %s\n", err.c_str());
        return 1;
    }
    if (!validar_config(cfg, err)) {
        fprintf(stderr, "Configuracion invalida: %s\n", err.c_str());
        return 1;
    }
    construir_topologia(cfg.diagonales);
    LOG.abrir(cfg.log_archivo, cfg.consola);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  
    sigaction(SIGINT, &sa, nullptr);
    Servidor S;
    S.cfg = &cfg;
    S.N = static_cast<int>(N);
    S.M = static_cast<int>(M);
    S.pares_totales = S.N / 2;
    sem_init(&S.mesas, 0, static_cast<unsigned>(M));
    init_cond(&S.cv);
    for (auto& r : S.res) r.store(0);

    LOG.msg(-1, "Torneo: N=%ld clientes, M=%ld mesas, seed=%u, diagonales=%d, retardo=%d..%d ms", N, M, cfg.seed,
            cfg.diagonales ? 1 : 0, cfg.retardo_min, cfg.retardo_max);
    pthread_t mm;
    if (pthread_create(&mm, nullptr, matchmaker_main, &S) != 0) {
        fprintf(stderr, "No se pudo crear el matchmaker\n");
        return 1;
    }
    vector<std::unique_ptr<Cliente>> clientes;
    vector<pthread_t> hilos;
    std::mt19937 rng(cfg.seed);
    std::uniform_int_distribution<int> llegada(cfg.llegada_min, cfg.llegada_max);
    for (int i = 0; i < S.N; ++i) {
        // espera de llegada en rebanadas para poder reaccionar a Ctrl+C
        long resto = llegada(rng);
        while (resto > 0 && !g_stop.load()) {
            const long s = std::min(resto, 20L);
            timespec ts = {0, s * 1000000L};
            nanosleep(&ts, nullptr);
            resto -= s;
        }
        if (g_stop.load()) {
            anunciar_sigint();
            break;
        }
        clientes.emplace_back(new Cliente());
        Cliente* c = clientes.back().get();
        c->id = i;
        c->srv = &S;
        pthread_t t;
        if (pthread_create(&t, nullptr, cliente_main, c) != 0) {
            LOG.msg(-1, "ERROR: no se pudo crear el hilo del cliente %d", i);
            clientes.pop_back();
            continue;
        }
        hilos.push_back(t);
    }
    for (pthread_t t : hilos) pthread_join(t, nullptr);
    pthread_join(mm, nullptr);
    int jugadas = 0;
    for (int r = 1; r < N_RESULTADOS; ++r) jugadas += S.res[r].load();
    LOG.msg(-1, "=========== RESUMEN ===========");
    LOG.msg(-1, "Clientes atendidos: %zu de %ld | partidas: %d | maximo de mesas simultaneas: %d (limite M=%ld)",
            clientes.size(), N, jugadas, S.max_mesas.load(), M);
    for (int r = 1; r < N_RESULTADOS; ++r)
        if (S.res[r].load() > 0) LOG.msg(-1, "  %-58s : %d", texto_resultado(r).c_str(), S.res[r].load());
    LOG.msg(-1, "Movimientos totales=%ld | colisiones resueltas=%ld", S.movs_total.load(),
            S.colisiones_total.load());
    sem_destroy(&S.mesas);
    pthread_cond_destroy(&S.cv);
    pthread_mutex_destroy(&S.mx);
    LOG.msg(-1, "Recursos liberados (semaforos, mutex, condiciones, hilos). Hasta luego.");
    LOG.cerrar();
    return g_stop.load() ? 130 : 0;
}
