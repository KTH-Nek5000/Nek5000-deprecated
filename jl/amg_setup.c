#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include "c99.h"
#include "name.h"
#include "types.h"
#include "fail.h"
#include "mem.h"
#include "sort.h"
#include "sarray_sort.h"
#include "gs_defs.h"
#include "comm.h"
#include "crystal.h"
#include "sarray_transfer.h"
#include "gs.h"
#include "amg_tools.h"
#include "amg_setup.h"

/* 
    Parallel version of the AMG setup for Nek5000 based on the Matlab version. 

    Algorithm is based on the the Ph.D. thesis of J. Lottes:
    "Towards Robust Algebraic Multigrid Methods for Nonsymmetric Problems"

    - Author of the original version (Matlab): James Lottes
    - Author of the parallel version: Nicolas Offermans

    - Last update: 16 December, 2015

    - Status: developing function interp(...) based cinterp.c from MAtlab.

*/

/*
    REMARKS:
        - at some point in the Matlab code, conj(Af) is used. A priori, 
          the matrices A or Af should never be complex so we ignored this
          operation in this code. This should be checked though!

        - in function interpolation, u is a vector of 1s (default value) but
          other choices are possible (vector needs to be "near null space"
          --> cf. thesis)
*/

/*
    TODO: 
        - properly check anyvc (Done but not tested)
        - wrap gs operations in a function (like apply_Q and apply_Qt) ? (Maybe)
        - write free function for csr matrices
        - write sym_sparsify
        - rewrite sparsify so that the matrix at output is the sparsified matrix
*/

void amg_setup(uint n, const ulong *id, uint nz_unassembled, const uint *Ai, 
    const uint* Aj, const double *Av,  struct crs_data *data)
/*    Build data, the "struct crs_data" defined in amg_setup.h that is required
      to execute the AMG solver. 
      Matrices are partitioned by rows. */
{   

/* Declare csr matrix A (assembled matrix Av under csr format) and array gs_id, 
   an array of ids to setup gather-scatter */  
    struct csr_mat *A = tmalloc(struct csr_mat, 1);
    slong *gs_id;

/* Build A and gs_id, the required data for the setup */
    build_setup_data(A, &gs_id, n, id, nz_unassembled, Ai, Aj, Av, data);
    uint rn = A->rn;
    uint cn = A->cn;

/* Create handles for gather-scatter */
    // General gather-scatter handle for global vectors (asymmetric behavior)
    struct gs_data *gsh = gs_setup(gs_id, cn, &(data->comm), 1, gs_auto, 0);
    // Gather-scatter handle for single data
    slong foo = 1;
    struct gs_data *gsh_single = gs_setup(&foo, 1, &(data->comm), 0, gs_auto,0);

/* At this point:
    - A is stored using csr format
    - each row is located entirely on one process only
    - gather-scatter has been setup for the main level
*/

/**************************************/
/* Memory allocation for data struct. */
/**************************************/

    uint rnglob = rn;
    gs(&rnglob, gs_int, gs_add, 0, gsh_single, 0);

    data->tni = 1./rnglob;

    // Initial size for number of sublevels
    // If more levels than this, realloction is required!
    uint initsize = 10; 
    data->cheb_m = tmalloc(uint, initsize);
    data->cheb_rho = tmalloc(double, initsize); 
    data->lvl_offset = tmalloc(uint, initsize+1);

    data->Dff = tmalloc(double, rn);

    data->Q_W = tmalloc(struct Q, initsize);
    data->Q_AfP = tmalloc(struct Q, initsize);
    data->Q_Aff = tmalloc(struct Q, initsize);

    data->W = tmalloc(struct csr_mat, initsize);
    data->AfP = tmalloc(struct csr_mat, initsize);
    data->Aff = tmalloc(struct csr_mat, initsize);

/**************************************/
/* AMG setup (previously Matlab code) */
/**************************************/

    // Sublevel number
    uint slevel = 0;
    
    uint offset = 0;
    data->lvl_offset[slevel] = offset;

/* Tolerances (hard-coded so far) */
    double tol = 0.5; 
    double ctol = 0.7; // Coarsening tolerance
    double itol = 1e-4; // Interpolation tolerance
    double gamma2 = 1. - sqrt(1. - tol);
    double gamma = sqrt(gamma2);
    double stol = 1e-4;

// BEGIN WHILE LOOP

/* Make sure that enough memory is allocated */
    if (slevel > 0 && slevel % initsize == 0)
    {
        data->cheb_m = trealloc(uint, data->cheb_m, initsize);
        data->cheb_rho = trealloc(double, data->cheb_rho, initsize); 
        data->lvl_offset = trealloc(uint, data->lvl_offset, initsize);

        data->Q_W = trealloc(struct Q, data->Q_W, initsize);
        data->Q_AfP = trealloc(struct Q, data->Q_AfP, initsize);
        data->Q_Aff = trealloc(struct Q, data->Q_Aff, initsize);

        data->W = trealloc(struct csr_mat, data->W, initsize);
        data->AfP = trealloc(struct csr_mat, data->AfP, initsize);
        data->Aff = trealloc(struct csr_mat, data->Aff, initsize);
    }

/* Coarsen */ 
    double *vc = tmalloc(double, cn);
    // compute vc for i = 1,...,rn
    coarsen(vc, A, ctol, gs_id, gsh, gsh_single); 
    // update vc for i = rn+1,...,cn
    gs(vc, gs_double, gs_add, 0, gsh, 0); 

    double *vf = tmalloc(double, cn);
    bin_op(vf, vc, cn, not_op); // vf  = ~vc for i = 1,...,cn

/* Smoother */
    // Af = A(F, F)
    struct csr_mat *Af = tmalloc(struct csr_mat, 1);
    sub_mat(Af, A, vf, vf);

    // Letter f denotes dimensions for Af
    uint rnf = Af->rn;
    uint cnf = Af->cn;
    uint ncolf = Af->row_off[rnf];

    // Build new gs handle (Fine mesh)
    slong *gs_id_f = tmalloc(slong, cnf);
    sub_slong(gs_id_f, gs_id, vf, cn);
    struct gs_data *gsh_f = gs_setup(gs_id_f, cnf, &(data->comm), 1, 
                                         gs_auto, 0);

    // af2 = Af.*Af ( Af.*conj(Af) in Matlab --> make sure Af is never complex)  
    double *af = tmalloc(double, ncolf); 
    memcpy(af, Af->a, ncolf*sizeof(double));
    double *af2 = af;
    vv_op(af2, af2, ncolf, ewmult); // af2 = Af.*Af

    // s = 1./sum(Af.*Af)
    double *s = tmalloc(double, rnf);
    uint i;
    for (i=0; i<rnf; i++)
    {
        uint js = Af->row_off[i];
        uint je = Af->row_off[i+1]; 
        uint nsum = je-js;

        s[i] = array_op(af2, nsum, sum_op); // s = sum(af2)
        af2 += nsum; 
    }

    array_op(s, rnf, minv_op); // s = 1./s

    // D = diag(Af)' .* s
    double *D = tmalloc(double, rnf);
    init_array(D, rnf, -1);
    diag(D, Af);

    vv_op(D, s, rnf, ewmult);

    // nf = nnz(vf) (globally)
    uint nf = rnf;
    gs(&nf, gs_int, gs_add, 0, gsh_single, 0);

    double gap;

    if (nf >= 2)
    {
        // Dh = sqrt(D)
        double *Dh = tmalloc(double, cnf);
        memcpy(Dh, D, rnf*sizeof(double));
        array_op(Dh, rnf, sqrt_op);

        gs(Dh, gs_double, gs_add, 0, gsh_f, 0);

        struct csr_mat *DhAfDh = tmalloc(struct csr_mat, 1);
        copy_csr(DhAfDh, Af); // DhAfDh = Af
        diagcsr_op(DhAfDh, Dh, dmult); // DhAfDh = Dh*Af
        diagcsr_op(DhAfDh, Dh, multd); // DhAfDh = Dh*Af*Dh

        // Vector of eigenvalues
        double *lambda;
        // Number of eigenvalues
        uint k = lanczos(&lambda, DhAfDh, gs_id_f, gsh_f, gsh_single);

        // First and last eigenvalues
        double a = lambda[0];
        double b = lambda[k-1];

        ar_scal_op(D, 2./(a+b), rnf, mult_op);
        data->Dff += offset;
        data->Dff = D;        

        double rho = (b-a)/(b+a);
        data->cheb_rho[slevel] = rho;
        
        double m, c;
        chebsim(&m, &c, rho, gamma2);
        data->cheb_m[slevel] = m;

        gap = gamma2-c;

        /* Sparsification is skipped for the moment */
        //sym_sparsify(Sf, DhAfDh, (1.-rho)*(.5*gap)/(2.+.5*gap)); => not implemented

        free(Dh);    
        free(lambda);        
    }
    else
    {
        gap = 0;

        data->Dff += offset;
        data->Dff = D;  

        data->cheb_rho[slevel] = 0;
        data->cheb_m[slevel] = 1;
    }
        
    data->Aff = Af;
    data->Q_Aff->nloc = Af->cn;
    data->Q_Aff->gsh = gsh_f;

/* Interpolation */
    // Afc = A(F, C)
    struct csr_mat *Afc = tmalloc(struct csr_mat, 1);
    sub_mat(Afc, A, vf, vc);

    // Ac = A(C, C)
    struct csr_mat *Ac = tmalloc(struct csr_mat, 1);
    sub_mat(Ac, A, vc, vc);

    // Letter c denotes dimensions for Ac
    uint rnc = Ac->rn;
    uint cnc = Ac->cn;
    uint ncolc = Ac->row_off[rnc];

    // New gs handle (Coarse mesh)
    slong *gs_id_c = tmalloc(slong, cnc);
    sub_slong(gs_id_c, gs_id, vc, cn);
    struct gs_data *gsh_c = gs_setup(gs_id_c, cnc, &(data->comm), 1, 
                                         gs_auto, 0);

    // W
    struct csr_mat *W = tmalloc(struct csr_mat, 1);

    interpolation(W, Af, Ac, Afc, gamma2, itol, gsh_f, gsh_c, gsh_single);
    
/* Update data structure */
    offset += rnf;
    slevel += 1;

    data->lvl_offset[slevel] = offset;

// END WHILE LOOP

    data->levels = slevel;

    // Compute dimensions for remaining arrays
    uint max_f = 0, max_e = 0;
    for (i=0; i<slevel; i++)
    {
        uint f = data->lvl_offset[i+1] - data->lvl_offset[i];
        if (f > max_f) max_f = f;

        uint e = data->W[i].cn;
        if (e > max_e) max_e = e;

        e = data->AfP[i].cn;
        if (e > max_e) max_e = e;

        e = data->Aff[i].cn;
        if (e > max_e) max_e = e;
    }

    // Initialize remaining arrays of data structure to 0
    data->b = tmalloc(double, rn);
    init_array(data->b, rn, 0.);
    data->x = tmalloc(double, rn);
    init_array(data->x, rn, 0.);

    data->c = tmalloc(double, max_f);
    init_array(data->c, max_f, 0.);
    data->c_old = tmalloc(double, max_f);
    init_array(data->c_old, max_f, 0.);
    data->r = tmalloc(double, max_f);
    init_array(data->r, max_f, 0.);

    data->buf = tmalloc(double, max_e);
    init_array(data->buf, max_e, 0.);

    data->timing_n = 0;
    data->timing = tmalloc(double, 6*(slevel-1));
    init_array(data->timing, 6*(slevel-1), 0.);

/* Free */
    // Free arrays
    free(vc);
    free(gs_id);
    free(gs_id_f);
    free(gs_id_c);
    free(af);
    free(s);
    free(D);

    // Free csr matrices
    csr_free(&A);
    csr_free(&Af);
    csr_free(&Afc);
    csr_free(&Ac);
    //csr_free(&W);

    // Free gs
    gs_free(gsh);
    gs_free(gsh_f);
    gs_free(gsh_c);
    gs_free(gsh_single);
}

