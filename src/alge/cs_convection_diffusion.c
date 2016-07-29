/*============================================================================
 * Convection-diffusion operators.
 *============================================================================*/

/*
  This file is part of Code_Saturne, a general-purpose CFD tool.

  Copyright (C) 1998-2016 EDF S.A.

  This program is free software; you can redistribute it and/or modify it under
  the terms of the GNU General Public License as published by the Free Software
  Foundation; either version 2 of the License, or (at your option) any later
  version.

  This program is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
  details.

  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
  Street, Fifth Floor, Boston, MA 02110-1301, USA. */

/*----------------------------------------------------------------------------*/

#include "cs_defs.h"

/*----------------------------------------------------------------------------
 * Standard C library headers
 *----------------------------------------------------------------------------*/

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <float.h>

#if defined(HAVE_MPI)
#include <mpi.h>
#endif

/*----------------------------------------------------------------------------
 *  Local headers
 *----------------------------------------------------------------------------*/

#include "bft_error.h"
#include "bft_mem.h"
#include "bft_printf.h"

#include "cs_blas.h"
#include "cs_halo.h"
#include "cs_halo_perio.h"
#include "cs_log.h"
#include "cs_math.h"
#include "cs_mesh.h"
#include "cs_field.h"
#include "cs_field_operator.h"
#include "cs_field_pointer.h"
#include "cs_gradient.h"
#include "cs_gradient_perio.h"
#include "cs_ext_neighborhood.h"
#include "cs_mesh_quantities.h"
#include "cs_parall.h"
#include "cs_parameters.h"
#include "cs_prototypes.h"
#include "cs_timer.h"
#include "cs_stokes_model.h"
#include "cs_boundary_conditions.h"

/*----------------------------------------------------------------------------
 *  Header for the current file
 *----------------------------------------------------------------------------*/

#include "cs_convection_diffusion.h"

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*=============================================================================
 * Additional Doxygen documentation
 *============================================================================*/

/*! \file  cs_convection_diffusion.c

*/

/*! \cond DOXYGEN_SHOULD_SKIP_THIS */

/*=============================================================================
 * Local Macro Definitions
 *============================================================================*/

/*=============================================================================
 * Local type definitions
 *============================================================================*/

/*============================================================================
 * Private function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------
 * Return pointer to slope test indicator field values if active.
 *
 * parameters:
 *   f_id        <-- field id (or -1)
 *   var_cal_opt <-- variable calculation options
 *
 * return:
 *   pointer to local values array, or NULL;
 *----------------------------------------------------------------------------*/

static cs_real_t  *_get_v_slope_test(int                       f_id,
                                     const cs_var_cal_opt_t    var_cal_opt)
{
  const int iconvp = var_cal_opt.iconv;
  const int isstpp = var_cal_opt.isstpc;
  const double blencp = var_cal_opt.blencv;

  cs_real_t  *v_slope_test = NULL;

  if (f_id > -1 && iconvp > 0 && blencp > 0. && isstpp == 0) {

    static int _k_slope_test_f_id = -1;

    cs_field_t *f = cs_field_by_id(f_id);

    int f_track_slope_test_id = -1;

    if (_k_slope_test_f_id < 0)
      _k_slope_test_f_id = cs_field_key_id_try("slope_test_upwind_id");
    if (_k_slope_test_f_id > -1 && isstpp == 0)
      f_track_slope_test_id = cs_field_get_key_int(f, _k_slope_test_f_id);

    if (f_track_slope_test_id > -1)
    v_slope_test = (cs_field_by_id(f_track_slope_test_id))->val;

    if (v_slope_test != NULL) {
      const cs_lnum_t n_cells_ext = cs_glob_mesh->n_cells_with_ghosts;
#     pragma omp parallel for
      for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++)
        v_slope_test[cell_id] = 0.;
    }

  }

  return v_slope_test;
}

/*----------------------------------------------------------------------------
 * Return the denominator to build the Min/max limiter.
 *
 * parameters:
 *   f_id        <-- field id (or -1)
 *   inc         <-- 0 if an increment, 1 otherwise
 *   denom_inf   --> computed denominator for the inferior bound
 *   denom_sup   --> computed denominator for the superior bound
 *
 * return:
 *   pointer to local values array, or NULL;
 *----------------------------------------------------------------------------*/

static void
_max_limiter_denom(const int              f_id,
                   const int              inc,
                   cs_real_t    *restrict denom_inf,
                   cs_real_t    *restrict denom_sup)
{
  const cs_mesh_t  *m = cs_glob_mesh;
  cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  const cs_lnum_t n_cells_ext = m->n_cells_with_ghosts;
  const int n_i_groups = m->i_face_numbering->n_groups;
  const int n_i_threads = m->i_face_numbering->n_threads;
  const cs_lnum_t *restrict i_group_index = m->i_face_numbering->group_index;

  const cs_lnum_2_t *restrict i_face_cells
    = (const cs_lnum_2_t *restrict)m->i_face_cells;
  const cs_real_t *restrict weight = fvq->weight;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)fvq->cell_cen;
  const cs_real_3_t *restrict i_face_cog
    = (const cs_real_3_t *restrict)fvq->i_face_cog;
  const cs_real_3_t *restrict dijpf
    = (const cs_real_3_t *restrict)fvq->dijpf;

  /* Get option from the field */
  cs_field_t *f = cs_field_by_id(f_id);

  const cs_real_t *restrict pvar  = f->val;
  const cs_real_t *restrict pvara = f->val_pre;

  const cs_real_t *restrict coefap = f->bc_coeffs->a;
  const cs_real_t *restrict coefbp = f->bc_coeffs->b;

  int key_cal_opt_id = cs_field_key_id("var_cal_opt");
  cs_var_cal_opt_t var_cal_opt;

  cs_field_get_key_struct(f, key_cal_opt_id, &var_cal_opt);

  const int ischcp = var_cal_opt.ischcv;
  const int ircflp = var_cal_opt.ircflu;
  const cs_real_t thetap = var_cal_opt.thetav;
  const cs_real_t blencp = var_cal_opt.blencv;

  const int kimasf = cs_field_key_id("inner_mass_flux_id");
  const int kbmasf = cs_field_key_id("boundary_mass_flux_id");
  const cs_real_t *restrict i_massflux =
    cs_field_by_id(cs_field_get_key_int(f, kimasf))->val;
  const cs_real_t *restrict b_massflux =
    cs_field_by_id(cs_field_get_key_int(f, kbmasf))->val;

  //Step 1: Building of the upwind gradient if needed
  cs_real_3_t *grdpa; // For the Implicit part
  cs_real_3_t *grdpaa;// For the Explicit part

  BFT_MALLOC(grdpa, n_cells_ext, cs_real_3_t);
  BFT_MALLOC(grdpaa, n_cells_ext, cs_real_3_t);

# pragma omp parallel for
  for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++) {
    grdpa[cell_id][0] = 0.;
    grdpa[cell_id][1] = 0.;
    grdpa[cell_id][2] = 0.;

    grdpaa[cell_id][0] = 0.;
    grdpaa[cell_id][1] = 0.;
    grdpaa[cell_id][2] = 0.;
  }

  /* Choose gradient type */
  cs_halo_type_t halo_type = CS_HALO_STANDARD;
  cs_gradient_type_t gradient_type = CS_GRADIENT_ITER;

  /* SOLU Scheme asked with standard gradient
   *  or CENTERED scheme */
  if (ischcp == 0 || ischcp == 1) {

    cs_gradient_type_by_imrgra(var_cal_opt.imrgra,
                               &gradient_type,
                               &halo_type);

    cs_field_gradient_scalar(f,
                             false, /* use_previous_t */
                             gradient_type,
                             halo_type,
                             inc,
                             true, /* _recompute_cocg */
                             grdpa);

    cs_field_gradient_scalar(f,
                             true, /* use_previous_t */
                             gradient_type,
                             halo_type,
                             inc,
                             true, /* _recompute_cocg */
                             grdpaa);

  }
  /* pure SOLU scheme without using gradient_slope_test function*/
  else if (ischcp == 2) {

    cs_upwind_gradient(f_id,
                       inc,
                       halo_type,
                       coefap,
                       coefbp,
                       i_massflux,
                       b_massflux,
                       pvar,
                       grdpa);

    cs_upwind_gradient(f_id,
                       inc,
                       halo_type,
                       coefap,
                       coefbp,
                       i_massflux,
                       b_massflux,
                       pvara,
                       grdpaa);

  }

  /* Step 2: Building of denominator */

# pragma omp parallel for
  for (cs_lnum_t ii = 0; ii < n_cells_ext; ii++) {
    denom_inf[ii] = 0.;
    denom_sup[ii] = 0.;
  }

  /* ---> Contribution from interior faces */

  for (int g_id = 0; g_id < n_i_groups; g_id++) {
#   pragma omp parallel for
    for (int t_id = 0; t_id < n_i_threads; t_id++) {
      for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
          face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
          face_id++) {

        cs_lnum_t ii = i_face_cells[face_id][0];
        cs_lnum_t jj = i_face_cells[face_id][1];

        cs_real_t pi = pvar[ii];
        cs_real_t pj = pvar[jj];
        cs_real_t pia = pvara[ii];
        cs_real_t pja = pvara[jj];
        cs_real_t pif, pjf, pip, pjp;
        cs_real_t pifa, pjfa, pipa, pjpa;

        /* Value at time n */
        cs_i_cd_unsteady(ircflp,
                         ischcp,
                         blencp,
                         weight[face_id],
                         cell_cen[ii],
                         cell_cen[jj],
                         i_face_cog[face_id],
                         dijpf[face_id],
                         grdpa[ii], /* Std gradient when needed */
                         grdpa[jj], /* Std gradient when needed */
                         grdpa[ii], /* Upwind gradient when needed */
                         grdpa[jj], /* Upwind gradient when needed */
                         pi,
                         pj,
                         &pif,
                         &pjf,
                         &pip,
                         &pjp);

        /* Value at time n-1 */
        cs_i_cd_unsteady(ircflp,
                         ischcp,
                         blencp,
                         weight[face_id],
                         cell_cen[ii],
                         cell_cen[jj],
                         i_face_cog[face_id],
                         dijpf[face_id],
                         grdpaa[ii], /* Std gradient when needed */
                         grdpaa[jj], /* Std gradient when needed */
                         grdpaa[ii], /* Upwind gradient when needed */
                         grdpaa[jj], /* Upwind gradient when needed */
                         pia,
                         pja,
                         &pifa,
                         &pjfa,
                         &pipa,
                         &pjpa);


        cs_real_t flui = 0.5*(i_massflux[face_id] +fabs(i_massflux[face_id]));
        cs_real_t fluj = 0.5*(i_massflux[face_id] -fabs(i_massflux[face_id]));

        cs_real_t flux =     thetap  * ( (pif  - pi )*flui
                                       + (pjf  - pj )*fluj)
                       + (1.-thetap) * ( (pifa - pia)*flui
                                       + (pjfa - pja)*fluj);

        /* blending to prevent INFERIOR bound violation
          We need to take the positive part*/
        cs_real_t partii = 0.5*(flux + CS_ABS(flux));
        cs_real_t partjj = 0.5*(flux - CS_ABS(flux));

        denom_inf[ii] = denom_inf[ii] + partii;
        denom_inf[jj] = denom_inf[jj] - partjj;

        /* blending to prevent SUPERIOR bound violation
          We need to take the negative part
          Note: the swap between ii and jj is due to the fact
          that an upwind value on (Y-bound) is equivalent to a
          downwind value on (bound-Y) */
        denom_sup[ii] = denom_sup[ii] - partjj;
        denom_sup[jj] = denom_sup[jj] + partii;
      }
    }
  }

  //Free Gradient arrays
  BFT_FREE(grdpa);
  BFT_FREE(grdpaa);

}

/*----------------------------------------------------------------------------
 * Return the diagonal part of the numerator to build the Min/max limiter.
 *
 * parameters:
 *   f_id        <-- field id (or -1)
 *   rovsdt      <-- rho * volume / dt
 *   num_inf     --> computed numerator for the inferior bound
 *   num_sup     --> computed numerator for the superior bound
 *
 *----------------------------------------------------------------------------*/

static void
_max_limiter_num(const int           f_id,
                 const int           inc,
                 const cs_real_t     rovsdt[],
                 cs_real_t          *num_inf,
                 cs_real_t          *num_sup)
{
  const cs_mesh_t  *m = cs_glob_mesh;

  const cs_lnum_t n_cells = m->n_cells;
  const cs_lnum_t n_cells_ext = m->n_cells_with_ghosts;
  const int n_i_groups = m->i_face_numbering->n_groups;
  const int n_i_threads = m->i_face_numbering->n_threads;
  const int n_b_groups = m->b_face_numbering->n_groups;
  const int n_b_threads = m->b_face_numbering->n_threads;
  const cs_lnum_t *restrict i_group_index = m->i_face_numbering->group_index;
  const cs_lnum_t *restrict b_group_index = m->b_face_numbering->group_index;

  const cs_lnum_2_t *restrict i_face_cells
    = (const cs_lnum_2_t *restrict)m->i_face_cells;
  const cs_lnum_t *restrict b_face_cells
    = (const cs_lnum_t *restrict)m->b_face_cells;

  /* Get option from the field */
  cs_field_t *f = cs_field_by_id(f_id);

  const cs_real_t *restrict pvara = f->val_pre;

  const cs_real_t *restrict coefap = f->bc_coeffs->a;
  const cs_real_t *restrict coefbp = f->bc_coeffs->b;

  int key_scamax_id = cs_field_key_id("max_scalar");
  int key_scamin_id = cs_field_key_id("min_scalar");

  cs_real_t scalar_max = cs_field_get_key_double(f, key_scamax_id);
  cs_real_t scalar_min = cs_field_get_key_double(f, key_scamin_id);

  int key_cal_opt_id = cs_field_key_id("var_cal_opt");
  cs_var_cal_opt_t var_cal_opt;

  cs_field_get_key_struct(f, key_cal_opt_id, &var_cal_opt);

  const cs_real_t thetex =  1.-var_cal_opt.thetav;

  const int kimasf = cs_field_key_id("inner_mass_flux_id");
  const int kbmasf = cs_field_key_id("boundary_mass_flux_id");
  const cs_real_t *restrict i_massflux =
    cs_field_by_id( cs_field_get_key_int(f, kimasf) )->val;
  const cs_real_t *restrict b_massflux =
    cs_field_by_id( cs_field_get_key_int(f, kbmasf) )->val;

  for (cs_lnum_t ii = n_cells; ii < n_cells_ext; ii++) {
    num_inf[ii] = 0.;
    num_sup[ii] = 0.;
  }

# pragma omp parallel for
  for (cs_lnum_t ii = 0; ii < n_cells; ii++) {
    num_inf[ii] = rovsdt[ii] * (pvara[ii] -scalar_min);
    num_sup[ii] = rovsdt[ii] * (scalar_max-pvara[ii]);
  }

  /* ---> Contribution from interior faces */

  for (int g_id = 0; g_id < n_i_groups; g_id++) {
#   pragma omp parallel for
    for (int t_id = 0; t_id < n_i_threads; t_id++) {
      for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
          face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
          face_id++) {

        cs_lnum_t ii = i_face_cells[face_id][0];
        cs_lnum_t jj = i_face_cells[face_id][1];

        cs_real_t flui = 0.5*(i_massflux[face_id] + fabs(i_massflux[face_id]));
        cs_real_t fluj = 0.5*(i_massflux[face_id] - fabs(i_massflux[face_id]));

        cs_real_t pi = pvara[ii]-scalar_min;
        cs_real_t pj = pvara[jj]-scalar_min;

        num_inf[ii] -= thetex *(pi * flui + pj * fluj);
        num_inf[jj] += thetex *(pj * fluj + pi * flui);

        pi = scalar_max-pvara[ii];
        pj = scalar_max-pvara[jj];

        num_sup[ii] -= thetex *(pi * flui + pj * fluj);
        num_sup[jj] += thetex *(pj * fluj + pi * flui);
      }
    }
  }

  /* ---> Contribution from boundary faces */

  for (int g_id = 0; g_id < n_b_groups; g_id++) {
#   pragma omp parallel for
    for (int t_id = 0; t_id < n_b_threads; t_id++) {
      for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
          face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
          face_id++) {

        cs_lnum_t ii = b_face_cells[face_id];

        cs_real_t flui = 0.5*(b_massflux[face_id]+fabs(b_massflux[face_id]));
        cs_real_t fluf = 0.5*(b_massflux[face_id]-fabs(b_massflux[face_id]));
        cs_real_t pfabor = inc*coefap[face_id]+coefbp[face_id]*pvara[ii];

        num_inf[ii] -= thetex *( (pvara[ii]-scalar_min) * flui
                               + (pfabor   -scalar_min) * fluf);
        num_sup[ii] -= thetex *( (scalar_max-pvara[ii]) * flui
                               + (scalar_max-pfabor   ) * fluf);
      }
    }
  }
}


/*! (DOXYGEN_SHOULD_SKIP_THIS) \endcond */

/*----------------------------------------------------------------------------*/
/*!
 * \brief Compute the minmod limiter function
 *
 * \param[in]     r            r=downstream_slope/face_slope
                               in a structured 1D mesh, for face "i+1/2" and
                               for positive mass flux, it could represent:
                               (Y_{i+2}-Y_{i+1})/(Y_{i+1}-Y_{i})
 */
/*----------------------------------------------------------------------------*/

static cs_real_t cs_limiter_minmod(cs_real_t r)
{
  cs_real_t phi = CS_MAX(0.,CS_MIN(1,r));
  return phi;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Compute the Van-Leer limiter function
 *
 * \param[in]     r             = downstream_slope/face_slope
                               in a structured 1D mesh, for face "i+1/2" and
                               for positive mass flux, it could represent:
                               (Y_{i+2}-Y_{i+1})/(Y_{i+1}-Y_{i})
 */
/*----------------------------------------------------------------------------*/

static
cs_real_t cs_limiter_van_leer(cs_real_t  r)
{
  cs_real_t phi;
  phi = (r + CS_ABS(r)) / (1. + r);
  return phi;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Compute the Van-Albada limiter function
 *
 * \param[in]     r            r = downstream_slope/face_slope
                               in a structured 1D mesh, for face "i+1/2" and
                               for positive mass flux, it could represent:
                               (Y_{i+2}-Y_{i+1})/(Y_{i+1}-Y_{i})
 */
/*----------------------------------------------------------------------------*/

static
cs_real_t cs_limiter_van_albada(cs_real_t  r)
{
  cs_real_t phi;
  phi = CS_MAX(0., (r * (1. + r)) / (1. + pow(r, 2.)));
  return phi;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Compute the Van-Leer limiter function
 *
 * \param[in]     r            r = downstream_slope/face_slope
                               in a structured 1D mesh, for face "i+1/2" and
                               for positive mass flux, it could represent:
                               (Y_{i+2}-Y_{i+1})/(Y_{i+1}-Y_{i})
 */
/*----------------------------------------------------------------------------*/

static
cs_real_t cs_limiter_superbee(cs_real_t  r)
{
  cs_real_t phi;
  phi = CS_MAX(0., CS_MAX( CS_MIN(2.*r,1.), CS_MIN(2., r)));
  return phi;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Compute different types of "limiter function" according to the method
    proposed by Roe-Sweby (Second Order Upwind TVD schemes)
 *
 * \param[in]     limiter      choice of the limiter function
 * \param[in]     r            r = downstream_slope/face_slope
                               in a structured 1D mesh, for face "i+1/2" and
                               for positive mass flux, it could represent:
                               (Y_{i+2}-Y_{i+1})/(Y_{i+1}-Y_{i})
 */
/*----------------------------------------------------------------------------*/

cs_real_t cs_limiter_function(const int   limiter,
                              cs_real_t   r)
{

  cs_real_t phi;

  /* minmod limiter */
  if (limiter == 0) {
    phi = cs_limiter_minmod(r);
  }
  /* Van-Leer limiter */
  else if (limiter == 1) {
    phi = cs_limiter_van_leer(r);
  }
  /* Van-Albada limiter */
  else if (limiter == 2) {
    phi = cs_limiter_van_albada(r);
  }
  /* Superbee limiter */
  else if (limiter == 3) {
    phi = cs_limiter_superbee(r);
  }
  /* Lax Wendroff */
  else {
    phi = 1.;
  }

  return phi;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Compute The Upstream "Y_{U}" Value of a scalar in order to use it in new limiters
    or slope tests.
 *
 * \param[in]     p_c                  current  point value for the face
 * \param[in]     c_vol                current  volume (related to current cell)
 * \param[in]     surf                 surface of the studied face
 * \param[in]     normal               surf*normal vector of the face
 * \param[in]     gradup               upwind gradient of the current point
 */
/*----------------------------------------------------------------------------*/

static cs_real_t
cs_upstream_val(const cs_real_t                     p_c,
                const cs_real_t                     c_vol,
                const cs_real_t                     surf,
                const cs_real_3_t                   normal,
                const cs_real_3_t                   gradup)
{
  cs_real_t p_u;
  p_u = p_c - c_vol/pow(surf, 2)*(  normal[0]*gradup[0]
                                  + normal[1]*gradup[1]
                                  + normal[2]*gradup[2]);
  return p_u;
}

/*============================================================================
 * Public function definitions for Fortran API
 *============================================================================*/

/*----------------------------------------------------------------------------
 * Wrapper to cs_face_diffusion_potential
 *----------------------------------------------------------------------------*/

void CS_PROCF (itrmas, ITRMAS)
(
 const cs_int_t  *const   f_id,
 const cs_int_t  *const   init,
 const cs_int_t  *const   inc,
 const cs_int_t  *const   imrgra,
 const cs_int_t  *const   iccocg,
 const cs_int_t  *const   nswrgp,
 const cs_int_t  *const   imligp,
 const cs_int_t  *const   iphydp,
 const cs_int_t  *const   iwarnp,
 const cs_real_t *const   epsrgp,
 const cs_real_t *const   climgp,
 const cs_real_t *const   extrap,
 cs_real_3_t              frcxt[],
 cs_real_t                pvar[],
 const cs_real_t          coefap[],
 const cs_real_t          coefbp[],
 const cs_real_t          cofafp[],
 const cs_real_t          cofbfp[],
 const cs_real_t          i_visc[],
 const cs_real_t          b_visc[],
 cs_real_t                viselx[],
 const cs_real_t          visely[],
 const cs_real_t          viselz[],
 cs_real_t                i_massflux[],
 cs_real_t                b_massflux[]
)
{
  const cs_mesh_t  *m = cs_glob_mesh;
  cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  cs_face_diffusion_potential(*f_id,
                              m,
                              fvq,
                              *init,
                              *inc,
                              *imrgra,
                              *iccocg,
                              *nswrgp,
                              *imligp,
                              *iphydp,
                              *iwarnp,
                              *epsrgp,
                              *climgp,
                              *extrap,
                              frcxt,
                              pvar,
                              coefap,
                              coefbp,
                              cofafp,
                              cofbfp,
                              i_visc,
                              b_visc,
                              viselx,
                              visely,
                              viselz,
                              i_massflux,
                              b_massflux);
}

/*----------------------------------------------------------------------------
 * Wrapper to cs_face_anisotropic_diffusion_potential
 *----------------------------------------------------------------------------*/

void CS_PROCF (itrmav, ITRMAV)
(
 const cs_int_t  *const   f_id,
 const cs_int_t  *const   init,
 const cs_int_t  *const   inc,
 const cs_int_t  *const   imrgra,
 const cs_int_t  *const   iccocg,
 const cs_int_t  *const   nswrgp,
 const cs_int_t  *const   imligp,
 const cs_int_t  *const   ircflp,
 const cs_int_t  *const   iphydp,
 const cs_int_t  *const   iwarnp,
 const cs_real_t *const   epsrgp,
 const cs_real_t *const   climgp,
 const cs_real_t *const   extrap,
 cs_real_3_t              frcxt[],
 cs_real_t                pvar[],
 const cs_real_t          coefap[],
 const cs_real_t          coefbp[],
 const cs_real_t          cofafp[],
 const cs_real_t          cofbfp[],
 const cs_real_t          i_visc[],
 const cs_real_t          b_visc[],
 cs_real_6_t              viscel[],
 const cs_real_2_t        weighf[],
 const cs_real_t          weighb[],
 cs_real_t                i_massflux[],
 cs_real_t                b_massflux[]
)
{
  const cs_mesh_t  *m = cs_glob_mesh;
  cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  cs_face_anisotropic_diffusion_potential(*f_id,
                                          m,
                                          fvq,
                                          *init,
                                          *inc,
                                          *imrgra,
                                          *iccocg,
                                          *nswrgp,
                                          *imligp,
                                          *ircflp,
                                          *iphydp,
                                          *iwarnp,
                                          *epsrgp,
                                          *climgp,
                                          *extrap,
                                          frcxt,
                                          pvar,
                                          coefap,
                                          coefbp,
                                          cofafp,
                                          cofbfp,
                                          i_visc,
                                          b_visc,
                                          viscel,
                                          weighf,
                                          weighb,
                                          i_massflux,
                                          b_massflux);
}

/*----------------------------------------------------------------------------
 * Wrapper to cs_diffusion_potential
 *----------------------------------------------------------------------------*/

void CS_PROCF (itrgrp, ITRGRP)
(
 const cs_int_t  *const   f_id,
 const cs_int_t  *const   init,
 const cs_int_t  *const   inc,
 const cs_int_t  *const   imrgra,
 const cs_int_t  *const   iccocg,
 const cs_int_t  *const   nswrgp,
 const cs_int_t  *const   imligp,
 const cs_int_t  *const   iphydp,
 const cs_int_t  *const   iwarnp,
 const cs_real_t *const   epsrgp,
 const cs_real_t *const   climgp,
 const cs_real_t *const   extrap,
 cs_real_3_t              frcxt[],
 cs_real_t                pvar[],
 const cs_real_t          coefap[],
 const cs_real_t          coefbp[],
 const cs_real_t          cofafp[],
 const cs_real_t          cofbfp[],
 const cs_real_t          i_visc[],
 const cs_real_t          b_visc[],
 const cs_real_t          viselx[],
 const cs_real_t          visely[],
 const cs_real_t          viselz[],
 cs_real_t                diverg[]
)
{
  const cs_mesh_t  *m = cs_glob_mesh;
  cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  cs_diffusion_potential(*f_id,
                         m,
                         fvq,
                         *init,
                         *inc,
                         *imrgra,
                         *iccocg,
                         *nswrgp,
                         *imligp,
                         *iphydp,
                         *iwarnp,
                         *epsrgp,
                         *climgp,
                         *extrap,
                         frcxt,
                         pvar,
                         coefap,
                         coefbp,
                         cofafp,
                         cofbfp,
                         i_visc,
                         b_visc,
                         viselx,
                         visely,
                         viselz,
                         diverg);
}

/*----------------------------------------------------------------------------
 * Wrapper to cs_anisotropic_diffusion_potential
 *----------------------------------------------------------------------------*/

void CS_PROCF (itrgrv, ITRGRV)
(
 const cs_int_t  *const   f_id,
 const cs_int_t  *const   init,
 const cs_int_t  *const   inc,
 const cs_int_t  *const   imrgra,
 const cs_int_t  *const   iccocg,
 const cs_int_t  *const   nswrgp,
 const cs_int_t  *const   imligp,
 const cs_int_t  *const   ircflp,
 const cs_int_t  *const   iphydp,
 const cs_int_t  *const   iwarnp,
 const cs_real_t *const   epsrgp,
 const cs_real_t *const   climgp,
 const cs_real_t *const   extrap,
 cs_real_3_t              frcxt[],
 cs_real_t                pvar[],
 const cs_real_t          coefap[],
 const cs_real_t          coefbp[],
 const cs_real_t          cofafp[],
 const cs_real_t          cofbfp[],
 const cs_real_t          i_visc[],
 const cs_real_t          b_visc[],
 cs_real_6_t              viscel[],
 const cs_real_2_t        weighf[],
 const cs_real_t          weighb[],
 cs_real_t                diverg[]
)
{
  const cs_mesh_t  *m = cs_glob_mesh;
  cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  cs_anisotropic_diffusion_potential(*f_id,
                                     m,
                                     fvq,
                                     *init,
                                     *inc,
                                     *imrgra,
                                     *iccocg,
                                     *nswrgp,
                                     *imligp,
                                     *ircflp,
                                     *iphydp,
                                     *iwarnp,
                                     *epsrgp,
                                     *climgp,
                                     *extrap,
                                     frcxt,
                                     pvar,
                                     coefap,
                                     coefbp,
                                     cofafp,
                                     cofbfp,
                                     i_visc,
                                     b_visc,
                                     viscel,
                                     weighf,
                                     weighb,
                                     diverg);
}

/*============================================================================
 * Public function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief Compute the upwind gradient used in the slope tests.
 *
 * \param[in]     f_id         field index
 * \param[in]     inc          Not an increment flag
 * \param[in]     halo_type    halo type
 * \param[in]     grad         standard gradient
 * \param[out]    grdpa        upwind gradient
 * \param[in]     pvar         values
 * \param[in]     coefap       boundary condition array for the variable
 *                             (explicit part)
 * \param[in]     coefbp       boundary condition array for the variable
 *                             (implicit part)
 * \param[in]     i_massflux   mass flux at interior faces
 */
/*----------------------------------------------------------------------------*/

void
cs_slope_test_gradient(const int               f_id,
                       const int               inc,
                       const cs_halo_type_t    halo_type,
                       cs_real_3_t            *grad,
                       cs_real_3_t            *grdpa,
                       cs_real_t              *pvar,
                       const cs_real_t        *coefap,
                       const cs_real_t        *coefbp,
                       const cs_real_t        *i_massflux)
{
  const cs_mesh_t  *m = cs_glob_mesh;
  const cs_halo_t  *halo = m->halo;
  cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  const cs_lnum_t n_cells = m->n_cells;

  const cs_lnum_2_t *restrict i_face_cells
    = (const cs_lnum_2_t *restrict)m->i_face_cells;
  const cs_lnum_t *restrict b_face_cells
    = (const cs_lnum_t *restrict)m->b_face_cells;
  const cs_real_t *restrict cell_vol = fvq->cell_vol;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)fvq->cell_cen;
  const cs_real_3_t *restrict i_face_normal
    = (const cs_real_3_t *restrict)fvq->i_face_normal;
  const cs_real_3_t *restrict b_face_normal
    = (const cs_real_3_t *restrict)fvq->b_face_normal;
  const cs_real_3_t *restrict i_face_cog
    = (const cs_real_3_t *restrict)fvq->i_face_cog;
  const cs_real_3_t *restrict diipb
    = (const cs_real_3_t *restrict)fvq->diipb;

  const int n_i_groups = m->i_face_numbering->n_groups;
  const int n_i_threads = m->i_face_numbering->n_threads;
  const int n_b_groups = m->b_face_numbering->n_groups;
  const int n_b_threads = m->b_face_numbering->n_threads;
  const cs_lnum_t *restrict i_group_index = m->i_face_numbering->group_index;
  const cs_lnum_t *restrict b_group_index = m->b_face_numbering->group_index;

  for (int g_id = 0; g_id < n_i_groups; g_id++) {
#   pragma omp parallel for
    for (int t_id = 0; t_id < n_i_threads; t_id++) {
      for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
           face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
           face_id++) {

        cs_lnum_t ii = i_face_cells[face_id][0];
        cs_lnum_t jj = i_face_cells[face_id][1];

        cs_real_t difx = i_face_cog[face_id][0] - cell_cen[ii][0];
        cs_real_t dify = i_face_cog[face_id][1] - cell_cen[ii][1];
        cs_real_t difz = i_face_cog[face_id][2] - cell_cen[ii][2];
        cs_real_t djfx = i_face_cog[face_id][0] - cell_cen[jj][0];
        cs_real_t djfy = i_face_cog[face_id][1] - cell_cen[jj][1];
        cs_real_t djfz = i_face_cog[face_id][2] - cell_cen[jj][2];

        cs_real_t pif =   pvar[ii]
                        + difx*grad[ii][0]+dify*grad[ii][1]+difz*grad[ii][2];
        cs_real_t pjf =   pvar[jj]
                        + djfx*grad[jj][0]+djfy*grad[jj][1]+djfz*grad[jj][2];

        cs_real_t pfac = pjf;
        if (i_massflux[face_id] > 0.) pfac = pif;

        cs_real_t pfac1 = pfac*i_face_normal[face_id][0];
        cs_real_t pfac2 = pfac*i_face_normal[face_id][1];
        cs_real_t pfac3 = pfac*i_face_normal[face_id][2];

        grdpa[ii][0] = grdpa[ii][0] + pfac1;
        grdpa[ii][1] = grdpa[ii][1] + pfac2;
        grdpa[ii][2] = grdpa[ii][2] + pfac3;

        grdpa[jj][0] = grdpa[jj][0] - pfac1;
        grdpa[jj][1] = grdpa[jj][1] - pfac2;
        grdpa[jj][2] = grdpa[jj][2] - pfac3;

      }
    }
  }

  for (int g_id = 0; g_id < n_b_groups; g_id++) {
#   pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
    for (int t_id = 0; t_id < n_b_threads; t_id++) {
      for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
           face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
           face_id++) {

        cs_lnum_t ii = b_face_cells[face_id];

        cs_real_t diipbx = diipb[face_id][0];
        cs_real_t diipby = diipb[face_id][1];
        cs_real_t diipbz = diipb[face_id][2];
        cs_real_t pfac =   inc*coefap[face_id]
                         + coefbp[face_id] * (pvar[ii] + diipbx*grad[ii][0]
                                                       + diipby*grad[ii][1]
                                                      + diipbz*grad[ii][2]);
        grdpa[ii][0] = grdpa[ii][0] + pfac*b_face_normal[face_id][0];
        grdpa[ii][1] = grdpa[ii][1] + pfac*b_face_normal[face_id][1];
        grdpa[ii][2] = grdpa[ii][2] + pfac*b_face_normal[face_id][2];

      }
    }
  }

# pragma omp parallel for
  for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {

    cs_real_t unsvol = 1./cell_vol[cell_id];

    grdpa[cell_id][0] = grdpa[cell_id][0]*unsvol;
    grdpa[cell_id][1] = grdpa[cell_id][1]*unsvol;
    grdpa[cell_id][2] = grdpa[cell_id][2]*unsvol;

  }

  /* Synchronization for parallelism or periodicity */

  if (halo != NULL) {
    cs_halo_sync_var_strided(halo, halo_type, (cs_real_t *)grdpa, 3);
    if (cs_glob_mesh->n_init_perio > 0)
      cs_halo_perio_sync_var_vect(halo, halo_type, (cs_real_t *)grdpa, 3);

    /* Gradient periodicity of rotation for Reynolds stress components */
    if (cs_glob_mesh->have_rotation_perio > 0 && f_id != -1)
      cs_gradient_perio_process_rij(&f_id, grdpa);
  }

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Compute the upwind gradient in order to cope with SOLU schemes
 *        observed in the litterature.
 *
 * \param[in]     f_id         field index
 * \param[in]     inc          Not an increment flag
 * \param[in]     halo_type    halo type
 * \param[in]     coefap       boundary condition array for the variable
 *                             (explicit part)
 * \param[in]     coefbp       boundary condition array for the variable
 *                             (implicit part)
 * \param[in]     i_massflux   mass flux at interior faces
 * \param[in]     b_massflux   mass flux at boundary faces
 * \param[in]     pvar         values
 * \param[out]    grdpa        upwind gradient
 */
/*----------------------------------------------------------------------------*/

void
cs_upwind_gradient(const int                     f_id,
                   const int                     inc,
                   const cs_halo_type_t          halo_type,
                   const cs_real_t               coefap[],
                   const cs_real_t               coefbp[],
                   const cs_real_t               i_massflux[],
                   const cs_real_t               b_massflux[],
                   const cs_real_t     *restrict pvar,
                   cs_real_3_t         *restrict grdpa)
{
  const cs_mesh_t  *m = cs_glob_mesh;
  const cs_halo_t  *halo = m->halo;
  cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  const cs_lnum_t n_cells = m->n_cells;

  const cs_lnum_2_t *restrict i_face_cells
    = (const cs_lnum_2_t *restrict)m->i_face_cells;
  const cs_lnum_t *restrict b_face_cells
    = (const cs_lnum_t *restrict)m->b_face_cells;
  const cs_real_t *restrict cell_vol = fvq->cell_vol;
  const cs_real_3_t *restrict i_face_normal
    = (const cs_real_3_t *restrict)fvq->i_face_normal;
  const cs_real_3_t *restrict b_face_normal
    = (const cs_real_3_t *restrict)fvq->b_face_normal;

  const int n_i_groups = m->i_face_numbering->n_groups;
  const int n_i_threads = m->i_face_numbering->n_threads;
  const int n_b_groups = m->b_face_numbering->n_groups;
  const int n_b_threads = m->b_face_numbering->n_threads;
  const cs_lnum_t *restrict i_group_index = m->i_face_numbering->group_index;
  const cs_lnum_t *restrict b_group_index = m->b_face_numbering->group_index;

  for (int g_id = 0; g_id < n_i_groups; g_id++) {
#   pragma omp parallel for
    for (int t_id = 0; t_id < n_i_threads; t_id++) {
      for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
           face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
           face_id++) {

        cs_lnum_t ii = i_face_cells[face_id][0];
        cs_lnum_t jj = i_face_cells[face_id][1];

        cs_real_t pif = pvar[ii];
        cs_real_t pjf = pvar[jj];

        cs_real_t pfac = pjf;
        if (i_massflux[face_id] > 0.) pfac = pif;

        cs_real_t pfac1 = pfac*i_face_normal[face_id][0];
        cs_real_t pfac2 = pfac*i_face_normal[face_id][1];
        cs_real_t pfac3 = pfac*i_face_normal[face_id][2];

        grdpa[ii][0] = grdpa[ii][0] + pfac1;
        grdpa[ii][1] = grdpa[ii][1] + pfac2;
        grdpa[ii][2] = grdpa[ii][2] + pfac3;

        grdpa[jj][0] = grdpa[jj][0] - pfac1;
        grdpa[jj][1] = grdpa[jj][1] - pfac2;
        grdpa[jj][2] = grdpa[jj][2] - pfac3;

      }
    }
  }

  for (int g_id = 0; g_id < n_b_groups; g_id++) {
#   pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
    for (int t_id = 0; t_id < n_b_threads; t_id++) {
      for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
           face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
           face_id++) {

        cs_lnum_t ii = b_face_cells[face_id];

        cs_lnum_t pfac = pvar[ii];

        if (b_massflux[face_id] < 0)
          pfac = inc*coefap[face_id] + coefbp[face_id] * pvar[ii];

        grdpa[ii][0] = grdpa[ii][0] + pfac*b_face_normal[face_id][0];
        grdpa[ii][1] = grdpa[ii][1] + pfac*b_face_normal[face_id][1];
        grdpa[ii][2] = grdpa[ii][2] + pfac*b_face_normal[face_id][2];

      }
    }
  }

# pragma omp parallel for
  for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {

    cs_real_t unsvol = 1./cell_vol[cell_id];

    grdpa[cell_id][0] = grdpa[cell_id][0]*unsvol;
    grdpa[cell_id][1] = grdpa[cell_id][1]*unsvol;
    grdpa[cell_id][2] = grdpa[cell_id][2]*unsvol;

  }

  /* Synchronization for parallelism or periodicity */

  if (halo != NULL) {
    cs_halo_sync_var_strided(halo, halo_type, (cs_real_t *)grdpa, 3);
    if (cs_glob_mesh->n_init_perio > 0)
      cs_halo_perio_sync_var_vect(halo, halo_type, (cs_real_t *)grdpa, 3);

    /* Gradient periodicity of rotation for Reynolds stress components */
    if (cs_glob_mesh->have_rotation_perio > 0 && f_id != -1)
      cs_gradient_perio_process_rij(&f_id, grdpa);
  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Compute the upwind gradient used in the slope tests.
 *
 * \param[in]     inc          Not an increment flag
 * \param[in]     halo_type    halo type
 * \param[in]     grad         standard gradient
 * \param[out]    grdpa        upwind gradient
 * \param[in]     pvar         values
 * \param[in]     coefa        boundary condition array for the variable
                               (Explicit part)
 * \param[in]     coefb        boundary condition array for the variable
                              (Implicit part)
 * \param[in]     i_massflux   mass flux at interior faces
 */
/*----------------------------------------------------------------------------*/

void
cs_slope_test_gradient_vector(const int              inc,
                              const cs_halo_type_t   halo_type,
                              cs_real_33_t          *grad,
                              cs_real_33_t          *grdpa,
                              cs_real_3_t           *pvar,
                              const cs_real_3_t     *coefa,
                              const cs_real_33_t    *coefb,
                              const cs_real_t       *i_massflux)
{
  const cs_mesh_t  *m = cs_glob_mesh;
  const cs_halo_t  *halo = m->halo;
  cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  const cs_lnum_t n_cells = m->n_cells;

  const cs_lnum_2_t *restrict i_face_cells
    = (const cs_lnum_2_t *restrict)m->i_face_cells;
  const cs_lnum_t *restrict b_face_cells
    = (const cs_lnum_t *restrict)m->b_face_cells;
  const cs_real_t *restrict cell_vol = fvq->cell_vol;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)fvq->cell_cen;
  const cs_real_3_t *restrict i_f_face_normal
    = (const cs_real_3_t *restrict)fvq->i_f_face_normal;
  const cs_real_3_t *restrict b_f_face_normal
    = (const cs_real_3_t *restrict)fvq->b_f_face_normal;
  const cs_real_3_t *restrict i_face_cog
    = (const cs_real_3_t *restrict)fvq->i_face_cog;
  const cs_real_3_t *restrict diipb
    = (const cs_real_3_t *restrict)fvq->diipb;

  const int n_i_groups = m->i_face_numbering->n_groups;
  const int n_i_threads = m->i_face_numbering->n_threads;
  const int n_b_groups = m->b_face_numbering->n_groups;
  const int n_b_threads = m->b_face_numbering->n_threads;
  const cs_lnum_t *restrict i_group_index = m->i_face_numbering->group_index;
  const cs_lnum_t *restrict b_group_index = m->b_face_numbering->group_index;

  for (int g_id = 0; g_id < n_i_groups; g_id++) {
#   pragma omp parallel for
    for (int t_id = 0; t_id < n_i_threads; t_id++) {
      for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
           face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
           face_id++) {

        cs_real_t difv[3], djfv[3];

        cs_lnum_t ii = i_face_cells[face_id][0];
        cs_lnum_t jj = i_face_cells[face_id][1];

        for (int jsou = 0; jsou < 3; jsou++) {
          difv[jsou] = i_face_cog[face_id][jsou] - cell_cen[ii][jsou];
          djfv[jsou] = i_face_cog[face_id][jsou] - cell_cen[jj][jsou];
        }

        /* x-y-z component, p = u, v, w */

        for (int isou = 0; isou < 3; isou++) {
          cs_real_t pif = pvar[ii][isou];
          cs_real_t pjf = pvar[jj][isou];
          for (int jsou = 0; jsou < 3; jsou++) {
            pif = pif + grad[ii][isou][jsou]*difv[jsou];
            pjf = pjf + grad[jj][isou][jsou]*djfv[jsou];
          }

          cs_real_t pfac = pjf;
          if (i_massflux[face_id] > 0.) pfac = pif;

          /* U gradient */

          cs_real_t vfac[3];

          for (int jsou = 0; jsou < 3; jsou++) {
            vfac[jsou] = pfac*i_f_face_normal[face_id][jsou];

            grdpa[ii][isou][jsou] = grdpa[ii][isou][jsou] + vfac[jsou];
            grdpa[jj][isou][jsou] = grdpa[jj][isou][jsou] - vfac[jsou];
          }
        }

      }
    }
  }

  for (int g_id = 0; g_id < n_b_groups; g_id++) {
#   pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
    for (int t_id = 0; t_id < n_b_threads; t_id++) {
      for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
           face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
           face_id++) {

        cs_real_t diipbv[3];
        cs_lnum_t ii = b_face_cells[face_id];

        for (int jsou = 0; jsou < 3; jsou++)
          diipbv[jsou] = diipb[face_id][jsou];

        /* x-y-z components, p = u, v, w */

        for (int isou = 0; isou < 3; isou++) {
          cs_real_t pfac = inc*coefa[face_id][isou];
          /*coefu is a matrix */
          for (int jsou =  0; jsou < 3; jsou++)
            pfac += coefb[face_id][jsou][isou]*(  pvar[ii][jsou]
                                                + grad[ii][jsou][0]*diipbv[0]
                                                + grad[ii][jsou][1]*diipbv[1]
                                                + grad[ii][jsou][2]*diipbv[2]);

          for (int jsou = 0; jsou < 3; jsou++)
            grdpa[ii][isou][jsou] += pfac*b_f_face_normal[face_id][jsou];
        }

      }
    }
  }

# pragma omp parallel for
  for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
    cs_real_t unsvol = 1./cell_vol[cell_id];
    for (int isou = 0; isou < 3; isou++) {
      for (int jsou = 0; jsou < 3; jsou++)
        grdpa[cell_id][isou][jsou] = grdpa[cell_id][isou][jsou]*unsvol;
    }
  }

  /* Handle parallelism and periodicity */

  if (halo != NULL) {
    cs_halo_sync_var_strided(halo, halo_type, (cs_real_t *)grdpa, 9);
    if (m->n_init_perio > 0)
      cs_halo_perio_sync_var_sym_tens(halo, halo_type, (cs_real_t *)grdpa);
  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Compute the upwind gradient used in the slope tests.
 *
 * \param[in]     inc          Not an increment flag
 * \param[in]     halo_type    halo type
 * \param[in]     grad         standard gradient
 * \param[out]    grdpa        upwind gradient
 * \param[in]     pvar         values
 * \param[in]     coefa        boundary condition array for the variable
                               (Explicit part)
 * \param[in]     coefb        boundary condition array for the variable
                              (Implicit part)
 * \param[in]     i_massflux   mass flux at interior faces
 */
/*----------------------------------------------------------------------------*/

void
cs_slope_test_gradient_tensor(const int              inc,
                              const cs_halo_type_t   halo_type,
                              cs_real_63_t          *grad,
                              cs_real_63_t          *grdpa,
                              cs_real_6_t           *pvar,
                              const cs_real_6_t     *coefa,
                              const cs_real_66_t    *coefb,
                              const cs_real_t       *i_massflux)
{
  const cs_mesh_t  *m = cs_glob_mesh;
  const cs_halo_t  *halo = m->halo;
  cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  const cs_lnum_t n_cells = m->n_cells;

  const cs_lnum_2_t *restrict i_face_cells
    = (const cs_lnum_2_t *restrict)m->i_face_cells;
  const cs_lnum_t *restrict b_face_cells
    = (const cs_lnum_t *restrict)m->b_face_cells;
  const cs_real_t *restrict cell_vol = fvq->cell_vol;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)fvq->cell_cen;
  const cs_real_3_t *restrict i_f_face_normal
    = (const cs_real_3_t *restrict)fvq->i_f_face_normal;
  const cs_real_3_t *restrict b_f_face_normal
    = (const cs_real_3_t *restrict)fvq->b_f_face_normal;
  const cs_real_3_t *restrict i_face_cog
    = (const cs_real_3_t *restrict)fvq->i_face_cog;
  const cs_real_3_t *restrict diipb
    = (const cs_real_3_t *restrict)fvq->diipb;

  const int n_i_groups = m->i_face_numbering->n_groups;
  const int n_i_threads = m->i_face_numbering->n_threads;
  const int n_b_groups = m->b_face_numbering->n_groups;
  const int n_b_threads = m->b_face_numbering->n_threads;
  const cs_lnum_t *restrict i_group_index = m->i_face_numbering->group_index;
  const cs_lnum_t *restrict b_group_index = m->b_face_numbering->group_index;

  for (int g_id = 0; g_id < n_i_groups; g_id++) {
#   pragma omp parallel for
    for (int t_id = 0; t_id < n_i_threads; t_id++) {
      for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
           face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
           face_id++) {

        cs_real_t difv[3], djfv[3];

        cs_lnum_t ii = i_face_cells[face_id][0];
        cs_lnum_t jj = i_face_cells[face_id][1];

        for (int jsou = 0; jsou < 3; jsou++) {
          difv[jsou] = i_face_cog[face_id][jsou] - cell_cen[ii][jsou];
          djfv[jsou] = i_face_cog[face_id][jsou] - cell_cen[jj][jsou];
        }

        /* x-y-z component, p = u, v, w */

        for (int isou = 0; isou < 6; isou++) {
          cs_real_t pif = pvar[ii][isou];
          cs_real_t pjf = pvar[jj][isou];
          for (int jsou = 0; jsou < 3; jsou++) {
            pif = pif + grad[ii][isou][jsou]*difv[jsou];
            pjf = pjf + grad[jj][isou][jsou]*djfv[jsou];
          }

          cs_real_t pfac = pjf;
          if (i_massflux[face_id] > 0.) pfac = pif;

          /* U gradient */

          cs_real_t vfac[3];

          for (int jsou = 0; jsou < 3; jsou++) {
            vfac[jsou] = pfac*i_f_face_normal[face_id][jsou];

            grdpa[ii][isou][jsou] = grdpa[ii][isou][jsou] + vfac[jsou];
            grdpa[jj][isou][jsou] = grdpa[jj][isou][jsou] - vfac[jsou];
          }
        }

      }
    }
  }

  for (int g_id = 0; g_id < n_b_groups; g_id++) {
#   pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
    for (int t_id = 0; t_id < n_b_threads; t_id++) {
      for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
           face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
           face_id++) {

        cs_real_t diipbv[3];
        cs_lnum_t ii = b_face_cells[face_id];

        for (int jsou = 0; jsou < 3; jsou++)
          diipbv[jsou] = diipb[face_id][jsou];

        /* x-y-z components, p = u, v, w */

        for (int isou = 0; isou < 6; isou++) {
          cs_real_t pfac = inc*coefa[face_id][isou];
          /*coefu is a matrix */
          for (int jsou =  0; jsou < 6; jsou++)
            pfac += coefb[face_id][jsou][isou]*(  pvar[ii][jsou]
                                                + grad[ii][jsou][0]*diipbv[0]
                                                + grad[ii][jsou][1]*diipbv[1]
                                                + grad[ii][jsou][2]*diipbv[2]);

          for (int jsou = 0; jsou < 3; jsou++)
            grdpa[ii][isou][jsou] += pfac*b_f_face_normal[face_id][jsou];
        }

      }
    }
  }

