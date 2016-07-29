/*============================================================================
 * Radiation solver main subroutine.
 *============================================================================*/

/* This file is part of Code_Saturne, a general-purpose CFD tool.

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
#include "cs_math.h"

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

#include "cs_boundary_conditions.h"
#include "cs_field.h"
#include "cs_field_pointer.h"
#include "cs_gui_util.h"
#include "cs_log.h"
#include "cs_mesh.h"
#include "cs_parall.h"
#include "cs_parameters.h"
#include "cs_thermal_model.h"
#include "cs_prototypes.h"
#include "cs_equation_iterative_solve.h"
#include "cs_gradient.h"
#include "cs_face_viscosity.h"
#include "cs_physical_constants.h"
#include "cs_physical_model.h"
#include "cs_sles.h"
#include "cs_sles_it.h"

#include "cs_gui_radiative_transfer.h"
#include "cs_rad_transfer.h"
#include "cs_rad_transfer_absorption.h"
#include "cs_rad_transfer_pun.h"
#include "cs_rad_transfer_bcs.h"

/*----------------------------------------------------------------------------
 *  Header for the current file
 *----------------------------------------------------------------------------*/

#include "cs_rad_transfer_solve.h"

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*=============================================================================
 * Additional Doxygen documentation
 *============================================================================*/

/*  \file cs_rad_transfer_solve.c */

/*! \cond DOXYGEN_SHOULD_SKIP_THIS */

static int ipadom = 0;
static cs_real_t *wq = NULL;

/*=============================================================================
 * Local Macro Definitions
 *============================================================================*/

/*=============================================================================
 * Local type definitions
 *============================================================================*/

/*============================================================================
 * Public function definitions for fortran API
 *============================================================================*/

/*============================================================================
 * Private function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------
 * Descend binary tree for the lexicographical ordering of axis coordinates.
 *
 * parameters:
 *   s     <-- axial coordinate
 *   level <-- level of the binary tree to descend
 *   n     <-- number of entities in the binary tree to descend
 *   order <-> ordering array
 *----------------------------------------------------------------------------*/

inline static void
_order_axis_descend_tree(const cs_real_t   s[],
                         size_t            level,
                         const size_t      n,
                         cs_lnum_t         order[])
{
  const cs_real_t eps = 1e-24;

  size_t i_save, i1, i2, lv_cur;

  i_save = (size_t)(order[level]);

  while (level <= (n/2)) {

    lv_cur = (2*level) + 1;

    if (lv_cur < n - 1) {

      i1 = (size_t)(order[lv_cur+1]);
      i2 = (size_t)(order[lv_cur]);

      if (s[i1] > s[i2])
        lv_cur++;
      else if (s[i1] + eps > s[i2] && i1 > i2)
        lv_cur++;

    }

    if (lv_cur >= n) break;

    i1 = i_save;
    i2 = (size_t)(order[lv_cur]);

    if (s[i1] > s[i2])
      break;
    if (s[i1] + eps >= s[i2] && i1 >= i2)
      break;

    order[level] = order[lv_cur];
    level = lv_cur;

  }

  order[level] = i_save;
}

/*----------------------------------------------------------------------------
 * Order cells based on an axis coordinate and original numbering.
 *
 * parameters:
 *   s     <-- axial coordinate
 *   order --> pointer to pre-allocated ordering table
 *   n     <-- number of entities considered
 *----------------------------------------------------------------------------*/

static void
_order_axis(const cs_real_t  s[],
            cs_lnum_t        order[],
            const size_t     n)
{
  size_t i;
  cs_lnum_t o_save;

  /* Initialize ordering array */

  for (i = 0; i < n; i++)
    order[i] = i;

  if (n < 2)
    return;

  /* Create binary tree */

  i = (n / 2);
  do {
    i--;
    _order_axis_descend_tree(s, i, n, order);
  } while (i > 0);

  /* Sort binary tree */

  for (i = n - 1; i > 0; i--) {
    o_save   = order[0];
    order[0] = order[i];
    order[i] = o_save;
    _order_axis_descend_tree(s, 0, i, order);
  }

}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Order linear solvers for DOM radiative model.
 */
/*----------------------------------------------------------------------------*/

static void
_order_by_direction(void)
{
  const cs_mesh_t  *m = cs_glob_mesh;
  const cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  const cs_lnum_t n_cells = m->n_cells;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)fvq->cell_cen;

  int kdir = 0;

  cs_real_t *s;
  BFT_MALLOC(s, n_cells, cs_real_t);

  for (int ii = -1; ii < 2; ii+=2) {
    for (int jj = -1; jj < 2; jj+=2) {
      for (int kk = -1; kk < 2; kk+=2) {
        for (int idir = 0; idir < cs_glob_rad_transfer_params->ndirs; idir++) {

          cs_real_t v[3] = {ii*cs_glob_rad_transfer_params->sxyz[idir][0],
                            jj*cs_glob_rad_transfer_params->sxyz[idir][1],
                            kk*cs_glob_rad_transfer_params->sxyz[idir][2]};

          kdir = kdir + 1;

          char name[32];
          sprintf(name, "radiation_%03d", kdir);

          cs_sles_t *sles = cs_sles_find(-1, name);

          if (sles == NULL) {
            (void)cs_sles_it_define(-1,
                                    name,
                                    CS_SLES_P_GAUSS_SEIDEL,
                                    0,      /* poly_degree */
                                    1000);  /* n_max_iter */
            sles = cs_sles_find(-1, name);
          }

          if (strcmp(cs_sles_get_type(sles), "cs_sles_it_t") != 0)
            continue;

          cs_sles_it_t *sc = cs_sles_get_context(sles);

          for (cs_lnum_t c_id = 0; c_id < n_cells; c_id++)
            s[c_id] =   v[0]*cell_cen[c_id][0]
                      + v[1]*cell_cen[c_id][1]
                      + v[2]*cell_cen[c_id][2];

          cs_lnum_t *order;
          BFT_MALLOC(order, n_cells, cs_lnum_t);

          _order_axis(s, order, n_cells);

          cs_sles_it_assign_order(sc, &order); /* becomes owner of order */

        }
      }
    }
  }

  BFT_FREE(s);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Radiative flux and source term compoutation
 *
 * 1/ Luminance data at domain boundaries
 *       (BC: reflection and isotropic emission)
 *                              ->  ->           ->
 * 2/ Compute luminance L(x, s) at point x
 *                          d L
 *    by solving equation : --- = -TK.L +TS
 *                          d S
 *                              ->                o
 *    also expressed as: div (L.S ) = -TK.L + TK.L
 *                            ->   /   -> -> ->
 * 3/ Compute flux densities  Q = /  L(X, S).S domega
 *                               /4.pi
 *                                 /   -> ->
 *        and absorption      Sa= /  L(X, S). domega
 *                               /4.pi
 *    by integration of luminances over solid angles.
 *    Note: it is useful to compute the heating rate
 *    -----
 *                                        /   -> ->  ->  ->
 * 4/ Compute the incident flux Qincid = /  L(X ,S ).S . N domega
 *                                      /->->
 *       ->                            / S.N >0
 *       N fluid to wall normal
 *
 * \param[in, out]  coefap    boundary condition work array for the luminance
 *                             (explicit part)
 * \param[in, out]  coefbp    boundary condition work array for the luminance
 *                             (implicit part)
 * \param[in, out]  cofafp    boundary condition work array for the diffusion
 *                             of the luminance (explicit part)
 * \param[in, out]  cofbfp    boundary condition work array for the diffusion
 *                             of the luminance (implicit part)
 * \param[in, out]  flurds    pseudo mass flux work array (interior faces)
 * \param[in, out]  flurdb    pseudo mass flux work array (boundary faces)
 * \param[in, out]  viscf     visc*surface/dist work array at interior faces
 * \param[in, out]  viscb     visc*surface/dist work array at boundary faces
 * \param[in, out]  smbrs     work array for RHS
 * \param[in, out]  rovsdt    work array for unsteady term
 * \param[out]      q         explicit flux density vector
 * \param[in]       iband     number of the i-th gray gas
 */
/*----------------------------------------------------------------------------*/