/* Interpolation */
void interpolation(struct csr_mat *W, struct csr_mat *Af, struct csr_mat *Ac, 
    struct csr_mat *Ar, double gamma2, double tol, struct gs_data *gsh_f, 
    struct gs_data *gsh_c, struct gs_data *gsh_single)
{

    // Dimensions of the matrices
    uint rnf = Af->rn, cnf = Af->cn;
    uint rnc = Ac->rn, cnc = Ac->cn;
    // uint rnr = Ar->rn, cnr = Ar->cn;
    // rnr = rnf and cnr = cnc

    // If nc==0
    if (rnc == 0)
    {
        W->rn = rnf;
        W->cn = 0;
    }

    // d = 1./diag(Af)
    double *Df = tmalloc(double, rnf);
    diag(Df, Af);

    double *Dfinv = tmalloc(double, rnf);
    diag(Dfinv, Af);
    array_op(Dfinv, rnf, minv_op);

    // uc = ones(nf, 1) /!\ in the Matlab code, uc could be diffent from ones
    //                      though this is the default value.
    double *uc = tmalloc(double, cnc);
    init_array(uc, cnc, 1.);

    // v = pcg(Af,full(-Ar*uc),d,1e-16);
    double *r = tmalloc(double, rnf); 
    apply_M(r, 0, uc, -1, Ar, uc);

    double *v = tmalloc(double, rnf); 

    pcg(v, Af, r, Df, 1e-16, gsh_f, gsh_single);   
    
    // dc = diag(Ac)
    double *Dc = tmalloc(double, cnc);
    diag(Dc, Ac);

    double *Dcinv = tmalloc(double, cnc);
    diag(Dcinv, Ac);
    array_op(Dcinv, rnc, minv_op);
    gs(Dcinv, gs_double, gs_add, 0, gsh_c, 0);

    // W_skel = intp.min_skel( (Ar/Dc) .* (Df\Ar) );
    struct csr_mat *ArD = tmalloc(struct csr_mat, 1); //ArD = (Ar/Dc) .* (Df\Ar)
    copy_csr(ArD, Ar);
    
    array_op(ArD->a, ArD->row_off[ArD->rn], sqr_op);

    diagcsr_op(ArD, Dfinv, dmult);
    diagcsr_op(ArD, Dcinv, multd);

    // Minimum interpolation skeleton
    struct csr_mat *W_skel = tmalloc(struct csr_mat, 1);
    min_skel(W_skel, ArD);

    // Initialize eigenvalues array and matrix W0
    double *lam = tmalloc(double, rnf);
    init_array(lam, rnf, 0.);
    struct csr_mat *W0 = tmalloc(struct csr_mat, 1);

    //while(true){
            
        solve_weights(W, W0, lam, W_skel, Af, Ar, rnc, Dc, uc, v, tol);        

    //}

    free(Df);
    free(Dfinv);
    free(Dc);
    free(Dcinv);
    free(uc);
    free(v);
    free(r);

    // free Arcpy
    free(ArD->row_off);
    free(ArD->col);
    free(ArD->a);
    free(ArD);
    // free W_skel
    free(W_skel->row_off);
    free(W_skel->col);
    free(W_skel->a);
    free(W_skel);
}

/* Solve interpolation weights */
void solve_weights(struct csr_mat *W, struct csr_mat *W0, double *lam, 
    struct csr_mat *W_skel, struct csr_mat *Af, struct csr_mat *Ar, uint rnc,
    double *alpha, double *u, double *v, double tol)
{
    // Dimensions of the matrices
    uint rnf = Af->rn, cnf = Af->cn;
    uint rnr = Ar->rn, cnr = Ar->cn;

    // au = alpha.*u
    double *au = tmalloc(double, rnc);
    memcpy(au, alpha, rnc*sizeof(double));
    vv_op(au, u, rnc, ewmult);

    copy_csr(W0, W_skel);

/*    
    // Matrices W0, Af and Ar should be transpose to get correct result !
    //interp(W0, Af, Ar, au, lam);
    
*/

}

/* Build interpolation matrix.
   It is assumed that matrix W is initialized with minimum skeleton.
   Wt, At and Bt are transposed matrices ! */
