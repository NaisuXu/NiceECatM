# Copyright (C) 2026 Naisu Xu
# SPDX-License-Identifier: GPL-3.0-only

# SDO Information is optional, untrusted metadata. Keep the upstream API while
# validating lengths, service/response identity and multipart list completion.
# The original source and upload/download patch hashes were checked by caller.
_niceecatm_sdo_function("int ecx_readODlist(" "/** CoE read Object Description. Adds")
_niceecatm_patch_once([=[   boolean First;
]=] [=[   boolean First;
   uint16 previousFragments = 0xffff;
   osal_timert transferTimer;
   osal_timer_start(&transferTimer, EC_TIMEOUTTXM + EC_TIMEOUTRXM);
]=])
_niceecatm_patch_once([=[   if (MbxIn) ecx_dropmbx(context, MbxIn);
   MbxOut = ecx_getmbx(context);
   if (!MbxOut) return wkc;]=] [=[   if (MbxIn) ecx_dropmbx(context, MbxIn);
   MbxIn = NULL;
   MbxOut = ecx_getmbx(context);
   if (!MbxOut) return 0;]=])
_niceecatm_patch_once([=[         wkc = ecx_mbxreceive(context, Slave, &MbxIn, EC_TIMEOUTRXM);]=] [=[         wkc = ecx_mbxreceive(context, Slave, &MbxIn, First ? EC_TIMEOUTRXM : 100000);
         if (!MbxIn) wkc = 0;]=])
_niceecatm_patch_once([=[            if (((aSDOp->MbxHeader.mbxtype & 0x0f) == ECT_MBXT_COE) &&
                ((aSDOp->Opcode & 0x7f) == ECT_GET_ODLIST_RES))]=] [=[            if (((aSDOp->MbxHeader.mbxtype & 0x0f) == ECT_MBXT_COE) &&
                ((etohs(aSDOp->CANOpen) >> 12) == ECT_COES_SDOINFO) &&
                (etohs(aSDOp->MbxHeader.length) >= (First ? 8 : 6)) &&
                (etohs(aSDOp->MbxHeader.length) <= EC_MAXMBX - 6) &&
                (((etohs(aSDOp->MbxHeader.length) - (First ? 8 : 6)) & 1) == 0) &&
                ((aSDOp->Opcode & 0x7f) == ECT_GET_ODLIST_RES) &&
                (!First || (etohs(aSDOp->wdata[0]) == 1)) &&
                (First || ((previousFragments > 0) && (etohs(aSDOp->Fragments) == previousFragments - 1))))]=])
_niceecatm_patch_once([=[                  n = EC_MAXODLIST + 1 - sp;
                  ecx_SDOinfoerror(context, Slave, 0, 0, 0xf000000); /* Too many entries for master buffer */
                  stop = TRUE;]=] [=[                  ecx_SDOinfoerror(context, Slave, 0, 0, 0xf000000); /* Too many entries for master buffer */
                  wkc = 0;
                  break;]=])
_niceecatm_patch_once([=[               if (aSDOp->Fragments > 0)
               {
                  stop = FALSE;
               }
               First = FALSE;]=] [=[               previousFragments = etohs(aSDOp->Fragments);
               if ((previousFragments > 0) || (aSDOp->Opcode & 0x80))
               {
                  /* An incomplete-only reply without a fragment count is
                   * unsupported here, never a completed object list. */
                  if (!n || !previousFragments) { ecx_packeterror(context, Slave, 0, 0, 1); wkc = 0; }
                  else stop = FALSE;
               }
               First = FALSE;]=])
_niceecatm_patch_once([=[               if ((aSDOp->Opcode & 0x7f) == ECT_SDOINFO_ERROR) /* SDO info error received */]=] [=[               if (((aSDOp->MbxHeader.mbxtype & 0x0f) == ECT_MBXT_COE) &&
                   ((etohs(aSDOp->CANOpen) >> 12) == ECT_COES_SDOINFO) &&
                   (etohs(aSDOp->MbxHeader.length) >= 10) &&
                   (etohs(aSDOp->MbxHeader.length) <= EC_MAXMBX - 6) &&
                   ((aSDOp->Opcode & 0x7f) == ECT_SDOINFO_ERROR)) /* SDO info error received */]=])
