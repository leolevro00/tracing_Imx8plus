# OSSERVAZIONI

Questo risultato conferma che il problema è legato al carico grafico/rendering/traffico di memoria  generato da GPU + DRM/KMS + framebuffer.

kmscube non è Qt, non è HMI ESA, non usa la shared memory ESA. Quindi se il ritardo cambia così tanto solo cambiando la risoluzione, vuol dire che il sottosistema grafico basso livello è sufficiente a influenzare il determinismo.

Dunque più pixel da renderizzare → più traffico grafico/memoria/bus → più pressione sul sistema → il path kernel di wakeup della clock_nanosleep() può diventare più lento.

Con 1920x1080, nel max sleep:

```bash
CPU3:
  Ritardo sleep      : 413 us
  cpu_cycles         : 1.078.006
  istruzioni         : 100.066
  IPC                : 0.0928
  CPI                : 10.77

```

Con 640x480, nel max sleep:

```bash
CPU3:
  Ritardo sleep      : 31 us
  cpu_cycles         : 189.957
  istruzioni         : 99.633
  IPC                : 0.5245
  CPI                : 1.906

```

Come si può osservare le istruzioni sono quasi identiche, dunque su cpu3 sta girando la stessa quantità di codice solo che nel caso della 1920x1080 quel codice procede molto lentamente

| Risoluzione | Max sleep | CPU3 cycles | CPU3 instructions | CPU3 IPC | CPU3 CPI |
| ----------- | --------: | ----------: | ----------------: | -------: | -------: |
| `1920x1080` |    413 µs |   1.078.006 |           100.066 |   0.0928 |    10.77 |
| `1280x720`  |    360 µs |     722.606 |            99.383 |   0.1375 |     7.27 |
| `800x600`   |    305 µs |     523.365 |            99.357 |   0.1898 |     5.27 |
| `640x480`   |     31 µs |     189.957 |            99.633 |   0.5245 |     1.91 |

Si può osservare come il max sleep crolli drasticamente  nel passaggio da 800x600 a 640x480 e questo potrebbe indicare che sotto una certa risoluzione il carico grafico scende abbastanza da non disturbare piu pesantemente il path real-time