void interp(struct csr_mat *Wt, struct csr_mat *At, struct csr_mat *Bt, double *u, 
    double *lambda)
{
    uint nf = Wt->rn, nc = Wt->cn;
    uint max_nz = 0, max_Q;
    uint i;

    double *sqv1, *sqv2;
    double *Q, *QQt;

    for (i=0;i<nf;i++)
    {
        uint nz = Wt->row_off[i+1]-Wt->row_off[i];
        if (nz>max_nz) max_nz = nz;
    }
    
    max_Q = (max_nz*(max_nz+1))/2;

    sqv1 = tmalloc(double, 2*max_nz + max_Q);
    sqv2 = sqv1+max_nz, Q = sqv2+max_nz;

    for(i=0;i<nf;++i) 
    {
        uint wir = Wt->row_off[i];
        const uint *Qj = &Wt->col[wir];
        uint nz = Wt->row_off[i+1]-wir;
        uint m,k;
        double *qk = Q;
        for(k=0;k<nz;++k,qk+=k) 
        {
            double alpha;
            uint s = Qj[k];
            // sqv1 := R_(k+1) A e_s
            sp_restrict_sorted(sqv1, k+1,Qj, At->row_off[s+1]-At->row_off[s],
            &At->col[At->row_off[s]], &At->a[At->row_off[s]]);
            // sqv2 := Q^t A e_s 
            mv_utt(sqv2, k,Q, sqv1);
            // qk := Q Q^t A e_s 
            mv_ut(qk, k,Q, sqv2);
            // alpha := ||(I-Q Q^t A)e_s||_A^2 = (A e_s)^t (I-Q Q^t A)e_s
            alpha = sqv1[k];
            for(m=0;m<k;++m) alpha -= sqv1[m] * qk[m];
            // qk := Q e_(k+1) = alpha^{-1/2} (I-Q Q^t A)e_s
            alpha = -1.0 / sqrt(alpha);
            for(m=0;m<k;++m) qk[m] *= alpha;
            qk[k] = -alpha;
        }
        // sqv1 := R B e_i
        sp_restrict_sorted(sqv1, nz,Qj, Bt->row_off[i+1]-Bt->row_off[i],
                           &Bt->col[Bt->row_off[i]], &Bt->a[Bt->row_off[i]]);
        // sqv1 := R (B e_i + u_i lambda)
        for(k=0;k<nz;++k) sqv1[k] += u[i]*lambda[Qj[k]];
        // sqv2 := Q^t (B e_i + u_i lambda)
        mv_utt(sqv2, nz,Q, sqv1);
        // X e_i := Q Q^t (B e_i + u_i lambda)
        mv_ut(&Wt->a[wir], nz,Q, sqv2);

    }

    free(sqv1);
}

////////////////////////////////////////////////////////////////////////////////

/* Upper triangular transpose matrix vector product
   y[0] = U[0] * x[0]
   y[1] = U[1] * x[0] + U[2] * x[1]
   y[2] = U[3] * x[0] + U[4] * x[1] + U[5] * x[2]
   ... */
static void mv_utt(double *y, uint n, const double *U, const double *x)
{
  double *ye = y+n; uint i=1;
  for(;y!=ye;++y,++i) {
    double v=0;
    const double *xp=x, *xe=x+i;
    for(;xp!=xe;++xp) v += (*U++) * (*xp);
    *y=v;
  }
}

/* Upper triangular matrix vector product
   y[0] = U[0] * x[0] + U[1] * x[1] + U[3] * x[2] + ...
   y[1] =               U[2] * x[1] + U[4] * x[2] + ...
   y[2] =                             U[5] * x[2] + ...
   ... */
static void mv_ut(double *y, uint n, const double *U, const double *x)
{
  uint i,j;
  for(j=0;j<n;++j) {
    y[j]=0;
    for(i=0;i<=j;++i) y[i] += (*U++) * x[j];
  }
}

/*--------------------------------------------------------------------------
   sparse restriction
   
   y := R * x
   
   the sparse vector x is restricted to y
   R is indicated by map_to_y
   map_to_y[i] == k   <->    e_k^t R == e_i^t I
   map_to_y[i] == -1  <->    row i of I not present in R
--------------------------------------------------------------------------*/
static void sp_restrict_unsorted(double *y, uint yn, const uint *map_to_y,
    uint xn, const uint *xi, const double *x)
{
  const uint *xe = xi+xn; uint i;
  for(i=0;i<yn;++i) y[i]=0;
  for(;xi!=xe;++xi,++x) {
    uint i = map_to_y[*xi];
    if(i>=0) y[i]=*x;
  }
}

/*--------------------------------------------------------------------------
   sparse restriction
   
   y := R * x
   
   the sparse vector x is restricted to y
   Ri[k] == i   <->   e_k^t R == e_i^t I
   Ri must be sorted
--------------------------------------------------------------------------*/
static void sp_restrict_sorted(double *y, uint Rn, const uint *Ri, uint xn, 
    const uint *xi, const double *x)
{
  const uint *xe = xi+xn;
  double *ye = y+Rn;
  uint iy;
  if(y==ye) return; iy = *Ri;
  for(;xi!=xe;++xi,++x) {
    uint ix = *xi;
    while(iy<ix) { *y++ = 0; if(y==ye) return; iy = *(++Ri); }
    if(iy==ix) { *y++ = *x; if(y==ye) return; iy = *(++Ri); }
  }
  while(y!=ye) *y++ = 0;
}


////////////////////////////////////////////////////////////////////////////////

/* Minimum skeleton */
void min_skel(struct csr_mat *W_skel, struct csr_mat *R)
{
    uint rn = R->rn;
    uint cn = R->cn;

    uint i, k;
    double *y_max = tmalloc(double, rn);
    init_array(y_max, rn, -1e+30);

    // Create the array W_skel
    W_skel->rn = rn;
    W_skel->cn = cn;
    W_skel->row_off = tmalloc(uint,rn+1);
    W_skel->col = tmalloc(uint, rn);
    W_skel->a = tmalloc(double, rn);
    
    for (i = 0; i < rn; i++) 
    {
	    uint j = 0;
	    for(k = R->row_off[i]; k < R->row_off[i+1]; k++) 
        {
	        if (R->a[k] > y_max[i]) 
            {
		        y_max[i] = R->a[k];
		        j = R->col[k];
	        }
	    }
	    if (y_max[i] > 0.0) 
        {
	        W_skel->a[i] = 1.0; 
	    }
	    else 
        {
	        W_skel->a[i] = 0.0;
	    }
	    W_skel->col[i] = j;
	    W_skel->row_off[i] = i;
    }
    W_skel->row_off[rn] = rn;    
}

/* Preconditioned conjugate gradient */
uint pcg(double *x, struct csr_mat *A, double *r, double *M, double tol,
    struct gs_data *gsh, struct gs_data *gsh_single)
{
    uint rn = A->rn;
    uint cn = A->cn;

    // x = zeros(size(r)); p=x;
    init_array(x, rn, 0.);

    double *p = tmalloc(double, cn);
    init_array(p, cn, 0.);

    // z = M(r); (M(r) = M.*r)
    double *z = tmalloc(double, rn);
    memcpy(z, M, rn*sizeof(double));
    vv_op(z, r, rn, ewmult);
    
    // rho_0 = rho;
    // rho_stop=tol*tol*rho_0
    double rho = vv_dot(r, z, rn);

    gs(&rho, gs_double, gs_add, 0, gsh_single, 0);
    double rho_stop = tol*tol*rho;

    // n = min(length(r),100);
    uint n = rn;
    gs(&n, gs_int, gs_add, 0, gsh_single, 0);
    n = (n <= 100) ? n : 100;

    // if n==0; return; end
    if (n == 0)
    {
        return 0;
    } 

    // rho_old = 1;
    double rho_old = 1;
    
    uint k = 0;
    double alpha = 0, beta = 0;
    double *tmp = tmalloc(double, rn);
    double *w = tmalloc(double, rn);

