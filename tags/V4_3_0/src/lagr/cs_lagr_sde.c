/*============================================================================
 * Methods for particle deposition
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
  Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

/*----------------------------------------------------------------------------*/

/*============================================================================
 * Functions dealing with particle deposition
 *============================================================================*/

#include "cs_defs.h"
#include "cs_math.h"

/*----------------------------------------------------------------------------
 * Standard C library headers
 *----------------------------------------------------------------------------*/

#include <math.h>

/*----------------------------------------------------------------------------
 *  Local headers
 *----------------------------------------------------------------------------*/

#include "bft_mem.h"

#include "cs_mesh.h"
#include "cs_mesh_quantities.h"
#include "cs_physical_constants.h"
#include "cs_physical_model.h"
#include "cs_prototypes.h"
#include "cs_thermal_model.h"

#include "cs_lagr.h"
#include "cs_lagr_deposition_model.h"
#include "cs_lagr_roughness.h"
#include "cs_lagr_tracking.h"
#include "cs_lagr_prototypes.h"

/*----------------------------------------------------------------------------
 *  Header for the current file
 *----------------------------------------------------------------------------*/

#include "cs_lagr_sde.h"

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*! \cond DOXYGEN_SHOULD_SKIP_THIS */

/*============================================================================
 * Static global variables
 *============================================================================*/

/* Boltzmann constant */
static const double _k_boltz = 1.38e-23;

/*! (DOXYGEN_SHOULD_SKIP_THIS) \endcond */

/*============================================================================
 * Private function prototypes (definitions follow)
 *============================================================================*/

/*============================================================================
 * Private function definitions
 *============================================================================*/

/* ------------------------------------------------------------------------------*/
/* INTEGRATION DES EDS PAR UN SCHEMA D'ORDRE 1 (scheO1)
 *
 * Parameters
 * \param[in] taup      temps caracteristique dynamique
 * \param[in] tlag      temps caracteristique fluide
 * \param[in] piil      terme dans l'integration des eds up
 * \param[in] bx        caracteristiques de la turbulence
 * \param[in] vagaus    variables aleatoires gaussiennes
 * \param[in] gradpr    pressure gradient
 * \param[in] romp      masse volumique des particules
 * \param[in] fextla    champ de forces exterieur utilisateur (m/s2)
 */
/*------------------------------------------------------------------------------*/

static void
_lages1(cs_real_t     dtp,
        cs_real_t    *taup,
        cs_real_3_t  *tlag,
        cs_real_3_t  *piil,
        cs_real_t    *bx,
        cs_real_33_t *vagaus,
        cs_real_3_t  *gradpr,
        cs_real_t    *romp,
        cs_real_t    *brgaus,
        cs_real_t    *terbru,
        cs_real_3_t  *fextla)
{
  /* Particles management */
  cs_lagr_particle_set_t  *p_set = cs_glob_lagr_particle_set;
  const cs_lagr_attribute_map_t  *p_am = p_set->p_am;

  cs_lagr_extra_module_t *extra = cs_get_lagr_extra_module();

  /* Initialisations*/
  cs_real_3_t grav = {cs_glob_physical_constants->gx,
                      cs_glob_physical_constants->gy,
                      cs_glob_physical_constants->gz};

  cs_real_t tkelvi =  273.15;

  cs_real_t vitf = 0.0;

  cs_real_t aux1, aux2, aux3, aux4, aux5, aux6, aux7, aux8, aux9, aux10, aux11;
  cs_real_t ter1f, ter2f, ter3f;
  cs_real_t ter1p, ter2p, ter3p, ter4p, ter5p;
  cs_real_t ter1x, ter2x, ter3x, ter4x, ter5x;
  cs_real_t p11, p21, p22, p31, p32, p33;
  cs_real_t omega2, gama2, omegam;
  cs_real_t grga2, gagam, gaome;
  cs_real_t tbrix1, tbrix2, tbriu;

  cs_lnum_t nor = cs_glob_lagr_time_step->nor;

  /* Integration des eds sur les particules */

  for (cs_lnum_t ip = 0; ip < p_set->n_particles; ip++) {

    unsigned char *particle = p_set->p_buffer + p_am->extents * ip;

    cs_lnum_t cell_id = cs_lagr_particle_get_cell_id(particle, p_am);

    if (cell_id >= 0) {

      cs_real_t *old_part_vel      = cs_lagr_particle_attr_n(particle, p_am, 1,
                                                             CS_LAGR_VELOCITY);
      cs_real_t *old_part_vel_seen = cs_lagr_particle_attr_n(particle, p_am, 1,
                                                             CS_LAGR_VELOCITY_SEEN);
      cs_real_t *part_vel          = cs_lagr_particle_attr(particle, p_am,
                                                           CS_LAGR_VELOCITY);
      cs_real_t *part_vel_seen     = cs_lagr_particle_attr(particle, p_am,
                                                           CS_LAGR_VELOCITY_SEEN);
      cs_real_t *part_coords       = cs_lagr_particle_attr(particle, p_am,
                                                           CS_LAGR_COORDS);
      cs_real_t *old_part_coords   = cs_lagr_particle_attr_n(particle, p_am, 1,
                                                             CS_LAGR_COORDS);

      cs_real_t rom = extra->cromf->val[cell_id];

      for (cs_lnum_t id = 0; id < 3; id++) {

        vitf = extra->vel->vals[1][cell_id * 3 + id];

        /* --> (2.1) Calcul preliminaires :    */
        /* ----------------------------   */
        /* calcul de II*TL+<u> et [(grad<P>/rhop+g)*tau_p+<Uf>] ?  */

        cs_real_t tci = piil[ip][id] * tlag[ip][id] + vitf;
        cs_real_t force = 0.0;

        if (cs_glob_lagr_time_scheme->iadded_mass == 0)
          force = (- gradpr[cell_id][id] / romp[ip]
                   + grav[id] + fextla[ip][id]) * taup[ip];

        /* Added-mass term?     */
        else
          force =   (- gradpr[cell_id][id] / romp[ip]
                     * (1.0 + 0.5 * cs_glob_lagr_time_scheme->added_mass_const)
                  / (1.0 + 0.5 * cs_glob_lagr_time_scheme->added_mass_const * rom / romp[ip])
                  + grav[id] + fextla[ip][id]) * taup[ip];


        /* --> (2.2) Calcul des coefficients/termes deterministes */
        /* ----------------------------------------------------    */

        aux1 = exp(-dtp / taup[ip]);
        aux2 = exp(-dtp / tlag[ip][id]);
        aux3 = tlag[ip][id] / (tlag[ip][id] - taup[ip]);
        aux4 = tlag[ip][id] / (tlag[ip][id] + taup[ip]);
        aux5 = tlag[ip][id] * (1.0 - aux2);
        aux6 = pow(bx[p_set->n_particles * (3 * nor + id) + ip], 2.0) * tlag[ip][id];
        aux7 = tlag[ip][id] - taup[ip];
        aux8 = pow(bx[p_set->n_particles * (3 * nor + id) + ip], 2.0) * pow(aux3, 2);

        /* --> trajectory terms */
        cs_real_t aa = taup[ip] * (1.0 - aux1);
        cs_real_t bb = (aux5 - aa) * aux3;
        cs_real_t cc = dtp - aa - bb;

        ter1x = aa * old_part_vel[id];
        ter2x = bb * old_part_vel_seen[id];
        ter3x = cc * tci;
        ter4x = (dtp - aa) * force;

        /* --> flow-seen velocity terms   */
        ter1f = old_part_vel_seen[id] * aux2;
        ter2f = tci * (1.0 - aux2);

        /* --> termes pour la vitesse des particules     */
        cs_real_t dd = aux3 * (aux2 - aux1);
        cs_real_t ee = 1.0 - aux1;

        ter1p = old_part_vel[id] * aux1;
        ter2p = old_part_vel_seen[id] * dd;
        ter3p = tci * (ee - dd);
        ter4p = force * ee;

        /* --> (2.3) Coefficients computation for the stochastic integral    */
        /* --> Integral for particles position */
        gama2  = 0.5 * (1.0 - aux2 * aux2);
        omegam = (0.5 * aux4 * (aux5 - aux2 * aa) - 0.5 * aux2 * bb) * sqrt(aux6);
        omega2 =  aux7 * (aux7 * dtp - 2.0 * (tlag[ip][id] * aux5 - taup[ip] * aa))
                 + 0.5 * tlag[ip][id] * tlag[ip][id] * aux5 * (1.0 + aux2)
                 + 0.5 * taup[ip] * taup[ip] * aa * (1.0 + aux1)
                 - 2.0 * aux4 * tlag[ip][id] * taup[ip] * taup[ip] * (1.0 - aux1 * aux2);
        omega2 = aux8 * omega2;

        if (CS_ABS(gama2) > cs_math_epzero) {

          p21 = omegam / sqrt(gama2);
          p22 = omega2 - pow(p21, 2);
          p22 = sqrt(CS_MAX(0.0, p22));

        }
        else {

          p21 = 0.0;
          p22 = 0.0;

        }

        ter5x = p21 * vagaus[ip][id][0] + p22 * vagaus[ip][id][1];

        /* --> integral for the flow-seen velocity  */
        p11   = sqrt(gama2 * aux6);
        ter3f = p11 * vagaus[ip][id][0];

        /* --> integral for the particles velocity  */
        aux9  = 0.5 * tlag[ip][id] * (1.0 - aux2 * aux2);
        aux10 = 0.5 * taup[ip] * (1.0 - aux1 * aux1);
        aux11 =   taup[ip] * tlag[ip][id]
                * (1.0 - aux1 * aux2)
                / (taup[ip] + tlag[ip][id]);

        grga2 = (aux9 - 2.0 * aux11 + aux10) * aux8;
        gagam = (aux9 - aux11) * (aux8 / aux3);
        gaome = ( (tlag[ip][id] - taup[ip]) * (aux5 - aa)
                  - tlag[ip][id] * aux9
                  - taup[ip] * aux10
                  + (tlag[ip][id] + taup[ip]) * aux11)
                * aux8;

        if (p11 > cs_math_epzero)
          p31 = gagam / p11;
        else
          p31 = 0.0;

        if (p22 > cs_math_epzero)
          p32 = (gaome - p31 * p21) / p22;
        else
          p32 = 0.0;

        p33 = grga2 - pow(p31, 2) - pow(p32, 2);
        p33 = sqrt(CS_MAX(0.0, p33));
        ter5p = p31 * vagaus[ip][id][0] + p32 * vagaus[ip][id][1] + p33 * vagaus[ip][id][2];

        /* --> (2.3) Calcul des Termes dans le cas du mouvement Brownien :   */
        if (cs_glob_lagr_brownian->lamvbr == 1) {

          /* Calcul de la temperature du fluide en fonction du type  */
          /* d'ecoulement    */
          cs_real_t tempf;
          if (   cs_glob_physical_model_flag[CS_COMBUSTION_COAL] >= 0
              || cs_glob_physical_model_flag[CS_COMBUSTION_PCLC] >= 0)
            tempf = extra->t_gaz->val[cell_id];

          else if (   cs_glob_physical_model_flag[CS_COMBUSTION_3PT] >= 0
                   || cs_glob_physical_model_flag[CS_COMBUSTION_EBU] >= 0
                   || cs_glob_physical_model_flag[CS_ELECTRIC_ARCS] >= 0
                   || cs_glob_physical_model_flag[CS_JOULE_EFFECT] >= 0)
            tempf = extra->temperature->val[cell_id];

          else if (   cs_glob_thermal_model->itherm == 1
                   && cs_glob_thermal_model->itpscl == 2)
            tempf = extra->scal_t->val[cell_id] + tkelvi;

          else if (   cs_glob_thermal_model->itherm == 1
                   && cs_glob_thermal_model->itpscl == 1)
            tempf = extra->scal_t->val[cell_id];

          else if (cs_glob_thermal_model->itherm == 2){

            cs_lnum_t mode  = 1;
            CS_PROCF (usthht,USTHHT) (&mode, &(extra->scal_t->val[cell_id]), &tempf);

            tempf = tempf + tkelvi;

          }

          else
            tempf = cs_glob_fluid_properties->t0;

          cs_real_t p_mass = cs_lagr_particle_get_real(particle, p_am, CS_LAGR_MASS);

          cs_real_t ddbr = sqrt(2.0 * _k_boltz * tempf / (p_mass * taup[ip]));

          cs_real_t tix2 = pow((taup[ip] * ddbr), 2) * (dtp - taup[ip] * (1.0 - aux1) * (3.0 - aux1) / 2.0);
          cs_real_t tiu2 = ddbr * ddbr * taup[ip] * (1.0 - exp( -2.0 * dtp / taup[ip])) / 2.0;

          cs_real_t tixiu  = pow((ddbr * taup[ip] * (1.0 - aux1)), 2) / 2.0;

          tbrix2 = tix2 - (tixiu * tixiu) / tiu2;

          if (tbrix2 > 0.0)
            tbrix2    = sqrt(tbrix2) * brgaus[ip * 6 + id];
          else
            tbrix2    = 0.0;

          if (tiu2 > 0.0)
            tbrix1    = tixiu / sqrt(tiu2) * brgaus[ip * 6 + id + 3];
          else
            tbrix1    = 0.0;

          if (tiu2 > 0.0) {

            tbriu      = sqrt(tiu2) * brgaus[ip * 6 + id + 3];
            terbru[ip] = sqrt(tiu2);

          }
          else {

            tbriu     = 0.0;
            terbru[ip]  = 0.0;

          }

        }
        else {

          tbrix1  = 0.0;
          tbrix2  = 0.0;
          tbriu   = 0.0;

        }

        /* Finalisation des ecritures */

        /* --> trajectoire */
        part_coords[id] = old_part_coords[id] + ter1x + ter2x + ter3x
                                              + ter4x + ter5x + tbrix1 + tbrix2;

        /* --> vitesse fluide vu     */
        part_vel_seen[id] = ter1f + ter2f + ter3f;

        /* --> vitesse particules    */
        part_vel[id] = ter1p + ter2p + ter3p + ter4p + ter5p + tbriu;

      }


    }

  }
}

