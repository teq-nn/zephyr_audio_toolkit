# Zephyr Audio Pipeline – Software-Spezifikation (v1)

Dieses Dokument beschreibt die Architektur und das Verhalten eines Audio-Pipeline-Subsystems für Zephyr.  
Es ist so formuliert, dass ein externer Entwickler die Implementierung ohne weitere Kontextinformationen umsetzen kann.

---

## 1. Zielsetzung

### 1.1 Zweck

Das Subsystem stellt eine **leichtgewichtige Audio-Pipeline** bereit, mit der sich Audio-Daten in Form von Quellen (Sources), Verarbeitern (Filter) und Senken (Sinks) verketten lassen.

### 1.2 Designziele

- **Pull-basiertes Datenflussmodell**
- **Ein Worker-Thread pro Pipeline**
- **Statische Speicherallokation** (keine `k_malloc` im Subsystem)
- **Kanonisches internes Sampleformat: 32‑bit signed, Little Endian**
- **Eindeutige Rollen**: Source, Filter, Sink
- **Deterministische Latenz durch globale Frame-Größe**
- **Zephyr-konforme Fehlercodes und Coding Style**

### 1.3 Nicht-Ziele (v1)

- Kein generischer Support für Float-Verarbeitung (nur vorbereitete Erweiterbarkeit).
- Keine dynamische Rekonfiguration der Pipeline zur Laufzeit (Format & Struktur sind in v1 statisch).
- Keine Unterstützung von Mehrfach-Ein-/Ausgängen in Nodes (keine Mixer/Splitter in v1).

---

## 2. Grundlegende Architektur

### 2.1 Komponenten

- **Audio-Pipeline (`audio_pipeline`)**  
  - Hält eine geordnete Liste von Nodes (Source → Filter... → Sink).  
  - Besitzt einen eigenen Worker-Thread.  
  - Verantwortlich für:
    - Lebenszyklus (open/close) aller Nodes,
    - Start/Stop der Verarbeitung,
    - Event-Erzeugung (EOF, Fehler).

- **Nodes (`audio_node`)**  
  - Implementieren Audio-Funktionalität.  
  - sind in drei Rollen klassifiziert:
    - **Source**: erzeugt Daten
    - **Filter**: transformiert Daten
    - **Sink**: konsumiert Daten

- **Event-System**  
  - Meldet wichtige Zustände (EOF, Fehler, Reconfigure) an die Anwendung.

### 2.2 Datenflussmodell (Pull)

- Der **Sink** initiiert den Datenfluss, indem er Daten von seinem Upstream anfordert.
- Jeder Filter ruft seinerseits seinen Upstream auf, bis letztlich eine Source bedient wird.
- Daten werden in festen **Frames** verarbeitet, deren Größe global definiert ist.

Sequenz (vereinfacht):

```text
Pipeline-Thread:
    while (playing) {
        frame = pull_frame_from_sink();
        if (frame_size == 0) {
            signal EOF;
            playing = false;
        }
    }
```

---

## 3. Threading & Ausführung

### 3.1 Pipeline-Thread

- Die Pipeline erstellt bei `audio_pipeline_start()` einen **Worker-Thread** über `k_thread_create`.
- Dieser Thread:
  - führt in einer Schleife Frame-Verarbeitung durch, solange ein interner `playing`-Flag gesetzt ist,
  - läuft danach in einem Idle-/Wartezustand weiter (Thread bleibt existierend),
  - wird nur durch `audio_pipeline_stop()`/`audio_pipeline_join()` endgültig beendet.

### 3.2 Timing-Modell (v1)

- In v1 gibt es **keine explizite Timer-Taktung** innerhalb der Pipeline.
- Der Pipeline-Thread verarbeitet Frames **so schnell wie möglich**, solange:
  - `playing == true` ist und
  - die Nodes Daten liefern.
- Echtzeit-Synchronisation (z. B. I2S-Ausgabe) ist Aufgabe der Sinks:
  - Ein Hardware-Sink kann im `process()` bei Bedarf blockieren oder intern mit eigener Taktung arbeiten.
- Für rein dateibasierte Tests (File→File, File→Memory) ist dieser „best effort“-Durchlauf ausreichend und einfach.

> Hinweis: Für spätere Versionen kann ein Timer-basiertes Scheduling ergänzt werden, ist aber explizit *kein* Bestandteil von v1.

