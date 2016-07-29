/*============================================================================
 * Management of the GUI parameters file: main parameters
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

#include "cs_defs.h"

/*----------------------------------------------------------------------------
 * Standard C library headers
 *----------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

/*----------------------------------------------------------------------------
 * libxml2 library headers
 *----------------------------------------------------------------------------*/

#if defined(HAVE_LIBXML2)

#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#endif

/*----------------------------------------------------------------------------
 * Local headers
 *----------------------------------------------------------------------------*/

#include "bft_mem.h"
#include "bft_error.h"
#include "bft_printf.h"

#include "fvm_selector.h"

#include "mei_evaluate.h"
#include "mei_math_util.h"

#include "cs_base.h"
#include "cs_field.h"
#include "cs_field_pointer.h"
#include "cs_file.h"
#include "cs_log.h"
#include "cs_gui_util.h"
#include "cs_gui_variables.h"
#include "cs_gui_boundary_conditions.h"
#include "cs_gui_specific_physics.h"
#include "cs_gui_mobile_mesh.h"
#include "cs_mesh.h"
#include "cs_mesh_location.h"
#include "cs_multigrid.h"
#include "cs_parall.h"
#include "cs_parameters.h"
#include "cs_partition.h"
#include "cs_prototypes.h"
#include "cs_rotation.h"
#include "cs_timer.h"
#include "cs_time_moment.h"
#include "cs_thermal_model.h"
#include "cs_physical_properties.h"
#include "cs_time_step.h"
#include "cs_turbomachinery.h"
#include "cs_sles.h"
#include "cs_sles_it.h"
#include "cs_turbulence_model.h"
#include "cs_wall_functions.h"
#include "cs_physical_constants.h"
#include "cs_stokes_model.h"

/*----------------------------------------------------------------------------
 * Header for the current file
 *----------------------------------------------------------------------------*/

#include "cs_gui.h"

/*----------------------------------------------------------------------------*/

BEGIN_C_DECLS

/*! \cond DOXYGEN_SHOULD_SKIP_THIS */

/*=============================================================================
 * Local Macro Definitions
 *============================================================================*/

/* debugging switch */
#define _XML_DEBUG_ 0

/*============================================================================
 * Local Structure Definitions
 *============================================================================*/

/*============================================================================
 * External global variables
 *============================================================================*/

/*----------------------------------------------------------------------------
 * Management of the XML document
 *----------------------------------------------------------------------------*/

#if defined(HAVE_LIBXML2)
extern xmlXPathContextPtr xpathCtx;   /* Pointer on the Context       */
extern xmlNodePtr node;               /* Pointer on the root node     */
#endif

/*============================================================================
 * Private global variables
 *============================================================================*/

/*============================================================================
 * Static global variables
 *============================================================================*/

/* Pointer on the main variable structure */

cs_var_t    *cs_glob_var = NULL;

/*============================================================================
 * Private function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------
 * Turbulence model parameters.
 *
 * parameters:
 *   param                <--  name of the parameters
 *   keyword             -->   turbulence model parameter
 *----------------------------------------------------------------------------*/

static void
cs_gui_advanced_options_turbulence(const char  *param,
                                   int         *keyword)
{
  char *path = NULL;
  int  result;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3, "thermophysical_models", "turbulence", param);

  if (cs_gui_strcmp("gravity_terms", param)) {
    cs_xpath_add_attribute(&path, "status");
    if (cs_gui_get_status(path, &result)) *keyword = result;
  } else if (cs_gui_strcmp("wall_function", param)) {
    cs_xpath_add_function_text(&path);
    if (cs_gui_get_int(path, &result)) *keyword = result;
  } else
    bft_error(__FILE__, __LINE__, 0, _("Invalid xpath: %s\n"), path);

  BFT_FREE(path);
}

/*-----------------------------------------------------------------------------
 * Return the name of the related scalar if the scalar "name" is a variance
 *
 * parameter:
 *   name           <--  scalar name
 *----------------------------------------------------------------------------*/

static char *
_scalar_variance(const char *name)
{
  char *path = NULL;
  char *variance = NULL;

  path = cs_xpath_init_path();
  cs_xpath_add_element(&path, "additional_scalars");
  cs_xpath_add_element(&path, "variable");
  cs_xpath_add_test_attribute(&path, "name", name);
  cs_xpath_add_element(&path, "variance");
  cs_xpath_add_function_text(&path);

  variance = cs_gui_get_text_value(path);

  BFT_FREE(path);

  return variance;
}

/*----------------------------------------------------------------------------
 * Return the value of the choice attribute for material, method, ...
 *
 * parameters:
 *   name        <--  name of the property
 *----------------------------------------------------------------------------*/

static char*
_thermal_table_choice(const char *name)
{
  char *path   = NULL;
  char *choice = NULL;

  path = cs_xpath_short_path();
  cs_xpath_add_element(&path, "fluid_properties");
  cs_xpath_add_element(&path, name);
  cs_xpath_add_attribute(&path, "choice");
  choice = cs_gui_get_attribute_value(path);
  BFT_FREE(path);
  return choice;
}

/*----------------------------------------------------------------------------
 * Return the value of the choice attribute from a property name.
 *
 * parameters:
 *   property_name        <--  name of the property
 *----------------------------------------------------------------------------*/

static char*
_properties_choice(const char *property_name)
{
  char *path   = NULL;
  char *choice = NULL;

  path = cs_xpath_short_path();
  cs_xpath_add_element(&path, "property");
  cs_xpath_add_test_attribute(&path, "name", property_name);
  cs_xpath_add_attribute(&path, "choice");
  choice = cs_gui_get_attribute_value(path);
  BFT_FREE(path);
  return choice;
}

/*----------------------------------------------------------------------------
 * Return 0 if default value is needed
 *
 * parameters:
 *   name        <--  name of the property
 *----------------------------------------------------------------------------*/

static int
_thermal_table_needed(const char *name)
{
  int choice = 0;

  char *prop_choice = _properties_choice(name);
  if (cs_gui_strcmp(prop_choice, "thermal_law"))
    choice = 1;
  BFT_FREE(prop_choice);
  return choice;
}

/*-----------------------------------------------------------------------------
 * use MEI for physical property
 *----------------------------------------------------------------------------*/

static void
_physical_property(const char       *param,
                   const char       *symbol,
                   const cs_lnum_t  ncel,
                   const cs_lnum_t  ncelet,
                   const cs_int_t   icp,
                   const cs_real_t  p0,
                   const cs_real_t  ro0,
                   const cs_real_t  cp0,
                   const cs_real_t  viscl0,
                   const cs_real_t  *visls0,
                   double            values[])
{
  cs_var_t  *vars = cs_glob_var;

  int user_law = 0;
  char *law = NULL;
  double time0;
  char *path = NULL;
  mei_tree_t *ev_law = NULL;
  cs_lnum_t i, iel;

  char *prop_choice = _properties_choice(param);

  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)cs_glob_mesh_quantities->cell_cen;

  if (cs_gui_strcmp(prop_choice, "variable"))
    user_law = 1;

  if (user_law) {

    /* search the formula for the law */
    path = cs_xpath_short_path();
    cs_xpath_add_element(&path, "property");
    cs_xpath_add_test_attribute(&path, "name", param);
    cs_xpath_add_element(&path, "formula");
    cs_xpath_add_function_text(&path);

    law = cs_gui_get_text_value(path);
    BFT_FREE(path);

    if (law != NULL) {

      time0 = cs_timer_wtime();

      const int itherm = cs_glob_thermal_model->itherm;
      const int iscalt = cs_glob_thermal_model->iscalt;

      ev_law = mei_tree_new(law);

      mei_tree_insert(ev_law, "x", 0.0);
      mei_tree_insert(ev_law, "y", 0.0);
      mei_tree_insert(ev_law," z", 0.0);

      mei_tree_insert(ev_law, "p0", p0);

      if (cs_gui_strcmp(param, "density"))
      {
        mei_tree_insert(ev_law, "rho0", ro0);
      }
      else if (cs_gui_strcmp(param, "molecular_viscosity")) {
        mei_tree_insert(ev_law, "rho0", ro0);
        mei_tree_insert(ev_law, "mu0", viscl0);
        mei_tree_insert(ev_law, "rho", 0.0);
        if (cs_gui_strcmp(vars->model, "compressible_model"))
          mei_tree_insert(ev_law, "t0", 0.0);
      }
      else if (cs_gui_strcmp(param, "specific_heat")) {
        mei_tree_insert(ev_law, "cp0", cp0);
      }
      else if (cs_gui_strcmp(param, "thermal_conductivity")) {
        /* for the Temperature, the diffusivity factor is not divided by Cp */
        if (itherm != 1)
          mei_tree_insert(ev_law, "lambda0", visls0[iscalt-1]*(cp0));
        else
          mei_tree_insert(ev_law, "lambda0", visls0[iscalt-1]);
      }

      for (int f_id2 = 0; f_id2 < cs_field_n_fields(); f_id2++) {
        const cs_field_t  *f2 = cs_field_by_id(f_id2);
        if (f2->type & CS_FIELD_USER)
          mei_tree_insert(ev_law, f2->name, 0.0);
      }

      cs_field_t *fth;

      switch (itherm) {
      case 1:
        fth = CS_F_(t);
        break;
      case 2:
        fth = CS_F_(h);
        break;
      case 3:
        fth = CS_F_(energy);
        break;
      default:
        fth = NULL;
      }
      if (fth != NULL)
        mei_tree_insert(ev_law, fth->name, 0.0);

      /* try to build the interpreter */

      if (mei_tree_builder(ev_law))
        bft_error(__FILE__, __LINE__, 0,
                  _("Error: can not interpret expression: %s\n"), ev_law->string);

      if (mei_tree_find_symbol(ev_law, symbol))
        bft_error(__FILE__, __LINE__, 0,
                  _("Error: can not find the required symbol: %s\n"), symbol);

      /* for each cell, update the value of the table of symbols for each scalar
         (including the thermal scalar), and evaluate the interpreter */

      cs_field_t *c_cp = CS_F_(cp);
      cs_field_t *c_rho = CS_F_(rho);
      cs_field_t *c_t = CS_F_(t);

      for (iel = 0; iel < ncel; iel++) {

        mei_tree_insert(ev_law, "x", cell_cen[iel][0]);
        mei_tree_insert(ev_law, "y", cell_cen[iel][1]);
        mei_tree_insert(ev_law, "z", cell_cen[iel][2]);
        for (int f_id = 0; f_id < cs_field_n_fields(); f_id++) {
          cs_field_t  *f = cs_field_by_id(f_id);
          if (f->type & CS_FIELD_USER)
            mei_tree_insert(ev_law, f->name, f->val[iel]);
        }

        if (fth != NULL)
          mei_tree_insert(ev_law, fth->name, fth->val[iel]);

        if (cs_gui_strcmp(param, "molecular_viscosity")) {
          mei_tree_insert(ev_law, "rho", c_rho->val[iel]);
          if (cs_gui_strcmp(vars->model, "compressible_model"))
            mei_tree_insert(ev_law, "T", c_t->val[iel]);
          }

        mei_evaluate(ev_law);

        if (cs_gui_strcmp(param, "thermal_conductivity")) {
          const cs_thermal_model_t  *tm = cs_glob_thermal_model;
          if (tm->itherm == 1)
            values[iel] = mei_tree_lookup(ev_law, symbol);
          else if (icp > 0)
            values[iel] = mei_tree_lookup(ev_law, symbol) / c_cp->val[iel];
          else
            values[iel] = mei_tree_lookup(ev_law, symbol) / cp0;
        }
        else {
          values[iel] = mei_tree_lookup(ev_law, symbol);
        }
      }

      mei_tree_destroy(ev_law);

      cs_gui_add_mei_time(cs_timer_wtime() - time0);
    }
  }
  else if (cs_gui_strcmp(prop_choice, "thermal_law")) {
    cs_phys_prop_type_t property;
    cs_field_t *c_prop;

    if (cs_gui_strcmp(param, "density")) {
      property = CS_PHYS_PROP_DENSITY;
      c_prop = CS_F_(rho);
    }
    else if (cs_gui_strcmp(param, "molecular_viscosity")) {
      property = CS_PHYS_PROP_DYNAMIC_VISCOSITY;
      c_prop = CS_F_(mu);
    }
    else if (cs_gui_strcmp(param, "specific_heat")) {
      property = CS_PHYS_PROP_ISOBARIC_HEAT_CAPACITY;
      c_prop = CS_F_(cp);
    }
    else if (cs_gui_strcmp(param, "thermal_conductivity")) {
      property = CS_PHYS_PROP_THERMAL_CONDUCTIVITY;

      cs_field_t *_th_f[] = {CS_F_(t), CS_F_(h), CS_F_(energy)};

      for (i = 0; i < 3; i++) {
        if (_th_f[i]) {
          if ((_th_f[i])->type & CS_FIELD_VARIABLE) {
            int k = cs_field_key_id("scalar_diffusivity_id");
            int cond_diff_id = cs_field_get_key_int(_th_f[i], k);
            if (cond_diff_id > -1)
              c_prop = cs_field_by_id(cond_diff_id);
            break;
          }
        }
      }
    }
    else {
      bft_error(__FILE__, __LINE__, 0,
                _("Error: can not use evaluate property: %s\n"), prop_choice);
    }

    cs_field_t *c_pres = CS_F_(p);

    cs_real_t *ptot;
    BFT_MALLOC(ptot, ncelet, cs_real_t);
    for (iel = 0; iel < ncelet; iel++)
      ptot[iel] = c_pres->val[iel] + p0;

    cs_field_t *_th_f[] = {CS_F_(t), CS_F_(h), CS_F_(energy)};

    for (i = 0; i < 3; i++)
      if (_th_f[i]) {
        if ((_th_f[i])->type & CS_FIELD_VARIABLE) {
          cs_phys_prop_compute(property,
                              ncel, ptot, _th_f[i]->val, c_prop->val);
          break;
        }
      }
    BFT_FREE(ptot);
  }
  BFT_FREE(prop_choice);
  BFT_FREE(law);

}

/*-----------------------------------------------------------------------------
 * use MEI for compressible physical property
 *----------------------------------------------------------------------------*/

static void
_compressible_physical_property(const char       *param,
                                const char       *symbol,
                                const cs_int_t    idx,
                                const cs_lnum_t  ncel,
                                const cs_int_t   *itempk,
                                const cs_real_t  p0,
                                const cs_real_t  t0,
                                const cs_real_t  ro0,
                                const cs_real_t  *visls0,
                                const cs_real_t  *viscv0)
{
  int variable = 0;
  char *law = NULL;
  double time0;
  char *path = NULL;
  mei_tree_t *ev_law = NULL;

  char *prop_choice = _properties_choice(param);
  int n_fields = cs_field_n_fields();

  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)cs_glob_mesh_quantities->cell_cen;

  if (cs_gui_strcmp(prop_choice, "variable"))
    variable = 1;
  BFT_FREE(prop_choice);

  if (variable) {
    /* search the formula for the law */
    path = cs_xpath_short_path();
    cs_xpath_add_element(&path, "property");
    cs_xpath_add_test_attribute(&path, "name", param);
    cs_xpath_add_element(&path, "formula");
    cs_xpath_add_function_text(&path);

    law = cs_gui_get_text_value(path);

    BFT_FREE(path);

    if (law != NULL) {
      time0 = cs_timer_wtime();

      ev_law = mei_tree_new(law);
      BFT_FREE(law);

      mei_tree_insert(ev_law, "x", 0.0);
      mei_tree_insert(ev_law, "y", 0.0);
      mei_tree_insert(ev_law," z", 0.0);

      mei_tree_insert(ev_law, "p0", p0);
      mei_tree_insert(ev_law, "t0", t0);

      if (cs_gui_strcmp(param, "thermal_conductivity")) {
        mei_tree_insert(ev_law, "lambda0", visls0[*itempk -1]);
        mei_tree_insert(ev_law, "rho0", ro0);
      }
      else if (cs_gui_strcmp(param, "volume_viscosity")) {
        mei_tree_insert(ev_law, "viscv0", *viscv0);
        mei_tree_insert(ev_law, "T", 0.);
      }

      if (cs_gui_strcmp(param, "thermal_conductivity")) {
        for (int f_id2 = 0; f_id2 < n_fields; f_id2++) {
          const cs_field_t  *f2 = cs_field_by_id(f_id2);
          if (f2->type & CS_FIELD_USER)
            mei_tree_insert(ev_law, f2->name, 0.0);
        }
      }

      /* try to build the interpreter */

      if (mei_tree_builder(ev_law))
        bft_error(__FILE__, __LINE__, 0,
                  _("Error: can not interpret expression: %s\n"), ev_law->string);

      if (mei_tree_find_symbol(ev_law, symbol))
        bft_error(__FILE__, __LINE__, 0,
                  _("Error: can not find the required symbol: %s\n"), symbol);

      /* for each cell, update the value of the table of symbols for each scalar
         (including the thermal scalar), and evaluate the interpreter */

      cs_field_t *c = cs_field_by_id(idx);

      const int itherm = cs_glob_thermal_model->itherm;

      assert(itherm == 3);

      cs_field_t *f = CS_F_(energy);

      for (cs_lnum_t iel = 0; iel < ncel; iel++) {
        mei_tree_insert(ev_law, "x", cell_cen[iel][0]);
        mei_tree_insert(ev_law, "y", cell_cen[iel][1]);
        mei_tree_insert(ev_law, "z", cell_cen[iel][2]);
        if (cs_gui_strcmp(param, "thermal_conductivity")) {
          for (int f_id2 = 0; f_id2 < n_fields; f_id2++) {
            const cs_field_t  *f2 = cs_field_by_id(f_id2);
            if (f2->type & CS_FIELD_USER)
              mei_tree_insert(ev_law,
                              f2->name,
                              f2->val[iel]);
          }
        }

        mei_tree_insert(ev_law, f->name, f->val[iel]);

        mei_evaluate(ev_law);
        c->val[iel] = mei_tree_lookup(ev_law, symbol);
      }
      mei_tree_destroy(ev_law);

      cs_gui_add_mei_time(cs_timer_wtime() - time0);
    }
  }
}

/*-----------------------------------------------------------------------------
 * Return the value of choice for user scalar's property
 *
 * parameters:
 *   scalar_num <-- number of scalar
 *   choice     <-> choice for property
 *----------------------------------------------------------------------------*/

static int
_scalar_properties_choice(int  scalar_num,
                          int *choice)
{
  char *path = NULL;
  char *buff = NULL;
  int   ichoice;

  path = cs_xpath_init_path();
  cs_xpath_add_element(&path, "additional_scalars");
  cs_xpath_add_element_num(&path, "variable", scalar_num);
  cs_xpath_add_element(&path, "property");
  cs_xpath_add_attribute(&path, "choice");

  buff = cs_gui_get_attribute_value(path);

  if (buff == NULL) {
    ichoice = 0;

  } else {
    ichoice = 1;

    if (cs_gui_strcmp(buff, "variable"))
      *choice = 1;
    else if (cs_gui_strcmp(buff, "constant"))
      *choice = 0;
    else
      bft_error(__FILE__, __LINE__, 0, _("Invalid xpath: %s\n"), path);
  }

  BFT_FREE(path);
  BFT_FREE(buff);

  return ichoice;
}

/*-----------------------------------------------------------------------------
 * Return value of diffusion coefficient for user scalars
 *        return 1 if value exists
 *        return 0 if not
 *
 * parameters:
 *   num_sca  <-- number of scalar
 *   value   <--  value of diffusion coefficient
 *----------------------------------------------------------------------------*/

static void
_scalar_diffusion_value(int      num_sca,
                        double  *value)
{
  char  *path = NULL;
  double result;

  path = cs_xpath_init_path();
  cs_xpath_add_element(&path, "additional_scalars");
  cs_xpath_add_element_num(&path, "variable", num_sca);
  cs_xpath_add_element(&path, "property");
  cs_xpath_add_element(&path, "initial_value");
  cs_xpath_add_function_text(&path);

  if (cs_gui_get_double(path, &result))
    *value = result;

  BFT_FREE(path);
}

/*-----------------------------------------------------------------------------
 * Get the status of steady management.
 *
 * parameter:
 *   keyword         -->  if 1 unsteady management else steady management
 *----------------------------------------------------------------------------*/

static void
_get_steady_status(int *keyword)
{
  char *path = NULL;
  int   result;

  path = cs_xpath_short_path();
  cs_xpath_add_element(&path, "steady_management");
  cs_xpath_add_attribute(&path, "status");

  if (cs_gui_get_status(path, &result))
    *keyword = result;
  else
    *keyword = 1;

  BFT_FREE(path);
}

/*----------------------------------------------------------------------------
 * Return the initialization choice of the turbulence variables.
 *----------------------------------------------------------------------------*/

static char *
_velocity_pressure_algo_choice(void)
{
  char *path = NULL;
  char *algo_choice;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 2,
                        "numerical_parameters",
                        "velocity_pressure_algo");
  cs_xpath_add_attribute(&path, "choice");

  algo_choice = cs_gui_get_attribute_value(path);

  BFT_FREE(path);

  return algo_choice;
}

/*-----------------------------------------------------------------------------
 * Return  parameters for steady management.
 *
 * parameter:
 *   param           <--  steady parameter
 *   keyword         -->  new value for the steady parameter
 *----------------------------------------------------------------------------*/

static void
_steady_parameters(const char  *param,
                   double      *keyword)
{
  char   *path   = NULL;
  double  result = 0.0;
  int     status = 0;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3, "analysis_control", "steady_management", param);

  if (cs_gui_strcmp(param,"zero_iteration")) {
    cs_xpath_add_attribute(&path, "status");
    if(cs_gui_get_status(path, &status))
      *keyword = status;
  } else {
    cs_xpath_add_function_text(&path);
    if (cs_gui_get_double(path, &result))
      *keyword = result;
  }
  BFT_FREE(path);
}

/*-----------------------------------------------------------------------------
 * Modify time parameters.
 *
 * parameters:
 *   param              <--  time parameter
 *   keyword            -->  new value of the time parameter
 *----------------------------------------------------------------------------*/

static void
_time_parameters(const char  *param,
                 double      *keyword)
{
  char   *path   = NULL;
  double  result = 0.0;
  int     status = 0;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3, "analysis_control", "time_parameters", param);

  if (cs_gui_strcmp(param,"zero_time_step") ||
      cs_gui_strcmp(param,"thermal_time_step")) {

    cs_xpath_add_attribute(&path, "status");
    if(cs_gui_get_status(path, &status))
      *keyword = status;

  } else {
    cs_xpath_add_function_text(&path);
    if (cs_gui_get_double(path, &result))
      *keyword = result;
  }
  BFT_FREE(path);
}

/*-----------------------------------------------------------------------------
 * Modify restart parameters.
 *
 * parameters:
 *   param     <--  restart parameter
 *   keyword   <->  new value of the restart parameter
 *----------------------------------------------------------------------------*/

static void
_restart_parameters_status(const char  *param,
                           int         *keyword)
{
  int   result;
  char *path = NULL;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3, "calculation_management", "start_restart", param);

  if (cs_gui_strcmp(param, "restart_rescue")) {
    cs_xpath_add_function_text(&path);
    if (cs_gui_get_int(path, &result))
      *keyword = result;

  } else {
    cs_xpath_add_attribute(&path, "status");

    if (cs_gui_get_status(path, &result))
      *keyword = result;
  }

  BFT_FREE(path);
}

/*-----------------------------------------------------------------------------
 * Return value of numerical parameter markup
 *
 * parameters:
 *   variable_name  <-- name of variable
 *   value_type     <-- name of numerical parameter parkup
 *   value          --> value of numerical parameter
 *----------------------------------------------------------------------------*/

static void
_variable_value(const char  *variable_name,
                const char  *value_type,
                double      *value)
{
  char  *path = NULL;
  double result;

  path = cs_xpath_short_path();
  cs_xpath_add_element(&path, "variable");
  cs_xpath_add_test_attribute(&path, "name", variable_name);
  cs_xpath_add_element(&path, value_type);
  cs_xpath_add_function_text(&path);

  if (cs_gui_get_double(path, &result))
    *value = result;

  BFT_FREE(path);
}

/*-----------------------------------------------------------------------------
 * Return value of turbulent flux model
 *
 * parameters:
 *   variable_name  <-- name of variable
 *   value          --> value of turbulent flux model
 *----------------------------------------------------------------------------*/

static void
_variable_turbulent_flux_model(const char   *variable_name,
                               int          *value)
{
  char *path = NULL;
  char *result = NULL;

  path = cs_xpath_short_path();
  cs_xpath_add_element(&path, "variable");
  cs_xpath_add_test_attribute(&path, "name", variable_name);
  cs_xpath_add_element(&path, "turbulent_flux_model");
  cs_xpath_add_function_text(&path);

  result = cs_gui_get_text_value(path);

  if (cs_gui_strcmp(result, "SGDH"))
    *value = 0;
  else if (cs_gui_strcmp(result, "GGDH"))
    *value = 10;
  else if (cs_gui_strcmp(result, "AFM"))
    *value = 20;
  else if (cs_gui_strcmp(result, "DFM"))
    *value = 30;
  else
    *value = 0; /* assign default */

  BFT_FREE(path);
  BFT_FREE(result);
}

/*----------------------------------------------------------------------------
 * Get the attribute value from the xpath query.
 *
 * parameters:
 *   path          <-- path for xpath query
 *   child         <-- child markup
 *   keyword      -->  value of attribute node
 *----------------------------------------------------------------------------*/

static void
_attribute_value(char        *path,
                 const char  *child,
                 int         *keyword)
{
  char *choice = NULL;
  int   result;

  assert(path != NULL);
  assert(child != NULL);

  if (cs_gui_strcmp(child, "order_scheme")) {

    /* *keyword = 1; */
    cs_xpath_add_attribute(&path, "choice");
    choice = cs_gui_get_attribute_value(path);

    if (cs_gui_strcmp(choice, "centered"))
      *keyword = 1;
    else if (cs_gui_strcmp(choice, "solu"))
      *keyword = 0;
    BFT_FREE(choice);

  } else {

    cs_xpath_add_attribute(&path, "status");

    if (cs_gui_get_status(path, &result)) {
      *keyword = result;

      if (cs_gui_strcmp(child, "slope_test")) {
        if (result == 1)
          *keyword = 0;
        if (result == 0)
          *keyword = 1;
      }
    }
  }
  BFT_FREE(path);
}

/*----------------------------------------------------------------------------
 * Get the attribute value associated to a child markup from a variable.
 *
 * parameters:
 *   name          <--  name of the variable markup
 *   child         <--  child markup
 *   keyword      -->   value of attribute node contained in the child markup
 *----------------------------------------------------------------------------*/

static void
_variable_attribute(const char  *name,
                    const char  *child,
                    int         *keyword)
{
  char *path = NULL;

  path = cs_xpath_short_path();
  cs_xpath_add_element(&path, "variable");
  cs_xpath_add_test_attribute(&path, "name", name);
  cs_xpath_add_element(&path, child);

  _attribute_value(path, child, keyword);
}