static void
_cs_rad_transfer_sol(cs_real_t    *restrict coefap,
                     cs_real_t    *restrict coefbp,
                     cs_real_t    *restrict cofafp,
                     cs_real_t    *restrict cofbfp,
                     cs_real_t    *restrict flurds,
                     cs_real_t    *restrict flurdb,
                     cs_real_t    *restrict viscf,
                     cs_real_t    *restrict viscb,
                     cs_real_t    *restrict smbrs,
                     cs_real_t    *restrict rovsdt,
                     cs_real_3_t  *restrict q,
                     int           iband)
{
  cs_lnum_t n_b_faces = cs_glob_mesh->n_b_faces;
  cs_lnum_t n_i_faces  = cs_glob_mesh->n_i_faces;
  cs_lnum_t n_cells_ext = cs_glob_mesh->n_cells_with_ghosts;
  cs_lnum_t n_cells   = cs_glob_mesh->n_cells;

  cs_real_3_t *surfbo = (cs_real_3_t *)cs_glob_mesh_quantities->b_face_normal;
  cs_real_3_t *surfac = (cs_real_3_t *)cs_glob_mesh_quantities->i_face_normal;
  cs_real_t   *surfbn = cs_glob_mesh_quantities->b_face_surf;

  cs_field_t *f_qincid = cs_field_by_name("rad_incident_flux");
  cs_field_t *f_sa = cs_field_by_name("rad_st");
  cs_field_t *f_snplus = cs_field_by_name("rad_net_flux");

  /* Allocate work arrays */

  cs_real_t *rhs0, *dpvar, *ru, *rua;
  BFT_MALLOC(rhs0,  n_cells_ext, cs_real_t);
  BFT_MALLOC(dpvar, n_cells_ext, cs_real_t);
  BFT_MALLOC(ru,    n_cells_ext, cs_real_t);
  BFT_MALLOC(rua,   n_cells_ext, cs_real_t);

  /* Initialization */

  cs_field_t *f_qinspe;
  if (cs_glob_rad_transfer_params->imoadf >= 1)
    /* Pointer to the spectral flux density field */
    f_qinspe = cs_field_by_name_try("spectral_rad_incident_flux");

  cs_var_cal_opt_t vcopt = cs_parameters_var_cal_opt_default();

  vcopt.iwarni =  cs_glob_rad_transfer_params->iimlum;
  vcopt.iconv  =  1;
  vcopt.istat  = -1;
  vcopt.idiff  =  0; /* no face diffusion */
  vcopt.idifft = -1;
  vcopt.isstpc =  0;
  vcopt.nswrsm =  2;//FIXME useless
  vcopt.imrgra =  cs_glob_space_disc->imrgra;
  vcopt.blencv =  0;
  vcopt.epsrsm =  1e-08;  /* TODO: try with default (1e-07) */

  int iescap = 0;
  int imucpp = 0;

  /* There are Dirichlet BCs */
  int ndirc1 = 1;

  /* Pure convection */
  vcopt.iconv = 1;

  if (cs_glob_time_step->nt_cur == cs_glob_time_step->nt_prev + 1)
    _order_by_direction();

  /*                              / -> ->
   * Correct BCs to ensure : pi= /  s. n domega
   *                            /2PI
   */

  for (cs_lnum_t face_id = 0; face_id < n_b_faces; face_id++)
    f_snplus->val[face_id] = 0.0;

  cs_real_3_t sxyzt;
  cs_real_t domegat, aa;
  for (int ii = -1; ii <= 1; ii+=2) {
    for (int jj = -1; jj <= 1; jj+=2) {
      for (int kk = -1; kk <= 1; kk+=2) {

        for (int idir = 0; idir < cs_glob_rad_transfer_params->ndirs; idir++) {
          sxyzt[0] = ii * cs_glob_rad_transfer_params->sxyz[idir][0];
          sxyzt[1] = jj * cs_glob_rad_transfer_params->sxyz[idir][1];
          sxyzt[2] = kk * cs_glob_rad_transfer_params->sxyz[idir][2];
          domegat = cs_glob_rad_transfer_params->angsol[idir];
          for (cs_lnum_t face_id = 0; face_id < n_b_faces; face_id++) {
            aa =  sxyzt[0] * surfbo[face_id][0]
                + sxyzt[1] * surfbo[face_id][1]
                + sxyzt[2] * surfbo[face_id][2];
            aa /= surfbn[face_id];
            f_snplus->val[face_id] += 0.5 * ( -aa + CS_ABS(aa)) * domegat;
          }
        }

      }
    }
  }

  for (cs_lnum_t face_id = 0; face_id < n_b_faces; face_id++) {
    coefap[face_id] *= cs_math_pi / f_snplus->val[face_id];
    cofafp[face_id] *= cs_math_pi / f_snplus->val[face_id];
  }

  /* initialization for integration in following loops */

  for (cs_lnum_t face_id = 0; face_id < n_b_faces; face_id++) {
    f_qincid->val[face_id] = 0.0;
    f_snplus->val[face_id] = 0.0;
    if (cs_glob_rad_transfer_params->imoadf >= 1)
      f_qinspe->val[iband + face_id * cs_glob_rad_transfer_params->nwsgg] = 0.0;
  }

  for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++) {
    f_sa->val[cell_id] = 0.0;
    q[cell_id][0] = 0.0;
    q[cell_id][1] = 0.0;
    q[cell_id][2] = 0.0;
  }

  /* Save smbrs in buffer, reload at each change of direction */

  for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
    rhs0[cell_id] = smbrs[cell_id];

  /* rovsdt loaded once only */

  for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
    rovsdt[cell_id] = CS_MAX(rovsdt[cell_id], 0.0);

  /* Angular discretization */

  int kdir = 0;

  for (int ii = -1; ii <= 1; ii+=2) {
    for (int jj = -1; jj <= 1; jj+=2) {
      for (int kk = -1; kk <= 1; kk+=2) {

        for (int idir = 0; idir < cs_glob_rad_transfer_params->ndirs; idir++) {
          sxyzt[0] = ii * cs_glob_rad_transfer_params->sxyz[idir][0];
          sxyzt[1] = jj * cs_glob_rad_transfer_params->sxyz[idir][1];
          sxyzt[2] = kk * cs_glob_rad_transfer_params->sxyz[idir][2];
          domegat = cs_glob_rad_transfer_params->angsol[idir];
          kdir++;

          char    cnom[80];
          snprintf(cnom, 80, "%s%03d", "radiation_", kdir);

          /* Spatial discretization */

          /* Explicit source term */

          for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
            smbrs[cell_id] = rhs0[cell_id];

          /* Implicit source term (rovsdt seen above) */

          for (cs_lnum_t face_id = 0; face_id < n_i_faces; face_id++)
            viscf[face_id] = 0.0;

          for (cs_lnum_t face_id = 0; face_id < n_b_faces; face_id++)
            viscb[face_id] = 0.0;

          for (cs_lnum_t cell_id = 0; cell_id < n_cells_ext; cell_id++) {
            ru[cell_id]  = 0.0;
            rua[cell_id] = 0.0;
          }

          for (cs_lnum_t face_id = 0; face_id < n_i_faces; face_id++)
            flurds[face_id] =  sxyzt[0] * surfac[face_id][0]
                             + sxyzt[1] * surfac[face_id][1]
                             + sxyzt[2] * surfac[face_id][2];

          for (cs_lnum_t face_id = 0; face_id < n_b_faces; face_id++)
            flurdb[face_id] =  sxyzt[0] * surfbo[face_id][0]
                             + sxyzt[1] * surfbo[face_id][1]
                             + sxyzt[2] * surfbo[face_id][2];

          /* Resolution
             ---------- */

          /* In case of a theta-scheme, set theta = 1;
             no relaxation in steady case either */

          /* All boundary convective fluxes with upwind */
          int icvflb = 0;

          cs_equation_iterative_solve_scalar(0,   /* idtvar */
                                             -1,  /* f_id */
                                             cnom,
                                             ndirc1,
                                             iescap,
                                             imucpp,
                                             &vcopt,
                                             rua,
                                             ru,
                                             coefap,
                                             coefbp,
                                             cofafp,
                                             cofbfp,
                                             flurds,
                                             flurdb,
                                             viscf,
                                             viscb,
                                             viscf,
                                             viscb,
                                             NULL,
                                             NULL,
                                             NULL,
                                             icvflb,
                                             NULL,
                                             rovsdt,
                                             smbrs,
                                             ru,
                                             dpvar,
                                             NULL,
                                             NULL);

          /* Integration of fluxes and source terms */

          for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
            aa = ru[cell_id] * domegat;
            f_sa->val[cell_id]  += aa;
            q[cell_id][0] += aa * sxyzt[0];
            q[cell_id][1] += aa * sxyzt[1];
            q[cell_id][2] += aa * sxyzt[2];
          }

          /* Flux incident to wall */

          for (cs_lnum_t face_id = 0; face_id < n_b_faces; face_id++) {
            aa =  sxyzt[0] * surfbo[face_id][0]
                + sxyzt[1] * surfbo[face_id][1]
                + sxyzt[2] * surfbo[face_id][2];
            aa /= surfbn[face_id];
            aa = 0.5 * (aa + CS_ABS(aa)) * domegat;
            f_snplus->val[face_id] += aa;
            if (cs_glob_rad_transfer_params->imoadf >= 1)
              f_qinspe->val[iband + face_id * cs_glob_rad_transfer_params->nwsgg]
                += aa * ru[cs_glob_mesh->b_face_cells[face_id]];

            else
              f_qincid->val[face_id]
                += aa * ru[cs_glob_mesh->b_face_cells[face_id]];

          }
        }

      }
    }
  }

  /* Free memory */

  BFT_FREE(rhs0);
  BFT_FREE(dpvar);
  BFT_FREE(ru);
  BFT_FREE(rua);
}