### 3.3 Concurrency-Regeln

- **Pipeline-API (`audio_pipeline_*`)**  
  - Darf ausschließlich aus einem **Steuerthread** aufgerufen werden (z. B. dem Hauptthread oder einer dedizierten Control-Task).
- **Nodes (`audio_node`)**  
  - Werden ausschließlich vom **Pipeline-Thread** aufgerufen.  
  - Nodes sind **nicht reentrant** und müssen keine eigene Thread-Sicherheit implementieren.
- **Event-Queue**  
  - Darf von der Anwendung aus beliebigen Threads gelesen werden (Zephyr-typische `k_msgq`-Semantik).

---

## 4. Rollenmodell: Source, Filter, Sink

### 4.1 Gemeinsamer Node-Typ

```c
enum audio_node_role {
    AUDIO_NODE_ROLE_SOURCE,
    AUDIO_NODE_ROLE_FILTER,
    AUDIO_NODE_ROLE_SINK,
};

struct audio_node_ops {
    int (*open)(struct audio_node *node);
    ssize_t (*process)(struct audio_node *node,
                       int32_t *buf,
                       size_t capacity,   /* in Samples */
                       size_t *out_size); /* in Samples */
    int (*close)(struct audio_node *node);
};

struct audio_node {
    enum audio_node_role role;
    const struct audio_node_ops *ops;
    struct audio_node *upstream; /* NULL für Sources */
    void *context;               /* Implementierungsspezifischer Zustand */
};
```

**Namensvorgabe:**  
Die Funktionszeiger in `audio_node_ops` heißen **`open`**, **`process`**, **`close`** („opc“), wie vom Auftraggeber vorgegeben.  

**Fehlercodes:**  
- `open()` und `close()` liefern `int` mit Zephyr-Fehlercodes (`0` bei Erfolg, `< 0` bei Fehler; z. B. `-EINVAL`, `-EIO`, `-ENOMEM`).
- `process()` liefert `ssize_t`:
  - `>= 0`: Anzahl der tatsächlich produzierten Samples (in `out_size` gespiegelt),
  - `< 0`: Fehlercode (z. B. `-EIO`).

### 4.2 Source-Rolle

- `role == AUDIO_NODE_ROLE_SOURCE`
- `upstream == NULL`
- `process()`:
  - liest Daten aus einer Quelle (Datei, Generator, etc.),
  - füllt `buf` mit bis zu `capacity` Samples,
  - schreibt die tatsächliche Sampleanzahl nach `*out_size`.

EOF-Konvention:
- Bei End-of-Data: `*out_size = 0`, `process()` gibt `0` zurück.

### 4.3 Filter-Rolle

- `role == AUDIO_NODE_ROLE_FILTER`
- `upstream != NULL`
- `process()`:
  - ruft zuerst `upstream->ops->process(...)` auf,
  - verarbeitet die gelieferten Samples (in-place oder mit internem Scratch),
  - schreibt die resultierende Anzahl Samples wieder in `*out_size`.

EOF-Verhalten:
- Wenn Upstream `out_size == 0` liefert, muss der Filter:
  - selbst `*out_size = 0` setzen,
  - `0` zurückgeben,
  - keine weiteren Daten mehr erzeugen.

### 4.4 Sink-Rolle

- `role == AUDIO_NODE_ROLE_SINK`
- `upstream != NULL`
- `process()`:
  - ruft `upstream->ops->process(...)` auf,
  - verarbeitet oder konsumiert die Daten (z. B. schreibt sie in eine Datei),
  - die Pipeline interessiert sich primär für EOF/Fehler:
    - Wenn `*out_size == 0`: End-of-Stream erkannt.

Anmerkung:
- Im Gegensatz zu vielen Frameworks schreibt der Sink nicht zwingend in `buf`, sondern nutzt diesen als transienten Transportpuffer.

---

## 5. Datenformat

### 5.1 Kanonisches internes Format

- Datentyp: `int32_t`
- Endianness: Little Endian
- Enum-Wert: `AUDIO_SAMPLE_FORMAT_S32_LE`
- Alle Nodes arbeiten intern mit 32‑Bit Samples.

### 5.2 gültige Bits pro Sample

