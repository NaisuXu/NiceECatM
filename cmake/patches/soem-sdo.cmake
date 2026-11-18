# Copyright (C) 2026 Naisu Xu
# SPDX-License-Identifier: GPL-3.0-only

# SOEM v2.0.1 / 49fc3492: additional checked, bounded SDO transfers.
# Preserve upstream source and all v2.0.1 security fixes. Generate a compiled
# copy with the application's response/deadline guards in the build directory.
# Upstream GPLv3 / commercial licensing is unchanged. See THIRD_PARTY.md.
if(NOT DEFINED soem_SOURCE_DIR OR NOT DEFINED NICEECATM_SOEM_PATCHED_SOURCE_DIR)
  message(FATAL_ERROR "SOEM source and generated patch directories are required")
endif()
set(_niceecatm_coe_path "${NICEECATM_SOEM_PATCHED_SOURCE_DIR}/ec_coe.c")
file(READ "${soem_SOURCE_DIR}/src/ec_coe.c" _niceecatm_coe)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${soem_SOURCE_DIR}/src/ec_coe.c")
string(REPLACE "\r\n" "\n" _niceecatm_coe "${_niceecatm_coe}")
string(SHA256 _niceecatm_coe_hash "${_niceecatm_coe}")
set(_niceecatm_coe_original "bcc379bd3bd76c97c0361016d56647a33767a60e87c24eec8de4c1d31182ab0e")
set(_niceecatm_coe_patched "c27f519575d144bdced5e23fa753adfe027fb6f0ca5b1e84197e617a7243ccf6")
if(NOT _niceecatm_coe_hash STREQUAL _niceecatm_coe_original)
  message(FATAL_ERROR "SOEM ec_coe.c differs from pinned v2.0.1; review the SDO patch before building")
endif()

# Limit replacements to the named function and require exact context counts.
macro(_niceecatm_sdo_function start end)
  string(FIND "${_niceecatm_coe}" "${start}" _niceecatm_start)
  string(FIND "${_niceecatm_coe}" "${end}" _niceecatm_end)
  if(_niceecatm_start LESS 0 OR _niceecatm_end LESS _niceecatm_start)
    message(FATAL_ERROR "SOEM SDO function boundaries were not found")
  endif()
  string(LENGTH "${_niceecatm_coe}" _niceecatm_length)
  math(EXPR _niceecatm_body_length "${_niceecatm_end} - ${_niceecatm_start}")
  math(EXPR _niceecatm_suffix_length "${_niceecatm_length} - ${_niceecatm_end}")
  string(SUBSTRING "${_niceecatm_coe}" 0 ${_niceecatm_start} _niceecatm_prefix)
  string(SUBSTRING "${_niceecatm_coe}" ${_niceecatm_end} ${_niceecatm_suffix_length} _niceecatm_suffix)
  string(SUBSTRING "${_niceecatm_coe}" ${_niceecatm_start} ${_niceecatm_body_length} _niceecatm_coe)
endmacro()
macro(_niceecatm_sdo_end_function)
  set(_niceecatm_coe "${_niceecatm_prefix}${_niceecatm_coe}${_niceecatm_suffix}")
endmacro()

function(_niceecatm_patch_once old new)
  string(FIND "${_niceecatm_coe}" "${old}" position)
  if(position LESS 0)
    message(FATAL_ERROR "SOEM SDO patch context was not found")
  endif()
  string(REPLACE "${old}" "" removed "${_niceecatm_coe}")
  string(LENGTH "${_niceecatm_coe}" before_length)
  string(LENGTH "${removed}" after_length)
  string(LENGTH "${old}" old_length)
  if(ARGC EQUAL 3)
    math(EXPR old_length "${old_length} * ${ARGV2}")
  endif()
  math(EXPR removed_length "${before_length} - ${after_length}")
  if(NOT removed_length EQUAL old_length)
    message(FATAL_ERROR "SOEM SDO patch context was not unique")
  endif()
  string(REPLACE "${old}" "${new}" changed "${_niceecatm_coe}")
  set(_niceecatm_coe "${changed}" PARENT_SCOPE)
endfunction()

_niceecatm_sdo_function("int ecx_SDOread(" "/** CoE SDO write,")
_niceecatm_patch_once([=[   boolean NotLast;

   MbxIn = NULL;]=] [=[   boolean NotLast;
   /* NiceECatM: keep one transfer deadline across all upload segments. */
   osal_timert transferTimer;
   if (!psize || !p || (*psize < 0) || (timeout <= 0)) return 0;
   osal_timer_start(&transferTimer, timeout);

   MbxIn = NULL;]=])

_niceecatm_patch_once([=[   if (!MbxOut) return wkc;
   ec_clearmbx(MbxOut);]=] [=[   if (!MbxOut)
   {
      if (MbxIn) ecx_dropmbx(context, MbxIn);
      return 0;
   }
   ec_clearmbx(MbxOut);]=])