/*-------------------------------------------------------------------------------*/
/*!
 * \brief Compute the net radiation flux.
 *
 * The density of net radiation flux must be calculated
 * consistently with the boundary conditions of the intensity.
 * The density of net flux is the balance between the radiative
 * emiting part of a boudary face (and not the reflecting one)
 * and the radiative absorbing part.
 *
 * \param[in]   bc_type   boundary face types
 * \param[in]   izfrdp    boundary faces -> zone number
 * \param[in]   dt        time step (per cell)
 * \param[in]   coefap    boundary condition work array for the luminance
 *                        (explicit part)
 * \param[in]   coefbp    boundary condition work array for the luminance
 *                       (implicit part)
 * \param[in]   twall     inside current wall temperature (K)
 * \param[in]   qincid    radiative incident flux  (W/m2)
 * \param[in]   xlamp     conductivity (W/m/K)
 * \param[in]   epap      thickness (m)
 * \param[in]   epsp      emissivity (>0)
 * \param[in]   ck        absorption coefficient
 * \param[out]  net_flux  net flux (W/m2)
 */
/*-------------------------------------------------------------------------------*/

static void
_compute_net_flux(const int        itypfb[],
                  const int        izfrdp[],
                  const cs_real_t  dt[],
                  const cs_real_t  coefap[],
                  const cs_real_t  coefbp[],
                  const cs_real_t  twall[],
                  const cs_real_t  qincid[],
                  const cs_real_t  xlam[],
                  const cs_real_t  epa[],
                  const cs_real_t  eps[],
                  const cs_real_t  ck[],
                  cs_real_t        net_flux[])
{
  cs_real_t  stephn = 5.6703e-8;
  cs_real_t  xmissing = -cs_math_big_r * 0.2;

  /* Initializations */

  /* Stop indicator (forgotten boundary faces)     */
  int iok = 0;

  /* Net flux dendity for the boundary faces
   * The provided examples are sufficient in most of cases.*/

  /* If the boundary conditions given above have been modified
   *   it is necessary to change the way in which density is calculated from
   *   the net radiative flux consistently.*/

  /* The rule is:
   *   the density of net flux is a balance between the emitting energy from a
   *   boundary face (and not the reflecting energy) and the absorbing radiative
   *   energy. Therefore if a wall heats the fluid by radiative transfer, the
   *   net flux is negative */

  for (cs_lnum_t ifac = 0; ifac < cs_glob_mesh->n_b_faces; ifac++) {

    /* Wall faces */
    if (   itypfb[ifac] == CS_SMOOTHWALL
        || itypfb[ifac] == CS_ROUGHWALL)
      net_flux[ifac] = eps[ifac] * (qincid[ifac] - stephn * pow(twall[ifac], 4));

    /* Symmetry   */
    else if (itypfb[ifac] == CS_SYMMETRY)
      net_flux[ifac] = 0.0;

    /* Inlet/Outlet    */
    else if (   itypfb[ifac] == CS_INLET
             || itypfb[ifac] == CS_CONVECTIVE_INLET
             || itypfb[ifac] == CS_OUTLET
             || itypfb[ifac] == CS_FREE_INLET) {
      if (cs_glob_rad_transfer_params->iirayo == 1)
        net_flux[ifac] = qincid[ifac] - cs_math_pi * coefap[ifac];
      else if (cs_glob_rad_transfer_params->iirayo == 2)
        net_flux[ifac] = 0.0;
    }

    /* Set "impossible" value for other faces, so that if this
       is not overwritten in \ref cs_user_rad_transfer_net_flux,
       top if there are forgotten faces   */
    else
      net_flux[ifac] = xmissing;

  }
}

/*! (DOXYGEN_SHOULD_SKIP_THIS) \endcond */

/*============================================================================
 * Public function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief Solve the radiative transfer equation.
 *
 *  Two types of method are available:
 *  - Discretes Ordinates Methods (DOM)
 *  - P-1 approximation (only recommended for pulverized coal)
 *
 *  \param[in, out]  bc_type       boundary face types
 *  \param[in]       izfrad        zone number of boundary faces
 *  \param[in]       nclacp        number of pulverized coal classes
 *  \param[in]       nclafu        number of fuel classes
 *  \param[in]       dt            time step (per cell)
 *  \param[in]       cp2fol        fuel oil liquid CP
 *  \param[in]       cp2ch         pulverized coal CP's
 *  \param[in]       ichcor        pulverized coal indirection
 */
/*----------------------------------------------------------------------------*/