```c
struct audio_format {
    uint32_t sample_rate;           /* Hz, z. B. 44100, 48000 */
    uint8_t  channels;              /* v1: immer 2 */
    uint8_t  valid_bits_per_sample; /* z. B. 16, 24, 32 */
    enum audio_sample_format format;/* intern: AUDIO_SAMPLE_FORMAT_S32_LE */
};
```

- In v1 wird `channels` fest auf **2** (Stereo) festgelegt.  
- Die Samples sind **interleaved** im Buffer:

```text
buf: L0, R0, L1, R1, L2, R2, ...
```

- `valid_bits_per_sample` beschreibt die effektive Auflösung:
  - z. B. `16` für aus 16‑Bit-PCM konvertierte Daten,
  - `24` oder `32` bei höherer Auflösung.

### 5.3 Formatkonvertierung

- Sources übernehmen die Konvertierung von ihrem Eingangsformat nach S32_LE:
  - 16-Bit PCM → `int32_t s32 = (int32_t)s16 << 16;`
  - 24-Bit PCM → `int32_t s32 = (int32_t)s24 << 8;`
- Sinks konvertieren S32_LE zurück in ihr Ziel-Format:
  - z. B. `int16_t s16 = (int16_t)(s32 >> 16);`

Filter erwarten und produzieren ausschließlich S32_LE.

---

## 6. Pipeline-Objekt

### 6.1 Struktur

Ein möglicher Entwurf für `struct audio_pipeline` (Details können in der Implementierung variieren, sollten aber diese Elemente enthalten):

```c
struct audio_pipeline {
    struct audio_node *source;
    struct audio_node *sink;
    struct audio_node **filters;
    size_t filter_count;

    struct audio_format format;

    struct k_thread thread;
    k_thread_stack_t *stack;
    size_t stack_size;
    int priority;

    int32_t *frame_buffer;
    size_t frame_capacity; /* in Samples */

    struct k_msgq *event_queue;

    bool initialized;
    bool running;
    bool playing;
};
```

### 6.2 Statische Definition

Die Pipeline wird statisch über ein Makro definiert, z. B.:

```c
AUDIO_PIPELINE_DEFINE(my_pipeline,
    .frame_samples = CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES,
    .stack_size    = CONFIG_AUDIO_PIPELINE_STACK_SIZE,
    .priority      = CONFIG_AUDIO_PIPELINE_PRIORITY);
```

Das Makro:
- legt `struct audio_pipeline my_pipeline` an,
- erzeugt `K_THREAD_STACK_DEFINE` für den Pipeline-Thread,
- erzeugt einen statischen Frame-Puffer:
  ```c
  static int32_t my_pipeline_frame_buf[CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES * 2];
  ```
  (2 = Kanäle),
- verknüpft diese mit der Pipeline-Struktur.

---

## 7. Kconfig-Optionen

Minimaler Satz an Konfigurationsoptionen:

```kconfig
config AUDIO_PIPELINE
    bool "Audio processing pipeline framework"

config AUDIO_PIPELINE_FRAME_SAMPLES
    int "Samples per frame (per channel)"
    default 64
    range 8 1024

config AUDIO_PIPELINE_STACK_SIZE
    int "Stack size for pipeline worker thread"
    default 2048

config AUDIO_PIPELINE_PRIORITY
    int "Priority of pipeline worker thread"
    default 5
```

---

## 8. Pipeline-API

### 8.1 Initialisierung & Konfiguration

```c
int audio_pipeline_init(struct audio_pipeline *pl);

int audio_pipeline_set_nodes(struct audio_pipeline *pl,
                             struct audio_node *source,
                             struct audio_node **filters,
                             size_t filter_count,
                             struct audio_node *sink);

int audio_pipeline_set_format(struct audio_pipeline *pl,
                              const struct audio_format *fmt);
```

Vereinbarungen:

- `audio_pipeline_init()`:
  - setzt interne Flags,
  - initialisiert Event-Queue,
  - darf nur einmal pro Pipeline-Instanz aufgerufen werden.
- `audio_pipeline_set_nodes()`:
  - legt `source`, Filterliste und `sink` fest,
  - verkettet intern `upstream`-Zeiger (Filter[i].upstream = (i==0 ? source : Filter[i-1]); sink.upstream = letzter Filter oder Source).
