#ifndef GFX_SCREEN_CONFIG_H
#define GFX_SCREEN_CONFIG_H

#define DESIRED_SCREEN_WIDTH 640
#define DESIRED_SCREEN_HEIGHT 480

/* PSP: 1 = el ultimo cuadro de cada tick de 60 fps se presenta (sync del GE + vblank + cambio de buffers) en un hilo, y la CPU ya
 * ejecuta la logica del tick siguiente mientras el GE termina. 0 = comportamiento anterior (todo sincrono). */
/* Perfil de GPU (Triangulo+L): 0 = no se compila (los hooks de la ruta caliente desaparecen). Poner 1 solo para medir. */
#ifndef PSP_GPU_PROF
#define PSP_GPU_PROF 0
#endif
/* Interruptores para aislar un fallo: 0 = desactiva esa pieza.
 * PSP_F2_SKIP: omitir el 2o cuadro de un tick de 60 fps si el 1o se paso de un vblank (pc_main.c).
 * PSP_ME_FASTPOLL: dormir ~3/4 del tiempo del bloque anterior antes de sondear al ME (psp_me.c). */
#ifndef PSP_F2_SKIP
#define PSP_F2_SKIP 0 /* 0 = siempre se dibujan los 2 cuadros del tick (60 fps fijos; si no caben, el juego va mas lento). 1 = en zonas pesadas baja a 30 fps a velocidad normal */
#endif
/* PSP_F2_REPLAY (opcion 3, etapa 1): en un tick de 60 fps el 2o cuadro NO vuelve a recorrer la lista del juego ni a generar triangulos;
 * llama (sceGuCallList) a la escena que el 1er cuadro ya dejo escrita en la lista del GE. Etapa 1: la repite SIN cambiar nada (el 2o cuadro
 * sale igual al 1o; es una prueba del mecanismo y de cuanta CPU se ahorra). Si algo no cuadra (lista reiniciada, texturas borradas...),
 * ese cuadro se dibuja como siempre. 0 = desactivado del todo. */
#ifndef PSP_F2_REPLAY
#define PSP_F2_REPLAY 0 /* APAGADO por defecto: el 2o cuadro vuelve a emitir la escena completa con el estado real (estable). 1 = repetir la lista del 1o */
#endif
/* PSP_F2_REPLAY_MTX (opcion 3, etapa 2): el 2o cuadro, ademas de repetir la escena, reescribe en la lista del GE las matrices de
 * cada objeto con las del estado real (el 1o las lleva interpoladas), asi se mueve de verdad en 60 fps. Si hay algun otro parche
 * pendiente (HUD, dialogos, transiciones, nieve, burbujas, cuadros, cambio de fov...) o un objeto se movio demasiado, ese cuadro se
 * dibuja entero como siempre. 0 = comportamiento de la etapa 1 (repetir sin cambiar nada). */
#ifndef PSP_F2_REPLAY_MTX
#define PSP_F2_REPLAY_MTX 1
#endif
/* PSP_F2_SKY: el cielo (matriz ortografica propia dentro de su lista) tambien se reescribe en el 2o cuadro con la del estado real:
 * el cuadro 1 lo lleva interpolado, asi que sin esto el cielo se actualizaba a 30 fps. Solo se parchea si las dos versiones dibujan
 * los mismos tiles (si no, se queda como antes). Necesita PSP_F2_REPLAY_MTX. 0 = desactivado. */
#ifndef PSP_F2_SKY
#define PSP_F2_SKY 1
#endif
/* Triangulos en modo "decal" (ZMODE_DEC: sombras, manchas, sombras incrustadas en el piso del mapa...). La N64 los compara con el z-buffer
 * con una tolerancia que depende de la INCLINACION del piso en pantalla (dz del pixel). El GE de la PSP solo tiene un desplazamiento
 * fijo y el z-buffer es de 16 bits.
 * PSP_DECAL_ADAPTIVE 1: el desplazamiento se calcula por triangulo con la formula EXACTA de la N64 (ver gfx_pc.c, gfx_decal_bias).
 * 0 = valor fijo PSP_DECAL_OFFSET. */
#ifndef PSP_DECAL_ADAPTIVE
#define PSP_DECAL_ADAPTIVE 1
#endif
/* Margen que se suma a la tolerancia de la N64 para el error de interpolacion de z del GE (unidades del z-buffer de 16 bits).
 * Si alguna sombra se corta o parpadea (sobre todo lejos), subirlo (16, 24...); si cubre lo que esta pegado al piso, bajarlo. */
#ifndef PSP_DECAL_OFFSET
#define PSP_DECAL_OFFSET 1
#endif
/* Escala de la tolerancia de la N64 (1.0 = la formula tal cual). El GE no necesita tanto como la N64 (su error de z es menor que el de la
 * N64 a 320x240) y una tolerancia grande hace que la sombra tape lo que esta pegado al piso (pies, bases de objetos). 0.25 = un cuarto. */
#ifndef PSP_DECAL_SCALE
#define PSP_DECAL_SCALE 0.25f
#endif
#ifndef PSP_DECAL_MAX
#define PSP_DECAL_MAX 1024      /* tope de seguridad */
#endif
#ifndef PSP_DECAL_FALLBACK
#define PSP_DECAL_FALLBACK 128  /* triangulos que tocan el plano near: no se pueden medir */
#endif
/* PSP_SAFE_WARP: durante un cambio de nivel/area o una transicion de warp el juego se dibuja en modo normal (1 cuadro por tick, sin
 * interpolar ni repetir listas). 0 = desactivado. */
#ifndef PSP_SAFE_WARP
#define PSP_SAFE_WARP 0 /* con 60 fps fijos no se baja a 30 en las transiciones; poner 1 si algo falla al cambiar de nivel */
#endif
#ifndef PSP_ME_FASTPOLL
#define PSP_ME_FASTPOLL 1
#endif
#ifndef PSP_ASYNC_PRESENT
#define PSP_ASYNC_PRESENT 1
#endif

#endif