    // while rho > rho_stop && k<n
    while (rho > rho_stop && k < n)
    {
        k++;

        // beta = rho / rho_old;
        beta = rho / rho_old;

        // p = z + beta * p;
        ar_scal_op(p, beta, rn, mult_op);
        vv_op(p, z, rn, plus);
        gs(p, gs_double, gs_add, 0, gsh, 0);

        // w = A(p); (A(p) = A*p)
        apply_M(w, 0, p, 1, A, p);

        // alpha = rho / (p'*w);
        alpha = vv_dot(p, w, rn);
        gs(&alpha, gs_double, gs_add, 0, gsh_single, 0);
        alpha = rho / alpha;

        // x = x + alpha*p;
        memcpy(tmp, p, rn*sizeof(double));
        ar_scal_op(tmp, alpha, rn, mult_op);
        vv_op(x, tmp, rn, plus);

        // r = r - alpha*w;
        ar_scal_op(w, alpha, rn, mult_op);
        vv_op(r, w, rn, minus);

        // z = M(r);
        memcpy(z, M, rn*sizeof(double));
        vv_op(z, r, rn, ewmult);

        // rho_old = rho;
        rho_old = rho;

        // rho = r'*z;
        rho = vv_dot(r, z, rn);
        gs(&rho, gs_double, gs_add, 0, gsh_single, 0);
    }

    free(p);
    free(z);
    free(w);
    free(tmp);

    return k;
}

/*
  Sym_sparsify (for symmetric "sparsification")
*/
// TO BE IMPLEMENTED

/* 
  Sparsify (for non symmetric "sparsification")
  This function is slightly different from the Matlab version: S is an array 
  having the same number of elements as A.
   - S[i] = 0 if A->a[i] is "sparsified"
   - S[i] = 1 if A->a[i] is kept
  S needs to be allocated with A->row_off[rn] doubles before the call!

==> Needs to be reimplemented and return the sparsified matrix directly
*/
void sparsify(double *S, struct csr_mat *A, double tol)
{
    uint rn = A->rn;
    uint cn = A->cn;
    uint *row_off = A->row_off;
    uint *col = A->col;
    double *a = A->a;
    uint ncol = A->row_off[rn];

    // S has the same number of elements as A -> initialized with 1s
    init_array(S, ncol, 1.);

    // E[i] is the sum of the smallest elements of line i such that the sum is
    // lower than the tolerance
    double *E = tmalloc(double, rn);    
    init_array(E, rn, 0.);

    // Build matrix A using coordinate list format for sorting
    // coo_A = abs(A)
    struct coo_mat *coo_A = tmalloc(struct coo_mat, ncol);  
    struct coo_mat *p = coo_A;  

    uint i;
    for(i=0;i<rn;++i) 
    {
        uint j; 
        uint je=row_off[i+1]; 
        for(j=row_off[i]; j<je; ++j) 
        {
            p->i = i;
            p->j = *col++;
            p->v = fabs(*a++);
            p++;
        }
    }

    qsort(coo_A, ncol, sizeof(struct coo_mat), comp_coo_v);

    for (i=0; i<ncol; i++)
    {
        if (coo_A[i].v > tol) break;
        
        if (coo_A[i].i != coo_A[i].j)
        {
            E[coo_A[i].i] += fabs(coo_A[i].v);
            if (E[coo_A[i].i] < tol) S[i] = 0.;
        }
    }

    free(coo_A);
    free(E);
}

/*
  Chebsim
*/
void chebsim(double *m, double *c, double rho, double tol)
{
    double alpha = 0.25*rho*rho;
    *m = 1;
    double cp = 1;
    *c = rho;
    double gamma = 1;
    double d, cn;
    
    while (*c > tol)
    {
        *m += 1;
        d = alpha*(1+gamma);
        gamma = d/(1-d);
        cn = (1+gamma)*rho*(*c) - gamma*cp;
        cp = *c;
        *c = cn;
    }
}

/* 
  Eigenvalues evaluations by Lanczos algorithm
*/
uint lanczos(double **lambda, struct csr_mat *A, slong *gs_id,
             struct gs_data *gsh, struct gs_data *gsh_single)
{
    uint rn = A->rn;
    uint cn = A->cn;
    double *r = tmalloc(double, rn);

//    int seed = time(NULL);   // Better ?
//    srand(seed);

    uint i;
    for (i=0; i<rn; i++)
    {
        r[i] = (double)rand() / (double)RAND_MAX;
    }

    // Fixed length for max value of k
    uint kmax = 300;
    *lambda = tmalloc(double, kmax);
    double *l = *lambda;
    double *y = tmalloc(double, kmax);
    double *a = tmalloc(double, kmax);
    double *b = tmalloc(double, kmax);
    double *d = tmalloc(double, kmax+1);
    double *v = tmalloc(double, kmax);

    double beta = array_op(r, rn, norm2_op);
    double beta2 = beta*beta;
    gs(&beta2, gs_double, gs_add, 0, gsh_single, 0);
    beta = sqrt(beta2);

    uint k = 0; 
    double change = 0;

    // norm(A-speye(n),'fro') < 0.00000000001
    double *eye = tmalloc(double, rn);
    init_array(eye, rn, 1.);

    // Dummy matrix
    struct csr_mat *Acpy = tmalloc(struct csr_mat, 1);
    copy_csr(Acpy, A);

    // Frobenius norm
    diagcsr_op(Acpy, eye, dminus);

    // A assumed to be real ==> Frobenius norm is 2-norm of A->a
    double fronorm = array_op(Acpy->a, Acpy->row_off[rn], norm2_op);

    // Free matrix
    free(Acpy->row_off);
    free(Acpy->col);
    free(Acpy->a);
    free(Acpy);

    double fronorm2 = fronorm * fronorm;
    gs(&fronorm2, gs_double, gs_add, 0, gsh_single, 0);
    fronorm = sqrt(fronorm2);

    if (fronorm < 1e-11)
    {
        l[0] = 1;
        l[1] = 1;
        y[0] = 0;
        y[1] = 0;
        k = 2;
        change = 0;
    }
    else
    {
        change = 1;
    }

    // If n == 1
    uint rnglob = rn;
    gs(&rnglob, gs_int, gs_add, 0, gsh_single, 0);
    if (rnglob == 1)
    {
        double A00 = 0;

        // If global number of rows is one and process has one row
        if (rn == 1)
        {
            A00 = A->a[0];
        }
        
        gs(&A00, gs_double, gs_add, 0, gsh_single, 0);

        l[0] = A00;
        l[1] = A00;
        y[0] = 0;
        y[1] = 0;
        k = 2;
        change = 0; 
    }

    // While...
    double *qk = tmalloc(double, cn);
    init_array(qk, cn, 0.);
    double *qkm1 = tmalloc(double, rn);
    double *alphaqk = tmalloc(double, rn); // alpha * qk vector
    double *Aqk = tmalloc(double, rn); 
    uint na = 0, nb = 0;

    while (k < kmax && ( change > 1e-5 || y[0] > 1e-3 || y[k-1] > 1e-3))
    {
        k++;

        // qkm1 = qk
        memcpy(qkm1, qk, rn*sizeof(double));

        // qk = r/beta
        memcpy(qk, r, rn*sizeof(double));
        ar_scal_op(qk, 1./beta, rn, mult_op);
        gs(qk, gs_double, gs_add, 0, gsh, 0);

        // Aqk = A*qk
        apply_M(Aqk, 0, qk, 1, A, qk);

        // alpha = qk'*Aqk
        double alpha = vv_dot(qk, Aqk, rn); 
        gs(&alpha, gs_double, gs_add, 0, gsh_single, 0);

        //a = [a; alpha];
        a[na++] = alpha;

        // r = Aqk - alpha*qk - beta*qkm1
        memcpy(alphaqk, qk, rn*sizeof(double)); // alphaqk = qk
        ar_scal_op(alphaqk, alpha, rn, mult_op); // alphaqk = alpha*qk
        ar_scal_op(qkm1, beta, rn, mult_op); // qkm1 = beta*qkm1

        memcpy(r, Aqk, rn*sizeof(double)); // r = Aqk
        vv_op(r, alphaqk, rn, minus); // r = Aqk - alpha*qk
        vv_op(r, qkm1, rn, minus); // r = Aqk - alpha*qk - beta*qkm1
        
        if (k == 1)
        {
            l[0] = alpha;
            y[0] = 1;
        }
        else
        {
            double l0 = l[0];
            double lkm2 = l[k-2];
            // d
            //double *d = tmalloc(double, k+1);
            d[0] = 0;
            for (i=1; i<k; i++) d[i] = l[i-1];
            d[k] = 0;

            // v
            //double *v = tmalloc(double, k);
            v[0] = alpha;
            for (i=1; i<k; i++) v[i] = beta*y[i-1]; // y assumed to be real !!!

            tdeig(l, y, d, v, k-1);
  
            change = fabs(l0 - l[0]) + fabs(lkm2 - l[k-1]);
        }        

        beta = array_op(r, rn, norm2_op);
        beta2 = beta*beta;
        gs(&beta2, gs_double, gs_add, 0, gsh_single, 0);
        beta = sqrt(beta2);
        b[nb++] = beta;

        if (beta == 0) {break;}
    }