- `audio_pipeline_set_format()`:
  - setzt das für die gesamte Pipeline gültige `audio_format`.  
  - v1: Format ist statisch für gesamte Laufzeit.

### 8.2 Lifecycle

```c
int audio_pipeline_start(struct audio_pipeline *pl);
int audio_pipeline_play(struct audio_pipeline *pl);
int audio_pipeline_stop(struct audio_pipeline *pl); /* stoppt Playing */
int audio_pipeline_join(struct audio_pipeline *pl); /* optional: Thread Ende abwarten */
```

Empfohlenes Verhalten:

- `audio_pipeline_start()`:
  - erstellt den Thread (falls nicht bereits existierend),
  - öffnet alle Nodes via `node->ops->open`.
- `audio_pipeline_play()`:
  - setzt `pl->playing = true`,
  - der Pipeline-Thread beginnt Frames zu ziehen.
- `audio_pipeline_stop()`:
  - setzt `pl->playing = false`,
  - Thread bleibt existierend, aber im Idle/Wartezustand.
- `audio_pipeline_join()`:
  - optional: beendet Thread (für Clean-up-Szenarien).

### 8.3 Events

```c
enum audio_pipeline_event_type {
    AUDIO_PIPELINE_EVENT_EOF,
    AUDIO_PIPELINE_EVENT_ERROR,
    AUDIO_PIPELINE_EVENT_RECONFIG,
};

struct audio_pipeline_event {
    enum audio_pipeline_event_type type;
    int error; /* optional: Fehlercode bei ERROR */
};
```

API (Beispiel):

```c
int audio_pipeline_get_event(struct audio_pipeline *pl,
                             struct audio_pipeline_event *evt,
                             k_timeout_t timeout);
```

Verhalten:

- EOF: Sobald ein Sink `out_size == 0` erhält, wird ein `AUDIO_PIPELINE_EVENT_EOF` erzeugt.
- ERROR: Wenn ein Node `process()` oder `open()`/`close()` < 0 zurückgibt, erzeugt die Pipeline `AUDIO_PIPELINE_EVENT_ERROR` und setzt `evt.error` entsprechend.

---

## 9. Verhalten bei EOF & Fehlern

### 9.1 EOF

- Quelle signalisiert EOF mit `out_size == 0` und Rückgabewert `0`.
- Filter propagieren diesen Zustand unverändert weiter.
- Sink erkennt EOF, informiert die Pipeline.
- Pipeline:
  - setzt `playing = false`,
  - erzeugt `AUDIO_PIPELINE_EVENT_EOF`.

### 9.2 Fehler

- Jeder negative Rückgabewert aus `open()`, `process()`, `close()` ist ein Fehler.
- Beim ersten Fehler:
  - stoppt die Pipeline die weitere Frame-Verarbeitung (`playing = false`),
  - erzeugt `AUDIO_PIPELINE_EVENT_ERROR`,
  - optional: führt ein `close()` auf allen Nodes aus.

---

## 10. Beispielnodes (v1)

### 10.1 File Reader Node (Source)

- Aufgabe:
  - Öffnet eine WAV-Datei (z. B. über RAMFS oder LittleFS),
  - liest den Header (nur PCM, Stereo, 16 Bit),
  - liefert S16-Daten als S32_LE an die Pipeline.

- Kontextstruktur (Beispiel):

```c
struct file_reader_context {
    struct fs_file_t file;
    struct audio_format fmt;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t bytes_read;
    bool eof;
};
```

- `open()`:
  - Datei öffnen,
  - Header parsen,
  - `fmt` setzen,
  - `bytes_read = 0`, `eof = false`.
- `process()`:
  - liest `capacity` * 2 (Kanäle) * 2 (Bytes pro Sample) aus der Datei,
  - konvertiert `int16_t` → `int32_t` in `buf`,
  - setzt `*out_size` (in Samples, nicht in Bytes).
- `close()`:
  - Datei schließen.

### 10.2 File Writer Node (Sink)

- Aufgabe:
  - Nimmt S32_LE entgegen,
  - konvertiert auf gewünschtes Ausgabeformat (z. B. 16 Bit PCM),
  - schreibt in Datei.

Implementation analog zum Reader, nur umgekehrt.

---

## 11. Speicher- und Modulstruktur

### 11.1 Keine dynamische Allokation

