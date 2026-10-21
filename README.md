# OP-1 Monolith

Synth engine personali per il **Teenage Engineering OP-1 originale** con firmware [op1r](https://github.com/pcppcp/op1r)
(R246), scritti con il synth SDK di [op1r-src](https://github.com/pcppcp/op1r-src).

## monolith

Engine drone techno / industrial: tappeti lunghi che non si ripetono mai, per basi techno e progressive.

| manopola | nome | cosa fa |
|---|---|---|
| blu | colore | apertura del filtro, da cupo a brillante (key tracking 50%) |
| verde | movimento | quanto e quanto in fretta il drone evolve (0 = fermo) |
| bianco | ruggine | ring mod inarmonico, wavefolder, soffio |
| rosso | spessore | detune dei 3 saw e livello del sub |

Demo da 90 s (manopole ferme): [`demos/`](demos/). Dettagli tecnici: [`monolith/README.md`](monolith/README.md).

## Struttura

    monolith/        il plugin: monolith.c, plugin.json, harness per render lunghi
    demos/           render sul PC (mp3) e spettrogramma
    release/         monolith.op1p pronto da installare (op1r 34+, API 1)
    setup.sh         collega i plugin a un checkout di op1r-src

## Sviluppo

    ./setup.sh                                   # clona op1r-src accanto e collega i plugin
    cd ../op1r-src/synth_sdk
    python3 harness/audio.py monolith            # test con sanitizers -> WAV
    python3 make_plugin.py monolith              # -> build/plugins/monolith.op1p (toolchain bfin-elf)

Installazione sull'OP-1 (op1r 34 o successivo): web patcher di op1r, sezione **Connect OP-1**, "own plugin files",
scegli `monolith.op1p`; l'OP-1 chiede conferma (T1 riavvia e applica).

## Licenza

GPL-3.0-or-later, con il permesso aggiuntivo di op1r per combinarlo con il firmware dell'OP-1 (vedi
[NOTICE di op1r-src](https://github.com/pcppcp/op1r-src/blob/main/NOTICE)). Progetto indipendente, non affiliato a
Teenage Engineering. OP-1 è un marchio di Teenage Engineering AB.
