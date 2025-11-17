# Audio Pipeline Specification (Kurzfassung)

## 1. Überblick
Dieses Dokument beschreibt eine leichtgewichtige, MCU‑freundliche Audio-Pipeline für Zephyr auf Basis eines Pull-Modells mit statischer Allokation.

## 2. Architekturprinzipien
- **Pull-Modell**: Der Sink startet den Datenfluss.
- **Ein Pipeline-Thread**: Ein Worker-Thread treibt alle Nodes an.
- **Statische Allokation**: Keine dynamische Speicherverwaltung im Subsystem.
- **Kanonisches internes Format**: 32‑bit signed Little Endian (S32_LE).
- **Drei Node-Typen**: Source, Filter, Sink.
- **Deterministische Latenz**: Globale Frame-Größe über Kconfig.

## 3. Pipeline-Thread
- Gestartet via `audio_pipeline_start()`.
- Läuft im Hintergrund und verarbeitet Frames.
- Stoppt nicht automatisch bei EOF, sondern erzeugt ein Ereignis.

## 4. Datenformat
- Intern: `int32_t` Samples.
- Alle Eingangsdaten werden konvertiert.
- Sinks konvertieren beim Ausgeben zurück in Originalformat.

## 5. Event-System
- `AUDIO_PIPELINE_EVENT_EOF`
- `AUDIO_PIPELINE_EVENT_ERROR`
- `AUDIO_PIPELINE_EVENT_RECONFIG`

## 6. Speicherlayout
- Statische Pipeline-Definition (Makro).
- Nodes über DEFINE-Makros angelegt.
- Gemeinsamer Frame-Puffer pro Pipeline-Thread.

## 7. Node API (Kurz)
```
open(node)
process(node, buf, capacity, out_size)
close(node)
```

## 8. Pipeline-Aufrufkette (Pull)
```
sink -> filter -> filter -> source
```

## 9. Konfiguration
- `CONFIG_AUDIO_PIPELINE_FRAME_SAMPLES`
- Optional: Sample-Rate, Channels arbeitsweise global.

## 10. Erweiterbarkeit
- Float-Unterstützung nur via Converter-Nodes.
- Spätere Multi-Input/Output-Nodes möglich.
