!-------------------------------------------------------------------------------

! This file is part of Code_Saturne, a general-purpose CFD tool.
!
! Copyright (C) 1998-2016 EDF S.A.
!
! This program is free software; you can redistribute it and/or modify it under
! the terms of the GNU General Public License as published by the Free Software
! Foundation; either version 2 of the License, or (at your option) any later
! version.
!
! This program is distributed in the hope that it will be useful, but WITHOUT
! ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
! FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
! details.
!
! You should have received a copy of the GNU General Public License along with
! this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
! Street, Fifth Floor, Boston, MA 02110-1301, USA.

!-------------------------------------------------------------------------------

!===============================================================================
! Function:
! ---------

!> \file resrij2.f90
!>
!> \brief This subroutine performs the solving of the coupled Reynolds stress
!> components in \f$ R_{ij} - \varepsilon \f$ RANS (LRR) turbulence model.
!>
!> \remark
!> - cvar_var(1,*) for \f$ R_{11} \f$
!> - cvar_var(2,*) for \f$ R_{22} \f$
!> - cvar_var(3,*) for \f$ R_{33} \f$
!> - cvar_var(4,*) for \f$ R_{12} \f$
!> - cvar_var(5,*) for \f$ R_{23} \f$
!> - cvar_var(6,*) for \f$ R_{13} \f$
!-------------------------------------------------------------------------------

!-------------------------------------------------------------------------------
! Arguments
!______________________________________________________________________________.
!  mode           name          role
!______________________________________________________________________________!
!> \param[in]     nvar          total number of variables
!> \param[in]     nscal         total number of scalars
!> \param[in]     ncepdp        number of cells with head loss
!> \param[in]     ncesmp        number of cells with mass source term
!> \param[in]     ivar          variable number
!> \param[in]     icepdc        index of cells with head loss
!> \param[in]     icetsm        index of cells with mass source term
!> \param[in]     itypsm        type of mass source term for each variable
!>                               (see \ref cs_user_mass_source_terms)
!> \param[in]     dt            time step (per cell)
!> \param[in]     produc        work array for production
!> \param[in]     gradro        work array for grad rom
!>                              (without rho volume) only for iturb=30
!> \param[in]     ckupdc        work array for the head loss
!> \param[in]     smacel        value associated to each variable in the mass
!>                               source terms or mass rate (see \ref cs_user_mass_source_terms)
!> \param[in]     viscf         visc*surface/dist at internal faces
!> \param[in]     viscb         visc*surface/dist at edge faces
!> \param[in]     tslage        explicit source terms for the Lagrangian module
!> \param[in]     tslagi        implicit source terms for the Lagrangian module
!> \param[in]     smbr          working array
!> \param[in]     rovsdt        working array
!______________________________________________________________________________!

subroutine resrij2 &
 ( nvar   , nscal  , ncepdp , ncesmp ,                            &
   ivar   ,                                                       &
   icepdc , icetsm , itypsm ,                                     &
   dt     ,                                                       &
   produc , gradro ,                                              &
   ckupdc , smacel ,                                              &
   viscf  , viscb  ,                                              &
   tslage , tslagi ,                                              &
   smbr   , rovsdt )

!===============================================================================

!===============================================================================
! Module files
!===============================================================================

use paramx
use numvar
use entsor
use optcal
use cstphy
use cstnum
use parall
use period
use lagran
use mesh
use field
use cs_f_interfaces
use rotation
use turbomachinery
use cs_c_bindings

!===============================================================================

implicit none

! Arguments

integer          nvar   , nscal
integer          ncepdp , ncesmp
integer          ivar

integer          icepdc(ncepdp)
integer          icetsm(ncesmp), itypsm(ncesmp,nvar)

double precision dt(ncelet)
double precision produc(6,ncelet)
double precision gradro(3,ncelet)
double precision ckupdc(ncepdp,6), smacel(ncesmp,nvar)
double precision viscf(nfac), viscb(nfabor)
double precision tslage(6,ncelet),tslagi(6,ncelet)
double precision smbr(6,ncelet), rovsdt(6,6,ncelet)

! Local variables

