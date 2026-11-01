#ifndef PSP_AUDIO_CFG_H
#define PSP_AUDIO_CFG_H

/* Notas simultaneas del sintetizador (la N64 original usa 16). Cuando se agotan, una nota nueva roba una vieja o se pierde (efectos que
 * no suenan con la musica densa). Solo cuentan las notas ACTIVAS. El ME sintetiza en paralelo, pero medido en el registro (parche 36):
 * hasta ~23 notas la CPU casi nunca espera al ME (MEWMS < 4 ms por ventana de 30 ticks); a partir de 24 la espera se multiplica
 * (10-19 ms por ventana). Por eso el tope es 22 (margen bajo ese codo). Maximo 32 (sSampleDmas tiene 4 entradas por nota).
 * 16 = comportamiento original. */
#ifndef PSP_AUDIO_NOTES
#if defined(TARGET_PSP) && !defined(VERSION_EU) && !defined(VERSION_JP)
#define PSP_AUDIO_NOTES 22
#else
#define PSP_AUDIO_NOTES 16
#endif
#endif

#if PSP_AUDIO_NOTES > 32
#error PSP_AUDIO_NOTES: maximo 32 (sSampleDmas[0x60])
#endif

/* Memoria extra del heap de audio (bytes) para las notas y buffers de DMA/comandos de mas (por nota: 3 buffers de DMA de 1296 bytes + 1 de 1440
 * + 20 * 4 comandos de 8 bytes * 2). Con 16 notas el heap original de la N64 va justo. */
#ifndef PSP_AUDIO_HEAP_EXTRA
#if PSP_AUDIO_NOTES > 16
#define PSP_AUDIO_HEAP_EXTRA 0x18000
#else
#define PSP_AUDIO_HEAP_EXTRA 0
#endif
#endif

#endif