/* ==============================================================================
 * INTEGRATION DES EDS PAR UN SCHEMA D'ORDRE 2 (sche2c)
 *    Lorsqu'il y a eu interaction avec une face de bord,
 *    les calculs de la vitesse de la particule et de
 *    la vitesse du fluide vu sont forcement a l'ordre 1
 *    (meme si on est a l'ordre 2 par ailleurs).
 * --------------------------------------------------------------------
 * Parameters
 * \param[in] taup      temps caracteristique dynamique
 * \param[in] tlag      temps caracteristique fluide
 * \param[in] piil      terme dans l'integration des eds up
 * \param[in] bx        caracteristiques de la turbulence
 * \param[in] tsfext    infos pour couplage retour dynamique
 * \param[in] vagaus    variables aleatoires gaussiennes
 * \param[in] gradpr    pressure gradient
 * \param[in] romp      masse volumique des particules
 * \param[in] brgaus
 * \param[in] terbru
 * \param[in] fextla    champ de forces exterieur utilisateur (m/s2)
 * -----------------------------------------------------------------------------*/

static void
_lages2(cs_real_t     dtp,
        cs_real_t    *taup,
        cs_real_3_t  *tlag,
        cs_real_3_t  *piil,
        cs_real_t    *bx,
        cs_real_t    *tsfext,
        cs_real_33_t *vagaus,
        cs_real_3_t  *gradpr,
        cs_real_t    *romp,
        cs_real_t    *brgaus,
        cs_real_t    *terbru,
        cs_real_3_t  *fextla)
{
  cs_real_t  aux0, aux1, aux2, aux3, aux4, aux5, aux6, aux7, aux8, aux9;
  cs_real_t  aux10, aux11, aux12, aux14, aux15, aux16, aux17, aux18, aux19;
  cs_real_t  aux20;
  cs_real_t  ter1, ter2, ter3, ter4, ter5;
  cs_real_t  sige, tapn, gamma2, omegam;
  cs_real_t  omega2, grgam2, gagam, gaome;
  cs_real_t  p11, p21, p22, p31, p32, p33;
  cs_real_t  tbriu;


  /* Particles management */
  cs_lagr_particle_set_t  *p_set = cs_glob_lagr_particle_set;
  const cs_lagr_attribute_map_t  *p_am = p_set->p_am;

  cs_lagr_extra_module_t *extra = cs_get_lagr_extra_module();

  /* ====================================================================   */
  /* 0.  GESTION MEMOIRE  */
  /* ====================================================================   */

  /* ==============================================================================*/
  /* 1. INITIALISATIONS                                                            */
  /* ==============================================================================*/
  cs_real_t grav[] = {cs_glob_physical_constants->gx,
                      cs_glob_physical_constants->gy,
                      cs_glob_physical_constants->gz};

  cs_lnum_t nor = cs_glob_lagr_time_step->nor;

  cs_real_t *auxl;
  BFT_MALLOC(auxl, p_set->n_particles*6, cs_real_t);

  /* =============================================================================
   * 2. INTEGRATION DES EDS SUR LES PARTICULES
   * =============================================================================*/

  /* =============================================================================
   * 2.1 CALCUL A CHAQUE SOUS PAS DE TEMPS
   * ==============================================================================*/
  /* --> Calcul de tau_p*A_p et de II*TL+<u> :
   *     -------------------------------------*/

  for (cs_lnum_t id = 0; id < 3; id++) {

    for (cs_lnum_t ip = 0; ip < p_set->n_particles; ip++) {

      unsigned char *particle = p_set->p_buffer + p_am->extents * ip;

      cs_lnum_t cell_id = cs_lagr_particle_get_cell_id(particle, p_am);

      if (cell_id >= 0) {

        cs_real_t rom   = extra->cromf->val[cell_id];

        if (cs_glob_lagr_time_scheme->iadded_mass == 0)
          auxl[ip * 6 + id] = (- gradpr[cell_id][id] / romp[ip] + grav[id] + fextla[ip][id]) * taup[ip];

        /* Added-mass term?     */
        else
          auxl[ip * 6 + id] =   ( - gradpr[cell_id][id] / romp[ip]
                                  * (1.0 + 0.5 * cs_glob_lagr_time_scheme->added_mass_const)
                                  / (1.0 + 0.5 * cs_glob_lagr_time_scheme->added_mass_const * rom / romp[ip])
                                  + grav[id] + fextla[ip][id] )
                              * taup[ip];

        auxl[ip * 6 + id + 3] =   piil[ip][id] * tlag[ip][id]
                                + extra->vel->vals[nor][cell_id];

      }

    }

  }

  /* ==============================================================================*/
  /* 2.2 ETAPE DE PREDICTION : */
  /* ==============================================================================*/

  if (nor == 1) {

    /* --> Sauvegarde de tau_p^n */
    for (cs_lnum_t ip = 0; ip < p_set->n_particles; ip++) {

      unsigned char *particle = p_set->p_buffer + p_am->extents * ip;

      if (cs_lagr_particle_get_real(particle, p_am, CS_LAGR_CELL_NUM) >= 0)
        cs_lagr_particle_set_real(particle, p_am, CS_LAGR_TAUP_AUX, taup[ip]);

    }

    /* --> Sauvegarde couplage   */
    if (cs_glob_lagr_time_scheme->iilagr == 2) {

      for (cs_lnum_t ip = 0; ip < p_set->n_particles; ip++) {

        unsigned char *particle = p_set->p_buffer + p_am->extents * ip;

        if (cs_lagr_particle_get_real(particle, p_am, CS_LAGR_CELL_NUM) >= 0) {

          aux0     = -dtp / taup[ip];
          aux1     =  exp (aux0);
          tsfext[ip]      =   taup[ip]
                            * cs_lagr_particle_get_real(particle, p_am, CS_LAGR_MASS)
                            * (-aux1 + (aux1 - 1.0) / aux0);

        }

      }

    }

    /* --> Chargement des termes a t = t_n :    */
    for (cs_lnum_t id = 0; id < 3; id++) {

      for (cs_lnum_t ip = 0; ip < p_set->n_particles; ip++) {

        unsigned char *particle = p_set->p_buffer + p_am->extents * ip;

        cs_lnum_t cell_id = cs_lagr_particle_get_cell_id(particle, p_am);

        if (cell_id >= 0) {

          cs_real_t *old_part_vel      = cs_lagr_particle_attr_n(particle, p_am,
                                                                 1, CS_LAGR_VELOCITY);
          cs_real_t *old_part_vel_seen = cs_lagr_particle_attr_n(particle, p_am,
                                                                 1, CS_LAGR_VELOCITY_SEEN);
          cs_real_t *pred_part_vel_seen = cs_lagr_particle_attr(particle, p_am,
                                                                CS_LAGR_PRED_VELOCITY_SEEN);
          cs_real_t *pred_part_vel = cs_lagr_particle_attr(particle, p_am,
                                                           CS_LAGR_PRED_VELOCITY);


          aux0    =  -dtp / taup[ip];
          aux1    =  -dtp / tlag[ip][id];
          aux2    = exp(aux0);
          aux3    = exp(aux1);
          aux4    = tlag[ip][id] / (tlag[ip][id] - taup[ip]);
          aux5    = aux3 - aux2;

          pred_part_vel_seen[id] =   0.5 * old_part_vel_seen[id]
                                   * aux3 + auxl[ip * 6 + id + 3]
                                   * (-aux3 + (aux3 - 1.0) / aux1);

          ter1    = 0.5 * old_part_vel[id] * aux2;
          ter2    = 0.5 * old_part_vel_seen[id] * aux4 * aux5;
          ter3    = auxl[ip * 6 + id + 3] * (-aux2 + ((tlag[ip][id] + taup[ip]) / dtp) * (1.0 - aux2)
                                             - (1.0 + tlag[ip][id] / dtp) * aux4 * aux5);
          ter4    = auxl[ip * 6 + id] * ( -aux2 + (aux2 - 1.0) / aux0);
          pred_part_vel[id] = ter1 + ter2 + ter3 + ter4;

        }

      }

    }

    /* --> Schema d'Euler : */
    _lages1 (dtp, taup, tlag, piil, bx, vagaus, gradpr, romp, brgaus, terbru, fextla);
  }

  else {

    /* ==============================================================================*/
    /* 2.2 ETAPE DE CORRECTION :                                                     */
    /* ==============================================================================*/
    /* --> Calcul de Us :                                                            */

    for (cs_lnum_t id = 0; id < 3; id++) {

      for (cs_lnum_t ip = 0; ip < p_set->n_particles; ip++) {

        unsigned char *particle = p_set->p_buffer + p_am->extents * ip;

        cs_lnum_t cell_id = cs_lagr_particle_get_cell_id(particle, p_am);

        if (    cell_id >= 0
             && cs_lagr_particle_get_lnum(particle, p_am,CS_LAGR_SWITCH_ORDER_1) == 0) {

          cs_real_t *part_vel
            = cs_lagr_particle_attr(particle, p_am, CS_LAGR_VELOCITY);
          cs_real_t *part_vel_seen
            = cs_lagr_particle_attr(particle, p_am, CS_LAGR_VELOCITY_SEEN);
          cs_real_t *old_part_vel
            = cs_lagr_particle_attr_n(particle, p_am, 1, CS_LAGR_VELOCITY);
          cs_real_t *old_part_vel_seen
            = cs_lagr_particle_attr_n(particle, p_am, 1, CS_LAGR_VELOCITY_SEEN);
          cs_real_t *pred_part_vel_seen
            = cs_lagr_particle_attr(particle, p_am, CS_LAGR_PRED_VELOCITY_SEEN);
          cs_real_t *pred_part_vel
            = cs_lagr_particle_attr(particle, p_am, CS_LAGR_PRED_VELOCITY);

          aux0    =  -dtp / taup[ip];
          aux1    =  -dtp / tlag[ip][id];
          aux2    = exp(aux0);
          aux3    = exp(aux1);
          aux4    = tlag[ip][id] / (tlag[ip][id] - taup[ip]);
          aux5    = aux3 - aux2;
          aux6    = aux3 * aux3;

          ter1    = 0.5 * old_part_vel_seen[id] * aux3;
          ter2    = auxl[ip * 6 + id + 3] * (1.0 - (aux3 - 1.0) / aux1);
          ter3    =  -aux6 + (aux6 - 1.0) / (2.0 * aux1);
          ter4    = 1.0 - (aux6 - 1.0) / (2.0 * aux1);

          sige    =   (  ter3 * bx[p_set->n_particles * (3 * (nor-1) + id) + ip]
                       + ter4 * bx[p_set->n_particles * (3 * nor + id) + ip])
                    * (1.0 / (1.0 - aux6));

          ter5    = 0.5 * tlag[ip][id] * (1.0 - aux6);


          part_vel_seen[id] = pred_part_vel_seen[id] + ter1 + ter2 + sige * sqrt(ter5) * vagaus[ip][id][0];

          /* --> Calcul de Up :   */
          ter1    = 0.5 * old_part_vel[id] * aux2;
          ter2    = 0.5 * old_part_vel_seen[id] * aux4 * aux5;
          ter3    = auxl[ip * 6 + id + 3]
            * (1.0 - ((tlag[ip][id] + taup[ip]) / dtp) * (1.0 - aux2) + (tlag[ip][id] / dtp) * aux4 * aux5)
            + auxl[ip * 6 + id] * (1.0 - (aux2 - 1.0) / aux0);

          tapn    = cs_lagr_particle_get_real(particle, p_am, CS_LAGR_TAUP_AUX);

          aux7    = exp( -dtp / tapn);
          aux8    = 1.0 - aux3 * aux7;
          aux9    = 1.0 - aux6;
          aux10   = 1.0 - aux7 * aux7;
          aux11   = tapn / (tlag[ip][id] + tapn);
          aux12   = tlag[ip][id] / (tlag[ip][id] - tapn);
          aux14   = tlag[ip][id] - tapn;
          aux15   = tlag[ip][id] * (1.0 - aux3);
          aux16   = tapn * (1.0 - aux7);
          aux17   = sige * sige * aux12 * aux12;
          aux18   = 0.5 * tlag[ip][id] * aux9;
          aux19   = 0.5 * tapn * aux10;
          aux20   = tlag[ip][id] * aux11 * aux8;

          /* ---> calcul de la matrice de correlation */
          gamma2  = sige * sige * aux18;
          grgam2  = aux17 * (aux18 - 2.0 * aux20 + aux19);
          gagam   = sige * sige * aux12 * (aux18 - aux20);
          omega2  = aux17 * (aux14 * (aux14 * dtp - 2.0 * tlag[ip][id] * aux15 + 2.0 * tapn * aux16) + tlag[ip][id] * tlag[ip][id] * aux18 + tapn * tapn * aux19- 2.0 * tlag[ip][id] * tapn * aux20);
          omegam  = aux14 * (1.0 - aux3) - aux18 + tapn * aux11 * aux8;
          omegam  = omegam * sige * sige * aux12 * tlag[ip][id];
          gaome   = aux17 * (aux14 * (aux15 - aux16) - tlag[ip][id] * aux18 - tapn * aux19 + tapn * tlag[ip][id] * aux8);

          /* ---> simulation du vecteur Gaussien */

          p11     = sqrt(CS_MAX(0.0, gamma2));
          if (p11 > cs_math_epzero) {

            p21  = omegam / p11;
            p22  = omega2 - p21 * p21;
            p22  = sqrt(CS_MAX(0.0, p22));

          }
          else {

            p21  = 0.0;
            p22  = 0.0;

          }

          if (p11 > cs_math_epzero)
            p31  = gagam / p11;
          else
            p31  = 0.0;

          if (p22 > cs_math_epzero)
            p32  = (gaome - p31 * p21) / p22;
          else
            p32  = 0.0;

          p33     = grgam2 - p31 * p31 - p32 * p32;
          p33     = sqrt(CS_MAX(0.0, p33));
          ter4    = p31 * vagaus[ip][id][0] + p32 * vagaus[ip][id][1] + p33 * vagaus[ip][id][2];

          /* ---> Calcul des Termes dans le cas du mouvement Brownien     */
          if (cs_glob_lagr_brownian->lamvbr == 1)
            tbriu = terbru[ip] * brgaus[ip * 6 + id + 3];
          else
            tbriu = 0.0;

          /* ---> finalisation de l'ecriture     */

          part_vel[id] = pred_part_vel[id] + ter1 + ter2 + ter3 + ter4 + tbriu;

        }

      }

    }

  }

  BFT_FREE(auxl);
}