# pragma omp parallel for
  for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
    cs_real_t unsvol = 1./cell_vol[cell_id];
    for (int isou = 0; isou < 6; isou++) {
      for (int jsou = 0; jsou < 3; jsou++)
        grdpa[cell_id][isou][jsou] = grdpa[cell_id][isou][jsou]*unsvol;
    }
  }

  /* Handle parallelism and periodicity */

  if (halo != NULL) {
    cs_halo_sync_var_strided(halo, halo_type, (cs_real_t *)grdpa, 18);
    if (m->n_init_perio > 0)
      cs_halo_perio_sync_var_sym_tens(halo, halo_type, (cs_real_t *)grdpa);
  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Compute a coefficient for blending that ensures the positivity
 *  of the scalar.
 *
 * \param[in]     f_id         field id
 * \param[in]     inc          "not an increment" flag
 * \param[in]     rovsdt       rho * volume / dt
 */
/*----------------------------------------------------------------------------*/

void
cs_max_limiter_building(int              f_id,
                        int              inc,
                        const cs_real_t  rovsdt[])
{
  const cs_mesh_t *m = cs_glob_mesh;
  const cs_halo_t *halo = m->halo;
  const cs_lnum_t n_cells = m->n_cells;
  const cs_lnum_t n_cells_ext = m->n_cells_with_ghosts;

  /* Get options from the field */

  cs_field_t *f = cs_field_by_id(f_id);
  int key_cal_opt_id = cs_field_key_id("var_cal_opt");
  cs_var_cal_opt_t var_cal_opt;

  cs_field_get_key_struct(f, key_cal_opt_id, &var_cal_opt);

  if (var_cal_opt.isstpc != 2)
    return;

  cs_real_t* cpro_beta = cs_field_by_id(
      cs_field_get_key_int(f, cs_field_key_id("convection_limiter_id")))->val;

  cs_real_t* denom_inf;
  cs_real_t* num_inf;
  cs_real_t* denom_sup;
  cs_real_t* num_sup;

  BFT_MALLOC(denom_inf, n_cells_ext, cs_real_t);
  BFT_MALLOC(denom_sup, n_cells_ext, cs_real_t);
  BFT_MALLOC(num_inf, n_cells_ext, cs_real_t);
  BFT_MALLOC(num_sup, n_cells_ext, cs_real_t);

  /* First Step: Treatment of the denominator for the inferior and superior bound */

  _max_limiter_denom(f_id,
                     inc,
                     denom_inf,
                     denom_sup);

  /* Second Step: Treatment of the numenator for the inferior and superior bound */

  _max_limiter_num(f_id,
                   inc,
                   rovsdt,
                   num_inf,
                   num_sup);

# pragma omp parallel for
  for (cs_lnum_t ii = 0; ii < n_cells; ii++) {

    /* Treatment of the inferior bound */
    cs_real_t beta_inf;
    if (denom_inf[ii] <= num_inf[ii]) {
      beta_inf = 1.;
    }
    else if (denom_inf[ii] <= CS_ABS(num_inf[ii])) {
      beta_inf = -1.;
    }
    else {
      beta_inf = num_inf[ii]/denom_inf[ii]; //FIXME division by 0
      beta_inf = CS_MIN(beta_inf, 1.);
    }

    /* Treatment of the superior bound */
    cs_real_t beta_sup;
    if (denom_sup[ii] <= num_sup[ii]) {
      beta_sup = 1.;
    }
    else if (denom_sup[ii] <= CS_ABS(num_sup[ii])) {
      beta_sup = -1.;
    }
    else {
      beta_sup = num_sup[ii]/denom_sup[ii]; //FIXME division by 0
      beta_sup = CS_MIN(beta_sup, 1.);
    }

    cpro_beta[ii] = CS_MIN(beta_inf, beta_sup);
  }

  /* Synchronize variable */
  if (halo != NULL)
    cs_halo_sync_var(halo, CS_HALO_STANDARD, cpro_beta);

  BFT_FREE(denom_inf);
  BFT_FREE(num_inf);
  BFT_FREE(denom_sup);
  BFT_FREE(num_sup);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Add the explicit part of the convection/diffusion terms of a
 * standard transport equation of a scalar field \f$ \varia \f$.
 *
 * More precisely, the right hand side \f$ Rhs \f$ is updated as
 * follows:
 * \f[
 * Rhs = Rhs - \sum_{\fij \in \Facei{\celli}}      \left(
 *        \dot{m}_\ij \left( \varia_\fij - \varia_\celli \right)
 *      - \mu_\fij \gradv_\fij \varia \cdot \vect{S}_\ij  \right)
 * \f]
 *
 * Warning:
 * - \f$ Rhs \f$ has already been initialized before calling bilsc2!
 * - mind the sign minus
 *
 * \param[in]     idtvar        indicator of the temporal scheme
 * \param[in]     f_id          field id (or -1)
 * \param[in]     var_cal_opt   variable calculation options
 * \param[in]     icvflb        global indicator of boundary convection flux
 *                               - 0 upwind scheme at all boundary faces
 *                               - 1 imposed flux at some boundary faces
 * \param[in]     inc           indicator
 *                               - 0 when solving an increment
 *                               - 1 otherwise
 * \param[in]     iccocg        indicator
 *                               - 1 re-compute cocg matrix
 *                                   (for iterative gradients)
 *                               - 0 otherwise
 * \param[in]     imasac        take mass accumulation into account?
 * \param[in]     pvar          solved variable (current time step)
 * \param[in]     pvara         solved variable (previous time step)
 * \param[in]     icvfli        boundary face indicator array of convection flux
 *                               - 0 upwind scheme
 *                               - 1 imposed flux
 * \param[in]     coefap        boundary condition array for the variable
 *                               (explicit part)
 * \param[in]     coefbp        boundary condition array for the variable
 *                               (implicit part)
 * \param[in]     cofafp        boundary condition array for the diffusion
 *                               of the variable (explicit part)
 * \param[in]     cofbfp        boundary condition array for the diffusion
 *                               of the variable (implicit part)
 * \param[in]     i_massflux    mass flux at interior faces
 * \param[in]     b_massflux    mass flux at boundary faces
 * \param[in]     i_visc        \f$ \mu_\fij \dfrac{S_\fij}{\ipf \jpf} \f$
 *                               at interior faces for the r.h.s.
 * \param[in]     b_visc        \f$ \mu_\fib \dfrac{S_\fib}{\ipf \centf} \f$
 *                               at border faces for the r.h.s.
 * \param[in,out] rhs           right hand side \f$ \vect{Rhs} \f$
 */
/*----------------------------------------------------------------------------*/

void
cs_convection_diffusion_scalar(int                       idtvar,
                               int                       f_id,
                               const cs_var_cal_opt_t    var_cal_opt,
                               int                       icvflb,
                               int                       inc,
                               int                       iccocg,
                               int                       imasac,
                               cs_real_t       *restrict pvar,
                               const cs_real_t *restrict pvara,
                               const cs_int_t            icvfli[],
                               const cs_real_t           coefap[],
                               const cs_real_t           coefbp[],
                               const cs_real_t           cofafp[],
                               const cs_real_t           cofbfp[],
                               const cs_real_t           i_massflux[],
                               const cs_real_t           b_massflux[],
                               const cs_real_t           i_visc[],
                               const cs_real_t           b_visc[],
                               cs_real_t       *restrict rhs)
{
  const int iconvp = var_cal_opt.iconv;
  const int idiffp = var_cal_opt.idiff;
  const int nswrgp = var_cal_opt.nswrgr;
  const int imrgra = var_cal_opt.imrgra;
  const int imligp = var_cal_opt.imligr;
  const int ircflp = var_cal_opt.ircflu;
  const int ischcp = var_cal_opt.ischcv;
  const int isstpp = var_cal_opt.isstpc;
  const int iwarnp = var_cal_opt.iwarni;
  int limiter_choice = -1;
  const double blencp = var_cal_opt.blencv;
  const double epsrgp = var_cal_opt.epsrgr;
  const double climgp = var_cal_opt.climgr;
  const double extrap = var_cal_opt.extrag;
  const double relaxp = var_cal_opt.relaxv;
  const double thetap = var_cal_opt.thetav;

  const cs_mesh_t  *m = cs_glob_mesh;
  cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  const cs_lnum_t n_cells = m->n_cells;
  const cs_lnum_t n_cells_ext = m->n_cells_with_ghosts;
  const int n_i_groups = m->i_face_numbering->n_groups;
  const int n_i_threads = m->i_face_numbering->n_threads;
  const int n_b_groups = m->b_face_numbering->n_groups;
  const int n_b_threads = m->b_face_numbering->n_threads;
  const cs_lnum_t *restrict i_group_index = m->i_face_numbering->group_index;
  const cs_lnum_t *restrict b_group_index = m->b_face_numbering->group_index;

  const cs_lnum_2_t *restrict i_face_cells
    = (const cs_lnum_2_t *restrict)m->i_face_cells;
  const cs_lnum_t *restrict b_face_cells
    = (const cs_lnum_t *restrict)m->b_face_cells;
  const cs_real_t *restrict weight = fvq->weight;
  const cs_real_t *restrict i_dist = fvq->i_dist;
  const cs_real_t *restrict i_face_surf = fvq->i_face_surf;
  const cs_real_t *restrict cell_vol = fvq->cell_vol;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)fvq->cell_cen;
  const cs_real_3_t *restrict i_face_normal
    = (const cs_real_3_t *restrict)fvq->i_face_normal;
  const cs_real_3_t *restrict i_face_cog
    = (const cs_real_3_t *restrict)fvq->i_face_cog;
  const cs_real_3_t *restrict dijpf
    = (const cs_real_3_t *restrict)fvq->dijpf;
  const cs_real_3_t *restrict diipb
    = (const cs_real_3_t *restrict)fvq->diipb;

  const int *bc_type = cs_glob_bc_type;

  /* Local variables */

  char var_name[32];

  int iupwin = 0;
  int tr_dim = 0;
  int w_stride = 1;

  bool recompute_cocg = (iccocg) ? true : false;

  cs_real_t *coface, *cofbce;

  cs_real_3_t *grad;
  cs_real_3_t *gradup = NULL;
  cs_real_3_t *gradst = NULL;
  cs_field_t *f;

  cs_field_t *f_limiter = NULL;
  cs_real_t *limiter = NULL;

  cs_real_t *gweight = NULL;

  cs_real_t  *v_slope_test = _get_v_slope_test(f_id,  var_cal_opt);

  /* 1. Initialization */

  /* Allocate work arrays */

  BFT_MALLOC(grad, n_cells_ext, cs_real_3_t);

  /* Choose gradient type */

  cs_halo_type_t halo_type = CS_HALO_STANDARD;
  cs_gradient_type_t gradient_type = CS_GRADIENT_ITER;

  cs_gradient_type_by_imrgra(imrgra,
                             &gradient_type,
                             &halo_type);

  if (f_id != -1) {
    f = cs_field_by_id(f_id);
    cs_gradient_perio_init_rij(f, &tr_dim, grad);
    /* Get option from the field */
    if (isstpp >= 3) {
      const int key_limiter = cs_field_key_id("limiter_choice");
      limiter_choice = cs_field_get_key_int(f, key_limiter);
    }

    int f_limiter_id = cs_field_get_key_int(f, cs_field_key_id("convection_limiter_id"));
    if (f_limiter_id > -1) {
      f_limiter = cs_field_by_id(f_limiter_id);
      limiter = f_limiter->val;
    }

    snprintf(var_name, 31, "%s", f->name); var_name[31] = '\0';
  }
  else
    strcpy(var_name, "Work array"); var_name[31] = '\0';

  if (iwarnp >= 2) {
    if (ischcp == 1) {
      bft_printf(
        _(" %s: Convection in centered blending with %f percent of upwind\n"),
        var_name, (1.-blencp)*100.);
    } else {
      bft_printf(
        _(" %s: Convection in 2nd order blending with %f percent of upwind\n"),
        var_name, (1.-blencp)*100.);
    }
  }

  iupwin = (blencp > 0.) ? 0 : 1;

  /* 2. Compute the balance with reconstruction */

  /* Compute the gradient of the variable */

  /* The gradient (grad) is used in the flux reconstruction and the slope test.
     Thus we must compute it:
         - when we have diffusion and we reconstruct the fluxes,
         - when the convection scheme is SOLU,
         - when we have convection, we are not in pure upwind
           and we reconstruct the fluxes,
         - when we have convection, we are not in pure upwind
           and we have not shunted the slope test. */

  if (  (idiffp != 0 && ircflp == 1)
     || (  iconvp != 0 && iupwin == 0
        && (ischcp == 0 || ircflp == 1 || isstpp == 0))) {

    if (f_id != -1) {
      /* Get the calculation option from the field */
      if (f->type & CS_FIELD_VARIABLE && var_cal_opt.iwgrec == 1) {
        if (var_cal_opt.idiff > 0) {
          int key_id = cs_field_key_id("gradient_weighting_id");
          int diff_id = cs_field_get_key_int(f, key_id);
          if (diff_id > -1) {
            cs_field_t *weight_f = cs_field_by_id(diff_id);
            gweight = weight_f->val;
            w_stride = weight_f->dim;
          }
        }
      }
    }

    cs_gradient_scalar(var_name,
                       gradient_type,
                       halo_type,
                       inc,
                       recompute_cocg,
                       nswrgp,
                       tr_dim,
                       0, /* hyd_p_flag */
                       w_stride,
                       iwarnp,
                       imligp,
                       epsrgp,
                       extrap,
                       climgp,
                       NULL, /* f_ext exterior force */
                       coefap,
                       coefbp,
                       pvar,
                       gweight, /* Weighted gradient */
                       grad);

  } else {
#   pragma omp parallel for
    for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++) {
      grad[cell_id][0] = 0.;
      grad[cell_id][1] = 0.;
      grad[cell_id][2] = 0.;
    }
  }

  /* 2.1 Compute the gradient for convective scheme (the slope test, limiter, SOLU, etc) */

  /* Slope test gradient */
  if (iconvp > 0 && iupwin == 0 && isstpp == 0) {

    BFT_MALLOC(gradst, n_cells_ext, cs_real_3_t);

#   pragma omp parallel for
    for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++) {
      gradst[cell_id][0] = 0.;
      gradst[cell_id][1] = 0.;
      gradst[cell_id][2] = 0.;
    }

    cs_slope_test_gradient(f_id,
                           inc,
                           halo_type,
                           grad,
                           gradst,
                           pvar,
                           coefap,
                           coefbp,
                           i_massflux);

  }

  /* Pure SOLU scheme without using gradient_slope_test function
     or Roe and Sweby limiters */
  if (iconvp > 0 && iupwin == 0 && (ischcp == 2 || isstpp == 3)) {

    BFT_MALLOC(gradup, n_cells_ext, cs_real_3_t);

#   pragma omp parallel for
    for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++) {
      gradup[cell_id][0] = 0.;
      gradup[cell_id][1] = 0.;
      gradup[cell_id][2] = 0.;
    }

    cs_upwind_gradient(f_id,
                       inc,
                       halo_type,
                       coefap,
                       coefbp,
                       i_massflux,
                       b_massflux,
                       pvar,
                       gradup);

  }

  /* ======================================================================
    ---> Contribution from interior faces
    ======================================================================*/

  cs_gnum_t n_upwind = 0;

  if (n_cells_ext>n_cells) {
#   pragma omp parallel for if(n_cells_ext - n_cells > CS_THR_MIN)
    for (cs_lnum_t cell_id = n_cells; cell_id < n_cells_ext; cell_id++) {
      rhs[cell_id] = 0.;
    }
  }

  /* --> Pure upwind flux
    =====================*/

  if (iupwin == 1) {

    /* Steady */
    if (idtvar < 0) {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for reduction(+:n_upwind)
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            /* in parallel, face will be counted by one and only one rank */
            if (ii < n_cells) {
              n_upwind++;
            }

            cs_real_2_t fluxij = {0.,0.};

            cs_real_t pifri, pjfri, pifrj, pjfrj;
            cs_real_t pip, pjp, pipr, pjpr;

            cs_i_cd_steady_upwind(ircflp,
                                  relaxp,
                                  weight[face_id],
                                  cell_cen[ii],
                                  cell_cen[jj],
                                  i_face_cog[face_id],
                                  dijpf[face_id],
                                  grad[ii],
                                  grad[jj],
                                  pvar[ii],
                                  pvar[jj],
                                  pvara[ii],
                                  pvara[jj],
                                  &pifri,
                                  &pifrj,
                                  &pjfri,
                                  &pjfrj,
                                  &pip,
                                  &pjp,
                                  &pipr,
                                  &pjpr);

            cs_i_conv_flux(iconvp,
                           1.,
                           1,
                           pvar[ii],
                           pvar[jj],
                           pifri,
                           pifrj,
                           pjfri,
                           pjfrj,
                           i_massflux[face_id],
                           1., /* xcpp */
                           1., /* xcpp */
                           fluxij);

            cs_i_diff_flux(idiffp,
                           1.,
                           pip,
                           pjp,
                           pipr,
                           pjpr,
                           i_visc[face_id],
                           fluxij);

            rhs[ii] -= fluxij[0];
            rhs[jj] += fluxij[1];

          }
        }
      }

    /* Unsteady */
    } else {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for reduction(+:n_upwind)
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            /* in parallel, face will be counted by one and only one rank */
            if (ii < n_cells) {
              n_upwind++;
            }

            cs_real_2_t fluxij = {0.,0.};

            cs_real_t pif, pjf;
            cs_real_t pip, pjp;

            cs_i_cd_unsteady_upwind(ircflp,
                                    weight[face_id],
                                    cell_cen[ii],
                                    cell_cen[jj],
                                    i_face_cog[face_id],
                                    dijpf[face_id],
                                    grad[ii],
                                    grad[jj],
                                    pvar[ii],
                                    pvar[jj],
                                    &pif,
                                    &pjf,
                                    &pip,
                                    &pjp);

            cs_i_conv_flux(iconvp,
                           thetap,
                           imasac,
                           pvar[ii],
                           pvar[jj],
                           pif,
                           pif, /* no relaxation */
                           pjf,
                           pjf, /* no relaxation */
                           i_massflux[face_id],
                           1., /* xcpp */
                           1., /* xcpp */
                           fluxij);

            cs_i_diff_flux(idiffp,
                           thetap,
                           pip,
                           pjp,
                           pip,/* no relaxation */
                           pjp,/* no relaxation */
                           i_visc[face_id],
                           fluxij);

            rhs[ii] -= fluxij[0];
            rhs[jj] += fluxij[1];

          }
        }
      }

    }

  /* --> Flux with no slope test or Min/Max Beta limiter
    ====================================================*/

  } else if (isstpp == 1 || isstpp == 2) {

    if (ischcp < 0 || ischcp > 2) {
      bft_error(__FILE__, __LINE__, 0,
                _("invalid value of ischcv"));
    }

    /* Steady */
    if (idtvar < 0) {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            cs_real_2_t fluxij = {0.,0.};

            cs_real_t pifri, pjfri, pifrj, pjfrj;
            cs_real_t pip, pjp, pipr, pjpr;

            cs_i_cd_steady(ircflp,
                           ischcp,
                           relaxp,
                           blencp,
                           weight[face_id],
                           cell_cen[ii],
                           cell_cen[jj],
                           i_face_cog[face_id],
                           dijpf[face_id],
                           grad[ii],
                           grad[jj],
                           gradup[ii],
                           gradup[jj],
                           pvar[ii],
                           pvar[jj],
                           pvara[ii],
                           pvara[jj],
                           &pifri,
                           &pifrj,
                           &pjfri,
                           &pjfrj,
                           &pip,
                           &pjp,
                           &pipr,
                           &pjpr);

            cs_i_conv_flux(iconvp,
                           1.,
                           1,
                           pvar[ii],
                           pvar[jj],
                           pifri,
                           pifrj,
                           pjfri,
                           pjfrj,
                           i_massflux[face_id],
                           1., /* xcpp */
                           1., /* xcpp */
                           fluxij);

            cs_i_diff_flux(idiffp,
                           1.,
                           pip,
                           pjp,
                           pipr,
                           pjpr,
                           i_visc[face_id],
                           fluxij);

            rhs[ii] -= fluxij[0];
            rhs[jj] += fluxij[1];

          }
        }
      }

    /* Unsteady */
    } else {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            cs_real_t beta = blencp;

            cs_real_t pif, pjf;
            cs_real_t pip, pjp;

            /*Beta blending coefficient assuring the positivity of the scalar*/
            if (isstpp == 2) {
              beta = CS_MAX(CS_MIN(limiter[ii], limiter[jj]), 0.);
            }

            cs_real_2_t fluxij = {0.,0.};

            cs_i_cd_unsteady(ircflp,
                             ischcp,
                             beta,
                             weight[face_id],
                             cell_cen[ii],
                             cell_cen[jj],
                             i_face_cog[face_id],
                             dijpf[face_id],
                             grad[ii],
                             grad[jj],
                             gradup[ii],
                             gradup[jj],
                             pvar[ii],
                             pvar[jj],
                             &pif,
                             &pjf,
                             &pip,
                             &pjp);

            cs_i_conv_flux(iconvp,
                           thetap,
                           imasac,
                           pvar[ii],
                           pvar[jj],
                           pif,
                           pif, /* no relaxation */
                           pjf,
                           pjf, /* no relaxation */
                           i_massflux[face_id],
                           1., /* xcpp */
                           1., /* xcpp */
                           fluxij);

            cs_i_diff_flux(idiffp,
                           thetap,
                           pip,
                           pjp,
                           pip, /* no relaxation */
                           pjp, /* no relaxation */
                           i_visc[face_id],
                           fluxij);

            rhs[ii] -= fluxij[0];
            rhs[jj] += fluxij[1];

          }
        }
      }

    }

  /* --> Flux with slope test or Roe and Sweby limiter
    ==================================================*/

  } else {

    if (ischcp < 0 || ischcp > 2) {
      bft_error(__FILE__, __LINE__, 0,
                _("invalid value of ischcv"));
    }
    if (isstpp != 0 && isstpp != 3) {
      bft_error(__FILE__, __LINE__, 0,
                _("invalid value of isstpc"));
    }

    /* Steady */
    if (idtvar < 0) {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for reduction(+:n_upwind)
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            cs_real_2_t fluxij = {0., 0.};

            bool upwind_switch;
            cs_real_t pifri, pjfri, pifrj, pjfrj;
            cs_real_t pip, pjp, pipr, pjpr;

            cs_i_cd_steady_slope_test(&upwind_switch,
                                      iconvp,
                                      ircflp,
                                      ischcp,
                                      relaxp,
                                      blencp,
                                      weight[face_id],
                                      i_dist[face_id],
                                      i_face_surf[face_id],
                                      cell_cen[ii],
                                      cell_cen[jj],
                                      i_face_normal[face_id],
                                      i_face_cog[face_id],
                                      dijpf[face_id],
                                      i_massflux[face_id],
                                      grad[ii],
                                      grad[jj],
                                      gradup[ii],
                                      gradup[jj],
                                      gradst[ii],
                                      gradst[jj],
                                      pvar[ii],
                                      pvar[jj],
                                      pvara[ii],
                                      pvara[jj],
                                      &pifri,
                                      &pifrj,
                                      &pjfri,
                                      &pjfrj,
                                      &pip,
                                      &pjp,
                                      &pipr,
                                      &pjpr);

            cs_i_conv_flux(iconvp,
                           1.,
                           1,
                           pvar[ii],
                           pvar[jj],
                           pifri,
                           pifrj,
                           pjfri,
                           pjfrj,
                           i_massflux[face_id],
                           1., /* xcpp */
                           1., /* xcpp */
                           fluxij);

            cs_i_diff_flux(idiffp,
                           1.,
                           pip,
                           pjp,
                           pipr,
                           pjpr,
                           i_visc[face_id],
                           fluxij);

            if (upwind_switch) {

              /* in parallel, face will be counted by one and only one rank */
              if (ii < n_cells)
                n_upwind++;
              if (v_slope_test != NULL) {
                v_slope_test[ii] += fabs(i_massflux[face_id]) / cell_vol[ii];
                v_slope_test[jj] += fabs(i_massflux[face_id]) / cell_vol[jj];
              }

            }

            rhs[ii] -= fluxij[0];
            rhs[jj] += fluxij[1];

          }
        }
      }

    /* Unsteady */
    } else {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for reduction(+:n_upwind)
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            bool upwind_switch = false;

            cs_real_2_t fluxij = {0.,0.};

            cs_real_t pif, pjf;
            cs_real_t pip, pjp;

            /* Original slope test */
            if (isstpp == 0) {

              cs_i_cd_unsteady_slope_test(&upwind_switch,
                                          iconvp,
                                          ircflp,
                                          ischcp,
                                          blencp,
                                          weight[face_id],
                                          i_dist[face_id],
                                          i_face_surf[face_id],
                                          cell_cen[ii],
                                          cell_cen[jj],
                                          i_face_normal[face_id],
                                          i_face_cog[face_id],
                                          dijpf[face_id],
                                          i_massflux[face_id],
                                          grad[ii],
                                          grad[jj],
                                          gradup[ii],
                                          gradup[jj],
                                          gradst[ii],
                                          gradst[jj],
                                          pvar[ii],
                                          pvar[jj],
                                          &pif,
                                          &pjf,
                                          &pip,
                                          &pjp);

              cs_i_conv_flux(iconvp,
                             thetap,
                             imasac,
                             pvar[ii],
                             pvar[jj],
                             pif,
                             pif, /* no relaxation */
                             pjf,
                             pjf, /* no relaxation */
                             i_massflux[face_id],
                             1., /* xcpp */
                             1., /* xcpp */
                             fluxij);

            }
            /* Roe- Sweby limiter */
            else { /* if (isstpp == 3) */

              cs_real_t rij;

              int cur;
              cs_real_t p_u, p_c, p_d;

              cur = ii;
              // Current    point value
              p_c = pvar[ii];
              // Downstream point value
              p_d = pvar[jj];

              if (i_massflux[face_id] < 0.) {
                cur = jj;
                p_c = pvar[jj];
                p_d = pvar[ii];
              }

              /* Compute the upstream point value */
              p_u = cs_upstream_val(p_c,
                                    cell_vol[cur],
                                    i_face_surf[face_id],
                                    i_face_normal[face_id],
                                    gradup[cur]);

              /* If non monotonicity is detected at the downstream side
                 the scheme switches to a first order upwind scheme */
              if ((p_c-p_u)*(p_d-p_c) <= 0.) {
                 rij = 0.;
              }
              /* There is monotonicity at the downstream side */
              else {
                if (CS_ABS(p_d-p_c) < cs_math_epzero*(CS_ABS(p_u)+CS_ABS(p_c)+CS_ABS(p_d))) {
                  rij = cs_math_big_r;
                }
                /* consecutive downstream slopes rate */
                else {
                  rij = CS_MIN(CS_ABS((p_c-p_u)/(p_d-p_c)), cs_math_big_r);
                }
              }

              cs_real_t phi = cs_limiter_function(limiter_choice, rij);
              /* If stored for post-processing */
              if (limiter != NULL)
                limiter[face_id] = phi;

              /* Compute the limited convective flux based on
                 SOLU or centered scheme */

              cs_i_cd_unsteady_limiter(ircflp,
                                       ischcp,
                                       weight[face_id],
                                       cell_cen[ii],
                                       cell_cen[jj],
                                       i_face_cog[face_id],
                                       phi,
                                       dijpf[face_id],
                                       grad[ii],
                                       grad[jj],
                                       gradup[ii],
                                       gradup[jj],
                                       pvar[ii],
                                       pvar[jj],
                                       &pif,
                                       &pjf,
                                       &pip,
                                       &pjp);

              cs_i_conv_flux(iconvp,
                             thetap,
                             imasac,
                             pvar[ii],
                             pvar[jj],
                             pif,
                             pif, /* no relaxation */
                             pjf,
                             pjf, /* no relaxation */
                             i_massflux[face_id],
                             1., /* xcpp */
                             1., /* xcpp */
                             fluxij);

            }
            cs_i_diff_flux(idiffp,
                           thetap,
                           pip,
                           pjp,
                           pip, /* no relaxation */
                           pjp, /* no relaxation */
                           i_visc[face_id],
                           fluxij);

            if (upwind_switch) {

              /* in parallel, face will be counted by one and only one rank */
              if (ii < n_cells)
                n_upwind++;
              if (v_slope_test != NULL) {
                v_slope_test[ii] += fabs(i_massflux[face_id]) / cell_vol[ii];
                v_slope_test[jj] += fabs(i_massflux[face_id]) / cell_vol[jj];
              }

            }

            rhs[ii] -= fluxij[0];
            rhs[jj] += fluxij[1];

          }
        }
      }

    } /* idtvar */

  } /* iupwin */


  if (iwarnp >= 2) {

    /* Sum number of clippings */
    cs_parall_counter(&n_upwind, 1);

  bft_printf(_(" %s: %llu Faces with upwind on %llu interior faces \n"),
             var_name, (unsigned long long)n_upwind,
             (unsigned long long)m->n_g_i_faces);
  }

  /* ======================================================================
    ---> Contribution from boundary faces
    ======================================================================*/

  /* Boundary convective flux are all computed with an upwind scheme */
  if (icvflb == 0) {

    /* Steady */
    if (idtvar < 0) {

      for (int g_id = 0; g_id < n_b_groups; g_id++) {
#       pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
        for (int t_id = 0; t_id < n_b_threads; t_id++) {
          for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
               face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = b_face_cells[face_id];

            cs_real_t fluxi = 0.;
            cs_real_t pir, pipr;

            cs_b_cd_steady(ircflp,
                           relaxp,
                           diipb[face_id],
                           grad[ii],
                           pvar[ii],
                           pvara[ii],
                           &pir,
                           &pipr);

            cs_b_upwind_flux(iconvp,
                             1.,
                             1,
                             inc,
                             bc_type[face_id],
                             pvar[ii],
                             pir,
                             pipr,
                             coefap[face_id],
                             coefbp[face_id],
                             b_massflux[face_id],
                             1., /* xcpp */
                             &fluxi);

            cs_b_diff_flux(idiffp,
                           1., /* thetap */
                           inc,
                           pipr,
                           cofafp[face_id],
                           cofbfp[face_id],
                           b_visc[face_id],
                           &fluxi);

            rhs[ii] -= fluxi;

          }
        }
      }

    /* Unsteady */
    } else {

      for (int g_id = 0; g_id < n_b_groups; g_id++) {
#       pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
        for (int t_id = 0; t_id < n_b_threads; t_id++) {
          for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
               face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = b_face_cells[face_id];

            cs_real_t fluxi = 0.;
            cs_real_t pip;

            cs_b_cd_unsteady(ircflp,
                             diipb[face_id],
                             grad[ii],
                             pvar[ii],
                             &pip);

            cs_b_upwind_flux(iconvp,
                             thetap,
                             imasac,
                             inc,
                             bc_type[face_id],
                             pvar[ii],
                             pvar[ii], /* no relaxation */
                             pip,
                             coefap[face_id],
                             coefbp[face_id],
                             b_massflux[face_id],
                             1., /* xcpp */
                             &fluxi);

            cs_b_diff_flux(idiffp,
                           thetap,
                           inc,
                           pip,
                           cofafp[face_id],
                           cofbfp[face_id],
                           b_visc[face_id],
                           &fluxi);

            rhs[ii] -= fluxi;

          }
        }
      }

    }

  /* Boundary convective flux is imposed at some faces
     (tagged in icvfli array) */
  } else if (icvflb == 1) {

    /* Retrieve the value of the convective flux to be imposed */
    if (f_id != -1) {
      coface = f->bc_coeffs->ac;
      cofbce = f->bc_coeffs->bc;
    } else {
      bft_error(__FILE__, __LINE__, 0,
                _("invalid value of icvflb and f_id"));
    }

    /* Steady */
    if (idtvar < 0) {

      for (int g_id = 0; g_id < n_b_groups; g_id++) {
#       pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
        for (int t_id = 0; t_id < n_b_threads; t_id++) {
          for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
               face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = b_face_cells[face_id];

            cs_real_t fluxi = 0.;
            cs_real_t pir, pipr;

            cs_b_cd_steady(ircflp,
                           relaxp,
                           diipb[face_id],
                           grad[ii],
                           pvar[ii],
                           pvara[ii],
                           &pir,
                           &pipr);

            cs_b_imposed_conv_flux(iconvp,
                                   1.,
                                   1,
                                   inc,
                                   bc_type[face_id],
                                   icvfli[face_id],
                                   pvar[ii],
                                   pir,
                                   pipr,
                                   coefap[face_id],
                                   coefbp[face_id],
                                   coface[face_id],
                                   cofbce[face_id],
                                   b_massflux[face_id],
                                   1., /* xcpp */
                                   &fluxi);

            cs_b_diff_flux(idiffp,
                           1., /* thetap */
                           inc,
                           pipr,
                           cofafp[face_id],
                           cofbfp[face_id],
                           b_visc[face_id],
                           &fluxi);

            rhs[ii] -= fluxi;

          }
        }
      }

    /* Unsteady */
    } else {

      for (int g_id = 0; g_id < n_b_groups; g_id++) {
#       pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
        for (int t_id = 0; t_id < n_b_threads; t_id++) {
          for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
               face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = b_face_cells[face_id];

            cs_real_t fluxi = 0.;
            cs_real_t pip;

            cs_b_cd_unsteady(ircflp,
                             diipb[face_id],
                             grad[ii],
                             pvar[ii],
                             &pip);

            cs_b_imposed_conv_flux(iconvp,
                                   thetap,
                                   imasac,
                                   inc,
                                   bc_type[face_id],
                                   icvfli[face_id],
                                   pvar[ii],
                                   pvar[ii], /* no relaxation */
                                   pip,
                                   coefap[face_id],
                                   coefbp[face_id],
                                   coface[face_id],
                                   cofbce[face_id],
                                   b_massflux[face_id],
                                   1., /* xcpp */
                                   &fluxi);

            cs_b_diff_flux(idiffp,
                           thetap,
                           inc,
                           pip,
                           cofafp[face_id],
                           cofbfp[face_id],
                           b_visc[face_id],
                           &fluxi);

            rhs[ii] -= fluxi;

          }
        }
      }

    }
  }

  /* Free memory */
  BFT_FREE(grad);
  grad = NULL;
  if (gradup!=NULL) {
    BFT_FREE(gradup);
    gradup = NULL;
  }
  if (gradst!=NULL) {
    BFT_FREE(gradst);
    gradst = NULL;
  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Add the explicit part of the convection/diffusion terms of a transport
 *  equation of a vector field \f$ \vect{\varia} \f$.
 *
 * More precisely, the right hand side \f$ \vect{Rhs} \f$ is updated as
 * follows:
 * \f[
 *  \vect{Rhs} = \vect{Rhs} - \sum_{\fij \in \Facei{\celli}}      \left(
 *         \dot{m}_\ij \left( \vect{\varia}_\fij - \vect{\varia}_\celli \right)
 *       - \mu_\fij \gradt_\fij \vect{\varia} \cdot \vect{S}_\ij  \right)
 * \f]
 *
 * Remark:
 * if ivisep = 1, then we also take \f$ \mu \transpose{\gradt\vect{\varia}}
 * + \lambda \trace{\gradt\vect{\varia}} \f$, where \f$ \lambda \f$ is
 * the secondary viscosity, i.e. usually \f$ -\frac{2}{3} \mu \f$.
 *
 * Warning:
 * - \f$ \vect{Rhs} \f$ has already been initialized before calling bilsc!
 * - mind the sign minus
 *
 * \param[in]     idtvar        indicator of the temporal scheme
 * \param[in]     f_id          index of the current variable
 * \param[in]     var_cal_opt   variable calculation options
 * \param[in]     icvflb        global indicator of boundary convection flux
 *                               - 0 upwind scheme at all boundary faces
 *                               - 1 imposed flux at some boundary faces
 * \param[in]     inc           indicator
 *                               - 0 when solving an increment
 *                               - 1 otherwise
 * \param[in]     ivisep        indicator to take \f$ \divv
 *                               \left(\mu \gradt \transpose{\vect{a}} \right)
 *                               -2/3 \grad\left( \mu \dive \vect{a} \right)\f$
 *                               - 1 take into account,
 *                               - 0 otherwise
 * \param[in]     imasac        take mass accumulation into account?
 * \param[in]     pvar          solved velocity (current time step)
 * \param[in]     pvara         solved velocity (previous time step)
 * \param[in]     icvfli        boundary face indicator array of convection flux
 *                               - 0 upwind scheme
 *                               - 1 imposed flux
 * \param[in]     coefav        boundary condition array for the variable
 *                               (explicit part)
 * \param[in]     coefbv        boundary condition array for the variable
 *                               (implicit part)
 * \param[in]     cofafv        boundary condition array for the diffusion
 *                               of the variable (explicit part)
 * \param[in]     cofbfv        boundary condition array for the diffusion
 *                               of the variable (implicit part)
 * \param[in]     i_massflux    mass flux at interior faces
 * \param[in]     b_massflux    mass flux at boundary faces
 * \param[in]     i_visc        \f$ \mu_\fij \dfrac{S_\fij}{\ipf \jpf} \f$
 *                               at interior faces for the r.h.s.
 * \param[in]     b_visc        \f$ \mu_\fib \dfrac{S_\fib}{\ipf \centf} \f$
 *                               at border faces for the r.h.s.
 * \param[in]     secvif        secondary viscosity at interior faces
 * \param[in,out] rhs           right hand side \f$ \vect{Rhs} \f$
 */
/*----------------------------------------------------------------------------*/

void
cs_convection_diffusion_vector(int                         idtvar,
                               int                         f_id,
                               const cs_var_cal_opt_t      var_cal_opt,
                               int                         icvflb,
                               int                         inc,
                               int                         ivisep,
                               int                         imasac,
                               cs_real_3_t       *restrict pvar,
                               const cs_real_3_t *restrict pvara,
                               const cs_int_t              icvfli[],
                               const cs_real_3_t           coefav[],
                               const cs_real_33_t          coefbv[],
                               const cs_real_3_t           cofafv[],
                               const cs_real_33_t          cofbfv[],
                               const cs_real_t             i_massflux[],
                               const cs_real_t             b_massflux[],
                               const cs_real_t             i_visc[],
                               const cs_real_t             b_visc[],
                               const cs_real_t             secvif[],
                               cs_real_3_t       *restrict rhs)
{
  const int iconvp = var_cal_opt.iconv;
  const int idiffp = var_cal_opt.idiff;
  const int nswrgp = var_cal_opt.nswrgr;
  const int imrgra = var_cal_opt.imrgra;
  const int imligp = var_cal_opt.imligr;
  const int ircflp = var_cal_opt.ircflu;
  const int ischcp = var_cal_opt.ischcv;
  const int isstpp = var_cal_opt.isstpc;
  const int iwarnp = var_cal_opt.iwarni;
  const double blencp = var_cal_opt.blencv;
  const double epsrgp = var_cal_opt.epsrgr;
  const double climgp = var_cal_opt.climgr;
  const double relaxp = var_cal_opt.relaxv;
  const double thetap = var_cal_opt.thetav;

  const cs_mesh_t  *m = cs_glob_mesh;
  const cs_halo_t  *halo = m->halo;
  cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  const cs_lnum_t n_cells = m->n_cells;
  const cs_lnum_t n_cells_ext = m->n_cells_with_ghosts;
  const int n_i_groups = m->i_face_numbering->n_groups;
  const int n_i_threads = m->i_face_numbering->n_threads;
  const int n_b_groups = m->b_face_numbering->n_groups;
  const int n_b_threads = m->b_face_numbering->n_threads;
  const cs_lnum_t *restrict i_group_index = m->i_face_numbering->group_index;
  const cs_lnum_t *restrict b_group_index = m->b_face_numbering->group_index;

  const cs_lnum_2_t *restrict i_face_cells
    = (const cs_lnum_2_t *restrict)m->i_face_cells;
  const cs_lnum_t *restrict b_face_cells
    = (const cs_lnum_t *restrict)m->b_face_cells;
  const cs_real_t *restrict weight = fvq->weight;
  const cs_real_t *restrict i_dist = fvq->i_dist;
  const cs_real_t *restrict i_face_surf = fvq->i_face_surf;
  const cs_real_t *restrict cell_vol = fvq->cell_vol;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)fvq->cell_cen;
  const cs_real_3_t *restrict i_face_normal
    = (const cs_real_3_t *restrict)fvq->i_face_normal;
  const cs_real_3_t *restrict i_f_face_normal
    = (const cs_real_3_t *restrict)fvq->i_f_face_normal;
  const cs_real_3_t *restrict i_face_cog
    = (const cs_real_3_t *restrict)fvq->i_face_cog;
  const cs_real_3_t *restrict dijpf
    = (const cs_real_3_t *restrict)fvq->dijpf;
  const cs_real_3_t *restrict diipb
    = (const cs_real_3_t *restrict)fvq->diipb;

  const int *bc_type = cs_glob_bc_type;

  /* Local variables */

  char var_name[32];

  cs_gnum_t n_upwind;
  int iupwin = 0;

  cs_real_33_t *grad, *grdpa;
  cs_real_t *bndcel;

  cs_real_3_t *coface;
  const cs_real_33_t *cofbce;

  cs_field_t *f;

  cs_real_t  *v_slope_test = _get_v_slope_test(f_id,  var_cal_opt);

  /*==========================================================================*/

  /* 1. Initialization */

  /* Allocate work arrays */

  BFT_MALLOC(grad, n_cells_ext, cs_real_33_t);
  BFT_MALLOC(grdpa, n_cells_ext, cs_real_33_t);

  /* Choose gradient type */

  cs_halo_type_t halo_type = CS_HALO_STANDARD;
  cs_gradient_type_t gradient_type = CS_GRADIENT_ITER;

  cs_gradient_type_by_imrgra(imrgra,
                             &gradient_type,
                             &halo_type);

  if (f_id != -1) {
    f = cs_field_by_id(f_id);
    snprintf(var_name, 31, "%s", f->name); var_name[31] = '\0';
  }
  else
    strcpy(var_name, "Work array"); var_name[31] = '\0';

  if (iwarnp >= 2 && iconvp == 1) {
    if (ischcp == 1) {
      bft_printf
        (
         _(" %s: Convection in centered blending with %f percent of upwind\n"),
         var_name, (1.-blencp)*100.);
    } else {
      bft_printf
        (
         _(" %s: Convection in 2nd order blending with %f percent of upwind\n"),
         var_name, (1.-blencp)*100.);
    }
  }

  iupwin = (blencp > 0.) ? 0 : 1;

  /* 2. Compute the balance with reconstruction */

  /* Compute the gradient of the velocity

     The gradient (grad) is used in the flux reconstruction and the slope test.
     Thus we must compute it:
         - when we have diffusion and we reconstruct the fluxes,
         - when the convection scheme is SOLU,
         - when we have convection, we are not in pure upwind
           and we reconstruct the fluxes,
         - when we have convection, we are not in pure upwind
           and we have not shunted the slope test. */

  if (  (idiffp != 0 && ircflp == 1) || ivisep == 1
     || (   iconvp != 0 && iupwin == 0
         && (ischcp == 0 || ircflp == 1 || isstpp == 0))) {

    cs_gradient_vector(var_name,
                       gradient_type,
                       halo_type,
                       inc,
                       nswrgp,
                       iwarnp,
                       imligp,
                       epsrgp,
                       climgp,
                       coefav,
                       coefbv,
                       pvar,
                       grad);

  } else {
#   pragma omp parallel for
    for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++) {
      for (int isou = 0; isou < 3; isou++) {
        for (int jsou = 0; jsou < 3; jsou++)
          grad[cell_id][isou][jsou] = 0.;
      }
    }
  }

  /* ======================================================================
     ---> Compute uncentered gradient grdpa for the slope test
     ======================================================================*/