/*----------------------------------------------------------------------------
 * Return the attribute choice associated to a child markup from a variable.
 *
 * parameters:
 *   name          <--  name of the variable markup
 *   child         <--  child markup
 *----------------------------------------------------------------------------*/

static char *
_variable_choice(const char  *name,
                 const char  *child)
{
  char *path = NULL;
  char *choice;

  path = cs_xpath_short_path();
  cs_xpath_add_element(&path, "variable");
  cs_xpath_add_test_attribute(&path, "name", name);
  cs_xpath_add_element(&path, child);
  cs_xpath_add_attribute(&path, "choice");

  choice = cs_gui_get_attribute_value(path);

  BFT_FREE(path);

  return choice;
}

/*-----------------------------------------------------------------------------
 * Modify integer numerical parameters.
 *
 * parameters:
 *   param     <--  label of the numerical parameter
 *   keyword   <--  value of the numerical parameter
 *----------------------------------------------------------------------------*/

static void
_numerical_int_parameters(const char  *param,
                          int         *keyword)
{
  char *path = NULL;
  char *choice = NULL;
  int   result;

  path = cs_xpath_init_path();
  cs_xpath_add_element(&path, "numerical_parameters");

  if (cs_gui_strcmp(param, "gradient_reconstruction")) {

    cs_xpath_add_element(&path, param);
    cs_xpath_add_attribute(&path, "choice");
    choice = cs_gui_get_attribute_value(path);
    if (choice)
      *keyword = atoi(choice);
    BFT_FREE(choice);

  } else if (cs_gui_strcmp(param,"piso_sweep_number")) {

    cs_xpath_add_element(&path, "velocity_pressure_algo");
    cs_xpath_add_element(&path, param);
    cs_xpath_add_function_text(&path);
    if (cs_gui_get_int(path, &result))
      *keyword = result;

  } else {

    cs_xpath_add_element(&path, param);
    cs_xpath_add_attribute(&path, "status");
    if (cs_gui_get_status(path, &result))
      *keyword = result;
  }
  BFT_FREE(path);
}

/*-----------------------------------------------------------------------------
 * Modify gravity parameters.
 *
 * parameters:
 *   param               <--  gravity parameter (GX, GY, GZ)
 *   keyword            <<--  new value of the gravity parameter
 *----------------------------------------------------------------------------*/

static void
_gravity_value(const char  *param,
               double      *value)
{
  char   *path = NULL;
  double  result;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3, "physical_properties", "gravity", param);
  cs_xpath_add_function_text(&path);

  if (cs_gui_get_double(path, &result))
    *value = result;

  BFT_FREE(path);
}

/*-----------------------------------------------------------------------------
 * Modify coriolis source terms parameters.
 *
 * parameters:
 *   param    <--  coriolis parameter (omegax, omegay, omegaz)
 *   value    -->  new value of the coriolis parameter
 *----------------------------------------------------------------------------*/

static void
_coriolis_value(const char *param,
                double     *value)
{
  char   *path = NULL;
  double  result;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3, "physical_properties", "omega", param);
  cs_xpath_add_function_text(&path);

  if (cs_gui_get_double(path, &result))
    *value = result;

  BFT_FREE(path);
}

/*----------------------------------------------------------------------------
 * Get the value of the choice attribute from a property markup.
 * Return 1 if the xpath request has succeeded, 0 otherwise.
 *
 * parameters:
 *   property_name <-- name of the property
 *   choice        --> value of the attribute choice
 *----------------------------------------------------------------------------*/

static int
_properties_choice_id(const char  *property_name,
                      int         *choice)
{
  char *buff = NULL;
  int   iok = 0;

  buff = _properties_choice(property_name);
  *choice = 0; /* default */
  if (buff)
  {
    iok = 1;
    if (cs_gui_strcmp(buff, "variable") || cs_gui_strcmp(buff, "thermal_law"))
      *choice = 1;
    else if (cs_gui_strcmp(buff, "constant"))
      *choice = 0;
  }
  else
    iok = 0;
  BFT_FREE(buff);
  return iok;
}

/*----------------------------------------------------------------------------
 * Turbulence model parameters.
 *
 * parameters:
 *   param                <--  name of the parameters
 *   keyword             -->   turbulence model parameter
 *----------------------------------------------------------------------------*/

static void
_option_turbulence_double(const char  *param,
                          double      *keyword)
{
  char *path = NULL;
  double  result;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3, "thermophysical_models", "turbulence", param);

  cs_xpath_add_function_text(&path);
  if (cs_gui_get_double(path, &result))
    *keyword = result;

  BFT_FREE(path);
}

/*----------------------------------------------------------------------------
 * Return the length choice for initialize turbulence
 *----------------------------------------------------------------------------*/

static char
*_reference_length_initialization_choice(void)
{
  char *path = NULL;
  char *initialization_choice;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3,
                        "thermophysical_models",
                        "reference_values",
                        "length");
  cs_xpath_add_attribute(&path, "choice");

  initialization_choice = cs_gui_get_attribute_value(path);

  BFT_FREE(path);

  return initialization_choice;
}

/*----------------------------------------------------------------------------
 * Return the initialization choice of the turbulence variables.
 *
 * parameters:
 *   zone_id        <--  zone number
 *----------------------------------------------------------------------------*/

static char *
_turbulence_initialization_choice(const char* zone_id)
{
  char *path = NULL;
  char *initialization_choice;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3,
                        "thermophysical_models",
                        "turbulence",
                        "initialization");
  cs_xpath_add_test_attribute(&path, "zone_id", zone_id);
  cs_xpath_add_attribute(&path, "choice");

  initialization_choice = cs_gui_get_attribute_value(path);

  BFT_FREE(path);

  return initialization_choice;
}

/*----------------------------------------------------------------------------
 * Return the number of variables and properties inside a given time average.
 *
 * parameters:
 *   id           <--  time average number (imom)
 *----------------------------------------------------------------------------*/

static int
_get_time_average_n_variables(const int  id)
{
  char *path = NULL;
  int   number = 0;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 2, "analysis_control", "time_averages");
  cs_xpath_add_element_num(&path, "time_average", id);
  cs_xpath_add_element(&path, "var_prop");
  number = cs_gui_get_nb_element(path);

  BFT_FREE(path);

  return number;
}

/*-----------------------------------------------------------------------------
 * Return the label model's property.
 *
 * parameters:
 *   moment_id  <-- moment id
 *----------------------------------------------------------------------------*/

static char *
_get_time_average_label(int  moment_id)
{
  char *path = NULL;
  char *label_name = NULL;

  path = cs_xpath_short_path();
  cs_xpath_add_element_num(&path, "time_average", moment_id+1);
  cs_xpath_add_attribute(&path, "label");

  label_name = cs_gui_get_attribute_value(path);

  BFT_FREE(path);

  return label_name;
}

/*----------------------------------------------------------------------------
 * Return the component of variables or properties or scalar for a given time average
 *
 * parameters:
 *   id <--  number of 1D profile
 *   nm <--  number of the variable name of the idth 1D profile
 *----------------------------------------------------------------------------*/

static int
_get_time_average_component(const int  id,
                            const int  nm)
{
  char *path = NULL;
  char *comp = NULL;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 2, "analysis_control", "time_averages");
  cs_xpath_add_element_num(&path, "time_average", id);
  cs_xpath_add_element_num(&path, "var_prop", nm);
  cs_xpath_add_attribute(&path, "component");

  comp = cs_gui_get_attribute_value(path);
  if (comp == NULL)
    bft_error(__FILE__, __LINE__, 0,
              _("Invalid xpath: %s\n component not found"), path);
  BFT_FREE(path);

  int compId = atoi(comp);

  BFT_FREE(comp);

  return compId;
}

/*----------------------------------------------------------------------------
 * Get value of a parameter for a given time average.
 *
 * parameters:
 *   id              <--  time average number (imom)
 *   param           <--  name of the parameter
 *   data           -->   value of the parameter
 *----------------------------------------------------------------------------*/

static void
_get_time_average_data(int          id,
                       const char  *param,
                       int         *data)
{
  char *path = NULL;
  int   result = 0;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 2, "analysis_control", "time_averages");
  cs_xpath_add_element_num(&path, "time_average", id);
  cs_xpath_add_element(&path, param);

  cs_xpath_add_function_text(&path);
  if (cs_gui_get_int(path, &result))
    *data = result;

  BFT_FREE(path);
}

/*----------------------------------------------------------------------------
 * Get value of a parameter for a given time average.
 *
 * parameters:
 *   id              <--  time average number (imom)
 *   param           <--  name of the parameter
 *   data           -->   value of the parameter
 *----------------------------------------------------------------------------*/

static void
_get_time_average_time_start(int          id,
                             const char  *param,
                             double      *data)
{
  char  *path = NULL;
  double result = 0;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 2, "analysis_control", "time_averages");
  cs_xpath_add_element_num(&path, "time_average", id);
  cs_xpath_add_element(&path, param);

  cs_xpath_add_function_text(&path);
  if (cs_gui_get_double(path, &result))
    *data = result;

  BFT_FREE(path);
}

/*----------------------------------------------------------------------------
 * Return the name of a variable or a property for a given time average.
 *
 * parameters:
 *   id <--  time average id
 *   nb <--  variable or property number
 *----------------------------------------------------------------------------*/

static char *
_get_time_average_variable_name(int  id,
                                int  nb)
{
  char *path = NULL;
  char *name = NULL;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 2, "analysis_control", "time_averages");
  cs_xpath_add_element_num(&path, "time_average", id);
  cs_xpath_add_element_num(&path, "var_prop", nb);
  cs_xpath_add_attribute(&path, "name");

  name = cs_gui_get_attribute_value(path);
  BFT_FREE(path);

  return name;
}

/*-----------------------------------------------------------------------------
 * Return label of variable
 *
 * parameters:
 *   variable   <-- name of variable
 *----------------------------------------------------------------------------*/

static char *
_variable_label(const char *variable)
{
  char *path = NULL;
  char *label = NULL;

  path = cs_xpath_short_path();
  cs_xpath_add_element(&path, "variable");
  cs_xpath_add_test_attribute(&path, "name", variable);
  cs_xpath_add_attribute(&path, "label");

  label = cs_gui_get_attribute_value(path);

  BFT_FREE(path);

  return label;
}

/*-----------------------------------------------------------------------------
 * Return the label attribute of a property markup.
 *
 * parameters:
 *   property_name        <--  name of the property
 *----------------------------------------------------------------------------*/

static char *
_properties_label(const char *property_name)
{
  char *path = NULL;
  char *label = NULL;

  path = cs_xpath_short_path();
  cs_xpath_add_element(&path, "property");
  cs_xpath_add_test_attribute(&path, "name", property_name);
  cs_xpath_add_attribute(&path, "label");

  label = cs_gui_get_attribute_value(path);

  BFT_FREE(path);

  return label;
}

/*-----------------------------------------------------------------------------
 * Return the label or the name from a scalar.
 *
 * parameters:
 *   kw                   <--  keyword: 'label' or 'name'
 *   scalar_num          -->   number of the searching scalar
 *----------------------------------------------------------------------------*/

static char *
_scalar_name_label(const char  *kw,
                   const int    scalar_num)
{
  char *path = NULL;
  char *str  = NULL;

  path = cs_xpath_short_path();
  cs_xpath_add_element(&path, "additional_scalars");
  cs_xpath_add_element_num(&path, "variable", scalar_num);
  cs_xpath_add_attribute(&path, kw);

  str = cs_gui_get_attribute_value(path);

  BFT_FREE(path);

  return str;
}

/*-----------------------------------------------------------------------------
 * Return the name tor thermal scalar.
 *
 * parameters:
 *   kw                   <--  scalar name
 *----------------------------------------------------------------------------*/

static char *
_thermal_scalar_name_label(const char  *kw)
{
  char *path = NULL;
  char *str  = NULL;

  path = cs_xpath_short_path();
  cs_xpath_add_elements(&path, 3,
                        "thermophysical_models",
                        "thermal_scalar",
                        "variable");
  cs_xpath_add_attribute(&path, kw);

  str = cs_gui_get_attribute_value(path);

  BFT_FREE(path);

  return str;
}

/*==========================
 * FOR VOLUMIC ZONES
 *==========================*/

/*-----------------------------------------------------------------------------
 * Return the name of the volumic zone
 *
 * parameters:
 *   ith_zone        <--  id of volumic zone
 *----------------------------------------------------------------------------*/

static char *
_volumic_zone_id(int  ith_zone)
{
  char *path = NULL;
  char *name = NULL;

  /* 1) get the name of the ith initialization zone */
  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 2, "solution_domain", "volumic_conditions");
  cs_xpath_add_element_num(&path, "zone", ith_zone);
  cs_xpath_add_attribute(&path, "id");

  name = cs_gui_get_attribute_value(path);

  BFT_FREE(path);

  return name;
}

/*-----------------------------------------------------------------------------
 * Return the localisation for the volumic zone with a given id
 *
 * parameters:
 *   zone_id      <--  volumic zone id
 *----------------------------------------------------------------------------*/

static char *
_volumic_zone_localization(const char *zone_id)
{
  char *path = NULL;
  char *description = NULL;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3, "solution_domain",
                        "volumic_conditions",
                        "zone");
  cs_xpath_add_test_attribute(&path, "id", zone_id);
  cs_xpath_add_function_text(&path);

  description = cs_gui_get_text_value(path);

  BFT_FREE(path);

  return description;
}

/*-----------------------------------------------------------------------------
 * Get initial value from property markup.
 *
 * parameters:
 *   zone_id            <--  zone number
 *   parameter          <--  name of the parameter
 *   value              -->  new initial value of the property
 *----------------------------------------------------------------------------*/

static void
_van_genuchten_parameter_value(const char  *zone_id,
                               const char  *parameter,
                               double      *value)
{
  char   *path = NULL;
  double  result;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3,
                        "thermophysical_models",
                        "groundwater",
                        "groundwater_law");
  cs_xpath_add_test_attribute(&path, "zone_id", zone_id);
  cs_xpath_add_element(&path, "VanGenuchten_parameters");
  cs_xpath_add_element(&path, parameter);
  cs_xpath_add_function_text(&path);
  if (cs_gui_get_double(path, &result))
    *value = result;

  BFT_FREE(path);
}

/*-----------------------------------------------------------------------------
 * Return the initial value of variable for the volumic zone named name
 *
 * parameters:
 *   variable_name    <--  name of variable
 *   zone_id          <--  id of volumic zone
 *   initial_value    -->  initial value
 *----------------------------------------------------------------------------*/

#if (_XML_DEBUG_ > 0)

static void
_variable_initial_value(const char  *variable_name,
                        const char  *zone_id,
                        double      *initial_value)
{
  char *path = NULL;
  double result;

  path = cs_xpath_short_path();
  cs_xpath_add_element(&path, "variable");
  cs_xpath_add_test_attribute(&path, "name", variable_name);
  cs_xpath_add_element(&path, "initial_value");
  cs_xpath_add_test_attribute(&path, "zone_id", zone_id);
  cs_xpath_add_function_text(&path);

  if (cs_gui_get_double(path, &result))
    *initial_value = result;
  else
    *initial_value = 0.0;

  BFT_FREE(path);
}

#endif /* (_XML_DEBUG_ > 0) */

/*-----------------------------------------------------------------------------
 * Return the list of cells describing a given zone.
 *
 * parameters:
 *   zone_id             <--  volume zone id
 *   n_cells_with_ghosts <--  number of cells with halo
 *   cells               -->  number of selected cells
 *----------------------------------------------------------------------------*/

static int *
_get_cells_list(const char  *zone_id,
                cs_lnum_t    n_cells_with_ghosts,
                cs_lnum_t   *cells )
{
  cs_lnum_t  c_id         = 0;
  cs_lnum_t  *cells_list  = NULL;
  char *description = NULL;

  description = _volumic_zone_localization(zone_id);

  /* build list of cells */
  BFT_MALLOC(cells_list, n_cells_with_ghosts, cs_lnum_t);

  c_id = fvm_selector_get_list(cs_glob_mesh->select_cells,
                               description,
                               0,
                               cells,
                               cells_list);

  if (fvm_selector_n_missing(cs_glob_mesh->select_cells, c_id) > 0) {
    const char *missing
      = fvm_selector_get_missing(cs_glob_mesh->select_cells, c_id, 0);
    cs_base_warn(__FILE__, __LINE__);
    bft_printf(_("The group or attribute \"%s\" in the selection\n"
                 "criteria:\n"
                 "\"%s\"\n"
                 " does not correspond to any cell.\n"),
                 missing, description);
  }
  BFT_FREE(description);
  return cells_list;
}

/*-----------------------------------------------------------------------------
 * Initialize mei tree and check for symbols existence
 *
 * parameters:
 *   formula        <--  mei formula
 *   symbols        <--  array of symbol to check
 *   symbol_size    <--  number of symbol in symbols
 *----------------------------------------------------------------------------*/

static mei_tree_t *
_init_mei_tree(const char  *formula,
               const char  *symbols)
{
  /* return an empty interpreter */
  mei_tree_t *tree = mei_tree_new(formula);

  /* add commun variables */
  mei_tree_insert(tree, "x",    0.0);
  mei_tree_insert(tree, "y",    0.0);
  mei_tree_insert(tree, "z",    0.0);

  /* try to build the interpreter */
  if (mei_tree_builder(tree))
    bft_error(__FILE__, __LINE__, 0,
              _("Error: can not interpret expression: %s\n"), tree->string);
  /* check for symbols */
  if (mei_tree_find_symbol(tree, symbols))
    bft_error(__FILE__, __LINE__, 0,
              _("Error: can not find the required symbol: %s\n"), symbols);

  return tree;
}

/*----------------------------------------------------------------------------
 * Get label of 1D profile file name
 *
 * parameters:
 *   id           <--  number of order in list of 1D profile
 *----------------------------------------------------------------------------*/

static char
*_get_profile(const char  *kw,
              int          id)
{
  char *path = NULL;
  char *label = NULL;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 2, "analysis_control", "profiles");
  cs_xpath_add_element_num(&path, "profile", id+1);
  cs_xpath_add_attribute(&path, kw);

  label = cs_gui_get_attribute_value(path);

  BFT_FREE(path);

  return label;
}

/*----------------------------------------------------------------------------
 * Return the component of variables or properties or scalar for 1D profile
 *
 * parameters:
 *   id           <--  number of 1D profile
 *   nm           <--  number of the variable name of the idst 1D profile
 *----------------------------------------------------------------------------*/

static int
_get_profile_component(int  id,
                       int  nm)
{
  char *path = NULL;
  char *comp = NULL;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 2, "analysis_control", "profiles");
  cs_xpath_add_element_num(&path, "profile", id + 1);
  cs_xpath_add_element_num(&path, "var_prop", nm + 1);
  cs_xpath_add_attribute(&path, "component");

  comp = cs_gui_get_attribute_value(path);
  if (comp == NULL)
    bft_error(__FILE__, __LINE__, 0,
              _("Invalid xpath: %s\n component not found"), path);
  BFT_FREE(path);

  int compId = atoi(comp);

  BFT_FREE(comp);

  return compId;
}

/*----------------------------------------------------------------------------
 * Get number of variables or properties or scalar for 1D profile
 *
 * parameters:
 *   id           <--  number of 1D profile
 *----------------------------------------------------------------------------*/

static int
_get_profile_names_number(int id)
{
  char *path = NULL;
  int   number = 0;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 2, "analysis_control", "profiles");
  cs_xpath_add_element_num(&path, "profile", id+1);
  cs_xpath_add_element(&path, "var_prop");
  number = cs_gui_get_nb_element(path);

  BFT_FREE(path);

  return number;
}

/*----------------------------------------------------------------------------
 * Return the name of variables or properties or scalar for 1D profile
 *
 * parameters:
 *   id           <--  number of 1D profile
 *   nm           <--  number of the variable name of the idst 1D profile
 *----------------------------------------------------------------------------*/

static char *
_get_profile_name(int  id,
                  int  nm)
{
  char *path = NULL;
  char *name = NULL;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 2, "analysis_control", "profiles");
  cs_xpath_add_element_num(&path, "profile", id+1);
  cs_xpath_add_element_num(&path, "var_prop", nm+1);
  cs_xpath_add_attribute(&path, "name");

  name = cs_gui_get_attribute_value(path);
  if (name == NULL)
    bft_error(__FILE__, __LINE__, 0,
              _("Invalid xpath: %s\n name not found"), path);
  BFT_FREE(path);

  return name;
}

/*----------------------------------------------------------------------------
 * Return the label of variables or properties or scalar for 1D profile
 *
 * parameters:
 *   id           <--  number of 1D profile
 *   nm           <--  number of the variable name of the idst 1D profile
 *----------------------------------------------------------------------------*/

static char *
_get_profile_label_name(int   id,
                        int   nm)
{
  char *path = NULL;
  char *name = NULL;
  char *label = NULL;

  name = _get_profile_name(id, nm);
  int idim = _get_profile_component(id, nm);

  for (int f_id = 0; f_id < cs_field_n_fields(); f_id++) {
    const cs_field_t *f = cs_field_by_id(f_id);
    if (cs_gui_strcmp(name,  f->name)) {
      if (f->type & CS_FIELD_VARIABLE) {
        label = _variable_label(name);
        if (f->dim > 1) {
          int len = strlen(label) + 4;
          char *tmp = NULL;
          char *snumpp = NULL;
          BFT_MALLOC(snumpp, 2, char);
          sprintf(snumpp, "%1.1i", idim);
          BFT_MALLOC(tmp, len, char);
          strcpy(tmp, label);
          strcat(tmp, "[");
          strcat(tmp, snumpp);
          strcat(tmp, "]");
          BFT_FREE(label);
          BFT_MALLOC(label, len, char);
          strcpy(label, tmp);
          BFT_FREE(snumpp);
          BFT_FREE(tmp);
        }
      }
      else if (f->type & CS_FIELD_PROPERTY) {
        label = _properties_label(name);
        if (f->dim > 1) {
          int len = strlen(label) + 4;
          char *tmp = NULL;
          char *snumpp = NULL;
          BFT_MALLOC(snumpp, 2, char);
          sprintf(snumpp, "%1.1i", idim);
          BFT_MALLOC(tmp, len, char);
          strcpy(tmp, label);
          strcat(tmp, "[");
          strcat(tmp, snumpp);
          strcat(tmp, "]");
          BFT_FREE(label);
          BFT_MALLOC(label, len, char);
          strcpy(label, tmp);
          BFT_FREE(snumpp);
          BFT_FREE(tmp);
        }
      }
    }
  }

  if (label == NULL)
    bft_error(__FILE__, __LINE__, 0,
              _("Invalid markup name: %s\n label not found"), name);

  BFT_FREE(path);
  BFT_FREE(name);

  return label;
}

/*----------------------------------------------------------------------------
 * Get coordinates or output frequency for 1D profile
 *
 * parameters:
 *   id           <--  number of 1D profile
 *    x          -->   name of the coordinate (x1, y1, z1, x2, y2, z2)
 *                     or the output frequency
 *----------------------------------------------------------------------------*/

static double
_get_profile_coordinate(int          id,
                        const char  *x)
{
  char *path = NULL;
  double coordinate = 0.0;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 2, "analysis_control", "profiles");
  cs_xpath_add_element_num(&path, "profile", id+1);
  cs_xpath_add_element(&path, x);
  cs_xpath_add_function_text(&path);

  if (!cs_gui_get_double(path, &coordinate))
    bft_error(__FILE__, __LINE__, 0, _("Invalid xpath: %s\n"), path);

  BFT_FREE(path);

  return coordinate;
}

/*----------------------------------------------------------------------------
 * Return the type of output frequency for 1D profile
 *
 * parameters:
 *   id           <--  number of average
 *----------------------------------------------------------------------------*/

static char *
_get_profile_output_type(int  id)
{
  char *path = NULL;
  char *name = NULL;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 2, "analysis_control", "profiles");
  cs_xpath_add_element_num(&path, "profile", id + 1);
  cs_xpath_add_element(&path, "output_type");
  cs_xpath_add_function_text(&path);

  name = cs_gui_get_text_value(path);
  if (name == NULL)
    bft_error(__FILE__, __LINE__, 0,
              _("Invalid xpath: %s\n name not found"), path);
  BFT_FREE(path);

  return name;
}

/*----------------------------------------------------------------------------
 * Get output format for 1D profile
 *
 * parameters:
 *   id           <--  number of 1D profile
 *----------------------------------------------------------------------------*/

static int
_get_profile_format(int id)
{
  char *path = NULL, *format_s = NULL;
  int   format = 0;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 2, "analysis_control", "profiles");
  cs_xpath_add_element_num(&path, "profile", id+1);
  cs_xpath_add_element(&path, "format");
  cs_xpath_add_attribute(&path, "name");
  format_s = cs_gui_get_attribute_value(path);

  if (format_s != NULL) {
    if (cs_gui_strcmp(format_s, "CSV"))
      format = 1;
    else if (cs_gui_strcmp(format_s, "DAT"))
      format = 0;
    else
      bft_error(__FILE__, __LINE__, 0,
                _("Invalid attribute value: %s \nXpath: %s\n"), format_s, path);
    BFT_FREE(format_s);
  }

  BFT_FREE(path);

  return format;
}

/*-----------------------------------------------------------------------------
 * Change the head losses matrix from the local frame to the global frame.
 *
 * parameters:
 *   a_ij     <--  change matrix from the local frame to the global frame
 *   in_ij    <--  head losses matrix in the local frame
 *   out_ij   <--  head losses matrix in the global frame
 *----------------------------------------------------------------------------*/

static void
_matrix_base_conversion(double  a11,   double  a12,   double  a13,
                        double  a21,   double  a22,   double  a23,
                        double  a31,   double  a32,   double  a33,
                        double  in11,  double  in12,  double  in13,
                        double  in21,  double  in22,  double  in23,
                        double  in31,  double  in32,  double  in33,
                        double *out11, double *out12, double *out13,
                        double *out21, double *out22, double *out23,
                        double *out31, double *out32, double *out33)
{
  int     i, j, k;
  double  tensorP[3][3], tensorA[3][3], tensorB[3][3];
  double  tensorC[3][3], tensorD[3][3];

  tensorA[0][0] = in11;
  tensorA[0][1] = in12;
  tensorA[0][2] = in13;
  tensorA[1][0] = in21;
  tensorA[1][1] = in22;
  tensorA[1][2] = in23;
  tensorA[2][0] = in31;
  tensorA[2][1] = in32;
  tensorA[2][2] = in33;

  tensorP[0][0] = a11;
  tensorP[0][1] = a12;
  tensorP[0][2] = a13;
  tensorP[1][0] = a21;
  tensorP[1][1] = a22;
  tensorP[1][2] = a23;
  tensorP[2][0] = a31;
  tensorP[2][1] = a32;
  tensorP[2][2] = a33;

  for (i = 0; i < 3; i++) {
    for (j = 0; j < 3; j++) {
      tensorB[i][j] = 0.;
      for (k = 0; k < 3; k++)
        tensorB[i][j] += tensorP[i][k] * tensorA[k][j];
    }
  }

  /* Inversion of a 3x3 matrix */

  tensorC[0][0] = a11;
  tensorC[0][1] = a21;
  tensorC[0][2] = a31;
  tensorC[1][0] = a12;
  tensorC[1][1] = a22;
  tensorC[1][2] = a32;
  tensorC[2][0] = a13;
  tensorC[2][1] = a23;
  tensorC[2][2] = a33;

  for (i = 0; i < 3; i++) {
    for (j = 0; j < 3; j++) {
      tensorD[i][j] = 0.;
      for (k = 0; k < 3; k++)
        tensorD[i][j] += tensorB[i][k] * tensorC[k][j];
    }
  }

  *out11 = tensorD[0][0];
  *out22 = tensorD[1][1];
  *out33 = tensorD[2][2];
  *out12 = tensorD[0][1];
  *out13 = tensorD[0][2];
  *out21 = tensorD[1][0];
  *out23 = tensorD[1][2];
  *out31 = tensorD[2][0];
  *out32 = tensorD[2][1];
}