/* ----------------------------------------------------------------------------
 *
 * Deposition submodel:
 *  1/ Modification of the coordinate system (global ->local)
 *  2/ Call of subroutine lagcli
 *  3/ Integration of the stochastic differential equations
 *     in the 2 directions different from the normal to the boundary face
 *  4/ Modification of the coordinate system (local ->global)
 *  5/ Update of the particle position
 *
 * parameters:
 *    ifac              <--
 *    ip                <--
 *    taup(nbpart)      <--     temps caracteristique dynamique
 *    piil(nbpart,3)    <--     terme dans l'integration des eds up
 *    vagaus            <--     variables aleatoires gaussiennes
 *    (nbpart,nvgaus)
 *    gradpr(3,ncel)    <--     gradient de pression
 *    romp              <--     masse volumique des particules
 *    tempf             <--     temperature of the fluid (K)
 *    romf              <--     density of the fluid
 *    ustar             <--     friction velocity
 *    lvisq             <--     wall-unit lengthscale
 *    tvisq             <--     wall-unit timescale
 *    depint            <--     interface location near-wall/core-flow
 * ----------------------------------------------------------------------------- */

static void
_lagesd(cs_real_t     dtp,
        cs_lnum_t    *ifac,
        cs_lnum_t    *ip,
        cs_real_t    *taup,
        cs_real_3_t  *piil,
        cs_real_33_t *vagaus,
        cs_real_3_t  *gradpr,
        cs_real_t    *romp,
        cs_real_t    *tempf,
        cs_real_t    *romf,
        cs_real_t    *ustar,
        cs_real_t    *lvisq,
        cs_real_t    *tvisq,
        cs_real_t    *depint)
{
  /* mesh and mesh quantities */
  cs_mesh_quantities_t *mq = cs_glob_mesh_quantities;

  /* Particles management */
  cs_lagr_particle_set_t  *p_set = cs_glob_lagr_particle_set;
  const cs_lagr_attribute_map_t  *p_am = p_set->p_am;

  cs_lagr_extra_module_t *extra = cs_get_lagr_extra_module();

  /* Hydrodynamic drag on a deposited particle     */
  cs_real_t drag[3];

  /* Hydrodynamic torque on a deposited particle   */
  cs_real_t tordrg[3], tordrg_norm;

  /* Map field arrays     */
  cs_real_t *vela = extra->vel->vals[1];

  /* ===========================================================================
   * 1. INITIALISATIONS
   * ======================================================================== */

  cs_real_3_t grav = {cs_glob_physical_constants->gx,
                      cs_glob_physical_constants->gy,
                      cs_glob_physical_constants->gz};

  /* particle data */
  unsigned char *particle = p_set->p_buffer + p_am->extents * *ip;

  cs_real_t p_mass = cs_lagr_particle_get_real(particle, p_am,
                                               CS_LAGR_MASS);
  cs_real_t p_diam = cs_lagr_particle_get_real(particle, p_am,
                                               CS_LAGR_DIAMETER);
  cs_real_t p_stat_w = cs_lagr_particle_get_real(particle, p_am,
                                                 CS_LAGR_STAT_WEIGHT);

  cs_lnum_t cell_id = cs_lagr_particle_get_cell_id(particle, p_am);

  /* Friction velocity    */

  *ifac  = cs_lagr_particle_get_lnum(particle, p_am,
                                     CS_LAGR_NEIGHBOR_FACE_ID);
  *ustar = extra->uetbor[*ifac];

  /* Constants for the calculation of bxp and tlp  */
  cs_real_t c0 = 2.1;
  cs_real_t cl = 1.0 / (0.5 + (3.0 / 4.0) * c0);

  /* Pointer on the density w.r.t the flow    */
  *romf = extra->cromf->val[cell_id];

  cs_real_t visccf = extra->viscl->val[cell_id] / *romf;

  cs_real_t yplus = cs_lagr_particle_get_real(particle, p_am,
                                              CS_LAGR_YPLUS);

  /* Turbulent kinetic energy and dissipation w.r.t y+  */
  cs_real_t energi, dissip;
  if (yplus <= 5.0) {

    energi = 0.1 * (pow (yplus, 2)) * pow (*ustar, 2);
    dissip = 0.2 * pow (*ustar, 4) / visccf;

  }

  else if (   yplus > 5.0 && yplus <= 30.0) {

    energi = pow (*ustar, 2) / pow ((0.09), 0.5);
    dissip = 0.2 * pow (*ustar, 4) / visccf;

  }
  else if (   yplus > 30.0 && yplus <= 100.0) {

    energi   = pow (*ustar, 2) / pow ((0.09), 0.5);
    dissip   = pow (*ustar, 4) / (0.41 * yplus * visccf);

  }

  /* ===========================================================================
   * 2. Reference frame change:
   * --------------------------
   * global reference frame --> local reference frame for the boundary face
   * ======================================================================== */

  const cs_real_3_t *rot_m
    = (const cs_real_3_t *)cs_glob_lagr_b_face_proj[*ifac];

  /* 2.1 - particle velocity   */

  cs_real_t *old_part_vel = cs_lagr_particle_attr_n(particle, p_am, 1,
                                                    CS_LAGR_VELOCITY);
  cs_real_t vpart[3];

  cs_math_33_3_product(rot_m, old_part_vel, vpart);

  cs_real_t vpart0[3] = {vpart[0], vpart[1], vpart[2]};

  /* 2.2 - flow-seen velocity  */

  cs_real_t *old_part_vel_seen = cs_lagr_particle_attr_n(particle, p_am, 1,
                                                         CS_LAGR_VELOCITY_SEEN);
  cs_real_t vvue[3];

  cs_math_33_3_product(rot_m, old_part_vel_seen, vvue);

  /* 2.3 - Gravity vector */

  cs_real_t ggp[3];

  cs_math_33_3_product(rot_m, grav, ggp);

  /* 2.4 - flow velocity  */

  cs_real_t vflui[3];

  cs_math_33_3_product(rot_m, vela, vflui);

  cs_real_t norm = sqrt(pow(vflui[1],2)+pow(vflui[2],2));

  /* Velocity norm w.r.t y+    */
  cs_real_t norm_vit;

  if (yplus <= 5.0)
    norm_vit = yplus * *ustar;

  else if (   yplus > 5.0 && yplus <= 30.0)
    norm_vit = ( -3.05 + 5.0 * log(yplus)) * *ustar;

  else if (   yplus > 30.0
           && yplus < 100.0)
    norm_vit = (2.5 * log (yplus) + 5.5) * *ustar;

  if (norm_vit > 0.) {
    vflui[1] = norm_vit * vflui[1] / norm;
    vflui[2] = norm_vit * vflui[2] / norm;
  }
  else {
    vflui[1] = 0.;
    vflui[2] = 0.;
  }

  /* 2.5 - pressure gradient   */

  cs_real_t gdpr[3];

  cs_math_33_3_product(rot_m, gradpr[cell_id], gdpr);

  /* 2.6 - "piil" term    */

  cs_real_t piilp[3];

  cs_math_33_3_product(rot_m, piil[*ip], piilp);

  /* 2.7 - tlag */

  cs_real_t tlp = cs_math_epzero;
  if (energi > 0.0) {

    tlp = cl * energi / dissip;
    tlp = CS_MAX(tlp, cs_math_epzero);

  }

  /* 2.8 - bx   */
  cs_real_t bxp = sqrt(c0 * dissip);

  /* =========================================================================
   * 3. Integration of the EDS on the particles
   * =========================================================================*/

  /* Retrieve of the turbulent kinetic energy */
  cs_real_t  enertur;
  if (extra->itytur == 2 || extra->iturb == 50 || extra->iturb == 60)
    enertur  = extra->cvar_k->vals[1][cell_id];

  else if (extra->itytur == 3)
    enertur  = 0.5 * (  extra->cvar_r11->vals[1][cell_id]
                      + extra->cvar_r22->vals[1][cell_id]
                      + extra->cvar_r33->vals[1][cell_id]);

  cs_lnum_t marko  = cs_lagr_particle_get_lnum(particle, p_am,
                                               CS_LAGR_MARKO_VALUE);
  cs_real_t interf = cs_lagr_particle_get_real(particle, p_am, CS_LAGR_INTERF);
  cs_real_3_t depl;

  cs_lagr_deposition(dtp,
                     &marko,
                     tempf,
                     lvisq,
                     tvisq,
                     &vpart[0],
                     &vvue[0],
                     &depl[0],
                     &p_diam,
                     &romp[*ip],
                     &taup[*ip],
                     &yplus,
                     &interf,
                     &enertur,
                     &ggp[0],
                     &vflui[0],
                     &gdpr[0],
                     &piilp[0],
                     depint);

  if (cs_lagr_particle_get_lnum(particle, p_am, CS_LAGR_DEPOSITION_FLAG) != CS_LAGR_PART_IN_FLOW) {

    depl[0]  = 0.0;
    vpart[0] = 0.0;

  }

  /* Integration in the 2 other directions    */
  if (cs_lagr_particle_get_lnum(particle, p_am,
                                CS_LAGR_DEPOSITION_FLAG) == CS_LAGR_PART_IN_FLOW) {

    for (cs_lnum_t id = 1; id < 3; id++) {

      cs_real_t tci   = piilp[id] * tlp + vflui[id];
      cs_real_t force = (- gdpr[id] / romp[*ip] + ggp[id]) * taup[*ip];
      cs_real_t aux1  = exp ( -dtp / taup[*ip]);
      cs_real_t aux2  = exp ( -dtp / tlp);
      cs_real_t aux3  = tlp / (tlp - taup[*ip]);
      cs_real_t aux4  = tlp / (tlp + taup[*ip]);
      cs_real_t aux5  = tlp * (1.0 - aux2);
      cs_real_t aux6  = bxp * bxp * tlp;
      cs_real_t aux7  = tlp - taup[*ip];
      cs_real_t aux8  = bxp * bxp * pow (aux3, 2);

      /* --> Terms for the trajectory   */
      cs_real_t aa    = taup[*ip] * (1.0 - aux1);
      cs_real_t bb    = (aux5 - aa) * aux3;
      cs_real_t cc    = dtp - aa - bb;
      cs_real_t ter1x = aa * vpart[id];
      cs_real_t ter2x = bb * vvue[id];
      cs_real_t ter3x = cc * tci;
      cs_real_t ter4x = (dtp - aa) * force;

      /* --> Terms for the flow-seen velocity     */
      cs_real_t ter1f = vvue[id] * aux2;
      cs_real_t ter2f = tci * (1.0 - aux2);

      /* --> Terms for the particles velocity     */
      cs_real_t dd    = aux3 * (aux2 - aux1);
      cs_real_t ee    = 1.0 - aux1;
      cs_real_t ter1p = vpart[id] * aux1;
      cs_real_t ter2p = vvue[id] * dd;
      cs_real_t ter3p = tci * (ee - dd);
      cs_real_t ter4p = force * ee;

      /* --> (2.3) Coefficients computation for the stochastic integrals:  */
      cs_real_t gama2  = 0.5 * (1.0 - aux2 * aux2);
      cs_real_t omegam = (0.5 * aux4 * (aux5 - aux2 * aa) - 0.5 * aux2 * bb) * sqrt (aux6);
      cs_real_t omega2 =   aux7
                         * (aux7 * dtp - 2.0 * (tlp * aux5 - taup[*ip] * aa))
                         + 0.5 * tlp * tlp * aux5 * (1.0 + aux2)
                         + 0.5 * pow(taup[*ip], 2.0) * aa * (1.0 + aux1)
                         - 2.0 * aux4 * tlp * pow(taup[*ip],2.0) * (1.0 - aux1 * aux2);
      omega2 *= aux8 ;

      cs_real_t  p11, p21, p22, p31, p32, p33;

      if (CS_ABS(gama2) >cs_math_epzero) {

        p21    = omegam / sqrt (gama2);
        p22    = omega2 - pow (p21, 2);
        p22    = sqrt(CS_MAX(0.0, p22));

      }
      else {

        p21    = 0.0;
        p22    = 0.0;

      }

      cs_real_t ter5x = p21 * vagaus[*ip][id][0] + p22 * vagaus[*ip][id][1];

      /* --> Integral for the flow-seen velocity  */

      p11   = sqrt(gama2 * aux6);
      cs_real_t ter3f = p11 * vagaus[*ip][id][0];

      /* --> Integral for particles velocity */

      cs_real_t aux9  = 0.5 * tlp * (1.0 - aux2 * aux2);
      cs_real_t aux10 = 0.5 * taup[*ip] * (1.0 - aux1 * aux1);
      cs_real_t aux11 = taup[*ip] * tlp * (1.0 - aux1 * aux2) / (taup[*ip] + tlp);
      cs_real_t grga2 = (aux9 - 2.0 * aux11 + aux10) * aux8;
      cs_real_t gagam = (aux9 - aux11) * (aux8 / aux3);
      cs_real_t gaome = (  (tlp - taup[*ip]) * (aux5 - aa)
                         - tlp * aux9
                         - taup[*ip] * aux10
                         + (tlp + taup[*ip]) * aux11) * aux8;

      if (p11 > cs_math_epzero)
        p31 = gagam / p11;

      else
        p31 = 0.0;

      if (p22 > cs_math_epzero)
        p32 = (gaome - p31 * p21) / p22;

      else
        p32 = 0.0;

      p33 = grga2 - pow(p31, 2) - pow(p32, 2);
      p33 = sqrt (CS_MAX(0.0, p33));

      cs_real_t ter5p =   p31 * vagaus[*ip][id][0]
                        + p32 * vagaus[*ip][id][1]
                        + p33 * vagaus[*ip][id][2];

      /* --> trajectory  */
      depl[id] = ter1x + ter2x + ter3x + ter4x + ter5x;

      /* --> flow-seen velocity    */
      vvue[id] = ter1f + ter2f + ter3f;

      /* --> particles velocity    */
      vpart[id] = ter1p + ter2p + ter3p + ter4p + ter5p;

    }

  }
  else {

    for (cs_lnum_t id = 1; id < 3; id++) {

      cs_real_t tci   = piilp[id] * tlp + vflui[id];
      cs_real_t aux2  = exp ( -dtp / tlp);
      cs_real_t aux6  = bxp * bxp * tlp;

      /* --> Terms for the flow-seen velocity     */
      cs_real_t ter1f = vvue[id] * aux2;
      cs_real_t ter2f = tci * (1.0 - aux2);

      /* --> (2.3) Coefficients computation for the stochastic integrals:  */
      cs_real_t gama2 = 0.5 * (1.0 - aux2 * aux2);

      /* --> Integral for the flow-seen velocity  */
      cs_real_t p11   = sqrt (gama2 * aux6);
      cs_real_t ter3f = p11 * vagaus[*ip][id][0];

      /* --> flow-seen velocity    */
      vvue[id] = ter1f + ter2f + ter3f;

    }

  }

  if (cs_glob_lagr_reentrained_model->ireent == 1) {

    if (cs_lagr_particle_get_real(particle, p_am,
                                  CS_LAGR_DEPOSITION_FLAG) != CS_LAGR_PART_IN_FLOW) {

      /* Resuspension model
       * Calculation of the hydrodynamic drag and torque
       * applied on the deposited particle   */

      drag[0] =  3.0 * cs_math_pi * p_diam
               * (vvue[0] - vpart[0]) * visccf * *romf * 3.39;
      tordrg[0] = 0.0;

      for (cs_lnum_t id = 1; id < 3; id++) {

        drag[id]   =  3.0 * cs_math_pi * p_diam
                    * (vvue[id] - vpart[id]) * visccf * *romf * 1.7;
        tordrg[id] = 1.4 * drag[id] * p_diam * 0.5;

      }

      /* Is there direct wall-normal lift-off of the particle ?  */
      if (   CS_ABS (drag[0]) > cs_lagr_particle_get_real(particle, p_am,
                                                       CS_LAGR_ADHESION_FORCE)
          && drag[0] < 0.0) {

        /* The particle is resuspended    */
        cs_lagr_particle_set_lnum(particle, p_am, CS_LAGR_DEPOSITION_FLAG, CS_LAGR_PART_IN_FLOW);
        cs_lagr_particle_set_real(particle, p_am, CS_LAGR_ADHESION_FORCE, 0.0);
        cs_lagr_particle_set_real(particle, p_am, CS_LAGR_ADHESION_TORQUE, 0.0);

        if (p_am->count[0][CS_LAGR_N_LARGE_ASPERITIES] > 0)
          cs_lagr_particle_set_lnum(particle, p_am,
                                    CS_LAGR_N_LARGE_ASPERITIES, 0);

        if (p_am->count[0][CS_LAGR_N_SMALL_ASPERITIES] > 0)
          cs_lagr_particle_set_lnum(particle, p_am,
                                    CS_LAGR_N_SMALL_ASPERITIES, 0);

        if (p_am->count[0][CS_LAGR_DISPLACEMENT_NORM] > 0)
          cs_lagr_particle_set_real(particle, p_am,
                                    CS_LAGR_DISPLACEMENT_NORM, 0.0);

        cs_real_t adhesion_force =
          cs_lagr_particle_get_real(particle, p_am, CS_LAGR_ADHESION_FORCE);
        vpart[0] = CS_MIN(-1.0 / p_mass * CS_ABS(drag[0] - adhesion_force) * dtp,
                          0.001);
        vpart[1] = 0.0;
        vpart[2] = 0.0;

        /* Update of the number and weight of resuspended particles     */
        p_set->n_part_resusp += 1;
        p_set->weight_resusp += p_stat_w;

        if (cs_glob_lagr_boundary_interactions->iflmbd == 1) {

          cs_lnum_t n_f_id
            = cs_lagr_particle_get_lnum(particle, p_am, CS_LAGR_NEIGHBOR_FACE_ID);

          cs_lnum_t nfabor = cs_glob_mesh->n_b_faces;

          bound_stat[n_f_id + nfabor * cs_glob_lagr_boundary_interactions->ires] += p_stat_w;

          bound_stat[n_f_id + nfabor * cs_glob_lagr_boundary_interactions->iflres] +=
            p_stat_w + (  p_stat_w * p_mass / mq->b_f_face_surf[n_f_id]);

          bound_stat[n_f_id + nfabor * cs_glob_lagr_boundary_interactions->iflm]  +=
            - (  p_stat_w * p_mass / mq->b_f_face_surf[n_f_id]);

        }

      }
      /* No direct normal lift-off */
      else {

        /* Calculation of the norm of the hydrodynamic
         * torque and drag (tangential) */

        tordrg_norm  = sqrt (pow (tordrg[1], 2) + pow (tordrg[2], 2));

        cs_real_t adh_tor[3];
        adh_tor[1]  = - cs_lagr_particle_get_real(particle, p_am,
                                                  CS_LAGR_ADHESION_TORQUE)
                      / tordrg_norm * tordrg[1];

        adh_tor[2]  = - cs_lagr_particle_get_real(particle, p_am,
                                                  CS_LAGR_ADHESION_TORQUE)
                      / tordrg_norm * tordrg[2];

        cs_real_t cst_1, cst_4;

        for (cs_lnum_t id = 1; id < 3; id++) {

          cs_real_t iner_tor = (7.0 / 5.0) * p_mass * pow ((p_diam * 0.5), 2);

          cst_4 =   6 * cs_math_pi * visccf
                  * *romf * 1.7 * 1.4
                  * pow(p_diam * 0.5, 2);

          cst_1 = cst_4 * (p_diam * 0.5) / iner_tor;

          vpart0[id] = vpart[id];
          vpart[id]  =  (vpart0[id] - vvue[id] - adh_tor[id] / cst_4)
                      * exp ( -cst_1 * dtp)
                      + vvue[id] + adh_tor[id] / cst_4 ;

        }

        cs_real_t scalax  = vpart[1] * vvue[1] + vpart[2] * vvue[2];

        if (scalax > 0.0) {

          /* The calculated particle velocity is aligned   */
          /* with the flow seen   */
          /* --> The particle starts or keep on rolling    */

          cs_lagr_particle_set_lnum(particle, p_am,CS_LAGR_DEPOSITION_FLAG, CS_LAGR_PART_ROLLING);

          vpart[0] = 0.0;

          for (cs_lnum_t id = 1; id < 3; id++) {

            if (CS_ABS (vpart[id]) > CS_ABS (vvue[id]))
              /* The velocity of the rolling particle cannot   */
              /* exceed the surrounding fluid velocity    */
              vpart[id] = vvue[id];

            cs_real_t kk  = vpart0[id] - vvue[id] - adh_tor[id] / cst_4;
            cs_real_t kkk = vvue[id] + adh_tor[id] / cst_4;

            depl[id] =   ( kkk * dtp + kk / cst_1 * (1. - exp ( -cst_1 * dtp)));

          }

        }
        /* if (scalax..) */
        else {

          /* The particle is not set into motion or stops
           * the flag is set to 10 and velocity and displacement are null */

          cs_lagr_particle_set_lnum(particle, p_am,CS_LAGR_DEPOSITION_FLAG, CS_LAGR_PART_NO_MOTION);

          for (cs_lnum_t id = 1; id < 3; id++) {

            depl[id]     = 0.0;
            vpart[id]    = 0.0;

          }

        } /* if (scalax..)   */

      }

    }

  }
  /* if ireent.eq.0 --> Motionless deposited particle   */
  else {

    if (cs_lagr_particle_get_lnum(particle, p_am,
                                  CS_LAGR_DEPOSITION_FLAG) != CS_LAGR_PART_IN_FLOW) {

      for (cs_lnum_t id = 1; id < 3; id++) {

        vpart[id] = 0.0;
        vvue[id]  = 0.0;
        depl[id]  = 0.0;

      }

    }

  }

  /* ===========================================================================
   * 3. Reference frame change:
   * --------------------------
   * local reference frame for the boundary face --> global reference frame
   * ======================================================================== */

  /* 3.1 - Displacement   */

  cs_real_t depg[3];

  cs_math_33t_3_product(rot_m, depl, depg);

  /* 3.2 - Particle velocity   */

  cs_real_t *part_vel = cs_lagr_particle_attr(particle, p_am,
                                              CS_LAGR_VELOCITY);

  cs_math_33t_3_product(rot_m, vpart, part_vel);

  /* 3.3 - flow-seen velocity  */

  cs_real_t *part_vel_seen = cs_lagr_particle_attr(particle, p_am,
                                                   CS_LAGR_VELOCITY_SEEN);

  cs_math_33t_3_product(rot_m, vvue, part_vel_seen);

  /* ===========================================================================
   * 5. Computation of the new particle position
   * ======================================================================== */

  cs_real_t *part_coords = cs_lagr_particle_attr(particle, p_am,
                                                 CS_LAGR_COORDS);
  for (cs_lnum_t id = 0 ; id < 3; id++)
    part_coords[id] += depg[id];

  return;

}