# pragma omp parallel for
  for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++) {
    for (int jsou = 0; jsou < 3; jsou++) {
      for (int isou = 0; isou < 3; isou++)
        grdpa[cell_id][isou][jsou] = 0.;
    }
  }

  if (iconvp > 0 && iupwin == 0 && isstpp == 0) {

    cs_slope_test_gradient_vector(inc,
                                  halo_type,
                                  grad,
                                  grdpa,
                                  pvar,
                                  coefav,
                                  coefbv,
                                  i_massflux);

  }

  /* ======================================================================
     ---> Contribution from interior faces
     ======================================================================*/

  n_upwind = 0;

  if (n_cells_ext > n_cells) {
#   pragma omp parallel for if(n_cells_ext -n_cells > CS_THR_MIN)
    for (cs_lnum_t cell_id = n_cells; cell_id < n_cells_ext; cell_id++) {
      for (int isou = 0; isou < 3; isou++)
        rhs[cell_id][isou] = 0.;
    }
  }

  /* --> Pure upwind flux
     =====================*/

  if (iupwin == 1) {

    /* Steady */
    if (idtvar < 0) {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for reduction(+:n_upwind)
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            /* in parallel, face will be counted by one and only one rank */
            if (ii < n_cells) {
              n_upwind++;
            }

            cs_real_t fluxi[3], fluxj[3] ;
            for (int isou =  0; isou < 3; isou++) {
              fluxi[isou] = 0;
              fluxj[isou] = 0;
            }

            cs_real_3_t pip, pjp, pipr, pjpr;
            cs_real_3_t pifri, pifrj, pjfri, pjfrj;

            cs_i_cd_steady_upwind_vector(ircflp,
                                         relaxp,
                                         weight[face_id],
                                         cell_cen[ii],
                                         cell_cen[jj],
                                         i_face_cog[face_id],
                                         dijpf[face_id],
                                         (const cs_real_3_t *)grad[ii],
                                         (const cs_real_3_t *)grad[jj],
                                         pvar[ii],
                                         pvar[jj],
                                         pvara[ii],
                                         pvara[jj],
                                         pifri,
                                         pifrj,
                                         pjfri,
                                         pjfrj,
                                         pip,
                                         pjp,
                                         pipr,
                                         pjpr);


            cs_i_conv_flux_vector(iconvp,
                                  1.,
                                  1,
                                  pvar[ii],
                                  pvar[jj],
                                  pifri,
                                  pifrj,
                                  pjfri,
                                  pjfrj,
                                  i_massflux[face_id],
                                  fluxi,
                                  fluxj);


            cs_i_diff_flux_vector(idiffp,
                                  1.,
                                  pip,
                                  pjp,
                                  pipr,
                                  pjpr,
                                  i_visc[face_id],
                                  fluxi,
                                  fluxj);

           for (int isou = 0; isou < 3; isou++) {
              rhs[ii][isou] -= fluxi[isou];
              rhs[jj][isou] += fluxj[isou];
            }

          }
        }
      }

    /* Unsteady */
    } else {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for reduction(+:n_upwind)
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            /* in parallel, face will be counted by one and only one rank */
            if (ii < n_cells) {
              n_upwind++;
            }

            cs_real_t fluxi[3], fluxj[3] ;
            for (int isou =  0; isou < 3; isou++) {
              fluxi[isou] = 0;
              fluxj[isou] = 0;
            }
            cs_real_3_t pip, pjp;
            cs_real_3_t pif, pjf;

            cs_i_cd_unsteady_upwind_vector(ircflp,
                                           weight[face_id],
                                           cell_cen[ii],
                                           cell_cen[jj],
                                           i_face_cog[face_id],
                                           dijpf[face_id],
                                           (const cs_real_3_t *)grad[ii],
                                           (const cs_real_3_t *)grad[jj],
                                           pvar[ii],
                                           pvar[jj],
                                           pif,
                                           pjf,
                                           pip,
                                           pjp);

            cs_i_conv_flux_vector(iconvp,
                                  thetap,
                                  imasac,
                                  pvar[ii],
                                  pvar[jj],
                                  pif,
                                  pif, /* no relaxation */
                                  pjf,
                                  pjf, /* no relaxation */
                                  i_massflux[face_id],
                                  fluxi,
                                  fluxj);


            cs_i_diff_flux_vector(idiffp,
                                  thetap,
                                  pip,
                                  pjp,
                                  pip, /* no relaxation */
                                  pjp, /* no relaxation */
                                  i_visc[face_id],
                                  fluxi,
                                  fluxj);

            for (int isou = 0; isou < 3; isou++) {
              rhs[ii][isou] -= fluxi[isou];
              rhs[jj][isou] += fluxj[isou];
            }

          }
        }
      }

    }

    /* --> Flux with no slope test
       ============================*/

  } else if (isstpp == 1) {

    if (ischcp < 0 || ischcp > 1) {
      bft_error(__FILE__, __LINE__, 0,
                _("invalid value of ischcv"));
    }

    /* Steady */
    if (idtvar < 0) {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            cs_real_t fluxi[3], fluxj[3] ;
            for (int isou =  0; isou < 3; isou++) {
              fluxi[isou] = 0;
              fluxj[isou] = 0;
            }
            cs_real_3_t pip, pjp, pipr, pjpr;
            cs_real_3_t pifri, pifrj, pjfri, pjfrj;

            cs_i_cd_steady_vector(ircflp,
                                  ischcp,
                                  relaxp,
                                  blencp,
                                  weight[face_id],
                                  cell_cen[ii],
                                  cell_cen[jj],
                                  i_face_cog[face_id],
                                  dijpf[face_id],
                                  (const cs_real_3_t *)grad[ii],
                                  (const cs_real_3_t *)grad[jj],
                                  pvar[ii],
                                  pvar[jj],
                                  pvara[ii],
                                  pvara[jj],
                                  pifri,
                                  pifrj,
                                  pjfri,
                                  pjfrj,
                                  pip,
                                  pjp,
                                  pipr,
                                  pjpr);

            cs_i_conv_flux_vector(iconvp,
                                  1.,
                                  1,
                                  pvar[ii],
                                  pvar[jj],
                                  pifri,
                                  pifrj,
                                  pjfri,
                                  pjfrj,
                                  i_massflux[face_id],
                                  fluxi,
                                  fluxj);


            cs_i_diff_flux_vector(idiffp,
                                  1.,
                                  pip,
                                  pjp,
                                  pipr,
                                  pjpr,
                                  i_visc[face_id],
                                  fluxi,
                                  fluxj);

            for (int isou = 0; isou < 3; isou++) {
              rhs[ii][isou] -= fluxi[isou];
              rhs[jj][isou] += fluxj[isou];
            }

          }
        }
      }

      /* Unsteady */
    } else {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            cs_real_t fluxi[3], fluxj[3] ;
            for (int isou =  0; isou < 3; isou++) {
              fluxi[isou] = 0;
              fluxj[isou] = 0;
            }

            cs_real_3_t pip, pjp;
            cs_real_3_t pif, pjf;

            cs_i_cd_unsteady_vector(ircflp,
                                    ischcp,
                                    blencp,
                                    weight[face_id],
                                    cell_cen[ii],
                                    cell_cen[jj],
                                    i_face_cog[face_id],
                                    dijpf[face_id],
                                    (const cs_real_3_t *)grad[ii],
                                    (const cs_real_3_t *)grad[jj],
                                    pvar[ii],
                                    pvar[jj],
                                    pif,
                                    pjf,
                                    pip,
                                    pjp);

            cs_i_conv_flux_vector(iconvp,
                                  thetap,
                                  imasac,
                                  pvar[ii],
                                  pvar[jj],
                                  pif,
                                  pif, /* no relaxation */
                                  pjf,
                                  pjf, /* no relaxation */
                                  i_massflux[face_id],
                                  fluxi,
                                  fluxj);


            cs_i_diff_flux_vector(idiffp,
                                  thetap,
                                  pip,
                                  pjp,
                                  pip, /* no relaxation */
                                  pjp, /* no relaxation */
                                  i_visc[face_id],
                                  fluxi,
                                  fluxj);

            for (int isou = 0; isou < 3; isou++) {
              rhs[ii][isou] -= fluxi[isou];
              rhs[jj][isou] += fluxj[isou];
            }

          }
        }
      }

    }

    /* --> Flux with slope test
       =========================*/

  } else {

    if (ischcp < 0 || ischcp > 1) {
      bft_error(__FILE__, __LINE__, 0,
                _("invalid value of ischcv"));
    }

    /* Steady */
    if (idtvar < 0) {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for reduction(+:n_upwind)
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            cs_real_t fluxi[3], fluxj[3] ;
            for (int isou =  0; isou < 3; isou++) {
              fluxi[isou] = 0;
              fluxj[isou] = 0;
            }
            cs_real_3_t pip, pjp, pipr, pjpr;
            cs_real_3_t pifri, pifrj, pjfri, pjfrj;
            bool upwind_switch[3];
            cs_i_cd_steady_slope_test_vector(upwind_switch,
                                             iconvp,
                                             ircflp,
                                             ischcp,
                                             relaxp,
                                             blencp,
                                             weight[face_id],
                                             i_dist[face_id],
                                             i_face_surf[face_id],
                                             cell_cen[ii],
                                             cell_cen[jj],
                                             i_face_normal[face_id],
                                             i_face_cog[face_id],
                                             dijpf[face_id],
                                             i_massflux[face_id],
                                             (const cs_real_3_t *)grad[ii],
                                             (const cs_real_3_t *)grad[jj],
                                             (const cs_real_3_t *)grdpa[ii],
                                             (const cs_real_3_t *)grdpa[jj],
                                             pvar[ii],
                                             pvar[jj],
                                             pvara[ii],
                                             pvara[jj],
                                             pifri,
                                             pifrj,
                                             pjfri,
                                             pjfrj,
                                             pip,
                                             pjp,
                                             pipr,
                                             pjpr);

            cs_i_conv_flux_vector(iconvp,
                                  1.,
                                  1,
                                  pvar[ii],
                                  pvar[jj],
                                  pifri,
                                  pifrj,
                                  pjfri,
                                  pjfrj,
                                  i_massflux[face_id],
                                  fluxi,
                                  fluxj);


            cs_i_diff_flux_vector(idiffp,
                                  1.,
                                  pip,
                                  pjp,
                                  pipr,
                                  pjpr,
                                  i_visc[face_id],
                                  fluxi,
                                  fluxj);

            for (int isou = 0; isou < 3; isou++) {
              rhs[ii][isou] -= fluxi[isou];
              rhs[jj][isou] += fluxj[isou];
            }

          }
        }
      }

      /* Unsteady */
    } else {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for reduction(+:n_upwind)
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            cs_real_t fluxi[3], fluxj[3] ;
            for (int isou =  0; isou < 3; isou++) {
              fluxi[isou] = 0;
              fluxj[isou] = 0;
            }
            cs_real_3_t pip, pjp;
            cs_real_3_t pif, pjf;
            bool upwind_switch[3];
            cs_i_cd_unsteady_slope_test_vector(upwind_switch,
                                               iconvp,
                                               ircflp,
                                               ischcp,
                                               blencp,
                                               weight[face_id],
                                               i_dist[face_id],
                                               i_face_surf[face_id],
                                               cell_cen[ii],
                                               cell_cen[jj],
                                               i_face_normal[face_id],
                                               i_face_cog[face_id],
                                               dijpf[face_id],
                                               i_massflux[face_id],
                                               (const cs_real_3_t *)grad[ii],
                                               (const cs_real_3_t *)grad[jj],
                                               (const cs_real_3_t *)grdpa[ii],
                                               (const cs_real_3_t *)grdpa[jj],
                                               pvar[ii],
                                               pvar[jj],
                                               pif,
                                               pjf,
                                               pip,
                                               pjp);

            cs_i_conv_flux_vector(iconvp,
                                  thetap,
                                  imasac,
                                  pvar[ii],
                                  pvar[jj],
                                  pif,
                                  pif, /* no relaxation */
                                  pjf,
                                  pjf, /* no relaxation */
                                  i_massflux[face_id],
                                  fluxi,
                                  fluxj);


            cs_i_diff_flux_vector(idiffp,
                                  thetap,
                                  pip,
                                  pjp,
                                  pip, /* no relaxation */
                                  pjp, /* no relaxation */
                                  i_visc[face_id],
                                  fluxi,
                                  fluxj);

            for (int isou = 0; isou < 3; isou++) {

              if (upwind_switch[isou]) {
                if (ii < n_cells)
                  n_upwind++;
                if (v_slope_test != NULL) {
                  v_slope_test[ii] += fabs(i_massflux[face_id]) / cell_vol[ii];
                  v_slope_test[jj] += fabs(i_massflux[face_id]) / cell_vol[jj];
                }
              }

              rhs[ii][isou] -= fluxi[isou];
              rhs[jj][isou] += fluxj[isou];

            } /* isou */

          }
        }
      }

    } /* idtvar */

  } /* iupwin */

  if (iwarnp >= 2) {

    /* Sum number of clippings */
    cs_parall_counter(&n_upwind, 1);

    bft_printf(_(" %s: %llu Faces with upwind on %llu interior faces \n"),
               var_name, (unsigned long long)n_upwind,
               (unsigned long long)m->n_g_i_faces);
  }

  /* ======================================================================
     ---> Contribution from boundary faces
     ======================================================================*/

  /* Boundary convective flux are all computed with an upwind scheme */
  if (icvflb == 0) {

    /* Steady */
    if (idtvar < 0) {

      for (int g_id = 0; g_id < n_b_groups; g_id++) {
#       pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
        for (int t_id = 0; t_id < n_b_threads; t_id++) {
          for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
               face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = b_face_cells[face_id];

            cs_real_t fluxi[3] ;
            for (int isou =  0; isou < 3; isou++) {
              fluxi[isou] = 0;
            }
            cs_real_3_t pir, pipr;

            cs_b_cd_steady_vector(ircflp,
                                  relaxp,
                                  diipb[face_id],
                                  (const cs_real_3_t *)grad[ii],
                                  pvar[ii],
                                  pvara[ii],
                                  pir,
                                  pipr);

            cs_b_upwind_flux_vector(iconvp,
                                    1., /* thetap */
                                    1, /* imasac */
                                    inc,
                                    bc_type[face_id],
                                    pvar[ii],
                                    pir,
                                    pipr,
                                    coefav[face_id],
                                    coefbv[face_id],
                                    b_massflux[face_id],
                                    fluxi);

            cs_b_diff_flux_vector(idiffp,
                                  1., /* thetap */
                                  inc,
                                  pipr,
                                  cofafv[face_id],
                                  cofbfv[face_id],
                                  b_visc[face_id],
                                  fluxi);

            for (int isou = 0; isou < 3; isou++) {
              rhs[ii][isou] -= fluxi[isou];
            } /* isou */

          }
        }
      }

      /* Unsteady */
    } else {

      for (int g_id = 0; g_id < n_b_groups; g_id++) {
#       pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
        for (int t_id = 0; t_id < n_b_threads; t_id++) {
          for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
               face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = b_face_cells[face_id];

            cs_real_t fluxi[3] ;
            for (int isou =  0; isou < 3; isou++) {
              fluxi[isou] = 0;
            }
            cs_real_3_t pip;

            cs_b_cd_unsteady_vector(ircflp,
                                    diipb[face_id],
                                    (const cs_real_3_t *)grad[ii],
                                    pvar[ii],
                                    pip);

            cs_b_upwind_flux_vector(iconvp,
                                    thetap,
                                    imasac,
                                    inc,
                                    bc_type[face_id],
                                    pvar[ii],
                                    pvar[ii], /* no relaxation */
                                    pip,
                                    coefav[face_id],
                                    coefbv[face_id],
                                    b_massflux[face_id],
                                    fluxi);

            cs_b_diff_flux_vector(idiffp,
                                  thetap,
                                  inc,
                                  pip,
                                  cofafv[face_id],
                                  cofbfv[face_id],
                                  b_visc[face_id],
                                  fluxi);

            for(int isou = 0; isou < 3; isou++) {
              rhs[ii][isou] -= fluxi[isou];
            }

          }
        }
      }

    } /* idtvar */

    /* Boundary convective flux imposed at some faces (tags in icvfli array) */
  } else if (icvflb == 1) {

    /* Retrieve the value of the convective flux to be imposed */
    if (f_id != -1) {
      coface = (cs_real_3_t *)(f->bc_coeffs->ac);
      cofbce = (const cs_real_33_t *)(f->bc_coeffs->bc);
    } else {
      bft_error(__FILE__, __LINE__, 0,
                _("invalid value of icvflb and f_id"));
    }

    /* Steady */
    if (idtvar < 0) {

      for (int g_id = 0; g_id < n_b_groups; g_id++) {
#       pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
        for (int t_id = 0; t_id < n_b_threads; t_id++) {
          for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
               face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = b_face_cells[face_id];

            cs_real_t fluxi[3], pir[3], pipr[3];

            for (int isou =  0; isou < 3; isou++) {
              fluxi[isou] = 0;
            }

            cs_b_cd_steady_vector(ircflp,
                                  relaxp,
                                  diipb[face_id],
                                  (const cs_real_3_t *)grad[ii],
                                  pvar[ii],
                                  pvara[ii],
                                  pir,
                                  pipr);

            cs_b_imposed_conv_flux_vector(iconvp,
                                          1., /* thetap */
                                          1., /* imasac */
                                          inc,
                                          bc_type[face_id],
                                          icvfli[face_id],
                                          pvar[ii],
                                          pir,
                                          pipr,
                                          coefav[face_id],
                                          coefbv[face_id],
                                          coface[face_id],
                                          cofbce[face_id],
                                          b_massflux[face_id],
                                          fluxi);

            cs_b_diff_flux_vector(idiffp,
                                  1., /* thetap */
                                  inc,
                                  pipr,
                                  cofafv[face_id],
                                  cofbfv[face_id],
                                  b_visc[face_id],
                                  fluxi);

            for (int isou = 0; isou < 3; isou++) {
              rhs[ii][isou] -= fluxi[isou];
            }

          }
        }
      }

      /* Unsteady */
    } else {

      for (int g_id = 0; g_id < n_b_groups; g_id++) {
#       pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
        for (int t_id = 0; t_id < n_b_threads; t_id++) {
          for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
               face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = b_face_cells[face_id];

            cs_real_t fluxi[3] ;
            for (int isou =  0; isou < 3; isou++) {
              fluxi[isou] = 0;
            }
            cs_real_3_t pip;

            cs_b_cd_unsteady_vector(ircflp,
                                    diipb[face_id],
                                    (const cs_real_3_t *)grad[ii],
                                    pvar[ii],
                                    pip);

            cs_b_imposed_conv_flux_vector(iconvp,
                                          thetap,
                                          imasac,
                                          inc,
                                          bc_type[face_id],
                                          icvfli[face_id],
                                          pvar[ii],
                                          pvar[ii], /* no relaxation */
                                          pip,
                                          coefav[face_id],
                                          coefbv[face_id],
                                          coface[face_id],
                                          cofbce[face_id],
                                          b_massflux[face_id],
                                          fluxi);

            cs_b_diff_flux_vector(idiffp,
                                  thetap,
                                  inc,
                                  pip,
                                  cofafv[face_id],
                                  cofbfv[face_id],
                                  b_visc[face_id],
                                  fluxi);

            for (int isou = 0; isou < 3; isou++) {
              rhs[ii][isou] -= fluxi[isou];
            }

          }
        }
      }

    } /* idtvar */

  }

  /* 3. Computation of the transpose grad(vel) term and grad(-2/3 div(vel)) */

  if (ivisep == 1) {

    /* We do not know what condition to put in the inlets and the outlets, so we
       assume that there is an equilibrium. Moreover, cells containing a coupled
       are removed. */

    /* Allocate a temporary array */
    BFT_MALLOC(bndcel, n_cells_ext, cs_real_t);

#   pragma omp parallel for
    for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++)
      bndcel[cell_id] = 1.;