/*-----------------------------------------------------------------------------
 * Return value of coefficient associated to the head losses definition.
 *
 * parameters:
 *   zone_id   <--  id of the volume zone
 *   c         <--  name of the coefficient
 *----------------------------------------------------------------------------*/

static double
_c_head_losses(const char* zone_id, const char* c)
{
  char* path;
  double result = 0.0;
  double value  = 0.0;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3,
                        "thermophysical_models", "heads_losses", "head_loss");
  cs_xpath_add_test_attribute(&path, "zone_id", zone_id);
  cs_xpath_add_element(&path, c);
  cs_xpath_add_function_text(&path);
  if (cs_gui_get_double(path, &result))
    value = result;
  else
    value= 0.0;
  BFT_FREE(path);
  return value;
}

/*----------------------------------------------------------------------------
 * Return the value of the choice attribute for rotor (turbomachinery)
 *
 * parameters:
 *   rotor_id    <--  id of the rotor
 *   name        <--  name of the property
 *----------------------------------------------------------------------------*/

static double
_rotor_option(int          rotor_id,
              const char  *name)
{
  double value = 0.;
  char *path   = NULL;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 2,
                        "thermophysical_models",
                        "turbomachinery");
  cs_xpath_add_element_num(&path, "rotor", rotor_id + 1);
  cs_xpath_add_element(&path, "rotation");
  cs_xpath_add_element(&path, name);
  cs_xpath_add_function_text(&path);
  cs_gui_get_double(path, &value);
  BFT_FREE(path);

  return value;
}

/*-----------------------------------------------------------------------------
 * Return the value to a face joining markup for turbomachinery
 *
 * parameters:
 *   keyword <-- label of the markup
 *   number  <-- joining number
 *----------------------------------------------------------------------------*/

static char *
_get_rotor_face_joining(const char  *keyword,
                        int          number)
{
  char* value = NULL;
  char *path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3, "thermophysical_models",
                                  "turbomachinery",
                                  "joining");
  cs_xpath_add_element_num(&path, "face_joining", number);
  cs_xpath_add_element(&path, keyword);
  cs_xpath_add_function_text(&path);
  value = cs_gui_get_text_value(path);
  BFT_FREE(path);
  return value;
}

/*! (DOXYGEN_SHOULD_SKIP_THIS) \endcond */

/*============================================================================
 * Public Fortran function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------
 * Thermal model.
 *
 * Fortran Interface:
 *
 * subroutine csther ()
 * *****************
 *
 *----------------------------------------------------------------------------*/


void CS_PROCF (csther, CSTHER) (void)
{
  cs_thermal_model_t *thermal = cs_get_glob_thermal_model();

  switch(cs_gui_thermal_model()) {
  case 10:
    thermal->itherm = 1;
    thermal->itpscl = 2;
    break;
  case 11:
    thermal->itherm = 1;
    thermal->itpscl = 1;
    break;
  case 12:
    thermal->itherm = 1;
    thermal->itpscl = 2;
    break;
  case 13:
    thermal->itherm = 1;
    thermal->itpscl = 2;
    break;
  case 20:
    thermal->itherm = 2;
    thermal->itpscl = 1;
    break;
  case 30:
    thermal->itherm = 3;
    thermal->itpscl = 1;
    break;
  default:
    thermal->itherm = 0;
    thermal->itpscl = 0;
    break;
  }
}

/*----------------------------------------------------------------------------
 * Turbulence model.
 *
 * Fortran Interface:
 *
 * SUBROUTINE CSTURB (ITURB, IWALLF, IGRAKE, IGRAKI, XLOMLG)
 * *****************
 *
 * INTEGER          ITURB   -->   turbulence model
 * INTEGER          IWALLF  -->   wall law treatment
 * INTEGER          IGRAKE  -->   k-eps gravity effects
 * INTEGER          IGRAKI  -->   Rij-eps gravity effects
 * DOUBLE PRECISION XLOMLG  -->   mixing_length_scale
 *----------------------------------------------------------------------------*/

void CS_PROCF (csturb, CSTURB) (void)
{
  char *model = NULL;
  char *flux_model = NULL;

  model = cs_gui_get_thermophysical_model("turbulence");
  if (model == NULL)
    return;

  int iwallf = -1;
  cs_turb_model_t *turb_mdl = cs_get_glob_turb_model();
  cs_turb_rans_model_t *rans_mdl = cs_get_glob_turb_rans_model();

  if (cs_gui_strcmp(model, "off"))
    turb_mdl->iturb = 0;
  else if (cs_gui_strcmp(model, "mixing_length")) {
    turb_mdl->iturb = 10;
    _option_turbulence_double("mixing_length_scale", &(rans_mdl->xlomlg));
  } else if (cs_gui_strcmp(model, "k-epsilon")) {
    turb_mdl->iturb = 20;
    cs_gui_advanced_options_turbulence("wall_function", &iwallf);
    cs_gui_advanced_options_turbulence("gravity_terms", &(rans_mdl->igrake));
  } else if (cs_gui_strcmp(model, "k-epsilon-PL")) {
    turb_mdl->iturb = 21;
    cs_gui_advanced_options_turbulence("wall_function", &iwallf);
    cs_gui_advanced_options_turbulence("gravity_terms", &(rans_mdl->igrake));
  } else if (cs_gui_strcmp(model, "Rij-epsilon")) {
    turb_mdl->iturb = 30;
    cs_gui_advanced_options_turbulence("wall_function", &iwallf);
    cs_gui_advanced_options_turbulence("gravity_terms", &(rans_mdl->igrari));
  } else if (cs_gui_strcmp(model, "Rij-SSG")) {
    turb_mdl->iturb = 31;
    cs_gui_advanced_options_turbulence("wall_function", &iwallf);
    cs_gui_advanced_options_turbulence("gravity_terms", &(rans_mdl->igrari));
  } else if (cs_gui_strcmp(model, "Rij-EBRSM")) {
    turb_mdl->iturb = 32;
    cs_gui_advanced_options_turbulence("wall_function", &iwallf);
    cs_gui_advanced_options_turbulence("gravity_terms", &(rans_mdl->igrari));
  } else if (cs_gui_strcmp(model, "LES_Smagorinsky")) {
    turb_mdl->iturb = 40;
  } else if (cs_gui_strcmp(model, "LES_dynamique")) {
    turb_mdl->iturb = 41;
  } else if (cs_gui_strcmp(model, "LES_WALE")) {
    turb_mdl->iturb = 42;
  } else if (cs_gui_strcmp(model, "v2f-phi")) {
    turb_mdl->iturb = 50;
    cs_gui_advanced_options_turbulence("wall_function", &iwallf);
    cs_gui_advanced_options_turbulence("gravity_terms", &(rans_mdl->igrake));
  } else if (cs_gui_strcmp(model, "v2f-BL-v2/k")) {
    turb_mdl->iturb = 51;
    cs_gui_advanced_options_turbulence("wall_function", &iwallf);
    cs_gui_advanced_options_turbulence("gravity_terms", &(rans_mdl->igrake));
  } else if (cs_gui_strcmp(model, "k-omega-SST")) {
    turb_mdl->iturb = 60;
    cs_gui_advanced_options_turbulence("wall_function", &iwallf);
    cs_gui_advanced_options_turbulence("gravity_terms", &(rans_mdl->igrake));
  } else if (cs_gui_strcmp(model, "Spalart-Allmaras")) {
    turb_mdl->iturb = 70;
  } else
    bft_error(__FILE__, __LINE__, 0,
        _("Invalid turbulence model: %s.\n"), model);

  cs_wall_functions_t *wall_fnt = cs_get_glob_wall_functions();

  if (iwallf !=-1) {
    wall_fnt->iwallf = (cs_wall_f_type_t)iwallf;  // TODO mettre un switch case?
  }
#if _XML_DEBUG_
  bft_printf("==>CSTURB\n");
  bft_printf("--model: %s\n", model);
  bft_printf("--iturb = %i\n", turb_mdl->iturb);
  bft_printf("--igrake = %i\n", rans_mdl->igrake);
  bft_printf("--igrari = %i\n", rans_mdl->igrari);
  bft_printf("--iwallf = %i\n", wall_fnt->iwallf);
  bft_printf("--xlomlg = %f\n", rans_mdl->xlomlg);
#endif

  BFT_FREE(model);
  BFT_FREE(flux_model);
}

/*----------------------------------------------------------------------------
 * Specific heat variable or constant indicator.
 *
 * Fortran Interface:
 *
 * SUBROUTINE CSCPVA (ICP)
 * *****************
 *
 * INTEGER          ICP     -->   specific heat variable or constant indicator
 *----------------------------------------------------------------------------*/

void CS_PROCF (cscpva, CSCPVA) (void)
{
  int choice;
  cs_fluid_properties_t *phys_pp = cs_get_glob_fluid_properties();

  if (_properties_choice_id("specific_heat", &choice))
    phys_pp->icp = choice;

#if _XML_DEBUG_
  bft_printf("==>CSCPVA\n");
  bft_printf("--icp = %i\n", phys_pp->icp);
#endif
}

/*----------------------------------------------------------------------------
 * Volumic viscosity variable or constant indicator.
 *
 * Fortran Interface:
 *
 * SUBROUTINE CSCVVVA (ICP)
 * *****************
 *
 * INTEGER          IVISCV     -->   specific heat variable or constant indicator
 *----------------------------------------------------------------------------*/

void CS_PROCF (csvvva, CSVVVA) (int *iviscv)
{
  int choice;

  if (_properties_choice_id("volume_viscosity", &choice))
    *iviscv = choice;

#if _XML_DEBUG_
  bft_printf("==>CSVVVA\n");
  bft_printf("--iviscv = %i\n", *iviscv);
#endif
}

/*----------------------------------------------------------------------------
 * User thermal scalar.
 *
 * Fortran Interface:
 *
 * SUBROUTINE UITHSC
 * *****************
 *----------------------------------------------------------------------------*/

void CS_PROCF (uithsc, UITHSC) (void)
{
  cs_var_t  *vars = cs_glob_var;
  char *label = NULL;

  const int n_fields = cs_field_n_fields();
  const int keysca = cs_field_key_id("scalar_id");
  const int keylbl = cs_field_key_id("label");
  const int iscalt = cs_glob_thermal_model->iscalt;

  label = _thermal_scalar_name_label("label");

  for (int f_id = 0; f_id < n_fields; f_id++) {
    cs_field_t *f = cs_field_by_id(f_id);
    int i = cs_field_get_key_int(f, keysca) - 1;
    if (i == iscalt - 1) {
#if _XML_DEBUG_
      bft_printf("--label of thermal scalar: %s\n", label);
#endif
      cs_field_set_key_str(f, keylbl, label);
      break;
    }
  }

  BFT_FREE(label);

  BFT_REALLOC(vars->model, strlen("thermal_scalar")+1, char);
  strcpy(vars->model, "thermal_scalar");
}

/*----------------------------------------------------------------------------
 * Constant or variable indicator for the user scalar laminar viscosity.
 *
 * Fortran Interface:
 *
 * subroutine csivis
 * *****************
 *----------------------------------------------------------------------------*/

void CS_PROCF (csivis, CSIVIS) (void)
{
  int choice1, choice2;
  int test1, test2;

  cs_var_t  *vars = cs_glob_var;

  const int keysca = cs_field_key_id("scalar_id");
  const int kivisl = cs_field_key_id("scalar_diffusivity_id");
  const int kscavr = cs_field_key_id("first_moment_id");
  const int n_fields = cs_field_n_fields();
  const int itherm = cs_glob_thermal_model->itherm;
  const int iscalt = cs_glob_thermal_model->iscalt;

  if (vars->model != NULL && itherm) {
    test1 = _properties_choice_id("thermal_conductivity", &choice1);
    test2 = _properties_choice_id("specific_heat", &choice2);

    if (strcmp(vars->model, "thermal_scalar") == 0 && test1 && test2) {

      for (int f_id = 0; f_id < n_fields; f_id++) {
        cs_field_t  *f = cs_field_by_id(f_id);
        if (f->type & CS_FIELD_VARIABLE) {
          if (cs_field_get_key_int(f, keysca) == iscalt) {
            if (choice1 || choice2)
              cs_field_set_key_int(f, kivisl, 0);
            else
              cs_field_set_key_int(f, kivisl, -1);
          }
        }
      }
    }
  }

  for (int f_id = 0; f_id < n_fields; f_id++) {
    cs_field_t  *f = cs_field_by_id(f_id);

    if (   (f->type & CS_FIELD_VARIABLE)
        && (f->type & CS_FIELD_USER)) {
      int i = cs_field_get_key_int(f, keysca) - 1;
      if (i > -1) {
        if (cs_field_get_key_int(f, kscavr) < 0) {
          if (_scalar_properties_choice(i+1, &choice1))
            if (iscalt != i+1)
              cs_field_set_key_int(f, kivisl, choice1 - 1);
          // for groundwater we impose variable property
          if (cs_gui_strcmp(vars->model, "groundwater_model"))
            if (iscalt != i+1)
              cs_field_set_key_int(f, kivisl, 0);
        }
      }
    }
  }

  if (cs_gui_strcmp(vars->model, "compressible_model")) {
    int d_f_id = -1;
    char *prop_choice = _properties_choice("thermal_conductivity");
    if (cs_gui_strcmp(prop_choice, "variable"))
      d_f_id = 0;
    BFT_FREE(prop_choice);
    cs_field_t *c_temp = cs_field_by_name("temperature");
    cs_field_set_key_int(c_temp, kivisl, d_f_id);
  }
}

/*----------------------------------------------------------------------------
 * Time passing parameter.
 *
 * Fortran Interface:
 *
 * SUBROUTINE CSIDTV ()
 * *****************
 *
 *----------------------------------------------------------------------------*/

void CS_PROCF (csidtv, CSIDTV) (void)
{
  double param;
  int steady = 0;
  cs_time_step_options_t *time_opt = cs_get_glob_time_step_options();

  _get_steady_status(&steady);
  if (steady) {
    char *algo_choice = _velocity_pressure_algo_choice();
    if (cs_gui_strcmp(algo_choice, "simple"))
      time_opt->idtvar = -1;
    else
      time_opt->idtvar = 2;
    BFT_FREE(algo_choice);
  } else {
    param = (double) time_opt->idtvar;
    _time_parameters("time_passing", &param);
    time_opt->idtvar = (int) param;
  }

#if _XML_DEBUG_
  bft_printf("==>CSIDTV\n");
  bft_printf("--idtvar = %i\n", time_opt->idtvar);
#endif
}

/*----------------------------------------------------------------------------
 * Hydrostatic pressure parameter.
 *
 * Fortran Interface:
 *
 * SUBROUTINE CSIPHY ()
 * *****************
 *
 *----------------------------------------------------------------------------*/

void CS_PROCF (csiphy, CSIPHY) (void)
{
  char *path = NULL;
  int   result;
  cs_stokes_model_t *stokes = cs_get_glob_stokes_model();

  path = cs_xpath_short_path();
  cs_xpath_add_element(&path, "numerical_parameters");
  cs_xpath_add_element(&path, "hydrostatic_pressure");
  cs_xpath_add_attribute(&path, "status");

  if (cs_gui_get_status(path, &result))
    stokes->iphydr = result;

  BFT_FREE(path);

#if _XML_DEBUG_
  bft_printf("==>CSIPHY\n");
  bft_printf("--iphydr = %i\n", stokes->iphydr);
#endif
}

/*----------------------------------------------------------------------------
 * Hydrostatic equilibrium parameter.
 *
 * Fortran Interface:
 *
 * SUBROUTINE CSCFGP (icfgrp)
 * *****************
 *
 * INTEGER          icfgrp  -->   hydrostatic equilibrium
 *----------------------------------------------------------------------------*/

void CS_PROCF (cscfgp, CSCFGP) (int *icfgrp)
{
  char *path = NULL;
  int   result;

  path = cs_xpath_short_path();
  cs_xpath_add_element(&path, "numerical_parameters");
  cs_xpath_add_element(&path, "hydrostatic_equilibrium");
  cs_xpath_add_attribute(&path, "status");

  if (cs_gui_get_status(path, &result)) *icfgrp = result;

  BFT_FREE(path);

#if _XML_DEBUG_
  bft_printf("==>CSCFGP\n");
  bft_printf("--icfgrp = %i\n", *icfgrp);
#endif
}

/*----------------------------------------------------------------------------
 * Restart parameters.
 *
 * Fortran Interface:
 *
 * SUBROUTINE CSISUI (NTSUIT, ILEAUX, ICCVFG)
 * *****************
 *
 * INTEGER          NTSUIT  -->   checkpoint frequency
 * INTEGER          ILEAUX  -->   restart with auxiliary
 * INTEGER          ICCFVG  -->   restart with frozen field
 *----------------------------------------------------------------------------*/

void CS_PROCF (csisui, CSISUI) (int *ntsuit,
                                int *ileaux,
                                int *iccvfg)
{
  _restart_parameters_status("restart_rescue",         ntsuit);
  _restart_parameters_status("restart_with_auxiliary", ileaux);
  _restart_parameters_status("frozen_field",           iccvfg);

#if _XML_DEBUG_
  bft_printf("==>CSISUI\n");
  bft_printf("--ntsuit = %i\n", *ntsuit);
  bft_printf("--ileaux = %i\n", *ileaux);
  bft_printf("--iccvfg = %i\n", *iccvfg);
#endif
}

/*----------------------------------------------------------------------------
 * Time passing parameters.
 *
 * Fortran Interface:
 *
 * SUBROUTINE CSTIME ()
 *
 *----------------------------------------------------------------------------*/

void CS_PROCF (cstime, CSTIME) (void)
{
  double value;
  /* Default values for time step factor */
  double cdtmin = 0.1, cdtmax = 1000.;
  cs_time_step_options_t *time_opt = cs_get_glob_time_step_options();
  cs_time_step_t *time_stp = cs_get_glob_time_step();

  if (time_opt->idtvar == -1) {
    _steady_parameters("relaxation_coefficient", &(time_opt->relxst));

    value =(double) time_opt->inpdt0;
    _steady_parameters("zero_iteration", &value);
    time_opt->inpdt0 = (int) value;

    value =(double) time_stp->nt_max;
    _steady_parameters("iterations", &value);
    time_stp->nt_max = (int) value;
  } else {
    _time_parameters("time_step_ref", &(time_opt->dtref));
    _time_parameters("time_step_min_factor", &cdtmin);
    _time_parameters("time_step_max_factor", &cdtmax);
    _time_parameters("max_courant_num", &(time_opt->coumax));
    _time_parameters("max_fourier_num", &(time_opt->foumax));
    _time_parameters("time_step_var", &(time_opt->varrdt));

    time_opt->dtmin = cdtmin * time_opt->dtref;
    time_opt->dtmax = cdtmax * time_opt->dtref;

    /* We keep these two lines in case we read an old XML file... */
    _time_parameters("time_step_min", &(time_opt->dtmin));
    _time_parameters("time_step_max", &(time_opt->dtmax));

    value =(double) time_stp->nt_max;
    _time_parameters("iterations", &value);
    time_stp->nt_max = (int) value;

    value =(double) time_opt->inpdt0;
    _time_parameters("zero_time_step", &value);
    time_opt->inpdt0 = (int) value;

    value =(double) time_opt->iptlro;
    _time_parameters("thermal_time_step", &value);
    time_opt->iptlro = (int) value;
  }

#if _XML_DEBUG_
  bft_printf("==>CSTIME\n");
  bft_printf("--idtvar = %i\n", time_opt->idtvar);
  if (time_opt->idtvar == -1) {
    bft_printf("--inpdt0 = %i\n", time_opt->inpdt0);
    bft_printf("--relxst = %f\n", time_opt->relxst);
  } else {
    bft_printf("--inpdt0 = %i\n", time_opt->inpdt0);
    bft_printf("--iptlro = %i\n", time_opt->iptlro);
    bft_printf("--ntmabs = %i\n", time_stp->nt_max);
    bft_printf("--dtref = %f\n",  time_opt->dtref);
    bft_printf("--dtmin = %f\n",  time_opt->dtmin);
    bft_printf("--dtmax = %f\n",  time_opt->dtmax);
    bft_printf("--coumax = %f\n", time_opt->coumax);
    bft_printf("--foumax = %f\n", time_opt->foumax);
    bft_printf("--varrdt = %f\n", time_opt->varrdt);
  }
#endif
}

/*----------------------------------------------------------------------------
 * Treatment of local numerical aspects:
 *     BLENCV, ISCHCV, ISSTPC, IRCFLU, CDTVAR, NITMAX, EPSILO
 *----------------------------------------------------------------------------*/

void CS_PROCF (uinum1, UINUM1) (double  *blencv,
                                int     *ischcv,
                                int     *isstpc,
                                int     *ircflu,
                                double  *cdtvar,
                                double  *epsilo,
                                int     *nswrsm)
{
  double tmp;
  int key_cal_opt_id = cs_field_key_id("var_cal_opt");
  int var_key_id = cs_field_key_id("variable_id");
  cs_var_cal_opt_t var_cal_opt;

  /* 1) variables from velocity_pressure and turbulence */
  /* 1-a) for pressure */
  cs_field_t *c_pres = cs_field_by_name("pressure");
  cs_field_get_key_struct(c_pres, key_cal_opt_id, &var_cal_opt);
  int j = cs_field_get_key_int(c_pres, var_key_id) -1;

  _variable_value(c_pres->name, "solver_precision", &epsilo[j]);

  tmp = (double) nswrsm[j];
  _variable_value(c_pres->name, "rhs_reconstruction", &tmp);
  nswrsm[j] = (int) tmp;

  /* Set Field calculation options in the field structure */
  var_cal_opt.epsilo = epsilo[j];
  var_cal_opt.nswrsm = nswrsm[j];
  cs_field_set_key_struct(c_pres, key_cal_opt_id, &var_cal_opt);

  /* 1-b) for the other variables */
  int n_fields = cs_field_n_fields();
  for (int f_id = 0; f_id < n_fields; f_id++) {
    cs_field_t  *f = cs_field_by_id(f_id);
    if (f->type & CS_FIELD_VARIABLE && !cs_gui_strcmp(f->name, "pressure")) {
      j = cs_field_get_key_int(f, var_key_id) -1;
      cs_field_get_key_struct(f, key_cal_opt_id, &var_cal_opt);

      _variable_value(f->name, "blending_factor", &blencv[j]);
      _variable_value(f->name, "solver_precision", &epsilo[j]);

      // only for nscaus and model scalar
      _variable_value(f->name, "time_step_factor", &cdtvar[j]);

      _variable_attribute(f->name, "order_scheme", &ischcv[j]);
      _variable_attribute(f->name, "slope_test", &isstpc[j]);
      _variable_attribute(f->name, "flux_reconstruction", &ircflu[j]);
      tmp = (double) nswrsm[j];
      _variable_value(f->name, "rhs_reconstruction", &tmp);
      nswrsm[j] = (int) tmp;

      // Set Field calculation options in the field structure
      var_cal_opt.blencv = blencv[j];
      var_cal_opt.epsilo = epsilo[j];
      // TODO add nitmax, imgr, iresol, cdtvar
      var_cal_opt.nswrsm = nswrsm[j];
      cs_field_set_key_struct(f, key_cal_opt_id, &var_cal_opt);
    }
  }

#if _XML_DEBUG_
  bft_printf("==>UINUM1\n");
  for (int f_id = 0; f_id < n_fields; f_id++) {
    const cs_field_t  *f = cs_field_by_id(f_id);
    if (f->type & CS_FIELD_VARIABLE) {
      j = cs_field_get_key_int(f, var_key_id) -1;
      bft_printf("-->variable[%i] = %s\n", j, f->name);
      bft_printf("--blencv = %f\n", blencv[j]);
      bft_printf("--epsilo = %g\n", epsilo[j]);
      bft_printf("--cdtvar = %g\n", cdtvar[j]);
      //bft_printf("--nitmax = %i\n", nitmax[j]);
      bft_printf("--ischcv = %i\n", ischcv[j]);
      bft_printf("--isstpc = %i\n", isstpc[j]);
      bft_printf("--ircflu = %i\n", ircflu[j]);
      bft_printf("--nswrsm = %i\n", nswrsm[j]);
      //bft_printf("--imgr = %i\n"  , imgr[j]);
      //bft_printf("--iresol = %i\n", iresol[j]);
    }
  }
#endif
}

/*----------------------------------------------------------------------------
 * Global numerical parameters.
 *
 * Fortran Interface:
 *
 * SUBROUTINE CSNUM2 (RELAXP, EXTRAG, IMRGRA)
 * *****************
 * DOUBLE PRECISION RELAXP  -->   pressure relaxation
 * DOUBLE PRECISION EXTRAG  -->   wall pressure extrapolation
 * INTEGER          IMRGRA  -->   gradient reconstruction
 *----------------------------------------------------------------------------*/

void CS_PROCF (csnum2, CSNUM2)(double *relaxp,
                               double *extrag,
                                  int *imrgra)
{
  cs_piso_t *piso = cs_get_glob_piso();
  cs_stokes_model_t *stokes = cs_get_glob_stokes_model();
  _numerical_int_parameters("gradient_transposed", &(stokes->ivisse));
  _numerical_int_parameters("velocity_pressure_coupling", &(stokes->ipucou));
  _numerical_int_parameters("gradient_reconstruction", imrgra);
  _numerical_int_parameters("piso_sweep_number", &(piso->nterup));
  cs_gui_numerical_double_parameters("wall_pressure_extrapolation", extrag);
  cs_gui_numerical_double_parameters("pressure_relaxation", relaxp);

#if _XML_DEBUG_
  bft_printf("==>CSNUM2\n");
  bft_printf("--ivisse = %i\n", stokes->ivisse);
  bft_printf("--ipucou = %i\n", stokes->ipucou);
  bft_printf("--imrgra = %i\n", *imrgra);
  bft_printf("--nterup = %i\n", piso->nterup);
  bft_printf("--extrag = %f\n", *extrag);
  bft_printf("--relaxp = %f\n", *relaxp);
#endif
}

