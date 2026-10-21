# monolith — drone techno / industrial per OP-1 (op1r)

Engine per droni lunghi che non si ripetono mai, da usare come tappeto per techno e progressive.

## Manopole
- **blu – colore**: apertura del filtro, da cupo a brillante (con key tracking al 50%)
- **verde – movimento**: quanto e quanto in fretta il drone evolve (0 = fermo)
- **bianco – ruggine**: ring mod inarmonico, wavefolder e soffio (da pulito a industriale)
- **rosso – spessore**: detune dei 3 saw e livello del sub un'ottava sotto

Consiglio: sulla pagina T2 metti attacco e rilascio lunghi; un dito o il sequencer in hold su 1-2 note basse.

## Come funziona
3 saw + square sub → ring mod con un seno a rapporto non intero che deriva → wavefolder → rumore →
filtro state-variable risonante → soft clip. Cutoff, risonanza, velocità del battimento, rapporto del ring e
un leggero drift di intonazione sono mossi da 3 LFO lenti con periodi incommensurabili più 3 random walk:
la combinazione in pratica non si ripete. Lo stato di modulazione continua tra una nota e l'altra.

## Test sul PC
    python3 ../../harness/audio.py monolith                      # test ufficiale (sanitizers)
    ./render_demo.sh buio 90 7000,14000,4000,20000 7000,14000,4000,20000 24,36

## Stato
Testato solo sul PC (ASan + UBSan, nessun overflow, nessun click). Va ancora provato sul device: controlla il
carico con il resource monitor (schermo orologio + T1). Per arrivare sull'OP-1: pull request a op1r-src
(order 99, il numero definitivo lo assegnano i maintainer) oppure build locale del .op1p con la toolchain Blackfin.