void
cs_rad_transfer_solve(int               bc_type[],
                      const int         izfrad[],
                      int               nclacp,
                      int               nclafu,
                      const cs_real_t   dt[],
                      cs_real_t         cp2fol,
                      const cs_real_t   cp2ch[],
                      const int         ichcor[])
{
  /* Shorter notation */
  cs_rad_transfer_params_t *rt_params = cs_glob_rad_transfer_params;

  int nwsgg = rt_params->nwsgg;

  /* Physical constants */
  cs_real_t tkelvi = 273.15;
  cs_real_t c_stefan = 5.6703e-8;

  /* Mesh params */
  cs_lnum_t n_cells     = cs_glob_mesh->n_cells;
  cs_lnum_t n_cells_ext = cs_glob_mesh->n_cells_with_ghosts;
  cs_lnum_t n_b_faces   = cs_glob_mesh->n_b_faces;
  cs_lnum_t n_i_faces   = cs_glob_mesh->n_i_faces;

  cs_real_3_t *surfbo = (cs_real_3_t *)cs_glob_mesh_quantities->b_face_normal;
  const cs_real_t *b_face_surf = cs_glob_mesh_quantities->b_face_surf;
  const cs_real_t *cell_vol = cs_glob_mesh_quantities->cell_vol;

  char fname[80];

  /* Number of passes */
  ipadom++;

  /* Allocate temporary arrays for the radiative equations resolution */
  cs_real_t *viscf, *viscb, *smbrs, *rovsdt;
  BFT_MALLOC(viscf,  n_i_faces, cs_real_t);
  BFT_MALLOC(viscb,  n_b_faces, cs_real_t);
  BFT_MALLOC(smbrs,  n_cells_ext, cs_real_t);
  BFT_MALLOC(rovsdt, n_cells_ext, cs_real_t);

  /* Allocate specific arrays for the radiative transfer module */
  cs_real_t *tempk, *coefap, *coefbp, *cofafp, *cofbfp, *flurds, *flurdb;
  BFT_MALLOC(tempk,  n_cells_ext * rt_params->nrphas, cs_real_t);
  BFT_MALLOC(coefap, n_b_faces, cs_real_t);
  BFT_MALLOC(coefbp, n_b_faces, cs_real_t);
  BFT_MALLOC(cofafp, n_b_faces, cs_real_t);
  BFT_MALLOC(cofbfp, n_b_faces, cs_real_t);
  BFT_MALLOC(flurds, n_i_faces, cs_real_t);
  BFT_MALLOC(flurdb, n_b_faces, cs_real_t);

  /* Allocate work arrays */
  /* Absorption coeffcient of the bulk phase  */
  cs_real_t *ckmel;
  BFT_MALLOC(ckmel, n_cells_ext, cs_real_t);

  /* Specific heat capacity of the bulk phase */
  cs_real_t *dcp, *tparo;
  BFT_MALLOC(dcp, n_cells_ext, cs_real_t);
  BFT_MALLOC(tparo, n_b_faces, cs_real_t);

  /* Map field arrays */
  cs_field_t *f_tempb = CS_F_(t_b);
  cs_field_t *f_qinci = CS_F_(qinci);
  cs_field_t *f_xlam  = CS_F_(xlam);
  cs_field_t *f_epa   = CS_F_(epa);
  cs_field_t *f_eps   = CS_F_(emissivity);
  cs_field_t *f_fnet  = CS_F_(fnet);

  // CAUTION FOR NEPTUNE INTEGRATION HERE
  cs_field_t *f_cp = CS_F_(cp);

  /* ADF model parameters */
  /* Irradiating spectral flux density   */
  cs_field_t *f_qinsp = NULL;
  if (   rt_params->imoadf >= 1
      || rt_params->imfsck == 1)
    f_qinsp = cs_field_by_name("spectral_rad_incident_flux");

  /* Radiation coeffcient kgi and the corresponding weight
     agi of the i-th grey gas  */
  cs_real_t *kgi, *agi;
  BFT_MALLOC(kgi, n_cells_ext * nwsgg, cs_real_t);
  BFT_MALLOC(agi, n_cells_ext * nwsgg, cs_real_t);

  /* Flux density components   */
  cs_real_3_t *iqpar;
  BFT_MALLOC(iqpar, n_cells_ext, cs_real_3_t);

  /* Radiation absorbed by the gasphase and the solid phase
     (all particles classes) */
  cs_real_t *iabgaz, *iabpar;
  BFT_MALLOC(iabgaz, n_cells_ext, cs_real_t);
  BFT_MALLOC(iabpar, n_cells_ext, cs_real_t);

  /* Emmitted radtion of the gasphase and the solid phase
     (all particle classes) */
  cs_real_t *iemgex, *iempex;
  BFT_MALLOC(iemgex, n_cells_ext, cs_real_t);
  BFT_MALLOC(iempex, n_cells_ext, cs_real_t);

  /* Absorbed and emmitted radiation of a single size class (needed to
   * compute the source terms of the particle enthalpy equation) */
  cs_real_t *iabparh2, *iempexh2;
  BFT_MALLOC(iabparh2, n_cells_ext * nclacp, cs_real_t);
  BFT_MALLOC(iempexh2, n_cells_ext * nclacp, cs_real_t);

  /* Implicit source terms of the bulk phase enthalpie equation   */
  cs_real_t *iemgim, *iempim;
  BFT_MALLOC(iemgim, n_cells_ext, cs_real_t);
  BFT_MALLOC(iempim, n_cells_ext, cs_real_t);

  /* Implicit source term of the particle enthalpy equation */
  cs_real_t *iempimh2;
  BFT_MALLOC(iempimh2, n_cells_ext * nclacp, cs_real_t);

  /* Total emitted intensity   */
  cs_real_t *ilutot;
  BFT_MALLOC(ilutot, n_cells_ext, cs_real_t);

  /* Irradiating flux density at walls.
     Careful: Should not be confused with qinci */
  cs_real_t *iqpato;
  BFT_MALLOC(iqpato, n_b_faces, cs_real_t);

  /* Weight of the i-th grey gas at walls     */
  cs_real_t *agbi;
  BFT_MALLOC(agbi, n_b_faces * nwsgg, cs_real_t);

  /* Wall temperature */
  cs_real_t xptk;
  if (cs_glob_thermal_model->itpscl == 2)
    xptk = tkelvi;
  else
    xptk = 0.0;

  for (cs_lnum_t ifac = 0; ifac < n_b_faces; ifac++) {
    if (   bc_type[ifac] == CS_SMOOTHWALL
        || bc_type[ifac] == CS_ROUGHWALL)
      tparo[ifac]  = f_tempb->val[ifac] + xptk;
    else
      tparo[ifac]  = 0.0;
  }

  /* FSCK model parameters */
  if (ipadom == 1)
    /* Weight of the i-the gaussian quadrature  */
    BFT_MALLOC(wq, nwsgg, cs_real_t);

  /* Initializations
     --------------- */

  if (   ipadom > 1
      && cs_glob_time_step->nt_cur%rt_params->nfreqr != 0)
    return;

  cs_log_printf(CS_LOG_DEFAULT,
                _("   ** Information on the radiative source term\n"
                  "      ----------------------------------------\n"));

  /* Constants initialization */
  cs_real_t unspi  = 1.0 / cs_math_pi;

  cs_real_t *cpro_cak0 = CS_FI_(rad_cak, 0)->val;
  cs_real_t *cpro_ri_st0 = CS_FI_(rad_ist, 0)->val;
  cs_real_t *cpro_re_st0 = CS_FI_(rad_est, 0)->val;
  cs_real_t *cpro_abso0 = CS_FI_(rad_abs, 0)->val;
  cs_real_t *cpro_emi0  = CS_FI_(rad_emi, 0)->val;
  cs_real_t *cpro_lumin = CS_F_(rad_lumin)->val;
  cs_real_3_t *cpro_q     = (cs_real_3_t *)(CS_F_(rad_q)->val);

  /* -> Working arrays    */
  for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {

    /* Radiation coefficient k of the gas phase */
    cpro_cak0[cell_id]  = 0.0;

    /* TS implicit due to emission    */
    cpro_ri_st0[cell_id] = 0.0;

    /* TS explicit due to emission and absorption    */
    cpro_re_st0[cell_id] = 0.0;

    /* Absortion: Sum, i((kg, i+kp) * Integral(Ii)dOmega) */
    cpro_abso0[cell_id] = 0.0;

    /* Emission: Sum, i((kg, i+kp) * c_stefan * T^4 *agi)   */
    cpro_emi0[cell_id]  = 0.0;

    /* radiative flux vector     */
    cpro_q[cell_id][0] = 0.0;
    cpro_q[cell_id][1] = 0.0;
    cpro_q[cell_id][2] = 0.0;

    /* Absorption of the gas phase: kg, i * Integral(Ii) * dOmega   */
    iabgaz[cell_id] = 0.0;

    /* Absortion of particles: kp * Integral(Ii) * dOmega */
    iabpar[cell_id] = 0.0;

    /* Emission of the gas phase: kg, i * c_stefan * T^4 *agi    */
    iemgex[cell_id] = 0.0;

    /* Emission of particles: kp * c_stefan * T^4 *agi */
    iempex[cell_id] = 0.0;

    /* Gas phase related implicit source term in the bulk phase enthalpy eqn. */
    iemgim[cell_id] = 0.0;

    /* Particle related implicit source term in the bulk phase enthalpy eqn.  */
    iempim[cell_id] = 0.0;

    /* Total emitted intensity   */
    ilutot[cell_id] = 0.0;

    /* Radiation coeffcient of the bulk phase   */
    ckmel[cell_id] = 0.0;

    if (cs_glob_fluid_properties->icp > 0)
      dcp[cell_id] = 1.0 / f_cp->val[cell_id];
    else
      dcp[cell_id] = 1.0 / cs_glob_fluid_properties->cp0;

    /* In case of grey gas radiation properties (kgi!=f(lambda))    */
    /* agi must be set to 1.     */
    for (int i = 0; i < nwsgg; i++) {
      kgi[cell_id + n_cells * i]  = 0.0;
      agi[cell_id + n_cells * i]  = 1.0;
    }
  }

  for (cs_lnum_t ifac = 0; ifac < n_b_faces; ifac++) {
    iqpato[ifac] = 0.0;
    /* In case of grey gas radiation properties (kgi!=f(lambda))    */
    /* agbi must be set to 1.    */
    for (int i = 0; i < nwsgg; i++) {
      agbi[ifac + i * n_b_faces]     = 1.0;
    }
  }

  for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
    for (int icla = 0; icla < nclacp; icla++) {
      iabparh2[cell_id + n_cells * icla]    = 0.0;
      iempexh2[cell_id + n_cells * icla]    = 0.0;
      iempimh2[cell_id + n_cells * icla]    = 0.0;
    }
  }

  if (ipadom == 1) {
    /* Must be set to 1 in case of using the standard as well as */
    /* the ADF radiation models  */
    for (int i = 0; i < nwsgg; i++)
      wq[i] = 1.0;
  }

  /* Store temperature (in Kelvin) in tempk(cell_id, irphas) */

  /* --> Temperature transport */

  if (cs_glob_thermal_model->itherm == 1) {

    cs_real_t *cvara_scalt = CS_F_(t)->vals[1];

    /* temperature is in Celsius */
    if (cs_glob_thermal_model->itpscl == 2) {
      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
        tempk[cell_id] = cvara_scalt[cell_id] + tkelvi;

    }
    else {
      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
        tempk[cell_id] = cvara_scalt[cell_id];
    }

  }

  /* Enthalpy transport (flurdb is a temporary array) */

  else if (cs_glob_thermal_model->itherm == 2) {

    const cs_real_t *cvara_scalt = CS_F_(h)->vals[1];

    CS_PROCF(c_h_to_t, C_H_TO_T)(cvara_scalt, tempk);

    /* Particles temperature */
    if (cs_glob_physical_model_flag[CS_COMBUSTION_COAL] >= 0) {
      for (int icla = 0; icla < nclacp; icla++) {
        int ipcla = 1 + icla;
        snprintf(fname, 80, "t_p_%02d", icla+1);
        cs_field_t *f_temp2 = cs_field_by_name(fname);
        for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
          tempk[cell_id + n_cells * ipcla] = f_temp2->val[cell_id];
        }
      }
    }
    /* Fuel  */
    else if (cs_glob_physical_model_flag[CS_COMBUSTION_FUEL] >= 0) {
      for (int icla = 0; icla < nclafu; icla++) {
        int ipcla = 1 + icla;
        snprintf(fname, 80, "t_fuel_%02d", icla+1);
        cs_field_t *f_temp2 = cs_field_by_name(fname);
        for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
          tempk[cell_id + n_cells * ipcla] = f_temp2->val[cell_id];
        }
      }
    }

  }
  else
    cs_parameters_error
      (CS_ABORT_IMMEDIATE,
       _("Radiative transfer module"),
       _("Compatible thermal model should be temperature or enthaly-based,\n"
         "but here, \"cs_glob_thermal_model->itherm\" = %d."),
       cs_glob_thermal_model->itherm);

  /* Absorption coefficient for different modules;
     Warning: for the P-1 approximation, the absorption coefficient
     is required for boundary conditions. */

  if (cs_glob_physical_model_flag[CS_PHYSICAL_MODEL_FLAG] >= 2)
    cs_rad_transfer_absorption(tempk, kgi, agi, agbi);

  else {

    /* Absorption coefficient;
       initialization to a non-admissible value
       for testing after cs_user_rad_transfer_absorption(). */

    for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
      cpro_cak0[cell_id] = -cs_math_big_r;

    /* Data from GUI */

    if (cs_gui_file_is_loaded()) {

      /* FIXME for ADF */

      cs_gui_rad_transfer_absorption(cpro_cak0);

      if (   rt_params->iirayo == 2
          && cs_glob_physical_model_flag[CS_PHYSICAL_MODEL_FLAG] <= 1
          && ipadom <= 3)

        cs_rad_transfer_absorption_check_p1(cpro_cak0);

    }

    /* Only necessary when grey gas radiation properties are applied.
       In case of the ADF model this test doesnt make sense. */

    if (rt_params->imoadf == 0 && rt_params->imfsck == 0) {

      cs_user_rad_transfer_absorption(bc_type,
                                      izfrad,
                                      dt,
                                      cpro_cak0);

      if (cs_glob_rad_transfer_params->iirayo == 2)
        cs_rad_transfer_absorption_check_p1(cpro_cak0);

    }

  }

  /* -> Test if the radiation coeffcient has been assigned */
  if (rt_params->iirayo >= 1) {

    cs_real_t ckmin = 0.0;

    if (   rt_params->imoadf == 0
        && rt_params->imfsck == 0) {

      ckmin = cpro_cak0[0];
      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
        ckmin = CS_MIN(ckmin, cpro_cak0[cell_id]);

    }
    else {
      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
        for (int ngg = 0; ngg < nwsgg; ngg++)
          ckmin = CS_MIN(ckmin, kgi[cell_id + n_cells * ngg]);
      }

    }

    cs_parall_min(1, CS_DOUBLE, &ckmin);

    if (ckmin < 0.0)
      bft_error
        (__FILE__, __LINE__, 0,
         _("%s (in %s)\n"
           "The absorption coefficient must be > 0, but here the\n"
           "minimal value encountered is %g."),
         _("Radiative transfer module:\n"
           "-------------------------\n"), __func__,
         ckmin);

  }

  /* --> Check of a transparent case     */
  int idverl = rt_params->idiver;

  /* Solving the ETR.
     Loop over all gray gases. In case of the basic radiation models
     of Code_Saturne, nwsgg=1 */

  cs_real_t *cpro_cak;

  for (int ngg = 0; ngg < nwsgg; ngg++) {

    if (   rt_params->imoadf >= 1
        || rt_params->imfsck == 1) {

      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
        cpro_cak0[cell_id] = kgi[cell_id + n_cells * ngg];

    }
    else {
      cs_real_t aa = 0.0;

      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
        aa = CS_MAX(aa, cpro_cak0[cell_id]);

      if (cs_glob_rank_id >= 0)
        cs_parall_max(1, CS_DOUBLE, &aa);

      if (aa <= cs_math_epzero) {
        cs_log_printf(CS_LOG_DEFAULT,
                      _("      Radiative transfer with transparent medium."));
        idverl     =  -1;
      }

    }

    /* P-1 radiation model
       ------------------- */

    if (rt_params->iirayo == 2) {

      /* Gas phase: Explicit source term in the transport of theta4 */

      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
        smbrs[cell_id] =  3.0 * cpro_cak0[cell_id]
                              * pow(tempk[cell_id], 4.0)
                              * agi[cell_id + n_cells * ngg]
                              * cell_vol[cell_id];

      /* Solid phase/coal particles:
         Explicit source term in the transport eqn. of theta4 */
      if (cs_glob_physical_model_flag[CS_COMBUSTION_COAL] >= 0) {

        for (int icla = 0; icla < nclacp; icla++) {

          int ipcla = icla + 1;
          cpro_cak = CS_FI_(rad_cak, ipcla)->val;
          snprintf(fname, 80, "x_p_%02d", icla+1);
          cs_field_t *f_x2 = cs_field_by_name(fname);

          for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
            smbrs[cell_id] +=  3.0 * f_x2->val[cell_id] * cpro_cak[cell_id]
                                   * pow(tempk[cell_id + n_cells * ipcla], 4.0)
                                   * agi[cell_id + n_cells * ngg]
                                   * cell_vol[cell_id];

        }

      }
      /* Fuel droplets: Explicit source term in the transport eqn. of theta4 */
      else if (cs_glob_physical_model_flag[CS_COMBUSTION_FUEL] >= 0) {

        for (int icla = 0; icla < nclafu; icla++) {

          int ipcla = icla + 1;
          cpro_cak = CS_FI_(rad_cak, ipcla)->val;
          snprintf(fname, 80, "x_p_%02d", icla+1);
          cs_field_t *f_yfol = cs_field_by_name(fname);

          for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
            smbrs[cell_id] +=  3.0 * f_yfol->val[cell_id] * cpro_cak[cell_id]
                                   * pow (tempk[cell_id + n_cells * ipcla], 4.0)
                                   * agi[cell_id + n_cells * ngg]
                                   * cell_vol[cell_id];

        }

      }

      /* -> Gas phase: Implicit source term in the transport eqn. of theta4 */
      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
        rovsdt[cell_id] =  3.0 * cpro_cak0[cell_id]
                               * cell_vol[cell_id];

      /* -> Solid phase: */
      /* Coal particles: Implicit source term in the transport eqn. of theta4   */
      if (cs_glob_physical_model_flag[CS_COMBUSTION_COAL] >= 0) {

        for (int icla = 0; icla < nclacp; icla++) {

          cpro_cak = CS_FI_(rad_cak, icla+1)->val;

          snprintf(fname, 80, "x_p_%02d", icla + 1);
          cs_field_t *f_x2 = cs_field_by_name(fname);

          for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
            rovsdt[cell_id] +=  3.0 * f_x2->val[cell_id] * cpro_cak[cell_id]
                                    * cell_vol[cell_id];
        }

      }
      /* Fuel droplets: Implicit source term in the transport eqn. of theta4 */
      else if (cs_glob_physical_model_flag[CS_COMBUSTION_FUEL] >= 0) {

        for (int icla = 0; icla < nclafu; icla++) {

          cpro_cak = CS_FI_(rad_cak, icla+1)->val;

          snprintf(fname, 80, "x_p_%02d", icla + 1);
          cs_field_t *f_yfol = cs_field_by_name(fname);

          for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
            rovsdt[cell_id] +=  3.0 * f_yfol->val[cell_id] * cpro_cak[cell_id]
                                    * cell_vol[cell_id];
        }

      }

      /* Radiation coeffcient of the bulk phase   */
      /* Gas phase: */
      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
        ckmel[cell_id] = cpro_cak0[cell_id];

      /* Solid phase:    */
      /* Coal particles  */
      if (cs_glob_physical_model_flag[CS_COMBUSTION_COAL] >= 0) {

        for (int icla = 0; icla < nclacp; icla++) {

          cpro_cak = CS_FI_(rad_cak, icla+1)->val;

          snprintf(fname, 80, "x_p_%02d", icla + 1);
          cs_field_t *f_x2 = cs_field_by_name(fname);

          for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
            ckmel[cell_id] += f_x2->val[cell_id] * cpro_cak[cell_id];
        }

      }
      /* Fuel droplets   */
      else if (cs_glob_physical_model_flag[CS_COMBUSTION_FUEL] >= 0) {

        for (int icla = 0; icla < nclafu; icla++) {

          cpro_cak = CS_FI_(rad_cak, icla+1)->val;

          snprintf(fname, 80, "x_p_%02d", icla + 1);
          cs_field_t *f_yfol = cs_field_by_name(fname);

          for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
            ckmel[cell_id] += f_yfol->val[cell_id] * cpro_cak[cell_id];
        }

      }

      /* Test if ckmel is gt zero  */
      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
        if (ckmel[cell_id] <= 0.0)
          bft_error
            (__FILE__, __LINE__, 0,
             _("%s (in %s)\n"
               "The local radiation coeffcient of the bulk phase ckmel\n"
               "takes the value 0 somewhere. This often occurs during\n"
               "the very first iterations of the simulation.\n"
               "To avoid this, ensure the coal and/or the char mass fraction\n"
               "have been initialized to values different from zero."),
             _("Radiative transfer module (P-1 radiation):\n"
               "-------------------------\n"),
             __func__);
      }

      /* Update Boundary condition coefficients   */
      cs_rad_transfer_bc_coeffs(bc_type,
                                coefap, coefbp,
                                cofafp, cofbfp,
                                tparo,  ckmel,
                                agbi,   ngg);

      /* Solving    */
      cs_rad_transfer_pun(bc_type,
                          coefap, coefbp,
                          cofafp, cofbfp,
                          flurds, flurdb,
                          viscf, viscb,
                          smbrs, rovsdt,
                          tparo, ckmel,
                          iqpar,
                          agbi, ngg);
    }

    /* Solving of the radiative transfer equation (DOM)
       ------------------------------------------------ */

    else if (rt_params->iirayo == 1) {

      /* -> Gas phase: Explicit source term of the ETR */
      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
        smbrs[cell_id] =  c_stefan * cpro_cak0[cell_id]
                                   * (pow (tempk[cell_id], 4.0))
                                   * agi[cell_id + n_cells * ngg]
                                   * cell_vol[cell_id]
                                   * unspi;

      /* -> Solid phase: */
      /* Coal particles: Explicit source term of the ETR    */
      if (cs_glob_physical_model_flag[CS_COMBUSTION_COAL] >= 0) {

        for (int icla = 0; icla < nclacp; icla++) {

          int ipcla = icla + 1;
          cpro_cak = CS_FI_(rad_cak, ipcla)->val;
          snprintf(fname, 80, "x_p_%02d", icla+1);
          cs_field_t *f_x2 = cs_field_by_name(fname);

          for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
            smbrs[cell_id] +=   f_x2->val[cell_id]
                              * agi[cell_id + n_cells * ngg]
                              * c_stefan
                              * cpro_cak[cell_id]
                              * (pow (tempk[cell_id + n_cells * ipcla], 4.0))
                              * cell_vol[cell_id]
                              * unspi;
        }

      }
      /* Fuel droplets: Explicit source term of the ETR     */
      else if (cs_glob_physical_model_flag[CS_COMBUSTION_FUEL] >= 0) {

        for (int icla = 0; icla < nclafu; icla++) {

          int ipcla = icla + 1;
          cpro_cak = CS_FI_(rad_cak, ipcla)->val;
          snprintf(fname, 80, "x_p_%02d", icla+1);
          cs_field_t *f_yfol = cs_field_by_name(fname);

          for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
            smbrs[cell_id] +=   f_yfol->val[cell_id]
                              * agi[cell_id + n_cells * ngg]
                              * c_stefan
                              * cpro_cak[cell_id]
                              * (pow (tempk[cell_id + n_cells * ipcla], 4.0))
                              * cell_vol[cell_id]
                              * unspi;
        }

      }

      /* -> Gas phase: Implicit source term of the ETR */
      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
        rovsdt[cell_id] = cpro_cak0[cell_id] * cell_vol[cell_id];

      /* -> Solid phase  */
      /* Coal particles: Implicit source term of the ETR    */
      if (cs_glob_physical_model_flag[CS_COMBUSTION_COAL] >= 0) {

        for (int icla = 0; icla < nclacp; icla++) {

          cpro_cak = CS_FI_(rad_cak, icla+1)->val;

          snprintf(fname, 80, "x_p_%02d", icla + 1);
          cs_field_t *f_x2 = cs_field_by_name(fname);

          for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
            rovsdt[cell_id] +=   f_x2->val[cell_id]
                               * cpro_cak[cell_id]
                               * cell_vol[cell_id];
        }

      }
      /* Fuel droplets: Implicit source term of the ETR     */
      else if (cs_glob_physical_model_flag[CS_COMBUSTION_FUEL] >= 0) {

        for (int icla = 0; icla < nclafu; icla++) {

          cpro_cak = CS_FI_(rad_cak, icla+1)->val;

          snprintf(fname, 80, "x_p_%02d", icla + 1);
          cs_field_t *f_yfol = cs_field_by_name(fname);

          for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
            rovsdt[cell_id] =   f_yfol->val[cell_id]
                              * cpro_cak[cell_id]
                              * cell_vol[cell_id];
        }

      }

      /* Update boundary condition coefficients */
      cs_rad_transfer_bc_coeffs(bc_type,
                                coefap, coefbp,
                                cofafp, cofbfp,
                                tparo , ckmel,
                                agbi  , ngg);

      /* Solving    */
      _cs_rad_transfer_sol(coefap, coefbp,
                           cofafp, cofbfp,
                           flurds, flurdb,
                           viscf , viscb,
                           smbrs , rovsdt,
                           iqpar , ngg);

    }

    /* Summing up the quantities of each grey gas    */

    /* Absorption */
    for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
      iabgaz[cell_id] += cpro_cak0[cell_id] * cpro_re_st0[cell_id] * wq[ngg];

    if (cs_glob_physical_model_flag[CS_COMBUSTION_COAL] >= 0) {

      for (int icla = 0; icla < nclacp; icla++) {

        cpro_cak = CS_FI_(rad_cak, icla+1)->val;

        snprintf(fname, 80, "x_p_%02d", icla + 1);
        cs_field_t *f_x2 = cs_field_by_name(fname);
        for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
          iabpar[cell_id] +=  f_x2->val[cell_id] * cpro_cak[cell_id]
                        * cpro_re_st0[cell_id] * wq[ngg];
          iabparh2[cell_id + n_cells * icla]
            +=  cpro_cak[cell_id] * cpro_re_st0[cell_id] * wq[ngg];
        }
      }

    }

    else if (cs_glob_physical_model_flag[CS_COMBUSTION_FUEL] >= 0) {

      for (int icla = 0; icla < nclafu; icla++) {

        cpro_cak = CS_FI_(rad_cak, icla+1)->val;

        snprintf(fname, 80, "x_p_%02d", icla + 1);
        cs_field_t *f_yfol = cs_field_by_name(fname);
        for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
          iabpar[cell_id] +=  f_yfol->val[cell_id] * cpro_cak[cell_id]
                            * cpro_re_st0[cell_id] * wq[ngg];
          iabparh2[cell_id + n_cells * icla] +=   cpro_cak[cell_id]
                                                * cpro_re_st0[cell_id]
                                                * wq[ngg];
        }
      }

    }

    /* Emission   */

    for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
      iemgex[cell_id] -=   cpro_cak0[cell_id] * agi[cell_id + n_cells * ngg]
                         * 4.0 * c_stefan * pow(tempk[cell_id + n_cells * 0], 4.0)
                         * wq[ngg];
      iemgim[cell_id] -=   16.0 * dcp[cell_id] * cpro_cak0[cell_id]
                         * agi[cell_id + ngg * n_cells]
                         * c_stefan * pow(tempk[cell_id + n_cells * 0], 3.0)
                         * wq[ngg];
    }

    if (cs_glob_physical_model_flag[CS_COMBUSTION_COAL] >= 0) {

      for (int icla = 0; icla < nclacp; icla++) {

        int ipcla = icla + 1;
        cpro_cak = CS_FI_(rad_cak, ipcla)->val;
        snprintf(fname, 80, "x_p_%02d", icla+1);
        cs_field_t *f_x2 = cs_field_by_name(fname);

        for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {

          iempex[cell_id] += -  4.0
                          * f_x2->val[cell_id]
                          * c_stefan
                          * cpro_cak[cell_id]
                          * pow(tempk[cell_id + n_cells * ipcla], 4.0)
                          * agi[cell_id + n_cells * ngg]
                          * wq[ngg];

          iempexh2[cell_id + n_cells * icla]
            += -  4.0 * c_stefan
                      * cpro_cak[cell_id]
                      * pow(tempk[cell_id + n_cells * ipcla], 4.0)
                      * agi[cell_id + n_cells * ngg]
                      * wq[ngg];

          iempim[cell_id]
            += - 16.0 * c_stefan
                      * cpro_cak[cell_id]
                      * f_x2->val[cell_id]
                      * pow (tempk[cell_id + n_cells * ipcla], 3.0)
                      * agi[cell_id + n_cells * ngg]
                      / cp2ch[ichcor[icla]-1]
                      * wq[ngg];

          iempimh2[cell_id + n_cells * icla]
            += -  16.0 * c_stefan
                       * cpro_cak[cell_id]
                       * pow(tempk[cell_id + n_cells * ipcla], 3.0)
                       * agi[cell_id + n_cells * ngg]
                       / cp2ch[ichcor[icla]-1]
                       * wq[ngg];

        }
      }

    }
    else if (cs_glob_physical_model_flag[CS_COMBUSTION_FUEL] >= 0) {

      for (int icla = 0; icla < nclafu; icla++) {

        int ipcla = icla + 1;
        cpro_cak = CS_FI_(rad_cak, ipcla)->val;
        snprintf(fname, 80, "x_p_%02d", icla+1);
        cs_field_t *f_yfol = cs_field_by_name(fname);

        for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
          iempex[cell_id] += -  4.0 * c_stefan
                                    * f_yfol->val[cell_id]
                                    * cpro_cak[cell_id]
                                    * pow (tempk[cell_id + n_cells * ipcla], 4.0)
                                    * agi[cell_id + n_cells * ngg]
                                    * wq[ngg];

          iempexh2[cell_id + n_cells * icla]
            += -  4.0 * c_stefan * cpro_cak[cell_id]
                      * pow(tempk[cell_id + n_cells * ipcla], 4.0)
                      * agi[cell_id + n_cells * ngg]
                      * wq[ngg];

          iempim[cell_id] += -  16.0 * c_stefan
                                     * cpro_cak[cell_id]
                                     * f_yfol->val[cell_id]
                                     * pow(tempk[cell_id + n_cells * ipcla], 3.0)
                                     * agi[cell_id + n_cells * ngg]
                                     / cp2fol
                                     * wq[ngg];

          iempimh2[cell_id + n_cells * icla]
            += -  16.0  * c_stefan * cpro_cak[cell_id]
                        * pow(tempk[cell_id + n_cells * ipcla], 3.0)
                        * agi[cell_id + n_cells * ngg]
                        / cp2fol
                        * wq[ngg];
        }
      }

    }

    for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
      /* Emitted intensity    */
      ilutot[cell_id]  = ilutot[cell_id] + (cpro_re_st0[cell_id] * wq[ngg]);

      /* Flux vector components    */
      cpro_q[cell_id][0] += iqpar[cell_id][0] * wq[ngg];
      cpro_q[cell_id][1] += iqpar[cell_id][1] * wq[ngg];
      cpro_q[cell_id][2] += iqpar[cell_id][2] * wq[ngg];
    }

    /* If the ADF model is activated we have to sum
       the spectral flux densities */

    if (rt_params->imoadf >= 1) {
      for (cs_lnum_t ifac = 0; ifac < n_b_faces; ifac++)
        iqpato[ifac] += f_qinsp->val[ngg + ifac * nwsgg] * wq[ngg];
    }

  } /* end loop on grey gas */

  /* The total radiative flux is copied in bqinci   */
  /* a) for post-processing reasons and   */
  /* b) in order to calculate bfnet  */
  if (rt_params->imoadf >= 1) {
    for (cs_lnum_t ifac = 0; ifac < n_b_faces; ifac++)
      f_qinci->val[ifac] = iqpato[ifac];
  }

  /* Storing of the total emitted intensity:
   *      / -> ->
   * SA= / L( X, S ). DOMEGA
   *    /4.PI
   */
  for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
    cpro_lumin[cell_id] = ilutot[cell_id];

  /* Net radiative flux at walls: computation and integration */

  /* -> Initialization to a non-admissible value for testing after
   *    cs_user_rad_transfer_net_flux   */
  for (cs_lnum_t ifac = 0; ifac < n_b_faces; ifac++)
    f_fnet->val[ifac] = -cs_math_big_r;

  /* Basic definition for net flux */

  _compute_net_flux(bc_type,
                    izfrad,
                    dt,
                    coefap,
                    coefbp,
                    tparo,
                    f_qinci->val,
                    f_xlam->val,
                    f_epa->val,
                    f_eps->val,
                    cpro_cak0,
                    f_fnet->val);

  /*---> Reading of User data
   * CAREFUL: The user has access to the radiation coeffcient (field f_cak1)
   * in cs_user_rad_transfer_net_flux.
   * However, only when the standard radiation models of code_saturne are
   * applied, this table contains the true value given by the user.
   * Thus, the radiation coefficient in cs_user_rad_transfer_net_flux must
   * be used with caution.
   * In its present version cs_user_rad_transfer_net_flux does NOT use the
   * radiation coeffcient, and thus, cs_user_rad_transfer_net_flux can
   * still be called here, even if the ADF model is activated.
   */

  cs_user_rad_transfer_net_flux(bc_type,
                                izfrad,
                                dt,
                                coefap,
                                coefbp,
                                cofafp,
                                cofbfp,
                                tparo,
                                f_qinci->val,
                                f_xlam->val,
                                f_epa->val,
                                f_eps->val,
                                cpro_cak0,
                                f_fnet->val);

  /* Check net flux */
  cs_real_t xlimit = -cs_math_big_r * 0.1;
  cs_real_t flunmn = cs_math_big_r;

  for (cs_lnum_t ifac = 0; ifac < n_b_faces; ifac++) {
    if (f_fnet->val[ifac] <= xlimit)
      bc_type[ifac] = - CS_ABS(bc_type[ifac]);
  }

  cs_boundary_conditions_error(bc_type, "Net flux BC values");

  /* Integrate net flux net on different boundary zones;
     iflux used in parallel tolocate existing zones */
  int nozrdm = rt_params->nozrdm;
  int nozarm = rt_params->nozarm;

  int iflux[nozrdm];
  cs_real_t flux[nozrdm];

  for (int izone = 0; izone < nozrdm; izone++) {
    flux[izone]  = 0.0;
    iflux[izone] = 0;
  }

  for (cs_lnum_t ifac = 0; ifac < n_b_faces; ifac++) {
    int izone = izfrad[ifac] - 1;// TODO Check index here
    flux[izone]  = flux[izone] + f_fnet->val[ifac] * b_face_surf[ifac];
    iflux[izone] = 1;
  }

  if (cs_glob_rank_id >= 0) {
    cs_parall_sum(nozarm, CS_DOUBLE, flux);
    cs_parall_max(nozarm, CS_INT_TYPE, iflux);
  }

  cs_log_printf
    (CS_LOG_DEFAULT,
     _("-------------------------------------------------------------------\n"));
  cs_log_printf
    (CS_LOG_DEFAULT,
     _("Zone         Radiative net flux (Watt) (outward-facing unit normal)\n"));

  for (int izone = 0; izone < nozarm; izone++) {

    if (iflux[izone] == 1)
      cs_log_printf(CS_LOG_DEFAULT,
                    _("%6d             %11.4e\n"),
                    izone + 1, // for listing identicity with fortran version
                    flux[izone]);
  }

  cs_log_printf
    (CS_LOG_DEFAULT,
     _("-------------------------------------------------------------------\n"));

  /* -> Integration de la densite de flux net aux frontieres */
  cs_real_t aa = 0.0;
  for (cs_lnum_t ifac = 0; ifac < n_b_faces; ifac++)
    aa += f_fnet->val[ifac] * b_face_surf[ifac];

  if (cs_glob_rank_id >= 0)
    cs_parall_sum(1, CS_DOUBLE, &aa);

  cs_log_printf(CS_LOG_DEFAULT,
                _("'Flux net radiatif sur toutes les frontieres  Fnet = %11.4e Watt\n"),
                aa);

  /*Semi-analitical radiative source terms */

  if (idverl >= 0) {

    for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {

      /* Absorption of the gas is copied into abso1    */
      cpro_abso0[cell_id] = iabgaz[cell_id];

      /* Emission of the gas phase is copied into emi1 */
      cpro_emi0[cell_id] = iemgex[cell_id];

    }

    if (   cs_glob_physical_model_flag[CS_COMBUSTION_COAL] >= 0
        || cs_glob_physical_model_flag[CS_COMBUSTION_FUEL] >= 0) {

      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {

        /* Absoprtion of particles is added to abso1 */
        cpro_abso0[cell_id] += iabpar[cell_id];

        /* Emission of particles is added to emi1 */
        cpro_emi0[cell_id] += iempex[cell_id];

      }

    }

    /* Emission + Absorption of gas and particles --> TSexplicit    */
    for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
      cpro_re_st0[cell_id] = cpro_abso0[cell_id] + cpro_emi0[cell_id];

    /* TSimplicit of the gas phase    */
    for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
      cpro_ri_st0[cell_id] = iemgim[cell_id];

    /* TSimplicit of the solid phase is added to stri1    */
    if (   cs_glob_physical_model_flag[CS_COMBUSTION_COAL] >= 0
        || cs_glob_physical_model_flag[CS_COMBUSTION_FUEL] >= 0) {
      for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
        cpro_ri_st0[cell_id] += iempim[cell_id];
    }

    /* In order to determine the source terms of the particle enthalpy
       tranport equation, we have to copie the approriate determined above
       into the corressponding tables. */

    if (   cs_glob_physical_model_flag[CS_COMBUSTION_COAL] >= 0
        || cs_glob_physical_model_flag[CS_COMBUSTION_FUEL] >= 0) {

      for (int icla = 0; icla < nclacp; icla++) {
        int ipcla = 1 + icla;

        cs_real_t *cpro_tsri = CS_FI_(rad_ist, ipcla)->val;
        cs_real_t *cpro_tsre = CS_FI_(rad_est, ipcla)->val;
        cs_real_t *cpro_abso = CS_FI_(rad_abs, ipcla)->val;
        cs_real_t *cpro_emi  = CS_FI_(rad_emi, ipcla)->val;

        for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
          cpro_abso[cell_id] = iabparh2[cell_id + n_cells * icla];
          cpro_emi[cell_id]  = iempexh2[cell_id + n_cells * icla];
          cpro_tsre[cell_id] =   iabparh2[cell_id + n_cells * icla]
                               + iempexh2[cell_id + n_cells * icla];
          cpro_tsri[cell_id] = iempimh2[cell_id + n_cells * icla];
        }

      }

    }

  }
  else {

    for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
      cpro_abso0[cell_id]   = 0.0;
      cpro_emi0[cell_id]    = 0.0;
      cpro_re_st0[cell_id]   = 0.0;
      cpro_ri_st0[cell_id]   = 0.0;
    }

  }

  /* Explicit conservative radiative source terms */

  /* coefap and coefbp are now Boundary conditions on the divergence */
  if (idverl == 1 || idverl == 2) {

    /* Allocate temporary arrays for gradient computation */
    cs_real_3_t *coefaq;
    BFT_MALLOC(coefaq, n_b_faces, cs_real_3_t);
    cs_real_33_t *coefbq;
    BFT_MALLOC(coefbq, n_b_faces, cs_real_33_t);

    for (cs_lnum_t ifac = 0; ifac < n_b_faces; ifac++) {
      coefaq[ifac][0] = f_fnet->val[ifac] * surfbo[ifac][0] / b_face_surf[ifac];
      coefaq[ifac][1] = f_fnet->val[ifac] * surfbo[ifac][1] / b_face_surf[ifac];
      coefaq[ifac][2] = f_fnet->val[ifac] * surfbo[ifac][2] / b_face_surf[ifac];
    }

    for (cs_lnum_t ifac = 0; ifac < n_b_faces; ifac++) {
      for (int isou = 0; isou < 3; isou++) {
        for (int jsou = 0; jsou < 3; jsou++)
          coefbq[ifac][jsou][isou] = 0.;
      }
    }

    cs_real_33_t *grad;
    BFT_MALLOC(grad, n_cells_ext, cs_real_33_t);

    /* Data for computation of divergence */

    cs_halo_type_t halo_type = CS_HALO_STANDARD;
    cs_gradient_type_t gradient_type = CS_GRADIENT_ITER;

    cs_gradient_type_by_imrgra(cs_glob_space_disc->imrgra,
                               &gradient_type,
                               &halo_type);

    cs_gradient_vector("Work array",
                       gradient_type,
                       halo_type,
                       1,      /* inc */
                       100,    /* n_r_sweeps, */
                       rt_params->iimlum,  /* iwarnp */
                       -1,     /* imligp */
                       1e-8,   /* epsrgp */
                       1.5,    /* climgp */
                       (const cs_real_3_t *)coefaq,
                       (const cs_real_33_t *)coefbq,
                       cpro_q,
                       grad);

    for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++) {
      cpro_re_st0[cell_id] = - grad[cell_id][0][0]
                             - grad[cell_id][1][1]
                             - grad[cell_id][2][2];
    }

    /* Free memory */
    BFT_FREE(grad);
    BFT_FREE(coefbq);
    BFT_FREE(coefaq);

  } /* End of computation of divergence */

  /* Explicit radiative semi-analytical corrected source term */

  if (idverl == 2) {

    /* Comparison of the semi-analytical and conservative source terms */

    cs_real_t s[2] = {0, 0};

    for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
      s[0] += cpro_re_st0[cell_id] * cell_vol[cell_id];

    for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
      s[1] +=   (cpro_abso0[cell_id] + cpro_emi0[cell_id])
              * cell_vol[cell_id];

    cs_parall_sum(2, CS_DOUBLE, s);

    s[0] /= s[1];

    /* Correction of the semi-analytical source term
       by the conservative source term  */
    for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
      cpro_re_st0[cell_id] = (  cpro_abso0[cell_id]
                              + cpro_emi0[cell_id]) * s[0];

  }

  /* Finalization of explicit source terms */

  if (idverl >= 0) {

    /* -> Integration volumique du terme source explicite
     * Le resultat de cette integration DOIT etre le meme que l'integration
     * surfacique de la densite de flux net radiatif faite plus haut
     * si IDVERL = 1 ou 2 */

    aa = 0.0;
    for (cs_lnum_t cell_id = 0; cell_id < n_cells; cell_id++)
      aa += cpro_re_st0[cell_id] * cell_vol[cell_id];

    if (cs_glob_rank_id >= 0)
      cs_parall_sum(1, CS_DOUBLE, &aa);

    cs_log_printf
      (CS_LOG_DEFAULT,
       _("Volume integral of radiative source term Srad = %11.4e Watt\n"
         "(If IDIVER = 1 or 2, we must have Srad = -Fnet)\n"),
       aa);

    /* Correction of explicit source term in raysca to allow a correct
     * post-processing of that term when the transported variable is the
     * temperature (for combustion, it is always enthalpy) */

  }

  cs_log_printf
    (CS_LOG_DEFAULT,
     _("-------------------------------------------------------------------\n"));

  /* Free memory */

  BFT_FREE(viscf);
  BFT_FREE(viscb);
  BFT_FREE(smbrs);
  BFT_FREE(rovsdt);
  BFT_FREE(tempk);
  BFT_FREE(coefap);
  BFT_FREE(coefbp);
  BFT_FREE(cofafp);
  BFT_FREE(cofbfp);
  BFT_FREE(flurds);
  BFT_FREE(flurdb);
  BFT_FREE(ckmel);
  BFT_FREE(dcp);
  BFT_FREE(tparo);
  BFT_FREE(kgi);
  BFT_FREE(agi);
  BFT_FREE(agbi);
  BFT_FREE(iqpar);
  BFT_FREE(iabgaz);
  BFT_FREE(iabpar);
  BFT_FREE(iemgex);
  BFT_FREE(iempex);
  BFT_FREE(ilutot);
  BFT_FREE(iemgim);
  BFT_FREE(iempim);
  BFT_FREE(iabparh2);
  BFT_FREE(iempexh2);
  BFT_FREE(iempimh2);

  if (cs_glob_time_step->nt_cur == cs_glob_time_step->nt_max)
    BFT_FREE(wq);
}

/*----------------------------------------------------------------------------*/

END_C_DECLS