/*----------------------------------------------------------------------------
 * Treatment of gravity and fluid physical properties
 * Initialize reference pressure and temperature if present
 *----------------------------------------------------------------------------*/

void CS_PROCF (csphys, CSPHYS) (const int  *nmodpp,
                                    double *viscv0,
                                    double *visls0,
                                const int  *itempk)
{
  int choice;
  char *material = NULL;
  char *phas = NULL;

  cs_var_t  *vars = cs_glob_var;

  const int itherm = cs_glob_thermal_model->itherm;

  cs_physical_constants_t *phys_cst = cs_get_glob_physical_constants();

  _gravity_value("gravity_x", &(phys_cst->gx));
  _gravity_value("gravity_y", &(phys_cst->gy));
  _gravity_value("gravity_z", &(phys_cst->gz));

  cs_real_t w_x, w_y, w_z;
  w_x = 0.;
  w_y = 0.;
  w_z = 0.;

  _coriolis_value("omega_x", &w_x);
  _coriolis_value("omega_y", &w_y);
  _coriolis_value("omega_z", &w_z);

  if (w_x*w_x + w_y*w_y + w_z*w_z > 0.) {
    cs_rotation_define(w_x, w_y, w_z, 0, 0, 0);
    phys_cst->icorio = 1;
  }
  else
    phys_cst->icorio = 0;

  cs_fluid_properties_t *phys_pp = cs_get_glob_fluid_properties();
  cs_gui_reference_initialization("pressure", &(phys_pp->p0));

  /* Variable rho and viscl */
  if (*nmodpp == 0) {
    if (_properties_choice_id("density", &choice))
      phys_pp->irovar = choice;

    if (_properties_choice_id("molecular_viscosity", &choice))
      phys_pp->ivivar = choice;
  }
  if (cs_gui_strcmp(vars->model, "compressible_model"))
    if (_properties_choice_id("molecular_viscosity", &choice))
      phys_pp->ivivar = choice;

  // Read T0 in each case for user
  cs_gui_reference_initialization("temperature", &(phys_pp->t0));

  if (cs_gui_strcmp(vars->model, "compressible_model"))
    cs_gui_reference_initialization("mass_molar", &(phys_pp->xmasmr));

  material = _thermal_table_choice("material");
  if (material != NULL) {
    if (!(cs_gui_strcmp(material, "user_material"))) {
      phas = _thermal_table_choice("phas");

      if (!phas) {
        BFT_MALLOC(phas, 6, char);
        strcpy(phas, "undef");
      }

      cs_phys_prop_thermo_plane_type_t thermal_plane = CS_PHYS_PROP_PLANE_PH;
      if (itherm <= 1)
        thermal_plane = CS_PHYS_PROP_PLANE_PT;
      //else if (itherm == 3)
      //  // TODO compressible
      //  thermal_plane = CS_PHYS_PROP_PLANE_PS;

      const int itpscl = cs_glob_thermal_model->itpscl;

      cs_thermal_table_set(material,
                           _thermal_table_choice("method"),
                           phas,
                           _thermal_table_choice("reference"),
                           thermal_plane,
                           itpscl);
    }
    BFT_FREE(material);
  }

  /* ro0, viscl0, cp0, isls0[iscalt-1] si tables */
  if (_thermal_table_needed("density") == 0)
    cs_gui_properties_value("density", &phys_pp->ro0);
  else
    cs_phys_prop_compute(CS_PHYS_PROP_DENSITY,
                         1,
                         &phys_pp->p0,
                         &phys_pp->t0,
                         &phys_pp->ro0);

  if (_thermal_table_needed("molecular_viscosity") == 0)
    cs_gui_properties_value("molecular_viscosity", &phys_pp->viscl0);
  else
    cs_phys_prop_compute(CS_PHYS_PROP_DYNAMIC_VISCOSITY,
                         1,
                         &phys_pp->p0,
                         &phys_pp->t0,
                         &phys_pp->viscl0);

  if (_thermal_table_needed("specific_heat") == 0)
    cs_gui_properties_value("specific_heat", &phys_pp->cp0);
  else
    cs_phys_prop_compute(CS_PHYS_PROP_ISOBARIC_HEAT_CAPACITY,
                         1,
                         &phys_pp->p0,
                         &phys_pp->t0,
                         &phys_pp->cp0);

  if (cs_gui_strcmp(vars->model, "compressible_model")) {
    cs_gui_properties_value("volume_viscosity", viscv0);
    cs_gui_properties_value("thermal_conductivity", &visls0[*itempk -1]);
  }

#if _XML_DEBUG_
  bft_printf("==>CSPHYS\n");
  bft_printf("--gx = %f \n",*gx);
  bft_printf("--gy = %f \n",*gy);
  bft_printf("--gz = %f \n",*gz);
  //bft_printf("--omegax = %f \n",*omegax);
  //bft_printf("--omegay = %f \n",*omegay);
  //bft_printf("--omegaz = %f \n",*omegaz);
  bft_printf("--icorio = %i \n", cs_glob_physical_constants->icorio);
  bft_printf("--rho = %g , variable %i\n", cs_glob_fluid_properties->ro0, cs_glob_fluid_properties->irovar);
  bft_printf("--mu = %g , variable %i \n", cs_glob_fluid_properties->viscl0, cs_glob_fluid_properties->ivivar);
  bft_printf("--Cp = %g \n", cs_glob_fluid_properties->cp0);
  bft_printf("--T0 = %f \n", cs_glob_fluid_properties->t0);
  bft_printf("--P0 = %f \n", cs_glob_fluid_properties->p0);
  if (cs_gui_strcmp(vars->model, "compressible_model")) {
    bft_printf("--viscv0 = %g \n", *viscv0);
    bft_printf("--xmasmr = %f \n", cs_glob_fluid_properties->xmasmr);
  }
#endif
}

/*----------------------------------------------------------------------------
 * User scalar min and max values for clipping.
 *
 * Fortran Interface:
 *
 * subroutine cssca2 (iturt)
 * *****************
 *
 * integer          iturt    -->  turbulent flux model
 *----------------------------------------------------------------------------*/

void CS_PROCF (cssca2, CSSCA2) (int        *iturt)
{
#if _XML_DEBUG_
  bft_printf("==>CSSCA2\n");
#endif

  cs_var_t  *vars = cs_glob_var;

  const int kscmin = cs_field_key_id("min_scalar_clipping");
  const int kscmax = cs_field_key_id("max_scalar_clipping");

  /* Specific physics: the min max of the model scalar are not given */
  const int keysca = cs_field_key_id("scalar_id");
  const int kscavr = cs_field_key_id("first_moment_id");

  for (int f_id = 0; f_id < cs_field_n_fields(); f_id++) {
    cs_field_t  *f = cs_field_by_id(f_id);
    if (   (f->type & CS_FIELD_VARIABLE)
        && (f->type & CS_FIELD_USER)) {
      int i = cs_field_get_key_int(f, keysca) - 1;
      if (i > -1) {
        if (cs_field_get_key_int(f, kscavr) < 0) {
          double scal_min = cs_field_get_key_double(f, kscmin);
          double scal_max = cs_field_get_key_double(f, kscmax);
          _variable_value(f->name, "min_value", &scal_min);
          _variable_value(f->name, "max_value", &scal_max);
          cs_field_set_key_double(f, kscmin, scal_min);
          cs_field_set_key_double(f, kscmax, scal_max);

          if (cs_glob_turb_model->iturb/10 == 3) {
            int turb_mdl;
            _variable_turbulent_flux_model(f->name, &turb_mdl);
            iturt[i] = turb_mdl;
          }
#if _XML_DEBUG_
          bft_printf("--min_scalar_clipping[%i] = %f\n", i, scal_min);
          bft_printf("--max_scalar_clipping[%i] = %f\n", i, scal_max);
#endif
        }
      }
    }
  }

  if (cs_gui_strcmp(vars->model, "thermal_scalar")) {

    /* thermal model with no specific physics */

    const int itherm = cs_glob_thermal_model->itherm;
    assert(itherm > 0);

    const char *t_names[] = {"temperature", "enthalpy", "total_energy"};

    cs_field_t *f = cs_field_by_name(t_names[itherm-1]);

    double scal_min = cs_field_get_key_double(f, kscmin);
    double scal_max = cs_field_get_key_double(f, kscmax);
    _variable_value(f->name, "min_value", &scal_min);
    _variable_value(f->name, "max_value", &scal_max);
    cs_field_set_key_double(f, kscmin, scal_min);
    cs_field_set_key_double(f, kscmax, scal_max);
    int i = cs_field_get_key_int(f, keysca) - 1;

    if (cs_glob_turb_model->iturb/10 == 3) {
      int turb_mdl;
      _variable_turbulent_flux_model(f->name, &turb_mdl);
      iturt[i] = turb_mdl;
    }
#if _XML_DEBUG_
    bft_printf("--min_scalar_clipping[%i] = %f\n", i, scal_min);
    bft_printf("--max_scalar_clipping[%i] = %f\n", i, scal_max);
#endif
  }
}

/*----------------------------------------------------------------------------
 * Read reference dynamic and user scalar viscosity
 *----------------------------------------------------------------------------*/

void CS_PROCF (cssca3, CSSCA3) (double     *visls0)
{
  double result, coeff, density;

  cs_var_t  *vars = cs_glob_var;

  const int keysca = cs_field_key_id("scalar_id");
  const int kscavr = cs_field_key_id("first_moment_id");

  const int itherm = cs_glob_thermal_model->itherm;
  const int iscalt = cs_glob_thermal_model->iscalt;

  if (vars->model != NULL) {

    if (itherm != 0) {
      int i = iscalt-1;

      if (_thermal_table_needed("thermal_conductivity") == 0)
        cs_gui_properties_value("thermal_conductivity", &visls0[i]);
      else
        cs_phys_prop_compute(CS_PHYS_PROP_THERMAL_CONDUCTIVITY,
                             1, &(cs_glob_fluid_properties->p0),
                             &(cs_glob_fluid_properties->t0), &visls0[i]);

      /* for the Temperature, the diffusivity factor is not divided by Cp */
      if (itherm != 1)
        visls0[i] = visls0[i] / cs_glob_fluid_properties->cp0;
    }
  }

  /* User scalar
     In the interface, the user gives the diffusion coefficient, whereas in
     the solver, one sets the diffusivity, thus one need to multiply
     this coefficient by the density to remain coherent */

  if (!cs_gui_strcmp(vars->model, "groundwater_model")) {
    int n_fields = cs_field_n_fields();
    for (int f_id = 0; f_id < n_fields; f_id++) {
      const cs_field_t  *f = cs_field_by_id(f_id);
      if (   (f->type & CS_FIELD_VARIABLE)
          && (f->type & CS_FIELD_USER)) {
        int i = cs_field_get_key_int(f, keysca) - 1;
        if (cs_field_get_key_int(f, kscavr) < 0) {

          if (cs_gui_strcmp(vars->model, "solid_fuels")) {
            /* Air molar mass */
            result = 0.028966;
            cs_gui_reference_initialization("mass_molar", &result);
            if (result <= 0)
              bft_error(__FILE__, __LINE__, 0,
                        _("mass molar value is zero or not found in the xml file.\n"));
            density = cs_glob_fluid_properties->p0 *
                      result / (8.31446 *(cs_glob_fluid_properties->t0));
          }
          else
            cs_gui_properties_value("density", &density);

          if (density <= 0)
            bft_error(__FILE__, __LINE__, 0,
                      _("Density value is zero or not found in the xml file.\n"));

          coeff = visls0[i] / density ;
          _scalar_diffusion_value(i+1, &coeff);
          visls0[i] = coeff * density;
        }
#if _XML_DEBUG_
        bft_printf("--visls0[%i] = %f\n", i, visls0[i]);
#endif
      }
    }
  }
}

/*----------------------------------------------------------------------------
 * Turbulence initialization parameters.
 *
 * Fortran Interface:
 *
 * SUBROUTINE CSTINI ()
 * *****************
 *
 *----------------------------------------------------------------------------*/

void CS_PROCF (cstini, CSTINI) (void)
{
  char* length_choice = NULL;
  cs_turb_ref_values_t *ref_values = cs_get_glob_turb_ref_values();

  ref_values->uref = 1.; /* default if not specified */

  cs_gui_reference_initialization("velocity", &(ref_values->uref));

  length_choice = _reference_length_initialization_choice();

  if (length_choice != NULL) {
    if (cs_gui_strcmp(length_choice, "prescribed"))
      cs_gui_reference_initialization("length", &(ref_values->almax));
    BFT_FREE(length_choice);
  }

#if _XML_DEBUG_
  bft_printf("==>CSTINI\n");
  bft_printf("--almax = %f\n", ref_values->almax);
  bft_printf("--uref  = %f\n", ref_values->uref);
#endif
}

/*----------------------------------------------------------------------------
 * Solver taking a scalar porosity into account
 *
 * Fortran Interface:
 *
 * SUBROUTINE UIIPSU
 * *****************
 *
 * INTEGER          IPOROS     -->   porosity
 *----------------------------------------------------------------------------*/

void CS_PROCF (uiipsu, UIIPSU) (int *iporos)
{
  char *path = NULL;
  char *status = NULL;

  /* number of volumic zone */
  int zones = cs_gui_get_tag_count("/solution_domain/volumic_conditions/zone\n", 1);

  for (int i = 1; i < zones+1; i++) {
    path = cs_xpath_init_path();
    cs_xpath_add_elements(&path, 2, "solution_domain", "volumic_conditions");
    cs_xpath_add_element_num(&path, "zone", i);
    cs_xpath_add_attribute(&path, "porosity");
    status = cs_gui_get_attribute_value(path);
    BFT_FREE(path);

    if (cs_gui_strcmp(status, "on")) {
      char *zone_id = _volumic_zone_id(i);
      path = cs_xpath_init_path();
      cs_xpath_add_elements(&path, 3,
                            "thermophysical_models",
                            "porosities",
                            "porosity");
      cs_xpath_add_test_attribute(&path, "zone_id", zone_id);
      BFT_FREE(zone_id);
      cs_xpath_add_attribute(&path, "model");
      char *mdl = cs_gui_get_attribute_value(path);
      BFT_FREE(path);
      if (cs_gui_strcmp(mdl, "anisotropic"))
        *iporos = 2;
      else
        *iporos = CS_MAX(1, *iporos);

      BFT_FREE(mdl);
    }

    BFT_FREE(status);
  }
}

/*----------------------------------------------------------------------------
 * Define porosity.
 *
 * Fortran Interface:
 *
 * SUBROUTINE UIPORO
 * *****************
 *
 * integer          iporos   <--  porosity model
 *----------------------------------------------------------------------------*/

void CS_PROCF (uiporo, UIPORO) (const int        *iporos)
{
  const cs_lnum_t n_cells_ext = cs_glob_mesh->n_cells_with_ghosts;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)cs_glob_mesh_quantities->cell_cen;

  char *path = NULL;
  char *status = NULL;
  char *formula = NULL;
  cs_lnum_t *cells_list = NULL;
  cs_lnum_t cells = 0;

  mei_tree_t *ev_formula  = NULL;

  assert(*iporos == 1 || *iporos == 2 || *iporos == 3);

  /* number of volumic zone */
  int zones = cs_gui_get_tag_count("/solution_domain/volumic_conditions/zone\n", 1);

  /* Porosity fields */
  cs_field_t *fporo = CS_F_(poro);
  cs_field_t *ftporo = CS_F_(t_poro);

  cs_real_t   *porosi = NULL;
  cs_real_6_t *porosf = NULL;

  if (fporo != NULL) {
    porosi = fporo->val;
    if (ftporo != NULL) {
      porosf = (cs_real_6_t *)ftporo->val;
    }
  }

  for (cs_lnum_t iel = 0; iel < n_cells_ext; iel++) {
    porosi[iel] = 1.;
    if (ftporo != NULL) {
      porosf[iel][0] = 1.;
      porosf[iel][1] = 1.;
      porosf[iel][2] = 1.;
      porosf[iel][3] = 0.;
      porosf[iel][4] = 0.;
      porosf[iel][5] = 0.;
    }
  }

  for (int i = 1; i < zones+1; i++) {
    path = cs_xpath_init_path();
    cs_xpath_add_elements(&path, 2, "solution_domain", "volumic_conditions");
    cs_xpath_add_element_num(&path, "zone", i);
    cs_xpath_add_attribute(&path, "porosity");
    status = cs_gui_get_attribute_value(path);
    BFT_FREE(path);

    if (cs_gui_strcmp(status, "on")) {
      char *zone_id = _volumic_zone_id(i);
      cells_list = _get_cells_list(zone_id, n_cells_ext, &cells);

      path = cs_xpath_init_path();
      cs_xpath_add_elements(&path, 3,
                            "thermophysical_models",
                            "porosities",
                            "porosity");
      cs_xpath_add_test_attribute(&path, "zone_id", zone_id);
      cs_xpath_add_attribute(&path, "model");
      char *mdl = cs_gui_get_attribute_value(path);
      BFT_FREE(path);

      path = cs_xpath_init_path();
      cs_xpath_add_elements(&path, 3,
                            "thermophysical_models",
                            "porosities",
                            "porosity");

      cs_xpath_add_test_attribute(&path, "zone_id",zone_id);
      cs_xpath_add_element(&path, "formula");
      cs_xpath_add_function_text(&path);
      formula = cs_gui_get_text_value(path);
      BFT_FREE(path);

      if (formula != NULL) {
        ev_formula = mei_tree_new(formula);
        BFT_FREE(formula);
        mei_tree_insert(ev_formula,"x",0.0);
        mei_tree_insert(ev_formula,"y",0.0);
        mei_tree_insert(ev_formula,"z",0.0);

        /* try to build the interpreter */
        if (mei_tree_builder(ev_formula))
          bft_error(__FILE__, __LINE__, 0,
                    _("Error: can not interpret expression: %s\n %i"),
                    ev_formula->string, mei_tree_builder(ev_formula));

        if (cs_gui_strcmp(mdl, "anisotropic")) {
          const char *symbols[] = {"porosity",
                                   "porosity[XX]",
                                   "porosity[YY]",
                                   "porosity[ZZ]",
                                   "porosity[XY]",
                                   "porosity[YZ]",
                                   "porosity[XZ]"};
          if (mei_tree_find_symbols(ev_formula, 7, symbols))
            bft_error(__FILE__, __LINE__, 0,
                      _("Error: can not find the required symbol: %s\n %s\n"),
                      "porosity, porosity[XX], porosity[YY], porosity[ZZ]",
                      "          porosity[XY], porosity[XZ] or porosity[YZ]");
        }
        else {
          const char *symbols[] = {"porosity"};
          if (mei_tree_find_symbols(ev_formula, 1, symbols))
            bft_error(__FILE__, __LINE__, 0,
                      _("Error: can not find the required symbol: %s\n"),
                      "porosity");
        }

        for (cs_lnum_t icel = 0; icel < cells; icel++) {
          cs_lnum_t iel = cells_list[icel];
          mei_tree_insert(ev_formula, "x", cell_cen[iel][0]);
          mei_tree_insert(ev_formula, "y", cell_cen[iel][1]);
          mei_tree_insert(ev_formula, "z", cell_cen[iel][2]);
          mei_evaluate(ev_formula);

          porosi[iel] = mei_tree_lookup(ev_formula,"porosity");
          if (cs_gui_strcmp(mdl, "anisotropic")) {
              porosf[iel][0] = mei_tree_lookup(ev_formula,"porosity[XX]");
              porosf[iel][1] = mei_tree_lookup(ev_formula,"porosity[YY]");
              porosf[iel][2] = mei_tree_lookup(ev_formula,"porosity[ZZ]");
              porosf[iel][3] = mei_tree_lookup(ev_formula,"porosity[XY]");
              porosf[iel][4] = mei_tree_lookup(ev_formula,"porosity[YZ]");
              porosf[iel][5] = mei_tree_lookup(ev_formula,"porosity[XZ]");
          }
        }

        mei_tree_destroy(ev_formula);
      }
      BFT_FREE(cells_list);
      BFT_FREE(zone_id);
      BFT_FREE(mdl);
    }
    BFT_FREE(status);
  }
}

/*----------------------------------------------------------------------------
 * User momentum source terms.
 *
 * Fortran Interface:
 *
 * subroutine uitsnv (vel, tsexp, tsimp)
 * *****************
 *
 * double precision vel      <--  fluid velocity
 * double precision tsexp    -->  explicit source terms
 * double precision tsimp    -->  implicit source terms
 *----------------------------------------------------------------------------*/

void CS_PROCF(uitsnv, UITSNV)(const cs_real_3_t  *restrict vel,
                                    cs_real_3_t  *restrict tsexp,
                                    cs_real_33_t *restrict tsimp)
{
  const cs_lnum_t n_cells_ext = cs_glob_mesh->n_cells_with_ghosts;
  const cs_real_t *restrict cell_f_vol = cs_glob_mesh_quantities->cell_f_vol;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)cs_glob_mesh_quantities->cell_cen;


  cs_lnum_t i, icel, iel;
  int zones = 0;
  cs_lnum_t cells = 0;
  cs_lnum_t *cells_list = NULL;
  double dSudu, dSudv, dSudw;
  double dSvdu, dSvdv, dSvdw;
  double dSwdu, dSwdv, dSwdw;
  char *path = NULL;
  char *status = NULL;
  char *zone_id = NULL;
  char *formula = NULL;

  mei_tree_t *ev_formula  = NULL;

  /* number of volumic zone */

  zones = cs_gui_get_tag_count("/solution_domain/volumic_conditions/zone\n", 1);

#if _XML_DEBUG_
  bft_printf("==>UITSNV\n");
#endif

  for (i=1; i < zones+1; i++) {
    /* momentum source term */
    path = cs_xpath_init_path();
    cs_xpath_add_elements(&path, 2, "solution_domain", "volumic_conditions");
    cs_xpath_add_element_num(&path, "zone", i);
    cs_xpath_add_attribute(&path, "momentum_source_term");
    status = cs_gui_get_attribute_value(path);
    BFT_FREE(path);

    if (cs_gui_strcmp(status, "on")) {
      zone_id = _volumic_zone_id(i);
      cells_list = _get_cells_list(zone_id, n_cells_ext, &cells);

      path = cs_xpath_init_path();
      cs_xpath_add_elements(&path, 1, "thermophysical_models");
      cs_xpath_add_elements(&path, 1, "source_terms");
      cs_xpath_add_elements(&path, 1, "momentum_formula");
      cs_xpath_add_test_attribute(&path, "zone_id",zone_id);
      cs_xpath_add_function_text(&path);
      formula = cs_gui_get_text_value(path);
      BFT_FREE(path);
      if (formula != NULL) {
        ev_formula = mei_tree_new(formula);
        mei_tree_insert(ev_formula,"x",0.0);
        mei_tree_insert(ev_formula,"y",0.0);
        mei_tree_insert(ev_formula,"z",0.0);
        mei_tree_insert(ev_formula, "velocity[0]", 0.0);
        mei_tree_insert(ev_formula, "velocity[1]", 0.0);
        mei_tree_insert(ev_formula, "velocity[2]", 0.0);
        /* try to build the interpreter */
        if (mei_tree_builder(ev_formula))
          bft_error(__FILE__, __LINE__, 0,
                    _("Error: can not interpret expression: %s\n %i"),
                    ev_formula->string, mei_tree_builder(ev_formula));
        const char *symbols[] = {"Su","Sv","Sw",
                                 "dSudu","dSudv","dSudw",
                                 "dSvdu","dSvdv","dSvdw",
                                 "dSwdu","dSwdv","dSwdw"};
        if (mei_tree_find_symbols(ev_formula, 12, symbols))
          bft_error(__FILE__, __LINE__, 0,
                    _("Error: can not find the required symbol: %s\n%s\n%s\n%s\n"),
                    "Su, Sv, Sw,",
                    "dSudu, dSudv, dSudw,",
                    "dSvdu, dSvdv, dSvdw,",
                    "dSwdu, dSwdv or dSwdw");
        for (icel = 0; icel < cells; icel++) {
          iel = cells_list[icel];
          mei_tree_insert(ev_formula, "x", cell_cen[iel][0]);
          mei_tree_insert(ev_formula, "y", cell_cen[iel][1]);
          mei_tree_insert(ev_formula, "z", cell_cen[iel][2]);
          mei_tree_insert(ev_formula, "velocity[0]", vel[iel][0]);
          mei_tree_insert(ev_formula, "velocity[1]", vel[iel][1]);
          mei_tree_insert(ev_formula, "velocity[2]", vel[iel][2]);
          mei_evaluate(ev_formula);

          dSudu = mei_tree_lookup(ev_formula,"dSudu");
          dSudv = mei_tree_lookup(ev_formula,"dSudv");
          dSudw = mei_tree_lookup(ev_formula,"dSudw");
          dSvdu = mei_tree_lookup(ev_formula,"dSvdu");
          dSvdv = mei_tree_lookup(ev_formula,"dSvdv");
          dSvdw = mei_tree_lookup(ev_formula,"dSvdw");
          dSwdu = mei_tree_lookup(ev_formula,"dSwdu");
          dSwdv = mei_tree_lookup(ev_formula,"dSwdv");
          dSwdw = mei_tree_lookup(ev_formula,"dSwdw");

          tsimp[iel][0][0] = cell_f_vol[iel]*dSudu;
          tsimp[iel][0][1] = cell_f_vol[iel]*dSudv;
          tsimp[iel][0][2] = cell_f_vol[iel]*dSudw;
          tsimp[iel][1][0] = cell_f_vol[iel]*dSvdu;
          tsimp[iel][1][1] = cell_f_vol[iel]*dSvdv;
          tsimp[iel][1][2] = cell_f_vol[iel]*dSvdw;
          tsimp[iel][2][0] = cell_f_vol[iel]*dSwdu;
          tsimp[iel][2][1] = cell_f_vol[iel]*dSwdv;
          tsimp[iel][2][2] = cell_f_vol[iel]*dSwdw;

          tsexp[iel][0] = mei_tree_lookup(ev_formula,"Su")
                        - ( dSudu*vel[iel][0]
                          + dSudv*vel[iel][1]
                          + dSudw*vel[iel][2]
                          );
          tsexp[iel][0] *= cell_f_vol[iel];
          tsexp[iel][1] = mei_tree_lookup(ev_formula,"Sv")
                        - ( dSvdu*vel[iel][0]
                          + dSvdv*vel[iel][1]
                          + dSvdw*vel[iel][2]
                          );
          tsexp[iel][1] *= cell_f_vol[iel];
          tsexp[iel][2] = mei_tree_lookup(ev_formula,"Sw")
                        - ( dSwdu*vel[iel][0]
                          + dSwdv*vel[iel][1]
                          + dSwdw*vel[iel][2]
                          );
          tsexp[iel][2] *= cell_f_vol[iel];
        }
        mei_tree_destroy(ev_formula);
      }
      BFT_FREE(cells_list);
      BFT_FREE(zone_id);
    }
    BFT_FREE(status);
  }
}