integer          iel
integer          isou, jsou
integer          ii    , jj    , kk    , iiun
integer          iflmas, iflmab
integer          nswrgp, imligp, iwarnp
integer          iconvp, idiffp, ndircp
integer          nswrsp, ircflp, ischcp, isstpp
integer          st_prv_id
integer          isoluc
integer          idftnp, iswdyp
integer          icvflb
integer          ivoid(1)
integer          dimrij

double precision blencp, epsilp, epsrgp, climgp, extrap, relaxp
double precision epsrsp
double precision trprod, trrij , deltij(6)
double precision tuexpr, thets , thetv , thetp1
double precision d1s3  , d2s3
double precision ccorio, matrot(3,3)
double precision rctse

character(len=80) :: label
double precision, allocatable, dimension(:) :: w1
double precision, allocatable, dimension(:) :: w8
double precision, allocatable, dimension(:,:) :: w7
double precision, allocatable, dimension(:) :: dpvar
double precision, allocatable, dimension(:,:) :: viscce
double precision, allocatable, dimension(:,:) :: weighf
double precision, allocatable, dimension(:) :: weighb
double precision, dimension(:), pointer :: imasfl, bmasfl
double precision, dimension(:), pointer :: crom, cromo
double precision, dimension(:,:), pointer :: coefap, cofafp
double precision, dimension(:,:,:), pointer :: coefbp, cofbfp
double precision, dimension(:,:), pointer :: visten
double precision, dimension(:), pointer :: cvara_ep
double precision, dimension(:,:), pointer :: cvar_var, cvara_var
double precision, allocatable, dimension(:,:) :: cvara_r
double precision, dimension(:,:), pointer :: c_st_prv
double precision, dimension(:), pointer :: viscl

!===============================================================================

!===============================================================================
! 1. Initialization
!===============================================================================

! Allocate work arrays
allocate(w1(ncelet))
allocate(w7(6,ncelet))
allocate(w8(ncelet))
allocate(dpvar(ncelet))
allocate(viscce(6,ncelet))
allocate(weighf(2,nfac))
allocate(weighb(nfabor))

if (iwarni(ivar).ge.1) then
  call field_get_label(ivarfl(ivar), label)
  write(nfecra,1000) label
endif

call field_get_val_s(icrom, crom)
call field_get_val_s(iprpfl(iviscl), viscl)
call field_get_key_int(ivarfl(iu), kimasf, iflmas)
call field_get_key_int(ivarfl(iu), kbmasf, iflmab)
call field_get_val_s(iflmas, imasfl)
call field_get_val_s(iflmab, bmasfl)

call field_get_val_prev_s(ivarfl(iep), cvara_ep)


call field_get_val_v(ivarfl(ivar), cvar_var)
call field_get_val_prev_v(ivarfl(ivar), cvara_var)
call field_get_dim(ivarfl(ivar),dimrij)! dimension of Rij

call field_get_coefa_v(ivarfl(ivar), coefap)
call field_get_coefb_v(ivarfl(ivar), coefbp)
call field_get_coefaf_v(ivarfl(ivar), cofafp)
call field_get_coefbf_v(ivarfl(ivar), cofbfp)

do isou = 1, 6
  deltij(isou) = 1.0d0
  if (isou.gt.3) then
    deltij(isou) = 0.0d0
  endif
enddo
d1s3 = 1.d0/3.d0
d2s3 = 2.d0/3.d0

!     S as Source, V as Variable
thets  = thetst
thetv  = thetav(ivar)

call field_get_key_int(ivarfl(ivar), kstprv, st_prv_id)
if (st_prv_id.ge.0) then
  call field_get_val_v(st_prv_id, c_st_prv)
else
  c_st_prv=> null()
endif

if (st_prv_id.ge.0.and.iroext.gt.0) then
  call field_get_val_prev_s(icrom, cromo)
else
  call field_get_val_s(icrom, cromo)
endif

do iel = 1, ncel
  do isou = 1 ,6
    smbr(isou,iel) = 0.d0
  enddo
enddo
do iel = 1, ncel
  do isou = 1, 6
    do jsou = 1, 6
      rovsdt(isou,jsou,iel) = 0.d0
    enddo
  enddo
enddo

! Coefficient of the "Coriolis-type" term
if (icorio.eq.1) then
  ! Relative velocity formulation
  ccorio = 2.d0