_niceecatm_patch_once([=[      } while ((x <= 128) && !stop);
]=] [=[      } while ((x <= 128) && !stop && !osal_timer_is_expired(&transferTimer));
      if (!stop) { ecx_packeterror(context, Slave, 0, 0, 1); wkc = 0; }
]=])
_niceecatm_sdo_end_function()

foreach(_niceecatm_info_kind IN ITEMS OD OE)
  if(_niceecatm_info_kind STREQUAL OD)
    _niceecatm_sdo_function("int ecx_readODdescription(" "/** CoE read SDO service object entry, single")
    set(_niceecatm_info_minimum 12)
    set(_niceecatm_info_identity "(aSDOp->wdata[0] == htoes(pODlist->Index[Item]))")
  else()
    _niceecatm_sdo_function("int ecx_readOEsingle(" "/** CoE read SDO service object entry.\n")
    set(_niceecatm_info_minimum 16)
    set(_niceecatm_info_identity "(aSDOp->wdata[0] == htoes(Index)) && (aSDOp->bdata[2] == SubI)")
  endif()
  _niceecatm_patch_once([=[   if (MbxIn) ecx_dropmbx(context, MbxIn);
   MbxOut = ecx_getmbx(context);
   if (!MbxOut) return wkc;]=] [=[   if (MbxIn) ecx_dropmbx(context, MbxIn);
   MbxIn = NULL;
   MbxOut = ecx_getmbx(context);
   if (!MbxOut) return 0;]=])
  _niceecatm_patch_once([=[      wkc = ecx_mbxreceive(context, Slave, &MbxIn, EC_TIMEOUTRXM);]=] [=[      wkc = ecx_mbxreceive(context, Slave, &MbxIn, EC_TIMEOUTRXM);
      if (!MbxIn) wkc = 0;]=])
  set(_niceecatm_info_old "         if (((aSDOp->MbxHeader.mbxtype & 0x0f) == ECT_MBXT_COE) &&\n             ((aSDOp->Opcode & 0x7f) == ECT_GET_${_niceecatm_info_kind}_RES))")
  set(_niceecatm_info_new "         if (((aSDOp->MbxHeader.mbxtype & 0x0f) == ECT_MBXT_COE) &&\n             ((etohs(aSDOp->CANOpen) >> 12) == ECT_COES_SDOINFO) &&\n             (etohs(aSDOp->MbxHeader.length) >= ${_niceecatm_info_minimum}) &&\n             (etohs(aSDOp->MbxHeader.length) <= EC_MAXMBX - 6) &&\n             (aSDOp->Fragments == 0) &&\n             (aSDOp->Opcode == ECT_GET_${_niceecatm_info_kind}_RES) &&\n             ${_niceecatm_info_identity})")
  _niceecatm_patch_once("${_niceecatm_info_old}" "${_niceecatm_info_new}")
  _niceecatm_patch_once([=[            if (((aSDOp->Opcode & 0x7f) == ECT_SDOINFO_ERROR)) /* SDO info error received */]=] [=[            if (((aSDOp->MbxHeader.mbxtype & 0x0f) == ECT_MBXT_COE) &&
                ((etohs(aSDOp->CANOpen) >> 12) == ECT_COES_SDOINFO) &&
                (etohs(aSDOp->MbxHeader.length) >= 10) &&
                (etohs(aSDOp->MbxHeader.length) <= EC_MAXMBX - 6) &&
                ((aSDOp->Opcode & 0x7f) == ECT_SDOINFO_ERROR)) /* SDO info error received */]=])
  _niceecatm_sdo_end_function()
endforeach()
