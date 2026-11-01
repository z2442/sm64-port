.set noat

.text
.set push
.set noreorder

/*
 * void gfx_transform_vertices_vfpu(
 *     struct LoadedVertex *dest,
 *     const Vtx *source,
 *     uint32_t count,
 *     const float matrix[4][4]);
 *
 * Vtx is 16 bytes, with signed object coordinates at offsets 0, 2, and 4.
 * LoadedVertex is 48 bytes; its model and clip vectors start at 0 and 16.
 * The renderer uses row vectors, so the VFPU dot products use matrix columns.
 */

#ifdef F3DEX_GBI_2E
#define VTX_SIZE 24
#else
#define VTX_SIZE 16
#endif
#define LOADED_VERTEX_SIZE 48
#define LOADED_VERTEX_MODEL_POS 0
#define LOADED_VERTEX_CLIP_POS 16

.globl gfx_transform_vertices_vfpu
gfx_transform_vertices_vfpu:
	beq     $a2, $zero, .Ltransform_done
	nop

	/* Combined model-view-projection matrix columns. */
	lv.s    S000, 0($a3)
	lv.s    S001, 16($a3)
	lv.s    S002, 32($a3)
	lv.s    S003, 48($a3)
	lv.s    S010, 4($a3)
	lv.s    S011, 20($a3)
	lv.s    S012, 36($a3)
	lv.s    S013, 52($a3)
	lv.s    S020, 8($a3)
	lv.s    S021, 24($a3)
	lv.s    S022, 40($a3)
	lv.s    S023, 56($a3)
	lv.s    S030, 12($a3)
	lv.s    S031, 28($a3)
	lv.s    S032, 44($a3)
	lv.s    S033, 60($a3)

.Ltransform_vertex:
#ifdef F3DEX_GBI_2E
	lv.s    S200, 0($a1)
	lv.s    S201, 4($a1)
	lv.s    S202, 8($a1)
#else
	lh      $t0, 0($a1)
	lh      $t1, 2($a1)
	lh      $t2, 4($a1)
	mtv     $t0, S200
	mtv     $t1, S201
	mtv     $t2, S202
	vi2f.s  S200, S200, 0
	vi2f.s  S201, S201, 0
	vi2f.s  S202, S202, 0
#endif
	vone.s  S203

	vdot.q  S300, C000, C200
	vdot.q  S301, C010, C200
	vdot.q  S302, C020, C200
	vdot.q  S303, C030, C200

	sv.q    C200, LOADED_VERTEX_MODEL_POS($a0)
	sv.q    C300, LOADED_VERTEX_CLIP_POS($a0)

	addiu   $a1, $a1, VTX_SIZE
	addiu   $a0, $a0, LOADED_VERTEX_SIZE
	addiu   $a2, $a2, -1
	bne     $a2, $zero, .Ltransform_vertex
	nop

.Ltransform_done:
	jr      $ra
	nop

/*
 * void gfx_transform_vertices_eye_vfpu(
 *     struct LoadedVertex *dest,
 *     const Vtx *source,
 *     uint32_t count,
 *     const float matrices[2][4][4]);
 *
 * Igual que gfx_transform_vertices_vfpu, pero ademas de la posicion de recorte (offset 16, matrices[0] = modelo*vista*proyeccion)
 * guarda en el offset 0 la posicion en ESPACIO DE VISTA (matrices[1] = modelo*vista, afin; w = 1) en vez de la del modelo.
 * Con eso el GE solo necesita la proyeccion (la misma durante todo el cuadro) y objetos distintos con el mismo estado pueden ir en
 * una sola llamada de dibujo.
 */
.globl gfx_transform_vertices_eye_vfpu
gfx_transform_vertices_eye_vfpu:
	beq     $a2, $zero, .Leye_done
	nop

	/* Columnas de la matriz de recorte (matrices[0]). */
	lv.s    S000, 0($a3)
	lv.s    S001, 16($a3)
	lv.s    S002, 32($a3)
	lv.s    S003, 48($a3)
	lv.s    S010, 4($a3)
	lv.s    S011, 20($a3)
	lv.s    S012, 36($a3)
	lv.s    S013, 52($a3)
	lv.s    S020, 8($a3)
	lv.s    S021, 24($a3)
	lv.s    S022, 40($a3)
	lv.s    S023, 56($a3)
	lv.s    S030, 12($a3)
	lv.s    S031, 28($a3)
	lv.s    S032, 44($a3)
	lv.s    S033, 60($a3)

	/* Columnas 0, 1 y 2 de la matriz de vista (matrices[1], 64 bytes mas alla). */
	lv.s    S100, 64($a3)
	lv.s    S101, 80($a3)
	lv.s    S102, 96($a3)
	lv.s    S103, 112($a3)
	lv.s    S110, 68($a3)
	lv.s    S111, 84($a3)
	lv.s    S112, 100($a3)
	lv.s    S113, 116($a3)
	lv.s    S120, 72($a3)
	lv.s    S121, 88($a3)
	lv.s    S122, 104($a3)
	lv.s    S123, 120($a3)

.Leye_vertex:
#ifdef F3DEX_GBI_2E
	lv.s    S200, 0($a1)
	lv.s    S201, 4($a1)
	lv.s    S202, 8($a1)
