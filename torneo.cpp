
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