    uint n = 0;

    for (i=0; i<k; i++)
    {
        if (y[i] < 0.01)
        {
            (*lambda)[n++] = l[i];
        }
    }  

    free(r);
    free(eye);
    free(qk);
    free(qkm1);
    free(alphaqk);
    free(Aqk); 
    free(y);
    free(a);
    free(b);
    free(d);
    free(v);

    return n; 
}

/*
  TDEIG
*/

#define EPS (128*DBL_EPSILON)

/* minimizes cancellation error (but not round-off ...) */
static double sum_3(const double a, const double b, const double c)
{
  if     ( (a>=0 && b>=0) || (a<=0 && b<=0) ) return (a+b)+c;
  else if( (a>=0 && c>=0) || (a<=0 && c<=0) ) return (a+c)+b;
  else return a+(b+c);
}

/* solve     c
          - --- + b + a x == 0        with sign(x) = sign
             x
*/
static double rat_root(const double a, const double b, const double c,
                       const double sign)
{
  double bh = (fabs(b) + sqrt(b*b + 4*a*c))/2;
  return sign * (b*sign <= 0 ? bh/a : c/bh);
}

/*
  find d[ri] <= lambda <= d[ri+1]
  such that 0 = lambda - v[0] + \sum_i^n v[i]^2 / (d[i] - lambda)
*/
static double sec_root(double *y, const double *d, const double *v,
                       const int ri, const int n)
{
  double dl = d[ri], dr = d[ri+1], L = dr-dl;
  double x0l = L/2, x0r = -L/2;
  int i;
  double al, ar, bln, blp, brn, brp, cl, cr;
  double fn, fp, lambda0, lambda;
  double tol = L;
  if(fabs(dl)>tol) tol=fabs(dl);
  if(fabs(dr)>tol) tol=fabs(dr);
  tol *= EPS;
  for(;;) {
    if(fabs(x0l)==0 || x0l < 0) { *y=0; return dl; }
    if(fabs(x0r)==0 || x0r > 0) { *y=0; return dr; }
    lambda0 = fabs(x0l) < fabs(x0r) ? dl + x0l : dr + x0r;
    al = ar = cl = cr = bln = blp = brn = brp = 0;
    fn = fp = 0;
    for(i=1;i<=ri;++i) {
      double den = (d[i]-dl)-x0l;
      double fac = v[i]/den;
      double num = sum_3(d[i],-dr,-2*x0r);
      fn += v[i]*fac;
      fac *= fac;
      ar += fac;
      if(num > 0) brp += fac*num; else brn += fac*num;
      bln += fac*(d[i]-dl);
      cl  += fac*x0l*x0l;
    }
    for(i=ri+1;i<=n;++i) {
      double den = (d[i]-dr)-x0r;
      double fac = v[i]/den;
      double num = sum_3(d[i],-dl,-2*x0l);
      fp += v[i]*fac;
      fac *= fac;
      al += fac;
      if(num > 0) blp += fac*num; else bln += fac*num;
      brp += fac*(d[i]-dr);
      cr  += fac*x0r*x0r;
    }
    if(lambda0>0) fp+=lambda0; else fn+=lambda0;
    if(v[0]<0) fp-=v[0],blp-=v[0],brp-=v[0];
          else fn-=v[0],bln-=v[0],brn-=v[0];
    if(fp+fn > 0) { /* go left */
      x0l = rat_root(1+al,sum_3(dl,blp,bln),cl,1);
      lambda = dl + x0l;
      x0r = x0l - L;
    } else { /* go right */
      x0r = rat_root(1+ar,sum_3(dr,brp,brn),cr,-1);
      lambda = dr + x0r;
      x0l = x0r + L;
    }
    if( fabs(lambda-lambda0) < tol ) {
      double ty=0, fac;
      for(i=1;i<=ri;++i) fac = v[i]/((d[i]-dl)-x0l), ty += fac*fac;
      for(i=ri+1;i<=n;++i) fac = v[i]/((d[i]-dr)-x0r), ty += fac*fac;
      *y = 1/sqrt(1+ty);
      return lambda;
    }
  }
}

/*
  find the eigenvalues of
  
  d[1]           v[1]
       d[2]      v[2]
            d[n] v[n]
  v[1] v[2] v[n] v[0]
  
  sets d[0], d[n+1] to Gershgorin bounds
  
  also gives (n+1)th component of each orthonormal eigenvector in y
*/
static void tdeig(double *lambda, double *y, double *d, const double *v,
                  const int n)
{
  int i;
  double v1norm = 0, min=v[0], max=v[0];
  for(i=1;i<=n;++i) {
    double vi = fabs(v[i]), a=d[i]-vi, b=d[i]+vi;
    v1norm += vi;
    if(a<min) min=a;
    if(b>max) max=b;
  }
  d[0]   = v[0] - v1norm < min ? v[0] - v1norm : min;
  d[n+1] = v[0] + v1norm > max ? v[0] + v1norm : max;
  for(i=0;i<=n;++i) lambda[i] = sec_root(&y[i],d,v,i,n);
}

void coarsen(double *vc, struct csr_mat *A, double ctol, slong *gs_id,
    struct gs_data *gsh, struct gs_data *gsh_single)
{
    uint rn = A->rn, cn = A->cn;

    // D = diag(A)
    double *D = tmalloc(double, cn);
    diag(D, A);
    gs(D, gs_double, gs_add, 0, gsh, 0);

    // D = 1/sqrt(D)
    array_op(D, cn, sqrt_op);
    array_op(D, cn, minv_op);

    // S = abs(D*A*D)
    struct csr_mat *S = tmalloc(struct csr_mat, 1);
    copy_csr(S, A); // S = A
    diagcsr_op(S, D, dmult); // S = D*S
    diagcsr_op(S, D, multd); // S = S*D
    array_op(S->a, S->row_off[rn], abs_op); // S = abs(S)

    // S = S - diag(S)
    diag(D, S); // D = diag(S)
    diagcsr_op(S, D, dminus); // S = S - D

    // Free data not required anymore  
    free(D);

    /* Write out csr matrices for external tests (in matlab) */
    //write_mat(S, id_rows_owned, nnz, data);

    // vc = zeros(n, 1), vf = ones(n, 1)
    init_array(vc, rn, 0.); 
    int anyvc = 0; // = 0 if vc = all zeros / = 1 if at least one element is !=0
    double *vf = tmalloc(double, cn);
    init_array(vf, cn, 1.);
    
    // Other needed arrays
    double *g = tmalloc(double, cn);

    double *w1 = tmalloc(double, cn);
    double *w2 = tmalloc(double, cn);

    double *tmp = tmalloc(double, cn); // temporary array required for  
                                       // storing intermediate results
    double *w = tmalloc(double, rn);

    double *mask = tmalloc(double, rn);
    double *m = tmalloc(double, rn);