#   pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
    for (cs_lnum_t face_id = 0; face_id < m->n_b_faces; face_id++) {
      int ityp = bc_type[face_id];
      if (   ityp == CS_OUTLET
          || ityp == CS_INLET
          || ityp == CS_CONVECTIVE_INLET
          || ityp == CS_COUPLED_FD)
        bndcel[b_face_cells[face_id]] = 0.;
    }

    if (halo != NULL)
      cs_halo_sync_var(halo, halo_type, bndcel);

    /* ---> Interior faces */

    for (int g_id = 0; g_id < n_i_groups; g_id++) {
#     pragma omp parallel for
      for (int t_id = 0; t_id < n_i_threads; t_id++) {
        for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
             face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = i_face_cells[face_id][0];
          cs_lnum_t jj = i_face_cells[face_id][1];

          double pnd = weight[face_id];
          double secvis = secvif[face_id];
          double visco = i_visc[face_id];

          double grdtrv =      pnd*(grad[ii][0][0]+grad[ii][1][1]+grad[ii][2][2])
                 + (1.-pnd)*(grad[jj][0][0]+grad[jj][1][1]+grad[jj][2][2]);

          double tgrdfl;
          /* We need to compute trans_grad(u).IJ which is equal to IJ.grad(u) */

          for (int isou = 0; isou < 3; isou++) {

            tgrdfl = dijpf[face_id][0] * (      pnd*grad[ii][0][isou]
                                         + (1.-pnd)*grad[jj][0][isou])
                   + dijpf[face_id][1] * (      pnd*grad[ii][1][isou]
                                         + (1.-pnd)*grad[jj][1][isou])
                   + dijpf[face_id][2] * (      pnd*grad[ii][2][isou]
                                         + (1.-pnd)*grad[jj][2][isou]);

            double flux = visco*tgrdfl + secvis*grdtrv*i_f_face_normal[face_id][isou];

            rhs[ii][isou] = rhs[ii][isou] + flux*bndcel[ii];
            rhs[jj][isou] = rhs[jj][isou] - flux*bndcel[jj];

          }

        }
      }
    }

    /* ---> Boundary FACES
       the whole flux term of the stress tensor is already taken into account
       (so, no corresponding term in forbr)
       TODO in theory we should take the normal component into account (the
       tangential one is modeled by the wall law) */

    /*Free memory */
    BFT_FREE(bndcel);

  }

  /* Free memory */
  BFT_FREE(grdpa);
  BFT_FREE(grad);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Add the explicit part of the convection/diffusion terms of a transport
 *  equation of a vector field \f$ \vect{\varia} \f$.
 *
 * More precisely, the right hand side \f$ \vect{Rhs} \f$ is updated as
 * follows:
 * \f[
 *  \vect{Rhs} = \vect{Rhs} - \sum_{\fij \in \Facei{\celli}}      \left(
 *         \dot{m}_\ij \left( \vect{\varia}_\fij - \vect{\varia}_\celli \right)
 *       - \mu_\fij \gradt_\fij \vect{\varia} \cdot \vect{S}_\ij  \right)
 * \f]
 *
 * Warning:
 * - \f$ \vect{Rhs} \f$ has already been initialized before calling bilsc!
 * - mind the sign minus
 *
 * \param[in]     idtvar        indicator of the temporal scheme
 * \param[in]     f_id          index of the current variable
 * \param[in]     var_cal_opt   variable calculation options
 * \param[in]     icvflb        global indicator of boundary convection flux
 *                               - 0 upwind scheme at all boundary faces
 *                               - 1 imposed flux at some boundary faces
 * \param[in]     inc           indicator
 *                               - 0 when solving an increment
 *                               - 1 otherwise
 * \param[in]     imasac        take mass accumulation into account?
 * \param[in]     pvar          solved velocity (current time step)
 * \param[in]     pvara         solved velocity (previous time step)
 * \param[in]     coefa         boundary condition array for the variable
 *                               (Explicit part)
 * \param[in]     coefb         boundary condition array for the variable
 *                               (Implicit part)
 * \param[in]     cofaf         boundary condition array for the diffusion
 *                               of the variable (Explicit part)
 * \param[in]     cofbf         boundary condition array for the diffusion
 *                               of the variable (Implicit part)
 * \param[in]     i_massflux    mass flux at interior faces
 * \param[in]     b_massflux    mass flux at boundary faces
 * \param[in]     i_visc        \f$ \mu_\fij \dfrac{S_\fij}{\ipf \jpf} \f$
 *                               at interior faces for the r.h.s.
 * \param[in]     b_visc        \f$ \mu_\fib \dfrac{S_\fib}{\ipf \centf} \f$
 *                               at border faces for the r.h.s.
 * \param[in,out] rhs           right hand side \f$ \vect{Rhs} \f$
 */
/*----------------------------------------------------------------------------*/

void
cs_convection_diffusion_tensor(int                         idtvar,
                               int                         f_id,
                               const cs_var_cal_opt_t      var_cal_opt,
                               int                         icvflb,
                               int                         inc,
                               int                         imasac,
                               cs_real_6_t       *restrict pvar,
                               const cs_real_6_t *restrict pvara,
                               const cs_real_6_t           coefa[],
                               const cs_real_66_t          coefb[],
                               const cs_real_6_t           cofaf[],
                               const cs_real_66_t          cofbf[],
                               const cs_real_t             i_massflux[],
                               const cs_real_t             b_massflux[],
                               const cs_real_t             i_visc[],
                               const cs_real_t             b_visc[],
                               cs_real_6_t       *restrict rhs)
{
  const int iconvp = var_cal_opt.iconv;
  const int idiffp = var_cal_opt.idiff;
  const int nswrgp = var_cal_opt.nswrgr;
  const int imrgra = var_cal_opt.imrgra;
  const int imligp = var_cal_opt.imligr;
  const int ircflp = var_cal_opt.ircflu;
  const int ischcp = var_cal_opt.ischcv;
  const int isstpp = var_cal_opt.isstpc;
  const int iwarnp = var_cal_opt.iwarni;
  const double blencp = var_cal_opt.blencv;
  const double epsrgp = var_cal_opt.epsrgr;
  const double climgp = var_cal_opt.climgr;
  const double relaxp = var_cal_opt.relaxv;
  const double thetap = var_cal_opt.thetav;

  const cs_mesh_t  *m = cs_glob_mesh;
  cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  const cs_lnum_t n_cells = m->n_cells;
  const cs_lnum_t n_cells_ext = m->n_cells_with_ghosts;
  const int n_i_groups = m->i_face_numbering->n_groups;
  const int n_i_threads = m->i_face_numbering->n_threads;
  const int n_b_groups = m->b_face_numbering->n_groups;
  const int n_b_threads = m->b_face_numbering->n_threads;
  const cs_lnum_t *restrict i_group_index = m->i_face_numbering->group_index;
  const cs_lnum_t *restrict b_group_index = m->b_face_numbering->group_index;

  const cs_lnum_2_t *restrict i_face_cells
    = (const cs_lnum_2_t *restrict)m->i_face_cells;
  const cs_lnum_t *restrict b_face_cells
    = (const cs_lnum_t *restrict)m->b_face_cells;
  const cs_real_t *restrict weight = fvq->weight;
  const cs_real_t *restrict i_dist = fvq->i_dist;
  const cs_real_t *restrict i_face_surf = fvq->i_face_surf;
  const cs_real_t *restrict cell_vol = fvq->cell_vol;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)fvq->cell_cen;
  const cs_real_3_t *restrict i_face_normal
    = (const cs_real_3_t *restrict)fvq->i_face_normal;
  const cs_real_3_t *restrict i_face_cog
    = (const cs_real_3_t *restrict)fvq->i_face_cog;
  const cs_real_3_t *restrict dijpf
    = (const cs_real_3_t *restrict)fvq->dijpf;
  const cs_real_3_t *restrict diipb
    = (const cs_real_3_t *restrict)fvq->diipb;

  const int *bc_type = cs_glob_bc_type;

  /* Local variables */

  char var_name[32];

  int tr_dim = 0;

  cs_gnum_t n_upwind;
  int iupwin;

  cs_real_63_t *grad, *grdpa;

  cs_field_t *f;

  cs_real_t  *v_slope_test = _get_v_slope_test(f_id, var_cal_opt);

  /*==========================================================================*/

  /* 1. Initialization */

  /* Allocate work arrays */

  BFT_MALLOC(grad, n_cells_ext, cs_real_63_t);
  BFT_MALLOC(grdpa, n_cells_ext, cs_real_63_t);

  /* Choose gradient type */

  cs_halo_type_t halo_type = CS_HALO_STANDARD;
  cs_gradient_type_t gradient_type = CS_GRADIENT_ITER;

  cs_gradient_type_by_imrgra(imrgra,
                             &gradient_type,
                             &halo_type);

  if (f_id != -1) {
    f = cs_field_by_id(f_id);
    cs_gradient_perio_init_rij_tensor(&tr_dim, grad);
    snprintf(var_name, 31, "%s", f->name); var_name[31] = '\0';
  }
  else
    strcpy(var_name, "Work array"); var_name[31] = '\0';

  if (iwarnp >= 2 && iconvp == 1) {
    if (ischcp == 1) {
      bft_printf
        (
         _(" %s: Convection in centered blending with %f percent of upwind\n"),
         var_name, (1.-blencp)*100.);
    } else {
      bft_printf
        (
         _(" %s: Convection in 2nd order blending with %f percent of upwind\n"),
         var_name, (1.-blencp)*100.);
    }
  }

  iupwin = (blencp > 0.) ? 0 : 1;

  /* 2. Compute the balance with reconstruction */

  /* Compute the gradient of the velocity

     The gradient (grad) is used in the flux reconstruction and the slope test.
     Thus we must compute it:
         - when we have diffusion and we reconstruct the fluxes,
         - when the convection scheme is SOLU,
         - when we have convection, we are not in pure upwind
           and we reconstruct the fluxes,
         - when we have convection, we are not in pure upwind
           and we have not shunted the slope test. */

  if (  (idiffp != 0 && ircflp == 1)
     || (   iconvp != 0 && iupwin == 0
         && (ischcp == 0 || ircflp == 1 || isstpp == 0))) {

    cs_gradient_tensor(var_name,
                       gradient_type,
                       halo_type,
                       inc,
                       nswrgp,
                       iwarnp,
                       imligp,
                       epsrgp,
                       climgp,
                       coefa,
                       coefb,
                       pvar,
                       grad);

  } else {
#   pragma omp parallel for
    for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++) {
      for (int isou = 0; isou < 6; isou++) {
        for (int jsou = 0; jsou < 3; jsou++)
          grad[cell_id][isou][jsou] = 0.;
      }
    }
  }

  /* ======================================================================
     ---> Compute uncentred gradient grdpa for the slope test
     ======================================================================*/

# pragma omp parallel for
  for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++) {
    for (int jsou = 0; jsou < 3; jsou++) {
      for (int isou = 0; isou < 6; isou++)
        grdpa[cell_id][isou][jsou] = 0.;
    }
  }

  if (iconvp > 0 && iupwin == 0 && isstpp == 0) {

    cs_slope_test_gradient_tensor(inc,
                                  halo_type,
                                  grad,
                                  grdpa,
                                  pvar,
                                  coefa,
                                  coefb,
                                  i_massflux);

  }

  /* ======================================================================
     ---> Contribution from interior faces
     ======================================================================*/

  n_upwind = 0;

  if (n_cells_ext > n_cells) {
#   pragma omp parallel for
    for (cs_lnum_t cell_id = n_cells; cell_id < n_cells_ext; cell_id++) {
      for (int isou = 0; isou < 6; isou++)
        rhs[cell_id][isou] = 0.;
    }
  }

  /* --> Pure upwind flux
     =====================*/

  if (iupwin == 1) {

    /* Steady */
    if (idtvar < 0) {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for reduction(+:n_upwind)
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            /* in parallel, face will be counted by one and only one rank */
            if (ii < n_cells) {
              n_upwind++;
            }
            double fluxi[6], fluxj[6] ;
            for (int isou =  0; isou < 6; isou++) {
              fluxi[isou] = 0;
              fluxj[isou] = 0;
            }
            cs_real_6_t pip, pjp, pipr, pjpr;
            cs_real_6_t pifri, pifrj, pjfri, pjfrj;
            cs_i_cd_steady_upwind_tensor(ircflp,
                                         relaxp,
                                         weight[face_id],
                                         cell_cen[ii],
                                         cell_cen[jj],
                                         i_face_cog[face_id],
                                         dijpf[face_id],
                                         (const cs_real_3_t *)grad[ii],
                                         (const cs_real_3_t *)grad[jj],
                                         pvar[ii],
                                         pvar[jj],
                                         pvara[ii],
                                         pvara[jj],
                                         pifri,
                                         pifrj,
                                         pjfri,
                                         pjfrj,
                                         pip,
                                         pjp,
                                         pipr,
                                         pjpr);


            cs_i_conv_flux_tensor(iconvp,
                                  1.,
                                  1,
                                  pvar[ii],
                                  pvar[jj],
                                  pifri,
                                  pifrj,
                                  pjfri,
                                  pjfrj,
                                  i_massflux[face_id],
                                  fluxi,
                                  fluxj);


            cs_i_diff_flux_tensor(idiffp,
                                  1.,
                                  pip,
                                  pjp,
                                  pipr,
                                  pjpr,
                                  i_visc[face_id],
                                  fluxi,
                                  fluxj);

           for (int isou = 0; isou < 6; isou++) {
              rhs[ii][isou] -= fluxi[isou];
              rhs[jj][isou] += fluxj[isou];
            }

          }
        }
      }

    /* Unsteady */
    } else {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for reduction(+:n_upwind)
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            /* in parallel, face will be counted by one and only one rank */
            if (ii < n_cells) {
              n_upwind++;
            }
            double fluxi[6], fluxj[6] ;
            for (int isou =  0; isou < 6; isou++) {
              fluxi[isou] = 0;
              fluxj[isou] = 0;
            }
            cs_real_6_t pip, pjp;
            cs_real_6_t pif, pjf;

            cs_i_cd_unsteady_upwind_tensor(ircflp,
                                           weight[face_id],
                                           cell_cen[ii],
                                           cell_cen[jj],
                                           i_face_cog[face_id],
                                           dijpf[face_id],
                                           (const cs_real_3_t *)grad[ii],
                                           (const cs_real_3_t *)grad[jj],
                                           pvar[ii],
                                           pvar[jj],
                                           pif,
                                           pjf,
                                           pip,
                                           pjp);

            cs_i_conv_flux_tensor(iconvp,
                                  thetap,
                                  imasac,
                                  pvar[ii],
                                  pvar[jj],
                                  pif,
                                  pif, /* no relaxation */
                                  pjf,
                                  pjf, /* no relaxation */
                                  i_massflux[face_id],
                                  fluxi,
                                  fluxj);


            cs_i_diff_flux_tensor(idiffp,
                                  thetap,
                                  pip,
                                  pjp,
                                  pip, /* no relaxation */
                                  pjp, /* no relaxation */
                                  i_visc[face_id],
                                  fluxi,
                                  fluxj);

            for (int isou = 0; isou < 6; isou++) {

              rhs[ii][isou] -= fluxi[isou];
              rhs[jj][isou] += fluxj[isou];
            }

          }
        }
      }

    }

    /* --> Flux with no slope test
       ============================*/

  } else if (isstpp == 1) {

    if (ischcp < 0 || ischcp > 1) {
      bft_error(__FILE__, __LINE__, 0,
                _("invalid value of ischcp"));
    }

    /* Steady */
    if (idtvar < 0) {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for

        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            double fluxi[6], fluxj[6] ;
            for (int isou =  0; isou < 6; isou++) {
              fluxi[isou] = 0;
              fluxj[isou] = 0;
            }
            cs_real_6_t pip, pjp, pipr, pjpr;
            cs_real_6_t pifri, pifrj, pjfri, pjfrj;

            cs_i_cd_steady_tensor(ircflp,
                                  ischcp,
                                  relaxp,
                                  blencp,
                                  weight[face_id],
                                  cell_cen[ii],
                                  cell_cen[jj],
                                  i_face_cog[face_id],
                                  dijpf[face_id],
                                  (const cs_real_3_t *)grad[ii],
                                  (const cs_real_3_t *)grad[jj],
                                  pvar[ii],
                                  pvar[jj],
                                  pvara[ii],
                                  pvara[jj],
                                  pifri,
                                  pifrj,
                                  pjfri,
                                  pjfrj,
                                  pip,
                                  pjp,
                                  pipr,
                                  pjpr);

            cs_i_conv_flux_tensor(iconvp,
                                  1.,
                                  1,
                                  pvar[ii],
                                  pvar[jj],
                                  pifri,
                                  pifrj,
                                  pjfri,
                                  pjfrj,
                                  i_massflux[face_id],
                                  fluxi,
                                  fluxj);


            cs_i_diff_flux_tensor(idiffp,
                                  1.,
                                  pip,
                                  pjp,
                                  pipr,
                                  pjpr,
                                  i_visc[face_id],
                                  fluxi,
                                  fluxj);

            for (int isou = 0; isou < 6; isou++) {
              rhs[ii][isou] -= fluxi[isou];
              rhs[jj][isou] += fluxj[isou];
            }

          }
        }
      }

      /* Unsteady */
    } else {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            double fluxi[6], fluxj[6] ;
            for (int isou =  0; isou < 6; isou++) {
              fluxi[isou] = 0;
              fluxj[isou] = 0;
            }
            cs_real_6_t pip, pjp;
            cs_real_6_t pif, pjf;

            cs_i_cd_unsteady_tensor(ircflp,
                                    ischcp,
                                    blencp,
                                    weight[face_id],
                                    cell_cen[ii],
                                    cell_cen[jj],
                                    i_face_cog[face_id],
                                    dijpf[face_id],
                                    (const cs_real_3_t *)grad[ii],
                                    (const cs_real_3_t *)grad[jj],
                                    pvar[ii],
                                    pvar[jj],
                                    pif,
                                    pjf,
                                    pip,
                                    pjp);

            cs_i_conv_flux_tensor(iconvp,
                                  thetap,
                                  imasac,
                                  pvar[ii],
                                  pvar[jj],
                                  pif,
                                  pif, /* no relaxation */
                                  pjf,
                                  pjf, /* no relaxation */
                                  i_massflux[face_id],
                                  fluxi,
                                  fluxj);


            cs_i_diff_flux_tensor(idiffp,
                                  thetap,
                                  pip,
                                  pjp,
                                  pip, /* no relaxation */
                                  pjp, /* no relaxation */
                                  i_visc[face_id],
                                  fluxi,
                                  fluxj);

            for (int isou = 0; isou < 6; isou++) {
              rhs[ii][isou] -= fluxi[isou];
              rhs[jj][isou] += fluxj[isou];
            }
          }
        }
      }
    }

    /* --> Flux with slope test
       =========================*/

  } else {

    if (ischcp < 0 || ischcp > 1) {
      bft_error(__FILE__, __LINE__, 0,
                _("invalid value of ischcp"));
    }

    /* Steady */
    if (idtvar < 0) {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for reduction(+:n_upwind)
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            double fluxi[6], fluxj[6] ;
            for (int isou =  0; isou < 6; isou++) {
              fluxi[isou] = 0;
              fluxj[isou] = 0;
            }
            cs_real_6_t pip, pjp, pipr, pjpr;
            cs_real_6_t pifri, pifrj, pjfri, pjfrj;
            bool upwind_switch[6];
            cs_i_cd_steady_slope_test_tensor(upwind_switch,
                                             iconvp,
                                             ircflp,
                                             ischcp,
                                             relaxp,
                                             blencp,
                                             weight[face_id],
                                             i_dist[face_id],
                                             i_face_surf[face_id],
                                             cell_cen[ii],
                                             cell_cen[jj],
                                             i_face_normal[face_id],
                                             i_face_cog[face_id],
                                             dijpf[face_id],
                                             i_massflux[face_id],
                                             (const cs_real_3_t *)grad[ii],
                                             (const cs_real_3_t *)grad[jj],
                                             (const cs_real_3_t *)grdpa[ii],
                                             (const cs_real_3_t *)grdpa[jj],
                                             pvar[ii],
                                             pvar[jj],
                                             pvara[ii],
                                             pvara[jj],
                                             pifri,
                                             pifrj,
                                             pjfri,
                                             pjfrj,
                                             pip,
                                             pjp,
                                             pipr,
                                             pjpr);

            cs_i_conv_flux_tensor(iconvp,
                                  1.,
                                  1,
                                  pvar[ii],
                                  pvar[jj],
                                  pifri,
                                  pifrj,
                                  pjfri,
                                  pjfrj,
                                  i_massflux[face_id],
                                  fluxi,
                                  fluxj);


            cs_i_diff_flux_tensor(idiffp,
                                  1.,
                                  pip,
                                  pjp,
                                  pipr,
                                  pjpr,
                                  i_visc[face_id],
                                  fluxi,
                                  fluxj);

            for (int isou = 0; isou < 6; isou++) {
              rhs[ii][isou] -= fluxi[isou];
              rhs[jj][isou] += fluxj[isou];
            }

          }
        }
      }

      /* Unsteady */
    } else {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for reduction(+:n_upwind)
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            double fluxi[6], fluxj[6] ;
            for (int isou =  0; isou < 6; isou++) {
              fluxi[isou] = 0;
              fluxj[isou] = 0;
            }
            cs_real_6_t pip, pjp;
            cs_real_6_t pif, pjf;
            bool upwind_switch[6];
            cs_i_cd_unsteady_slope_test_tensor(upwind_switch,
                                               iconvp,
                                               ircflp,
                                               ischcp,
                                               blencp,
                                               weight[face_id],
                                               i_dist[face_id],
                                               i_face_surf[face_id],
                                               cell_cen[ii],
                                               cell_cen[jj],
                                               i_face_normal[face_id],
                                               i_face_cog[face_id],
                                               dijpf[face_id],
                                               i_massflux[face_id],
                                               (const cs_real_3_t *)grad[ii],
                                               (const cs_real_3_t *)grad[jj],
                                               (const cs_real_3_t *)grdpa[ii],
                                               (const cs_real_3_t *)grdpa[jj],
                                               pvar[ii],
                                               pvar[jj],
                                               pif,
                                               pjf,
                                               pip,
                                               pjp);

            cs_i_conv_flux_tensor(iconvp,
                                  thetap,
                                  imasac,
                                  pvar[ii],
                                  pvar[jj],
                                  pif,
                                  pif, /* no relaxation */
                                  pjf,
                                  pjf, /* no relaxation */
                                  i_massflux[face_id],
                                  fluxi,
                                  fluxj);


            cs_i_diff_flux_tensor(idiffp,
                                  thetap,
                                  pip,
                                  pjp,
                                  pip, /* no relaxation */
                                  pjp, /* no relaxation */
                                  i_visc[face_id],
                                  fluxi,
                                  fluxj);

            for (int isou = 0; isou < 6; isou++) {

              if (upwind_switch[isou]) {
                if (ii < n_cells)
                  n_upwind++;
                if (v_slope_test != NULL) {
                  v_slope_test[ii] += fabs(i_massflux[face_id]) / cell_vol[ii];
                  v_slope_test[jj] += fabs(i_massflux[face_id]) / cell_vol[jj];
                }
              }

              rhs[ii][isou] -= fluxi[isou];
              rhs[jj][isou] += fluxj[isou];
            } /* isou */
          }
        }
      }
    } /* idtvar */
  } /* iupwin */

  if (iwarnp >= 2) {

    /* Sum number of clippings */
    cs_parall_counter(&n_upwind, 1);

    bft_printf(_(" %s: %llu Faces with upwind on %llu interior faces \n"),
               var_name, (unsigned long long)n_upwind,
               (unsigned long long)m->n_g_i_faces);
  }

  /* ======================================================================
     ---> Contribution from boundary faces
     ======================================================================*/

  /* Boundary convective flux are all computed with an upwind scheme */
  if (icvflb == 0) {

    /* Steady */
    if (idtvar < 0) {

      for (int g_id = 0; g_id < n_b_groups; g_id++) {
#       pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
        for (int t_id = 0; t_id < n_b_threads; t_id++) {
          for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
               face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = b_face_cells[face_id];

            double fluxi[6] ;
            for (int isou =  0; isou < 6; isou++) {
              fluxi[isou] = 0;
            }
            cs_real_6_t pir, pipr;

            cs_b_cd_steady_tensor(ircflp,
                                  relaxp,
                                  diipb[face_id],
                                  (const cs_real_3_t *)grad[ii],
                                  pvar[ii],
                                  pvara[ii],
                                  pir,
                                  pipr);

            cs_b_upwind_flux_tensor(iconvp,
                                    1., /* thetap */
                                    1, /* imasac */
                                    inc,
                                    bc_type[face_id],
                                    pvar[ii],
                                    pir,
                                    pipr,
                                    coefa[face_id],
                                    coefb[face_id],
                                    b_massflux[face_id],
                                    fluxi);

            cs_b_diff_flux_tensor(idiffp,
                                  1., /* thetap */
                                  inc,
                                  pipr,
                                  cofaf[face_id],
                                  cofbf[face_id],
                                  b_visc[face_id],
                                  fluxi);

            for (int isou = 0; isou < 6; isou++) {
              rhs[ii][isou] -= fluxi[isou];
            }
          }
        }
      }

      /* Unsteady */
    } else {

      for (int g_id = 0; g_id < n_b_groups; g_id++) {
#       pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
        for (int t_id = 0; t_id < n_b_threads; t_id++) {
          for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
               face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = b_face_cells[face_id];

            double fluxi[6] ;
            for (int isou =  0; isou < 6; isou++) {
              fluxi[isou] = 0;
            }
            cs_real_6_t pip;

            cs_b_cd_unsteady_tensor(ircflp,
                                    diipb[face_id],
                                    (const cs_real_3_t *)grad[ii],
                                    pvar[ii],
                                    pip);

            cs_b_upwind_flux_tensor(iconvp,
                                    thetap,
                                    imasac,
                                    inc,
                                    bc_type[face_id],
                                    pvar[ii],
                                    pvar[ii], /* no relaxation */
                                    pip,
                                    coefa[face_id],
                                    coefb[face_id],
                                    b_massflux[face_id],
                                    fluxi);

            cs_b_diff_flux_tensor(idiffp,
                                  thetap,
                                  inc,
                                  pip,
                                  cofaf[face_id],
                                  cofbf[face_id],
                                  b_visc[face_id],
                                  fluxi);

            for(int isou = 0; isou < 6; isou++) {
              rhs[ii][isou] -= fluxi[isou];
            } /* isou */
          }
        }
      }

    } /* idtvar */

  }

  /* Free memory */
  BFT_FREE(grdpa);
  BFT_FREE(grad);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Add the explicit part of the convection/diffusion terms of a transport
 * equation of a scalar field \f$ \varia \f$ such as the temperature.
 *
 * More precisely, the right hand side \f$ Rhs \f$ is updated as
 * follows:
 * \f[
 * Rhs = Rhs + \sum_{\fij \in \Facei{\celli}}      \left(
 *        C_p\dot{m}_\ij \varia_\fij
 *      - \lambda_\fij \gradv_\fij \varia \cdot \vect{S}_\ij  \right)
 * \f]
 *
 * Warning:
 * \f$ Rhs \f$ has already been initialized before calling bilsct!
 *
 * \param[in]     idtvar        indicator of the temporal scheme
 * \param[in]     f_id          index of the current variable
 * \param[in]     var_cal_opt   variable calculation options
 * \param[in]     inc           indicator
 *                               - 0 when solving an increment
 *                               - 1 otherwise
 * \param[in]     iccocg        indicator
 *                               - 1 re-compute cocg matrix
 *                                 (for iterative gradients)
 *                               - 0 otherwise
 * \param[in]     imasac        take mass accumulation into account?
 * \param[in]     pvar          solved variable (current time step)
 * \param[in]     pvara         solved variable (previous time step)
 * \param[in]     coefap        boundary condition array for the variable
 *                               (explicit part)
 * \param[in]     coefbp        boundary condition array for the variable
 *                               (implicit part)
 * \param[in]     cofafp        boundary condition array for the diffusion
 *                               of the variable (explicit part)
 * \param[in]     cofbfp        boundary condition array for the diffusion
 *                               of the variable (implicit part)
 * \param[in]     i_massflux    mass flux at interior faces
 * \param[in]     b_massflux    mass flux at boundary faces
 * \param[in]     i_visc        \f$ \mu_\fij \dfrac{S_\fij}{\ipf \jpf} \f$
 *                               at interior faces for the r.h.s.
 * \param[in]     b_visc        \f$ \mu_\fib \dfrac{S_\fib}{\ipf \centf} \f$
 *                               at border faces for the r.h.s.
 * \param[in]     xcpp          array of specific heat (\f$ C_p \f$)
 * \param[in,out] rhs           right hand side \f$ \vect{Rhs} \f$
 */