/*----------------------------------------------------------------------------
 * User scalar source terms.
 *
 * Fortran Interface:
 *
 * subroutine uitssc (f_id, pvar, tsexp, tsimp)
 * *****************
 *
 * integer          idarcy   <--  groundwater module activation
 * integer          f_id     <--  field id
 * double precision pvar     <--  scalar
 * double precision tsexp    -->  explicit source terms
 * double precision tsimp    -->  implicit source terms
 *----------------------------------------------------------------------------*/

void CS_PROCF(uitssc, UITSSC)(const int                  *idarcy,
                              const int                  *f_id,
                              const cs_real_t   *restrict pvar,
                              cs_real_t         *restrict tsexp,
                              cs_real_t         *restrict tsimp)
{
  const cs_lnum_t n_cells_ext = cs_glob_mesh->n_cells_with_ghosts;

  const cs_real_t *restrict cell_f_vol = cs_glob_mesh_quantities->cell_f_vol;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)cs_glob_mesh_quantities->cell_cen;

  cs_lnum_t i, icel, iel;
  int zones = 0;
  cs_lnum_t cells = 0;
  cs_lnum_t *cells_list = NULL;
  double dS;
  char *path = NULL;
  char *status = NULL;
  char *zone_id = NULL;
  char *formula = NULL;

  mei_tree_t *ev_formula  = NULL;
  cs_field_t  *f = cs_field_by_id(*f_id);

  /* number of volumic zone */

  zones = cs_gui_get_tag_count("/solution_domain/volumic_conditions/zone\n", 1);

#if _XML_DEBUG_
  bft_printf("==>UITSSC\n");
#endif

  for (i=1; i < zones+1; i++) {

    /* species source term */
    path = cs_xpath_init_path();
    cs_xpath_add_elements(&path, 2, "solution_domain", "volumic_conditions");
    cs_xpath_add_element_num(&path, "zone", i);
    cs_xpath_add_attribute(&path, "scalar_source_term");
    status = cs_gui_get_attribute_value(path);
    BFT_FREE(path);

    if (cs_gui_strcmp(status, "on")) {
      zone_id = _volumic_zone_id(i);
      cells_list = _get_cells_list(zone_id, n_cells_ext, &cells);

      path = cs_xpath_init_path();
      cs_xpath_add_elements(&path, 3,
                            "thermophysical_models",
                            "source_terms",
                            "scalar_formula");
      cs_xpath_add_test_attribute(&path, "name", f->name);
      cs_xpath_add_test_attribute(&path, "zone_id", zone_id);
      cs_xpath_add_function_text(&path);
      formula = cs_gui_get_text_value(path);
      BFT_FREE(path);

      if (formula != NULL) {
        if (*idarcy == 0) {
          ev_formula = mei_tree_new(formula);
          mei_tree_insert(ev_formula,"x",0.);
          mei_tree_insert(ev_formula,"y",0.);
          mei_tree_insert(ev_formula,"z",0.);
          mei_tree_insert(ev_formula, f->name, 0.0);
          /* try to build the interpreter */
          if (mei_tree_builder(ev_formula))
            bft_error(__FILE__, __LINE__, 0,
                      _("Error: can not interpret expression: %s\n %i"),
                      ev_formula->string, mei_tree_builder(ev_formula));

          const char *symbols[] = {"S","dS"};
          if (mei_tree_find_symbols(ev_formula, 2, symbols))
            bft_error(__FILE__, __LINE__, 0,
                      _("Error: can not find the required symbol: %s\n"), "S or dS");

          for (icel = 0; icel < cells; icel++) {
            iel = cells_list[icel];
            mei_tree_insert(ev_formula, "x", cell_cen[iel][0]);
            mei_tree_insert(ev_formula, "y", cell_cen[iel][1]);
            mei_tree_insert(ev_formula, "z", cell_cen[iel][2]);
            mei_tree_insert(ev_formula, f->name, pvar[iel]);
            mei_evaluate(ev_formula);
            dS = mei_tree_lookup(ev_formula,"dS");
            tsimp[iel] = cell_f_vol[iel]*dS;
            tsexp[iel] = mei_tree_lookup(ev_formula,"S") - dS*pvar[iel];
            tsexp[iel] *= cell_f_vol[iel];
          }
        }
        else {  // groundwater flow
          ev_formula = mei_tree_new(formula);
          mei_tree_insert(ev_formula,"x",0.);
          mei_tree_insert(ev_formula,"y",0.);
          mei_tree_insert(ev_formula,"z",0.);
          mei_tree_insert(ev_formula,"t",0.);
          /* try to build the interpreter */
          if (mei_tree_builder(ev_formula))
            bft_error(__FILE__, __LINE__, 0,
                      _("Error: can not interpret expression: %s\n %i"),
                      ev_formula->string, mei_tree_builder(ev_formula));

          const char *symbols[] = {"Q","lambda"};
          if (mei_tree_find_symbols(ev_formula, 2, symbols))
            bft_error(__FILE__, __LINE__, 0,
                      _("Error: can not find the required symbol: %s\n"), "Q or lambda");

          for (icel = 0; icel < cells; icel++) {
            iel = cells_list[icel];
            mei_tree_insert(ev_formula, "x", cell_cen[iel][0]);
            mei_tree_insert(ev_formula, "y", cell_cen[iel][1]);
            mei_tree_insert(ev_formula, "z", cell_cen[iel][2]);
            mei_tree_insert(ev_formula, "t", cs_glob_time_step->t_cur);
            mei_evaluate(ev_formula);
            dS = mei_tree_lookup(ev_formula,"lambda");
            tsimp[iel] = cell_f_vol[iel]*dS;
            tsexp[iel] = mei_tree_lookup(ev_formula,"Q");
            tsexp[iel] *= cell_f_vol[iel];
          }
        }
        mei_tree_destroy(ev_formula);
      }
      BFT_FREE(cells_list);
      BFT_FREE(zone_id);
    }
    BFT_FREE(status);
  }
}

/*----------------------------------------------------------------------------
 * Thermal scalar source terms.
 *
 * Fortran Interface:
 *
 * subroutine uitsth (f_id, pvar, tsexp, tsimp)
 * *****************
 *
 * integer          f_id     <--  field id
 * double precision pvar     <--  scalar
 * double precision tsexp    -->  explicit source terms
 * double precision tsimp    -->  implicit source terms
 *----------------------------------------------------------------------------*/

void CS_PROCF(uitsth, UITSTH)(const int                  *f_id,
                              const cs_real_t   *restrict pvar,
                              cs_real_t         *restrict tsexp,
                              cs_real_t         *restrict tsimp)
{
  const cs_lnum_t n_cells_ext = cs_glob_mesh->n_cells_with_ghosts;

  const cs_real_t *restrict cell_f_vol = cs_glob_mesh_quantities->cell_f_vol;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)cs_glob_mesh_quantities->cell_cen;

  cs_lnum_t i, icel, iel;
  int zones = 0;
  cs_lnum_t cells = 0;
  cs_lnum_t *cells_list = NULL;
  double dS;
  char *path = NULL;
  char *status = NULL;
  char *zone_id = NULL;
  char *formula = NULL;

  mei_tree_t *ev_formula  = NULL;
  cs_field_t  *f = cs_field_by_id(*f_id);

  /* number of volumic zone */

  zones = cs_gui_get_tag_count("/solution_domain/volumic_conditions/zone\n", 1);

#if _XML_DEBUG_
  bft_printf("==>UITSSC\n");
#endif

  for (i=1; i < zones+1; i++) {

    /* species source term */
    path = cs_xpath_init_path();
    cs_xpath_add_elements(&path, 2, "solution_domain", "volumic_conditions");
    cs_xpath_add_element_num(&path, "zone", i);
    cs_xpath_add_attribute(&path, "thermal_source_term");
    status = cs_gui_get_attribute_value(path);
    BFT_FREE(path);

    if (cs_gui_strcmp(status, "on")) {
      zone_id = _volumic_zone_id(i);
      cells_list = _get_cells_list(zone_id, n_cells_ext, &cells);

      path = cs_xpath_init_path();
      cs_xpath_add_elements(&path, 3,
                            "thermophysical_models",
                            "source_terms",
                            "thermal_formula");
      cs_xpath_add_test_attribute(&path, "name", f->name);
      cs_xpath_add_test_attribute(&path, "zone_id", zone_id);
      cs_xpath_add_function_text(&path);
      formula = cs_gui_get_text_value(path);
      BFT_FREE(path);

      if (formula != NULL) {
        ev_formula = mei_tree_new(formula);
        mei_tree_insert(ev_formula,"x",0.);
        mei_tree_insert(ev_formula,"y",0.);
        mei_tree_insert(ev_formula,"z",0.);
        mei_tree_insert(ev_formula, f->name, 0.0);
        /* try to build the interpreter */
        if (mei_tree_builder(ev_formula))
          bft_error(__FILE__, __LINE__, 0,
                    _("Error: can not interpret expression: %s\n %i"),
                    ev_formula->string, mei_tree_builder(ev_formula));

        const char *symbols[] = {"S","dS"};
        if (mei_tree_find_symbols(ev_formula, 2, symbols))
          bft_error(__FILE__, __LINE__, 0,
                    _("Error: can not find the required symbol: %s\n"), "S or dS");

        for (icel = 0; icel < cells; icel++) {
          iel = cells_list[icel];
          mei_tree_insert(ev_formula, "x", cell_cen[iel][0]);
          mei_tree_insert(ev_formula, "y", cell_cen[iel][1]);
          mei_tree_insert(ev_formula, "z", cell_cen[iel][2]);
          mei_tree_insert(ev_formula, f->name, pvar[iel]);
          mei_evaluate(ev_formula);
          dS = mei_tree_lookup(ev_formula,"dS");
          tsimp[iel] = cell_f_vol[iel]*dS;
          tsexp[iel] = mei_tree_lookup(ev_formula,"S") - dS*pvar[iel];
          tsexp[iel] *= cell_f_vol[iel];
        }
        mei_tree_destroy(ev_formula);
      }
      BFT_FREE(cells_list);
      BFT_FREE(zone_id);
    }
    BFT_FREE(status);
  }
}

/*----------------------------------------------------------------------------
 * Variables and user scalars initialization.
 *
 * Fortran Interface:
 *
 * subroutine uiiniv
 * *****************
 *
 * integer          isuite   <--  restart indicator
 * integer          idarcy   <--  groundwater module activation
 * integer          iccfth   -->  type of initialization (compressible model)
 *----------------------------------------------------------------------------*/

void CS_PROCF(uiiniv, UIINIV)(const int          *isuite,
                              const int          *idarcy,
                              int                *iccfth)
{
  /* Coal combustion: the initialization of the model scalar are not given */
  const cs_lnum_t n_cells_ext = cs_glob_mesh->n_cells_with_ghosts;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)cs_glob_mesh_quantities->cell_cen;

  cs_lnum_t icel, iel;
  int zones             = 0;
  cs_lnum_t cells       = 0;
  int ccfth             = 0;
  cs_lnum_t *cells_list = NULL;
  char *path            = NULL;
  char *path1           = NULL;
  char *status          = NULL;
  char *zone_id         = NULL;

  cs_var_t  *vars = cs_glob_var;

  /* number of volumic zone */

  zones
    = cs_gui_get_tag_count("/solution_domain/volumic_conditions/zone", 1);

#if _XML_DEBUG_
  bft_printf("==>UIINIV\n");
#endif

  for (int i = 1; i < zones + 1; i++) {

    path = cs_xpath_init_path();
    cs_xpath_add_elements(&path, 2, "solution_domain", "volumic_conditions");
    cs_xpath_add_element_num(&path, "zone", i);
    cs_xpath_add_attribute(&path, "initialization");
    status = cs_gui_get_attribute_value(path);
    BFT_FREE(path);

    if (cs_gui_strcmp(status, "on")) {

      zone_id = _volumic_zone_id(i);
      cells_list = _get_cells_list(zone_id, n_cells_ext, &cells);

      if (*isuite == 0) {
        char *path_velocity = cs_xpath_init_path();
        cs_xpath_add_elements(&path_velocity, 4,
            "thermophysical_models",
            "velocity_pressure",
            "initialization",
            "formula");
        cs_xpath_add_test_attribute(&path_velocity, "zone_id", zone_id);
        cs_xpath_add_function_text(&path_velocity);
        char *formula_uvw = cs_gui_get_text_value(path_velocity);

        cs_field_t *c_vel = cs_field_by_name("velocity");

        if (formula_uvw != NULL) {
          mei_tree_t *ev_formula_uvw = mei_tree_new(formula_uvw);
          mei_tree_insert(ev_formula_uvw, "x", 0.);
          mei_tree_insert(ev_formula_uvw, "y", 0.);
          mei_tree_insert(ev_formula_uvw, "z", 0.);
          /* try to build the interpreter */
          if (mei_tree_builder(ev_formula_uvw))
            bft_error(__FILE__, __LINE__, 0,
                      _("Error: can not interpret expression: %s\n %i"),
                      ev_formula_uvw->string, mei_tree_builder(ev_formula_uvw));

          const char *symbols_uvw[] = {"velocity[0]", "velocity[1]", "velocity[2]"};
          if (mei_tree_find_symbols(ev_formula_uvw, 3, symbols_uvw))
            bft_error(__FILE__, __LINE__, 0,
                      _("Error: can not find the required symbol: %s\n"),
                      "velocity[0], velocity[1] ou velocity[2]");

          for (icel = 0; icel < cells; icel++) {
            iel = cells_list[icel];
            mei_tree_insert(ev_formula_uvw, "x", cell_cen[iel][0]);
            mei_tree_insert(ev_formula_uvw, "y", cell_cen[iel][1]);
            mei_tree_insert(ev_formula_uvw, "z", cell_cen[iel][2]);
            mei_evaluate(ev_formula_uvw);
            c_vel->val[3 * iel    ] = mei_tree_lookup(ev_formula_uvw, "velocity[0]");
            c_vel->val[3 * iel + 1] = mei_tree_lookup(ev_formula_uvw, "velocity[1]");
            c_vel->val[3 * iel + 2] = mei_tree_lookup(ev_formula_uvw, "velocity[2]");
          }
          mei_tree_destroy(ev_formula_uvw);
        }
        else {
          for (icel = 0; icel < cells; icel++) {
            iel = cells_list[icel];
            for (cs_lnum_t j = 0; j < 3; j++)
              c_vel->val[3 * iel + j] = 0.0;
          }
        }
        BFT_FREE(formula_uvw);
        BFT_FREE(path_velocity);

        /* pressure initialization for groundwater model */
        if (*idarcy > 0) {
          char *formula        = NULL;
          mei_tree_t *ev_formula       = NULL;
          path = cs_xpath_short_path();
          cs_xpath_add_element(&path, "variable");
          cs_xpath_add_test_attribute(&path, "name", "pressure");
          cs_xpath_add_element(&path, "formula");
          cs_xpath_add_test_attribute(&path, "zone_id", zone_id);
          BFT_MALLOC(path1, strlen(path) +1, char);

          cs_field_t *c = cs_field_by_name_try("pressure");

          cs_xpath_add_function_text(&path);
          formula = cs_gui_get_text_value(path);
          if (formula != NULL) {
            ev_formula = _init_mei_tree(formula, "H");
            for (icel = 0; icel < cells; icel++) {
              iel = cells_list[icel];
              mei_tree_insert(ev_formula, "x", cell_cen[iel][0]);
              mei_tree_insert(ev_formula, "y", cell_cen[iel][1]);
              mei_tree_insert(ev_formula, "z", cell_cen[iel][2]);
              mei_evaluate(ev_formula);
              c->val[iel] = mei_tree_lookup(ev_formula, "H");
            }
            mei_tree_destroy(ev_formula);
          }
          BFT_FREE(formula);
          BFT_FREE(path);
        }

        /* Turbulence variables initialization */
        char *choice = _turbulence_initialization_choice(zone_id);

        if (cs_gui_strcmp(choice, "formula")) {
          char *path_turb = cs_xpath_init_path();
          cs_xpath_add_elements(&path_turb, 3,
                                "thermophysical_models",
                                "turbulence",
                                "initialization");
          cs_xpath_add_test_attribute(&path_turb, "zone_id", zone_id);
          cs_xpath_add_element(&path_turb, "formula");
          cs_xpath_add_function_text(&path_turb);
          char *formula_turb = cs_gui_get_text_value(path_turb);
          BFT_FREE(path_turb);

          if (formula_turb != NULL) {
            mei_tree_t *ev_formula_turb = mei_tree_new(formula_turb);
            mei_tree_insert(ev_formula_turb, "rho0", cs_glob_fluid_properties->ro0);
            mei_tree_insert(ev_formula_turb, "mu0", cs_glob_fluid_properties->viscl0);
            mei_tree_insert(ev_formula_turb, "cp0", cs_glob_fluid_properties->cp0);
            mei_tree_insert(ev_formula_turb, "uref", cs_glob_turb_ref_values->uref);
            mei_tree_insert(ev_formula_turb, "almax", cs_glob_turb_ref_values->almax);
            mei_tree_insert(ev_formula_turb, "x", 0.0);
            mei_tree_insert(ev_formula_turb, "y", 0.0);
            mei_tree_insert(ev_formula_turb, "z", 0.0);

            /* try to build the interpreter */

            if (mei_tree_builder(ev_formula_turb))
              bft_error(__FILE__, __LINE__, 0,
                        _("Error: can not interpret expression: %s\n %i"),
                        ev_formula_turb->string, mei_tree_builder(ev_formula_turb));

            char *model = cs_gui_get_thermophysical_model("turbulence");
            if (model == NULL)
              break;

            if (cs_gui_strcmp(model, "k-epsilon") ||
                cs_gui_strcmp(model, "k-epsilon-PL")) {

              const char *symbols[] = {"k","epsilon"};
              if (mei_tree_find_symbols(ev_formula_turb, 2, symbols))
                bft_error(__FILE__, __LINE__, 0,
                          _("Error: can not find the required symbol: %s\n"),
                          "k or epsilon");

              cs_field_t *c_k   = cs_field_by_name("k");
              cs_field_t *c_eps = cs_field_by_name("epsilon");

              for (icel = 0; icel < cells; icel++) {
                iel = cells_list[icel];
                mei_tree_insert(ev_formula_turb, "x", cell_cen[iel][0]);
                mei_tree_insert(ev_formula_turb, "y", cell_cen[iel][1]);
                mei_tree_insert(ev_formula_turb, "z", cell_cen[iel][2]);
                mei_evaluate(ev_formula_turb);
                c_k->val[iel]   = mei_tree_lookup(ev_formula_turb, "k");
                c_eps->val[iel] = mei_tree_lookup(ev_formula_turb, "epsilon");
              }
            }

            else if (cs_gui_strcmp(model, "Rij-epsilon") || cs_gui_strcmp(model, "Rij-SSG")) {
              const char *symbols[] = {"r11", "r22", "r33", "r12", "r13", "r23", "epsilon"};
              if (mei_tree_find_symbols(ev_formula_turb, 7, symbols))
                bft_error(__FILE__, __LINE__, 0,
                          _("Error: can not find the required symbol: %s\n"),
                          "r11, r22, r33, r12, r13, r23 or epsilon");

              cs_field_t *c_r11 = cs_field_by_name("r11");
              cs_field_t *c_r22 = cs_field_by_name("r22");
              cs_field_t *c_r33 = cs_field_by_name("r33");
              cs_field_t *c_r12 = cs_field_by_name("r12");
              cs_field_t *c_r13 = cs_field_by_name("r13");
              cs_field_t *c_r23 = cs_field_by_name("r23");
              cs_field_t *c_eps = cs_field_by_name("epsilon");

              for (icel = 0; icel < cells; icel++) {
                iel = cells_list[icel];
                mei_tree_insert(ev_formula_turb, "x", cell_cen[iel][0]);
                mei_tree_insert(ev_formula_turb, "y", cell_cen[iel][1]);
                mei_tree_insert(ev_formula_turb, "z", cell_cen[iel][2]);
                mei_evaluate(ev_formula_turb);
                c_r11->val[iel] = mei_tree_lookup(ev_formula_turb, "r11");
                c_r22->val[iel] = mei_tree_lookup(ev_formula_turb, "r22");
                c_r33->val[iel] = mei_tree_lookup(ev_formula_turb, "r33");
                c_r12->val[iel] = mei_tree_lookup(ev_formula_turb, "r12");
                c_r13->val[iel] = mei_tree_lookup(ev_formula_turb, "r13");
                c_r23->val[iel] = mei_tree_lookup(ev_formula_turb, "r23");
                c_eps->val[iel] = mei_tree_lookup(ev_formula_turb, "epsilon");
              }
            }

            else if (cs_gui_strcmp(model, "Rij-EBRSM")) {
              const char *symbols[] = {"r11", "r22", "r33", "r12", "r13", "r23",
                                       "epsilon", "alpha"};
              if (mei_tree_find_symbols(ev_formula_turb, 8, symbols))
                bft_error(__FILE__, __LINE__, 0,
                          _("Error: can not find the required symbol: %s\n"),
                          "r11, r22, r33, r12, r13, r23, epsilon or alpha");

              cs_field_t *c_r11 = cs_field_by_name("r11");
              cs_field_t *c_r22 = cs_field_by_name("r22");
              cs_field_t *c_r33 = cs_field_by_name("r33");
              cs_field_t *c_r12 = cs_field_by_name("r12");
              cs_field_t *c_r13 = cs_field_by_name("r13");
              cs_field_t *c_r23 = cs_field_by_name("r23");
              cs_field_t *c_eps = cs_field_by_name("epsilon");
              cs_field_t *c_alp = cs_field_by_name("alpha");

              for (icel = 0; icel < cells; icel++) {
                iel = cells_list[icel];
                mei_tree_insert(ev_formula_turb, "x", cell_cen[iel][0]);
                mei_tree_insert(ev_formula_turb, "y", cell_cen[iel][1]);
                mei_tree_insert(ev_formula_turb, "z", cell_cen[iel][2]);
                mei_evaluate(ev_formula_turb);
                c_r11->val[iel] = mei_tree_lookup(ev_formula_turb, "r11");
                c_r22->val[iel] = mei_tree_lookup(ev_formula_turb, "r22");
                c_r33->val[iel] = mei_tree_lookup(ev_formula_turb, "r33");
                c_r12->val[iel] = mei_tree_lookup(ev_formula_turb, "r12");
                c_r13->val[iel] = mei_tree_lookup(ev_formula_turb, "r13");
                c_r23->val[iel] = mei_tree_lookup(ev_formula_turb, "r23");
                c_eps->val[iel] = mei_tree_lookup(ev_formula_turb, "epsilon");
                c_alp->val[iel] = mei_tree_lookup(ev_formula_turb, "alpha");
              }
            }

            else if (cs_gui_strcmp(model, "v2f-BL-v2/k")) {
              const char *symbols[] = {"k", "epsilon", "phi", "alpha"};
              if (mei_tree_find_symbols(ev_formula_turb, 4, symbols))
                bft_error(__FILE__, __LINE__, 0,
                          _("Error: can not find the required symbol: %s\n"),
                          "k, epsilon, phi of al");

              cs_field_t *c_k   = cs_field_by_name("k");
              cs_field_t *c_eps = cs_field_by_name("epsilon");
              cs_field_t *c_phi = cs_field_by_name("phi");
              cs_field_t *c_alp = cs_field_by_name("alpha");

              for (icel = 0; icel < cells; icel++) {
                iel = cells_list[icel];
                mei_tree_insert(ev_formula_turb, "x", cell_cen[iel][0]);
                mei_tree_insert(ev_formula_turb, "y", cell_cen[iel][1]);
                mei_tree_insert(ev_formula_turb, "z", cell_cen[iel][2]);
                mei_evaluate(ev_formula_turb);
                c_k->val[iel]   = mei_tree_lookup(ev_formula_turb, "k");
                c_eps->val[iel] = mei_tree_lookup(ev_formula_turb, "epsilon");
                c_phi->val[iel] = mei_tree_lookup(ev_formula_turb, "phi");
                c_alp->val[iel] = mei_tree_lookup(ev_formula_turb, "alpha");
              }
            }

            else if (cs_gui_strcmp(model, "k-omega-SST")) {
              const char *symbols[] = {"k", "omega"};
              if (mei_tree_find_symbols(ev_formula_turb, 2, symbols))
                bft_error(__FILE__, __LINE__, 0,
                          _("Error: can not find the required symbol: %s\n"),
                          "k or omega");

              cs_field_t *c_k   = cs_field_by_name("k");
              cs_field_t *c_ome = cs_field_by_name("omega");

              for (icel = 0; icel < cells; icel++) {
                iel = cells_list[icel];
                mei_tree_insert(ev_formula_turb, "x", cell_cen[iel][0]);
                mei_tree_insert(ev_formula_turb, "y", cell_cen[iel][1]);
                mei_tree_insert(ev_formula_turb, "z", cell_cen[iel][2]);
                mei_evaluate(ev_formula_turb);
                c_k->val[iel]   = mei_tree_lookup(ev_formula_turb, "k");
                c_ome->val[iel] = mei_tree_lookup(ev_formula_turb, "omega");
              }
            }

            else if (cs_gui_strcmp(model, "Spalart-Allmaras")) {
              const char *symbols[] = {"nu_tilda"};
              if (mei_tree_find_symbols(ev_formula_turb, 1, symbols))
                bft_error(__FILE__, __LINE__, 0,
                          _("Error: can not find the required symbol: %s\n"),
                          "nu_tilda");

              cs_field_t *c_nu = cs_field_by_name("nu_tilda");

              for (icel = 0; icel < cells; icel++) {
                iel = cells_list[icel];
                mei_tree_insert(ev_formula_turb, "x", cell_cen[iel][0]);
                mei_tree_insert(ev_formula_turb, "y", cell_cen[iel][1]);
                mei_tree_insert(ev_formula_turb, "z", cell_cen[iel][2]);
                mei_evaluate(ev_formula_turb);
                c_nu->val[iel] = mei_tree_lookup(ev_formula_turb, "nu_tilda");
              }
            }

            else
              bft_error(__FILE__, __LINE__, 0,
                        _("Invalid turbulence model: %s.\n"), model);
            mei_tree_destroy(ev_formula_turb);
            BFT_FREE(model);
          }
          BFT_FREE(formula_turb);
        }
        BFT_FREE(choice);
      }

      /* Thermal scalar initialization */
      if (cs_gui_thermal_model()) {
        char *path_sca       = NULL;
        char *formula_sca    = NULL;
        mei_tree_t *ev_formula_sca   = NULL;
        path_sca = cs_xpath_init_path();
        cs_xpath_add_elements(&path_sca, 3,
                              "thermophysical_models",
                              "thermal_scalar",
                              "variable");
        cs_xpath_add_element(&path_sca, "formula");
        cs_xpath_add_test_attribute(&path_sca, "zone_id", zone_id);
        cs_xpath_add_function_text(&path_sca);
        formula_sca = cs_gui_get_text_value(path_sca);
        BFT_FREE(path_sca);

        const int itherm = cs_glob_thermal_model->itherm;

        /* For non-specific physics defined with the GUI,
           itherm can only be 1 or 2 here (as the thermal model is on) */

        cs_field_t *c;
        switch (itherm) {
        case 1:
          c = CS_F_(t);
          break;
        case 2:
          c = CS_F_(h);
        break;
        case 3:
          c = CS_F_(energy);
          break;
        default:
          assert(0);
          c = NULL;
        }

        if (formula_sca != NULL) {
          ev_formula_sca = mei_tree_new(formula_sca);
          mei_tree_insert(ev_formula_sca, "x", 0.);
          mei_tree_insert(ev_formula_sca, "y", 0.);
          mei_tree_insert(ev_formula_sca, "z", 0.);
          /* try to build the interpreter */
          if (mei_tree_builder(ev_formula_sca))
            bft_error(__FILE__, __LINE__, 0,
                      _("Error: can not interpret expression: %s\n %i"),
                      ev_formula_sca->string, mei_tree_builder(ev_formula_sca));

          if (mei_tree_find_symbol(ev_formula_sca, c->name))
            bft_error(__FILE__, __LINE__, 0,
                      _("Error: can not find the required symbol: %s\n"),
                      c->name);

          if (*isuite == 0) {
            for (icel = 0; icel < cells; icel++) {
              iel = cells_list[icel];
              mei_tree_insert(ev_formula_sca, "x", cell_cen[iel][0]);
              mei_tree_insert(ev_formula_sca, "y", cell_cen[iel][1]);
              mei_tree_insert(ev_formula_sca, "z", cell_cen[iel][2]);
              mei_evaluate(ev_formula_sca);
              c->val[iel] = mei_tree_lookup(ev_formula_sca, c->name);
            }
          }
          mei_tree_destroy(ev_formula_sca);
        } else {
          if (*isuite == 0) {
            for (icel = 0; icel < cells; icel++) {
              iel = cells_list[icel];
              c->val[iel] = 0.0;
            }
          }
        }
        BFT_FREE(formula_sca);
      }

      /* User Scalars initialization */
      int n_fields = cs_field_n_fields();
      for (int f_id = 0; f_id < n_fields; f_id++) {
        const cs_field_t  *f = cs_field_by_id(f_id);
        if (f->type & CS_FIELD_USER && f->location_id == CS_MESH_LOCATION_CELLS) {
          char *path_sca       = NULL;
          char *formula_sca    = NULL;
          mei_tree_t *ev_formula_sca   = NULL;

          path_sca = cs_xpath_init_path();
          cs_xpath_add_elements(&path_sca, 2,
                                "additional_scalars",
                                "variable");
          cs_xpath_add_test_attribute(&path_sca, "name", f->name);
          cs_xpath_add_element(&path_sca, "formula");
          cs_xpath_add_test_attribute(&path_sca, "zone_id", zone_id);
          cs_xpath_add_function_text(&path_sca);
          formula_sca = cs_gui_get_text_value(path_sca);
          BFT_FREE(path_sca);

          if (formula_sca != NULL) {
            ev_formula_sca = mei_tree_new(formula_sca);
            mei_tree_insert(ev_formula_sca, "x", 0.);
            mei_tree_insert(ev_formula_sca, "y", 0.);
            mei_tree_insert(ev_formula_sca, "z", 0.);
            /* try to build the interpreter */
            if (mei_tree_builder(ev_formula_sca))
              bft_error(__FILE__, __LINE__, 0,
                        _("Error: can not interpret expression: %s\n %i"),
                        ev_formula_sca->string, mei_tree_builder(ev_formula_sca));

            if (mei_tree_find_symbol(ev_formula_sca, f->name))
                bft_error(__FILE__, __LINE__, 0,
                        _("Error: can not find the required symbol: %s\n"),
                        f->name);

            if (*isuite == 0) {
              for (icel = 0; icel < cells; icel++) {
                iel = cells_list[icel];
                mei_tree_insert(ev_formula_sca, "x", cell_cen[iel][0]);
                mei_tree_insert(ev_formula_sca, "y", cell_cen[iel][1]);
                mei_tree_insert(ev_formula_sca, "z", cell_cen[iel][2]);
                mei_evaluate(ev_formula_sca);
                f->val[iel] = mei_tree_lookup(ev_formula_sca, f->name);
              }
            }
            mei_tree_destroy(ev_formula_sca);
          }
          BFT_FREE(formula_sca);
        }
      }
      /* Meteo Scalars initialization */
      if (cs_gui_strcmp(vars->model, "atmospheric_flows")) {
        int    size;
        char *name       = NULL;
        char *path_meteo = NULL;
        char *formula_meteo  = NULL;
        mei_tree_t *ev_formula_meteo = NULL;

        size = cs_gui_get_tag_count("/thermophysical_models/atmospheric_flows/variable\n", 1);

        for (int j = 0; j < size; j++)
        {
          path_meteo = cs_xpath_init_path();
          cs_xpath_add_elements(&path_meteo, 2,
                                 "thermophysical_models",
                                 "atmospheric_flows");
          cs_xpath_add_element_num(&path_meteo, "variable", j + 1);
          cs_xpath_add_attribute(&path_meteo, "name");
          name = cs_gui_get_attribute_value(path_meteo);

          cs_field_t *c = cs_field_by_name_try(name);
          BFT_FREE(path_meteo);

          path_meteo = cs_xpath_init_path();
          cs_xpath_add_elements(&path_meteo, 2,
                                 "thermophysical_models",
                                 "atmospheric_flows");
          cs_xpath_add_element_num(&path_meteo, "variable", j + 1);
          cs_xpath_add_test_attribute(&path_meteo, "zone_id", zone_id);
          cs_xpath_add_attribute(&path_meteo, "formula");
          formula_meteo = cs_gui_get_attribute_value(path_meteo);
          BFT_FREE(path_meteo);

          if (formula_meteo != NULL) {
            ev_formula_meteo = mei_tree_new(formula_meteo);
            mei_tree_insert(ev_formula_meteo, "x", 0.);
            mei_tree_insert(ev_formula_meteo, "y", 0.);
            mei_tree_insert(ev_formula_meteo, "z", 0.);
            /* try to build the interpreter */
            if (mei_tree_builder(ev_formula_meteo))
              bft_error(__FILE__, __LINE__, 0,
                        _("Error: can not interpret expression: %s\n %i"),
                        ev_formula_meteo->string, mei_tree_builder(ev_formula_meteo));

            if (mei_tree_find_symbol(ev_formula_meteo, name))
              bft_error(__FILE__, __LINE__, 0,
                        _("Error: can not find the required symbol: %s\n"),
                        name);

            if (*isuite == 0) {
              for (icel = 0; icel < cells; icel++) {
                iel = cells_list[icel];
                mei_tree_insert(ev_formula_meteo, "x", cell_cen[iel][0]);
                mei_tree_insert(ev_formula_meteo, "y", cell_cen[iel][1]);
                mei_tree_insert(ev_formula_meteo, "z", cell_cen[iel][2]);
                mei_evaluate(ev_formula_meteo);
                c->val[iel] = mei_tree_lookup(ev_formula_meteo, name);
              }
            }
            mei_tree_destroy(ev_formula_meteo);
          }
          else {
            if (*isuite == 0) {
              for (icel = 0; icel < cells; icel++) {
                iel = cells_list[icel];
                c->val[iel] = 0.0;
              }
            }
          }
          BFT_FREE(formula_meteo);
          BFT_FREE(name);
        }
      }

      if (cs_gui_strcmp(vars->model, "compressible_model")) {
        char *formula        = NULL;
        char *buff           = NULL;
        mei_tree_t *ev_formula       = NULL;
        const char *name[] = {"pressure", "temperature", "total_energy",
                              "density"};

        ccfth = 10000;
        for (int j = 0; j < 4; j++) {
          path = cs_xpath_short_path();
          if (j < 3)
            cs_xpath_add_element(&path, "variable");
          else
            cs_xpath_add_element(&path, "property");
          cs_xpath_add_test_attribute(&path, "name", name[j]);
          cs_xpath_add_element(&path, "formula");
          cs_xpath_add_test_attribute(&path, "zone_id", zone_id);
          BFT_MALLOC(path1, strlen(path) +1, char);
          strcpy(path1, path);
          cs_xpath_add_attribute(&path, "status");
          buff = cs_gui_get_attribute_value(path);

          if (cs_gui_strcmp(buff, "on")) {
            if (j == 0)
              ccfth = ccfth * 2;
            else if (j == 1)
              ccfth = ccfth * 5;
            else if (j == 2)
              ccfth = ccfth * 7;
            else if (j == 3)
              ccfth = ccfth * 3;

            cs_field_t *c = cs_field_by_name_try(name[j]);

            cs_xpath_add_function_text(&path1);
            formula = cs_gui_get_text_value(path1);
            ev_formula = _init_mei_tree(formula, name[j]);
            if (*isuite == 0) {
              for (icel = 0; icel < cells; icel++) {
                iel = cells_list[icel];
                mei_tree_insert(ev_formula, "x", cell_cen[iel][0]);
                mei_tree_insert(ev_formula, "y", cell_cen[iel][1]);
                mei_tree_insert(ev_formula, "z", cell_cen[iel][2]);
                mei_evaluate(ev_formula);
                c->val[iel] = mei_tree_lookup(ev_formula, name[j]);
              }
            }
            mei_tree_destroy(ev_formula);
          }
          BFT_FREE(buff);
          BFT_FREE(formula);
          BFT_FREE(path);
          BFT_FREE(path1);
        }
        *iccfth = ccfth;
      }
      BFT_FREE(cells_list);

#if _XML_DEBUG_
      bft_printf("--zone zone_id: %s\n", zone_id);
      bft_printf("--zone's element number: %i\n", cells);

      if (*isuite == 0) {
        double initial_value;
        for (int f_id = 0; f_id < cs_field_n_fields(); f_id++) {
          const cs_field_t *f = cs_field_by_id(f_id);
          _variable_initial_value(f->name, zone_id, &initial_value);
          bft_printf("--initial value for %s: %f\n", f->name, initial_value);
        }
      }
#endif
      BFT_FREE(cells_list);
      BFT_FREE(zone_id);
    }
    BFT_FREE(status);
  } /* zones+1 */
}