- Innerhalb des Subsystems wird **keine** dynamische Speicherallokation (`k_malloc`, `k_calloc`, `k_free`) verwendet.
- Alle Strukturen (Pipeline, Nodes, Kontexte, Puffer) sind statisch definiert oder vom Nutzer bereitgestellt.

### 11.2 Makros zur Definition

- `AUDIO_PIPELINE_DEFINE(name, ...)`  
  - legt Pipeline + Thread-Stack + Frame-Buffer an.
- `FILE_READER_NODE_DEFINE(name, path)`  
  - legt `struct audio_node` und `struct file_reader_context` statisch an.

Konkrete Makros können im Implementierungsdesign ausgearbeitet werden, sollen aber dieses Prinzip einhalten.

---

## 12. Teststrategie

### 12.1 Ziel

- Tests sollen vollständig auf **QEMU** laufen, ohne reale Audio-Hardware.
- Fokus auf:
  - Korrekte Datenweitergabe,
  - EOF-Verhalten,
  - Fehlerpfade.

### 12.2 Roundtrip-Test

Beispiel-Testaufbau:

```text
[file_reader_source] -> [optional filter] -> [file_writer_sink]
```

Testschritte:

1. Bekannte WAV-Datei (Golden Master) in RAMFS einbinden.
2. Pipeline laufen lassen, bis EOF.
3. Ausgabedatei mit Golden-Master vergleichen:
   - Dateigröße identisch,
   - Byte-für-Byte identisch.

### 12.3 Negative Tests

- Beschädigter WAV-Header → `open()` muss Fehler liefern.
- Frühzeitiges EOF → Pipeline muss sauber EOF-Event liefern.
- Simulierte I/O-Fehler → ERROR-Event.

---

## 13. Erweiterungspunkte für spätere Versionen

- **Float-DSP**:
  - Einführen von Converter-Nodes `s32_to_float`, `float_to_s32`.
- **Mehrkanal-Unterstützung**:
  - Aufhebung der 2‑Kanal-Einschränkung.
- **Mixer/Splitter**:
  - Erweiterung des Node-Modells um mehrere Upstreams/Downstreams.
- **Timer-getaktete Pipeline**:
  - Optionaler Modus, der Frames in Echtzeit basierend auf Sample-Rate ausgibt.

---

## 14 Projektstruktur

```

zephyr-audio-pipeline/
├─ module.yml
├─ CMakeLists.txt
├─ Kconfig
├─ include/
│  └─ zephyr/
│     └─ audio/
│        ├─ audio_format.h
│        ├─ audio_node.h
│        ├─ audio_pipeline.h
│        └─ audio_pipeline_events.h
├─ subsys/
│  └─ audio/
│     └─ pipeline/
│        ├─ CMakeLists.txt
│        ├─ Kconfig
│        ├─ audio_pipeline_core.c
│        ├─ audio_pipeline_config.c
│        ├─ audio_pipeline_events.c
│        ├─ audio_node_core.c
│        ├─ audio_internal.h
│        ├─ nodes/
│        │   ├─ file_reader_node.c
│        │   ├─ file_writer_node.c
│        │   ├─ gain_filter_node.c
│        │   └─ null_sink_node.c
│        └─ util/
│            ├─ wav_parser.c
│            └─ wav_parser.h
├─ samples/
│  └─ audio/
│     └─ pipeline_basic/
│        ├─ CMakeLists.txt
│        ├─ Kconfig
│        └─ src/main.c
└─ tests/
   └─ subsys/
      └─ audio/
         └─ pipeline/
            ├─ CMakeLists.txt
            ├─ Kconfig
            ├─ test_roundtrip.c
            └─ test_error_paths.c

```


## 15. Zusammenfassung

Diese Spezifikation definiert:

- Ein Pull-basiertes Audio-Pipeline-System,
- mit einem Worker-Thread pro Pipeline,
- statischer Speicherallokation,
- 32‑Bit internem Sampleformat (S32_LE),
- klaren Rollen (Source/Filter/Sink),
- vorgegebenem EOF- und Fehlerverhalten,
- Zephyr-konformer API und Fehlercodes,
- und einem einfachen, aber erweiterbaren Event-System.

Sie stellt den vollständigen technischen Vertrag dar, an den sich die Implementierung halten soll.