/*----------------------------------------------------------------------------*/

void
cs_convection_diffusion_thermal(int                       idtvar,
                                int                       f_id,
                                const cs_var_cal_opt_t    var_cal_opt,
                                int                       inc,
                                int                       iccocg,
                                int                       imasac,
                                cs_real_t       *restrict pvar,
                                const cs_real_t *restrict pvara,
                                const cs_real_t           coefap[],
                                const cs_real_t           coefbp[],
                                const cs_real_t           cofafp[],
                                const cs_real_t           cofbfp[],
                                const cs_real_t           i_massflux[],
                                const cs_real_t           b_massflux[],
                                const cs_real_t           i_visc[],
                                const cs_real_t           b_visc[],
                                const cs_real_t           xcpp[],
                                cs_real_t       *restrict rhs)
{
  const int iconvp = var_cal_opt.iconv ;
  const int idiffp = var_cal_opt.idiff ;
  const int nswrgp = var_cal_opt.nswrgr;
  const int imrgra = var_cal_opt.imrgra;
  const int imligp = var_cal_opt.imligr;
  const int ircflp = var_cal_opt.ircflu;
  const int ischcp = var_cal_opt.ischcv;
  const int isstpp = var_cal_opt.isstpc;
  const int iwarnp = var_cal_opt.iwarni;
  int limiter_choice = -1;
  const double blencp = var_cal_opt.blencv;
  const double epsrgp = var_cal_opt.epsrgr;
  const double climgp = var_cal_opt.climgr;
  const double extrap = var_cal_opt.extrag;
  const double relaxp = var_cal_opt.relaxv;
  const double thetap = var_cal_opt.thetav;

  const cs_mesh_t  *m = cs_glob_mesh;
  cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  const cs_lnum_t n_cells = m->n_cells;
  const cs_lnum_t n_cells_ext = m->n_cells_with_ghosts;
  const int n_i_groups = m->i_face_numbering->n_groups;
  const int n_i_threads = m->i_face_numbering->n_threads;
  const int n_b_groups = m->b_face_numbering->n_groups;
  const int n_b_threads = m->b_face_numbering->n_threads;
  const cs_lnum_t *restrict i_group_index = m->i_face_numbering->group_index;
  const cs_lnum_t *restrict b_group_index = m->b_face_numbering->group_index;

  const cs_lnum_2_t *restrict i_face_cells
    = (const cs_lnum_2_t *restrict)m->i_face_cells;
  const cs_lnum_t *restrict b_face_cells
    = (const cs_lnum_t *restrict)m->b_face_cells;
  const cs_real_t *restrict weight = fvq->weight;
  const cs_real_t *restrict i_dist = fvq->i_dist;
  const cs_real_t *restrict i_face_surf = fvq->i_face_surf;
  const cs_real_t *restrict cell_vol = fvq->cell_vol;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)fvq->cell_cen;
  const cs_real_3_t *restrict i_face_normal
    = (const cs_real_3_t *restrict)fvq->i_face_normal;
  const cs_real_3_t *restrict i_face_cog
    = (const cs_real_3_t *restrict)fvq->i_face_cog;
  const cs_real_3_t *restrict dijpf
    = (const cs_real_3_t *restrict)fvq->dijpf;
  const cs_real_3_t *restrict diipb
    = (const cs_real_3_t *restrict)fvq->diipb;

  const int *bc_type = cs_glob_bc_type;

  /* Local variables */

  char var_name[32];

  cs_gnum_t n_upwind;
  int iupwin;
  int tr_dim = 0;
  int w_stride = 1;

  bool recompute_cocg = (iccocg) ? true : false;

  cs_real_3_t *grad;
  cs_real_3_t *gradup = NULL;
  cs_real_3_t *gradst = NULL;
  cs_field_t *f;

  cs_field_t *f_limiter = NULL;
  cs_real_t *limiter = NULL;

  cs_real_t *gweight = NULL;

  cs_real_t  *v_slope_test = _get_v_slope_test(f_id,  var_cal_opt);

  /*==========================================================================*/

  /* 1. Initialization */

  /* Allocate work arrays */
  BFT_MALLOC(grad, n_cells_ext, cs_real_3_t);

  /* Choose gradient type */

  cs_halo_type_t halo_type = CS_HALO_STANDARD;
  cs_gradient_type_t gradient_type = CS_GRADIENT_ITER;

  cs_gradient_type_by_imrgra(imrgra,
                             &gradient_type,
                             &halo_type);

  if (f_id != -1) {
    f = cs_field_by_id(f_id);
    /* Get option from the field */
    if (isstpp >= 3) {
      const int key_limiter = cs_field_key_id("limiter_choice");
      limiter_choice = cs_field_get_key_int(f, key_limiter);
    }

    int f_limiter_id = cs_field_get_key_int(f, cs_field_key_id("convection_limiter_id"));
    if (f_limiter_id > -1) {
      f_limiter = cs_field_by_id(f_limiter_id);
      limiter = f_limiter->val;
    }

    snprintf(var_name, 31, "%s", f->name); var_name[31] = '\0';
  }
  else
    strcpy(var_name, "Work array"); var_name[31] = '\0';

  if (iwarnp >= 2) {
    if (ischcp == 1) {
      bft_printf
        (
         _(" %s: Convection in centered blending with %f percent of upwind\n"),
         var_name, (1.-blencp)*100.);
    } else {
      bft_printf
        (
         _(" %s: Convection in 2nd order blending with %f percent of upwind\n"),
         var_name, (1.-blencp)*100.);
    }
  }

  iupwin = (blencp > 0.) ? 0 : 1;

  /* 2. Compute the balance with reconstruction technics */

  /* Compute the gradient of the variable

     The gradient (grad) is used in the flux reconstruction and the slope test.
     Thus we must compute it:
         - when we have diffusion and we reconstruct the fluxes,
         - when the convection scheme is SOLU,
         - when we have convection, we are not in pure upwind
           and we reconstruct the fluxes,
         - when we have convection, we are not in pure upwind
           and we have not shunted the slope test. */

  if (   (idiffp != 0 && ircflp == 1)
      || (   iconvp != 0 && iupwin == 0
          && (ischcp == 0 || ircflp == 1 || isstpp == 0))) {

    if (f_id != -1) {
      /* Get the calculation option from the field */
      if (f->type & CS_FIELD_VARIABLE && var_cal_opt.iwgrec == 1) {
        if (var_cal_opt.idiff > 0) {
          int key_id = cs_field_key_id("gradient_weighting_id");
          int diff_id = cs_field_get_key_int(f, key_id);
          if (diff_id > -1) {
            cs_field_t *weight_f = cs_field_by_id(diff_id);
            gweight = weight_f->val;
            w_stride = weight_f->dim;
          }
        }
      }
    }

    cs_gradient_scalar(var_name,
                       gradient_type,
                       halo_type,
                       inc,
                       recompute_cocg,
                       nswrgp,
                       tr_dim,
                       0, /* hyd_p_flag */
                       w_stride,
                       iwarnp,
                       imligp,
                       epsrgp,
                       extrap,
                       climgp,
                       NULL, /* f_ext exterior force */
                       coefap,
                       coefbp,
                       pvar,
                       gweight, /* Weighted gradient */
                       grad);

  } else {
#   pragma omp parallel for
    for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++) {
      grad[cell_id][0] = 0.;
      grad[cell_id][1] = 0.;
      grad[cell_id][2] = 0.;
    }
  }

  /* ======================================================================
     ---> Compute uncentered gradient gradpa for the slope test
     ======================================================================*/

  /* 2.1 Compute the gradient for convective scheme (the slope test, limiter, SOLU, etc) */

  /* Slope test gradient */
  if (iconvp > 0 && iupwin == 0 && isstpp == 0) {

    BFT_MALLOC(gradst, n_cells_ext, cs_real_3_t);

# pragma omp parallel for
    for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++) {
      gradst[cell_id][0] = 0.;
      gradst[cell_id][1] = 0.;
      gradst[cell_id][2] = 0.;
    }

    cs_slope_test_gradient(f_id,
                           inc,
                           halo_type,
                           grad,
                           gradst,
                           pvar,
                           coefap,
                           coefbp,
                           i_massflux);

  }

  /* Pure SOLU scheme without using gradient_slope_test function
     or Roe and Sweby limiters */
  if (iconvp > 0 && iupwin == 0 && (ischcp == 2 || isstpp == 3)) {

    BFT_MALLOC(gradup, n_cells_ext, cs_real_3_t);

# pragma omp parallel for
    for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++) {
      gradup[cell_id][0] = 0.;
      gradup[cell_id][1] = 0.;
      gradup[cell_id][2] = 0.;
    }

    cs_upwind_gradient(f_id,
                       inc,
                       halo_type,
                       coefap,
                       coefbp,
                       i_massflux,
                       b_massflux,
                       pvar,
                       gradup);

  }

  /* ======================================================================
     ---> Contribution from interior faces
     ======================================================================*/

  n_upwind = 0;

  if (n_cells_ext > n_cells) {
#   pragma omp parallel for if(n_cells_ext - n_cells > CS_THR_MIN)
    for (cs_lnum_t cell_id = n_cells+0; cell_id < n_cells_ext; cell_id++)
      rhs[cell_id] = 0.;
  }

  /* --> Pure upwind flux
     =====================*/

  if (iupwin == 1) {

    /* Steady */
    if (idtvar < 0) {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for reduction(+:n_upwind)
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];
            /* in parallel, face will be counted by one and only one rank */
            if (ii < n_cells) {
              n_upwind = n_upwind+1;
            }

            cs_real_2_t fluxij = {0.,0.};

            cs_real_t pifri, pjfri, pifrj, pjfrj;
            cs_real_t pip, pjp, pipr, pjpr;

            cs_i_cd_steady_upwind(ircflp,
                                  relaxp,
                                  weight[face_id],
                                  cell_cen[ii],
                                  cell_cen[jj],
                                  i_face_cog[face_id],
                                  dijpf[face_id],
                                  grad[ii],
                                  grad[jj],
                                  pvar[ii],
                                  pvar[jj],
                                  pvara[ii],
                                  pvara[jj],
                                  &pifri,
                                  &pifrj,
                                  &pjfri,
                                  &pjfrj,
                                  &pip,
                                  &pjp,
                                  &pipr,
                                  &pjpr);

            cs_i_conv_flux(iconvp,
                           1.,
                           1,
                           pvar[ii],
                           pvar[jj],
                           pifri,
                           pifrj,
                           pjfri,
                           pjfrj,
                           i_massflux[face_id],
                           xcpp[ii],
                           xcpp[jj],
                           fluxij);

            cs_i_diff_flux(idiffp,
                           1.,
                           pip,
                           pjp,
                           pipr,
                           pjpr,
                           i_visc[face_id],
                           fluxij);

            rhs[ii] -= fluxij[0];
            rhs[jj] += fluxij[1];

          }
        }
      }

      /* Unsteady */
    } else {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for reduction(+:n_upwind)
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];
            /* in parallel, face will be counted by one and only one rank */
            if (ii < n_cells) {
              n_upwind = n_upwind+1;
            }

            cs_real_2_t fluxij = {0.,0.};

            cs_real_t pif, pjf;
            cs_real_t pip, pjp;

            cs_i_cd_unsteady_upwind(ircflp,
                                    weight[face_id],
                                    cell_cen[ii],
                                    cell_cen[jj],
                                    i_face_cog[face_id],
                                    dijpf[face_id],
                                    grad[ii],
                                    grad[jj],
                                    pvar[ii],
                                    pvar[jj],
                                    &pif,
                                    &pjf,
                                    &pip,
                                    &pjp);

            cs_i_conv_flux(iconvp,
                           thetap,
                           imasac,
                           pvar[ii],
                           pvar[jj],
                           pif,
                           pif, /* no relaxation */
                           pjf,
                           pjf, /* no relaxation */
                           i_massflux[face_id],
                           xcpp[ii],
                           xcpp[jj],
                           fluxij);

            cs_i_diff_flux(idiffp,
                           thetap,
                           pip,
                           pjp,
                           pip,/* no relaxation */
                           pjp,/* no relaxation */
                           i_visc[face_id],
                           fluxij);

            rhs[ii] -= fluxij[0];
            rhs[jj] += fluxij[1];

          }
        }
      }

    }

  /* --> Flux with no slope test or Min/Max Beta limiter
    ====================================================*/

  } else if (isstpp == 1 || isstpp == 2) {

    if (ischcp < 0 || ischcp > 2) {
      bft_error(__FILE__, __LINE__, 0,
                _("invalid value of ischcv"));
    }

    /* Steady */
    if (idtvar < 0) {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            cs_real_2_t fluxij = {0.,0.};

            cs_real_t pifri, pjfri, pifrj, pjfrj;
            cs_real_t pip, pjp, pipr, pjpr;

            cs_i_cd_steady(ircflp,
                           ischcp,
                           relaxp,
                           blencp,
                           weight[face_id],
                           cell_cen[ii],
                           cell_cen[jj],
                           i_face_cog[face_id],
                           dijpf[face_id],
                           grad[ii],
                           grad[jj],
                           gradup[ii],
                           gradup[jj],
                           pvar[ii],
                           pvar[jj],
                           pvara[ii],
                           pvara[jj],
                           &pifri,
                           &pifrj,
                           &pjfri,
                           &pjfrj,
                           &pip,
                           &pjp,
                           &pipr,
                           &pjpr);

            cs_i_conv_flux(iconvp,
                           1.,
                           1,
                           pvar[ii],
                           pvar[jj],
                           pifri,
                           pifrj,
                           pjfri,
                           pjfrj,
                           i_massflux[face_id],
                           xcpp[ii],
                           xcpp[jj],
                           fluxij);

            cs_i_diff_flux(idiffp,
                           1.,
                           pip,
                           pjp,
                           pipr,
                           pjpr,
                           i_visc[face_id],
                           fluxij);

            rhs[ii] -= fluxij[0];
            rhs[jj] += fluxij[1];

          }
        }
      }

      /* Unsteady */
    } else {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            cs_real_t beta = blencp;

            cs_real_t pif, pjf;
            cs_real_t pip, pjp;

            /*Beta blending coefficient assuring the positivity of the scalar*/
            if (isstpp == 2) {
              beta = CS_MAX(CS_MIN(limiter[ii], limiter[jj]), 0.);
            }

            cs_real_2_t fluxij = {0.,0.};

            cs_i_cd_unsteady(ircflp,
                             ischcp,
                             beta,
                             weight[face_id],
                             cell_cen[ii],
                             cell_cen[jj],
                             i_face_cog[face_id],
                             dijpf[face_id],
                             grad[ii],
                             grad[jj],
                             gradup[ii],
                             gradup[jj],
                             pvar[ii],
                             pvar[jj],
                             &pif,
                             &pjf,
                             &pip,
                             &pjp);

            cs_i_conv_flux(iconvp,
                           thetap,
                           imasac,
                           pvar[ii],
                           pvar[jj],
                           pif,
                           pif, /* no relaxation */
                           pjf,
                           pjf, /* no relaxation */
                           i_massflux[face_id],
                           xcpp[ii],
                           xcpp[jj],
                           fluxij);

            cs_i_diff_flux(idiffp,
                           thetap,
                           pip,
                           pjp,
                           pip, /* no relaxation */
                           pjp, /* no relaxation */
                           i_visc[face_id],
                           fluxij);

            rhs[ii] -= fluxij[0];
            rhs[jj] += fluxij[1];

          }
        }
      }

    }

  /* --> Flux with slope test or Roe and Sweby limiter
    ==================================================*/

  } else {

    if (ischcp < 0 || ischcp > 2) {
      bft_error(__FILE__, __LINE__, 0,
                _("invalid value of ischcv"));
    }
    if (isstpp != 0 && isstpp != 3) {
      bft_error(__FILE__, __LINE__, 0,
                _("invalid value of isstpc"));
    }

    /* Steady */
    if (idtvar < 0) {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for reduction(+:n_upwind)
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            cs_real_2_t fluxij = {0.,0.};

            cs_real_t pifri, pjfri, pifrj, pjfrj;
            cs_real_t pip, pjp, pipr, pjpr;

            bool upwind_switch;

            cs_i_cd_steady_slope_test(&upwind_switch,
                                      iconvp,
                                      ircflp,
                                      ischcp,
                                      relaxp,
                                      blencp,
                                      weight[face_id],
                                      i_dist[face_id],
                                      i_face_surf[face_id],
                                      cell_cen[ii],
                                      cell_cen[jj],
                                      i_face_normal[face_id],
                                      i_face_cog[face_id],
                                      dijpf[face_id],
                                      i_massflux[face_id],
                                      grad[ii],
                                      grad[jj],
                                      gradup[ii],
                                      gradup[jj],
                                      gradst[ii],
                                      gradst[jj],
                                      pvar[ii],
                                      pvar[jj],
                                      pvara[ii],
                                      pvara[jj],
                                      &pifri,
                                      &pifrj,
                                      &pjfri,
                                      &pjfrj,
                                      &pip,
                                      &pjp,
                                      &pipr,
                                      &pjpr);

            cs_i_conv_flux(iconvp,
                           1.,
                           1,
                           pvar[ii],
                           pvar[jj],
                           pifri,
                           pifrj,
                           pjfri,
                           pjfrj,
                           i_massflux[face_id],
                           xcpp[ii],
                           xcpp[jj],
                           fluxij);

            cs_i_diff_flux(idiffp,
                           1.,
                           pip,
                           pjp,
                           pipr,
                           pjpr,
                           i_visc[face_id],
                           fluxij);

            if (upwind_switch) {

              /* in parallel, face will be counted by one and only one rank */
              if (ii < n_cells)
                n_upwind++;
              if (v_slope_test != NULL) {
                v_slope_test[ii] += fabs(i_massflux[face_id]) / cell_vol[ii];
                v_slope_test[jj] += fabs(i_massflux[face_id]) / cell_vol[jj];
              }

            }

            rhs[ii] -= fluxij[0];
            rhs[jj] += fluxij[1];

          }
        }
      }

      /* Unsteady */
    } else {

      for (int g_id = 0; g_id < n_i_groups; g_id++) {
#       pragma omp parallel for reduction(+:n_upwind)
        for (int t_id = 0; t_id < n_i_threads; t_id++) {
          for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
               face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
               face_id++) {

            cs_lnum_t ii = i_face_cells[face_id][0];
            cs_lnum_t jj = i_face_cells[face_id][1];

            cs_real_2_t fluxij = {0.,0.};

            bool upwind_switch = false;

            cs_real_t pif, pjf;
            cs_real_t pip, pjp;

            /* Original slope test */
            if (isstpp == 0) {

              cs_i_cd_unsteady_slope_test(&upwind_switch,
                                          iconvp,
                                          ircflp,
                                          ischcp,
                                          blencp,
                                          weight[face_id],
                                          i_dist[face_id],
                                          i_face_surf[face_id],
                                          cell_cen[ii],
                                          cell_cen[jj],
                                          i_face_normal[face_id],
                                          i_face_cog[face_id],
                                          dijpf[face_id],
                                          i_massflux[face_id],
                                          grad[ii],
                                          grad[jj],
                                          gradup[ii],
                                          gradup[jj],
                                          gradst[ii],
                                          gradst[jj],
                                          pvar[ii],
                                          pvar[jj],
                                          &pif,
                                          &pjf,
                                          &pip,
                                          &pjp);

              cs_i_conv_flux(iconvp,
                             thetap,
                             imasac,
                             pvar[ii],
                             pvar[jj],
                             pif,
                             pif, /* no relaxation */
                             pjf,
                             pjf, /* no relaxation */
                             i_massflux[face_id],
                             xcpp[ii],
                             xcpp[jj],
                             fluxij);

            }
            /* Roe- Sweby limier */
            else { /* if (isstpp == 3) */

              cs_real_t rij;

              int cur;
              cs_real_t p_u, p_c, p_d;

              cur = ii;
              // Current    point value
              p_c = pvar[ii];
              // Downstream point value
              p_d = pvar[jj];

              if (i_massflux[face_id] < 0.) {
                cur = jj;
                p_c = pvar[jj];
                p_d = pvar[ii];
              }

              /* Compute the upstream point value */
              p_u = cs_upstream_val(p_c,
                                    cell_vol[cur],
                                    i_face_surf[face_id],
                                    i_face_normal[face_id],
                                    gradup[cur]);

              /* If non monotonicity is detected at the downstream side
                 the scheme switches to a first order upwind scheme */
              if ((p_c-p_u)*(p_d-p_c) <= 0.) {
                 rij = 0.;
              }
              /* There is monotonicity at the downstream side */
              else {
                if (CS_ABS(p_d-p_c) < cs_math_epzero*(CS_ABS(p_u)+CS_ABS(p_c)+CS_ABS(p_d))) {
                  rij = cs_math_big_r;
                }
                /* consecutive downstream slopes rate */
                else {
                  rij = CS_MIN(CS_ABS((p_c-p_u)/(p_d-p_c)), cs_math_big_r);
                }
              }

              cs_real_t phi = cs_limiter_function(limiter_choice, rij);
              /* If stored for post-processing */
              if (limiter != NULL)
                limiter[face_id] = phi;

              /* Compute the limited convective flux based on
                 SOLU or centered scheme */

              cs_i_cd_unsteady_limiter(ircflp,
                                       ischcp,
                                       weight[face_id],
                                       cell_cen[ii],
                                       cell_cen[jj],
                                       i_face_cog[face_id],
                                       phi,
                                       dijpf[face_id],
                                       grad[ii],
                                       grad[jj],
                                       gradup[ii],
                                       gradup[jj],
                                       pvar[ii],
                                       pvar[jj],
                                       &pif,
                                       &pjf,
                                       &pip,
                                       &pjp);

              cs_i_conv_flux(iconvp,
                             thetap,
                             imasac,
                             pvar[ii],
                             pvar[jj],
                             pif,
                             pif, /* no relaxation */
                             pjf,
                             pjf, /* no relaxation */
                             i_massflux[face_id],
                             xcpp[ii],
                             xcpp[jj],
                             fluxij);

            }
            cs_i_diff_flux(idiffp,
                           thetap,
                           pip,
                           pjp,
                           pip, /* no relaxation */
                           pjp, /* no relaxation */
                           i_visc[face_id],
                           fluxij);

            if (upwind_switch) {

              /* in parallel, face will be counted by one and only one rank */
              if (ii < n_cells)
                n_upwind++;
              if (v_slope_test != NULL) {
                v_slope_test[ii] += fabs(i_massflux[face_id]) / cell_vol[ii];
                v_slope_test[jj] += fabs(i_massflux[face_id]) / cell_vol[jj];
              }

            }

            rhs[ii] -= fluxij[0];
            rhs[jj] += fluxij[1];

          }
        }
      }

    } /* idtvar */

  } /* iupwin */


  if (iwarnp >= 2) {

    /* Sum number of clippings */
    cs_parall_counter(&n_upwind, 1);

    bft_printf(_(" %s: %llu Faces with upwind on %llu interior faces \n"),
               var_name, (unsigned long long)n_upwind,
               (unsigned long long)m->n_g_i_faces);
  }

  /* ======================================================================
     ---> Contribution from boundary faces
     ======================================================================*/

  /* Steady */
  if (idtvar < 0) {

    for (int g_id = 0; g_id < n_b_groups; g_id++) {
#     pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
      for (int t_id = 0; t_id < n_b_threads; t_id++) {
        for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
             face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = b_face_cells[face_id];

          cs_real_t fluxi = 0.;
          cs_real_t pir, pipr;

          cs_b_cd_steady(ircflp,
                         relaxp,
                         diipb[face_id],
                         grad[ii],
                         pvar[ii],
                         pvara[ii],
                         &pir,
                         &pipr);

          cs_b_upwind_flux(iconvp,
                           1.,
                           1,
                           inc,
                           bc_type[face_id],
                           pvar[ii],
                           pir,
                           pipr,
                           coefap[face_id],
                           coefbp[face_id],
                           b_massflux[face_id],
                           xcpp[ii],
                           &fluxi);

          cs_b_diff_flux(idiffp,
                         1., /* thetap */
                         inc,
                         pipr,
                         cofafp[face_id],
                         cofbfp[face_id],
                         b_visc[face_id],
                         &fluxi);

          rhs[ii] -= fluxi;

        }
      }
    }

    /* Unsteady */
  } else {

    for (int g_id = 0; g_id < n_b_groups; g_id++) {
#     pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
      for (int t_id = 0; t_id < n_b_threads; t_id++) {
        for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
             face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = b_face_cells[face_id];

          cs_real_t fluxi = 0.;
          cs_real_t pip;

          cs_b_cd_unsteady(ircflp,
                           diipb[face_id],
                           grad[ii],
                           pvar[ii],
                           &pip);

          cs_b_upwind_flux(iconvp,
                           thetap,
                           imasac,
                           inc,
                           bc_type[face_id],
                           pvar[ii],
                           pvar[ii], /* no relaxation */
                           pip,
                           coefap[face_id],
                           coefbp[face_id],
                           b_massflux[face_id],
                           xcpp[ii],
                           &fluxi);

          cs_b_diff_flux(idiffp,
                         thetap,
                         inc,
                         pip,
                         cofafp[face_id],
                         cofbfp[face_id],
                         b_visc[face_id],
                         &fluxi);

          rhs[ii] -= fluxi;

        }
      }
    }

  }

  /* Free memory */
  BFT_FREE(grad);
  grad = NULL;
  if (gradup!=NULL) {
    BFT_FREE(gradup);
    gradup = NULL;
  }
  if (gradst!=NULL) {
    BFT_FREE(gradst);
    gradst = NULL;
  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Add the explicit part of the diffusion terms with a symmetric tensor
 * diffusivity for a transport equation of a scalar field \f$ \varia \f$.
 *
 * More precisely, the right hand side \f$ Rhs \f$ is updated as
 * follows:
 * \f[
 * Rhs = Rhs - \sum_{\fij \in \Facei{\celli}}      \left(
 *      - \tens{\mu}_\fij \gradv_\fij \varia \cdot \vect{S}_\ij  \right)
 * \f]
 *
 * Warning:
 * - \f$ Rhs \f$ has already been initialized before
 *   calling cs_anisotropic_diffusion_scalar!
 * - mind the sign minus
 *
 * \param[in]     idtvar        indicator of the temporal scheme
 * \param[in]     f_id          index of the current variable
 * \param[in]     var_cal_opt   variable calculation options
 * \param[in]     inc           indicator
 *                               - 0 when solving an increment
 *                               - 1 otherwise
 * \param[in]     iccocg        indicator
 *                               - 1 re-compute cocg matrix
                                (for iterativ gradients)
 *                               - 0 otherwise
 * \param[in]     pvar          solved variable (current time step)
 * \param[in]     pvara         solved variable (previous time step)
 * \param[in]     coefap        boundary condition array for the variable
 *                               (explicit part)
 * \param[in]     coefbp        boundary condition array for the variable
 *                               (implicit part)
 * \param[in]     cofafp        boundary condition array for the diffusion
 *                               of the variable (explicit part)
 * \param[in]     cofbfp        boundary condition array for the diffusion
 *                               of the variable (implicit part)
 * \param[in]     i_visc        \f$ \mu_\fij \dfrac{S_\fij}{\ipf \jpf} \f$
 *                               at interior faces for the r.h.s.
 * \param[in]     b_visc        \f$ \mu_\fib \dfrac{S_\fib}{\ipf \centf} \f$
 *                               at border faces for the r.h.s.
 * \param[in]     viscel        symmetric cell tensor \f$ \tens{\mu}_\celli \f$
 * \param[in]     weighf        internal face weight between cells i j in case
 *                               of tensor diffusion
 * \param[in]     weighb        boundary face weight for cells i in case
 *                               of tensor diffusion
 * \param[in,out] rhs           right hand side \f$ \vect{Rhs} \f$
 */
/*----------------------------------------------------------------------------*/

void
cs_anisotropic_diffusion_scalar(int                       idtvar,
                                int                       f_id,
                                const cs_var_cal_opt_t    var_cal_opt,
                                int                       inc,
                                int                       iccocg,
                                cs_real_t       *restrict pvar,
                                const cs_real_t *restrict pvara,
                                const cs_real_t           coefap[],
                                const cs_real_t           coefbp[],
                                const cs_real_t           cofafp[],
                                const cs_real_t           cofbfp[],
                                const cs_real_t           i_visc[],
                                const cs_real_t           b_visc[],
                                cs_real_6_t     *restrict viscel,
                                const cs_real_2_t         weighf[],
                                const cs_real_t           weighb[],
                                cs_real_t       *restrict rhs)
{
  const int nswrgp = var_cal_opt.nswrgr;
  const int imrgra = var_cal_opt.imrgra;
  const int imligp = var_cal_opt.imligr;
  const int ircflp = var_cal_opt.ircflu;
  const int iwarnp = var_cal_opt.iwarni;
  const double epsrgp = var_cal_opt.epsrgr;
  const double climgp = var_cal_opt.climgr;
  const double extrap = var_cal_opt.extrag;
  const double relaxp = var_cal_opt.relaxv;
  const double thetap = var_cal_opt.thetav;

  const cs_mesh_t  *m = cs_glob_mesh;
  const cs_halo_t  *halo = m->halo;
  cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  const cs_lnum_t n_cells = m->n_cells;
  const cs_lnum_t n_cells_ext = m->n_cells_with_ghosts;
  const int n_i_groups = m->i_face_numbering->n_groups;
  const int n_i_threads = m->i_face_numbering->n_threads;
  const int n_b_groups = m->b_face_numbering->n_groups;
  const int n_b_threads = m->b_face_numbering->n_threads;
  const cs_lnum_t *restrict i_group_index = m->i_face_numbering->group_index;
  const cs_lnum_t *restrict b_group_index = m->b_face_numbering->group_index;

  const cs_lnum_2_t *restrict i_face_cells
    = (const cs_lnum_2_t *restrict)m->i_face_cells;
  const cs_lnum_t *restrict b_face_cells
    = (const cs_lnum_t *restrict)m->b_face_cells;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)fvq->cell_cen;
  const cs_real_3_t *restrict i_face_normal
    = (const cs_real_3_t *restrict)fvq->i_face_normal;
  const cs_real_3_t *restrict b_face_normal
    = (const cs_real_3_t *restrict)fvq->b_face_normal;
  const cs_real_3_t *restrict i_face_cog
    = (const cs_real_3_t *restrict)fvq->i_face_cog;
  const cs_real_3_t *restrict b_face_cog
    = (const cs_real_3_t *restrict)fvq->b_face_cog;

  /* Local variables */

  char var_name[32];

  int n_upwind;
  int tr_dim = 0;
  int w_stride = 1;

  bool recompute_cocg = (iccocg) ? true : false;

  cs_real_6_t *viscce;
  cs_real_6_t *w2;
  cs_real_3_t *grad;

  cs_field_t *f;

  cs_real_t *gweight = NULL;

  /* 1. Initialization */

  viscce = NULL;
  w2 = NULL;

  /* Allocate work arrays */
  BFT_MALLOC(grad, n_cells_ext, cs_real_3_t);

  /* Choose gradient type */
  cs_halo_type_t halo_type = CS_HALO_STANDARD;
  cs_gradient_type_t gradient_type = CS_GRADIENT_ITER;

  cs_gradient_type_by_imrgra(imrgra,
                             &gradient_type,
                             &halo_type);

  if (f_id != -1) {
    f = cs_field_by_id(f_id);
    snprintf(var_name, 31, "%s", f->name); var_name[31] = '\0';
  }
  else
    strcpy(var_name, "Work array"); var_name[31] = '\0';

  /* Porosity fields */
  cs_field_t *fporo = cs_field_by_name_try("porosity");
  cs_field_t *ftporo = cs_field_by_name_try("tensorial_porosity");

  cs_real_t *porosi = NULL;
  cs_real_6_t *porosf = NULL;

  if (cs_glob_porous_model == 1 || cs_glob_porous_model == 2) {
    porosi = fporo->val;
    if (ftporo != NULL) {
      porosf = (cs_real_6_t *)ftporo->val;
    }
  }

  /* Without porosity */
  if (porosi == NULL) {
    viscce = viscel;

    /* With porosity */
  } else if (porosi != NULL && porosf == NULL) {
    BFT_MALLOC(w2, n_cells_ext, cs_real_6_t);
    for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
      for (int isou = 0; isou < 6; isou++) {
        w2[cell_id][isou] = porosi[cell_id]*viscel[cell_id][isou];
      }
    }
    viscce = w2;

    /* With tensorial porosity */
  } else if (porosi != NULL && porosf != NULL) {
    BFT_MALLOC(w2, n_cells_ext, cs_real_6_t);
    for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
      cs_math_sym_33_product(porosf[cell_id],
                             viscel[cell_id],
                             w2[cell_id]);
    }
    viscce = w2;
  }

  /* ---> Periodicity and parallelism treatment of symmetric tensors */
  if (halo != NULL) {
    cs_halo_sync_var_strided(halo, halo_type, (cs_real_t *)viscce, 6);
    if (m->n_init_perio > 0)
      cs_halo_perio_sync_var_sym_tens(halo, halo_type, (cs_real_t *)viscce);
  }

  /* 2. Compute the diffusive part with reconstruction technics */

  /* ======================================================================
     ---> Compute the gradient of the current variable if needed
     ======================================================================*/

  if (ircflp == 1) {

    if (f_id != -1) {
      /* Get the calculation option from the field */
      if (f->type & CS_FIELD_VARIABLE && var_cal_opt.iwgrec == 1) {
        if (var_cal_opt.idifft > 0) {
          int key_id = cs_field_key_id("gradient_weighting_id");
          int diff_id = cs_field_get_key_int(f, key_id);
          if (diff_id > -1) {
            cs_field_t *weight_f = cs_field_by_id(diff_id);
            gweight = weight_f->val;
            w_stride = weight_f->dim;
          }
        }
      }
    }

    cs_gradient_scalar(var_name,
                       gradient_type,
                       halo_type,
                       inc,
                       recompute_cocg,
                       nswrgp,
                       tr_dim,
                       0, /* hyd_p_flag */
                       w_stride,
                       iwarnp,
                       imligp,
                       epsrgp,
                       extrap,
                       climgp,
                       NULL, /* f_ext exterior force */
                       coefap,
                       coefbp,
                       pvar,
                       gweight, /* Weighted gradient */
                       grad);

  } else {
#   pragma omp parallel for
    for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++) {
      grad[cell_id][0] = 0.;
      grad[cell_id][1] = 0.;
      grad[cell_id][2] = 0.;
    }
  }

  /* ======================================================================
     ---> Contribution from interior faces
     ======================================================================*/

  n_upwind = 0;

  if (n_cells_ext > n_cells) {
#   pragma omp parallel for if(n_cells_ext - n_cells > CS_THR_MIN)
    for (cs_lnum_t cell_id = n_cells; cell_id < n_cells_ext; cell_id++) {
      rhs[cell_id] = 0.;
    }
  }

  /* Steady */
  if (idtvar < 0) {

    for (int g_id = 0; g_id < n_i_groups; g_id++) {
#     pragma omp parallel for reduction(+:n_upwind)
      for (int t_id = 0; t_id < n_i_threads; t_id++) {
        for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
             face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = i_face_cells[face_id][0];
          cs_lnum_t jj = i_face_cells[face_id][1];
          /* in parallel, face will be counted by one and only one rank */
          if (ii < n_cells) {
            n_upwind++;
          }

          cs_real_t pi = pvar[ii];
          cs_real_t pj = pvar[jj];
          cs_real_t pia = pvara[ii];
          cs_real_t pja = pvara[jj];

          /* Recompute II" and JJ"
             ----------------------*/

          cs_real_t visci[3][3], viscj[3][3];

          visci[0][0] = viscce[ii][0];
          visci[1][1] = viscce[ii][1];
          visci[2][2] = viscce[ii][2];
          visci[1][0] = viscce[ii][3];
          visci[0][1] = viscce[ii][3];
          visci[2][1] = viscce[ii][4];
          visci[1][2] = viscce[ii][4];
          visci[2][0] = viscce[ii][5];
          visci[0][2] = viscce[ii][5];

          /* IF.Ki.S / ||Ki.S||^2 */
          cs_real_t fikdvi = weighf[face_id][0];

          cs_real_t diippf[3], djjppf[3];

          /* II" = IF + FI" */
          for (int i = 0; i < 3; i++) {
            diippf[i] = i_face_cog[face_id][i]-cell_cen[ii][i]
                      - fikdvi*( visci[0][i]*i_face_normal[face_id][0]
                               + visci[1][i]*i_face_normal[face_id][1]
                               + visci[2][i]*i_face_normal[face_id][2] );
          }

          viscj[0][0] = viscce[jj][0];
          viscj[1][1] = viscce[jj][1];
          viscj[2][2] = viscce[jj][2];
          viscj[1][0] = viscce[jj][3];
          viscj[0][1] = viscce[jj][3];
          viscj[2][1] = viscce[jj][4];
          viscj[1][2] = viscce[jj][4];
          viscj[2][0] = viscce[jj][5];
          viscj[0][2] = viscce[jj][5];

          /* FJ.Kj.S / ||Kj.S||^2 */
          cs_real_t fjkdvi = weighf[face_id][1];

          /* JJ" = JF + FJ" */
          for (int i = 0; i < 3; i++) {
            djjppf[i] = i_face_cog[face_id][i]-cell_cen[jj][i]
                      + fjkdvi*( viscj[0][i]*i_face_normal[face_id][0]
                               + viscj[1][i]*i_face_normal[face_id][1]
                               + viscj[2][i]*i_face_normal[face_id][2] );
          }

          /* p in I" and J" */
          cs_real_t pipp = pi + ircflp*(  grad[ii][0]*diippf[0]
                                        + grad[ii][1]*diippf[1]
                                        + grad[ii][2]*diippf[2]);
          cs_real_t pjpp = pj + ircflp*(  grad[jj][0]*djjppf[0]
                                        + grad[jj][1]*djjppf[1]
                                        + grad[jj][2]*djjppf[2]);

          cs_real_t pir = pi/relaxp - (1.-relaxp)/relaxp * pia;
          cs_real_t pjr = pj/relaxp - (1.-relaxp)/relaxp * pja;

          /* pr in I" and J" */
          cs_real_t pippr = pir + ircflp*(  grad[ii][0]*diippf[0]
                                          + grad[ii][1]*diippf[1]
                                          + grad[ii][2]*diippf[2]);
          cs_real_t pjppr = pjr + ircflp*(  grad[jj][0]*djjppf[0]
                                          + grad[jj][1]*djjppf[1]
                                          + grad[jj][2]*djjppf[2]);


          cs_real_t fluxi = i_visc[face_id]*(pippr - pjpp);
          cs_real_t fluxj = i_visc[face_id]*(pipp - pjppr);

          rhs[ii] -= fluxi;
          rhs[jj] += fluxj;

        }
      }
    }

    /* Unsteady */
  } else {

    for (int g_id = 0; g_id < n_i_groups; g_id++) {
#     pragma omp parallel for reduction(+:n_upwind)
      for (int t_id = 0; t_id < n_i_threads; t_id++) {
        for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
             face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = i_face_cells[face_id][0];
          cs_lnum_t jj = i_face_cells[face_id][1];
          /* in parallel, face will be counted by one and only one rank */
          if (ii < n_cells) {
            n_upwind++;
          }

          cs_real_t pi = pvar[ii];
          cs_real_t pj = pvar[jj];

          /* Recompute II" and JJ"
             ----------------------*/

          cs_real_t visci[3][3], viscj[3][3];

          visci[0][0] = viscce[ii][0];
          visci[1][1] = viscce[ii][1];
          visci[2][2] = viscce[ii][2];
          visci[1][0] = viscce[ii][3];
          visci[0][1] = viscce[ii][3];
          visci[2][1] = viscce[ii][4];
          visci[1][2] = viscce[ii][4];
          visci[2][0] = viscce[ii][5];
          visci[0][2] = viscce[ii][5];

          /* IF.Ki.S / ||Ki.S||^2 */
          cs_real_t fikdvi = weighf[face_id][0];

          cs_real_t diippf[3], djjppf[3];

          /* II" = IF + FI" */
          for (int i = 0; i < 3; i++) {
            diippf[i] = i_face_cog[face_id][i]-cell_cen[ii][i]
                      - fikdvi*( visci[0][i]*i_face_normal[face_id][0]
                               + visci[1][i]*i_face_normal[face_id][1]
                               + visci[2][i]*i_face_normal[face_id][2] );
          }

          viscj[0][0] = viscce[jj][0];
          viscj[1][1] = viscce[jj][1];
          viscj[2][2] = viscce[jj][2];
          viscj[1][0] = viscce[jj][3];
          viscj[0][1] = viscce[jj][3];
          viscj[2][1] = viscce[jj][4];
          viscj[1][2] = viscce[jj][4];
          viscj[2][0] = viscce[jj][5];
          viscj[0][2] = viscce[jj][5];

          /* FJ.Kj.S / ||Kj.S||^2 */
          cs_real_t fjkdvi = weighf[face_id][1];

          /* JJ" = JF + FJ" */
          for (int i = 0; i < 3; i++) {
            djjppf[i] = i_face_cog[face_id][i]-cell_cen[jj][i]
                      + fjkdvi*( viscj[0][i]*i_face_normal[face_id][0]
                               + viscj[1][i]*i_face_normal[face_id][1]
                               + viscj[2][i]*i_face_normal[face_id][2] );
          }

          /* p in I" and J" */
          cs_real_t pipp = pi + ircflp*(  grad[ii][0]*diippf[0]
                                        + grad[ii][1]*diippf[1]
                                        + grad[ii][2]*diippf[2]);
          cs_real_t pjpp = pj + ircflp*(  grad[jj][0]*djjppf[0]
                                        + grad[jj][1]*djjppf[1]
                                        + grad[jj][2]*djjppf[2]);

          cs_real_t flux = i_visc[face_id]*(pipp -pjpp);

          rhs[ii] -= thetap*flux;
          rhs[jj] += thetap*flux;

        }
      }
    }
  }

  /* ======================================================================
     ---> Contribution from boundary faces
     ======================================================================*/

  /* Steady */
  if (idtvar < 0) {

    for (int g_id = 0; g_id < n_b_groups; g_id++) {
#     pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
      for (int t_id = 0; t_id < n_b_threads; t_id++) {
        for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
             face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = b_face_cells[face_id];

          cs_real_t pi = pvar[ii];
          cs_real_t pia = pvara[ii];

          cs_real_t pir = pi/relaxp - (1.-relaxp)/relaxp*pia;

          /* Recompute II"
             --------------*/

          cs_real_t visci[3][3];

          visci[0][0] = viscce[ii][0];
          visci[1][1] = viscce[ii][1];
          visci[2][2] = viscce[ii][2];
          visci[1][0] = viscce[ii][3];
          visci[0][1] = viscce[ii][3];
          visci[2][1] = viscce[ii][4];
          visci[1][2] = viscce[ii][4];
          visci[2][0] = viscce[ii][5];
          visci[0][2] = viscce[ii][5];

          /* IF.Ki.S / ||Ki.S||^2 */
          cs_real_t fikdvi = weighb[face_id];

          cs_real_t diippf[3];

          /* II" = IF + FI" */
          for (int i = 0; i < 3; i++) {
            diippf[i] = b_face_cog[face_id][i] - cell_cen[ii][i]
                      - fikdvi*( visci[0][i]*b_face_normal[face_id][0]
                               + visci[1][i]*b_face_normal[face_id][1]
                               + visci[2][i]*b_face_normal[face_id][2] );
          }

          cs_real_t pippr = pir + ircflp*(  grad[ii][0]*diippf[0]
                                          + grad[ii][1]*diippf[1]
                                          + grad[ii][2]*diippf[2]);

          cs_real_t pfacd = inc*cofafp[face_id] + cofbfp[face_id]*pippr;

          cs_real_t flux = b_visc[face_id]*pfacd;
          rhs[ii] -= flux;

        }
      }
    }

    /* Unsteady */
  } else {

    for (int g_id = 0; g_id < n_b_groups; g_id++) {
#     pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
      for (int t_id = 0; t_id < n_b_threads; t_id++) {
        for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
             face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = b_face_cells[face_id];

          cs_real_t pi = pvar[ii];

          /* Recompute II"
             --------------*/

          cs_real_t visci[3][3];

          visci[0][0] = viscce[ii][0];
          visci[1][1] = viscce[ii][1];
          visci[2][2] = viscce[ii][2];
          visci[1][0] = viscce[ii][3];
          visci[0][1] = viscce[ii][3];
          visci[2][1] = viscce[ii][4];
          visci[1][2] = viscce[ii][4];
          visci[2][0] = viscce[ii][5];
          visci[0][2] = viscce[ii][5];

          /* IF.Ki.S / ||Ki.S||^2 */
          cs_real_t fikdvi = weighb[face_id];

          cs_real_t diippf[3];

          /* II" = IF + FI" */
          for (int i = 0; i < 3; i++) {
            diippf[i] = b_face_cog[face_id][i] - cell_cen[ii][i]
                      - fikdvi*( visci[0][i]*b_face_normal[face_id][0]
                               + visci[1][i]*b_face_normal[face_id][1]
                               + visci[2][i]*b_face_normal[face_id][2]);
          }

          cs_real_t pipp = pi + ircflp*(  grad[ii][0]*diippf[0]
                                        + grad[ii][1]*diippf[1]
                                        + grad[ii][2]*diippf[2]);

          cs_real_t pfacd = inc*cofafp[face_id] + cofbfp[face_id]*pipp;

          cs_real_t flux = b_visc[face_id]*pfacd;
          rhs[ii] -= thetap*flux;

        }
      }
    }

  }

  /* Free memory */
  BFT_FREE(grad);
  BFT_FREE(w2);
}

