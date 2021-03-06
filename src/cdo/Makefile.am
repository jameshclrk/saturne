## Process this file with automake to produce Makefile.in

#-------------------------------------------------------------------------------

# This file is part of Code_Saturne, a general-purpose CFD tool.
#
# Copyright (C) 1998-2016 EDF S.A.
#
# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation; either version 2 of the License, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along with
# this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
# Street, Fifth Floor, Boston, MA 02110-1301, USA.

#-------------------------------------------------------------------------------

# Main part

AM_CPPFLAGS = \
-I$(top_srcdir)/src/alge \
-I$(top_srcdir)/src/bft \
-I$(top_srcdir)/src/fvm \
-I$(top_srcdir)/src/base \
-I$(top_srcdir)/src/mesh \
$(PETSC_CPPFLAGS) $(MPI_CPPFLAGS)

AM_CFLAGS = $(CFLAGS_DBG) $(CFLAGS_OPT)

AM_LDFLAGS =

# Public header files (to be installed)

pkginclude_HEADERS = \
cs_advection_field.h \
cs_cdo.h \
cs_cdo_advection.h \
cs_cdo_bc.h \
cs_cdo_connect.h \
cs_cdo_diffusion.h \
cs_cdo_local.h \
cs_cdo_main.h \
cs_cdo_quantities.h \
cs_cdo_scheme_geometry.h \
cs_cdo_toolbox.h \
cs_cdofb_scaleq.h \
cs_cdovb_scaleq.h \
cs_cdovcb_scaleq.h \
cs_domain.h \
cs_equation.h \
cs_equation_common.h \
cs_equation_param.h \
cs_evaluate.h \
cs_groundwater.h \
cs_hodge.h \
cs_param.h \
cs_property.h \
cs_quadrature.h \
cs_reco.h \
cs_sla.h \
cs_source_term.h \
cs_walldistance.h

# Library source files

noinst_LTLIBRARIES = libcscdo.la
libcscdo_la_SOURCES = \
cs_advection_field.c \
cs_cdo.c \
cs_cdo_advection.c \
cs_cdo_bc.c \
cs_cdo_connect.c \
cs_cdo_diffusion.c \
cs_cdo_local.c \
cs_cdo_main.c \
cs_cdo_quantities.c \
cs_cdo_scheme_geometry.c \
cs_cdo_toolbox.c \
cs_cdofb_scaleq.c \
cs_cdovb_scaleq.c \
cs_cdovcb_scaleq.c \
cs_domain.c \
cs_equation.c \
cs_equation_common.c \
cs_equation_param.c \
cs_evaluate.c \
cs_groundwater.c \
cs_hodge.c \
cs_param.c \
cs_property.c \
cs_quadrature.c \
cs_reco.c \
cs_sla.c \
cs_source_term.c \
cs_walldistance.c

libcscdo_la_LDFLAGS = -no-undefined

clean-local:
	-rm -f *__genmod.f90 *.mod