elseif (iturbo.eq.1) then
  ! Mixed relative/absolute velocity formulation
  ccorio = 1.d0
else
  ccorio = 0.d0
endif

!===============================================================================
! 2. User source terms
!===============================================================================

call cs_user_turbulence_source_terms2 &
!===================================
 ( nvar   , nscal  , ncepdp , ncesmp ,                            &
   ivarfl(ivar)    ,                                              &
   icepdc , icetsm , itypsm ,                                     &
   ckupdc , smacel ,                                              &
   smbr   , rovsdt )

  !     If we extrapolate the source terms
do isou = 1, dimrij
  if (st_prv_id.ge.0) then
    do iel = 1, ncel
      !       Save for exchange
      tuexpr = c_st_prv(isou,iel)
      !       For continuation and the next time step
      c_st_prv(isou,iel) = smbr(isou,iel)
      !       Second member of the previous time step
      !       We suppose -rovsdt > 0: we implicite
      !          the user source term (the rest)
      smbr(isou,iel) = rovsdt(isou,isou,iel)*cvara_var(isou,iel)  - thets*tuexpr
      !       Diagonal
      rovsdt(isou,isou,iel) = - thetv*rovsdt(isou,isou,iel)
    enddo
  else
    do iel = 1, ncel
      smbr(isou,iel)   = rovsdt(isou,isou,iel)*cvara_var(isou,iel) + smbr(isou,iel)
      rovsdt(isou,isou,iel) = max(-rovsdt(isou,isou,iel),zero)
    enddo
  endif
enddo

!===============================================================================
! 3. Lagrangian source terms
!===============================================================================

!     2nd order is not taken into account
do isou = 1, dimrij
        if (iilagr.eq.2 .and. ltsdyn.eq.1) then
    do iel = 1,ncel
      smbr(isou, iel)   = smbr(isou, iel)   + tslage(isou,iel)
      rovsdt(isou,isou,iel) = rovsdt(isou,isou, iel) + max(-tslagi(isou, iel),zero)
    enddo
  endif
enddo

!===============================================================================
! 4. Mass source term
!===============================================================================

do isou = 1, dimrij
  if (ncesmp.gt.0) then

    !       Integer equal to 1 (for navsto: nb of sur-iter)
    iiun = 1

    ! We increment smbr with -Gamma.var_prev and rovsdr with Gamma
    call catsmt &
    !==========
   ( ncelet , ncel   , ncesmp , iiun     , isto2t ,                   &
     icetsm , itypsm(:,ivar + isou - 1)  ,                            &
     cell_f_vol , cvara_var  , smacel(:,ivar+isou-1)  , smacel(:,ipr) ,   &
     smbr   ,  rovsdt    , w1 )

    ! If we extrapolate the source terms we put Gamma Pinj in the previous st
    if (st_prv_id.ge.0) then
      do iel = 1, ncel
        c_st_prv(isou,iel) = c_st_prv(isou,iel) + w1(iel)
      enddo
    ! Otherwise we put it directly in the RHS
    else
      do iel = 1, ncel
        smbr(isou, iel) = smbr(isou, iel) + w1(iel)
      enddo
    endif

  endif
enddo

!===============================================================================
! 5. Unsteady term
!===============================================================================

! ---> Added in the matrix diagonal

do isou = 1, dimrij
  do iel=1,ncel
    rovsdt(isou,isou,iel) = rovsdt(isou, isou,iel)                                       &
              + istat(ivar+isou-1)*(crom(iel)/dt(iel))*cell_f_vol(iel)
  enddo
enddo


!===============================================================================
! 6. Production, Pressure-Strain correlation, dissipation
!===============================================================================

! ---> Calculation of k for the sub-routine continuation
!       we use a work array
do iel = 1, ncel
  w8(iel) = 0.5d0 * (cvara_var(1,iel) + cvara_var(2,iel) + cvara_var(3,iel))
enddo

! ---> Source term

!      (1-CRIJ2) Pij (for all components of Rij)

!      DELTAIJ*(2/3.CRIJ2.P+2/3.CRIJ1.EPSILON)
!                    (diagonal terms for R11, R22 et R33)

!      -DELTAIJ*2/3*EPSILON

