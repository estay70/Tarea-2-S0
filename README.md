# Torneo de Komikan Concurrente — Tarea 2, Sistemas Operativos (UDP)

Servidor de torneo con emparejamiento (*Matchmaker*), mesas concurrentes, jueces y fichas
(1 Puma + 12 Perros) que compiten en tiempo real y sin turnos. C++17, hilos y primitivas POSIX.

Integrantes: 
- Martin Estay 
- Diego Peñailillo

## Compilación (Ubuntu / Linux)

```bash
sudo apt install build-essential        # si falta g++
make                                     # g++ -Wall -Wextra -std=c++17 -pthread -O2
# o con los flags exactos de la rúbrica:
g++ -Wall -Wextra -std=c++17 -pthread -o torneo torneo.cpp
```

Compila sin ninguna advertencia.

## Ejecución

```bash
./torneo N M [config.txt]
```

* `N`: total de clientes (jugadores). Ids pares juegan de Puma, impares de Perros.
  Se forman `N/2` partidas; si `N` es impar, un Puma queda sin pareja y se retira.
* `M`: máximo de mesas simultáneas.
* `config.txt` (opcional): ver formato abajo. Sin archivo se usan los valores por defecto.

Ejemplos:

```bash
./torneo 8 2                      # 8 jugadores, 2 mesas, configuración por defecto
./torneo 20 4 config.txt          # con archivo de configuración
./torneo 40 5 mi_estres.txt       # alto número de hilos
```

Todo lo que ocurre (movimientos, colisiones, capturas, tableros, jueces, fallos) sale por consola
y también se guarda en `torneo.log` (o el archivo indicado en `log_archivo`).
Con `consola = 0` solo se escribe al archivo (útil para pruebas grandes).

## Formato de `config.txt`

Líneas `clave = valor`; `#` inicia un comentario. Todas las claves son opcionales
(ver `config.txt`, que contiene los valores por defecto comentados).

| Clave | Significado | Defecto |
|---|---|---|
| `seed` | Semilla: las decisiones y retardos de cada pieza son reproducibles | 42 |
| `retardo_min_ms`, `retardo_max_ms` | Retardo aleatorio entre movimientos de una pieza | 50, 300 |
| `diagonales` | 1: diagonales desde casillas de (fila+col) par; 0: solo ortogonal | 1 |
| `puma` | Posición inicial del Puma: `T` (vértice del trono) o `fila,col` | `T` |
| `perros` | Posiciones iniciales (1 a 12) separadas por espacios, `fila,col` | filas 3 y 4 + (2,0),(2,4) |
| `capturas_victoria` | Capturas para que gane el Puma | 6 |
| `max_movimientos` | Tope de seguridad por partida; si se alcanza: *tablas* | 1500 |
| `imprimir_cada` | Imprime la matriz del tablero cada K movimientos | 10 |
| `timeout_abandono_ms` | Sin señales de vida de un bando ⇒ abandono (≥ 2·retardo_max+200) | 1500 |
| `llegada_min_ms`, `llegada_max_ms` | Separación aleatoria entre llegadas de clientes | 0, 150 |
| `consola` | 1 imprime en pantalla además del log; 0 solo archivo | 1 |
| `log_archivo` | Archivo de log | `torneo.log` |
| `fallo_jugadores` | Ids de cliente que fallarán (inyección de fallos) | ninguno |
| `fallo_tras_movs` | Tras cuántos movimientos propios falla | 5 |
| `fallo_modo` | `muerte` (el hilo muere con el mutex tomado) o `silencio` (se cuelga) | `muerte` |

Un archivo inválido (clave desconocida, casilla fuera del tablero, fichas superpuestas, etc.)
se rechaza con un mensaje y código de salida 1.

## Modelo del tablero (supuestos)

Cuadrícula 5×5 (nodos `fila,col`) más el vértice del trono `T`, unido a `(0,0)`, `(0,2)` y `(0,4)`.
Los pasos son ortogonales; con `diagonales = 1`, también diagonales desde las casillas de
`(fila+col)` par (estilo alquerque). El Puma captura saltando en línea recta sobre un perro
adyacente hacia la casilla vacía inmediata (incluye el salto vertical `T ↔ (1,2)` sobre `(0,2)`).
La topología se construye una sola vez, es de solo lectura y se puede ajustar con `diagonales`.

## Arquitectura (resumen; detalle en `informe.pdf`)

```
main ──► N hilos cliente ──► cola Puma / cola Perros ──► matchmaker ──► sem_t mesas (M)
                                                             │
                                                             ▼
                                    juez (1 hilo por mesa) ── 1 hilo Puma + 12 hilos Perro
```

* **Límite de mesas:** `sem_t mesas` inicializado en `M`. Si no hay cupo, el matchmaker duerme
  (`sem_timedwait` en rebanadas de 100 ms solo para poder atender SIGINT); no hay espera activa.
* **Tablero:** un `pthread_mutex_t` **robusto** por mesa. Cada pieza toma una instantánea corta,
  decide fuera del lock, y luego vuelve a validar y aplicar dentro de la sección crítica: quien
  llega primero a una casilla libre se la queda; quien pierde cuenta una *colisión* y reintenta.
* **Juez:** inicializa y destruye las primitivas, detecta victoria/abandono/SIGINT y libera la mesa
  (`sem_post`) solo cuando todas sus piezas terminaron.
* **Fallos:** si un hilo muere con el mutex tomado, el siguiente `lock` recibe `EOWNERDEAD`, lo
  marca consistente (`pthread_mutex_consistent`) y el juez declara abandono. Si un jugador se
  cuelga sin morir, lo detecta el timeout de latidos.
* **SIGINT:** el handler solo activa un `atomic` lock-free. Todas las esperas son temporizadas,
  así que cada hilo lo observa en ≤100 ms: se cierran las partidas, se cancela a los clientes en
  cola, se hace `join` de todos los hilos y se destruyen mutex, condiciones y semáforos.
  Código de salida 130.
* **Recursos:** el programa usa un solo proceso con hilos y primitivas sin nombre, por lo que no
  deja objetos en `/dev/shm`, ni semáforos o memoria compartida System V.

## Pruebas

```bash
make test        # ejecuta ./pruebas.sh
```

Cubre: límite de mesas, espera pasiva, N impar, estrés (alto número de hilos con retardos casi
nulos), abandono por caída y por silencio, EOWNERDEAD, Ctrl+C (SIGINT), validación de argumentos y
configuración, ausencia de recursos huérfanos y valgrind (cero fugas). Además se verificó con
`make asan` (AddressSanitizer + UBSan) y `make tsan` (ThreadSanitizer): sin errores.

## Archivos

| Archivo | Contenido |
|---|---|
| `torneo.cpp` | Código fuente completo |
| `Makefile` | Compilación y pruebas |
| `config.txt` | Configuración de ejemplo con todas las claves |
| `pruebas.sh` | Pruebas automáticas |
| `informe.pdf` | Informe (2 páginas) |