/*----------------------------------------------------------------------------
 * Head losses definition
 *
 * Fortran Interface:
 *
 * subroutine uikpdc
 * *****************
 *
 * integer          iappel   <--  number of calls during a time step
 * integer          ncepdp  -->   number of cells with head losses
 * integer          icepdc  -->   ncepdp cells number with head losses
 * double precision ckupdc  -->   head losses matrix
 *----------------------------------------------------------------------------*/

void CS_PROCF(uikpdc, UIKPDC)(const int*   iappel,
                                    int    ncepdp[],
                                    int    icepdc[],
                                    double ckupdc[])
{
  const cs_lnum_t n_cells_ext = cs_glob_mesh->n_cells_with_ghosts;
  cs_lnum_t i, j, iel, ielpdc, ikpdc;
  int zones = 0;
  cs_lnum_t cells = 0;
  cs_lnum_t *cells_list = NULL;
  double vit;
  double a11, a12, a13, a21, a22, a23, a31, a32, a33;
  double c11, c12, c13, c21, c22, c23, c31, c32, c33;
  double k11, k22, k33;
  char *zone_id = NULL;
  char *status = NULL;
  char *path = NULL;

  /* number of volumic zone */

  zones
    = cs_gui_get_tag_count("/solution_domain/volumic_conditions/zone", 1);


  if (*iappel == 1 || *iappel == 2)
  {
    ielpdc = 0;

    for (i=1; i < zones+1; i++)
    {
      path = cs_xpath_init_path();
      cs_xpath_add_elements(&path, 2, "solution_domain", "volumic_conditions");
      cs_xpath_add_element_num(&path, "zone", i);
      cs_xpath_add_attribute(&path, "head_losses");
      status = cs_gui_get_attribute_value(path);
      BFT_FREE(path);

      if (cs_gui_strcmp(status, "on"))
      {
        zone_id = _volumic_zone_id(i);
        cells_list = _get_cells_list(zone_id, n_cells_ext, &cells);

        for (j=0; j < cells; j++)
        {
          if (*iappel == 2)
            icepdc[ielpdc] = cells_list[j] + 1;
          ielpdc++;
        }
        BFT_FREE(cells_list);
        BFT_FREE(zone_id);
      }
      BFT_FREE(status);
    } /* zones+1 */
    if (*iappel == 1)
      *ncepdp = ielpdc;
  }

  if (*iappel == 3) {

    for (ikpdc = 0; ikpdc < 6; ikpdc++) {
      for (ielpdc = 0; ielpdc < *ncepdp; ielpdc++)
        ckupdc[ikpdc * (*ncepdp) + ielpdc] = 0.0;
    }

    ielpdc = 0;

    cs_field_t *c_vel = cs_field_by_name("velocity");

    for (i=1; i < zones+1; i++) {

      path = cs_xpath_init_path();
      cs_xpath_add_elements(&path, 2, "solution_domain", "volumic_conditions");
      cs_xpath_add_element_num(&path, "zone", i);
      cs_xpath_add_attribute(&path, "head_losses");
      status = cs_gui_get_attribute_value(path);
      BFT_FREE(path);

      if (cs_gui_strcmp(status, "on")) {

        zone_id = _volumic_zone_id(i);
        cells_list = _get_cells_list(zone_id, n_cells_ext, &cells);

        k11 = _c_head_losses(zone_id, "kxx");
        k22 = _c_head_losses(zone_id, "kyy");
        k33 = _c_head_losses(zone_id, "kzz");

        a11 = _c_head_losses(zone_id, "a11");
        a12 = _c_head_losses(zone_id, "a12");
        a13 = _c_head_losses(zone_id, "a13");
        a21 = _c_head_losses(zone_id, "a21");
        a22 = _c_head_losses(zone_id, "a22");
        a23 = _c_head_losses(zone_id, "a23");
        a31 = _c_head_losses(zone_id, "a31");
        a32 = _c_head_losses(zone_id, "a32");
        a33 = _c_head_losses(zone_id, "a33");

        if (   cs_gui_is_equal_real(a12, 0.0)
            && cs_gui_is_equal_real(a13, 0.0)
            && cs_gui_is_equal_real(a23, 0.0)) {

          c11 = k11;
          c22 = k22;
          c33 = k33;
          c12 = 0.0;
          c13 = 0.0;
          c23 = 0.0;

        }
        else
          _matrix_base_conversion(a11, a12, a13, a21, a22, a23, a31, a32, a33,
                                  k11, 0.0, 0.0, 0.0, k22, 0.0, 0.0, 0.0, k33,
                                  &c11, &c12, &c13,
                                  &c21, &c22, &c23,
                                  &c31, &c32, &c33);

        for (j = 0; j < cells; j++) {
          iel = cells_list[j];
          vit =   c_vel->val_pre[3 * iel    ] * c_vel->val_pre[3 * iel    ]
                + c_vel->val_pre[3 * iel + 1] * c_vel->val_pre[3 * iel + 1]
                + c_vel->val_pre[3 * iel + 2] * c_vel->val_pre[3 * iel + 2];
          vit = sqrt(vit);
          ckupdc[0 * (*ncepdp) + ielpdc] = 0.5 * c11 * vit;
          ckupdc[1 * (*ncepdp) + ielpdc] = 0.5 * c22 * vit;
          ckupdc[2 * (*ncepdp) + ielpdc] = 0.5 * c33 * vit;
          ckupdc[3 * (*ncepdp) + ielpdc] = 0.5 * c12 * vit;
          ckupdc[4 * (*ncepdp) + ielpdc] = 0.5 * c23 * vit;
          ckupdc[5 * (*ncepdp) + ielpdc] = 0.5 * c13 * vit;
          ielpdc++;
        }
        BFT_FREE(cells_list);
        BFT_FREE(zone_id);
      }
      BFT_FREE(status);
    } /* zones+1 */
  }
#if _XML_DEBUG_
  bft_printf("==>uikpdc\n");
  if (*iappel == 1)
    bft_printf("--%i number of head losses cells: %i\n", *iappel, *ncepdp);
  if (*iappel == 3)
    bft_printf("--%i number of head losses cells: %i\n", *iappel, ielpdc);
#endif
}


/*----------------------------------------------------------------------------
 * User law for material properties
 *
 * Fortran Interface:
 *
 * subroutine uiphyv
 * *****************
 *
 * integer          iviscv   <--  pointer for volumic viscosity viscv
 * integer          itempk   <--  pointer for temperature (in K)
 * double precision visls0   <--  diffusion coefficient of the scalars
 * double precision viscv0   <--  volumic viscosity
 *----------------------------------------------------------------------------*/

void CS_PROCF(uiphyv, UIPHYV)(const cs_int_t  *iviscv,
                              const cs_int_t  *itempk,
                              const cs_real_t *visls0,
                              const cs_real_t *viscv0)
{
  const cs_lnum_t n_cells     = cs_glob_mesh->n_cells;
  const cs_lnum_t n_cells_ext = cs_glob_mesh->n_cells_with_ghosts;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)cs_glob_mesh_quantities->cell_cen;
  char *path = NULL;
  char *law = NULL;
  double time0;
  mei_tree_t *ev_law = NULL;
  cs_lnum_t i, iel;

  cs_var_t  *vars = cs_glob_var;
  const int iscalt = cs_glob_thermal_model->iscalt;

  /* law for density */
  if (!cs_gui_strcmp(vars->model, "compressible_model")) {
      if (cs_glob_fluid_properties->irovar == 1) {
          cs_field_t *c_rho = CS_F_(rho);
          _physical_property("density", "density",
                             n_cells, n_cells_ext, cs_glob_fluid_properties->icp,
                             cs_glob_fluid_properties->p0,
                             cs_glob_fluid_properties->ro0,
                             cs_glob_fluid_properties->cp0,
                             cs_glob_fluid_properties->viscl0, visls0,
                             c_rho->val);
      }
  }

  /* law for molecular viscosity */
  if (cs_glob_fluid_properties->ivivar == 1) {
    cs_field_t *c_mu = CS_F_(mu);
    _physical_property("molecular_viscosity", "molecular_viscosity",
                       n_cells, n_cells_ext, cs_glob_fluid_properties->icp,
                       cs_glob_fluid_properties->p0,
                       cs_glob_fluid_properties->ro0,
                       cs_glob_fluid_properties->cp0,
                       cs_glob_fluid_properties->viscl0, visls0,
                       c_mu->val);
  }

  /* law for specific heat */
  if (cs_glob_fluid_properties->icp > 0) {
    cs_field_t *c_cp = CS_F_(cp);
    _physical_property("specific_heat", "specific_heat",
                       n_cells, n_cells_ext, cs_glob_fluid_properties->icp,
                       cs_glob_fluid_properties->p0,
                       cs_glob_fluid_properties->ro0,
                       cs_glob_fluid_properties->cp0,
                       cs_glob_fluid_properties->viscl0, visls0,
                       c_cp->val);
  }

  /* law for thermal conductivity */
  if (iscalt > 0) {

    cs_field_t  *cond_dif = NULL;

    cs_field_t *_th_f[] = {CS_F_(t), CS_F_(h), CS_F_(energy)};

    for (i = 0; i < 3; i++)
      if (_th_f[i]) {
        if ((_th_f[i])->type & CS_FIELD_VARIABLE) {
          int k = cs_field_key_id("scalar_diffusivity_id");
          int cond_diff_id = cs_field_get_key_int(_th_f[i], k);
          if (cond_diff_id > -1) {
            cond_dif = cs_field_by_id(cond_diff_id);
            _physical_property("thermal_conductivity", "thermal_conductivity",
                               n_cells, n_cells_ext, cs_glob_fluid_properties->icp,
                               cs_glob_fluid_properties->p0,
                               cs_glob_fluid_properties->ro0,
                               cs_glob_fluid_properties->cp0,
                               cs_glob_fluid_properties->viscl0, visls0,
                               cond_dif->val);
          }
          break;
        }
      }
    }

  /* law for volumic viscosity (compressible model) */
  if (cs_gui_strcmp(vars->model, "compressible_model")) {
    if (*iviscv > 0) {
      cs_field_t *c = cs_field_by_name_try("volume_viscosity");
      _compressible_physical_property("volume_viscosity",
                                      "volume_viscosity", c->id,
                                      n_cells,
                                      itempk,
                                      cs_glob_fluid_properties->p0,
                                      cs_glob_fluid_properties->t0,
                                      cs_glob_fluid_properties->ro0,
                                      visls0, viscv0);
    }
  }

  /* law for scalar diffusivity */
  int user_id = -1;
  int n_fields = cs_field_n_fields();
  const int kivisl = cs_field_key_id("scalar_diffusivity_id");
  const int kscavr = cs_field_key_id("first_moment_id");

  for (int f_id = 0; f_id < n_fields; f_id++) {
    const cs_field_t  *f = cs_field_by_id(f_id);
    if (   (f->type & CS_FIELD_VARIABLE)
        && (f->type & CS_FIELD_USER)) {
      user_id++;

      int user_law = 0;

      if (   cs_field_get_key_int(f, kscavr) < 0
          && cs_field_get_key_int(f, kivisl) >= 0) {

        char *tmp = NULL;
        BFT_MALLOC(tmp, strlen(f->name) + 13, char);
        strcpy(tmp, f->name);
        strcat(tmp, "_diffusivity");

        char *prop_choice = _properties_choice(tmp);
        if (cs_gui_strcmp(prop_choice, "variable"))
          user_law = 1;
        BFT_FREE(prop_choice);
        BFT_FREE(tmp);
      }

      if (user_law) {
        int diff_id = cs_field_get_key_int(f, kivisl);
        cs_field_t *c_prop = NULL;
        if (diff_id > -1)
          c_prop = cs_field_by_id(diff_id);

        /* search the formula for the law */
        path = cs_xpath_init_path();
        cs_xpath_add_element(&path, "additional_scalars");
        cs_xpath_add_element_num(&path, "variable", user_id +1);
        cs_xpath_add_element(&path, "property");
        cs_xpath_add_element(&path, "formula");
        cs_xpath_add_function_text(&path);

        law = cs_gui_get_text_value(path);
        BFT_FREE(path);

        if (law != NULL) {
          /* return an empty interpreter */
          time0 = cs_timer_wtime();

          ev_law = mei_tree_new(law);
          BFT_FREE(law);

          char *tmp2 = NULL;
          BFT_MALLOC(tmp2, strlen(f->name) + 17, char);
          strcpy(tmp2, f->name);
          strcat(tmp2, "_diffusivity_ref");

          mei_tree_insert(ev_law,"x",0.0);
          mei_tree_insert(ev_law,"y",0.0);
          mei_tree_insert(ev_law,"z",0.0);
          mei_tree_insert(ev_law,tmp2,*visls0);
          BFT_FREE(tmp2);

          for (int f_id2 = 0; f_id2 < n_fields; f_id2++) {
            const cs_field_t  *f2 = cs_field_by_id(f_id2);
            if (f2->type & CS_FIELD_USER)
              mei_tree_insert(ev_law, f2->name, 0.0);
          }

          /* try to build the interpreter */
          char *tmp = NULL;
          BFT_MALLOC(tmp, strlen(f->name) + 13, char);
          strcpy(tmp, f->name);
          strcat(tmp, "_diffusivity");

          if (mei_tree_builder(ev_law))
            bft_error(__FILE__, __LINE__, 0,
                      _("Error: can not interpret expression: %s\n"), ev_law->string);

          if (mei_tree_find_symbol(ev_law, tmp))
            bft_error(__FILE__, __LINE__, 0,
                      _("Error: can not find the required symbol: %s\n"),
                      tmp);

          /* for each cell, update the value of the table of symbols for each scalar
             (including the thermal scalar), and evaluate the interpreter */

          if (cs_glob_fluid_properties->irovar == 1) {
            cs_field_t *c_rho = CS_F_(rho);
            for (iel = 0; iel < n_cells; iel++) {
              for (int f_id2 = 0; f_id2 < n_fields; f_id2++) {
                const cs_field_t  *f2 = cs_field_by_id(f_id2);
                if (f2->type & CS_FIELD_USER)
                  mei_tree_insert(ev_law,
                                  f2->name,
                                  f2->val[iel]);
              }
              mei_tree_insert(ev_law, "x", cell_cen[iel][0]);
              mei_tree_insert(ev_law, "y", cell_cen[iel][1]);
              mei_tree_insert(ev_law, "z", cell_cen[iel][2]);

              mei_evaluate(ev_law);
              c_prop->val[iel] = mei_tree_lookup(ev_law, tmp) * c_rho->val[iel];
            }
          }
          else {
            for (iel = 0; iel < n_cells; iel++) {
              for (int f_id2 = 0; f_id2 < n_fields; f_id2++) {
                const cs_field_t  *f2 = cs_field_by_id(f_id2);
                if (f2->type & CS_FIELD_USER)
                  mei_tree_insert(ev_law,
                                  f2->name,
                                  f2->val[iel]);
              }
              mei_tree_insert(ev_law, "x", cell_cen[iel][0]);
              mei_tree_insert(ev_law, "y", cell_cen[iel][1]);
              mei_tree_insert(ev_law, "z", cell_cen[iel][2]);

              mei_evaluate(ev_law);
              c_prop->val[iel] = mei_tree_lookup(ev_law, tmp) * cs_glob_fluid_properties->ro0;
            }
          }
          BFT_FREE(tmp);
          mei_tree_destroy(ev_law);

          cs_gui_add_mei_time(cs_timer_wtime() - time0);
        }
      }
    }
  }

#if _XML_DEBUG_
  bft_printf("==>UIPHYV\n");
  user_id = -1;
  for (int f_id = 0; f_id < n_fields; f_id++) {
    const cs_field_t  *f = cs_field_by_id(f_id);
    if (   (f->type & CS_FIELD_VARIABLE)
        && (f->type & CS_FIELD_USER)) {
      user_id++;
      if (   cs_field_get_key_int(f, kscavr) < 0
          && cs_field_get_key_int(f, kivisl) >= 0) {
        path = cs_xpath_init_path();
        cs_xpath_add_element(&path, "additional_scalars");
        cs_xpath_add_element_num(&path, "variable", user_id +1);
        cs_xpath_add_element(&path, "property");
        cs_xpath_add_element(&path, "formula");
        cs_xpath_add_function_text(&path);

        law = cs_gui_get_text_value(path);
        bft_printf("--law for the coefficient of diffusity of the scalar %s: %s\n",
                   f->name, law);
        BFT_FREE(path);
        BFT_FREE(law);
      }
    }
  }
#endif
}

/*----------------------------------------------------------------------------
 * 1D profile postprocessing
 *
 * Fortran Interface:
 *
 * SUBROUTINE UIPROF
 * *****************
 *
 *----------------------------------------------------------------------------*/