!     If we extrapolate the source terms
!     We modify the implicit part:
!     In PHI1, we will only take rho CRIJ1 epsilon/k and not
!                                rho CRIJ1 epsilon/k (1-2/3 DELTAIJ)
!     It allow to keep  k^n instead of (R11^(n+1)+R22^n+R33^n)
!     This choice is questionable. It is the solution isoluc = 1
!     If we want to take all as implicit (like it is done in
!     standard first order), it is the solution isoluc = 2
!     -> to  be tested more precisely if necessary


!     If we extrapolate the source terms
if (st_prv_id.ge.0) then

  isoluc = 1

  do iel = 1, ncel

    !     Half-traces of Prod and R
    trprod = 0.5d0*(produc(1,iel)+produc(2,iel)+produc(3,iel))
    trrij  = w8(iel)

    do isou = 1, 6
      !     Calculation of Prod+Phi1+Phi2-Eps
      !       = rhoPij-C1rho eps/k(Rij-2/3k dij)-C2rho(Pij-1/3Pkk dij)-2/3rho eps dij
      !       In c_st_prv:
      !       = rhoPij-C1rho eps/k(   -2/3k dij)-C2rho(Pij-1/3Pkk dij)-2/3rho eps dij
      !       = rho{2/3dij[C2 Pkk/2+(C1-1)eps)]+(1-C2)Pij           }
      c_st_prv(isou,iel) = c_st_prv(isou,iel) + cromo(iel) * cell_f_vol(iel) &
        *(   deltij(isou)*d2s3*                                           &
             (  crij2*trprod                                        &
              +(crij1-1.d0)* cvara_ep(iel)  )                       &
           +(1.0d0-crij2)*produc(isou,iel)               )
      !       In smbr
      !       =       -C1rho eps/k(Rij         )
      !       = rho{                                     -C1eps/kRij}
      smbr(isou,iel) = smbr(isou,iel) + crom(iel) * cell_f_vol(iel)               &
        *( -crij1*cvara_ep(iel)/trrij * cvara_var(isou,iel) )

      !     Calculation of the implicit part coming from Phil
      !       = C1rho eps/k(1        )
      rovsdt(isou,isou,iel) = rovsdt(isou,isou,iel) + crom(iel) * cell_f_vol(iel)           &
                              *crij1*cvara_ep(iel)/trrij*thetv
    enddo
  enddo

  !     If we want to implicit a part of -C1rho eps/k(   -2/3k dij)
  if (isoluc.eq.2) then

    do iel = 1, ncel

      trrij  = w8(iel)
      do isou = 1, 6
        !    We remove of cromo
        !       =       -C1rho eps/k(   -1/3Rij dij)
        c_st_prv(isou,iel) = c_st_prv(isou,iel) - cromo(iel) * cell_f_vol(iel)    &
        *(deltij(isou)*d1s3*crij1*cvara_ep(iel)/trrij * cvara_var(isou,iel))
        !    We add to smbr (with crom)
        !       =       -C1rho eps/k(   -1/3Rij dij)
        smbr(isou,iel)      = smbr(isou,iel)                       &
                            + crom(iel) * cell_f_vol(iel)          &
        *(deltij(isou)*d1s3*crij1*cvara_ep(iel)/trrij * cvara_var(isou,iel))
        !    We add to rovsdt (woth crom)
        !       =        C1rho eps/k(   -1/3    dij)
        rovsdt(isou,isou,iel) = rovsdt(isou,isou,iel) + crom(iel) * cell_f_vol(iel)         &
        *(deltij(isou)*d1s3*crij1*cvara_ep(iel)/trrij                 )
      enddo
    enddo

  endif