/*--------------------------------------------------------------*/
/* Deposition sub-model:
 *    Main subroutine of the submodel
 *    1/ Calculation of the normalized wall-normal distance of
 *           the boundary-cell particles
 *   2/ Sorting of the particles with respect to their normalized
 *           wall-normal distance
 *         * If y^+ > depint : the standard Langevin model is applied
 *         * If y^+ < depint : the deposition submodel is applied
 *
 * Parameters
 * \param[in] dtp
 * \param[in] taup(nbpart)           temps caracteristique dynamique
 * \param[in] tlag(nbpart)           temps caracteristique fluide
 * \param[in] piil(nbpart,3)         terme dans l'integration des eds up
 * \param[in] bx(nbpart,3,2)         caracteristiques de la turbulence
 * \param[in] vagaus(nbpart,nvgaus)  variables aleatoires gaussiennes
 * \param[in] gradpr(3,ncel)         gradient de pression
 * \param[in] romp                   masse volumique des particules
 * \param[in] fextla(ncelet,3)       champ de forces exterieur utilisateur (m/s2)
 * \param[in] vislen
 */
/*--------------------------------------------------------------*/

static void
_lagdep(cs_real_t     dtp,
        cs_real_t    *taup,
        cs_real_3_t  *tlag,
        cs_real_3_t  *piil,
        cs_real_t    *bx,
        cs_real_33_t *vagaus,
        cs_real_3_t  *gradpr,
        cs_real_t    *romp,
        cs_real_3_t  *fextla,
        cs_real_t    *vislen)
{
  /* Particles management */
  cs_lagr_particle_set_t  *p_set = cs_glob_lagr_particle_set;
  const cs_lagr_attribute_map_t  *p_am = p_set->p_am;
  const cs_mesh_quantities_t  *fvq = cs_glob_mesh_quantities;

  cs_lagr_extra_module_t *extra = cs_get_lagr_extra_module();

  /* Initialisations*/
  cs_real_3_t grav = {cs_glob_physical_constants->gx,
                      cs_glob_physical_constants->gy,
                      cs_glob_physical_constants->gz};

  cs_real_t tkelvi =  273.15;

  cs_real_t vitf = 0.0;

  cs_real_t aux1, aux2, aux3, aux4, aux5, aux6, aux7, aux8, aux9, aux10, aux11;
  cs_real_t ter1f, ter2f, ter3f;
  cs_real_t ter1p, ter2p, ter3p, ter4p, ter5p;
  cs_real_t ter1x, ter2x, ter3x, ter4x, ter5x;
  cs_real_t p11, p21, p22, p31, p32, p33;
  cs_real_t omega2, gama2, omegam;
  cs_real_t grga2, gagam, gaome;

  cs_lnum_t nor = cs_glob_lagr_time_step->nor;


  /* Interface location between near-wall region   */
  /* and core of the flow (normalized units)  */

  cs_real_t depint      = 100.0;

  /* loop on the particles  */
  for (cs_lnum_t ip = 0; ip < p_set->n_particles; ip++) {

    unsigned char *particle = p_set->p_buffer + p_am->extents * ip;

    cs_lnum_t cell_id = cs_lagr_particle_get_cell_id(particle, p_am);

    if (cell_id >= 0 &&
        cs_lagr_particle_get_lnum(particle, p_am, CS_LAGR_DEPOSITION_FLAG)
        != CS_LAGR_PART_IMPOSED_MOTION) {

      cs_real_t *old_part_vel      = cs_lagr_particle_attr_n(particle, p_am, 1,
                                                             CS_LAGR_VELOCITY);
      cs_real_t *old_part_vel_seen = cs_lagr_particle_attr_n(particle, p_am, 1,
                                                             CS_LAGR_VELOCITY_SEEN);
      cs_real_t *part_vel          = cs_lagr_particle_attr(particle, p_am,
                                                           CS_LAGR_VELOCITY);
      cs_real_t *part_vel_seen     = cs_lagr_particle_attr(particle, p_am,
                                                           CS_LAGR_VELOCITY_SEEN);
      cs_real_t *part_coords       = cs_lagr_particle_attr(particle, p_am,
                                                           CS_LAGR_COORDS);
      cs_real_t *old_part_coords   = cs_lagr_particle_attr_n(particle, p_am, 1,
                                                             CS_LAGR_COORDS);

      cs_real_t romf = extra->cromf->val[cell_id];

      /* Fluid temperature computation depending on the type of flow  */
      cs_real_t tempf;

      if (   cs_glob_physical_model_flag[CS_COMBUSTION_COAL] >= 0
          || cs_glob_physical_model_flag[CS_COMBUSTION_PCLC] >= 0
          || cs_glob_physical_model_flag[CS_COMBUSTION_FUEL] >= 0)
        tempf = extra->t_gaz->val[cell_id];

      else if (   cs_glob_physical_model_flag[CS_COMBUSTION_3PT] >= 0
               || cs_glob_physical_model_flag[CS_COMBUSTION_EBU] >= 0
               || cs_glob_physical_model_flag[CS_ELECTRIC_ARCS] >= 0
               || cs_glob_physical_model_flag[CS_JOULE_EFFECT] >= 0)
        tempf = extra->temperature->val[cell_id];

      else if (   cs_glob_thermal_model->itherm == 1
                  && cs_glob_thermal_model->itpscl == 2)
        tempf = extra->scal_t->val[cell_id] + tkelvi;

      else if (   cs_glob_thermal_model->itherm == 1
                  && cs_glob_thermal_model->itpscl == 1)
        tempf = extra->scal_t->val[cell_id];

      else if (cs_glob_thermal_model->itherm == 2){

        cs_lnum_t mode  = 1;
        CS_PROCF (usthht,USTHHT) (&mode, &(extra->scal_t->val[cell_id]), &tempf);

        tempf = tempf + tkelvi;

      }

      else
        tempf = cs_glob_fluid_properties->t0;

      /* ==============================================================    */
      /*   If y^+ is greater than the interface location,   */
      /*   the standard model is applied     */
      /* ==============================================================    */

      if (cs_lagr_particle_get_real(particle, p_am, CS_LAGR_YPLUS) > depint &&
          cs_lagr_particle_get_lnum(particle, p_am, CS_LAGR_DEPOSITION_FLAG)
          == CS_LAGR_PART_IN_FLOW) {

        cs_lagr_particle_set_lnum(particle, p_am, CS_LAGR_MARKO_VALUE, -1);

        for (cs_lnum_t id = 0; id < 3; id++) {

          vitf = extra->vel->vals[1][cell_id * 3 + id];

          /* --> (2.1) Calcul preliminaires :    */
          /* ----------------------------   */
          /* calcul de II*TL+<u> et [(grad<P>/rhop+g)*tau_p+<Uf>] ?  */

          cs_real_t tci = piil[ip][id] * tlag[ip][id] + vitf;
          cs_real_t force = 0.0;

          if (cs_glob_lagr_time_scheme->iadded_mass == 0)
            force = (- gradpr[cell_id][id] / romp[ip]
                     + grav[id] + fextla[ip][id]) * taup[ip];

          /* Added-mass term?     */
          else
            force =   ( - gradpr[cell_id][id] / romp[ip]
                  * (1.0 + 0.5 * cs_glob_lagr_time_scheme->added_mass_const)
                  / (1.0 + 0.5 * cs_glob_lagr_time_scheme->added_mass_const * romf / romp[ip])
                  + grav[id] + fextla[ip][id])* taup[ip];


          /* --> (2.2) Calcul des coefficients/termes deterministes */
          /* ----------------------------------------------------    */

          aux1 = exp(-dtp / taup[ip]);
          aux2 = exp(-dtp / tlag[ip][id]);
          aux3 = tlag[ip][id] / (tlag[ip][id] - taup[ip]);
          aux4 = tlag[ip][id] / (tlag[ip][id] + taup[ip]);
          aux5 = tlag[ip][id] * (1.0 - aux2);
          aux6 = pow(bx[p_set->n_particles * (3 * nor + id) + ip], 2.0) * tlag[ip][id];
          aux7 = tlag[ip][id] - taup[ip];
          aux8 = pow(bx[p_set->n_particles * (3 * nor + id) + ip], 2.0) * pow(aux3, 2);

          /* --> trajectory terms */
          cs_real_t aa = taup[ip] * (1.0 - aux1);
          cs_real_t bb = (aux5 - aa) * aux3;
          cs_real_t cc = dtp - aa - bb;

          ter1x = aa * old_part_vel[id];
          ter2x = bb * old_part_vel_seen[id];
          ter3x = cc * tci;
          ter4x = (dtp - aa) * force;

          /* --> flow-seen velocity terms   */
          ter1f = old_part_vel_seen[id] * aux2;
          ter2f = tci * (1.0 - aux2);

          /* --> termes pour la vitesse des particules     */
          cs_real_t dd = aux3 * (aux2 - aux1);
          cs_real_t ee = 1.0 - aux1;

          ter1p = old_part_vel[id] * aux1;
          ter2p = old_part_vel_seen[id] * dd;
          ter3p = tci * (ee - dd);
          ter4p = force * ee;

          /* --> (2.3) Coefficients computation for the stochastic integral    */
          /* --> Integral for particles position */
          gama2  = 0.5 * (1.0 - aux2 * aux2);
          omegam = (0.5 * aux4 * (aux5 - aux2 * aa) - 0.5 * aux2 * bb) * sqrt(aux6);
          omega2 =  aux7 * (aux7 * dtp - 2.0 * (tlag[ip][id] * aux5 - taup[ip] * aa))
                   + 0.5 * tlag[ip][id] * tlag[ip][id] * aux5 * (1.0 + aux2)
                   + 0.5 * taup[ip] * taup[ip] * aa * (1.0 + aux1)
                   - 2.0 * aux4 * tlag[ip][id] * taup[ip] * taup[ip] * (1.0 - aux1 * aux2);
          omega2 = aux8 * omega2;

          if (CS_ABS(gama2) > cs_math_epzero) {

            p21 = omegam / sqrt(gama2);
            p22 = omega2 - pow(p21, 2);
            p22 = sqrt(CS_MAX(0.0, p22));

          }
          else {

            p21 = 0.0;
            p22 = 0.0;

          }

          ter5x = p21 * vagaus[ip][id][0] + p22 * vagaus[ip][id][1];

          /* --> integral for the flow-seen velocity  */
          p11   = sqrt(gama2 * aux6);
          ter3f = p11 * vagaus[ip][id][0];

          /* --> integral for the particles velocity  */
          aux9  = 0.5 * tlag[ip][id] * (1.0 - aux2 * aux2);
          aux10 = 0.5 * taup[ip] * (1.0 - aux1 * aux1);
          aux11 =   taup[ip] * tlag[ip][id]
                  * (1.0 - aux1 * aux2)
                  / (taup[ip] + tlag[ip][id]);

          grga2 = (aux9 - 2.0 * aux11 + aux10) * aux8;
          gagam = (aux9 - aux11) * (aux8 / aux3);
          gaome = ( (tlag[ip][id] - taup[ip]) * (aux5 - aa)
                    - tlag[ip][id] * aux9
                    - taup[ip] * aux10
                    + (tlag[ip][id] + taup[ip]) * aux11)
                  * aux8;

          if (p11 > cs_math_epzero)
            p31 = gagam / p11;
          else
            p31 = 0.0;

          if (p22 > cs_math_epzero)
            p32 = (gaome - p31 * p21) / p22;
          else
            p32 = 0.0;

          p33 = grga2 - pow(p31, 2) - pow(p32, 2);
          p33 = sqrt(CS_MAX(0.0, p33));
          ter5p = p31 * vagaus[ip][id][0] + p32 * vagaus[ip][id][1] + p33 * vagaus[ip][id][2];

          /*  Update of the particle state-vector     */

          part_coords[id] = old_part_coords[id] + ter1x + ter2x + ter3x + ter4x + ter5x;

          part_vel_seen[id] =  ter1f + ter2f + ter3f;

          part_vel[id]      = ter1p + ter2p + ter3p + ter4p + ter5p;

        }

      }
      /* ====================================================================
       *   Otherwise, the deposition submodel is applied :
       * =====================================================================  */
      else {

        if (  cs_lagr_particle_get_real(particle, p_am,
                                        CS_LAGR_YPLUS)
            < cs_lagr_particle_get_real(particle, p_am,
                                        CS_LAGR_INTERF) ) {

          if (cs_lagr_particle_get_lnum(particle, p_am, CS_LAGR_MARKO_VALUE) < 0)
            cs_lagr_particle_set_lnum(particle, p_am, CS_LAGR_MARKO_VALUE, 10);
          else
            cs_lagr_particle_set_lnum(particle, p_am, CS_LAGR_MARKO_VALUE, 0);

        }
        else {

          if (cs_lagr_particle_get_lnum(particle, p_am, CS_LAGR_MARKO_VALUE) < 0)
            cs_lagr_particle_set_lnum(particle, p_am, CS_LAGR_MARKO_VALUE, 20);

          else if (cs_lagr_particle_get_lnum(particle, p_am, CS_LAGR_MARKO_VALUE) == 0)
            cs_lagr_particle_set_lnum(particle, p_am, CS_LAGR_MARKO_VALUE, 30);

        }

        cs_lnum_t ifac = cs_lagr_particle_get_lnum
                           (particle, p_am, CS_LAGR_NEIGHBOR_FACE_ID);
        cs_real_t ustar = extra->uetbor[ifac];
        cs_real_t lvisq = vislen[ifac];

        cs_real_t tvisq;
        if (ustar > 0.0)
          tvisq = lvisq / ustar;
        else
          tvisq = cs_math_big_r;

        _lagesd(dtp,
                &ifac,
                &ip,
                taup,
                piil,
                vagaus,
                gradpr,
                romp,
                &tempf,
                &romf,
                &ustar,
                &lvisq,
                &tvisq,
                &depint);

      }

    }

    /* Specific treatment for particles with
     * DEPOSITION_FLAG == CS_LAGR_PART_IMPOSED_MOTION */
    else if (cell_id >= 0) {
      cs_real_t disp[3] = {0., 0., 0.};

      cs_real_t *old_part_coords = cs_lagr_particle_attr_n(particle, p_am, 1,
                                                           CS_LAGR_COORDS);
      cs_real_t *part_coords = cs_lagr_particle_attr(particle, p_am,
                                                     CS_LAGR_COORDS);

      cs_real_t *part_vel_seen = cs_lagr_particle_attr(particle, p_am,
                                                       CS_LAGR_VELOCITY_SEEN);

      cs_real_t *part_vel = cs_lagr_particle_attr(particle, p_am,
                                                  CS_LAGR_VELOCITY);

      cs_user_lagr_imposed_motion(old_part_coords,
                                  dtp,
                                  &disp);

      for (cs_lnum_t id = 0; id < 3; id++) {

        part_coords[id] = old_part_coords[id] + disp[id];

        part_vel_seen[id] =  0.0;

        part_vel[id] = disp[id]/dtp;

      }
    }
  }

}