_niceecatm_patch_once([=[      } while ((wkc > 0) && !MbxIn);
      aSDOp = (ec_SDOt *)MbxIn;]=] [=[      } while ((wkc > 0) && !MbxIn && !osal_timer_is_expired(&transferTimer));
      if (!MbxIn) wkc = 0;
      aSDOp = (ec_SDOt *)MbxIn;]=])

_niceecatm_patch_once([=[         if (((aSDOp->MbxHeader.mbxtype & 0x0f) == ECT_MBXT_COE) &&
             ((etohs(aSDOp->CANOpen) >> 12) == ECT_COES_SDORES) &&
             (aSDOp->Index == SDOp->Index))]=] [=[         if (((aSDOp->MbxHeader.mbxtype & 0x0f) == ECT_MBXT_COE) &&
             ((etohs(aSDOp->CANOpen) >> 12) == ECT_COES_SDORES) &&
             (etohs(aSDOp->MbxHeader.length) >= 10) &&
             (etohs(aSDOp->MbxHeader.length) <= EC_MAXMBX - 6) &&
             (aSDOp->Index == htoes(index)) && (aSDOp->SubIndex == subindex))]=])

_niceecatm_patch_once([=[                     while (NotLast) /* segmented transfer */
                     {]=] [=[                     while (NotLast && !osal_timer_is_expired(&transferTimer)) /* bounded segmented transfer */
                     {]=])

_niceecatm_patch_once([=[                        if (!MbxOut)
                        {
                           NotLast = FALSE;
                           break;
                        }]=] [=[                        if (!MbxOut)
                        {
                           NotLast = FALSE;
                           wkc = 0;
                           break;
                        }]=])

_niceecatm_patch_once([=[                           if (wkc > 0)
                           {
                              /* slave response should be CoE, SDO response */]=] [=[                           if ((wkc > 0) && MbxIn)
                           {
                              /* slave response should be CoE, SDO response */]=])

_niceecatm_patch_once([=[                                   ((aSDOp->Command & 0xe0) == 0x00)))]=] [=[                                   ((aSDOp->Command & 0xe0) == 0x00) &&
                                   ((aSDOp->Command & 0x10) == toggle) &&
                                   (etohs(aSDOp->MbxHeader.length) >= 3) &&
                                   (etohs(aSDOp->MbxHeader.length) <= EC_MAXMBX - 6)))]=])

_niceecatm_patch_once([=[                                 if ((uint32)total + (uint32)Framedatasize > SDOlen)]=] [=[                                 if (((uint32)Framedatasize > SDOlen - total) ||
                                     (((aSDOp->Command & 0x01) == 0) && (Framedatasize == 0)))]=])

_niceecatm_patch_once([=[                                 /* update total */
                                 total += (uint32)Framedatasize;]=] [=[                                 /* Keep upstream's separate total; publish the size only on success. */
                                 total += (uint32)Framedatasize;
                                 if (!NotLast && (wkc > 0) && (total != SDOlen))
                                 {
                                    wkc = 0;
                                    ecx_packeterror(context, slave, index, subindex, 1);
                                 }]=])

_niceecatm_patch_once([=[                        /* stop if request failed or slave did not respond */
                        if (wkc <= 0)
                        {
                           NotLast = FALSE;
                        }
                        toggle = toggle ^ 0x10; /* toggle bit for segment request */
                     }
                     /* Only update *psize on successful completion */]=] [=[                        /* Stop on transport failure or an empty response. */
                        if ((wkc <= 0) || !MbxIn)
                        {
                           NotLast = FALSE;
                           wkc = 0;
                        }
                        toggle = toggle ^ 0x10; /* toggle bit for segment request */
                     }
                     if (NotLast)
                     {
                        wkc = 0;
                        ecx_packeterror(context, slave, index, subindex, 1);
                     }
                     /* Only update *psize on successful completion */]=])

_niceecatm_patch_once([=[             (aSDOp->Index == htoes(index)) && (aSDOp->SubIndex == subindex))]=] [=[             ((aSDOp->Command & 0xe0) == 0x40) &&
             (aSDOp->Index == htoes(index)) && (aSDOp->SubIndex == subindex))]=])

_niceecatm_patch_once([=[if ((aSDOp->Command) == ECT_SDO_ABORT)]=] [=[if (((aSDOp->Command) == ECT_SDO_ABORT) && (etohs(aSDOp->MbxHeader.length) >= 10))]=] 2)

_niceecatm_sdo_end_function()