! If we do not extrapolate the source terms
else

  do iel = 1, ncel

    !     Half-traces of Prod and R
    trprod = 0.5d0*(produc(1,iel)+produc(2,iel)+produc(3,iel))
    trrij  = w8(iel)

    do isou = 1, 6
      !     Calculation of Prod+Phi1+Phi2-Eps
      !       = rhoPij-C1rho eps/k(Rij-2/3k dij)-C2rho(Pij-1/3Pkk dij)-2/3rho eps dij
      !       = rho{2/3dij[C2 Pkk/2+(C1-1)eps)]+(1-C2)Pij-C1eps/kRij}
      smbr(isou,iel) = smbr(isou,iel) + crom(iel) * cell_f_vol(iel) &
        *(   deltij(isou)*d2s3*                                           &
             (  crij2*trprod                                        &
              +(crij1-1.d0)* cvara_ep(iel)  )                       &
           +(1.0d0-crij2)*produc(isou,iel)                          &
           -crij1*cvara_ep(iel)/trrij * cvara_var(isou,iel)  )

      !     Calculation of the implicit part coming from Phi1
      !       = C1rho eps/k(1-1/3 dij)
      rovsdt(isou,isou,iel) = rovsdt(isou,isou,iel) + crom(iel) * cell_f_vol(iel)           &
           *(1.d0-d1s3*deltij(isou))*crij1*cvara_ep(iel)/trrij
    enddo
  enddo

endif

!===============================================================================
! 6-bis. Coriolis terms in the Phi1 and production
!===============================================================================

if (icorio.eq.1 .or. iturbo.eq.1) then
  allocate(cvara_r(3,3))
  do iel = 1, ncel
    do isou = 1, 6
      w7(isou,iel) = 0.d0
    enddo
  enddo

  do iel = 1, ncel
      cvara_r(1,1) = cvara_var(1,iel)
      cvara_r(2,2) = cvara_var(2,iel)
      cvara_r(3,3) = cvara_var(3,iel)
      cvara_r(1,2) = cvara_var(4,iel)
      cvara_r(2,3) = cvara_var(5,iel)
      cvara_r(1,3) = cvara_var(6,iel)
      cvara_r(2,1) = cvara_var(4,iel)
      cvara_r(3,2) = cvara_var(5,iel)
      cvara_r(3,1) = cvara_var(6,iel)
  ! Compute Gij: (i,j) component of the Coriolis production
    do isou = 1, 6
      if (isou.eq.1) then
        ii = 1
        jj = 1
      else if (isou.eq.2) then
        ii = 2
        jj = 2
      else if (isou.eq.3) then
        ii = 3
        jj = 3
      else if (isou.eq.4) then
        ii = 1
        jj = 2
      else if (isou.eq.5) then
        ii = 2
        jj = 3
      else if (isou.eq.6) then
        ii = 1
        jj = 3
      end if
      do kk = 1, 3

        call coriolis_t(irotce(iel), 1.d0, matrot)

        w7(isou,iel) = w7(isou,iel) - ccorio*(  matrot(ii,kk)*cvara_r(jj,kk) &
                                    + matrot(jj,kk)*cvara_r(ii,kk) )
      enddo
    enddo
  enddo

  ! Coriolis contribution in the Phi1 term: (1-C2/2)Gij
  if (icorio.eq.1) then
    do iel = 1, ncel
      do isou = 1, 6
        w7(isou,iel) = crom(iel)*cell_f_vol(iel)*(1.d0 - 0.5d0*crij2)*w7(isou,iel)
      enddo
    enddo
  endif

  ! If source terms are extrapolated
  if (st_prv_id.ge.0) then
    do iel = 1, ncel
      do isou = 1, 6
        c_st_prv(isou,iel) = c_st_prv(isou,iel) + w7(isou,iel)
      enddo
    enddo
  ! Otherwise, directly in smbr
  else
    do iel = 1, ncel
      do isou = 1, 6
        smbr(isou,iel) = smbr(isou,iel) + w7(isou,iel)
      enddo
    enddo
  endif

endif

!===============================================================================
! 7. Wall echo terms
!===============================================================================

if (irijec.eq.1) then !todo

  do iel = 1, ncel
    do isou = 1, 6
      w7(isou,iel) = 0.d0
    enddo
  enddo

  call rijech2(produc, w7)

  ! If we extrapolate the source terms: c_st_prv
  if (st_prv_id.ge.0) then
    do iel = 1, ncel
      do isou = 1, 6
        c_st_prv(isou,iel) = c_st_prv(isou,iel) + w7(isou,iel)
      enddo
    enddo
  ! Otherwise smbr
  else
    do iel = 1, ncel
      do isou = 1, 6
        smbr(isou,iel) = smbr(isou,iel) + w7(isou,iel)
      enddo
    enddo
  endif

endif


!===============================================================================
! 8. Buoyancy source term
!===============================================================================