    while (1)
    {
        // w1 = vf.*(S*(vf.*(S*vf)))
        apply_M(g, 0, vf, 1., S, vf); // g = S*vf 
        vv_op(g, vf, rn, ewmult); // g = vf.*g
        gs(g, gs_double, gs_add, 0, gsh, 0);
        apply_M(w1, 0, g, 1., S, g); // w1 = S*g
        vv_op(w1, vf, rn, ewmult); // w1 = vf.*w1
        gs(w1, gs_double, gs_add, 0, gsh, 0);

        // w2 = vf.*(S *(vf.*(S*w1)))
        apply_M(w2, 0, w1, 1., S, w1); // w2 = S*w1
        vv_op(w2, vf, rn, ewmult); // w2 = vf.*w2
        gs(w2, gs_double, gs_add, 0, gsh, 0);
        apply_M(tmp, 0, w2, 1., S, w2); // tmp = S*w2
        memcpy(w2, tmp, rn*sizeof(double)); // tmp = w2
        vv_op(w2, vf, rn, ewmult); // w2 = vf.*w2

        // w = w2./w1
        memcpy(w, w1, rn*sizeof(double)); // w = w1
        array_op(w, rn, minv_op); // w = 1./w1
        vv_op(w, w2, rn, ewmult); // w = w2./w1

        uint i;
        for (i=0;i<rn;i++) // w(w1==0) = 0;
        {
            if (w1[i] == 0) w[i] = 0.;
        }

        // b = sqrt(min(max(w1),max(w)));
        double b, w1ml, w1m, wm; // w1m = max(w1), wm = max(w)
        uint mil, unused; // mi = index of w1m in w1
        extr_op(&w1ml, &mil, w1, rn, max);
        extr_op(&wm, &unused, w, rn, max);
        
        // Scatter global max to all processes
        w1m = w1ml;
        gs(&w1m, gs_double, gs_max, 0, gsh_single, 0);
        gs(&wm , gs_double, gs_max, 0, gsh_single, 0);

        b = (w1m < wm) ? sqrt(w1m) : sqrt(wm);

        // if b<=ctol; vc(mi)=true;
        if (b <= ctol)
        {
            if (anyvc == 0)
            {
                uint mi; // Global index for the max
                if (w1ml == w1m) // If local max is global mi = gs_id[mil]
                {
                    mi = (uint) gs_id[mil];
                }
                else // Else dummy value 
                {
                    mi = -1;
                }
                // We take the min global index to resolve ties
                gs(&mi, gs_int, gs_min, 0, gsh_single, 0);

                for (i=0;i<rn;i++)
                {
                    if (mi == (uint) gs_id[i]) {vc[i] = 1.; break;}
                }
            }
            break;
        }

        // mask = w > ctol^2;
        mask_op(mask, w, rn, ctol*ctol, gt);
 
        // m = mat_max(S,vf,mask.*g)
        double mat_max_tol = 0.1; // ==> hard-coded tolerance
        memcpy(tmp, g, rn*sizeof(double)); // tmp = g
            // g (needed later) copied into tmp
        vv_op(tmp, mask, rn, ewmult); // tmp = mask.*tmp (= mask.*g)        
        mat_max(m, S, vf, tmp, mat_max_tol, gsh); // m = mat_max(S,vf,mask.*g)
 
        // mask = mask & (g-m>=0)
        vv_op(g, m, rn, minus); // g = g - m
        mask_op(tmp, g, rn, 0., ge); // tmp = (g-m>=0)
        bin_op(mask, tmp, rn, and_op); // mask = mask & tmp;

        // m = mat_max(S,vf,mask.*id)
        for (i=0;i<rn;i++)
        {
            g[i] = (double)gs_id[i]; // g = (double) id
        }
        memcpy(tmp, mask, rn*sizeof(double)); // copy mask to tmp
        vv_op(tmp, g, rn, ewmult); // tmp = tmp.*g (= mask.*id)
        mat_max(m, S, vf, tmp, mat_max_tol, gsh); // m = mat_max(S,vf,mask.*g)

        // mask = mask & (id-m>0)
        vv_op(g, m, rn, minus); // id = id - m
        mask_op(tmp, g, rn, 0., gt); // tmp = (id-m>0)
        bin_op(mask, tmp, rn, and_op); // mask = mask & tmp;

        // vc = vc | mask ; vf = xor(vf, mask)
        bin_op(vc, mask, rn, or_op); // vc = vc | mask;
        if (anyvc == 0)
        {
            for (i=0;i<rn;i++) 
            {
                if (vc[i] == 1.)
                {
                    anyvc = 1;
                    break;
                }
            }
            gs(&anyvc , gs_int, gs_max, 0, gsh_single, 0);
        }
        bin_op(vf, mask, rn, xor_op); // vf = vf (xor) mask;
        
        gs(vf, gs_double, gs_add, 0, gsh, 0); // update vf
    }

    // Free S
    free(S->row_off);
    free(S->col);
    free(S->a);
    free(S);

    // Free arrays
    free(vf);
    free(w1);
    free(w2);
    free(w);
    free(g);
    free(mask);
    free(m);
    free(tmp);
}

/* Exctract sub-matrix 
   subA = A(vr, vc) */
void sub_mat(struct csr_mat *subA, struct csr_mat *A, double* vr, double *vc)
{
    uint rn = A->rn, cn = A->cn;
    uint *row_off = A->row_off, *col = A->col;
    double *a = A->a;

    uint subrn = 0, subcn = 0, subncol = 0;

    uint i, j;

    // Compute number of rows and of non-zero elements for sub-matrix
    for (i=0; i<rn; i++)
    {
        if (vr[i] != 0)
        {
            subrn += 1;
            uint je=row_off[i+1]; 
            for(j=row_off[i]; j<je; j++)
            {
                if (vc[col[j]] != 0)
                {
                    subncol += 1;
                }
            }
        }
    }

    // Compute cn for sub-matrix
    uint c = 0; // just a counter
    uint *g2lcol = tmalloc(uint, cn); // correspondence between local and global
                                      // column ids
    for (i=0; i<cn; i++)
    {
        if (vc[i] != 0)
        {
            subcn += 1;
            g2lcol[i] = c;
            c++;
        }
        else
        {
            g2lcol[i] = -1; // dummy because g2lcol is of type uint
        }
    }

    // Initialize and build sub-matrix
    subA->rn = subrn;
    subA->cn = subcn;

    subA->row_off = tmalloc(uint, subrn+1);
    subA->col     = tmalloc(uint, subncol);
    subA->a       = tmalloc(double, subncol);

    uint *subrow_off = subA->row_off;
    uint *subcol     = subA->col;
    double *suba     = subA->a;

    *subrow_off++ = 0;
    uint roffset = 0;

    for (i=0; i<rn; i++)
    {
        if (vr[i] != 0)
        {
            uint je=row_off[i+1]; 
            for(j=row_off[i]; j<je; j++)
            {
                if (vc[col[j]] != 0)
                {
                    roffset += 1;
                    *subcol++ = g2lcol[col[j]];
                    *suba++   = a[j];
                }
            }
            *subrow_off++ = roffset;
        }
    }

    free(g2lcol);
}

/* Exctract sub-vector of type double
   a = b(v) 
   n is length of b amd v */
void sub_vec(double *a, double *b, double* v, uint n)
{
    uint i;    
    for (i=0; i<n; i++)
    {
        if (v[i] != 0)
        {
            *a++ = b[i];  
        }
    }
}

/* Exctract sub-vector of type slong
   a = b(v) 
   n is length of b and v */
void sub_slong(slong *a, slong *b, double* v, uint n)
{
    uint i;    
    for (i=0; i<n; i++)
    {
        if (v[i] != 0)
        {
            *a++ = b[i];  
        }
    }
}

/* Vector-vector operations:
   a = a (op) b */
void vv_op(double *a, double *b, uint n, enum vv_ops op)
{
    uint i;
    switch(op)
    {
        case(plus):   for (i=0;i<n;i++) *a = *a + *b, a++, b++; break;
        case(minus):  for (i=0;i<n;i++) *a = *a - *b, a++, b++; break;
        case(ewmult): for (i=0;i<n;i++) *a = *a * (*b), a++, b++; break;
    }
}

// Dot product between two vectors
double vv_dot(double *a, double *b, uint n)
{
    double r = 0;
    uint i;
    for (i=0;i<n;i++)
    {
        r += (*a++)*(*b++);
    }

    return r;
}

/* Binary operations
   mask = mask (op) a */