_niceecatm_sdo_function("int ecx_SDOwrite(" "/** CoE RxPDO write,")
_niceecatm_patch_once([=[   const uint8 *hp;

   /* slave must have a usable mailbox */]=] [=[   const uint8 *hp;
   /* NiceECatM: reject unusable requests and bound the complete download. */
   osal_timert transferTimer;
   if ((psize <= 0) || !p || (Timeout <= 0) ||
       (context->slavelist[Slave].mbx_l < 16)) return 0;
   osal_timer_start(&transferTimer, Timeout);

   /* slave must have a usable mailbox */]=])

_niceecatm_patch_once([=[   if (!MbxOut) return wkc;]=] [=[   if (!MbxOut)
   {
      if (MbxIn) ecx_dropmbx(context, MbxIn);
      return 0;
   }]=])

_niceecatm_patch_once([=[         wkc = ecx_mbxreceive(context, Slave, &MbxIn, Timeout);
         if (wkc > 0)]=] [=[         wkc = ecx_mbxreceive(context, Slave, &MbxIn, Timeout);
         if (!MbxIn) wkc = 0;
         if ((wkc > 0) && MbxIn)]=] 2)

_niceecatm_patch_once([=[                (aSDOp->Index == SDOp->Index) &&
                (aSDOp->SubIndex == SDOp->SubIndex))]=] [=[                (etohs(aSDOp->MbxHeader.length) >= 6) &&
                (etohs(aSDOp->MbxHeader.length) <= EC_MAXMBX - 6) &&
                ((aSDOp->Command == 0x60) || (CA && (aSDOp->Command == 0x70))) &&
                (aSDOp->Index == htoes(Index)) &&
                (aSDOp->SubIndex == ((CA && SubIndex > 1) ? 1 : SubIndex)))]=] 2)

_niceecatm_patch_once([=[               while (NotLast)
               {]=] [=[               while (NotLast && !osal_timer_is_expired(&transferTimer))
               {]=])

_niceecatm_patch_once([=[                  if (!MbxOut)
                  {
                     NotLast = FALSE;
                     break;
                  }]=] [=[                  if (!MbxOut)
                  {
                     NotLast = FALSE;
                     wkc = 0;
                     break;
                  }]=])

_niceecatm_patch_once([=[                     wkc = ecx_mbxreceive(context, Slave, &MbxIn, Timeout);
                     if (wkc > 0)]=] [=[                     wkc = ecx_mbxreceive(context, Slave, &MbxIn, Timeout);
                     if (!MbxIn) wkc = 0;
                     if ((wkc > 0) && MbxIn)]=])

_niceecatm_patch_once([=[                            ((aSDOp->Command & 0xe0) == 0x20))]=] [=[                            (etohs(aSDOp->MbxHeader.length) >= 3) &&
                            (etohs(aSDOp->MbxHeader.length) <= EC_MAXMBX - 6) &&
                            (aSDOp->Command == (uint8)(0x20 | toggle)))]=])

_niceecatm_patch_once([=[                  /* stop if request failed or slave did not respond */
                  if (wkc <= 0)
                  {
                     NotLast = FALSE;
                  }
                  toggle = toggle ^ 0x10; /* toggle bit for segment request */
               }
            }]=] [=[                  /* Stop on transport failure or an empty response. */
                  if ((wkc <= 0) || !MbxIn)
                  {
                     NotLast = FALSE;
                     wkc = 0;
                  }
                  toggle = toggle ^ 0x10; /* toggle bit for segment request */
               }
               if (NotLast)
               {
                  wkc = 0;
                  ecx_packeterror(context, Slave, Index, SubIndex, 1);
               }
            }]=])

_niceecatm_patch_once([=[if (aSDOp->Command == ECT_SDO_ABORT)]=] [=[if ((aSDOp->Command == ECT_SDO_ABORT) && (etohs(aSDOp->MbxHeader.length) >= 10))]=] 3)

_niceecatm_sdo_end_function()

string(SHA256 _niceecatm_coe_hash "${_niceecatm_coe}")
if(NOT _niceecatm_coe_hash STREQUAL _niceecatm_coe_patched)
  message(FATAL_ERROR "SOEM patched source hash mismatch")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/soem-sdoinfo.cmake")
file(MAKE_DIRECTORY "${NICEECATM_SOEM_PATCHED_SOURCE_DIR}")
set(_niceecatm_write_coe TRUE)
if(EXISTS "${_niceecatm_coe_path}")
  file(READ "${_niceecatm_coe_path}" _niceecatm_existing_coe)
  string(REPLACE "\r\n" "\n" _niceecatm_existing_coe "${_niceecatm_existing_coe}")
  if(_niceecatm_existing_coe STREQUAL _niceecatm_coe)
    set(_niceecatm_write_coe FALSE)
  endif()
endif()
if(_niceecatm_write_coe)
  file(WRITE "${_niceecatm_coe_path}" "${_niceecatm_coe}")
endif()
message(STATUS "Applied NiceECatM SDO / SDO Information response/deadline guards to pinned SOEM v2.0.1")