/*-----------------------------------------------------------------------------*/
/*!
 * \brief Add the explicit part of the diffusion terms with a symmetric tensorial
 * diffusivity for a transport equation of a vector field \f$ \vect{\varia} \f$.
 *
 * More precisely, the right hand side \f$ \vect{Rhs} \f$ is updated as
 * follows:
 * \f[
 * \vect{Rhs} = \vect{Rhs} - \sum_{\fij \in \Facei{\celli}}      \left(
 *      - \tens{\mu}_\fij \gradt_\fij \vect{\varia} \cdot \vect{S}_\ij  \right)
 * \f]
 *
 * Warning:
 * - \f$ \vect{Rhs} \f$ has already been initialized before calling diftnv!
 * - mind the sign minus
 *
 * \param[in]     idtvar        indicator of the temporal scheme
 * \param[in]     f_id          index of the current variable
 * \param[in]     var_cal_opt   variable calculation options
 * \param[in]     inc           indicator
 *                               - 0 when solving an increment
 *                               - 1 otherwise
 * \param[in]     ivisep        indicator to take \f$ \divv
 *                               \left(\mu \gradt \transpose{\vect{a}} \right)
 *                               -2/3 \grad\left( \mu \dive \vect{a} \right)\f$
 *                               - 1 take into account,
 * \param[in]     pvar          solved variable (current time step)
 * \param[in]     pvara         solved variable (previous time step)
 * \param[in]     coefav        boundary condition array for the variable
 *                               (explicit part)
 * \param[in]     coefbv        boundary condition array for the variable
 *                               (implicit part)
 * \param[in]     cofafv        boundary condition array for the diffusion
 *                               of the variable (explicit part)
 * \param[in]     cofbfv        boundary condition array for the diffusion
 *                               of the variable (implicit part)
 * \param[in]     i_visc        \f$ \tens{\mu}_\fij \dfrac{S_\fij}{\ipf\jpf} \f$
 *                               at interior faces for the r.h.s.
 * \param[in]     b_visc        \f$ \dfrac{S_\fib}{\ipf \centf} \f$
 *                               at border faces for the r.h.s.
 * \param[in]     secvif        secondary viscosity at interior faces
 * \param[in,out] rhs           right hand side \f$ \vect{Rhs} \f$
 */
/*----------------------------------------------------------------------------*/

void
cs_anisotropic_diffusion_vector(int                         idtvar,
                                int                         f_id,
                                const cs_var_cal_opt_t      var_cal_opt,
                                int                         inc,
                                int                         ivisep,
                                cs_real_3_t       *restrict pvar,
                                const cs_real_3_t *restrict pvara,
                                const cs_real_3_t           coefav[],
                                const cs_real_33_t          coefbv[],
                                const cs_real_3_t           cofafv[],
                                const cs_real_33_t          cofbfv[],
                                const cs_real_33_t          i_visc[],
                                const cs_real_t             b_visc[],
                                const cs_real_t             secvif[],
                                cs_real_3_t       *restrict rhs)
{
  const int nswrgp = var_cal_opt.nswrgr;
  const int imrgra = var_cal_opt.imrgra;
  const int imligp = var_cal_opt.imligr;
  const int ircflp = var_cal_opt.ircflu;
  const int iwarnp = var_cal_opt.iwarni;
  const double epsrgp = var_cal_opt.epsrgr;
  const double climgp = var_cal_opt.climgr;
  const double relaxp = var_cal_opt.relaxv;
  const double thetap = var_cal_opt.thetav;

  const cs_mesh_t  *m = cs_glob_mesh;
  const cs_halo_t  *halo = m->halo;
  cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  const cs_lnum_t n_cells = m->n_cells;
  const cs_lnum_t n_cells_ext = m->n_cells_with_ghosts;
  const int n_i_groups = m->i_face_numbering->n_groups;
  const int n_i_threads = m->i_face_numbering->n_threads;
  const int n_b_groups = m->b_face_numbering->n_groups;
  const int n_b_threads = m->b_face_numbering->n_threads;
  const cs_lnum_t *restrict i_group_index = m->i_face_numbering->group_index;
  const cs_lnum_t *restrict b_group_index = m->b_face_numbering->group_index;

  const cs_lnum_2_t *restrict i_face_cells
    = (const cs_lnum_2_t *restrict)m->i_face_cells;
  const cs_lnum_t *restrict b_face_cells
    = (const cs_lnum_t *restrict)m->b_face_cells;
  const cs_real_t *restrict weight = fvq->weight;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)fvq->cell_cen;
  const cs_real_3_t *restrict i_f_face_normal
    = (const cs_real_3_t *restrict)fvq->i_f_face_normal;
  const cs_real_3_t *restrict i_face_cog
    = (const cs_real_3_t *restrict)fvq->i_face_cog;
  const cs_real_3_t *restrict dijpf
    = (const cs_real_3_t *restrict)fvq->dijpf;
  const cs_real_3_t *restrict diipb
    = (const cs_real_3_t *restrict)fvq->diipb;

  const int *bc_type = cs_glob_bc_type;

  /* Local variables */

  char var_name[32];

  cs_gnum_t n_upwind;

  cs_real_33_t *gradv;
  cs_real_t *bndcel;

  cs_field_t *f;

  /* 1. Initialization */

  /* Allocate work arrays */
  BFT_MALLOC(gradv, n_cells_ext, cs_real_33_t);

  /* Choose gradient type */

  cs_halo_type_t halo_type = CS_HALO_STANDARD;
  cs_gradient_type_t gradient_type = CS_GRADIENT_ITER;

  cs_gradient_type_by_imrgra(imrgra,
                             &gradient_type,
                             &halo_type);

  if (f_id != -1) {
    f = cs_field_by_id(f_id);
    snprintf(var_name, 31, "%s", f->name); var_name[31] = '\0';
  }
  else
    strcpy(var_name, "Work array"); var_name[31] = '\0';

  /* 2. Compute the diffusive part with reconstruction technics */

  /* Compute the gradient of the current variable if needed */

  if (ircflp == 1 || ivisep == 1) {

    cs_gradient_vector(var_name,
                       gradient_type,
                       halo_type,
                       inc,
                       nswrgp,
                       iwarnp,
                       imligp,
                       epsrgp,
                       climgp,
                       coefav,
                       coefbv,
                       pvar,
                       gradv);

  } else {
#   pragma omp parallel for
    for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++) {
      for (int isou = 0; isou < 3; isou++) {
        for (int jsou = 0; jsou < 3; jsou++)
          gradv[cell_id][isou][jsou] = 0.;
      }
    }
  }

  /* ======================================================================
     ---> Contribution from interior faces
     ======================================================================*/

  n_upwind = 0;

  if (n_cells_ext > n_cells) {
#   pragma omp parallel for if(n_cells_ext -n_cells > CS_THR_MIN)
    for (cs_lnum_t cell_id = n_cells; cell_id < n_cells_ext; cell_id++) {
      for (int isou = 0; isou < 3; isou++) {
        rhs[cell_id][isou] = 0.;
      }
    }
  }

  /* Steady */
  if (idtvar < 0) {

    for (int g_id = 0; g_id < n_i_groups; g_id++) {
#     pragma omp parallel for reduction(+:n_upwind)
      for (int t_id = 0; t_id < n_i_threads; t_id++) {
        for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
             face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = i_face_cells[face_id][0];
          cs_lnum_t jj = i_face_cells[face_id][1];

          /* in parallel, face will be counted by one and only one rank */
          if (ii < n_cells) {
            n_upwind++;
          }

          cs_real_t dijpfv[3], diipfv[3], djjpfv[3];

          for (int jsou = 0; jsou < 3; jsou++) {
            dijpfv[jsou] = dijpf[face_id][jsou];
          }

          cs_real_t pnd = weight[face_id];

          /* Recompute II' and JJ' at this level */
          for (int jsou = 0; jsou < 3; jsou++) {
            diipfv[jsou] =   i_face_cog[face_id][jsou]
                           - (cell_cen[ii][jsou] + (1.-pnd) * dijpfv[jsou]);
            djjpfv[jsou] =   i_face_cog[face_id][jsou]
                           - cell_cen[jj][jsou]  + pnd  * dijpfv[jsou];
          }

          cs_real_t pip[3], pjp[3], pipr[3], pjpr[3];

          /*-----------------
            X-Y-Z components, p = u, v, w */
          for (int isou = 0; isou < 3; isou++) {

            cs_real_t dpvf[3];

            for (int jsou = 0; jsou < 3; jsou++) {
              dpvf[jsou] = 0.5*(gradv[ii][isou][jsou] + gradv[jj][isou][jsou]);
            }

            cs_real_t pi  = pvar [ii][isou];
            cs_real_t pj  = pvar [jj][isou];

            cs_real_t pia = pvara[ii][isou];
            cs_real_t pja = pvara[jj][isou];

            /* reconstruction only if IRCFLP = 1 */
            pip[isou] = pi + ircflp*(  dpvf[0]*diipfv[0]
                                     + dpvf[1]*diipfv[1]
                                     + dpvf[2]*diipfv[2]);
            pjp[isou] = pj + ircflp*(  dpvf[0]*djjpfv[0]
                                     + dpvf[1]*djjpfv[1]
                                     + dpvf[2]*djjpfv[2]);

            pipr[isou] = pi /relaxp - (1.-relaxp)/relaxp * pia
                         + ircflp*(  dpvf[0]*diipfv[0]
                                   + dpvf[1]*diipfv[1]
                                   + dpvf[2]*diipfv[2]);
            pjpr[isou] = pj /relaxp - (1.-relaxp)/relaxp * pja
                         + ircflp*(  dpvf[0]*djjpfv[0]
                                   + dpvf[1]*djjpfv[1]
                                   + dpvf[2]*djjpfv[2]);

          }

          for (int isou = 0; isou < 3; isou++) {

            cs_real_t fluxi =   i_visc[face_id][0][isou]*(pipr[0] - pjp[0])
                              + i_visc[face_id][1][isou]*(pipr[1] - pjp[1])
                              + i_visc[face_id][2][isou]*(pipr[2] - pjp[2]);
            cs_real_t fluxj =   i_visc[face_id][0][isou]*(pip[0] - pjpr[0])
                              + i_visc[face_id][1][isou]*(pip[1] - pjpr[1])
                              + i_visc[face_id][2][isou]*(pip[2] - pjpr[2]);

            rhs[ii][isou] = rhs[ii][isou] - fluxi;
            rhs[jj][isou] = rhs[jj][isou] + fluxj;

          }

        }
      }
    }

    /* Unsteady */
  } else {

    for (int g_id = 0; g_id < n_i_groups; g_id++) {
#     pragma omp parallel for reduction(+:n_upwind)
      for (int t_id = 0; t_id < n_i_threads; t_id++) {
        for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
             face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = i_face_cells[face_id][0];
          cs_lnum_t jj = i_face_cells[face_id][1];

          /* in parallel, face will be counted by one and only one rank */
          if (ii < n_cells) {
            n_upwind++;
          }

          cs_real_t dijpfv[3], diipfv[3], djjpfv[3];

          for (int jsou = 0; jsou < 3; jsou++) {
            dijpfv[jsou] = dijpf[face_id][jsou];
          }

          cs_real_t pnd = weight[face_id];

          /* Recompute II' and JJ' at this level */
          for (int jsou = 0; jsou < 3; jsou++) {
            diipfv[jsou] =   i_face_cog[face_id][jsou]
                           - (cell_cen[ii][jsou] + (1.-pnd) * dijpfv[jsou]);
            djjpfv[jsou] =   i_face_cog[face_id][jsou]
                           - cell_cen[jj][jsou]  +  pnd * dijpfv[jsou];
          }

          cs_real_t pip[3], pjp[3];

          /*-----------------
            X-Y-Z components, p = u, v, w */
          for (int isou = 0; isou < 3; isou++) {

            cs_real_t dpvf[3];

            for (int jsou = 0; jsou < 3; jsou++) {
              dpvf[jsou] = 0.5*(gradv[ii][isou][jsou] + gradv[jj][isou][jsou]);
            }

            cs_real_t pi = pvar[ii][isou];
            cs_real_t pj = pvar[jj][isou];

            pip[isou] = pi + ircflp*(  dpvf[0]*diipfv[0]
                                     + dpvf[1]*diipfv[1]
                                     + dpvf[2]*diipfv[2]);
            pjp[isou] = pj + ircflp*(  dpvf[0]*djjpfv[0]
                                     + dpvf[1]*djjpfv[1]
                                     + dpvf[2]*djjpfv[2]);

          }

          for (int isou = 0; isou < 3; isou++) {

            cs_real_t flux =   i_visc[face_id][0][isou]*(pip[0] - pjp[0])
                             + i_visc[face_id][1][isou]*(pip[1] - pjp[1])
                             + i_visc[face_id][2][isou]*(pip[2] - pjp[2]);

            rhs[ii][isou] = rhs[ii][isou] - thetap*flux;
            rhs[jj][isou] = rhs[jj][isou] + thetap*flux;

          }

        }
      }
    }

  }

  /* ======================================================================
     ---> Contribution from boundary faces
     ======================================================================*/

  /* Steady */
  if (idtvar < 0) {

    for (int g_id = 0; g_id < n_b_groups; g_id++) {
#     pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
      for (int t_id = 0; t_id < n_b_threads; t_id++) {
        for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
             face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = b_face_cells[face_id];

          cs_real_t diipbv[3];

          for (int jsou = 0; jsou < 3; jsou++)
            diipbv[jsou] = diipb[face_id][jsou];

          /*-----------------
            X-Y-Z components, p = u, v, w */
          for (int isou = 0; isou < 3; isou++) {

            cs_real_t pfacd = inc*cofafv[face_id][isou];

            cs_real_t pipr[3];

            /*coefu and cofuf are matrices */
            for (int jsou = 0; jsou < 3; jsou++) {
              cs_real_t pir  =   pvar[ii][jsou]/relaxp
                               - (1.-relaxp)/relaxp*pvara[ii][jsou];

              pipr[jsou] = pir +ircflp*(  gradv[ii][jsou][0]*diipbv[0]
                                        + gradv[ii][jsou][1]*diipbv[1]
                                        + gradv[ii][jsou][2]*diipbv[2]);
              pfacd += cofbfv[face_id][jsou][isou]*pipr[jsou];
            }

            cs_real_t flux = b_visc[face_id]*pfacd;
            rhs[ii][isou] = rhs[ii][isou] - flux;

          } /* isou */

        }
      }
    }

    /* Unsteady */
  } else {

    for (int g_id = 0; g_id < n_b_groups; g_id++) {
#     pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
      for (int t_id = 0; t_id < n_b_threads; t_id++) {
        for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
             face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = b_face_cells[face_id];

          cs_real_t diipbv[3];

          for (int jsou = 0; jsou < 3; jsou++)
            diipbv[jsou] = diipb[face_id][jsou];

          /*-----------------
            X-Y-Z components, p = u, v, w */
          for (int isou = 0; isou < 3; isou++) {

            cs_real_t pfacd = inc*cofafv[face_id][isou];

            /*coefu and cofuf are matrices */
            for (int jsou = 0; jsou < 3; jsou++) {
              cs_real_t pir =   pvar[ii][jsou]
                              + ircflp*(  gradv[ii][jsou][0]*diipbv[0]
                                        + gradv[ii][jsou][1]*diipbv[1]
                                        + gradv[ii][jsou][2]*diipbv[2]);
              pfacd += cofbfv[face_id][jsou][isou]*pir;
            }

            cs_real_t flux = b_visc[face_id]*pfacd;
            rhs[ii][isou] = rhs[ii][isou] - thetap * flux;

          } /* isou */

        }
      }
    }

  } /* idtvar */

  /* 3. Computation of the transpose grad(vel) term and grad(-2/3 div(vel)) */

  if (ivisep == 1) {

    /* We do not know what condition to put in the inlets and the outlets, so we
       assume that there is an equilibrium. Moreover, cells containing a coupled
       are removed. */

    /* Allocate a temporary array */
    BFT_MALLOC(bndcel, n_cells_ext, cs_real_t);

#   pragma omp parallel for
    for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++) {
      bndcel[cell_id] = 1.;
    }

#   pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
    for (cs_lnum_t face_id = 0; face_id < m->n_b_faces; face_id++) {
      int ityp = bc_type[face_id];
      if (   ityp == CS_OUTLET
          || ityp == CS_INLET
          || ityp == CS_CONVECTIVE_INLET
          || ityp == CS_COUPLED_FD) {
        bndcel[b_face_cells[face_id]] = 0.;
      }
    }

    if (halo != NULL)
      cs_halo_sync_var(halo, halo_type, bndcel);

    /* ---> Interior faces */

    for (int g_id = 0; g_id < n_i_groups; g_id++) {
#     pragma omp parallel for
      for (int t_id = 0; t_id < n_i_threads; t_id++) {
        for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
             face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = i_face_cells[face_id][0];
          cs_lnum_t jj = i_face_cells[face_id][1];

          cs_real_t pnd = weight[face_id];
          cs_real_t secvis = secvif[face_id];

          cs_real_t grdtrv
            =        pnd*(gradv[ii][0][0]+gradv[ii][1][1]+gradv[ii][2][2])
              + (1.-pnd)*(gradv[jj][0][0]+gradv[jj][1][1]+gradv[jj][2][2]);

          for (int i = 0; i < 3; i++) {

            cs_real_t flux = secvis*grdtrv*i_f_face_normal[ face_id][i];

            /* We need to compute (K grad(u)^T) .IJ
               which is equal to IJ . (grad(u) . K^T)
               But: (IJ . (grad(u) . K^T))_i = IJ_k grad(u)_kj K_ij
               Warning, in FORTRAN K_ij = K(j, i) */

            for (int j = 0; j < 3; j++) {
              for (int k = 0; k < 3; k++) {
                flux = flux + dijpf[face_id][k]
                            *(pnd*gradv[ii][k][j]+(1-pnd)*gradv[jj][k][j])
                            *i_visc[face_id][i][j];
              }
            }

            rhs[ii][i] = rhs[ii][i] + flux*bndcel[ii];
            rhs[jj][i] = rhs[jj][i] - flux*bndcel[jj];

          }

        }
      }
    }

    /* ---> Boundary FACES
       the whole flux term of the stress tensor is already taken into account
       (so, no corresponding term in forbr)
       TODO in theory we should take the normal component into account (the
       tangential one is modeled by the wall law) */

    /*Free memory */
    BFT_FREE(bndcel);

  }

  /* Free memory */
  BFT_FREE(gradv);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Add the explicit part of the diffusion terms with a symmetric tensor
 * diffusivity for a transport equation of a scalar field \f$ \varia \f$.
 *
 * More precisely, the right hand side \f$ Rhs \f$ is updated as
 * follows:
 * \f[
 * Rhs = Rhs - \sum_{\fij \in \Facei{\celli}}      \left(
 *      - \tens{\mu}_\fij \gradv_\fij \varia \cdot \vect{S}_\ij  \right)
 * \f]
 *
 * Warning:
 * - \f$ Rhs \f$ has already been initialized before
 *   calling cs_anisotropic_diffusion_scalar!
 * - mind the sign minus
 *
 * \param[in]     idtvar        indicator of the temporal scheme
 * \param[in]     f_id          index of the current variable
 * \param[in]     var_cal_opt   variable calculation options
 * \param[in]     inc           indicator
 *                               - 0 when solving an increment
 *                               - 1 otherwise
 * \param[in]     pvar          solved variable (current time step)
 * \param[in]     pvara         solved variable (previous time step)
 * \param[in]     coefa         boundary condition array for the variable
 *                               (explicit part)
 * \param[in]     coefb         boundary condition array for the variable
 *                               (implicit part)
 * \param[in]     cofaf         boundary condition array for the diffusion
 *                               of the variable (explicit part)
 * \param[in]     cofbf         boundary condition array for the diffusion
 *                               of the variable (implicit part)
 * \param[in]     i_visc        \f$ \mu_\fij \dfrac{S_\fij}{\ipf \jpf} \f$
 *                               at interior faces for the r.h.s.
 * \param[in]     b_visc        \f$ \mu_\fib \dfrac{S_\fib}{\ipf \centf} \f$
 *                               at border faces for the r.h.s.
 * \param[in]     viscel        symmetric cell tensor \f$ \tens{\mu}_\celli \f$
 * \param[in]     weighf        internal face weight between cells i j in case
 *                               of tensor diffusion
 * \param[in]     weighb        boundary face weight for cells i in case
 *                               of tensor diffusion
 * \param[in,out] rhs           right hand side \f$ \vect{Rhs} \f$
 */
/*----------------------------------------------------------------------------*/