if (igrari.eq.1) then

  do iel = 1, ncel
    do isou = 1, dimrij
      w7(isou,iel) = 0.d0
    enddo
  enddo

  call rijthe2(nscal, gradro, w7)

  do isou = 1, dimrij
    ! If we extrapolate the source terms: previous ST
    if (st_prv_id.ge.0) then
      do iel = 1, ncel
        c_st_prv(isou,iel) = c_st_prv(isou,iel) + w7(isou,iel)
      enddo
    ! Otherwise smbr
    else
      do iel = 1, ncel
        smbr(isou,iel) = smbr(isou,iel) + w7(isou,iel)
      enddo
    endif
  enddo


endif

!===============================================================================
! 9. Diffusion term (Daly Harlow: generalized gradient hypothesis method)
!===============================================================================

! Symmetric tensor diffusivity (GGDH)
if (idften(ivar).eq.6) then

  call field_get_val_v(ivsten, visten)

  do iel = 1, ncel
    viscce(1,iel) = visten(1,iel) + viscl(iel)
    viscce(2,iel) = visten(2,iel) + viscl(iel)
    viscce(3,iel) = visten(3,iel) + viscl(iel)
    viscce(4,iel) = visten(4,iel)
    viscce(5,iel) = visten(5,iel)
    viscce(6,iel) = visten(6,iel)
  enddo

  iwarnp = iwarni(ivar)

  call vitens &
 ( viscce , iwarnp ,             &
   weighf , weighb ,             &
   viscf  , viscb  )

! Scalar diffusivity
else

  do iel = 1, ncel
    trrij = 0.5d0 * (cvara_var(1,iel) + cvara_var(2,iel) + cvara_var(3,iel))
    rctse = crom(iel) * csrij * trrij**2 / cvara_ep(iel)
    w1(iel) = viscl(iel) + idifft(ivar)*rctse
  enddo

  call viscfa                    &
 ( imvisf ,                      &
   w1     ,                      &
   viscf  , viscb  )

endif

!===============================================================================
! 10. Solving
!===============================================================================

if (st_prv_id.ge.0) then
  thetp1 = 1.d0 + thets
  do iel = 1, ncel
    do isou = 1, dimrij
      smbr(isou,iel) = smbr(isou,iel) + thetp1*c_st_prv(isou,iel)
    enddo
  enddo
endif

iconvp = iconv (ivar)
idiffp = idiff (ivar)
ndircp = ndircl(ivar)
nswrsp = nswrsm(ivar)
nswrgp = nswrgr(ivar)
imligp = imligr(ivar)
ircflp = ircflu(ivar)
ischcp = ischcv(ivar)
isstpp = isstpc(ivar)
idftnp = idften(ivar)
iswdyp = iswdyn(ivar)
iwarnp = iwarni(ivar)
blencp = blencv(ivar)
epsilp = epsilo(ivar)
epsrsp = epsrsm(ivar)
epsrgp = epsrgr(ivar)
climgp = climgr(ivar)
extrap = extrag(ivar)
relaxp = relaxv(ivar)
! all boundary convective flux with upwind
icvflb = 0

call coditts &
!==========
 ( idtvar , ivar   , iconvp , idiffp , ndircp ,                   &
   imrgra , nswrsp , nswrgp , imligp , ircflp ,                   &
   ischcp , isstpp , idftnp , iswdyp ,                            &
   iwarnp ,                                                       &
   blencp , epsilp , epsrsp , epsrgp , climgp ,                   &
   relaxp , thetv  ,                                              &
   cvara_var       , cvara_var       ,                            &
   coefap , coefbp , cofafp , cofbfp ,                            &
   imasfl , bmasfl ,                                              &
   viscf  , viscb  , viscf  , viscb  ,  viscce ,                  &
   weighf , weighb ,                                              &
   icvflb , ivoid  ,                                              &
   rovsdt , smbr   , cvar_var        )

! Free memory
deallocate(w1)
deallocate(w7, w8)
deallocate(dpvar)
deallocate(viscce)
deallocate(weighf, weighb)

!--------
! Formats
!--------

#if defined(_CS_LANG_FR)

 1000 format(/,'           Resolution pour la variable ',A8,/)

#else

 1000 format(/,'           Solving variable ',A8           ,/)

#endif

!----
! End
!----

return

end subroutine