void bin_op(double *mask, double *a, uint n, enum bin_ops op)
{
    uint i;
    switch(op)
    {
        case(and_op): for (i=0;i<n;i++) 
                      {
                          *mask = (*mask != 0. && *a != 0.) ? 1. : 0.; 
                          mask++, a++;
                      } 
                      break;
        case(or_op):  for (i=0;i<n;i++) 
                      {
                          *mask = (*mask == 0. && *a == 0.) ? 0. : 1.; 
                          mask++, a++; 
                      }
                      break;
        case(xor_op): for (i=0;i<n;i++) 
                      { 
                          *mask = ((*mask == 0. && *a != 0.) || 
                                   (*mask != 0. && *a == 0.)   ) ? 1. : 0.; 
                          mask++, a++; 
                      }
                      break;
        // mask = not(a)
        case(not_op): for (i=0;i<n;i++) 
                      { 
                          *mask = (*a == 0. ) ? 1. : 0.; 
                          mask++, a++; 
                      }
                      break;
    }
}

/* Mask operations */
void mask_op(double *mask, double *a, uint n, double trigger, enum mask_ops op)
{
    uint i;
    switch(op)
    {
        case(gt): for (i=0;i<n;i++) *mask++ = (*a++ > trigger)? 1. : 0.; break;
        case(lt): for (i=0;i<n;i++) *mask++ = (*a++ < trigger)? 1. : 0.; break;
        case(ge): for (i=0;i<n;i++) *mask++ = (*a++ >= trigger)? 1. : 0.; break;
        case(le): for (i=0;i<n;i++) *mask++ = (*a++ <= trigger)? 1. : 0.; break;
        case(eq): for (i=0;i<n;i++) *mask++ = (*a++ = trigger)? 1. : 0.; break;
    }
}

/* Extremum operations */
void extr_op(double *extr, uint *idx, double *a, uint n, enum extr_ops op)
{
    *extr = a[0];
    *idx = 0;
    uint i;
    switch(op)
    {
        case(max): for (i=1;i<n;i++) if (a[i] > *extr) *extr = a[i], *idx = i;
                   break;
        case(min): for (i=1;i<n;i++) if (a[i] < *extr) *extr = a[i], *idx = i;
                   break;
    }
}

/* Array operations */
// a[i] = op(a[i])
// i = 0, n-1
double array_op(double *a, uint n, enum array_ops op)
{
    double r = 0;
    uint i;
    switch(op)
    {
        case(abs_op):  for (i=0;i<n;i++) *a = fabs(*a), a++; break;
        case(sqrt_op): for (i=0;i<n;i++) *a = sqrt(*a), a++; break;
        case(minv_op): for (i=0;i<n;i++) *a = 1./(*a), a++; break;
        case(sqr_op): for (i=0;i<n;i++) *a = (*a)*(*a), a++; break;
        case(sum_op)  :  for (i=0;i<n;i++) r += *a++; break;
        case(norm2_op):  for (i=0;i<n;i++)
                         { 
                             r += (*a)*(*a);
                             a++; 
                         }
                         r = sqrt(r); 
                         break;
    }

    return r;
}

// Array initialisation
// a = v*ones(n, 1)
void init_array(double *a, uint n, double v)
{
    uint i;
    for (i=0;i<n;i++)
    {
        *a++ = v;
    }
}

// Operations between an array and a scalar
// a[i] = a[i] op scal
void ar_scal_op(double *a, double scal, uint n, enum ar_scal_ops op)
{
    uint i;
    switch(op)
    {
        case(mult_op)  :  for (i=0;i<n;i++) *a = (*a)*scal, a++; break;
    }
}

/*******************************************************************************
* Diagonal operations
*******************************************************************************/
/* Extract diagonal from square sparse matrix */
void diag(double *D, struct csr_mat *A)
{
    uint i; 
    uint rn=A->rn;
    uint *row_off = A->row_off, *col = A->col;
    double *a = A->a;
    
    for(i=0;i<rn;++i) 
    {
        uint j; 
        const uint jb=row_off[i], je=row_off[i+1];
        for(j=jb; j<je; ++j) 
        {  
            if (col[j] == i) 
            {
                *D++ = a[j]; 
                break;
            }
        }
    }
}

// Operations between csr and diagonal matrices
// Assumption: length(D) = A->cn
void diagcsr_op(struct csr_mat *A, double *D, enum diagcsr_ops op)
{
    uint i;
    uint rn = A->rn;
    uint *row_off = A->row_off, *col = A->col;
    double *a = A->a;

    switch(op)
    {
        case(dplus):   for (i=0;i<rn;i++) 
                      {
                          uint j; 
                          const uint jb=row_off[i], je=row_off[i+1];
                          for(j=jb; j<je; ++j) 
                          {
                              if (col[j] == i) 
                              {
                                  a[j] = a[j]+D[i]; 
                                  break;
                              }
                          }
                      }
                      break;
        case(dminus): for (i=0;i<rn;i++) 
                      {
                          uint j; 
                          const uint jb=row_off[i], je=row_off[i+1];
                          for(j=jb; j<je; ++j) 
                          {
                              if (col[j] == i) 
                              {
                                  a[j] = a[j]-D[i]; 
                                  break;
                              }
                          }
                      }
                      break;
        case(dmult):  for(i=0;i<rn;++i) 
                      {
                          uint j; 
                          const uint jb=row_off[i], je=row_off[i+1];
                          for(j=jb; j<je; ++j) 
                          {       
                              a[j] = a[j]*D[i];
                          }
                      }
                      break;
        case(multd):  for(i=0;i<rn;++i) 
                      {
                          uint j; 
                          const uint jb=row_off[i], je=row_off[i+1];
                          for(j=jb; j<je; ++j) 
                          {       
                              a[j] = a[j]*D[col[j]];
                          }
                      }
                      break;
    }
}

/*******************************************************************************
* Others
*******************************************************************************/
/* Copy sparse matrix A into B */
void copy_csr(struct csr_mat *B, struct csr_mat *A)
{
    B->rn = A->rn;
    B->cn = A->cn;
    B->row_off =tmalloc(uint, A->rn+1);
    uint nnz = A->row_off[A->rn];
    B->col = tmalloc(uint, nnz);
    B->a = tmalloc(double, nnz);

    memcpy(B->row_off, A->row_off, (A->rn+1)*sizeof(uint));
    memcpy(B->col, A->col, (nnz)*sizeof(uint));
    memcpy(B->a, A->a, (nnz)*sizeof(double));
}

/* Free csr matrices */
void csr_free(struct csr_mat **A)
{
     if (A) 
    {
        free((*A)->row_off);
        free((*A)->col);
        free((*A)->a);
        free(*A);
        *A = NULL;
     }
}

/* Build sparse matrix using the csr format
   Assumptions:
    - A->rn and A->cn are set already
    - coo_A is sorted
*/
void build_csr(struct csr_mat *A, struct coo_mat *coo_A, uint nnz,
    slong *gs_id)
{
    uint rn = A->rn, cn = A->cn;
    uint i, j;
    uint row_cur, row_prev = coo_A[0].i, counter = 1;

    A->row_off[0] = 0;
    
    for (i=0;i<nnz;i++)
    {
        // row_off
        row_cur = coo_A[i].i;
        if (row_cur != row_prev) A->row_off[counter++] = i;
        if (i == nnz-1) A->row_off[counter++] = nnz;
    
        row_prev = row_cur;
        
        // col /!\ col is not sorted!!!
        // local index of the column is computed
        slong *item;
        slong key = (slong)coo_A[i].j+1;
	    item = (slong*)bsearch(&key, gs_id, rn, sizeof(slong), comp_gs_id);
        if (item != NULL) A->col[i] = (uint)(item - gs_id);
	    else A->col[i] = (uint) ((slong*)bsearch(&key, gs_id+rn,
                             cn-rn, sizeof(slong), comp_gs_id) - gs_id);

        // a
        A->a[i] = coo_A[i].v;
    }
}

int comp_gs_id(const void *a, const void *b)
{
    if (abs(*(slong*)a) > abs(*(slong*)b)) return 1;
    else if (abs(*(slong*)a) < abs(*(slong*)b)) return -1;
    else return 0;
}

int comp_coo_v (const void * a, const void * b)
{
    if (((struct coo_mat*)a)->v > ((struct coo_mat*)b)->v) return 1;
    else if (((struct coo_mat*)a)->v < ((struct coo_mat*)b)->v) return -1;
    else return 0;
} 