void
cs_anisotropic_diffusion_tensor(int                         idtvar,
                                int                         f_id,
                                const cs_var_cal_opt_t      var_cal_opt,
                                int                         inc,
                                cs_real_6_t       *restrict pvar,
                                const cs_real_6_t *restrict pvara,
                                const cs_real_6_t           coefa[],
                                const cs_real_66_t          coefb[],
                                const cs_real_6_t           cofaf[],
                                const cs_real_66_t          cofbf[],
                                const cs_real_t             i_visc[],
                                const cs_real_t             b_visc[],
                                cs_real_6_t     *restrict   viscel,
                                const cs_real_2_t           weighf[],
                                const cs_real_t             weighb[],
                                cs_real_6_t     *restrict   rhs)
{
  const int nswrgp = var_cal_opt.nswrgr;
  const int imrgra = var_cal_opt.imrgra;
  const int imligp = var_cal_opt.imligr;
  const int ircflp = var_cal_opt.ircflu;
  const int iwarnp = var_cal_opt.iwarni;
  const double epsrgp = var_cal_opt.epsrgr;
  const double climgp = var_cal_opt.climgr;
  const double relaxp = var_cal_opt.relaxv;
  const double thetap = var_cal_opt.thetav;

  const cs_mesh_t  *m = cs_glob_mesh;
  const cs_halo_t  *halo = m->halo;
  cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  const cs_lnum_t n_cells = m->n_cells;
  const cs_lnum_t n_cells_ext = m->n_cells_with_ghosts;
  const int n_i_groups = m->i_face_numbering->n_groups;
  const int n_i_threads = m->i_face_numbering->n_threads;
  const int n_b_groups = m->b_face_numbering->n_groups;
  const int n_b_threads = m->b_face_numbering->n_threads;
  const cs_lnum_t *restrict i_group_index = m->i_face_numbering->group_index;
  const cs_lnum_t *restrict b_group_index = m->b_face_numbering->group_index;

  const cs_lnum_2_t *restrict i_face_cells
    = (const cs_lnum_2_t *restrict)m->i_face_cells;
  const cs_lnum_t *restrict b_face_cells
    = (const cs_lnum_t *restrict)m->b_face_cells;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)fvq->cell_cen;
  const cs_real_3_t *restrict i_face_normal
    = (const cs_real_3_t *restrict)fvq->i_face_normal;
  const cs_real_3_t *restrict b_face_normal
    = (const cs_real_3_t *restrict)fvq->b_face_normal;
  const cs_real_3_t *restrict i_face_cog
    = (const cs_real_3_t *restrict)fvq->i_face_cog;
  const cs_real_3_t *restrict b_face_cog
    = (const cs_real_3_t *restrict)fvq->b_face_cog;

  /* Local variables */

  char var_name[32];

  int  n_upwind;

  cs_real_6_t *viscce;
  cs_real_6_t *w2;
  cs_real_63_t *grad;

  cs_field_t *f;

  /* 1. Initialization */

  viscce = NULL;
  w2 = NULL;

  /* Allocate work arrays */
  BFT_MALLOC(grad, n_cells_ext, cs_real_63_t);

  /* Choose gradient type */
  cs_halo_type_t halo_type = CS_HALO_STANDARD;
  cs_gradient_type_t gradient_type = CS_GRADIENT_ITER;

  cs_gradient_type_by_imrgra(imrgra,
                             &gradient_type,
                             &halo_type);

  if (f_id != -1) {
    f = cs_field_by_id(f_id);
    snprintf(var_name, 31, "%s", f->name); var_name[31] = '\0';
  }
  else
    strcpy(var_name, "Work array"); var_name[31] = '\0';

  /* Porosity fields */
  cs_field_t *fporo = cs_field_by_name_try("porosity");
  cs_field_t *ftporo = cs_field_by_name_try("tensorial_porosity");

  cs_real_t *porosi = NULL;
  cs_real_6_t *porosf = NULL;

  if (cs_glob_porous_model == 1 || cs_glob_porous_model == 2) {
    porosi = fporo->val;
    if (ftporo != NULL) {
      porosf = (cs_real_6_t *)ftporo->val;
    }
  }

  /* Without porosity */
  if (porosi == NULL) {
    viscce = viscel;

    /* With porosity */
  } else if (porosi != NULL && porosf == NULL) {
    BFT_MALLOC(w2, n_cells_ext, cs_real_6_t);
    for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
      for (int isou = 0; isou < 6; isou++) {
        w2[cell_id][isou] = porosi[cell_id]*viscel[cell_id][isou];
      }
    }
    viscce = w2;

    /* With tensorial porosity */
  } else if (porosi != NULL && porosf != NULL) {
    BFT_MALLOC(w2, n_cells_ext, cs_real_6_t);
    for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
      cs_math_sym_33_product(porosf[cell_id],
                             viscel[cell_id],
                             w2[cell_id]);
    }
    viscce = w2;
  }

  /* ---> Periodicity and parallelism treatment of symmetric tensors */
  if (halo != NULL) {
    cs_halo_sync_var_strided(halo, halo_type, (cs_real_t *)viscce, 6);
    if (m->n_init_perio > 0)
      cs_halo_perio_sync_var_sym_tens(halo, halo_type, (cs_real_t *)viscce);
  }

  /* 2. Compute the diffusive part with reconstruction technics */

  /* ======================================================================
     ---> Compute the gradient of the current variable if needed
     ======================================================================*/

  if (ircflp == 1) {

    cs_gradient_tensor(var_name,
                       gradient_type,
                       halo_type,
                       inc,
                       nswrgp,
                       iwarnp,
                       imligp,
                       epsrgp,
                       climgp,
                       coefa,
                       coefb,
                       pvar,
                       grad);

  } else {
#   pragma omp parallel for
    for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++) {
      for (int isou = 0; isou < 6; isou++) {
        for (int jsou = 0; jsou < 3; jsou++)
          grad[cell_id][isou][jsou] = 0.;
      }
    }
  }

  /* ======================================================================
     ---> Contribution from interior faces
     ======================================================================*/

  n_upwind = 0;

  if (n_cells_ext > n_cells) {
#   pragma omp parallel for if(n_cells_ext -n_cells > CS_THR_MIN)
    for (cs_lnum_t cell_id = n_cells; cell_id < n_cells_ext; cell_id++) {
      for (int isou = 0; isou < 6; isou++) {
        rhs[cell_id][isou] = 0.;
      }
    }
  }

  /* Steady */
  if (idtvar < 0) {

    for (int g_id = 0; g_id < n_i_groups; g_id++) {
#     pragma omp parallel for reduction(+:n_upwind)
      for (int t_id = 0; t_id < n_i_threads; t_id++) {
        for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
             face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = i_face_cells[face_id][0];
          cs_lnum_t jj = i_face_cells[face_id][1];

          /* in parallel, face will be counted by one and only one rank */
          if (ii < n_cells) {
            n_upwind++;
          }

          cs_real_t visci[3][3], viscj[3][3];
          cs_real_t diippf[3], djjppf[3], pipp[6], pjpp[6];
          cs_real_t  pir[6], pjr[6], pippr[6], pjppr[6];
          cs_real_t  pi[6], pj[6], pia[6], pja[6];

          for (int isou = 0; isou < 6; isou++) {
            pi[isou] = pvar[ii][isou];
            pj[isou] = pvar[jj][isou];
            pia[isou] = pvara[ii][isou];
            pja[isou] = pvara[jj][isou];
          }

          /* Recompute II" and JJ"
             ----------------------*/

          visci[0][0] = viscce[ii][0];
          visci[1][1] = viscce[ii][1];
          visci[2][2] = viscce[ii][2];
          visci[1][0] = viscce[ii][3];
          visci[0][1] = viscce[ii][3];
          visci[2][1] = viscce[ii][4];
          visci[1][2] = viscce[ii][4];
          visci[2][0] = viscce[ii][5];
          visci[0][2] = viscce[ii][5];

          /* IF.Ki.S / ||Ki.S||^2 */
          cs_real_t fikdvi = weighf[face_id][0];

          /* II" = IF + FI" */
          for (int i = 0; i < 3; i++) {
            diippf[i] = i_face_cog[face_id][i]-cell_cen[ii][i]
                      - fikdvi*( visci[0][i]*i_face_normal[face_id][0]
                               + visci[1][i]*i_face_normal[face_id][1]
                               + visci[2][i]*i_face_normal[face_id][2] );
          }

          viscj[0][0] = viscce[jj][0];
          viscj[1][1] = viscce[jj][1];
          viscj[2][2] = viscce[jj][2];
          viscj[1][0] = viscce[jj][3];
          viscj[0][1] = viscce[jj][3];
          viscj[2][1] = viscce[jj][4];
          viscj[1][2] = viscce[jj][4];
          viscj[2][0] = viscce[jj][5];
          viscj[0][2] = viscce[jj][5];

          /* FJ.Kj.S / ||Kj.S||^2 */
          cs_real_t fjkdvi = weighf[face_id][1];

          /* JJ" = JF + FJ" */
          for (int i = 0; i < 3; i++) {
            djjppf[i] = i_face_cog[face_id][i]-cell_cen[jj][i]
                      + fjkdvi*( viscj[0][i]*i_face_normal[face_id][0]
                               + viscj[1][i]*i_face_normal[face_id][1]
                               + viscj[2][i]*i_face_normal[face_id][2] );
          }

          for (int isou = 0; isou < 6; isou++) {
            /* p in I" and J" */
            pipp[isou] = pi[isou] + ircflp*( grad[ii][isou][0]*diippf[0]
                                           + grad[ii][isou][1]*diippf[1]
                                           + grad[ii][isou][2]*diippf[2]);
            pjpp[isou] = pj[isou] + ircflp*( grad[jj][isou][0]*djjppf[0]
                                           + grad[jj][isou][1]*djjppf[1]
                                           + grad[jj][isou][2]*djjppf[2]);

            pir[isou] = pi[isou]/relaxp - (1.-relaxp)/relaxp * pia[isou];
            pjr[isou] = pj[isou]/relaxp - (1.-relaxp)/relaxp * pja[isou];


            /* pr in I" and J" */
            pippr[isou] = pir[isou] + ircflp*( grad[ii][isou][0]*diippf[0]
                                             + grad[ii][isou][1]*diippf[1]
                                             + grad[ii][isou][2]*diippf[2]);
            pjppr[isou] = pjr[isou] + ircflp*( grad[jj][isou][0]*djjppf[0]
                                             + grad[jj][isou][1]*djjppf[1]
                                             + grad[jj][isou][2]*djjppf[2]);


            cs_real_t fluxi = i_visc[face_id]*(pippr[isou] - pjpp[isou]);
            cs_real_t fluxj = i_visc[face_id]*(pipp[isou] - pjppr[isou]);

            rhs[ii][isou] -= fluxi;
            rhs[jj][isou] += fluxj;
          } // loop on isou
        }
      }
    }

    /* Unsteady */
  } else {

    for (int g_id = 0; g_id < n_i_groups; g_id++) {
#     pragma omp parallel for reduction(+:n_upwind)
      for (int t_id = 0; t_id < n_i_threads; t_id++) {
        for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
             face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = i_face_cells[face_id][0];
          cs_lnum_t jj = i_face_cells[face_id][1];
          /* in parallel, face will be counted by one and only one rank */
          if (ii < n_cells) {
            n_upwind++;
          }

          cs_real_t visci[3][3], viscj[3][3];
          cs_real_t diippf[3], djjppf[3], pipp[6], pjpp[6];
          cs_real_t pi[6], pj[6];

          for (int isou = 0; isou < 6; isou++) {
            pi[isou] = pvar[ii][isou];
            pj[isou] = pvar[jj][isou];
          }

          /* Recompute II" and JJ"
             ----------------------*/

          visci[0][0] = viscce[ii][0];
          visci[1][1] = viscce[ii][1];
          visci[2][2] = viscce[ii][2];
          visci[1][0] = viscce[ii][3];
          visci[0][1] = viscce[ii][3];
          visci[2][1] = viscce[ii][4];
          visci[1][2] = viscce[ii][4];
          visci[2][0] = viscce[ii][5];
          visci[0][2] = viscce[ii][5];

          /* IF.Ki.S / ||Ki.S||^2 */
          cs_real_t fikdvi = weighf[face_id][0];

          /* II" = IF + FI" */
          for (int i = 0; i < 3; i++) {
            diippf[i] = i_face_cog[face_id][i]-cell_cen[ii][i]
                      - fikdvi*( visci[0][i]*i_face_normal[face_id][0]
                               + visci[1][i]*i_face_normal[face_id][1]
                               + visci[2][i]*i_face_normal[face_id][2] );
          }

          viscj[0][0] = viscce[jj][0];
          viscj[1][1] = viscce[jj][1];
          viscj[2][2] = viscce[jj][2];
          viscj[1][0] = viscce[jj][3];
          viscj[0][1] = viscce[jj][3];
          viscj[2][1] = viscce[jj][4];
          viscj[1][2] = viscce[jj][4];
          viscj[2][0] = viscce[jj][5];
          viscj[0][2] = viscce[jj][5];

          /* FJ.Kj.S / ||Kj.S||^2 */
          cs_real_t fjkdvi = weighf[face_id][1];

          /* JJ" = JF + FJ" */
          for (int i = 0; i < 3; i++) {
            djjppf[i] = i_face_cog[face_id][i]-cell_cen[jj][i]
                      + fjkdvi*( viscj[0][i]*i_face_normal[face_id][0]
                               + viscj[1][i]*i_face_normal[face_id][1]
                               + viscj[2][i]*i_face_normal[face_id][2] );
          }

          for (int isou = 0; isou < 6; isou++) {
            /* p in I" and J" */
            pipp[isou] = pi[isou] + ircflp*( grad[ii][isou][0]*diippf[0]
                                           + grad[ii][isou][1]*diippf[1]
                                           + grad[ii][isou][2]*diippf[2]);
            pjpp[isou] = pj[isou] + ircflp*( grad[jj][isou][0]*djjppf[0]
                                           + grad[jj][isou][1]*djjppf[1]
                                           + grad[jj][isou][2]*djjppf[2]);

            cs_real_t flux = i_visc[face_id]*(pipp[isou] -pjpp[isou]);

            rhs[ii][isou] -= thetap*flux;
            rhs[jj][isou] += thetap*flux;
          }
        }
      }
    }
  }

  /* ======================================================================
     ---> Contribution from boundary faces
     ======================================================================*/

  /* Steady */
  if (idtvar < 0) {

    for (int g_id = 0; g_id < n_b_groups; g_id++) {
#     pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
      for (int t_id = 0; t_id < n_b_threads; t_id++) {
        for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
             face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = b_face_cells[face_id];

          cs_real_t pi[6], pia[6], pir[6], pippr[6];
          cs_real_t visci[3][3];
          cs_real_t diippf[3];

          for (int isou = 0; isou < 6; isou++) {
            pi[isou] = pvar[ii][isou];
            pia[isou] = pvara[ii][isou];
            pir[isou] = pi[isou]/relaxp - (1.-relaxp)/relaxp*pia[isou];
          }

          /* Recompute II"
             --------------*/

          visci[0][0] = viscce[ii][0];
          visci[1][1] = viscce[ii][1];
          visci[2][2] = viscce[ii][2];
          visci[1][0] = viscce[ii][3];
          visci[0][1] = viscce[ii][3];
          visci[2][1] = viscce[ii][4];
          visci[1][2] = viscce[ii][4];
          visci[2][0] = viscce[ii][5];
          visci[0][2] = viscce[ii][5];

          /* IF.Ki.S / ||Ki.S||^2 */
          cs_real_t fikdvi = weighb[face_id];

          /* II" = IF + FI" */
          for (int i = 0; i < 3; i++) {
            diippf[i] = b_face_cog[face_id][i] - cell_cen[ii][i]
                      - fikdvi*( visci[0][i]*b_face_normal[face_id][0]
                               + visci[1][i]*b_face_normal[face_id][1]
                               + visci[2][i]*b_face_normal[face_id][2] );
          }
          for (int isou = 0; isou < 6; isou++) {
            pippr[isou] = pir[isou] + ircflp*( grad[ii][isou][0]*diippf[0]
                                             + grad[ii][isou][1]*diippf[1]
                                             + grad[ii][isou][2]*diippf[2]);
          }
          for (int isou = 0; isou < 6; isou++) {
            cs_real_t pfacd = inc*cofaf[face_id][isou];
            for (int jsou = 0; jsou < 6; jsou++) {
              pfacd += cofbf[face_id][isou][jsou]*pippr[jsou];
            }

            cs_real_t flux = b_visc[face_id]*pfacd;
            rhs[ii][isou] -= flux;
          }
        }
      }
    }

    /* Unsteady */
  } else {

    for (int g_id = 0; g_id < n_b_groups; g_id++) {
#     pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
      for (int t_id = 0; t_id < n_b_threads; t_id++) {
        for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
             face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = b_face_cells[face_id];

          cs_real_t visci[3][3];
          cs_real_t diippf[3], pi[6], pipp[6];

          for (int isou = 0; isou < 6; isou++) {
            pi[isou] = pvar[ii][isou];
          }

          /* Recompute II"
             --------------*/

          visci[0][0] = viscce[ii][0];
          visci[1][1] = viscce[ii][1];
          visci[2][2] = viscce[ii][2];
          visci[1][0] = viscce[ii][3];
          visci[0][1] = viscce[ii][3];
          visci[2][1] = viscce[ii][4];
          visci[1][2] = viscce[ii][4];
          visci[2][0] = viscce[ii][5];
          visci[0][2] = viscce[ii][5];

          /* IF.Ki.S / ||Ki.S||^2 */
          cs_real_t fikdvi = weighb[face_id];

          /* II" = IF + FI" */
          for (int i = 0; i < 3; i++) {
            diippf[i] = b_face_cog[face_id][i] - cell_cen[ii][i]
                      - fikdvi*( visci[0][i]*b_face_normal[face_id][0]
                               + visci[1][i]*b_face_normal[face_id][1]
                               + visci[2][i]*b_face_normal[face_id][2]);
          }
          for (int isou = 0; isou < 6; isou++) {
            pipp[isou] = pi[isou] + ircflp*( grad[ii][isou][0]*diippf[0]
                                           + grad[ii][isou][1]*diippf[1]
                                           + grad[ii][isou][2]*diippf[2]);
          }
          for (int isou = 0; isou < 6; isou++) {
            cs_real_t pfacd = inc*cofaf[face_id][isou];
            for (int jsou = 0; jsou < 6; jsou++) {
              pfacd += cofbf[face_id][isou][jsou]*pipp[jsou];
            }

            cs_real_t flux = b_visc[face_id]*pfacd;
            rhs[ii][isou] -= thetap*flux;
          }

        }
      }
    }

  }

  /* Free memory */
  BFT_FREE(grad);
  BFT_FREE(w2);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Update the face mass flux with the face pressure (or pressure
 * increment, or pressure double increment) gradient.
 *
 * \f[
 * \dot{m}_\ij = \dot{m}_\ij
 *             - \Delta t \grad_\fij \delta p \cdot \vect{S}_\ij
 * \f]
 *
 * \param[in]     f_id          field id (or -1)
 * \param[in]     m             pointer to mesh
 * \param[in]     fvq           pointer to finite volume quantities
 * \param[in]     init          indicator
 *                               - 1 initialize the mass flux to 0
 *                               - 0 otherwise
 * \param[in]     inc           indicator
 *                               - 0 when solving an increment
 *                               - 1 otherwise
 * \param[in]     imrgra        indicator
 *                               - 0 iterative gradient
 *                               - 1 least square gradient
 * \param[in]     iccocg        indicator
 *                               - 1 re-compute cocg matrix
 *                                 (for iterative gradients)
 *                               - 0 otherwise
 * \param[in]     nswrgp        number of reconstruction sweeps for the
 *                               gradients
 * \param[in]     imligp        clipping gradient method
 *                               - < 0 no clipping
 *                               - = 0 thank to neighbooring gradients
 *                               - = 1 thank to the mean gradient
 * \param[in]     iphydp        hydrostatic pressure indicator
 * \param[in]     iwarnp        verbosity
 * \param[in]     epsrgp        relative precision for the gradient
 *                               reconstruction
 * \param[in]     climgp        clipping coeffecient for the computation of
 *                               the gradient
 * \param[in]     extrap        coefficient for extrapolation of the gradient
 * \param[in]     frcxt         body force creating the hydrostatic pressure
 * \param[in]     pvar          solved variable (current time step)
 * \param[in]     coefap        boundary condition array for the variable
 *                               (explicit part)
 * \param[in]     coefbp        boundary condition array for the variable
 *                               (implicit part)
 * \param[in]     cofafp        boundary condition array for the diffusion
 *                               of the variable (explicit part)
 * \param[in]     cofbfp        boundary condition array for the diffusion
 *                               of the variable (implicit part)
 * \param[in]     i_visc        \f$ \mu_\fij \dfrac{S_\fij}{\ipf \jpf} \f$
 *                               at interior faces for the r.h.s.
 * \param[in]     b_visc        \f$ \mu_\fib \dfrac{S_\fib}{\ipf \centf} \f$
 *                               at border faces for the r.h.s.
 * \param[in]     viselx        viscosity by cell, dir x
 * \param[in]     visely        viscosity by cell, dir y
 * \param[in]     viselz        viscosity by cell, dir z
 * \param[in,out] i_massflux    mass flux at interior faces
 * \param[in,out] b_massflux    mass flux at boundary faces
 */
/*----------------------------------------------------------------------------*/

void
cs_face_diffusion_potential(const int                 f_id,
                            const cs_mesh_t          *m,
                            cs_mesh_quantities_t     *fvq,
                            int                       init,
                            int                       inc,
                            int                       imrgra,
                            int                       iccocg,
                            int                       nswrgp,
                            int                       imligp,
                            int                       iphydp,
                            int                       iwarnp,
                            double                    epsrgp,
                            double                    climgp,
                            double                    extrap,
                            cs_real_3_t     *restrict frcxt,
                            cs_real_t       *restrict pvar,
                            const cs_real_t           coefap[],
                            const cs_real_t           coefbp[],
                            const cs_real_t           cofafp[],
                            const cs_real_t           cofbfp[],
                            const cs_real_t           i_visc[],
                            const cs_real_t           b_visc[],
                            cs_real_t       *restrict viselx,
                            const cs_real_t           visely[],
                            const cs_real_t           viselz[],
                            cs_real_t       *restrict i_massflux,
                            cs_real_t       *restrict b_massflux)
{
  const cs_halo_t  *halo = m->halo;

  const cs_lnum_t n_cells_ext = m->n_cells_with_ghosts;
  const int n_i_groups = m->i_face_numbering->n_groups;
  const int n_i_threads = m->i_face_numbering->n_threads;
  const int n_b_groups = m->b_face_numbering->n_groups;
  const int n_b_threads = m->b_face_numbering->n_threads;
  const cs_lnum_t *restrict i_group_index = m->i_face_numbering->group_index;
  const cs_lnum_t *restrict b_group_index = m->b_face_numbering->group_index;
  const cs_lnum_2_t *restrict i_face_cells
    = (const cs_lnum_2_t *restrict)m->i_face_cells;
  const cs_lnum_t *restrict b_face_cells
    = (const cs_lnum_t *restrict)m->b_face_cells;
  const cs_real_t *restrict i_dist = fvq->i_dist;
  const cs_real_t *restrict i_f_face_surf = fvq->i_f_face_surf;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)fvq->cell_cen;
  const cs_real_3_t *restrict dijpf
    = (const cs_real_3_t *restrict)fvq->dijpf;
  const cs_real_3_t *restrict diipb
    = (const cs_real_3_t *restrict)fvq->diipb;

  /* Local variables */

  char var_name[32];
  int tr_dim = 0;
  int w_stride = 1;

  bool recompute_cocg = (iccocg) ? true : false;

  cs_real_3_t *grad;
  cs_real_3_t *visel;
  cs_field_t *f;

  cs_real_t *gweight = NULL;

  /*==========================================================================*/

  /* i_visc and visel carry similar information */

  /*============================================================================
    1. Initialization
    ==========================================================================*/

  BFT_MALLOC(visel, n_cells_ext, cs_real_3_t);
  for (cs_lnum_t ii = 0; ii < n_cells_ext; ii++) {
    visel[ii][0] = viselx[ii];
    visel[ii][1] = visely[ii];
    visel[ii][2] = viselz[ii];
  }

  if (init >= 1) {
#   pragma omp parallel for
    for (cs_lnum_t face_id = 0; face_id < m->n_i_faces; face_id++) {
      i_massflux[face_id] = 0.;
    }
#   pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
    for (cs_lnum_t face_id = 0; face_id < m->n_b_faces; face_id++) {
      b_massflux[face_id] = 0.;
    }
  } else if (init != 0) {
    bft_error(__FILE__, __LINE__, 0,
              _("invalid value of init"));
  }

  /* Use iterative gradient */

  cs_halo_type_t halo_type = CS_HALO_STANDARD;
  cs_gradient_type_t gradient_type = CS_GRADIENT_ITER;

  if (imrgra < 0)
    imrgra = 0;

  cs_gradient_type_by_imrgra(imrgra,
                             &gradient_type,
                             &halo_type);

  if (f_id > -1) {
    f = cs_field_by_id(f_id);
    snprintf(var_name, 31, "%s", f->name);
  }
  else
    strcpy(var_name, "Work array");
  var_name[31] = '\0';

  /* Handle parallelism and periodicity */

  if (halo != NULL)
    cs_halo_sync_var(halo, halo_type, pvar);

  /*==========================================================================
    2. Update mass flux without reconstruction technics
    ==========================================================================*/

  if (nswrgp <= 1) {

    /* Mass flow through interior faces */

    for (int g_id = 0; g_id < n_i_groups; g_id++) {
#     pragma omp parallel for
      for (int t_id = 0; t_id < n_i_threads; t_id++) {
        for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
             face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = i_face_cells[face_id][0];
          cs_lnum_t jj = i_face_cells[face_id][1];

          i_massflux[face_id] += i_visc[face_id]*(pvar[ii] -pvar[jj]);

        }
      }
    }

    /* Mass flow through boundary faces */

    for (int g_id = 0; g_id < n_b_groups; g_id++) {
#     pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
      for (int t_id = 0; t_id < n_b_threads; t_id++) {
        for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
             face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = b_face_cells[face_id];
          double pfac = inc*cofafp[face_id] + cofbfp[face_id]*pvar[ii];

          b_massflux[face_id] += b_visc[face_id]*pfac;

        }
      }
    }

  }

  /*==========================================================================
    3. Update mass flux with reconstruction technique if the mesh is non
       orthogonal
    ==========================================================================*/

  if (nswrgp > 1) {

    /* Allocate a work array for the gradient calculation */
    BFT_MALLOC(grad, n_cells_ext, cs_real_3_t);

    /* Compute gradient */
    if (f_id > -1) {
      /* Get the calculation option from the field */
      int key_cal_opt_id = cs_field_key_id("var_cal_opt");
      cs_var_cal_opt_t var_cal_opt;
      cs_field_get_key_struct(f, key_cal_opt_id, &var_cal_opt);
      if (f->type & CS_FIELD_VARIABLE && var_cal_opt.iwgrec == 1) {
        if (var_cal_opt.idiff > 0) {
          int key_id = cs_field_key_id("gradient_weighting_id");
          int diff_id = cs_field_get_key_int(f, key_id);
          if (diff_id > -1) {
            cs_field_t *weight_f = cs_field_by_id(diff_id);
            gweight = weight_f->val;
            w_stride = weight_f->dim;
          }
        }
      }
    }
    else if (f_id == -2) {
      gweight = viselx;
    }

    cs_gradient_scalar(var_name,
                       gradient_type,
                       halo_type,
                       inc,
                       recompute_cocg,
                       nswrgp,
                       tr_dim,
                       iphydp,
                       w_stride,
                       iwarnp,
                       imligp,
                       epsrgp,
                       extrap,
                       climgp,
                       frcxt,
                       coefap,
                       coefbp,
                       pvar,
                       gweight, /* Weighted gradient */
                       grad);

    /* Handle parallelism and periodicity */

    if (halo != NULL) {
      cs_halo_sync_var_strided(halo, halo_type, (cs_real_t *)visel, 3);
      if (m->n_init_perio > 0)
        cs_halo_perio_sync_var_vect(halo, halo_type, (cs_real_t *)visel, 3);
    }

    /* Mass flow through interior faces */

    for (int g_id = 0; g_id < n_i_groups; g_id++) {
#     pragma omp parallel for
      for (int t_id = 0; t_id < n_i_threads; t_id++) {
        for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
             face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = i_face_cells[face_id][0];
          cs_lnum_t jj = i_face_cells[face_id][1];

          double dpxf = 0.5*(  visel[ii][0]*grad[ii][0]
                             + visel[jj][0]*grad[jj][0]);
          double dpyf = 0.5*(  visel[ii][1]*grad[ii][1]
                             + visel[jj][1]*grad[jj][1]);
          double dpzf = 0.5*(  visel[ii][2]*grad[ii][2]
                             + visel[jj][2]*grad[jj][2]);

          double dijpfx = dijpf[face_id][0];
          double dijpfy = dijpf[face_id][1];
          double dijpfz = dijpf[face_id][2];

          /*---> Dij = IJ - (IJ.N) N */
          double dijx = (cell_cen[jj][0]-cell_cen[ii][0])-dijpfx;
          double dijy = (cell_cen[jj][1]-cell_cen[ii][1])-dijpfy;
          double dijz = (cell_cen[jj][2]-cell_cen[ii][2])-dijpfz;

          i_massflux[face_id] =  i_massflux[face_id]
                               + i_visc[face_id]*(pvar[ii] - pvar[jj])
                               +  (dpxf *dijx + dpyf*dijy + dpzf*dijz)
                                 * i_f_face_surf[face_id]/i_dist[face_id];

        }
      }
    }

    /* Mass flow through boundary faces */

    for (int g_id = 0; g_id < n_b_groups; g_id++) {
#     pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
      for (int t_id = 0; t_id < n_b_threads; t_id++) {
        for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
             face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = b_face_cells[face_id];

          double diipbx = diipb[face_id][0];
          double diipby = diipb[face_id][1];
          double diipbz = diipb[face_id][2];

          double pip = pvar[ii] + grad[ii][0]*diipbx
                                + grad[ii][1]*diipby
                                + grad[ii][2]*diipbz;
          double pfac = inc*cofafp[face_id] + cofbfp[face_id]*pip;

          b_massflux[face_id] += b_visc[face_id]*pfac;

        }
      }
    }

    /* Free memory */
    BFT_FREE(grad);
  }
  BFT_FREE(visel);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Add the explicit part of the pressure gradient term to the mass flux
 * in case of anisotropic diffusion of the pressure field \f$ P \f$.
 *
 * More precisely, the mass flux side \f$ \dot{m}_\fij \f$ is updated as
 * follows:
 * \f[
 * \dot{m}_\fij = \dot{m}_\fij -
 *              \left( \tens{\mu}_\fij \gradv_\fij P \cdot \vect{S}_\ij  \right)
 * \f]
 *
 * \param[in]     f_id          field id (or -1)
 * \param[in]     m             pointer to mesh
 * \param[in]     fvq           pointer to finite volume quantities
 * \param[in]     init           indicator
 *                               - 1 initialize the mass flux to 0
 *                               - 0 otherwise
 * \param[in]     inc           indicator
 *                               - 0 when solving an increment
 *                               - 1 otherwise
 * \param[in]     imrgra        indicator
 *                               - 0 iterative gradient
 *                               - 1 least square gradient
 * \param[in]     iccocg        indicator
 *                               - 1 re-compute cocg matrix
                                    (for iterativ gradients)
 *                               - 0 otherwise
 * \param[in]     nswrgp        number of reconstruction sweeps for the
 *                               gradients
 * \param[in]     imligp        clipping gradient method
 *                               - < 0 no clipping
 *                               - = 0 thank to neighbooring gradients
 *                               - = 1 thank to the mean gradient
 * \param[in]     ircflp        indicator
 *                               - 1 flux reconstruction,
 *                               - 0 otherwise
 * \param[in]     iphydp        indicator
 *                               - 1 hydrostatic pressure taken into account
 *                               - 0 otherwise
 * \param[in]     iwarnp        verbosity
 * \param[in]     epsrgp        relative precision for the gradient
 *                               reconstruction
 * \param[in]     climgp        clipping coeffecient for the computation of
 *                               the gradient
 * \param[in]     extrap        coefficient for extrapolation of the gradient
 * \param[in]     frcxt         body force creating the hydrostatic pressure
 * \param[in]     pvar          solved variable (pressure)
 * \param[in]     coefap        boundary condition array for the variable
 *                               (explicit part)
 * \param[in]     coefbp        boundary condition array for the variable
 *                               (implicit part)
 * \param[in]     cofafp        boundary condition array for the diffusion
 *                               of the variable (explicit part)
 * \param[in]     cofbfp        boundary condition array for the diffusion
 *                               of the variable (implicit part)
 * \param[in]     i_visc        \f$ \mu_\fij \dfrac{S_\fij}{\ipf \jpf} \f$
 *                               at interior faces for the r.h.s.
 * \param[in]     b_visc        \f$ \mu_\fib \dfrac{S_\fib}{\ipf \centf} \f$
 *                               at border faces for the r.h.s.
 * \param[in]     viscel        symmetric cell tensor \f$ \tens{\mu}_\celli \f$
 * \param[in]     weighf        internal face weight between cells i j in case
 *                               of tensor diffusion
 * \param[in]     weighb        boundary face weight for cells i in case
 *                               of tensor diffusion
 * \param[in,out] i_massflux    mass flux at interior faces
 * \param[in,out] b_massflux    mass flux at boundary faces
 */
/*----------------------------------------------------------------------------*/