/*============================================================================
 * Public function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------*/
/*!
 * \brief Integration of particle equations of motion:
 *
 * - Standard Model : First order  -> call of subroutine lages1
 *                    Second order -> call of subroutine lages2
 * - Deposition submodel (Guingo & Minier, 2008) if needed
 *
 * \param[in]  dt_p      lagrangian time step
 * \param[in]  taup      dynamic characteristic time
 * \param[in]  tlag      fluid characteristic time
 * \param[out] piil      terme in P-U SDE integration
 * \param[in]  bx        turbulence characteristics
 * \param[in]  tsfext    info for return coupling source terms
 * \param[in]  gradpr    pressure gradient
 * \param[in]  gradvf    fluid velocity gradient
 */
/*----------------------------------------------------------------------------*/

void
cs_lagr_sde(cs_real_t      dt_p,
            cs_real_t      taup[],
            cs_real_3_t    tlag[],
            cs_real_3_t    piil[],
            cs_real_t      bx[],
            cs_real_t      tsfext[],
            cs_real_3_t    gradpr[],
            cs_real_33_t   gradvf[],
            cs_real_t      terbru[],
            cs_real_t      vislen[])
{
  cs_lnum_t one = 1;
  cs_real_t *romp;

  cs_lagr_particle_set_t  *p_set = cs_glob_lagr_particle_set;
  const cs_lagr_attribute_map_t *p_am = p_set->p_am;

  BFT_MALLOC(romp, p_set->n_particles, cs_real_t);

  /* Computation of particle density     */
  cs_real_t aa = 6.0 / cs_math_pi;

  for (cs_lnum_t ip = 0; ip < p_set->n_particles; ip++) {

    unsigned char *particle = p_set->p_buffer + p_am->extents * ip;
    if (cs_lagr_particle_get_cell_id(particle, p_am) >= 0) {

      cs_real_t d3 = pow(cs_lagr_particle_get_real(particle, p_am,
                                                   CS_LAGR_DIAMETER),3.0);
      romp[ip] = aa * cs_lagr_particle_get_real(particle, p_am,
                                                CS_LAGR_MASS) / d3;

    }

  }

  /* ====================================================================
   * 2.  Management of user external force fields
   * ====================================================================   */

  /* Allocate temporay arrays  */
  cs_real_3_t *fextla;
  BFT_MALLOC(fextla, p_set->n_particles, cs_real_3_t);

  cs_real_33_t *vagaus;
  BFT_MALLOC(vagaus, p_set->n_particles, cs_real_33_t);

  /* Random values   */
  if (cs_glob_lagr_time_scheme->idistu == 1) {

    if (p_set->n_particles > 0) {

      for (cs_lnum_t ivf = 0; ivf < 3; ivf++) {

        for (cs_lnum_t id = 0; id < 3; id++) {

          for (cs_lnum_t ip = 0; ip < p_set->n_particles; ip++)
            CS_PROCF(normalen, NORMALEN)(&one, &(vagaus[ip][id][ivf]));

        }

      }

    }

  }
  else {

    for (cs_lnum_t ivf = 0; ivf < 3; ivf++) {

      for (cs_lnum_t id = 0; id < 3; id++) {

        for (cs_lnum_t ip = 0; ip < p_set->n_particles; ip++)
          vagaus[ip][id][ivf] = 0.0;

      }

    }

  }

  cs_real_t *brgaus = NULL;

  /* Brownian movement */

  if (cs_glob_lagr_brownian->lamvbr == 1) {

    BFT_MALLOC(brgaus, cs_glob_lagr_const_dim->nbrgau * p_set->n_particles, cs_real_t);

    for (cs_lnum_t ivf = 0; ivf < cs_glob_lagr_const_dim->nbrgau; ivf++) {

      for (cs_lnum_t ip = 0; ip < p_set->n_particles; ip++)
        CS_PROCF(normalen, NORMALEN)
          (&one, &(brgaus[ip * cs_glob_lagr_const_dim->nbrgau + ivf]));

    }

  }

  for (cs_lnum_t ip = 0; ip < p_set->n_particles; ip++) {
    fextla[ip][0] = 0.0;
    fextla[ip][1] = 0.0;
    fextla[ip][2] = 0.0;
  }

  cs_user_lagr_ef(dt_p,
                  (const cs_real_t *)taup,
                  (const cs_real_3_t *)tlag,
                  (const cs_real_3_t *)piil,
                  (const cs_real_t *)bx,
                  (const cs_real_t *)tsfext,
                  (const cs_real_33_t *)vagaus,
                  (const cs_real_3_t *)gradpr,
                  (const cs_real_33_t *)gradvf,
                  romp,
                  fextla);

  /* First order
     ----------- */

  if (cs_glob_lagr_time_scheme->t_order == 1) {

    /* If no deposition sub-model is activated, call of subroutine lages1
       for every particle */

    if (cs_glob_lagr_model->deposition <= 0)
      _lages1(dt_p, taup, tlag, piil, bx, vagaus, gradpr, romp, brgaus, terbru, fextla);

    /* Management of the deposition submodel */

    else
      _lagdep(dt_p, taup, tlag, piil, bx, vagaus, gradpr, romp, fextla, vislen);

  }

  /* Second order
     ------------ */

  else
    _lages2(dt_p, taup, tlag, piil, bx, tsfext,
            vagaus, gradpr, romp, brgaus, terbru, fextla);

  /* Free memory */
  if (cs_glob_lagr_brownian->lamvbr == 1)
    BFT_FREE(brgaus);

  BFT_FREE(romp);
  BFT_FREE(vagaus);
  BFT_FREE(fextla);
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Integration of a stochastic differential equation (SDE) for
 *        a user particle variable (attribute).
 *
 * \f[
 *  \frac{dV}{dt} = \frac{V - PIP}{TCARAC}
 * ]\f
 *
 * When there is interaction with a boundary face, the integration
 * degenerates to order 1 (even if the 2nd order scheme is active).
 *
 * \param[in]  attr    attribute/variable
 * \param[in]  tcarac  variable characteristic time
 * \param[in]  pip     right-hand side associated with SDE
 */
/*----------------------------------------------------------------------------*/

void
cs_lagr_sde_attr(cs_lagr_attribute_t   attr,
                 cs_real_t            *tcarac,
                 cs_real_t            *pip)
{
  /* Particles management */
  cs_lagr_particle_set_t         *p_set = cs_glob_lagr_particle_set;
  const cs_lagr_attribute_map_t  *p_am  = p_set->p_am;

  int ltsvar;

  if (p_set->p_am->source_term_displ[attr] >= 0)
    ltsvar = 1;
  else
    ltsvar = 0;

  int nor = cs_glob_lagr_time_step->nor;

  assert(nor == 1 || nor == 2);

  if (nor == 1) {

    for (cs_lnum_t npt = 0; npt < p_set->n_particles; npt++) {

      unsigned char *particle = p_set->p_buffer + p_am->extents * npt;

      if (cs_lagr_particle_get_cell_id(particle, p_am) >= 0) {

        if (tcarac[npt] <= 0.0)
          bft_error
            (__FILE__, __LINE__, 0,
             _("@\n"
               "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
               "@\n"
               "@ @@ ATTENTION : ARRET A L''EXECUTION DU MODULE LAGRANGIEN\n"
               "@    =========\n"
               "@\n"
               "@    LE TEMPS CARACTERISTIQUE LIE A L'EQUATION\n"
               "@      DIFFERENTIELLE STOCHASTIQUE DE LA VARIABLE\n"
               "@      NUMERO %d UNE VALEUR NON PERMISE (CS_LAGR_SDE).\n"
               "@\n"
               "@    TCARAC DEVRAIT ETRE UN ENTIER STRICTEMENT POSITIF\n"
               "@       IL VAUT ICI TCARAC = %e11.4\n"
               "@       POUR LA PARTICULE NUMERO %d\n"
               "@\n"
               "@  Le calcul ne sera pas execute.\n"
               "@\n"
               "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
               "@"),
             attr, tcarac[npt], npt);

        cs_real_t aux1 = cs_glob_lagr_time_step->dtp/tcarac[npt];
        cs_real_t aux2 = exp(-aux1);
        cs_real_t ter1 = cs_lagr_particle_get_real(particle, p_am, attr);
        cs_real_t ter2 = pip[npt] * (1.0 - aux2);

        /* Pour le cas NORDRE= 1 ou s'il y a rebond,     */
        /* le ETTP suivant est le resultat final    */
        cs_lagr_particle_set_real(particle, p_am, attr, ter1 + ter2);

        /* Pour le cas NORDRE= 2, on calcule en plus TSVAR pour NOR= 2  */
        if (ltsvar) {
          cs_real_t *part_ptsvar = cs_lagr_particles_source_terms(p_set, npt, attr);
          cs_real_t ter3 = ( -aux2 + (1.0 - aux2) / aux1) * pip[npt];
          *part_ptsvar = 0.5 * ter1 + ter3;

        }

      }

    }

  }
  else if (nor == 2) {

    for (cs_lnum_t npt = 0; npt < p_set->n_particles; npt++) {

      unsigned char *particle = p_set->p_buffer + p_am->extents * npt;

      if (   cs_lagr_particle_get_cell_id(particle, p_am) >= 0
          && cs_lagr_particle_get_lnum(particle, p_am, CS_LAGR_SWITCH_ORDER_1) == 0) {

        if (tcarac [npt] <= 0.0)
          bft_error
            (__FILE__, __LINE__, 0,
             _("@\n"
               "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
               "@\n"
               "@ @@ ATTENTION : ARRET A L''EXECUTION DU MODULE LAGRANGIEN\n"
               "@    =========\n"
               "@\n"
               "@    LE TEMPS CARACTERISTIQUE LIE A L'EQUATION\n"
               "@      DIFFERENTIELLE STOCHASTIQUE DE LA VARIABLE\n"
               "@      NUMERO %d UNE VALEUR NON PERMISE (CS_LAGR_SDE).\n"
               "@\n"
               "@    TCARAC DEVRAIT ETRE UN ENTIER STRICTEMENT POSITIF\n"
               "@       IL VAUT ICI TCARAC = %e11.4\n"
               "@       POUR LA PARTICULE NUMERO %d\n"
               "@\n"
               "@  Le calcul ne sera pas execute.\n"
               "@\n"
               "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
               "@"),
             attr, tcarac[npt], npt);

        cs_real_t aux1   = cs_glob_lagr_time_step->dtp / tcarac [npt];
        cs_real_t aux2   = exp ( -aux1);
        cs_real_t ter1   = 0.5 * cs_lagr_particle_get_real(particle,
                                                           p_am, attr) * aux2;
        cs_real_t ter2   = pip [npt] * (1.0 - (1.0 - aux2) / aux1);

        /* Pour le cas NORDRE= 2, le ETTP suivant est le resultat final */
        cs_real_t *part_ptsvar = cs_lagr_particles_source_terms(p_set, npt, attr);
        cs_lagr_particle_set_real(particle, p_am, attr,
                                  *part_ptsvar + ter1 + ter2 );

      }

    }

  }
}

/*----------------------------------------------------------------------------*/

END_C_DECLS