/* Unused but could be useful
int comp_coo_i (const void * a, const void * b)
{
    if (((struct coo_mat*)a)->i > ((struct coo_mat*)b)->i) return 1;
    else if (((struct coo_mat*)a)->i < ((struct coo_mat*)b)->i) return -1;
    else return 0;
} 

int comp_coo_j (const void * a, const void * b)
{
    if (((struct coo_mat*)a)->j > ((struct coo_mat*)b)->j) return 1;
    else if (((struct coo_mat*)a)->j < ((struct coo_mat*)b)->j) return -1;
    else return 0;
}
*/

/* Sort coo matrix by rows first then by columns */
int comp_coo_ij (const void * a, const void * b)
{
    if (  ((struct coo_mat*)a)->i >  ((struct coo_mat*)b)->i  ||
        ( ((struct coo_mat*)a)->i == ((struct coo_mat*)b)->i  &&
          ((struct coo_mat*)a)->j >  ((struct coo_mat*)b)->j )  ) return 1;
    else if (((struct coo_mat*)a)->i == ((struct coo_mat*)b)->i &&
             ((struct coo_mat*)a)->j == ((struct coo_mat*)b)->j) return 0;
    else return -1;
}

/* Sort coo matrix by columns first then by rows */
int comp_coo_ji (const void * a, const void * b)
{
    if (  ((struct coo_mat*)a)->j >  ((struct coo_mat*)b)->j  ||
        ( ((struct coo_mat*)a)->j == ((struct coo_mat*)b)->j  &&
          ((struct coo_mat*)a)->i >  ((struct coo_mat*)b)->i )  ) return 1;
    else if (((struct coo_mat*)a)->i == ((struct coo_mat*)b)->i &&
             ((struct coo_mat*)a)->j == ((struct coo_mat*)b)->j) return 0;
    else return -1;
}

/* Function based on what is done in amg.c in order to dump data to Matlab.
   Build the coarse assembled matrix and an id array for the gather-scatter */
void build_setup_data(struct csr_mat *A, slong **gs_id, uint n, const ulong
    *id, uint nz_unassembled, const uint *Ai, const uint* Aj, const double *Av,   
    struct crs_data *data)
{
    struct crystal cr;
    struct array uid; 
    struct rid *rid_map = tmalloc(struct rid,n);
    struct array mat;
    uint k; 
    struct rnz *out;

    crystal_init(&cr, &data->comm);
    assign_dofs(&uid,rid_map, id,n,data->comm.id,data->gs_top,&cr.data);
    array_init(struct rnz,&mat,nz_unassembled);
    for(out=mat.ptr,k=0;k<nz_unassembled;++k) {
    uint i=Ai[k], j=Aj[k]; double a=Av[k];
    if(id[i]==0 || id[j]==0 || fabs(a)==0) continue;
    out->v = a, out->i=rid_map[i], out->j=rid_map[j];
    ++out;
    }
    mat.n = out-(struct rnz*)mat.ptr;
    free(rid_map);

//////////////////////////////////

    const ulong *const uid_ptr = uid.ptr;
    const struct comm *comm = &(&cr)->comm;
    const uint pid = comm->id, np = comm->np;
    uint i,nnz;
    struct array nonlocal_id;
    const struct rnz *nz, *enz;
    const struct labelled_rid *rlbl;

    mat_distribute(&mat,row_distr,col_major,&cr);
    nnz = mat.n;

    mat_list_nonlocal_sorted(&nonlocal_id,&mat,row_distr,uid_ptr,&cr);

    rlbl = nonlocal_id.ptr;

    struct coo_mat *coo_A = tmalloc(struct coo_mat, nnz);
    struct coo_mat *coo_A_ptr = coo_A;

    for(nz=mat.ptr,enz=nz+nnz;nz!=enz;++nz) 
    {
        coo_A_ptr->i = (uint)uid_ptr[nz->i.i]-1; // -1 because C convention
        coo_A_ptr->v = nz->v; 
        if(nz->j.p==pid)
        {
            coo_A_ptr->j = (uint)uid_ptr[nz->j.i]-1; // -1 because C convention
        }
        else 
        {
            const uint jp = nz->j.p, ji = nz->j.i;
            while(rlbl->rid.p<jp) ++rlbl;
            if(rlbl->rid.p!=jp) printf("Error when assembling matrix\n");
            while(rlbl->rid.i<ji) ++rlbl;
            if(rlbl->rid.i!=ji) printf("Error when assembling matrix\n");
            coo_A_ptr->j = rlbl->id-1; // -1 because C convention
        }
        coo_A_ptr++;
    }
    // Free data not necessary anymore
    array_free(&uid);
    array_free(&mat);
    array_free(&nonlocal_id);
    crystal_free(&cr);

    /* Sort matrix by rows first then by columns */
    qsort(coo_A, nnz, sizeof(struct coo_mat), comp_coo_ij);

    /* Extract sorted global ids of rows and columns */
    uint *rows = tmalloc(uint, nnz);
    uint *cols = tmalloc(uint, nnz);

    for(i=0;i<nnz;i++)
    {
        rows[i] = coo_A[i].i;
        cols[i] = coo_A[i].j;
    }   

    /* Extract unique ids as well as numbers of rows and columns */
    uint rn, cn;
    rn = remdup(rows, nnz);
    qsort(cols, nnz, sizeof(uint), comp_uint);
    cn = remdup(cols, nnz);

    /* Build gs_id, the array of global ids for gs communication */
    *gs_id = tmalloc(slong, cn);

    // Variables for position of the ids owned and not owned in gs_id
    uint p = 0, q = rn;

    for (i=0;i<cn;i++)
    {
        if (cols[i] == rows[p])  (*gs_id)[p++] = (slong)(cols[i]+1);
        else (*gs_id)[q++] =  -(slong)(cols[i]+1);
    }

    free(rows);
    free(cols);

    /* Malloc and build sparse matrix using csr format */
    A->rn = rn;
    A->cn = cn;
    A->row_off = tmalloc(uint, rn+1);
    A->col = tmalloc(uint, nnz);
    A->a = tmalloc(double, nnz);

    build_csr(A, coo_A, nnz, *gs_id);

    free(coo_A);
}

/* Comparison function for uint */
int comp_uint (const void * a, const void * b)
{
    if ( *(uint*)a > *(uint*)b ) return 1;
    else if ( *(uint*)a < *(uint*)b ) return -1;
    else return 0;
}

/* Remove duplicates from a sorted list */
static uint remdup(uint *array, uint size)
{
    uint i;
    uint last = 0;

    for (i = 1; i < size; i++)
    {
        if (array[i] != array[last])
            array[++last] = array[i];
    }
    return(last + 1);
}

// C-MEX FUNCTIONS TRANSFORMED TO C
/* /!\ Contrary to the original mex function, this one is valid for a square 
   symmetric matrix A only 
   - x and y are local
   - f is global    
*/
static void mat_max(double *y, struct csr_mat *A, double *f, double *x, 
    double tol, struct gs_data *gsh)
{
    uint i, rn = A->rn, cn = A->cn;
    double *yg = tmalloc(double, cn); // global version of y

    for(i=0;i<cn;++i) 
    {
        yg[i] = -DBL_MAX;
    }

    for(i=0;i<rn;++i) 
    {
        double xj = *x++;
        uint j, jb = A->row_off[i], je = A->row_off[i+1];
        double Amax = 0;
        for(j=jb;j<je;++j)
            if(f[A->col[j]] != 0 && fabs(A->a[j])>Amax) Amax=fabs(A->a[j]);
        Amax *= tol;
        for(j=jb;j<je;++j) 
        {
            uint k = A->col[j];
            if(f[k] == 0 || fabs(A->a[j]) < Amax) continue;
            if(xj>yg[k]) yg[k]=xj;
        }
    }
    
    // Vector y contains maximum values associated with local rows only
    // ==> gather-scatter is required !
    gs(yg, gs_double, gs_max, 1, gsh, 0); // gather max
    gs(yg, gs_double, gs_max, 0, gsh, 0); // scatter max

    memcpy(y, yg, rn*sizeof(double));
    free(yg);
}
