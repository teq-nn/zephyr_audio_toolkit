# Audio Pipeline Manifest (Zephyr-Compatible)

Dieses Manifest definiert alle Architekturentscheidungen, die wir gemeinsam festgelegt haben.  
Es dient als verbindlicher technischer Vertrag („Engineering Contract“) für die weitere Entwicklung.

---

## 1. Grundprinzipien

- Das System basiert auf einem **Pull-Modell**.
- Daten werden immer vom **Sink** initiiert, der Samples von seinem Upstream anfordert.
- Jeder Node (Source, Filter, Sink) implementiert eine passive `process()`-Funktion, die nur ausgeführt wird, wenn der Pipeline-Thread sie aufruft.
- Es gibt **keine Tasks pro Node**.

---

## 2. Rollen: Source, Filter, Sink

### Source  
- Hat **keinen Upstream**.  
- Erzeugt Daten (z. B. File Reader, Generator).  
- Benötigt keinen Input-Buffer, schreibt aber in den vom Caller bereitgestellten Buffer.

### Filter  
- Hat **genau einen Upstream**.  
- Liest Daten vom Upstream und verarbeitet sie in-place oder mit internem Scratch-Puffer.  
- Kann Analyzer, Decoder, DSP, Resampler etc. sein.

### Sink  
- Ist der **Startpunkt des Pull-Zyklus**.  
- Ruft die gesamte Kette von Upstream-Nodes auf.  
- Verbraucht die finalen Daten (z. B. File Writer, Hardware-Sink, Test-Sink).

---

## 3. Thread-Modell

- Die Pipeline besitzt **einen eigenen Worker-Thread**, der im Hintergrund läuft.
- Dieser Thread wird bei `audio_pipeline_start()` erstellt.
- Der Thread bleibt bestehen, selbst wenn ein Track / eine Audioquelle endet.
- Mehrere Songs können ohne Neustart des Threads verarbeitet werden.

---

## 4. Datenformat (Kanonisches Format)

### Internes Containerformat:
- **int32_t** pro Sample  
- Little Endian  
- Enum: `AUDIO_SAMPLE_FORMAT_S32_LE`  
- Dieses Format gilt **global und immer** innerhalb der Pipeline.

### Valid Bits:
- Die tatsächliche Auflösung (z. B. 16, 24, 32 Bit) wird über `valid_bits_per_sample` gespeichert.
- Filter sehen immer 32-Bit-Containerwerte.

### Konvertierung:
- Alle Sources konvertieren eingehende Formate (z. B. WAV 16 Bit) → 32 Bit.  
- Alle Sinks konvertieren zurück, falls nötig.

---

## 5. Frame-Größe & Timing

- Die globale Frame-Größe wird über **Kconfig** definiert:  
  `CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES`
- Diese Größe bestimmt die Latenz und den Workload pro Prozesszyklus.
- Die Pipeline ruft `process()` für jeden Node einmal pro Frame auf.

---

## 6. Buffer-Strategie

- **Keine dynamischen Allokationen (`k_malloc`) innerhalb des Subsystems.**
- Alle Pipeline- und Node-Strukturen werden **statisch** über Makros erzeugt.
- Der Pipeline-Thread verwendet einen **gemeinsamen Frame-Buffer**, der ebenfalls statisch angelegt wird.
- Nodes dürfen **interne Scratch-Buffer** haben, ebenfalls statisch (über DEFINE-Makros).

---

## 7. Pipeline-Verhalten bei EOF

- Wenn ein Source „keine Daten mehr“ liefern kann (`out_size = 0`), gilt dies als **EOL / EOF**.
- Filter geben EOL unverändert weiter.
- Sink erzeugt ein **EOF-Ereignis** (über Message-Queue oder Callback).
- Die Audio-Verarbeitung stoppt, aber **der Thread läuft weiter** im Idle-Modus.

---

## 8. Event-Handling

- Die Pipeline erzeugt Events für:
  - `AUDIO_PIPELINE_EVENT_EOF`
  - `AUDIO_PIPELINE_EVENT_ERROR`
  - `AUDIO_PIPELINE_EVENT_RECONFIG`
- Events werden über eine interne `k_msgq` nach außen gereicht oder optional via Callback.

---

## 9. Speicher- und API-Design

- Nodes werden per `NODE_DEFINE`-Makros erzeugt (statische Allokation).
- Die Pipeline wird über `AUDIO_PIPELINE_DEFINE()` erzeugt (statischer Thread, Buffer, Kontext).
- Der Anwender muss **keine Pointer auf Puffer** bereitstellen.
- Alles ist vollständig statisch allokiert und deterministisch.

---

## 10. Erweiterbarkeit

- Float-Support ist möglich, aber ausschließlich über **explizite Converter-Nodes** (`float_to_s32`, `s32_to_float`).
- Formatänderungen im laufenden Betrieb geschehen nur explizit.
- Spätere Multi-Input- oder Multi-Output-Nodes (Mixer, Splitter) sind kompatibel mit diesem Modell.

---

## 11. Zusammenfassung

Dieses Manifest definiert die Architektur der Audio-Pipeline vollständig:

- Pull-Modell  
- Ein Pipeline-Thread  
- Nodes sind passiv, keine eigenen Threads  
- Statische Speicherstrategie  
- Kanonisches 32-Bit-Sampleformat  
- Globale Frame-Größe  
- EOL wird sauber propagiert, Thread bleibt bestehen  
- Event-System über Message Queue  
- API und interne Datenstrukturen sind deterministisch und MCU-freundlich  

Dieses Dokument ist unser gemeinsamer Architekturvertrag.
