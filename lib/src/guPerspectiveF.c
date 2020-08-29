#include "libultra_internal.h"

void guPerspectiveF(float mf[4][4], u16 *perspNorm, float fovy, float aspect, float near, float far,
                    float scale) {
    float yscale;
    int row;
    int col;
    guMtxIdentF(mf);
    fovy *= GU_PI / 180.0;
    yscale = cosf(fovy / 2) / sinf(fovy / 2);
    mf[0][0] = yscale / aspect;
    mf[1][1] = yscale;
    mf[2][2] = (near + far) / (near - far);
    mf[2][3] = -1;
    mf[3][2] = 2 * near * far / (near - far);
    mf[3][3] = 0.0f;
    for (row = 0; row < 4; row++) {
        for (col = 0; col < 4; col++) {
            mf[row][col] *= scale;
        }
    }
    if (perspNorm != NULL) {
        if (near + far <= 2.0) {
            *perspNorm = 65535;
        } else {
            *perspNorm = (double) (1 << 17) / (near + far);
            if (*perspNorm <= 0) {
                *perspNorm = 1;
            }
        }
    }
}
void guPerspective(Mtx *m, u16 *perspNorm, float fovy, float aspect, float near, float far,
                   float scale) {
    float mat[4][4];
    extern int gFrame;
    extern int gDoAA;
    extern short gFPS;
    /*@Note: Changes our perspective to 16/9 */
    /*@Note: Adds optional fake AA if we are at 60fps and relies on shit psp screen to smear for us */
    guPerspectiveF(mat, perspNorm, fovy + ((((gFrame & 1)  & ( gFPS >= 60)) & gDoAA) * 0.5f), aspect*1.32352941177f, near, far, scale);
    guMtxF2L(mat, m);
}