void CS_PROCF (uiprof, UIPROF) (void)
{
  const cs_lnum_t n_cells     = cs_glob_mesh->n_cells;
  const cs_lnum_t n_cells_ext = cs_glob_mesh->n_cells_with_ghosts;

  const cs_real_t *cell_cen = cs_glob_mesh_quantities->cell_cen;

  FILE *file = NULL;
  char *filename = NULL;
  char *title = NULL;
  char *name = NULL;
  char *path = NULL;
  char *formula = NULL;
  char *output_type = NULL;

  int output_format = 0;
  int fic_nbr = 0;
  int i, ii, iii, idim;
  int irang1, irangv;
  cs_lnum_t npoint, iel1, iel;
  int nvar_prop, nvar_prop4, output_frequency;
  double time_output;
  double x1 = 0., y1 = 0., z1 = 0.;
  double xx, yy, zz, xyz[3];
  double a, aa;
  double *array;
  static int ipass = 0;
  bool status;

  mei_tree_t *ev_formula  = NULL;

  /* get the number of 1D profile file to write*/

  fic_nbr = cs_gui_get_tag_count("/analysis_control/profiles/profile", 1);

  if (!fic_nbr) return;

  for (i = 0 ; i < fic_nbr ; i++) {

    /* for each profile, check the output frequency */

    output_format = _get_profile_format(i);
    output_type = _get_profile_output_type(i);
    time_output = 0.;
    status = false;

    if (cs_gui_strcmp(output_type, "time_value")) {
      time_output = _get_profile_coordinate(i, "output_frequency");
      int ifreqs = (int)((cs_glob_time_step->t_cur - cs_glob_time_step->t_prev) / time_output);
      if ((ifreqs > ipass) || (cs_glob_time_step->t_cur >= cs_glob_time_step->t_max &&
                               cs_glob_time_step->t_max > 0.))
        status = true;
    } else {
      output_frequency = (int) _get_profile_coordinate(i, "output_frequency");
      if (   (cs_glob_time_step->nt_max == cs_glob_time_step->nt_cur)
          || (output_frequency > 0 && (cs_glob_time_step->nt_cur % output_frequency) == 0))
        status = true;
    }
    BFT_FREE(output_type);

    if (status) {

      ipass++;
      path = cs_xpath_init_path();
      cs_xpath_add_elements(&path, 2, "analysis_control", "profiles");
      cs_xpath_add_element_num(&path, "profile", i+1);
      cs_xpath_add_element(&path, "formula");
      cs_xpath_add_function_text(&path);
      formula = cs_gui_get_text_value(path);
      ev_formula = mei_tree_new(formula);
      mei_tree_insert(ev_formula, "s", 0.0);
      BFT_FREE(formula);
      BFT_FREE(path);

      /* try to build the interpreter */

      if (mei_tree_builder(ev_formula))
        bft_error(__FILE__, __LINE__, 0,
                  _("Error: can not interpret expression: %s\n %i"),
                  ev_formula->string, mei_tree_builder(ev_formula));

      const char *coord[] = {"x","y","z"};

      if (mei_tree_find_symbols(ev_formula, 3, coord))
        bft_error(__FILE__, __LINE__, 0,
                  _("Error: can not find the required symbol: %s\n"),
                  "x, y or z");

      nvar_prop = _get_profile_names_number(i);
      nvar_prop4 = nvar_prop + 4;
      BFT_MALLOC(array, nvar_prop4, double);

      /* Only the first processor rank opens the file */

      if (cs_glob_rank_id <= 0) {

        filename = _get_profile("label", i);
        title    = _get_profile("title", i);

        if (output_frequency > 0 || time_output > 0.) {

          char buf1[16];

          /* Extension creation : format stored in 'buffer' */

          sprintf(buf1, "_%.4i", cs_glob_time_step->nt_cur);

          BFT_REALLOC(filename, strlen(filename) + strlen(buf1) + 1, char);

          strcat(filename, buf1);

        }

        BFT_REALLOC(filename, strlen(filename) + 4 + 1, char);
        if (output_format == 0)
          strcat(filename, ".dat");
        else
          strcat(filename, ".csv");
        file = fopen(filename, "w");

        if (file ==  NULL) {
          cs_base_warn(__FILE__, __LINE__);
          bft_printf( _("Unable to open the file: %s\n"), filename);
          break;
        }

        if (output_format == 0) {
          fprintf(file, "# Code_Saturne results 1D profile\n#\n");
          fprintf(file, "# Iteration output: %i\n", cs_glob_time_step->nt_cur);
          fprintf(file, "# Time output:     %12.5e\n#\n", cs_glob_time_step->t_cur);
          fprintf(file, "#TITLE: %s\n", title);
          fprintf(file, "#COLUMN_TITLES: Distance | X | Y | Z");
          for (ii = 0 ; ii < nvar_prop ; ii++) {
            char *buffer = _get_profile_label_name(i, ii);
            fprintf(file, " | %s", buffer);
            BFT_FREE(buffer);
          }
          fprintf(file, "\n");
        }
        else {
          fprintf(file, "s, x, y, z");
          for (ii = 0 ; ii < nvar_prop ; ii++) {
            char *buffer = _get_profile_label_name(i, ii);
            fprintf(file, ", %s", buffer);
            BFT_FREE(buffer);
          }
          fprintf(file, "\n");
        }
        BFT_FREE(filename);
        BFT_FREE(title);
      }

      path = cs_xpath_init_path();
      cs_xpath_add_elements(&path, 2, "analysis_control", "profiles");
      cs_xpath_add_element_num(&path, "profile", i+1);
      cs_xpath_add_element(&path, "points");
      cs_xpath_add_function_text(&path);
      if (!cs_gui_get_int(path, &npoint))
        bft_error(__FILE__, __LINE__, 0, _("Invalid xpath: %s\n"), path);
      BFT_FREE(path);

      iel1   = -999;
      irang1 = -999;

      a = 1. / (double) (npoint-1);

      for (ii = 0; ii < npoint; ii++) {

        aa = ii*a;
        mei_tree_insert(ev_formula,"s",aa);
        mei_evaluate(ev_formula);

        xyz[0] = mei_tree_lookup(ev_formula,"x");
        xyz[1] = mei_tree_lookup(ev_formula,"y");
        xyz[2] = mei_tree_lookup(ev_formula,"z");

        if (ii == 0) {
          x1 = xyz[0];
          y1 = xyz[1];
          z1 = xyz[2];
        }

        CS_PROCF(findpt, FINDPT)(&n_cells_ext, &n_cells, cell_cen,
                                &xyz[0], &xyz[1], &xyz[2],
                                &iel,    &irangv);

        if ((iel != iel1) || (irangv != irang1)) {
          iel1 = iel;
          irang1 = irangv;

          if (cs_glob_rank_id == irangv) {

            iel--;
            xx = cell_cen[3 * iel + 0];
            yy = cell_cen[3 * iel + 1];
            zz = cell_cen[3 * iel + 2];
            array[1] = xx;
            array[2] = yy;
            array[3] = zz;
            xx = xx - x1;
            yy = yy - y1;
            zz = zz - z1;
            array[0] = sqrt(xx*xx + yy*yy + zz*zz);

            for (iii=0; iii < nvar_prop; iii++) {

              name = _get_profile_name(i, iii);
              idim = _get_profile_component(i, iii);

              cs_field_t *f = cs_field_by_name_try(name);

              if (f != NULL) {
                if (f->type & CS_FIELD_VARIABLE) {
                  if (f->dim > 1)
                    array[iii+4] = f->val[f->dim * iel + idim];
                  else
                    array[iii+4] = f->val[iel + idim * n_cells_ext];
                }
                else
                  array[iii+4] = f->val[iel];
              }
              else {
                char *label = _get_profile_label_name(i, iii);
                const int keylbl = cs_field_key_id("label");
                for (int f_id = 0; f_id < cs_field_n_fields(); f_id++) {
                  f = cs_field_by_id(f_id);
                  const char *flab = cs_field_get_key_str(f, keylbl);
                  if (cs_gui_strcmp(label, flab))
                    array[iii+4] = f->val[iel];
                }
              }

              BFT_FREE(name);
            }

          } else {

            for (iii=0; iii < nvar_prop4; iii++)
              array[iii] = 0.0;
          }

          /* Send to other processors if parallel */
#if defined(HAVE_MPI)
          if (cs_glob_rank_id >= 0) {
            MPI_Bcast(array,
                      nvar_prop4,
                      CS_MPI_REAL,
                      irangv,
                      cs_glob_mpi_comm);
          }
#endif

          if (cs_glob_rank_id <= 0) {
            if (output_format == 0) {
              for (iii=0; iii < nvar_prop4; iii++)
                fprintf(file, "%12.5e ", array[iii]);
              fprintf(file, "\n");
            }
            else {
              if (nvar_prop > 0) {
                for (iii=0; iii < nvar_prop4 - 1; iii++)
                  fprintf(file, "%12.5e, ", array[iii]);
                fprintf(file, "%12.5e ", array[nvar_prop4 - 1]);
              }
              fprintf(file, "\n");
            }
          }
        }
      }
      mei_tree_destroy(ev_formula);

      if (cs_glob_rank_id <= 0) fclose(file);

      BFT_FREE(array);
    }
  }
}


/*----------------------------------------------------------------------------
 * groundwater model : read laws for capacity, saturation and permeability
 *
 * Fortran Interface:
 *
 * subroutine uidapp
 * *****************
 * integer         permeability    <--  permeability type
 * integer         diffusion       <--  diffusion type
 * integer         gravity         <--  check if gravity is taken into account
 * double          gravity_x       <--  gravity direction
 * double          gravity_y       <--  gravity direction
 * double          gravity_z       <--  gravity direction
 *----------------------------------------------------------------------------*/

void CS_PROCF (uidapp, UIDAPP) (const cs_int_t   *permeability,
                                const cs_int_t   *diffusion,
                                const cs_int_t   *gravity,
                                const cs_real_t  *gravity_x,
                                const cs_real_t  *gravity_y,
                                const cs_real_t  *gravity_z)
{
  char *path = NULL;
  char *status = NULL;
  double time0;
  char *formula = NULL;
  cs_lnum_t *cells_list = NULL;
  cs_lnum_t cells = 0;
  mei_tree_t *ev_formula  = NULL;

  const cs_lnum_t n_cells_ext = cs_glob_mesh->n_cells_with_ghosts;
  const cs_lnum_t n_cells     = cs_glob_mesh->n_cells;
  const cs_real_3_t *restrict cell_cen
    = (const cs_real_3_t *restrict)cs_glob_mesh_quantities->cell_cen;

  cs_field_t *fsaturation   = cs_field_by_name_try("saturation");
  cs_field_t *fcapacity     = cs_field_by_name_try("capacity");
  cs_field_t *fpermeability = cs_field_by_name_try("permeability");
  cs_field_t *fpressure     = CS_F_(p);

  cs_real_t   *saturation_field = fsaturation->val;
  cs_real_t   *capacity_field   = fcapacity->val;
  cs_real_t   *pressure_field   = fpressure->val;

  cs_real_t     *permeability_field = NULL;
  cs_real_6_t   *permeability_field_v = NULL;

  cs_gnum_t cw[3];

  if (*permeability == 0)
    permeability_field = fpermeability->val;
  else
    permeability_field_v = (cs_real_6_t *)fpermeability->val;

  /* number of volumic zone */
  int zones = cs_gui_get_tag_count("/solution_domain/volumic_conditions/zone\n", 1);

  for (int i = 1; i < zones+1; i++) {
    path = cs_xpath_init_path();
    cs_xpath_add_elements(&path, 2, "solution_domain", "volumic_conditions");
    cs_xpath_add_element_num(&path, "zone", i);
    cs_xpath_add_attribute(&path, "groundwater_law");
    status = cs_gui_get_attribute_value(path);
    BFT_FREE(path);

    if (cs_gui_strcmp(status, "on")) {
      char *zone_id = _volumic_zone_id(i);
      cells_list = _get_cells_list(zone_id, n_cells_ext, &cells);

      /* check if user law or Van Genuchten */
      path = cs_xpath_init_path();
      cs_xpath_add_elements(&path, 3,
                            "thermophysical_models",
                            "groundwater",
                            "groundwater_law");
      cs_xpath_add_test_attribute(&path, "zone_id", zone_id);
      cs_xpath_add_attribute(&path, "model");
      char *mdl = cs_gui_get_attribute_value(path);
      BFT_FREE(path);

      if (cs_gui_strcmp(mdl, "VanGenuchten")) {
        double alpha_param, ks_param, l_param, n_param, thetas_param, thetar_param;
        double ks_xx, ks_yy, ks_zz, ks_xy, ks_xz, ks_yz;
        double molecular_diffusion;
        _van_genuchten_parameter_value(zone_id, "alpha",  &alpha_param);
        _van_genuchten_parameter_value(zone_id, "l",      &l_param);
        _van_genuchten_parameter_value(zone_id, "n",      &n_param);
        _van_genuchten_parameter_value(zone_id, "thetar", &thetar_param);
        _van_genuchten_parameter_value(zone_id, "thetas", &thetas_param);
        _van_genuchten_parameter_value(zone_id, "molecularDiff",
                                       &molecular_diffusion);

        double m_param = 1 - 1 / n_param;
        if (*permeability == 0)
          _van_genuchten_parameter_value(zone_id, "ks",     &ks_param);
        else {
          _van_genuchten_parameter_value(zone_id, "ks_xx",     &ks_xx);
          _van_genuchten_parameter_value(zone_id, "ks_yy",     &ks_yy);
          _van_genuchten_parameter_value(zone_id, "ks_zz",     &ks_zz);
          _van_genuchten_parameter_value(zone_id, "ks_xy",     &ks_xy);
          _van_genuchten_parameter_value(zone_id, "ks_xz",     &ks_xz);
          _van_genuchten_parameter_value(zone_id, "ks_yz",     &ks_yz);
        }

        for (cs_lnum_t icel = 0; icel < cells; icel++) {
          cs_lnum_t iel = cells_list[icel];
          double pres = pressure_field[iel];

          if (*gravity == 1)
            pres -= (cell_cen[iel][0] * *gravity_x +
                     cell_cen[iel][1] * *gravity_y +
                     cell_cen[iel][2] * *gravity_z );

          if (pres >= 0) {
            capacity_field[iel] = 0.;
            saturation_field[iel] = thetas_param;

            if (*permeability == 0)
              permeability_field[iel] = ks_param;
            else {
              permeability_field_v[iel][0] = ks_xx;
              permeability_field_v[iel][1] = ks_yy;
              permeability_field_v[iel][2] = ks_zz;
              permeability_field_v[iel][3] = ks_xy;
              permeability_field_v[iel][4] = ks_xz;
              permeability_field_v[iel][5] = ks_yz;
            }
          }
          else {

            double tmp1 = pow(fabs(alpha_param * pres), n_param);
            double tmp2 = 1. / (1. + tmp1);
            double se_param = pow(tmp2, m_param);
            double perm = pow(se_param, l_param) *
                          pow((1. - pow((1. - tmp2), m_param)), 2);

            capacity_field[iel] = -m_param * n_param * tmp1 *
                                  (thetas_param - thetar_param) *
                                   se_param * tmp2 / pres;
            saturation_field[iel] = thetar_param +
                                    se_param * (thetas_param - thetar_param);


            if (*permeability == 0)
              permeability_field[iel] = perm * ks_param;
            else {
              permeability_field_v[iel][0] = perm * ks_xx;
              permeability_field_v[iel][1] = perm * ks_yy;
              permeability_field_v[iel][2] = perm * ks_zz;
              permeability_field_v[iel][3] = perm * ks_xy;
              permeability_field_v[iel][4] = perm * ks_xz;
              permeability_field_v[iel][5] = perm * ks_yz;
            }
          }
        }
      }
      else {  // user law
        path = cs_xpath_init_path();
        cs_xpath_add_elements(&path, 3,
                              "thermophysical_models",
                              "groundwater",
                              "groundwater_law");
        cs_xpath_add_test_attribute(&path, "zone_id", zone_id);
        cs_xpath_add_element(&path, "formula");
        cs_xpath_add_function_text(&path);
        formula = cs_gui_get_text_value(path);
        BFT_FREE(path);

        if (formula != NULL) {
          ev_formula = mei_tree_new(formula);
          BFT_FREE(formula);
          mei_tree_insert(ev_formula,"x",0.0);
          mei_tree_insert(ev_formula,"y",0.0);
          mei_tree_insert(ev_formula,"z",0.0);

          /* try to build the interpreter */
          if (mei_tree_builder(ev_formula))
            bft_error(__FILE__, __LINE__, 0,
                      _("Error: can not interpret expression: %s\n %i"),
                      ev_formula->string, mei_tree_builder(ev_formula));

          if (*permeability == 0) {
            const char *symbols[] = {"capacity",
                                     "saturation"
                                     "permeability"};
            if (mei_tree_find_symbols(ev_formula, 3, symbols))
                bft_error(__FILE__, __LINE__, 0,
                          _("Error: can not find the required symbol: %s\n"),
                          "capacity, saturation or permeability");
          }
          else {
            const char *symbols[] = {"capacity",
                                     "saturation",
                                     "permeability[XX]",
                                     "permeability[YY]",
                                     "permeability[ZZ]",
                                     "permeability[XY]",
                                     "permeability[XZ]",
                                     "permeability[YZ]"};
            if (mei_tree_find_symbols(ev_formula, 8, symbols))
                bft_error(__FILE__, __LINE__, 0,
                          _("Error: can not find the required symbol: %s\n %s\n %s\n"),
                          "capacity, saturation,",
                          "          permeability[XX], permeability[YY], permeability[ZZ]",
                          "          permeability[XY], permeability[XZ] or permeability[YZ]");
          }

          for (cs_lnum_t icel = 0; icel < cells; icel++) {
              cs_lnum_t iel = cells_list[icel];
              mei_tree_insert(ev_formula, "x", cell_cen[iel][0]);
              mei_tree_insert(ev_formula, "y", cell_cen[iel][1]);
              mei_tree_insert(ev_formula, "z", cell_cen[iel][2]);
              mei_evaluate(ev_formula);

              capacity_field[iel] = mei_tree_lookup(ev_formula,"capacity");
              saturation_field[iel] = mei_tree_lookup(ev_formula,"saturation");
              if (*permeability == 1) {
                  permeability_field_v[iel][0] = mei_tree_lookup(ev_formula,"permeability[XX]");
                  permeability_field_v[iel][1] = mei_tree_lookup(ev_formula,"permeability[YY]");
                  permeability_field_v[iel][2] = mei_tree_lookup(ev_formula,"permeability[ZZ]");
                  permeability_field_v[iel][3] = mei_tree_lookup(ev_formula,"permeability[XY]");
                  permeability_field_v[iel][4] = mei_tree_lookup(ev_formula,"permeability[XZ]");
                  permeability_field_v[iel][5] = mei_tree_lookup(ev_formula,"permeability[YZ]");
              }
              else
                  permeability_field[iel] = mei_tree_lookup(ev_formula,"permeability");

          }
          mei_tree_destroy(ev_formula);
        }
      }

      if (*diffusion == 1) {
        cs_field_t *fturbvisco
          = cs_field_by_name_try("anisotropic_turbulent_viscosity");
        cs_real_6_t  *visten_v = (cs_real_6_t *)fturbvisco->val;
        const cs_real_3_t *vel = (const cs_real_3_t *)(CS_F_(u)->val);

        double laminar_diffus;
        double turbulent_diffus;
        path = cs_xpath_init_path();
        cs_xpath_add_elements(&path, 3,
                              "thermophysical_models",
                              "groundwater",
                              "groundwater_law");
        cs_xpath_add_test_attribute(&path, "zone_id", zone_id);
        cs_xpath_add_element(&path, "diffusion_coefficient");
        cs_xpath_add_element(&path, "longitudinal");
        cs_xpath_add_function_text(&path);
        cs_gui_get_double(path, &laminar_diffus);
        BFT_FREE(path);

        path = cs_xpath_init_path();
        cs_xpath_add_elements(&path, 3,
                              "thermophysical_models",
                              "groundwater",
                              "groundwater_law");
        cs_xpath_add_test_attribute(&path, "zone_id", zone_id);
        cs_xpath_add_element(&path, "diffusion_coefficient");
        cs_xpath_add_element(&path, "transverse");
        cs_xpath_add_function_text(&path);
        cs_gui_get_double(path, &turbulent_diffus);
        BFT_FREE(path);

        for (cs_lnum_t icel = 0; icel < cells; icel++) {
          cs_lnum_t iel = cells_list[icel];
          double norm = sqrt(vel[iel][0] * vel[iel][0] +
                             vel[iel][1] * vel[iel][1] +
                             vel[iel][2] * vel[iel][2]);
          double tmp = turbulent_diffus * norm;
          double diff = laminar_diffus - turbulent_diffus;
          double denom = norm + 1.e-15;
          visten_v[iel][0] = tmp + diff * vel[iel][0] * vel[iel][0] / denom;
          visten_v[iel][1] = tmp + diff * vel[iel][1] * vel[iel][1] / denom;
          visten_v[iel][2] = tmp + diff * vel[iel][2] * vel[iel][2] / denom;
          visten_v[iel][3] =       diff * vel[iel][1] * vel[iel][0] / denom;
          visten_v[iel][4] =       diff * vel[iel][1] * vel[iel][2] / denom;
          visten_v[iel][5] =       diff * vel[iel][2] * vel[iel][0] / denom;
        }
      }
      else {
        /* TODO to control */
        cs_field_t *fturbvisco
          = cs_field_by_name_try("turbulent_viscosity");
        double  *visten = fturbvisco->val;
        const cs_real_3_t *vel = (const cs_real_3_t *)(CS_F_(u)->val);

        double diffus;
        path = cs_xpath_init_path();
        cs_xpath_add_elements(&path, 3,
                              "thermophysical_models",
                              "groundwater",
                              "groundwater_law");
        cs_xpath_add_test_attribute(&path, "zone_id", zone_id);
        cs_xpath_add_element(&path, "diffusion_coefficient");
        cs_xpath_add_element(&path, "isotropic");
        cs_xpath_add_function_text(&path);
        cs_gui_get_double(path, &diffus);
        BFT_FREE(path);

        for (cs_lnum_t icel = 0; icel < cells; icel++) {
          cs_lnum_t iel = cells_list[icel];
          double norm = sqrt(vel[iel][0] * vel[iel][0] +
                             vel[iel][1] * vel[iel][1] +
                             vel[iel][2] * vel[iel][2]);
          visten[iel] = diffus * norm;
        }
      }

      int user_id = -1;
      int n_fields = cs_field_n_fields();
      const int kivisl = cs_field_key_id("scalar_diffusivity_id");

      for (int f_id = 0; f_id < n_fields; f_id++) {
        cs_field_t *f = cs_field_by_id(f_id);
        if (   (f->type & CS_FIELD_VARIABLE)
            && (f->type & CS_FIELD_USER)) {
          user_id++;
          char *delayname = NULL;
          int len = strlen(f->name) + 7;
          BFT_MALLOC(delayname, len, char);
          strcpy(delayname, f->name);
          strcat(delayname, "_delay");
          cs_field_t *fdelay = cs_field_by_name_try(delayname);
          cs_real_t   *delay_val = fdelay->val;

          char *diffname = NULL;
          BFT_MALLOC(diffname, strlen(f->name) + 13, char);
          strcpy(diffname, f->name);
          strcat(diffname, "_diffusivity");

          int diff_id = cs_field_get_key_int(f, kivisl);
          cs_field_t *c_prop = NULL;
          if (diff_id >= 0)
            c_prop = cs_field_by_id(diff_id);

          path = cs_xpath_init_path();
          cs_xpath_add_elements(&path, 3,
                                "thermophysical_models",
                                "groundwater",
                                "groundwater_law");
          cs_xpath_add_test_attribute(&path, "zone_id", zone_id);
          cs_xpath_add_element_num(&path, "variable", user_id +1);
          cs_xpath_add_element(&path, "property");
          cs_xpath_add_element(&path, "formula");
          cs_xpath_add_function_text(&path);

          formula = cs_gui_get_text_value(path);
          BFT_FREE(path);

          if (formula != NULL) {
            /* return an empty interpreter */
            time0 = cs_timer_wtime();

            ev_formula = mei_tree_new(formula);
            BFT_FREE(formula);

            mei_tree_insert(ev_formula, "x", 0.0);
            mei_tree_insert(ev_formula, "y", 0.0);
            mei_tree_insert(ev_formula, "z", 0.0);
            mei_tree_insert(ev_formula, "saturation", 0.0);
            mei_tree_insert(ev_formula, f->name, 0.0);

            /* try to build the interpreter */

            if (mei_tree_builder(ev_formula))
              bft_error(__FILE__, __LINE__, 0,
                        _("Error: can not interpret expression: %s\n"),
                        ev_formula->string);

            const char *symbols[] = {delayname,
                                     diffname};

            if (mei_tree_find_symbols(ev_formula, 2, symbols))
              bft_error(__FILE__, __LINE__, 0,
                        _("Error: can not find the required symbol: %s %s\n"),
                        delayname, diffname);

            cw[0] = 0; cw[1] = 0;

            for (cs_lnum_t icel = 0; icel < cells; icel++) {

              cs_lnum_t iel = cells_list[icel];

              mei_tree_insert(ev_formula, "x", cell_cen[iel][0]);
              mei_tree_insert(ev_formula, "y", cell_cen[iel][1]);
              mei_tree_insert(ev_formula, "z", cell_cen[iel][2]);
              mei_tree_insert(ev_formula, "saturation", saturation_field[iel]);
              mei_tree_insert(ev_formula, f->name, f->val[iel]);

              mei_evaluate(ev_formula);
              delay_val[iel] = mei_tree_lookup(ev_formula, delayname);
              c_prop->val[iel] = mei_tree_lookup(ev_formula, diffname);

              if (c_prop->val[iel] < 0.)
                cw[0] += 1;
              if (*diffusion == 1 && c_prop->val[iel] <= 0.)
                cw[1] += 1;
            }

            cs_parall_counter(cw, 2);

            if (cw[0] > 0)
              bft_printf(_("soil_tracer_law, WARNING:\n"
                           "  isotropic diffusion is < 0 in %llu cells.\n"),
                         (unsigned long long)(cw[0]));
            if (cw[1] > 0)
              bft_printf(_("soil_tracer_law, WARNING:\n"
                           "  isotropic diffusion is <= 0 in %llu cells\n"
                           "  and  there is no anisotropic part.\n"),
                         (unsigned long long)(cw[1]));

            mei_tree_destroy(ev_formula);

            cs_gui_add_mei_time(cs_timer_wtime() - time0);
          }
          BFT_FREE(delayname);
          BFT_FREE(diffname);
        }
      }

      BFT_FREE(cells_list);
      BFT_FREE(mdl);
      BFT_FREE(zone_id);
    }
    BFT_FREE(status);
  }

  /* check values */

  cw[0] = 0; cw[1] = 0; cw[2] = 0;

  for (cs_lnum_t iel = 0; iel < n_cells; iel++) {
    if (saturation_field[iel] > 1. || saturation_field[iel] < 0.)
      cw[0] += 1;

    if (capacity_field[iel] < 0.)
      cw[1] += 1;

    if (*permeability == 0) {
      if (permeability_field[iel] < 0.)
        cw[2] += 1;
    }
  }

  cs_parall_counter(cw, 3);

  if (cw[0] > 0)
    bft_printf(_("soil_tracer_law, WARNING:\n"
                 "  saturation is outside [0, 1] in %llu cells.\n"),
               (unsigned long long)(cw[0]));

  if (cw[1] > 0)
    bft_printf(_("soil_tracer_law, WARNING:\n"
                 "  capacity is < 0 in %llu cells.\n"),
               (unsigned long long)(cw[1]));

  if (cw[2] > 0)
    bft_printf(_("soil_tracer_law, WARNING:\n"
                 "  isotropic permeability is < 0 in %llu cells.\n"),
               (unsigned long long)(cw[2]));
}

