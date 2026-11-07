/* Small dense linear algebra for calibration fits. */
#ifndef LINALG_H
#define LINALG_H

/* Solves A x = b in place (A: n*n row-major, destroyed; b: becomes x).
 * Gaussian elimination with partial pivoting. Returns 0, or -1 if singular. */
int la_solve(int n, double *A, double *b);

/* Eigen-decomposition of a symmetric 3x3 matrix (Jacobi).
 * w: eigenvalues; V: eigenvectors as columns. */
void la_eig_sym3(const double S[3][3], double w[3], double V[3][3]);

typedef struct {
    double offset[3];     /* ellipsoid center (hard iron / zero offset) */
    double W[3][3];       /* correction: corrected = W * (raw - offset) */
    double radius;        /* radius of the corrected sphere */
    double natural_radius;/* geometric-mean radius of the raw ellipsoid */
    double axes[3];       /* raw ellipsoid semi-axes (sorted as eigenvalues) */
    double residual;      /* RMS of |corrected| - radius over the input points */
} la_ellipsoid_t;

/* Least-squares ellipsoid fit to n points.
 * axis_aligned: 1 fits only center + per-axis scale (6 params), 0 fits a general
 *               ellipsoid (9 params, includes cross-axis / rotation terms).
 * target_radius: radius to map onto (e.g. 9.80665 for accel); <= 0 keeps the
 *               ellipsoid's natural geometric-mean radius.
 * Returns 0, or <0 if the points do not define a valid ellipsoid. */
int la_fit_ellipsoid(const double (*pts)[3], int n, int axis_aligned,
                     double target_radius, la_ellipsoid_t *out);

#endif