#else
	lh      $t0, 0($a1)
	lh      $t1, 2($a1)
	lh      $t2, 4($a1)
	mtv     $t0, S200
	mtv     $t1, S201
	mtv     $t2, S202
	vi2f.s  S200, S200, 0
	vi2f.s  S201, S201, 0
	vi2f.s  S202, S202, 0
#endif
	vone.s  S203

	vdot.q  S300, C000, C200
	vdot.q  S301, C010, C200
	vdot.q  S302, C020, C200
	vdot.q  S303, C030, C200

	vdot.q  S310, C100, C200
	vdot.q  S311, C110, C200
	vdot.q  S312, C120, C200
	vone.s  S313

	sv.q    C310, LOADED_VERTEX_MODEL_POS($a0)
	sv.q    C300, LOADED_VERTEX_CLIP_POS($a0)

	addiu   $a1, $a1, VTX_SIZE
	addiu   $a0, $a0, LOADED_VERTEX_SIZE
	addiu   $a2, $a2, -1
	bne     $a2, $zero, .Leye_vertex
	nop

.Leye_done:
	jr      $ra
	nop

#ifdef F3DEX_GBI_2E
/*
 * void gfx_transform_vertices_eye2_vfpu(...)  (mismos argumentos y mismo resultado, bit a bit, que gfx_transform_vertices_eye_vfpu)
 *
 * Procesa DOS vertices por vuelta con registros distintos (A: C200 -> C300 recorte, C310 vista; B: C210 -> C320 recorte, C330 vista) para
 * que las latencias de carga / producto punto / guardado de uno se solapen con el trabajo del otro, y deja la "w = 1" de entrada (S203, S213)
 * y de la posicion de vista (S313, S333) puesta una sola vez: las cargas solo tocan x, y, z. Si el numero de vertices es impar, el ultimo va solo.
 * Solo se usa para GBI_FLOATS (Vtx de 24 bytes con coordenadas float); el codigo C lo comprueba con una autoprueba al arrancar.
 */
.globl gfx_transform_vertices_eye2_vfpu
gfx_transform_vertices_eye2_vfpu:
	beq     $a2, $zero, .Leye2_done
	nop

	lv.s    S000, 0($a3)
	lv.s    S001, 16($a3)
	lv.s    S002, 32($a3)
	lv.s    S003, 48($a3)
	lv.s    S010, 4($a3)
	lv.s    S011, 20($a3)
	lv.s    S012, 36($a3)
	lv.s    S013, 52($a3)
	lv.s    S020, 8($a3)
	lv.s    S021, 24($a3)
	lv.s    S022, 40($a3)
	lv.s    S023, 56($a3)
	lv.s    S030, 12($a3)
	lv.s    S031, 28($a3)
	lv.s    S032, 44($a3)
	lv.s    S033, 60($a3)

	lv.s    S100, 64($a3)
	lv.s    S101, 80($a3)
	lv.s    S102, 96($a3)
	lv.s    S103, 112($a3)
	lv.s    S110, 68($a3)
	lv.s    S111, 84($a3)
	lv.s    S112, 100($a3)
	lv.s    S113, 116($a3)
	lv.s    S120, 72($a3)
	lv.s    S121, 88($a3)
	lv.s    S122, 104($a3)
	lv.s    S123, 120($a3)

	vone.s  S203
	vone.s  S213
	vone.s  S313
	vone.s  S333

	addiu   $t3, $a2, -1
	blez    $t3, .Leye2_single        /* un solo vertice */
	nop

.Leye2_pair:
	lv.s    S200, 0($a1)
	lv.s    S201, 4($a1)
	lv.s    S202, 8($a1)
	lv.s    S210, 24($a1)
	lv.s    S211, 28($a1)
	lv.s    S212, 32($a1)

	vdot.q  S300, C000, C200
	vdot.q  S320, C000, C210
	vdot.q  S301, C010, C200
	vdot.q  S321, C010, C210
	vdot.q  S302, C020, C200
	vdot.q  S322, C020, C210
	vdot.q  S303, C030, C200
	vdot.q  S323, C030, C210

	vdot.q  S310, C100, C200
	vdot.q  S330, C100, C210
	vdot.q  S311, C110, C200
	vdot.q  S331, C110, C210
	vdot.q  S312, C120, C200
	vdot.q  S332, C120, C210

	sv.q    C310, 0($a0)
	sv.q    C300, 16($a0)
	sv.q    C330, 48($a0)
	sv.q    C320, 64($a0)

	addiu   $a1, $a1, 48
	addiu   $a0, $a0, 96
	addiu   $a2, $a2, -2
	addiu   $t3, $a2, -1
	bgtz    $t3, .Leye2_pair          /* quedan al menos 2 */
	nop

	beq     $a2, $zero, .Leye2_done
	nop

.Leye2_single:
	lv.s    S200, 0($a1)
	lv.s    S201, 4($a1)
	lv.s    S202, 8($a1)

	vdot.q  S300, C000, C200
	vdot.q  S301, C010, C200
	vdot.q  S302, C020, C200
	vdot.q  S303, C030, C200

	vdot.q  S310, C100, C200
	vdot.q  S311, C110, C200
	vdot.q  S312, C120, C200

	sv.q    C310, 0($a0)
	sv.q    C300, 16($a0)

.Leye2_done:
	jr      $ra
	nop
#endif

.set pop