void
cs_face_anisotropic_diffusion_potential(const int                 f_id,
                                        const cs_mesh_t          *m,
                                        cs_mesh_quantities_t     *fvq,
                                        int                       init,
                                        int                       inc,
                                        int                       imrgra,
                                        int                       iccocg,
                                        int                       nswrgp,
                                        int                       imligp,
                                        int                       ircflp,
                                        int                       iphydp,
                                        int                       iwarnp,
                                        double                    epsrgp,
                                        double                    climgp,
                                        double                    extrap,
                                        cs_real_3_t     *restrict frcxt,
                                        cs_real_t       *restrict pvar,
                                        const cs_real_t           coefap[],
                                        const cs_real_t           coefbp[],
                                        const cs_real_t           cofafp[],
                                        const cs_real_t           cofbfp[],
                                        const cs_real_t           i_visc[],
                                        const cs_real_t           b_visc[],
                                        cs_real_6_t     *restrict viscel,
                                        const cs_real_2_t         weighf[],
                                        const cs_real_t           weighb[],
                                        cs_real_t       *restrict i_massflux,
                                        cs_real_t       *restrict b_massflux)
{
  const cs_halo_t  *halo = m->halo;

  const cs_lnum_t n_cells = m->n_cells;
  const cs_lnum_t n_cells_ext = m->n_cells_with_ghosts;

  const int n_i_groups = m->i_face_numbering->n_groups;
  const int n_i_threads = m->i_face_numbering->n_threads;
  const int n_b_groups = m->b_face_numbering->n_groups;
  const int n_b_threads = m->b_face_numbering->n_threads;
  const cs_lnum_t *restrict i_group_index = m->i_face_numbering->group_index;
  const cs_lnum_t *restrict b_group_index = m->b_face_numbering->group_index;

  const cs_lnum_2_t *restrict i_face_cells
    = (const cs_lnum_2_t *restrict)m->i_face_cells;
  const cs_lnum_t *restrict b_face_cells
    = (const cs_lnum_t *restrict)m->b_face_cells;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)fvq->cell_cen;
  const cs_real_3_t *restrict i_face_normal
    = (const cs_real_3_t *restrict)fvq->i_face_normal;
  const cs_real_3_t *restrict b_face_normal
    = (const cs_real_3_t *restrict)fvq->b_face_normal;
  const cs_real_3_t *restrict i_face_cog
    = (const cs_real_3_t *restrict)fvq->i_face_cog;
  const cs_real_3_t *restrict b_face_cog
    = (const cs_real_3_t *restrict)fvq->b_face_cog;

  /* Local variables */

  char var_name[32];
  int tr_dim = 0;
  int w_stride = 6;

  bool recompute_cocg = (iccocg) ? true : false;

  cs_real_6_t *viscce;
  cs_real_6_t *w2;
  cs_real_3_t *grad;
  cs_field_t *f;

  cs_real_t *gweight = NULL;

  /*==========================================================================
    1. Initialization
    ==========================================================================*/

  if (init >= 1) {
    #pragma omp parallel for
    for (cs_lnum_t face_id = 0; face_id < m->n_i_faces; face_id++) {
      i_massflux[face_id] = 0.;
    }
    for (cs_lnum_t face_id = 0; face_id < m->n_b_faces; face_id++) {
      b_massflux[face_id] = 0.;
    }
  } else if (init != 0) {
      bft_error(__FILE__, __LINE__, 0,
                _("invalid value of init"));
  }

  /* Use iterative gradient */

  cs_halo_type_t halo_type = CS_HALO_STANDARD;
  cs_gradient_type_t gradient_type = CS_GRADIENT_ITER;

  if (imrgra < 0)
    imrgra = 0;

  cs_gradient_type_by_imrgra(imrgra,
                             &gradient_type,
                             &halo_type);

  if (f_id > -1) {
    f = cs_field_by_id(f_id);
    snprintf(var_name, 31, "%s", f->name);
  }
  else
    strcpy(var_name, "Work array");
  var_name[31] = '\0';

  /* Porosity fields */
  cs_field_t *fporo = cs_field_by_name_try("porosity");
  cs_field_t *ftporo = cs_field_by_name_try("tensorial_porosity");

  cs_real_t *porosi = NULL;
  cs_real_6_t *porosf = NULL;

  if (cs_glob_porous_model == 1 || cs_glob_porous_model == 2) {
    porosi = fporo->val;
    if (ftporo != NULL) {
      porosf = (cs_real_6_t *)ftporo->val;
    }
  }

  /* Handle parallelism and periodicity */

  if (halo != NULL)
    cs_halo_sync_var(halo, halo_type, pvar);

  /*==========================================================================
    2. Update mass flux without reconstruction technique
    ==========================================================================*/

  if (nswrgp <= 1) {

    /* ---> Contribution from interior faces */

    for (int g_id = 0; g_id < n_i_groups; g_id++) {
#     pragma omp parallel for
      for (int t_id = 0; t_id < n_i_threads; t_id++) {
        for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
             face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = i_face_cells[face_id][0];
          cs_lnum_t jj = i_face_cells[face_id][1];

          i_massflux[face_id] += i_visc[face_id]*(pvar[ii] - pvar[jj]);

        }
      }
    }

    /* ---> Contribution from boundary faces */

    for (int g_id = 0; g_id < n_b_groups; g_id++) {
#     pragma omp parallel for
      for (int t_id = 0; t_id < n_b_threads; t_id++) {
        for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
             face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = b_face_cells[face_id];
          double pfac = inc*cofafp[face_id] + cofbfp[face_id]*pvar[ii];

          b_massflux[face_id] += b_visc[face_id]*pfac;

        }
      }
    }

  }

  /*==========================================================================
    3. Update mass flux with reconstruction technique
    ==========================================================================*/

  if (nswrgp > 1) {

    viscce = NULL;
    w2 = NULL;

    /* Without porosity */
    if (porosi == NULL) {
      viscce = viscel;

      /* With porosity */
    } else if (porosi != NULL && porosf == NULL) {
      BFT_MALLOC(w2, n_cells_ext, cs_real_6_t);
      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
        for (int isou = 0; isou < 6; isou++) {
          w2[cell_id][isou] = porosi[cell_id]*viscel[cell_id][isou];
        }
      }
      viscce = w2;

      /* With tensorial porosity */
    } else if (porosi != NULL && porosf != NULL) {
      BFT_MALLOC(w2, n_cells_ext, cs_real_6_t);
      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
        cs_math_sym_33_product(porosf[cell_id],
                               viscel[cell_id],
                               w2[cell_id]);
      }
      viscce = w2;
    }

    /* ---> Periodicity and parallelism treatment of symmetric tensors */

    if (halo != NULL) {
      cs_halo_sync_var_strided(halo, CS_HALO_STANDARD, (cs_real_t *)viscce, 6);

      if (m->n_init_perio > 0)
        cs_halo_perio_sync_var_sym_tens(halo,
                                        CS_HALO_STANDARD,
                                        (cs_real_t *)viscce);
    }

    /* Allocate a work array for the gradient calculation */
    BFT_MALLOC(grad, n_cells_ext, cs_real_3_t);

    /* Compute gradient */
    if (f_id > -1) {
      /* Get the calculation option from the field */
      int key_cal_opt_id = cs_field_key_id("var_cal_opt");
      cs_var_cal_opt_t var_cal_opt;
      cs_field_get_key_struct(f, key_cal_opt_id, &var_cal_opt);
      if (f->type & CS_FIELD_VARIABLE && var_cal_opt.iwgrec == 1) {
        if (var_cal_opt.idifft > 0) {
          int key_id = cs_field_key_id("gradient_weighting_id");
          int diff_id = cs_field_get_key_int(f, key_id);
          if (diff_id > -1) {
            cs_field_t *weight_f = cs_field_by_id(diff_id);
            gweight = weight_f->val;
            w_stride = weight_f->dim;
          }
        }
      }
    }
    else if (f_id == -2) {
      gweight = (cs_real_t *)viscce;
    }

    /* Compute gradient */
    cs_gradient_scalar(var_name,
                       gradient_type,
                       halo_type,
                       inc,
                       recompute_cocg,
                       nswrgp,
                       tr_dim,
                       iphydp,
                       w_stride,
                       iwarnp,
                       imligp,
                       epsrgp,
                       extrap,
                       climgp,
                       frcxt,
                       coefap,
                       coefbp,
                       pvar,
                       gweight, /* Weighted gradient */
                       grad);

    /* Mass flow through interior faces */

    for (int g_id = 0; g_id < n_i_groups; g_id++) {
#     pragma omp parallel for
      for (int t_id = 0; t_id < n_i_threads; t_id++) {
        for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
             face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = i_face_cells[face_id][0];
          cs_lnum_t jj = i_face_cells[face_id][1];

          double pi = pvar[ii];
          double pj = pvar[jj];

          /* Recompute II" and JJ"
             ----------------------*/

          cs_real_t visci[3][3], viscj[3][3];
          cs_real_t diippf[3], djjppf[3];

          visci[0][0] = viscce[ii][0];
          visci[1][1] = viscce[ii][1];
          visci[2][2] = viscce[ii][2];
          visci[1][0] = viscce[ii][3];
          visci[0][1] = viscce[ii][3];
          visci[2][1] = viscce[ii][4];
          visci[1][2] = viscce[ii][4];
          visci[2][0] = viscce[ii][5];
          visci[0][2] = viscce[ii][5];

          /* IF.Ki.S / ||Ki.S||^2 */
          cs_real_t fikdvi = weighf[face_id][0];

          /* II" = IF + FI" */
          for (int i = 0; i < 3; i++) {
            diippf[i] = i_face_cog[face_id][i]-cell_cen[ii][i]
                      - fikdvi*(  visci[0][i]*i_face_normal[face_id][0]
                                + visci[1][i]*i_face_normal[face_id][1]
                                + visci[2][i]*i_face_normal[face_id][2] );
          }

          viscj[0][0] = viscce[jj][0];
          viscj[1][1] = viscce[jj][1];
          viscj[2][2] = viscce[jj][2];
          viscj[1][0] = viscce[jj][3];
          viscj[0][1] = viscce[jj][3];
          viscj[2][1] = viscce[jj][4];
          viscj[1][2] = viscce[jj][4];
          viscj[2][0] = viscce[jj][5];
          viscj[0][2] = viscce[jj][5];

          /* FJ.Kj.S / ||Kj.S||^2 */
          double fjkdvi = weighf[face_id][1];

          /* JJ" = JF + FJ" */
          for (int i = 0; i < 3; i++) {
            djjppf[i] = i_face_cog[face_id][i]-cell_cen[jj][i]
                      + fjkdvi*(  viscj[0][i]*i_face_normal[face_id][0]
                                + viscj[1][i]*i_face_normal[face_id][1]
                                + viscj[2][i]*i_face_normal[face_id][2] );
          }

          /* p in I" and J" */
          double pipp = pi + ircflp*(  grad[ii][0]*diippf[0]
                                     + grad[ii][1]*diippf[1]
                                     + grad[ii][2]*diippf[2]);
          double pjpp = pj + ircflp*(  grad[jj][0]*djjppf[0]
                                     + grad[jj][1]*djjppf[1]
                                     + grad[jj][2]*djjppf[2]);

          i_massflux[face_id] += i_visc[face_id]*(pipp - pjpp);

        }
      }
    }

    /* ---> Contribution from boundary faces */

    for (int g_id = 0; g_id < n_b_groups; g_id++) {
#     pragma omp parallel for
      for (int t_id = 0; t_id < n_b_threads; t_id++) {
        for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
             face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = b_face_cells[face_id];

          double pi = pvar[ii];

          /* Recompute II"
             --------------*/

          cs_real_t visci[3][3];
          cs_real_t diippf[3];

          visci[0][0] = viscce[ii][0];
          visci[1][1] = viscce[ii][1];
          visci[2][2] = viscce[ii][2];
          visci[1][0] = viscce[ii][3];
          visci[0][1] = viscce[ii][3];
          visci[2][1] = viscce[ii][4];
          visci[1][2] = viscce[ii][4];
          visci[2][0] = viscce[ii][5];
          visci[0][2] = viscce[ii][5];

          /* IF.Ki.S / ||Ki.S||^2 */
          cs_real_t fikdvi = weighb[face_id];

          /* II" = IF + FI" */
          for (int i = 0; i < 3; i++) {
            diippf[i] = b_face_cog[face_id][i] - cell_cen[ii][i]
                      - fikdvi*(  visci[0][i]*b_face_normal[face_id][0]
                                + visci[1][i]*b_face_normal[face_id][1]
                                + visci[2][i]*b_face_normal[face_id][2] );
          }

          double pipp = pi + ircflp*( grad[ii][0]*diippf[0]
                                     + grad[ii][1]*diippf[1]
                                     + grad[ii][2]*diippf[2]);


          double pfac = inc*cofafp[face_id] + cofbfp[face_id]*pipp;

          b_massflux[face_id] += b_visc[face_id]*pfac;

        }
      }
    }

    /* Free memory */
    BFT_FREE(grad);
    BFT_FREE(w2);

  }
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Update the cell mass flux divergence with the face pressure (or
 * pressure increment, or pressure double increment) gradient.
 *
 * \f[
 * \dot{m}_\ij = \dot{m}_\ij
 *             - \sum_j \Delta t \grad_\fij p \cdot \vect{S}_\ij
 * \f]
 *
 * \param[in]     f_id          field id (or -1)
 * \param[in]     m             pointer to mesh
 * \param[in]     fvq           pointer to finite volume quantities
 * \param[in]     init          indicator
 *                               - 1 initialize the mass flux to 0
 *                               - 0 otherwise
 * \param[in]     inc           indicator
 *                               - 0 when solving an increment
 *                               - 1 otherwise
 * \param[in]     imrgra        indicator
 *                               - 0 iterative gradient
 *                               - 1 least square gradient
 * \param[in]     iccocg        indicator
 *                               - 1 re-compute cocg matrix
 *                                 (for iterative gradients)
 *                               - 0 otherwise
 * \param[in]     nswrgp        number of reconstruction sweeps for the
 *                               gradients
 * \param[in]     imligp        clipping gradient method
 *                               - < 0 no clipping
 *                               - = 0 thank to neighbooring gradients
 *                               - = 1 thank to the mean gradient
 * \param[in]     iphydp        hydrostatic pressure indicator
 * \param[in]     iwarnp        verbosity
 * \param[in]     epsrgp        relative precision for the gradient
 *                               reconstruction
 * \param[in]     climgp        clipping coeffecient for the computation of
 *                               the gradient
 * \param[in]     extrap        coefficient for extrapolation of the gradient
 * \param[in]     frcxt         body force creating the hydrostatic pressure
 * \param[in]     pvar          solved variable (current time step)
 * \param[in]     coefap        boundary condition array for the variable
 *                               (explicit part)
 * \param[in]     coefbp        boundary condition array for the variable
 *                               (implicit part)
 * \param[in]     cofafp        boundary condition array for the diffusion
 *                               of the variable (explicit part)
 * \param[in]     cofbfp        boundary condition array for the diffusion
 *                               of the variable (implicit part)
 * \param[in]     i_visc        \f$ \mu_\fij \dfrac{S_\fij}{\ipf \jpf} \f$
 *                               at interior faces for the r.h.s.
 * \param[in]     b_visc        \f$ \mu_\fib \dfrac{S_\fib}{\ipf \centf} \f$
 *                               at border faces for the r.h.s.
 * \param[in]     viselx        viscosity by cell, dir x
 * \param[in]     visely        viscosity by cell, dir y
 * \param[in]     viselz        viscosity by cell, dir z
 * \param[in,out] diverg        mass flux divergence
 */
/*----------------------------------------------------------------------------*/

void
cs_diffusion_potential(const int                 f_id,
                       const cs_mesh_t          *m,
                       cs_mesh_quantities_t     *fvq,
                       int                       init,
                       int                       inc,
                       int                       imrgra,
                       int                       iccocg,
                       int                       nswrgp,
                       int                       imligp,
                       int                       iphydp,
                       int                       iwarnp,
                       double                    epsrgp,
                       double                    climgp,
                       double                    extrap,
                       cs_real_3_t     *restrict frcxt,
                       cs_real_t       *restrict pvar,
                       const cs_real_t           coefap[],
                       const cs_real_t           coefbp[],
                       const cs_real_t           cofafp[],
                       const cs_real_t           cofbfp[],
                       const cs_real_t           i_visc[],
                       const cs_real_t           b_visc[],
                       const cs_real_t           viselx[],
                       const cs_real_t           visely[],
                       const cs_real_t           viselz[],
                       cs_real_t       *restrict diverg)
{
  const cs_halo_t  *halo = m->halo;

  const cs_lnum_t n_cells = m->n_cells;
  const cs_lnum_t n_cells_ext = m->n_cells_with_ghosts;
  const int n_i_groups = m->i_face_numbering->n_groups;
  const int n_i_threads = m->i_face_numbering->n_threads;
  const int n_b_groups = m->b_face_numbering->n_groups;
  const int n_b_threads = m->b_face_numbering->n_threads;
  const cs_lnum_t *restrict i_group_index = m->i_face_numbering->group_index;
  const cs_lnum_t *restrict b_group_index = m->b_face_numbering->group_index;

  const cs_lnum_2_t *restrict i_face_cells
    = (const cs_lnum_2_t *restrict)m->i_face_cells;
  const cs_real_3_t *restrict i_face_cog
    = (const cs_real_3_t *restrict)fvq->i_face_cog;
  const cs_lnum_t *restrict b_face_cells
    = (const cs_lnum_t *restrict)m->b_face_cells;
  const cs_real_t *restrict weight = fvq->weight;
  const cs_real_t *restrict i_dist = fvq->i_dist;
  const cs_real_t *restrict i_f_face_surf = fvq->i_f_face_surf;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)fvq->cell_cen;
  const cs_real_3_t *restrict dijpf
    = (const cs_real_3_t *restrict)fvq->dijpf;
  const cs_real_3_t *restrict diipb
    = (const cs_real_3_t *restrict)fvq->diipb;

  /* Local variables */

  char var_name[32];
  int tr_dim = 0;
  int mass_flux_rec_type = cs_glob_stokes_model->irecmf;
  int w_stride = 1;

  bool recompute_cocg = (iccocg) ? true : false;

  cs_real_3_t *grad;
  cs_real_3_t *visel;
  cs_field_t *f;

  cs_real_t *gweight = NULL;

  /*==========================================================================*/

  /*==========================================================================
    1. Initialization
    ==========================================================================*/

  BFT_MALLOC(visel, n_cells_ext, cs_real_3_t);

# pragma omp parallel for
  for (cs_lnum_t ii = 0; ii < n_cells_ext; ii++) {
    visel[ii][0] = viselx[ii];
    visel[ii][1] = visely[ii];
    visel[ii][2] = viselz[ii];
  }

  if (init >= 1) {
#   pragma omp parallel for
    for (cs_lnum_t ii = 0; ii < n_cells_ext; ii++) {
      diverg[ii] = 0.;
    }
  } else if (init == 0 && n_cells_ext > n_cells) {
#   pragma omp parallel for if(n_cells_ext - n_cells > CS_THR_MIN)
    for (cs_lnum_t ii = n_cells; ii < n_cells_ext; ii++) {
      diverg[ii] = 0.;
    }
  } else if (init != 0) {
    bft_error(__FILE__, __LINE__, 0,
              _("invalid value of init"));
  }

  /* Use iterative gradient */

  cs_halo_type_t halo_type = CS_HALO_STANDARD;
  cs_gradient_type_t gradient_type = CS_GRADIENT_ITER;

  if (imrgra < 0)
    imrgra = 0;

  cs_gradient_type_by_imrgra(imrgra,
                             &gradient_type,
                             &halo_type);
  if (f_id != -1) {
    f = cs_field_by_id(f_id);
    snprintf(var_name, 31, "%s", f->name); var_name[31] = '\0';
  }
  else
    strcpy(var_name, "Work array"); var_name[31] = '\0';

  /* Handle parallelism and periodicity */

  if (halo != NULL)
    cs_halo_sync_var(halo, halo_type, pvar);

  /*==========================================================================
    2. Update mass flux without reconstruction technics
    ==========================================================================*/

  if (nswrgp <= 1) {

    /* Mass flow through interior faces */

    for (int g_id = 0; g_id < n_i_groups; g_id++) {
#     pragma omp parallel for
      for (int t_id = 0; t_id < n_i_threads; t_id++) {
        for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
             face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = i_face_cells[face_id][0];
          cs_lnum_t jj = i_face_cells[face_id][1];

          double i_massflux = i_visc[face_id]*(pvar[ii] -pvar[jj]);
          diverg[ii] += i_massflux;
          diverg[jj] -= i_massflux;

        }
      }
    }

    /* Mass flow through boundary faces */

    for (int g_id = 0; g_id < n_b_groups; g_id++) {
#     pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
      for (int t_id = 0; t_id < n_b_threads; t_id++) {
        for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
             face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = b_face_cells[face_id];
          double pfac = inc*cofafp[face_id] +cofbfp[face_id]*pvar[ii];

          double b_massflux = b_visc[face_id]*pfac;
          diverg[ii] += b_massflux;

        }
      }
    }

  }

  /*==========================================================================
    3. Update mass flux with reconstruction technics if the mesh is non
       orthogonal
    ==========================================================================*/

  if (nswrgp > 1) {

    /* Allocate a work array for the gradient calculation */
    BFT_MALLOC(grad, n_cells_ext, cs_real_3_t);

    /* Compute gradient */
    if (f_id != -1) {
      /* Get the calculation option from the field */
      int key_cal_opt_id = cs_field_key_id("var_cal_opt");
      cs_var_cal_opt_t var_cal_opt;
      cs_field_get_key_struct(f, key_cal_opt_id, &var_cal_opt);
      if (f->type & CS_FIELD_VARIABLE && var_cal_opt.iwgrec == 1) {
        if (var_cal_opt.idiff > 0) {
          int key_id = cs_field_key_id("gradient_weighting_id");
          int diff_id = cs_field_get_key_int(f, key_id);
          if (diff_id > -1) {
            cs_field_t *weight_f = cs_field_by_id(diff_id);
            gweight = weight_f->val;
            w_stride = weight_f->dim;
          }
        }
      }
    }

    cs_gradient_scalar(var_name,
                       gradient_type,
                       halo_type,
                       inc,
                       recompute_cocg,
                       nswrgp,
                       tr_dim,
                       iphydp,
                       w_stride,
                       iwarnp,
                       imligp,
                       epsrgp,
                       extrap,
                       climgp,
                       frcxt,
                       coefap,
                       coefbp,
                       pvar,
                       gweight, /* Weighted gradient */
                       grad);

    /* Handle parallelism and periodicity */

    if (halo != NULL) {
      cs_halo_sync_var_strided(halo, halo_type, (cs_real_t *)visel, 3);
      if (m->n_init_perio > 0)
        cs_halo_perio_sync_var_vect(halo, halo_type, (cs_real_t *)visel, 3);
    }

    /* Mass flow through interior faces */

    for (int g_id = 0; g_id < n_i_groups; g_id++) {
#     pragma omp parallel for
      for (int t_id = 0; t_id < n_i_threads; t_id++) {
        for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
             face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = i_face_cells[face_id][0];
          cs_lnum_t jj = i_face_cells[face_id][1];

          double i_massflux = i_visc[face_id]*(pvar[ii] - pvar[jj]);

          if (mass_flux_rec_type == 0) {
            double dijpfx = dijpf[face_id][0];
            double dijpfy = dijpf[face_id][1];
            double dijpfz = dijpf[face_id][2];

            /*---> Dij = IJ - (IJ.N) N */
            double dijx = (cell_cen[jj][0]-cell_cen[ii][0])-dijpfx;
            double dijy = (cell_cen[jj][1]-cell_cen[ii][1])-dijpfy;
            double dijz = (cell_cen[jj][2]-cell_cen[ii][2])-dijpfz;

            double dpxf = 0.5*(  visel[ii][0]*grad[ii][0]
                               + visel[jj][0]*grad[jj][0]);
            double dpyf = 0.5*(  visel[ii][1]*grad[ii][1]
                               + visel[jj][1]*grad[jj][1]);
            double dpzf = 0.5*(  visel[ii][2]*grad[ii][2]
                               + visel[jj][2]*grad[jj][2]);

            i_massflux += (dpxf*dijx + dpyf*dijy + dpzf*dijz)
                          *i_f_face_surf[face_id]/i_dist[face_id];
          }
          else {
            /* Recompute II' and JJ' */
            double pnd = weight[face_id];
            double diipfx = i_face_cog[face_id][0]
                            - (cell_cen[ii][0]
                              + (1.-pnd)*dijpf[face_id][0]);
            double diipfy = i_face_cog[face_id][1]
                            - (cell_cen[ii][1]
                              + (1.-pnd)*dijpf[face_id][1]);
            double diipfz = i_face_cog[face_id][2]
                            - (cell_cen[ii][2]
                              + (1.-pnd)*dijpf[face_id][2]);
            double djjpfx = i_face_cog[face_id][0]
                            - cell_cen[jj][0]
                            + pnd*dijpf[face_id][0];
            double djjpfy = i_face_cog[face_id][1]
                            - cell_cen[jj][1]
                            + pnd*dijpf[face_id][1];
            double djjpfz = i_face_cog[face_id][2]
                            - cell_cen[jj][2]
                            + pnd*dijpf[face_id][2];

            i_massflux += i_visc[face_id]*
                          ( grad[ii][0]*diipfx
                          + grad[ii][1]*diipfy
                          + grad[ii][2]*diipfz
                          - grad[jj][0]*djjpfx
                          - grad[jj][1]*djjpfy
                          - grad[jj][2]*djjpfz );
          }

          diverg[ii] += i_massflux;
          diverg[jj] -= i_massflux;

        }
      }
    }

    /* Mass flow through boundary faces */

    for (int g_id = 0; g_id < n_b_groups; g_id++) {
#     pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
      for (int t_id = 0; t_id < n_b_threads; t_id++) {
        for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
             face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = b_face_cells[face_id];

          double diipbx = diipb[face_id][0];
          double diipby = diipb[face_id][1];
          double diipbz = diipb[face_id][2];

          double pip = pvar[ii] + grad[ii][0]*diipbx
                                + grad[ii][1]*diipby
                                + grad[ii][2]*diipbz;
          double pfac = inc*cofafp[face_id] +cofbfp[face_id]*pip;

          double b_massflux = b_visc[face_id]*pfac;
          diverg[ii] += b_massflux;

        }
      }
    }

    /* Free memory */
    BFT_FREE(grad);
  }
  BFT_FREE(visel);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Add the explicit part of the divergence of the mass flux due to the
 * pressure gradient (routine analog to diften).
 *
 * More precisely, the divergence of the mass flux side
 * \f$ \sum_{\fij \in \Facei{\celli}} \dot{m}_\fij \f$ is updated as follows:
 * \f[
 * \sum_{\fij \in \Facei{\celli}} \dot{m}_\fij
 *  = \sum_{\fij \in \Facei{\celli}} \dot{m}_\fij
 *  - \sum_{\fij \in \Facei{\celli}}
 *    \left( \tens{\mu}_\fij \gradv_\fij P \cdot \vect{S}_\ij  \right)
 * \f]
 *
 * \param[in]     f_id          field id (or -1)
 * \param[in]     m             pointer to mesh
 * \param[in]     fvq           pointer to finite volume quantities
 * \param[in]     init           indicator
 *                               - 1 initialize the mass flux to 0
 *                               - 0 otherwise
 * \param[in]     inc           indicator
 *                               - 0 when solving an increment
 *                               - 1 otherwise
 * \param[in]     imrgra        indicator
 *                               - 0 iterative gradient
 *                               - 1 least square gradient
 * \param[in]     iccocg        indicator
 *                               - 1 re-compute cocg matrix
                                     (for iterativ gradients)
 *                               - 0 otherwise
 * \param[in]     nswrgp        number of reconstruction sweeps for the
 *                               gradients
 * \param[in]     imligp        clipping gradient method
 *                               - < 0 no clipping
 *                               - = 0 thank to neighbooring gradients
 *                               - = 1 thank to the mean gradient
 * \param[in]     ircflp        indicator
 *                               - 1 flux reconstruction,
 *                               - 0 otherwise
 * \param[in]     iphydp        indicator
 *                               - 1 hydrostatic pressure taken into account
 *                               - 0 otherwise
 * \param[in]     iwarnp        verbosity
 * \param[in]     epsrgp        relative precision for the gradient
 *                               reconstruction
 * \param[in]     climgp        clipping coeffecient for the computation of
 *                               the gradient
 * \param[in]     extrap        coefficient for extrapolation of the gradient
 * \param[in]     frcxt         body force creating the hydrostatic pressure
 * \param[in]     pvar          solved variable (pressure)
 * \param[in]     coefap        boundary condition array for the variable
 *                               (explicit part)
 * \param[in]     coefbp        boundary condition array for the variable
 *                               (implicit part)
 * \param[in]     cofafp        boundary condition array for the diffusion
 *                               of the variable (explicit part)
 * \param[in]     cofbfp        boundary condition array for the diffusion
 *                               of the variable (implicit part)
 * \param[in]     i_visc        \f$ \mu_\fij \dfrac{S_\fij}{\ipf \jpf} \f$
 *                               at interior faces for the r.h.s.
 * \param[in]     b_visc        \f$ \mu_\fib \dfrac{S_\fib}{\ipf \centf} \f$
 *                               at border faces for the r.h.s.
 * \param[in]     viscel        symmetric cell tensor \f$ \tens{\mu}_\celli \f$
 * \param[in]     weighf        internal face weight between cells i j in case
 *                               of tensor diffusion
 * \param[in]     weighb        boundary face weight for cells i in case
 *                               of tensor diffusion
 * \param[in,out] diverg        divergence of the mass flux
 */
/*----------------------------------------------------------------------------*/

void
cs_anisotropic_diffusion_potential(const int                 f_id,
                                   const cs_mesh_t          *m,
                                   cs_mesh_quantities_t     *fvq,
                                   int                       init,
                                   int                       inc,
                                   int                       imrgra,
                                   int                       iccocg,
                                   int                       nswrgp,
                                   int                       imligp,
                                   int                       ircflp,
                                   int                       iphydp,
                                   int                       iwarnp,
                                   double                    epsrgp,
                                   double                    climgp,
                                   double                    extrap,
                                   cs_real_3_t     *restrict frcxt,
                                   cs_real_t       *restrict pvar,
                                   const cs_real_t           coefap[],
                                   const cs_real_t           coefbp[],
                                   const cs_real_t           cofafp[],
                                   const cs_real_t           cofbfp[],
                                   const cs_real_t           i_visc[],
                                   const cs_real_t           b_visc[],
                                   cs_real_6_t     *restrict viscel,
                                   const cs_real_2_t         weighf[],
                                   const cs_real_t           weighb[],
                                   cs_real_t       *restrict diverg)
{
  const cs_halo_t  *halo = m->halo;

  const cs_lnum_t n_cells = m->n_cells;
  const cs_lnum_t n_cells_ext = m->n_cells_with_ghosts;
  const int n_i_groups = m->i_face_numbering->n_groups;
  const int n_i_threads = m->i_face_numbering->n_threads;
  const int n_b_groups = m->b_face_numbering->n_groups;
  const int n_b_threads = m->b_face_numbering->n_threads;
  const cs_lnum_t *restrict i_group_index = m->i_face_numbering->group_index;
  const cs_lnum_t *restrict b_group_index = m->b_face_numbering->group_index;

  const cs_lnum_2_t *restrict i_face_cells
    = (const cs_lnum_2_t *restrict)m->i_face_cells;
  const cs_lnum_t *restrict b_face_cells
    = (const cs_lnum_t *restrict)m->b_face_cells;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)fvq->cell_cen;
  const cs_real_3_t *restrict i_face_normal
    = (const cs_real_3_t *restrict)fvq->i_face_normal;
  const cs_real_3_t *restrict b_face_normal
    = (const cs_real_3_t *restrict)fvq->b_face_normal;
  const cs_real_3_t *restrict i_face_cog
    = (const cs_real_3_t *restrict)fvq->i_face_cog;
  const cs_real_3_t *restrict b_face_cog
    = (const cs_real_3_t *restrict)fvq->b_face_cog;

  /* Local variables */

  char var_name[32];
  int tr_dim = 0;
  int w_stride = 6;

  bool recompute_cocg = (iccocg) ? true : false;

  double diippf[3], djjppf[3];

  cs_real_6_t *viscce;
  cs_real_6_t *w2;
  cs_real_3_t *grad;
  cs_field_t *f;

  cs_real_t *gweight = NULL;

  /*==========================================================================
    1. Initialization
    ==========================================================================*/

  if (init >= 1) {
#   pragma omp parallel for
    for (cs_lnum_t ii = 0; ii < n_cells_ext; ii++) {
      diverg[ii] = 0.;
    }
  } else if (init == 0 && n_cells_ext > n_cells) {
#   pragma omp parallel for if(n_cells_ext - n_cells > CS_THR_MIN)
    for (cs_lnum_t ii = n_cells+0; ii < n_cells_ext; ii++) {
      diverg[ii] = 0.;
    }
  } else if (init != 0) {
    bft_error(__FILE__, __LINE__, 0,
              _("invalid value of init"));
  }

  /* Use iterative gradient */

  cs_halo_type_t halo_type = CS_HALO_STANDARD;
  cs_gradient_type_t gradient_type = CS_GRADIENT_ITER;

  if (imrgra < 0)
    imrgra = 0;

  cs_gradient_type_by_imrgra(imrgra,
                             &gradient_type,
                             &halo_type);

  if (f_id != -1) {
    f = cs_field_by_id(f_id);
    snprintf(var_name, 31, "%s", f->name);
  }
  else
    strcpy(var_name, "Work array");
  var_name[31] = '\0';

  /* Porosity fields */
  cs_field_t *fporo = cs_field_by_name_try("porosity");
  cs_field_t *ftporo = cs_field_by_name_try("tensorial_porosity");

  cs_real_t *porosi = NULL;
  cs_real_6_t *porosf = NULL;

  if (cs_glob_porous_model == 1 || cs_glob_porous_model == 2) {
    porosi = fporo->val;
    if (ftporo != NULL) {
      porosf = (cs_real_6_t *)ftporo->val;
    }
  }

  /* Handle parallelism and periodicity */

  if (halo != NULL)
    cs_halo_sync_var(halo, halo_type, pvar);

  /*==========================================================================
    2. Update mass flux without reconstruction technics
    ==========================================================================*/

  if (nswrgp <= 1) {

    /* Mass flow through interior faces */

    for (int g_id = 0; g_id < n_i_groups; g_id++) {
#     pragma omp parallel for
      for (int t_id = 0; t_id < n_i_threads; t_id++) {
        for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
             face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = i_face_cells[face_id][0];
          cs_lnum_t jj = i_face_cells[face_id][1];

          double flux = i_visc[face_id]*(pvar[ii] - pvar[jj]);
          diverg[ii] += flux;
          diverg[jj] -= flux;

        }
      }
    }

    /* Mass flow though boundary faces */

    for (int g_id = 0; g_id < n_b_groups; g_id++) {
#     pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
      for (int t_id = 0; t_id < n_b_threads; t_id++) {
        for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
             face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = b_face_cells[face_id];
          double pfac = inc*cofafp[face_id] + cofbfp[face_id]*pvar[ii];

          double flux = b_visc[face_id]*pfac;
          diverg[ii] += flux;

        }
      }
    }

  }

  /*==========================================================================
    3. Update mass flux with reconstruction technics
    ==========================================================================*/

  if (nswrgp > 1) {

    viscce = NULL;
    w2 = NULL;

    /* Without porosity */
    if (porosi == NULL) {
      viscce = viscel;

      /* With porosity */
    } else if (porosi != NULL && porosf == NULL) {
      BFT_MALLOC(w2, n_cells_ext, cs_real_6_t);
      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
        for (int isou = 0; isou < 6; isou++) {
          w2[cell_id][isou] = porosi[cell_id]*viscel[cell_id][isou];
        }
      }
      viscce = w2;

      /* With tensorial porosity */
    } else if (porosi != NULL && porosf != NULL) {
      BFT_MALLOC(w2, n_cells_ext, cs_real_6_t);
      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
        cs_math_sym_33_product(porosf[cell_id],
                               viscel[cell_id],
                               w2[cell_id]);
      }
      viscce = w2;
    }

    /* ---> Periodicity and parallelism treatment of symmetric tensors */

    if (halo != NULL) {
      cs_halo_sync_var_strided(halo, CS_HALO_STANDARD, (cs_real_t *)viscce, 6);

      if (m->n_init_perio > 0)
        cs_halo_perio_sync_var_sym_tens(halo,
                                        CS_HALO_STANDARD,
                                        (cs_real_t *)viscce);
    }

    /* Allocate a work array for the gradient calculation */
    BFT_MALLOC(grad, n_cells_ext, cs_real_3_t);

    /* Compute gradient */
    if (f_id != -1) {
      /* Get the calculation option from the field */
      int key_cal_opt_id = cs_field_key_id("var_cal_opt");
      cs_var_cal_opt_t var_cal_opt;
      cs_field_get_key_struct(f, key_cal_opt_id, &var_cal_opt);
      if (f->type & CS_FIELD_VARIABLE && var_cal_opt.iwgrec == 1) {
        if (var_cal_opt.idifft > 0) {
          int key_id = cs_field_key_id("gradient_weighting_id");
          int diff_id = cs_field_get_key_int(f, key_id);
          if (diff_id > -1) {
            cs_field_t *weight_f = cs_field_by_id(diff_id);
            gweight = weight_f->val;
            w_stride = weight_f->dim;
          }
        }
      }
    }

    /* Compute gradient */
    cs_gradient_scalar(var_name,
                       gradient_type,
                       halo_type,
                       inc,
                       recompute_cocg,
                       nswrgp,
                       tr_dim,
                       iphydp,
                       w_stride,
                       iwarnp,
                       imligp,
                       epsrgp,
                       extrap,
                       climgp,
                       frcxt,
                       coefap,
                       coefbp,
                       pvar,
                       gweight, /* Weighted gradient */
                       grad);

    /* Mass flow through interior faces */

    for (int g_id = 0; g_id < n_i_groups; g_id++) {
#     pragma omp parallel for
      for (int t_id = 0; t_id < n_i_threads; t_id++) {
        for (cs_lnum_t face_id = i_group_index[(t_id*n_i_groups + g_id)*2];
             face_id < i_group_index[(t_id*n_i_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = i_face_cells[face_id][0];
          cs_lnum_t jj = i_face_cells[face_id][1];

          double pi = pvar[ii];
          double pj = pvar[jj];

          /* Recompute II" and JJ"
             ----------------------*/

          cs_real_t visci[3][3], viscj[3][3];

          visci[0][0] = viscce[ii][0];
          visci[1][1] = viscce[ii][1];
          visci[2][2] = viscce[ii][2];
          visci[1][0] = viscce[ii][3];
          visci[0][1] = viscce[ii][3];
          visci[2][1] = viscce[ii][4];
          visci[1][2] = viscce[ii][4];
          visci[2][0] = viscce[ii][5];
          visci[0][2] = viscce[ii][5];

          /* IF.Ki.S / ||Ki.S||^2 */
          cs_real_t fikdvi = weighf[face_id][0];

          /* II" = IF + FI" */
          for (int i = 0; i < 3; i++) {
            diippf[i] = i_face_cog[face_id][i]-cell_cen[ii][i]
                      - fikdvi*( visci[0][i]*i_face_normal[face_id][0]
                               + visci[1][i]*i_face_normal[face_id][1]
                               + visci[2][i]*i_face_normal[face_id][2] );
          }

          viscj[0][0] = viscce[jj][0];
          viscj[1][1] = viscce[jj][1];
          viscj[2][2] = viscce[jj][2];
          viscj[1][0] = viscce[jj][3];
          viscj[0][1] = viscce[jj][3];
          viscj[2][1] = viscce[jj][4];
          viscj[1][2] = viscce[jj][4];
          viscj[2][0] = viscce[jj][5];
          viscj[0][2] = viscce[jj][5];

          /* FJ.Kj.S / ||Kj.S||^2 */
          double fjkdvi = weighf[face_id][1];

          /* JJ" = JF + FJ" */
          for (int i = 0; i < 3; i++) {
            djjppf[i] = i_face_cog[face_id][i]-cell_cen[jj][i]
                      + fjkdvi*( viscj[0][i]*i_face_normal[face_id][0]
                               + viscj[1][i]*i_face_normal[face_id][1]
                               + viscj[2][i]*i_face_normal[face_id][2] );
          }

          /* p in I" and J" */
          double pipp = pi + ircflp*( grad[ii][0]*diippf[0]
                             + grad[ii][1]*diippf[1]
                             + grad[ii][2]*diippf[2]);
          double pjpp = pj + ircflp*( grad[jj][0]*djjppf[0]
                             + grad[jj][1]*djjppf[1]
                             + grad[jj][2]*djjppf[2]);

          double flux = i_visc[face_id]*(pipp - pjpp);

          diverg[ii] += flux;
          diverg[jj] -= flux;

        }
      }
    }

    /* Mass flow though boundary faces */

    for (int g_id = 0; g_id < n_b_groups; g_id++) {
#     pragma omp parallel for if(m->n_b_faces > CS_THR_MIN)
      for (int t_id = 0; t_id < n_b_threads; t_id++) {
        for (cs_lnum_t face_id = b_group_index[(t_id*n_b_groups + g_id)*2];
             face_id < b_group_index[(t_id*n_b_groups + g_id)*2 + 1];
             face_id++) {

          cs_lnum_t ii = b_face_cells[face_id];

          double pi = pvar[ii];

          /* Recompute II"
             --------------*/

          cs_real_t visci[3][3];

          visci[0][0] = viscce[ii][0];
          visci[1][1] = viscce[ii][1];
          visci[2][2] = viscce[ii][2];
          visci[1][0] = viscce[ii][3];
          visci[0][1] = viscce[ii][3];
          visci[2][1] = viscce[ii][4];
          visci[1][2] = viscce[ii][4];
          visci[2][0] = viscce[ii][5];
          visci[0][2] = viscce[ii][5];

          /* IF.Ki.S / ||Ki.S||^2 */
          double fikdvi = weighb[face_id];

          /* II" = IF + FI" */
          for (int i = 0; i < 3; i++) {
            diippf[i] = b_face_cog[face_id][i] - cell_cen[ii][i]
                      - fikdvi*( visci[0][i]*b_face_normal[face_id][0]
                               + visci[1][i]*b_face_normal[face_id][1]
                               + visci[2][i]*b_face_normal[face_id][2] );
          }

          double pipp = pi + ircflp*(  grad[ii][0]*diippf[0]
                                     + grad[ii][1]*diippf[1]
                                     + grad[ii][2]*diippf[2]);


          double pfac = inc*cofafp[face_id] + cofbfp[face_id]*pipp;

          double flux = b_visc[face_id]*pfac;
          diverg[ii] += flux;

        }
      }
    }

    /* Free memory */
    BFT_FREE(grad);
    BFT_FREE(w2);

  }

}

/*----------------------------------------------------------------------------*/

END_C_DECLS