/*----------------------------------------------------------------------------
 * Free memory: clean global private variables and libxml2 variables
 *
 * Fortran Interface:
 *
 * SUBROUTINE MEMUI1
 * *****************
 *
 * INTEGER          NCHARB  <-- number of coal
 *----------------------------------------------------------------------------*/

void CS_PROCF (memui1, MEMUI1) (const int *ncharb)
{
  cs_gui_boundary_conditions_free_memory(ncharb);
  cs_gui_finalize();
}

/*============================================================================
 * Public function definitions
 *============================================================================*/

/*----------------------------------------------------------------------------
 * Initialize GUI reader structures.
 *----------------------------------------------------------------------------*/

void
cs_gui_init(void)
{
  assert(cs_glob_var == NULL);

  BFT_MALLOC(cs_glob_var, 1, cs_var_t);

  cs_glob_var->model       = NULL;
  cs_glob_var->model_value = NULL;
}

/*-----------------------------------------------------------------------------
 * Free memory: clean global private variables and libxml2 variables
 *----------------------------------------------------------------------------*/

void
cs_gui_finalize(void)
{
  /* clean memory for global private structure vars */
  BFT_FREE(cs_glob_var->model);
  BFT_FREE(cs_glob_var->model_value);
  BFT_FREE(cs_glob_var);

  mei_data_free();

  /* clean memory for xml document */

#if defined(HAVE_LIBXML2)
  if (xpathCtx != NULL) xmlXPathFreeContext(xpathCtx);
  if (node != NULL) xmlFreeNode(node);
#endif

  /* Shutdown libxml */

#if defined(HAVE_LIBXML2)
  xmlCleanupParser();
  xmlMemoryDump();
#endif
}

/*-----------------------------------------------------------------------------
 * Selection of linear solvers.
 *----------------------------------------------------------------------------*/

void
cs_gui_linear_solvers(void)
{
  if (!cs_gui_file_is_loaded())
    return;

  bool multigrid = false;
  cs_sles_it_type_t sles_it_type = CS_SLES_N_IT_TYPES;

  double tmp;
  char* algo_choice = NULL;
  char* precond_choice = NULL;

  const int n_max_iter_default = 10000;

  int n_fields = cs_field_n_fields();

  for (int f_id = 0; f_id < n_fields; f_id++) {
    cs_field_t  *f = cs_field_by_id(f_id);
    if (f->type & CS_FIELD_VARIABLE) {

      tmp = (double) n_max_iter_default;
      _variable_value(f->name, "max_iter_number", &tmp);
      int n_max_iter = (int) tmp;

      multigrid = false;
      sles_it_type = CS_SLES_N_IT_TYPES;

      algo_choice = _variable_choice(f->name, "solver_choice");
      precond_choice = _variable_choice(f->name, "preconditioning_choice");

      if (cs_gui_strcmp(algo_choice, "multigrid"))
        multigrid = true;
      else if (cs_gui_strcmp(algo_choice, "conjugate_gradient"))
        sles_it_type = CS_SLES_PCG;
      else if (cs_gui_strcmp(algo_choice, "jacobi"))
        sles_it_type = CS_SLES_JACOBI;
      else if (cs_gui_strcmp(algo_choice, "bi_cgstab"))
        sles_it_type = CS_SLES_BICGSTAB;
      else if (cs_gui_strcmp(algo_choice, "bi_cgstab2"))
        sles_it_type = CS_SLES_BICGSTAB2;
      else if (cs_gui_strcmp(algo_choice, "gmres"))
        sles_it_type = CS_SLES_GMRES;
      else if (cs_gui_strcmp(algo_choice, "gauss_seidel"))
        sles_it_type = CS_SLES_P_GAUSS_SEIDEL;
      else if (cs_gui_strcmp(algo_choice, "PCR3"))
        sles_it_type = CS_SLES_PCR3;

      /* If choice is "automatic" or unspecified, delay
         choice to cs_sles_default, so do nothing here */

      if (sles_it_type < CS_SLES_N_IT_TYPES) {

        int poly_degree = 0;
        bool pc_multigrid = false;

        if (cs_gui_strcmp(precond_choice, "jacobi"))
          poly_degree = 0;
        else if (cs_gui_strcmp(precond_choice, "none"))
          poly_degree = -1;
        else if (cs_gui_strcmp(precond_choice, "polynomial"))
          poly_degree = 1;
        else if (cs_gui_strcmp(precond_choice, "multigrid")) {
          pc_multigrid = true;
          poly_degree = -1;
        }
        else { /* "automatic" or undefined */
          if (sles_it_type == CS_SLES_PCG) {
            pc_multigrid = true;
            poly_degree = -1;
          }
        }

        cs_sles_it_t *c = cs_sles_it_define(f->id, NULL, sles_it_type,
                                            poly_degree, n_max_iter);

        if (pc_multigrid) {
          cs_sles_pc_t *pc = cs_multigrid_pc_create();
          cs_multigrid_t *mg = cs_sles_pc_get_context(pc);
          cs_sles_it_transfer_pc(c, &pc);
          cs_multigrid_set_solver_options
            (mg,
             CS_SLES_P_GAUSS_SEIDEL,
             CS_SLES_P_GAUSS_SEIDEL,
             CS_SLES_PCG,
             1,   /* n max cycles */
             1,   /* n max iter for descent */
             1,   /* n max iter for ascent */
             500, /* n max iter for coarse solve */
             0, 0, 0,    /* precond degree */
             -1, -1, 1); /* precision multiplier */
        }

      }

      else if (multigrid == true) {
        cs_multigrid_t *mg = cs_multigrid_define(f->id, NULL);
        cs_multigrid_set_solver_options
          (mg,
           CS_SLES_PCG, CS_SLES_PCG, CS_SLES_PCG,
           100, /* n max cycles */
           2,   /* n max iter for descent (default 2) */
           10,  /* n max iter for ascent (default 10) */
           n_max_iter,
           0, 0, 0,  /* precond degree */
           1, 1, 1); /* precision multiplier */

        /* If we have convection, set appropriate options */
        if (f_id >= 0) {
          cs_var_cal_opt_t var_cal_opt;
          cs_field_get_key_struct(cs_field_by_id(f_id),
                                  cs_field_key_id("var_cal_opt"),
                                  &var_cal_opt);
          if (var_cal_opt.iconv > 0)
            cs_multigrid_set_solver_options
              (mg,
               CS_SLES_P_GAUSS_SEIDEL,
               CS_SLES_P_GAUSS_SEIDEL,
               CS_SLES_P_GAUSS_SEIDEL,
               100, /* n max cycles */
               3,   /* n max iter for descent (default 2) */
               2,   /* n max iter for ascent (default 10) */
               100, /* n max iter coarse solver */
               0, 0, 0,  /* precond degree */
               -1, -1, 1); /* precision multiplier */
        }
      }

      BFT_FREE(algo_choice);
      BFT_FREE(precond_choice);

    }
  }
}

/*-----------------------------------------------------------------------------
 * Modify double numerical parameters.
 *
 * parameters:
 *   param    <-- label of the numerical parameter
 *   keyword  <-- value of the numerical parameter
 *----------------------------------------------------------------------------*/

void
cs_gui_numerical_double_parameters(const char  *param,
                                   double      *keyword)
{
  char  *path = NULL;
  double result;

  path = cs_xpath_init_path();
  cs_xpath_add_element(&path, "numerical_parameters");
  cs_xpath_add_element(&path, param);
  cs_xpath_add_function_text(&path);

  if (cs_gui_get_double(path, &result))
    *keyword = result;

  BFT_FREE(path);
}

/*-----------------------------------------------------------------------------
 * Define parallel IO settings.
 *----------------------------------------------------------------------------*/

void
cs_gui_parallel_io(void)
{
  int op_id;
  char  *path = NULL;

  int  rank_step = 0, block_size = -1;

  cs_file_mode_t op_mode[2] = {CS_FILE_MODE_READ, CS_FILE_MODE_WRITE};
  const char *op_name[2] = {"read_method", "write_method"};

  if (!cs_gui_file_is_loaded())
    return;

  /* Block IO read and write method */

  for (op_id = 0; op_id < 2; op_id++) {

    cs_file_access_t  m = CS_FILE_DEFAULT;
    char  *method_name = NULL;

    path = cs_xpath_init_path();
    cs_xpath_add_elements(&path, 3,
                          "calculation_management", "block_io", op_name[op_id]);
    cs_xpath_add_function_text(&path);

    method_name = cs_gui_get_text_value(path);

    if (method_name != NULL) {
      if (!strcmp(method_name, "default"))
        m = CS_FILE_DEFAULT;
      else if (!strcmp(method_name, "stdio serial"))
        m = CS_FILE_STDIO_SERIAL;
      else if (!strcmp(method_name, "stdio parallel"))
        m = CS_FILE_STDIO_PARALLEL;
      else if (!strcmp(method_name, "mpi independent"))
        m = CS_FILE_MPI_INDEPENDENT;
      else if (!strcmp(method_name, "mpi noncollective"))
        m = CS_FILE_MPI_NON_COLLECTIVE;
      else if (!strcmp(method_name, "mpi collective"))
        m = CS_FILE_MPI_COLLECTIVE;
#if defined(HAVE_MPI)
      cs_file_set_default_access(op_mode[op_id], m, MPI_INFO_NULL);
#else
      cs_file_set_default_access(op_mode[op_id], m);
#endif
      BFT_FREE(method_name);
    }

    BFT_FREE(path);

  }

#if defined(HAVE_MPI)

  /* Rank step and block buffer size */

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3,
                        "calculation_management", "block_io", "rank_step");
  cs_xpath_add_function_text(&path);
  cs_gui_get_int(path, &rank_step);

  BFT_FREE(path);

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3,
                        "calculation_management",
                        "block_io",
                        "min_block_size");
  cs_xpath_add_function_text(&path);
  cs_gui_get_int(path, &block_size);

  BFT_FREE(path);

  if (rank_step > 0 || block_size > -1) {
    int def_rank_step, def_block_size;
    cs_file_get_default_comm(&def_rank_step, &def_block_size, NULL, NULL);
    if (rank_step < 1)
      rank_step = def_rank_step;
    if (block_size < 0)
      block_size = def_block_size;
    cs_file_set_default_comm(rank_step, block_size, cs_glob_mpi_comm);
  }

#endif /* defined(HAVE_MPI) */
}

/*-----------------------------------------------------------------------------
 * Set partitioning options.
 *----------------------------------------------------------------------------*/

void
cs_gui_partition(void)
{
  char  *path = NULL;
  char  *part_name = NULL;
  char  *s_perio = NULL;
  char  *s_output = NULL;
  char  *s_list = NULL;

  cs_partition_algorithm_t a = CS_PARTITION_DEFAULT;
  bool ignore_perio = false;
  int  rank_step = 1;
  int  write_level = 1;
  int  n_add_parts = 0;
  int  *add_parts = NULL;

  if (!cs_gui_file_is_loaded())
    return;

  /* Partitioning type */

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3,
                        "calculation_management", "partitioning", "type");
  cs_xpath_add_function_text(&path);

  part_name = cs_gui_get_text_value(path);

  if (part_name != NULL) {
    if (!strcmp(part_name, "default"))
      a = CS_PARTITION_DEFAULT;
    else if (!strcmp(part_name, "morton sfc"))
      a = CS_PARTITION_SFC_MORTON_BOX;
    else if (!strcmp(part_name, "morton sfc cube"))
      a = CS_PARTITION_SFC_MORTON_CUBE;
    else if (!strcmp(part_name, "hilbert sfc"))
      a = CS_PARTITION_SFC_HILBERT_BOX;
    else if (!strcmp(part_name, "hilbert sfc cube"))
      a = CS_PARTITION_SFC_HILBERT_CUBE;
    else if (!strcmp(part_name, "scotch"))
      a = CS_PARTITION_SCOTCH;
    else if (!strcmp(part_name, "metis"))
      a = CS_PARTITION_METIS;
    else if (!strcmp(part_name, "block"))
      a = CS_PARTITION_BLOCK;
    BFT_FREE(part_name);
  }

  BFT_FREE(path);

  /* Rank step */

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3,
                        "calculation_management",
                        "partitioning",
                        "rank_step");
  cs_xpath_add_function_text(&path);
  cs_gui_get_int(path, &rank_step);

  BFT_FREE(path);

  /* Ignore periodicity option */

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3,
                        "calculation_management",
                        "partitioning",
                        "ignore_periodicity");
  cs_xpath_add_attribute(&path, "status");
  s_perio = cs_gui_get_attribute_value(path);
  if (s_perio != NULL) {
    if (cs_gui_strcmp(s_perio, "on"))
      ignore_perio = true;
    BFT_FREE(s_perio);
  }

  BFT_FREE(path);

  /* Output option */

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3,
                        "calculation_management", "partitioning", "output");
  cs_xpath_add_function_text(&path);

  s_output = cs_gui_get_text_value(path);

  if (s_output != NULL) {
    if (!strcmp(s_output, "no"))
      write_level = 0;
    else if (!strcmp(s_output, "default"))
      write_level = 1;
    else if (!strcmp(s_output, "yes"))
      write_level = 2;
    BFT_FREE(s_output);
  }

  BFT_FREE(path);

  /* List of partitions to output */

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3,
                        "calculation_management",
                        "partitioning",
                        "partition_list");
  cs_xpath_add_function_text(&path);

  s_list = cs_gui_get_text_value(path);

  if (s_list != NULL) {
    char *p = strtok(s_list, " \t,;");
    while (p != NULL) {
      int np = atoi(p);
      if (np > 1) {
        BFT_REALLOC(add_parts, n_add_parts + 1, int);
        add_parts[n_add_parts] = np;
        n_add_parts += 1;
      }
      p = strtok(NULL, " \t,;");
    }
    BFT_FREE(s_list);
  }

  BFT_FREE(path);

  /* Set options */

  cs_partition_set_algorithm
    (CS_PARTITION_MAIN,
     a,
     rank_step,
     ignore_perio);

  cs_partition_set_write_level(write_level);

  if (n_add_parts > 0) {
    cs_partition_add_partitions(n_add_parts, add_parts);
    BFT_FREE(add_parts);
  }
}

/*-----------------------------------------------------------------------------
 * Get initial value from property markup.
 *
 * parameters:
 *   property_name      <--   name of the property
 *   value              -->   new initial value of the property
 *----------------------------------------------------------------------------*/

void
cs_gui_properties_value(const char  *property_name,
                        double      *value)
{
  char   *path = NULL;
  double  result;

  path = cs_xpath_short_path();
  cs_xpath_add_element(&path, "property");
  cs_xpath_add_test_attribute(&path, "name", property_name);
  cs_xpath_add_element(&path, "initial_value");
  cs_xpath_add_function_text(&path);

  if (cs_gui_get_double(path, &result))
    *value = result;

  BFT_FREE(path);
}

/*-----------------------------------------------------------------------------
 * Initialization choice of the reference variables parameters.
 *
 * parameters:
 *   name            <--   parameter name
 *   value           -->   parameter value
 *----------------------------------------------------------------------------*/

void
cs_gui_reference_initialization(const char  *param,
                                double      *value)
{
  char *path = NULL;
  double  result;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 3,
                        "thermophysical_models",
                        "reference_values",
                        param);
  cs_xpath_add_function_text(&path);

  if (cs_gui_get_double(path, &result))
    *value = result;
  BFT_FREE(path);
}

/*----------------------------------------------------------------------------
 * Get thermal scalar model.
 *
 * return:
 *   value of itherm*10 + (temperature variant flag)
 *----------------------------------------------------------------------------*/

int
cs_gui_thermal_model(void)
{
  char *model_name = NULL;
  int   test = 0;

  model_name = cs_gui_get_thermophysical_model("thermal_scalar");

  if (cs_gui_strcmp(model_name, "off"))
    test = 0;
  else {
    if (cs_gui_strcmp(model_name, "enthalpy"))
      test = 20;
    else if (cs_gui_strcmp(model_name, "temperature_kelvin"))
      test = 11;
    else if (cs_gui_strcmp(model_name, "temperature_celsius"))
      test = 10;
    else if (cs_gui_strcmp(model_name, "potential_temperature"))
      test = 12;
    else if (cs_gui_strcmp(model_name, "liquid_potential_temperature"))
      test = 13;
    else if (cs_gui_strcmp(model_name, "total_energy"))
      test = 30;
    else
      bft_error(__FILE__, __LINE__, 0,
          _("Invalid thermal model: %s\n"), model_name);
  }

  BFT_FREE(model_name);

  return test;
}

/*----------------------------------------------------------------------------
 * Time moments definition
 *----------------------------------------------------------------------------*/

void
cs_gui_time_moments(void)
{
  if (!cs_gui_file_is_loaded())
    return;

  int imom = 0;
  int isuite = cs_restart_present();

  int ntimaver
    = cs_gui_get_tag_count("/analysis_control/time_averages/time_average", 1);

  /* for each average */
  for (int i = 0; i < ntimaver; i++) {

    imom = i + 1;

    const char *restart_name;
    cs_time_moment_restart_t  restart_mode = CS_TIME_MOMENT_RESTART_AUTO;
    int nt_start = 0, restart_id = 0;
    double t_start = -1;

    char *m_name = _get_time_average_label(i);

    _get_time_average_data(imom, "time_step_start", &nt_start);
    _get_time_average_time_start(imom, "time_start", &t_start);

    /* test on restart */

    if (isuite != 0) {
      restart_id = -2;
      _get_time_average_data(imom, "restart_from_time_average", &restart_id);
      cs_time_moment_restart_options_by_id(restart_id,
                                           &restart_mode,
                                           &restart_name);
    }

    int n_m_fields = _get_time_average_n_variables(imom);
    int *m_f_id, *m_c_id;

    BFT_MALLOC(m_f_id, n_m_fields*2, int);
    m_c_id = m_f_id + n_m_fields;

    for (int j = 0; j < n_m_fields; j++) {

      char *f_name = _get_time_average_variable_name(imom, j + 1);
      int idim = _get_time_average_component(imom, j + 1);

      cs_field_t *f = cs_field_by_name_try(f_name);

      m_f_id[j] = f->id;
      m_c_id[j] = idim;

      BFT_FREE(f_name);
    }

    cs_time_moment_define_by_field_ids(m_name,
                                       n_m_fields,
                                       m_f_id,
                                       m_c_id,
                                       CS_TIME_MOMENT_MEAN,
                                       nt_start,
                                       t_start,
                                       restart_mode,
                                       restart_name);

    m_c_id = NULL;
    BFT_FREE(m_f_id);
    BFT_FREE(m_name);

  }
#if _XML_DEBUG_
  bft_printf("==>UIMOYT\n");
#endif
}

/*-----------------------------------------------------------------------------
 * Set turbomachinery model
 *----------------------------------------------------------------------------*/

void
cs_gui_turbomachinery(void)
{
  char *path = NULL;
  char *model = NULL;

  if (!cs_gui_file_is_loaded())
    return;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 2,
                        "thermophysical_models",
                        "turbomachinery");
  cs_xpath_add_attribute(&path, "model");
  model = cs_gui_get_attribute_value(path);

  BFT_FREE(path);

  if (cs_gui_strcmp(model, "off"))
    cs_turbomachinery_set_model(CS_TURBOMACHINERY_NONE);
  else if (cs_gui_strcmp(model, "transient"))
    cs_turbomachinery_set_model(CS_TURBOMACHINERY_TRANSIENT);
  else if (cs_gui_strcmp(model, "frozen"))
    cs_turbomachinery_set_model(CS_TURBOMACHINERY_FROZEN);
  else
    cs_turbomachinery_set_model(CS_TURBOMACHINERY_NONE);

  BFT_FREE(model);

}

/*-----------------------------------------------------------------------------
 * Set turbomachinery options.
 *----------------------------------------------------------------------------*/

void
cs_gui_turbomachinery_rotor(void)
{
  if (!cs_gui_file_is_loaded())
    return;

  char *path = NULL;

  double rotation_axis[3];
  double rotation_invariant[3];
  double rotation_velocity;
  char *cell_criteria;
  char *model;

  path = cs_xpath_init_path();
  cs_xpath_add_elements(&path, 2,
                        "thermophysical_models",
                        "turbomachinery");
  cs_xpath_add_attribute(&path, "model");
  model = cs_gui_get_attribute_value(path);

  BFT_FREE(path);

  if (!cs_gui_strcmp(model, "off")) {

    int n_rotors
      = cs_gui_get_tag_count("/thermophysical_models/turbomachinery/rotor\n", 1);

    for (int rotor_id = 0; rotor_id < n_rotors; rotor_id++) {

      rotation_axis[0] = _rotor_option(rotor_id, "axis_x");
      rotation_axis[1] = _rotor_option(rotor_id, "axis_y");
      rotation_axis[2] = _rotor_option(rotor_id, "axis_z");

      rotation_invariant[0] = _rotor_option(rotor_id, "invariant_x");
      rotation_invariant[1] = _rotor_option(rotor_id, "invariant_y");
      rotation_invariant[2] = _rotor_option(rotor_id, "invariant_z");

      path = cs_xpath_init_path();
      cs_xpath_add_elements(&path, 2,
                            "thermophysical_models",
                            "turbomachinery");

      cs_xpath_add_element_num(&path, "rotor", rotor_id + 1);
      cs_xpath_add_element(&path, "velocity");
      cs_xpath_add_element(&path, "value");
      cs_xpath_add_function_text(&path);
      cs_gui_get_double(path, &rotation_velocity);
      BFT_FREE(path);

      path = cs_xpath_init_path();
      cs_xpath_add_elements(&path, 2,
                            "thermophysical_models",
                            "turbomachinery");

      cs_xpath_add_element_num(&path, "rotor", rotor_id + 1);
      cs_xpath_add_element(&path, "criteria");
      cs_xpath_add_function_text(&path);
      cell_criteria = cs_gui_get_text_value(path);
      BFT_FREE(path);

      cs_turbomachinery_add_rotor(cell_criteria,
                                  rotation_velocity,
                                  rotation_axis,
                                  rotation_invariant);

      BFT_FREE(cell_criteria);
    }

    int n_join = 0;
    n_join = cs_gui_get_tag_count
                 ("/thermophysical_models/turbomachinery/joining/face_joining", 1);

    if (n_join != 0) {
      for (int join_id = 0; join_id < n_join; join_id++) {

        char *selector_s  =  _get_rotor_face_joining("selector", join_id+1);
        char *fraction_s  =  _get_rotor_face_joining("fraction", join_id+1);
        char *plane_s     =  _get_rotor_face_joining("plane", join_id+1);
        char *verbosity_s =  _get_rotor_face_joining("verbosity", join_id+1);
        char *visu_s      =  _get_rotor_face_joining("visualization", join_id+1);

        double fraction = (fraction_s != NULL) ? atof(fraction_s) : 0.1;
        double plane = (plane_s != NULL) ? atof(plane_s) : 25.0;
        int verbosity = (verbosity_s != NULL) ? atoi(verbosity_s) : 0;
        int visualization = (visu_s != NULL) ? atoi(visu_s) : 1;

        BFT_FREE(visu_s);
        BFT_FREE(verbosity_s);
        BFT_FREE(plane_s);
        BFT_FREE(fraction_s);

        (void) cs_turbomachinery_join_add(selector_s,
                                          fraction,
                                          plane,
                                          verbosity,
                                          visualization);

        BFT_FREE(selector_s);

      }
    }
  }

  BFT_FREE(model);
}

/*----------------------------------------------------------------------------
 * Logging output for MEI usage.
 *----------------------------------------------------------------------------*/

void
cs_gui_usage_log(void)
{
  double mei_wtime = cs_gui_get_mei_times();

#if defined(HAVE_MPI)

  if (cs_glob_n_ranks > 1) {
    double _wtime_loc = mei_wtime;
    MPI_Allreduce(&_wtime_loc, &mei_wtime, 1, MPI_DOUBLE, MPI_MAX,
                   cs_glob_mpi_comm);
  }

#endif

  if (mei_wtime > 0.0) {
    cs_log_printf(CS_LOG_PERFORMANCE,
                  _("\nTime elapsed defining values using MEI: %12.5f\n"),
                  mei_wtime);
    cs_log_printf(CS_LOG_PERFORMANCE, "\n");
    cs_log_separator(CS_LOG_PERFORMANCE);
  }
}

/*----------------------------------------------------------------------------
 * Set GUI-defined user scalar labels.
 *----------------------------------------------------------------------------*/

void
cs_gui_user_scalar_labels(void)
{
  const int keylbl = cs_field_key_id("label");

  int n_user_scalars = cs_gui_get_tag_count("/additional_scalars/variable", 1);

  for (int i = 0; i < n_user_scalars; i++) {
    char *label = _scalar_name_label("label", i+1);
    char *name = _scalar_name_label("name", i+1);
    cs_field_t *f = cs_field_by_name_try(name);
    if (f != NULL)
      cs_field_set_key_str(f, keylbl, label);
    BFT_FREE(label);
    BFT_FREE(name);
  }
}
/*----------------------------------------------------------------------------
 * Define user variables through the GUI.
 *----------------------------------------------------------------------------*/

void
cs_gui_user_variables(void)
{
  int n_user_scalars = cs_gui_get_tag_count("/additional_scalars/variable", 1);

  const int itherm = cs_glob_thermal_model->itherm;
  const int var_start_id = (itherm != 0) ? 0 : 1;
  const int var_end_id = n_user_scalars+1;

#if _XML_DEBUG_
  bft_printf("==> cs_gui_user_variables\n");
  bft_printf("--number of user scalars: %i\n", n_user_scalars);
#endif

  for (int i = 0; i < n_user_scalars; i++) {

    /* Names are equivalent to labels for initial definition of user fields */

    char *name = _scalar_name_label("name", i+1);

#if _XML_DEBUG_
    bft_printf("--name of scalar[%i]: %s\n", i, name);
#endif

    char *variance_name = _scalar_variance(name);

    /* In case of variance, search for matching field */

    if (variance_name != NULL) {

      /* Search in thermal and user scalars */

      for (int j = var_start_id; j < var_end_id; j++) {
        char *cmp_name;
        if (j == 0)
          cmp_name = _thermal_scalar_name_label("name");
        else
          cmp_name = _scalar_name_label("name", j);

        if (strcmp(cmp_name, variance_name) == 0) {
          cs_parameters_add_variable_variance(name, variance_name);
          BFT_FREE(cmp_name);
          BFT_FREE(variance_name);
          break;
        }
        else
          BFT_FREE(cmp_name);
      }

    }

    /* If not a variance, we have a regular variable */

    else
      cs_parameters_add_variable(name, 1);

    BFT_FREE(name);

  }
}

/*----------------------------------------------------------------------------*/

END_C_DECLS
