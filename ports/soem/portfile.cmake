vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO OpenEtherCATsociety/SOEM
    REF 304d1c05eab77dc0d426f1a5cf09c8cc7dc03713
    SHA512 b96f4ccc9d205b06a00a4739a05c16a995e4cbf01310b475cd708e8bd7965ebc5d2d54a756567e9708346b6254c181d8e5b26e0c9bfdebdfaa2dd8a8acff6b82
    HEAD_REF master
)

# macOS (Darwin) support.  Upstream SOEM 2.0 ships only Linux/Windows platform
# layers; the build does `include(cmake/${CMAKE_SYSTEM_NAME}.cmake)`, so a stock
# configure on macOS fails for lack of cmake/Darwin.cmake.  We provide one here
# plus a 2.0-compatible osal/macosx.  The oshw/macosx (BPF/libpcap NIC driver)
# already lives in upstream contrib/ and is already on the 2.0 API, so we just
# move it into place.  These copies are inert on Linux/Windows — cmake/Darwin.cmake
# is only include()d when CMAKE_SYSTEM_NAME is Darwin.
file(COPY "${SOURCE_PATH}/contrib/oshw/macosx" DESTINATION "${SOURCE_PATH}/oshw")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/files/osal_macosx/" DESTINATION "${SOURCE_PATH}/osal/macosx")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/files/Darwin.cmake" DESTINATION "${SOURCE_PATH}/cmake")

# Windows: upstream links the bundled wpcap import libs by ABSOLUTE buildtree
# path, which gets baked into the exported soemConfig and vanishes once vcpkg
# restores SOEM from its binary cache — breaking every consumer's link. Drop
# those two lines; motion-master links wpcap/Packet from the vendored Npcap SDK
# (extern/npcap-sdk-1.16) at a stable path instead (see libs/comm). ws2_32/winmm
# stay — they are always on the linker search path.
vcpkg_replace_string(
    "${SOURCE_PATH}/cmake/Windows.cmake"
"target_link_libraries(soem PUBLIC
  \${WPCAP_LIB_PATH}/wpcap.lib
  \${WPCAP_LIB_PATH}/Packet.lib
  ws2_32.lib
  winmm.lib
)"
"target_link_libraries(soem PUBLIC
  ws2_32.lib
  winmm.lib
)")

# EC_MAXNAME caps the length of every readable name SOEM stores — slavelist
# names and, crucially, CoE Object Dictionary entry names read via
# ecx_readODdescription / ecx_readOEsingle. Upstream defaults to 40, which
# truncates SOMANET OD names mid-word (e.g. the 50-char "Module ident of the
# module detected on position 2" gets cut to "...detected on p"). The
# previous-generation master raised this to 80; we match that so the full names
# reach the API. Cost is only stack: the ec_ODlistt / ec_OElistt locals in
# SoemFieldbusDriver::readObjectDictionary hold EC_MAXODLIST/EC_MAXOELIST rows
# of EC_MAXNAME+1 bytes (~100 KB total at 80), trivial on the non-RT HTTP thread.
# SOEM 2.0's FoE write path mishandles an FOE_BUSY reply from the slave, which a bootloader sends
# while it erases or programs flash — exactly what happens partway through a firmware image.
#
# Two defects, both introduced by the 1.x -> 2.0 refactor from a stack mailbox buffer to a pooled
# one, which converted the FOE_ACK branch and missed FOE_BUSY:
#
#  1. The resend writes its packet through `FOEp`, which still points at the buffer handed to the
#     previous ecx_mbxsend, and then calls `ecx_mbxsend(context, slave, (ec_mbxbuft *)&MbxOut, ...)`.
#     `MbxOut` is NULL by then, so `&MbxOut` is the address of a stack pointer variable cast to a
#     mailbox — the slave is sent garbage rather than the packet, and the transfer stalls until the
#     caller's timeout. In 1.x `MbxOut` was itself the buffer, so `&MbxOut` was correct there.
#     Fixed by mirroring the FOE_ACK branch: take a fresh mailbox, point FOEp at it, send it.
#
#  2. A BUSY arriving before any data packet ("otherwise ignore") leaves `worktodo` FALSE, so the
#     loop exits and ecx_FOEwrite returns the positive work counter from the receive — reporting
#     **success for a transfer that wrote nothing**. For a firmware flasher that is the worst
#     possible failure mode. Fixed by looping to wait for the slave's next message, which is what
#     "ignore" was meant to mean.
#
# Verified by reading both versions rather than inferred. SOEM 1.x declares `ec_mbxbuft MbxIn,
# MbxOut;` — stack buffers — so `(ec_mbxbuft *)&MbxOut` is correct there and `FOEp` stays valid
# because the buffer is reused rather than handed off. 2.0 made them pooled pointers and converted
# only the neighbouring branch.
#
# That this path matters on SOMANET hardware is not a guess either: the previous-generation master's
# vendored SOEM 1.x carries a local fix to this same BUSY branch, marked with two references to
# https://github.com/OpenEtherCATsociety/SOEM/pull/627 — someone had to make the BUSY resend work
# before firmware writes would succeed on these drives. Observed here as an ACTILINK whose
# application binary writes fine (its packets are acknowledged without a pause) while the
# communication binary written straight after it never completes, failing at whatever timeout the
# caller sets; the previous generation writes the same file to the same drive in ~3.5 s.
#
# One deliberate deviation from 1.x: it has defect (2) as well (a BUSY before any data returns
# success having written nothing), and we fix it rather than matching. Waiting for the slave's next
# message is strictly safer than reporting a firmware write that never happened.
vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_foe.c"
"                           dofinalzero = TRUE;
                        }
                        FOEp->MbxHeader.length = htoes((uint16)(0x0006 + segmentdata));"
"                           dofinalzero = TRUE;
                        }
                        MbxOut = ecx_getmbx(context);
                        ec_clearmbx(MbxOut);
                        FOEp = (ec_FOEt *)MbxOut;
                        FOEp->MbxHeader.length = htoes((uint16)(0x0006 + segmentdata));")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_foe.c"
"wkc = ecx_mbxsend(context, slave, (ec_mbxbuft *)&MbxOut, EC_TIMEOUTTXM);"
"wkc = ecx_mbxsend(context, slave, MbxOut, EC_TIMEOUTTXM);
                        MbxOut = NULL;")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_foe.c"
"                  /* resend if data has been send before */
                  /* otherwise ignore */
                  if (sendpacket)"
"                  /* resend if data has been send before */
                  /* otherwise keep waiting for the slave's next message: falling through would
                     return the receive's work counter and report success for a transfer that
                     never sent a byte */
                  worktodo = TRUE;
                  if (sendpacket)")

# Make an FOE_BUSY reply observable. SOEM calls context->FOEhook on every acknowledged packet but
# says nothing when a slave reports itself busy, so "does this drive send BUSY?" — the question the
# fix above turns on — cannot be answered from a log without this. The BUSY branch reuses the same
# hook with a negative packet number as a sentinel; a consumer that does not install a hook is
# unaffected, and one that does can tell BUSY from progress by the sign.
vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_foe.c"
"                  worktodo = TRUE;
                  if (sendpacket)"
"                  if (context->FOEhook)
                  {
                     context->FOEhook(slave, -1, psize);
                  }
                  worktodo = TRUE;
                  if (sendpacket)")

# A CoE read accepts a reply meant for a different subindex.
#
# SOEM validates a read response against the request's *index* only (ecx_SDOread) or against the
# service opcode alone (ecx_readODdescription, ecx_readOEsingle), while ecx_SDOwrite — three
# functions away in the same file — checks "correct index and subindex". The read paths are simply
# inconsistent with the write path, and every one of them walks subindices of a single index:
# 0x1C12:00, :01, :02 ... for a PDO assignment, :00, :01 ... for each mapping object, one entry
# description per subindex for a dictionary enumeration. So when a frame is lost, a reply arriving one
# step late is read as the answer to the *next* request and accepted, because the index still
# matches. Nothing reports an error: the caller receives another subindex's value.
#
# What that costs is not a wrong reading somewhere harmless. A stale reply accepted for 0x1C12:00 or
# 0x16xx:00 is a wrong *entry count*, which truncates the mapping walk — and the master programs each
# slave's SM lengths from its SII records while sizing its own process-data window from this CoE
# figure (ec_config.c: ecx_map_sm, and ecx_map_sii's SII fallback only fires when both sizes come
# back zero, never for a short read). A truncated read therefore yields a master exchanging a
# narrower window than the drives were configured for, with every AL transition succeeding. This is
# the leading explanation for a bus of 28 identical drives mapping 2212 process-data bytes where the
# same bus mapped 3152 through a different adapter minutes later. ecx_readOEsingle is worse again: it
# feeds DataType and BitLength, which the RT path reads off the parameter while decoding process
# data.
#
# Fixed by comparing each response against what was requested. Two or three integer comparisons per
# reply, and a mismatch falls into each function's existing "unexpected frame" branch — wkc 0 plus a
# queued EC_ERR_TYPE_PACKET_ERROR, which the SDO Info retry already handles and
# SoemFieldbusDriver's sdoErrorSuffix already decodes. Silent substitution becomes a named failure.
#
# The comparisons deliberately use each function's own parameters rather than the request buffer.
# ecx_mbxsend ends with `if (mbx) ecx_dropmbx(context, mbx)` — it returns the request to the mailbox
# pool — so upstream's `aSDOp->Index == SDOp->Index` reads a freed pool slot. It happens to hold the
# right bytes, because ecx_getmbx serves that pool FIFO and will not hand the slot back within one
# transaction, but it is a use-after-free either way and the parameters say the same thing without
# one: `index` is untouched, and `subindex` has already been coerced for Complete Access two lines
# above the send (`if (CA && (subindex > 1)) subindex = 1;`), so it holds exactly what went out.
#
# Three things this deliberately leaves alone, each because the check would carry risk without
# buying anything:
#
#  - An SDO abort is unaffected. It does not satisfy the SDORES condition at all — which is why SOEM
#    detects ECT_SDO_ABORT in the *else* branch below it — so abort codes still reach ecx_SDOerror
#    and read back through sdoErrorSuffix exactly as before.
#  - Complete Access skips the subindex comparison (`CA ||`). SOEM coerces the requested subindex to
#    1 before sending, so the check would rest on the slave echoing that coercion, and a slave that
#    echoed 0 would have every CA upload rejected and fall back to per-subindex reads. A CA transfer
#    carries the whole object in one exchange, so there is no subindex walk for a stale reply to
#    land in the middle of: the index comparison is the whole of what CA needs.
#  - Segmented transfers keep their existing checks: only the initial response passes through these
#    conditions, and the segment branches match on Command bits and carry no index. ecx_readODlist
#    likewise keeps its opcode-only check, because a list response echoes no request field to
#    compare against.
#
# The response layouts are read off SOEM's own field offsets rather than assumed: ecx_readOEsingle
# takes ValueInfo from bdata[3], DataType from wdata[2] and the name from wdata[5], which places
# Index at wdata[0] and SubIndex at bdata[2] — the ETG.1000.6 entry-description layout, and the same
# offsets the request is built at. ecx_readODdescription takes DataType from wdata[1], MaxSub from
# bdata[4] and ObjectCode from bdata[5], placing Index at wdata[0]. Both hold only if the slave
# populates those leading fields, which every correct name and data type read from one already
# demonstrates. If a device were to answer without echoing them, its dictionary read fails at the
# first object rather than degrading quietly — loud on the first bus test, which is where this
# belongs.
vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_coe.c"
"         /* slave response should be CoE, SDO response and the correct index */
         if (((aSDOp->MbxHeader.mbxtype & 0x0f) == ECT_MBXT_COE) &&
             ((etohs(aSDOp->CANOpen) >> 12) == ECT_COES_SDORES) &&
             (aSDOp->Index == SDOp->Index))"
"         /* slave response should be CoE, SDO response and the requested index and subindex */
         if (((aSDOp->MbxHeader.mbxtype & 0x0f) == ECT_MBXT_COE) &&
             ((etohs(aSDOp->CANOpen) >> 12) == ECT_COES_SDORES) &&
             (aSDOp->Index == htoes(index)) &&
             (CA || (aSDOp->SubIndex == subindex)))")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_coe.c"
"         aSDOp = (ec_SDOservicet *)MbxIn;
         if (((aSDOp->MbxHeader.mbxtype & 0x0f) == ECT_MBXT_COE) &&
             ((aSDOp->Opcode & 0x7f) == ECT_GET_OD_RES))"
"         aSDOp = (ec_SDOservicet *)MbxIn;
         if (((aSDOp->MbxHeader.mbxtype & 0x0f) == ECT_MBXT_COE) &&
             ((aSDOp->Opcode & 0x7f) == ECT_GET_OD_RES) &&
             (aSDOp->wdata[0] == htoes(pODlist->Index[Item])))")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_coe.c"
"         aSDOp = (ec_SDOservicet *)MbxIn;
         if (((aSDOp->MbxHeader.mbxtype & 0x0f) == ECT_MBXT_COE) &&
             ((aSDOp->Opcode & 0x7f) == ECT_GET_OE_RES))"
"         aSDOp = (ec_SDOservicet *)MbxIn;
         if (((aSDOp->MbxHeader.mbxtype & 0x0f) == ECT_MBXT_COE) &&
             ((aSDOp->Opcode & 0x7f) == ECT_GET_OE_RES) &&
             (aSDOp->wdata[0] == htoes(Index)) &&
             (aSDOp->bdata[2] == SubI))")

# A mailbox buffer is returned to the pool twice.
#
# ecx_readODlist, ecx_readODdescription and ecx_readOEsingle each open by draining whatever sits in
# the slave's read mailbox, and they drop that buffer without clearing the pointer to it:
#
#     wkc = ecx_mbxreceive(context, Slave, &MbxIn, 0);
#     if (MbxIn) ecx_dropmbx(context, MbxIn);    /* MbxIn stays non-NULL */
#
# Every one of them drops MbxIn again further down — where it replaces the drained buffer with the
# response, or on the way out — so the same pool slot is freed twice. ecx_dropmbx pushes the slot's
# index onto the free list and increments listcount without checking whether that slot is free
# already, and the pool holds EC_MBXPOOLSIZE (32) slots. One double drop therefore duplicates an
# index and overcounts the free list, so ecx_getmbx hands the same buffer to two callers: a request
# being built and a response being read occupy the same bytes. Once listcount passes 32 the ring
# overwrites its own entries and slots are lost from the pool for good, until ecx_getmbx returns
# NULL and every mailbox transfer on the bus fails.
#
# What triggers it is a message already in the mailbox when a transfer starts, which is a late reply
# to an earlier request — the same fault the response-identity check above exists for. The two
# arrive together: a device that answers late corrupts the pool through the drain that is there to
# clean up after it. That makes an enumeration over a lossy link the way to hit this, which is
# exactly where the identity check sends a rejected response back for a retry.
#
# ecx_SDOread and ecx_SDOwrite in the same file are correct. They drain into MbxIn and drop it once.
# The three SDO Info functions drop at the drain as well, and that is the extra one. SOEM 1.x is
# unaffected, because MbxIn was a stack buffer there and there was no pool to corrupt.
#
# Fixed by clearing the pointer at the drain, which is what the correct sites do. One match, three
# call sites: the comment line is present only in these three functions.
#
# ecx_TxPDO carries the identical defect and is deliberately left alone. Motion Master issues no
# TxPDO read remote request, and patching a function nothing calls buys nothing while giving the
# next version bump one more string to match.
vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_coe.c"
"   /* Empty slave out mailbox if something is in. Timeout set to 0 */
   wkc = ecx_mbxreceive(context, Slave, &MbxIn, 0);
   if (MbxIn) ecx_dropmbx(context, MbxIn);"
"   /* Empty slave out mailbox if something is in. Timeout set to 0 */
   wkc = ecx_mbxreceive(context, Slave, &MbxIn, 0);
   if (MbxIn) ecx_dropmbx(context, MbxIn);
   MbxIn = NULL;")

# An FoE Error packet loses its code and its text, and reads like a slave that never answered.
#
# When a slave refuses an FoE transfer it sends an Error packet carrying an ETG.1000.6 error code,
# 0x8001 "Not found" for a missing file, and usually a short text. SOEM 2.0 keeps neither.
# ecx_FOEread sets the return value to -EC_ERR_TYPE_FOE_ERROR and nothing else, and ecx_FOEwrite
# keeps only whether the code was 0x8001. Neither pushes an entry onto the error list, which is
# where ecx_SDOread puts an SDO abort code. And EC_ERR_TYPE_FOE_ERROR is 5 while EC_TIMEOUT is -5,
# so a refusal and a slave that never answered return the same value. A caller cannot say why a
# drive refused a file, or whether it answered at all.
#
# Fixed the way SOEM reports an SDO abort: on an Error packet both functions push an ec_errort with
# Etype EC_ERR_TYPE_FOE_ERROR, the slave, and the error code in AbortCode. The return values are
# unchanged. A failed transfer with an FoE entry on the error list was refused; one without it
# never got an answer.
#
# The error text has no field in ec_errort, so it goes into a new context field, FOEerrortext,
# which holds one text per slave. Each Error packet overwrites the text of the slave that sent it.
# One text per slave keeps the text of one slave from overwriting the text of another. The text is
# only meaningful next to the error-list entry that the same packet pushed. Text past
# EC_MAXFOEERRORTEXT - 1 bytes is cut, and the result is always NUL-terminated.
vcpkg_replace_string(
    "${SOURCE_PATH}/include/soem/ec_main.h"
"   int (*FOEhook)(uint16 slave, int packetnumber, int datasize);"
"   int (*FOEhook)(uint16 slave, int packetnumber, int datasize);
   /** Per slave, the text of the last FoE Error packet it sent, NUL-terminated, empty when it carried none */
   char FOEerrortext[EC_MAXSLAVE][EC_MAXFOEERRORTEXT];")

vcpkg_replace_string(
    "${SOURCE_PATH}/include/soem/ec_main.h"
"typedef struct ecx_context ecx_contextt;"
"typedef struct ecx_context ecx_contextt;

/** Size of one ecx_contextt::FOEerrortext entry, the terminating NUL included */
#define EC_MAXFOEERRORTEXT 128")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_foe.c"
"int ecx_FOEdefinehook(ecx_contextt *context, void *hook)"
"/** Records an FoE Error packet: its code on the error list, its text in context->FOEerrortext */
static void ecx_FOEerror(ecx_contextt *context, uint16 slave, const ec_FOEt *aFOEp)
{
   ec_errort Ec;
   uint16 textlength;

   memset(&Ec, 0, sizeof(Ec));
   Ec.Time = osal_current_time();
   Ec.Slave = slave;
   Ec.Etype = EC_ERR_TYPE_FOE_ERROR;
   Ec.AbortCode = (int32)etohl(aFOEp->ErrorCode);
   ecx_pusherror(context, &Ec);

   textlength = etohs(aFOEp->MbxHeader.length);
   textlength = (textlength > 0x0006) ? (uint16)(textlength - 0x0006) : 0;
   if (textlength > EC_MAXFOEERRORTEXT - 1)
   {
      textlength = EC_MAXFOEERRORTEXT - 1;
   }
   memcpy(context->FOEerrortext[slave], aFOEp->ErrorText, textlength);
   context->FOEerrortext[slave][textlength] = '\\0';
}

int ecx_FOEdefinehook(ecx_contextt *context, void *hook)")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_foe.c"
"                  if (aFOEp->OpCode == ECT_FOE_ERROR)
                  {
                     /* FoE error */
                     wkc = -EC_ERR_TYPE_FOE_ERROR;"
"                  if (aFOEp->OpCode == ECT_FOE_ERROR)
                  {
                     /* FoE error */
                     ecx_FOEerror(context, slave, aFOEp);
                     wkc = -EC_ERR_TYPE_FOE_ERROR;")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_foe.c"
"               case ECT_FOE_ERROR:
               {
                  /* FoE error */
                  if (aFOEp->ErrorCode == 0x8001)"
"               case ECT_FOE_ERROR:
               {
                  /* FoE error */
                  ecx_FOEerror(context, slave, aFOEp);
                  if (aFOEp->ErrorCode == 0x8001)")

# The error list is not safe to use from two threads, and a pop cannot select a slave.
#
# ecx_poperror always returns the oldest entry, whichever slave caused it. An entry can stay on the
# list after the work that pushed it, for example an SDO abort during a scan or an emergency during
# a transfer that succeeded. A transfer to slave 2 that fails could then pop the entry of slave 1
# and report it as its own reason. ecx_popslaveerror pops the oldest entry of one slave and leaves
# the entries of every other slave in place. Each ec_errort carries the slave that caused it.
#
# The error list is also the one structure that every CoE and FoE transfer writes and that has no
# lock. ecx_pusherror and ecx_poperror change elist.head and elist.tail without one. Two threads
# that push at the same time can write the same slot, and the list then loses an entry or counts
# one twice. Each slave has its own mailbox and its own mailbox counter, and the mailbox pool has
# its own mutex, so the error list is what stops the mailbox work of two slaves from running on two
# threads.
#
# The patch adds elistmutex to the context. ecx_init and ecx_init_redundant create it, and
# ecx_close destroys it. ecx_pusherror, ecx_poperror, ecx_iserror and ecx_popslaveerror take it.
# ecaterror changes only inside ecx_pusherror and the two pops, so the patch removes the writes to
# it that ecx_packeterror, ecx_SDOerror and ecx_SDOinfoerror make before they push. ecx_SoEerror
# keeps its write, because Motion Master uses no SoE.
vcpkg_replace_string(
    "${SOURCE_PATH}/include/soem/ec_main.h"
"   ec_mbxpoolt mbxpool;"
"   ec_mbxpoolt mbxpool;
   /** internal, guards elist and ecaterror */
   osal_mutext *elistmutex;")

vcpkg_replace_string(
    "${SOURCE_PATH}/include/soem/ec_main.h"
"boolean ecx_poperror(ecx_contextt *context, ec_errort *Ec);"
"boolean ecx_poperror(ecx_contextt *context, ec_errort *Ec);
boolean ecx_popslaveerror(ecx_contextt *context, uint16 slave, ec_errort *Ec);")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_main.c"
"   ecx_initmbxpool(context);"
"   ecx_initmbxpool(context);
   context->elistmutex = (osal_mutext *)osal_mutex_create();")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_main.c"
"   osal_mutex_destroy(context->mbxpool.mbxmutex);"
"   osal_mutex_destroy(context->mbxpool.mbxmutex);
   osal_mutex_destroy(context->elistmutex);")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_main.c"
"void ecx_pusherror(ecx_contextt *context, const ec_errort *Ec)
{
   context->elist.Error[context->elist.head] = *Ec;"
"void ecx_pusherror(ecx_contextt *context, const ec_errort *Ec)
{
   osal_mutex_lock(context->elistmutex);
   context->elist.Error[context->elist.head] = *Ec;")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_main.c"
"   if (context->elist.tail > EC_MAXELIST)
   {
      context->elist.tail = 0;
   }
   context->ecaterror = TRUE;
}"
"   if (context->elist.tail > EC_MAXELIST)
   {
      context->elist.tail = 0;
   }
   context->ecaterror = TRUE;
   osal_mutex_unlock(context->elistmutex);
}")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_main.c"
"boolean ecx_poperror(ecx_contextt *context, ec_errort *Ec)
{
   boolean notEmpty = (context->elist.head != context->elist.tail);
"
"boolean ecx_poperror(ecx_contextt *context, ec_errort *Ec)
{
   boolean notEmpty;

   osal_mutex_lock(context->elistmutex);
   notEmpty = (context->elist.head != context->elist.tail);
")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_main.c"
"   else
   {
      context->ecaterror = FALSE;
   }
   return notEmpty;
}"
"   else
   {
      context->ecaterror = FALSE;
   }
   osal_mutex_unlock(context->elistmutex);
   return notEmpty;
}

/** Pops the oldest error of one slave from the list, and keeps the errors of every other slave.
 *
 * @param[in] context        context struct
 * @param[in] slave          slave whose error to pop
 * @param[out] Ec Struct describing the error. Unchanged when the slave has no error on the list.
 * @return TRUE if an error was popped.
 */
boolean ecx_popslaveerror(ecx_contextt *context, uint16 slave, ec_errort *Ec)
{
   const int16 size = EC_MAXELIST + 1;
   boolean found = FALSE;
   int16 slot;
   int16 next;

   osal_mutex_lock(context->elistmutex);
   for (slot = context->elist.tail; slot != context->elist.head; slot = (int16)((slot + 1) % size))
   {
      if (context->elist.Error[slot].Slave == slave)
      {
         found = TRUE;
         break;
      }
   }
   if (found)
   {
      *Ec = context->elist.Error[slot];
      /* close the gap: every later entry moves one slot towards the tail, so the order stays */
      for (next = (int16)((slot + 1) % size); next != context->elist.head; next = (int16)((next + 1) % size))
      {
         context->elist.Error[slot] = context->elist.Error[next];
         slot = next;
      }
      context->elist.Error[slot].Signal = FALSE;
      context->elist.head = slot;
   }
   context->ecaterror = (context->elist.head != context->elist.tail);
   osal_mutex_unlock(context->elistmutex);
   return found;
}")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_main.c"
"boolean ecx_iserror(ecx_contextt *context)
{
   return (context->elist.head != context->elist.tail);
}"
"boolean ecx_iserror(ecx_contextt *context)
{
   boolean notEmpty;

   osal_mutex_lock(context->elistmutex);
   notEmpty = (context->elist.head != context->elist.tail);
   osal_mutex_unlock(context->elistmutex);
   return notEmpty;
}")

# Two matches in ec_coe.c (ecx_SDOerror and ecx_SDOinfoerror) and one in ec_main.c
# (ecx_packeterror). Each function pushes right after this line, and ecx_pusherror sets ecaterror.
vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_coe.c"
"   Ec.SubIdx = SubIdx;
   context->ecaterror = TRUE;"
"   Ec.SubIdx = SubIdx;")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_main.c"
"   Ec.SubIdx = SubIdx;
   context->ecaterror = TRUE;"
"   Ec.SubIdx = SubIdx;")

# A SOMANET device must never get one mailbox request twice (synapticon/somanet_software#5811).
#
# SOMANET firmware detects a repeated mailbox counter and ignores the message. It returns before it
# reads the message, so the ESC never clears the mailbox-full bit of SM0. The firmware then finds
# the same message on every pass of its main loop and ignores it again. Every later mailbox write is
# refused, and only INIT followed by PRE-OP frees the mailbox. ETG.1000.4 Table 29 allows a master
# to repeat a write with the same counter, and upstream SOEM does: ecx_FPWR sends the datagram again
# when no frame comes back within EC_TIMEOUTRET, and ecx_mbxsend writes again after a failed write.
# When the first write did reach the slave, the slave gets a second copy with the same counter.
#
# The patch adds ecx_FPWRonce, which sends its datagram once and waits for that one frame. When the
# driver sets mbxwriteonce for a slave, ecx_mbxsend writes to that slave only through it, and acts on
# what came back:
#   - A working counter of 1: the message is in SM0. Its counter is the last one the slave can have
#     seen.
#   - A working counter of 0: the ESC refused the write because SM0 was full, so the slave did not
#     get the message. The write is repeated once after SM0 is empty again. When that also fails,
#     mbx_cnt goes back to the last counter the slave can have seen. Without that, every refused
#     write would use up a counter, and a run of refused writes would bring the counter back to the
#     value the slave accepted last.
#   - No frame within EC_TIMEOUTLATE: it is not known whether the slave got the message, so it is
#     not sent again. The call returns 1, and the caller waits for the answer. A late frame is the
#     usual cause, and in that case the slave answers. The SyncManager status is read first: a message in SM0 or an
#     answer in SM1 shows that the slave has the message, and nothing more is needed. Otherwise the
#     write is recorded as unknown, and an answer that ecx_mbxreceive reads for it settles it.
#   - After a write that stayed unknown, the next request goes out with counter 0, and the request
#     after that takes the unknown counter again. The two SOMANET stacks need this. The XMOS stack
#     of a Circulo ignores a request whose counter equals the last one it accepted, and that is
#     either the unknown counter or the one before it. It accepts counter 0 and stores it, so after
#     the counter-0 request it accepts the unknown counter. The netX stack of an Integro accepts a
#     request only when its counter is the one after the last counter it accepted, and drops every
#     request until the sequence comes round, up to six. It accepts counter 0 without storing it.
#     When the unknown write was lost, the unknown counter is the one it expects. When the slave
#     had the write and did not answer, it drops the one request and takes the next. A counter-0
#     request whose own outcome is unknown is followed by another counter-0 request, because both
#     stacks accept a repeated counter 0. Counter 0 is the one value the two stacks agree on, which
#     is why no other sequence does without a per-stack rule. Measured on both with
#     hil/mailbox_counter0 and hil/mailbox_duplicate.
# mbxwriteunknowns counts the writes without a frame, and mbxwriterecovers the requests sent with
# counter 0, for the driver to report.
#
# Every other slave keeps the upstream behaviour, because a slave that follows ETG.1000.4 discards
# a repeated message and the repeat recovers a write that was lost on the wire.
vcpkg_replace_string(
    "${SOURCE_PATH}/include/soem/ec_main.h"
"   /** Counter value of mailbox link layer protocol 1..7 */
   uint8 mbx_cnt;"
"   /** Counter value of mailbox link layer protocol 1..7 */
   uint8 mbx_cnt;
   /** write each mailbox message once, never repeat it */
   boolean mbxwriteonce;
   /** the last counted write got no frame back, and nothing shows that the slave has it */
   boolean mbxunknown;
   /** counter of that write */
   uint8 mbxunknowncnt;
   /** that write is the one in flight, so an answer proves that the slave has it */
   boolean mbxunknownpending;
   /** mailbox writes that got no frame back */
   uint32 mbxwriteunknowns;
   /** requests sent with counter 0 after a write with an unknown outcome */
   uint32 mbxwriterecovers;")

vcpkg_replace_string(
    "${SOURCE_PATH}/include/soem/ec_base.h"
"int ecx_FPWR(ecx_portt *port, uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout);"
"int ecx_FPWR(ecx_portt *port, uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout);
int ecx_FPWRonce(ecx_portt *port, uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout);
int ecx_FPRDonce(ecx_portt *port, uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout);")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_base.c"
"   idx = ecx_getindex(port);
   ecx_setupdatagram(port, &(port->txbuf[idx]), EC_CMD_FPWR, idx, ADP, ADO, length, data);
   wkc = ecx_srconfirm(port, idx, timeout);
   ecx_setbufstat(port, idx, EC_BUF_EMPTY);

   return wkc;
}"
"   idx = ecx_getindex(port);
   ecx_setupdatagram(port, &(port->txbuf[idx]), EC_CMD_FPWR, idx, ADP, ADO, length, data);
   wkc = ecx_srconfirm(port, idx, timeout);
   ecx_setbufstat(port, idx, EC_BUF_EMPTY);

   return wkc;
}

/** FPWR primitive that sends its datagram once. Blocking.
 *
 * ecx_FPWR sends the datagram again when no frame comes back within EC_TIMEOUTRET. This one
 * sends it once and waits up to timeout for that frame.
 *
 * @return Workcounter, or EC_NOFRAME when no frame came back
 */
int ecx_FPWRonce(ecx_portt *port, uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   int wkc;
   uint8 idx;

   idx = ecx_getindex(port);
   ecx_setupdatagram(port, &(port->txbuf[idx]), EC_CMD_FPWR, idx, ADP, ADO, length, data);
   ecx_outframe_red(port, idx);
   wkc = ecx_waitinframe(port, idx, timeout);
   ecx_setbufstat(port, idx, EC_BUF_EMPTY);

   return wkc;
}

/** FPRD primitive that sends its datagram once. Blocking.
 *
 * ecx_FPRD sends the datagram again when no frame comes back within EC_TIMEOUTRET. This one
 * sends it once and waits up to timeout for that frame.
 *
 * @return Workcounter, or EC_NOFRAME when no frame came back
 */
int ecx_FPRDonce(ecx_portt *port, uint16 ADP, uint16 ADO, uint16 length, void *data, int timeout)
{
   int wkc;
   uint8 idx;

   idx = ecx_getindex(port);
   ecx_setupdatagram(port, &(port->txbuf[idx]), EC_CMD_FPRD, idx, ADP, ADO, length, data);
   ecx_outframe_red(port, idx);
   wkc = ecx_waitinframe(port, idx, timeout);
   if (wkc > 0)
   {
      memcpy(data, &(port->rxbuf[idx][EC_HEADERSIZE]), length);
   }
   ecx_setbufstat(port, idx, EC_BUF_EMPTY);

   return wkc;
}")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_main.c"
"/** Write IN mailbox to slave.
 * Mailbox is fetched from pool by caller, ownership is transferred and dropped back to pool automatically."
"/* How long a frame may be late, in us. A mailbox write and a mailbox read each go out once and
 * wait this long for their own frame, because a datagram sent again reaches a slave that already
 * has the first one. A register datagram keeps EC_TIMEOUTRET, because it is safe to send again. */
#define EC_TIMEOUTLATE 100000

/* Writes a mailbox message to a slave that has mbxwriteonce set. The comment in the Motion Master
 * SOEM port explains the rules.
 */
static int ecx_mbxsendonce(ecx_contextt *context, uint16 slave, ec_mbxbuft *mbx, int timeout)
{
   ec_slavet *slaveitem = &(context->slavelist[slave]);
   ec_mbxheadert *mbxh = (ec_mbxheadert *)mbx;
   uint8 cnt = (uint8)((mbxh->mbxtype >> 4) & 0x07);
   boolean recover = FALSE;
   uint8 sm[16];
   int wkc = 0;
   int attempt;

   /* A new transfer starts, so an answer from now on does not prove the earlier write. */
   slaveitem->mbxunknownpending = FALSE;
   if (slaveitem->mbxunknown && (cnt != 0))
   {
      /* Counter 0 instead of the counter the caller took, and the next request takes the unknown
       * counter: mbx_cnt goes back to the counter before it. */
      mbxh->mbxtype = (uint8)(mbxh->mbxtype & 0x8F);
      cnt = 0;
      recover = TRUE;
      slaveitem->mbx_cnt = (uint8)((slaveitem->mbxunknowncnt == 1) ? 7 : (slaveitem->mbxunknowncnt - 1));
      slaveitem->mbxwriterecovers++;
   }
   for (attempt = 0; (attempt < 2) && (wkc == 0); attempt++)
   {
      if (!ecx_mbxempty(context, slave, timeout))
      {
         break;
      }
      wkc = ecx_FPWRonce(&context->port, slaveitem->configadr, slaveitem->mbx_wo, slaveitem->mbx_l, mbx, EC_TIMEOUTLATE);
   }
   if (wkc > 0)
   {
      if (recover)
      {
         slaveitem->mbxunknown = FALSE;
      }
      return wkc;
   }
   if (wkc == 0)
   {
      if (recover)
      {
         /* SM0 holds a message, and that is the unknown write. The slave has it, so the next
          * request takes the counter after it. */
         slaveitem->mbxunknown = FALSE;
         slaveitem->mbx_cnt = slaveitem->mbxunknowncnt;
      }
      else if (cnt != 0)
      {
         /* The ESC refused the write, so the slave did not see this counter. The caller took it
          * with ec_nextmbxcnt, and giving it back means the counter before it. */
         slaveitem->mbx_cnt = (uint8)((cnt == 1) ? 7 : (cnt - 1));
      }
      return 0;
   }
   slaveitem->mbxwriteunknowns++;
   if (recover || (cnt == 0))
   {
      /* A counter-0 write with an unknown outcome. In recovery, mbxunknown stays set and the next
       * request goes out with counter 0 again. Outside it, there is nothing to track. */
      return 1;
   }
   /* The status byte of SM0 is at offset 5 of the 16 bytes from ECT_REG_SM0, and that of SM1 at
    * offset 13. Bit 3 is set while the mailbox holds a message. */
   if ((ecx_FPRD(&context->port, slaveitem->configadr, ECT_REG_SM0, sizeof(sm), sm, EC_TIMEOUTRET3) > 0) &&
       (((sm[5] & 0x08) != 0) || ((sm[13] & 0x08) != 0)))
   {
      return 1;
   }
   slaveitem->mbxunknown = TRUE;
   slaveitem->mbxunknowncnt = cnt;
   slaveitem->mbxunknownpending = TRUE;
   return 1;
}

/** Write IN mailbox to slave.
 * Mailbox is fetched from pool by caller, ownership is transferred and dropped back to pool automatically.")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_main.c"
"   else if ((mbxl > 0) && (mbxl <= EC_MAXMBX) && (slavelist->state >= EC_STATE_PRE_OP))
   {
      mbxwo = context->slavelist[slave].mbx_wo;"
"   else if (slavelist->mbxwriteonce && (mbxl > 0) && (mbxl <= EC_MAXMBX) && (slavelist->state >= EC_STATE_PRE_OP))
   {
      wkc = ecx_mbxsendonce(context, slave, mbx, timeout);
   }
   else if ((mbxl > 0) && (mbxl <= EC_MAXMBX) && (slavelist->state >= EC_STATE_PRE_OP))
   {
      mbxwo = context->slavelist[slave].mbx_wo;")

# ecx_mbxreceive reads the mailbox into a buffer it takes from the pool once, before its loop. When
# a pass reads a mailbox error or an emergency, it drops that buffer and sets the pointer to NULL,
# and the next pass reads the mailbox into the NULL pointer. The read copies its data only when
# the working counter is 1, so the crash comes when the slave has written a new message by then: a
# reply after an emergency, or under late frames. The patch takes a buffer at the top of every
# pass that has none, and leaves the loop when the pool is empty.
#
# The emergency check reads through `mbx`, the caller's pointer to its result pointer, instead of
# `mbxin`, the message. So an emergency is never recognised in this path and reaches the caller as
# the reply, which fails the transfer. The patch reads the message.
#
# A message read in this loop is the slave's answer to the request in flight, because every CoE
# and FoE transfer empties the mailbox before it writes. When that request is a write whose
# outcome was unknown (mbxunknownpending, see ecx_mbxsendonce), the answer settles it.
#
# The mailbox read is sent once, and its frame is awaited for up to EC_TIMEOUTLATE, the same wait
# as a mailbox write gets in ecx_mbxsendonce. Upstream
# sends it through ecx_FPRD, which gives up after EC_TIMEOUTRET and sends the read again. The
# first read empties the mailbox when it reaches the slave, so the second one reads nothing, and
# upstream then takes the message for lost and toggles the Repeat Request bit. On a host that
# delivers frames late, that happens on a read whose frame is merely late: the message is in that
# frame, and it is dropped when it comes. An Integro answers the repeat request with the message
# before the one that was read, so SOEM then reads an SDO reply for the wrong object or an FoE
# packet with the wrong number. The FoE read fails, and the Integro keeps its file position, so the
# next read of the file serves its tail with no error (issue #113). With the read sent once, a late
# frame is awaited and used. No frame within EC_TIMEOUTLATE and SM1 still full means the read did
# not reach the slave, and the read is sent again. An empty SM1 means the message is gone, and only
# a repeat request can bring it back, as upstream does. A slave with mbxwriteonce set gets no
# repeat request at all: the Integro answers it with the wrong message, and the XMOS stack of a
# Circulo was not tested. The read fails instead, and the caller reports it.
vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_main.c"
"         mbxro = slavelist->mbx_ro;
         mbxin = ecx_getmbx(context);
         mbxh = (ec_mbxheadert *)mbxin;
         do
         {
            wkc = ecx_FPRD(&context->port, configadr, mbxro, mbxl, mbxin, EC_TIMEOUTRET); /* get mailbox */"
"         boolean gotmsg = FALSE;
         mbxro = slavelist->mbx_ro;
         mbxin = NULL;
         do
         {
            /* a buffer is needed on the first pass and after a pass that dropped a message */
            if (mbxin == NULL)
            {
               mbxin = ecx_getmbx(context);
               if (mbxin == NULL)
               {
                  wkc = 0;
                  break;
               }
               mbxh = (ec_mbxheadert *)mbxin;
            }
            wkc = ecx_FPRDonce(&context->port, configadr, mbxro, mbxl, mbxin, EC_TIMEOUTLATE); /* get mailbox */
            if (wkc > 0)
            {
               gotmsg = TRUE;
            }
            else if ((wkc == EC_NOFRAME) && (ecx_readmbxstatus(context, slave, &SMstat) > 0) && ((SMstat & 0x08) != 0))
            {
               continue; /* the read did not reach the slave: read again */
            }")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_main.c"
"            else if ((wkc > 0) && ((mbxh->mbxtype & 0x0f) == ECT_MBXT_COE)) /* CoE response? */
            {
               EMp = (ec_emcyt *)mbx;"
"            else if ((wkc > 0) && ((mbxh->mbxtype & 0x0f) == ECT_MBXT_COE)) /* CoE response? */
            {
               EMp = (ec_emcyt *)mbxin;")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_main.c"
"            else /* read mailbox lost */
            {
               do /* read extended mailbox status */"
"            else if (slavelist->mbxwriteonce) /* read mailbox lost, and a repeat request would bring the wrong message */
            {
               break;
            }
            else /* read mailbox lost */
            {
               do /* read extended mailbox status */")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_main.c"
"         } while ((wkc <= 0) && (osal_timer_is_expired(&timer) == FALSE)); /* if WKC<=0 repeat */
         if (mbxin) ecx_dropmbx(context, mbxin);"
"         } while ((wkc <= 0) && (osal_timer_is_expired(&timer) == FALSE)); /* if WKC<=0 repeat */
         if (mbxin) ecx_dropmbx(context, mbxin);
         if (gotmsg && slavelist->mbxunknownpending)
         {
            slavelist->mbxunknown = FALSE;
            slavelist->mbxunknownpending = FALSE;
         }")

# SOEM matches a returning frame to its datagram by the 8-bit index alone. An index goes back into
# use as soon as its datagram is answered or given up on, and EC_MAXBUF datagrams later at the
# latest. A frame that comes back after that carries the index of a newer datagram, and SOEM takes
# it as that datagram's answer: a working counter and data that belong to another request. On a
# host that delivers frames late, a scan read the wrong registers and lost the station address of
# a slave, and a mailbox read took an old frame for a message.
#
# The patch compares the header of the first datagram of a received frame with the header of the
# datagram sent under that index: the command, the offset, the length, and the position for the
# commands whose position the slaves leave alone. The auto increment and broadcast commands have
# their position counted up by every slave the frame passes. A frame that does not match is
# dropped, and the caller keeps waiting. The three platform drivers share this code.
foreach(nicdrv "oshw/linux/nicdrv.c" "oshw/win32/nicdrv.c" "oshw/macosx/nicdrv.c")
  vcpkg_replace_string(
      "${SOURCE_PATH}/${nicdrv}"
"int ecx_inframe(ecx_portt *port, uint8 idx, int stacknumber)
{"
"/* True when the first datagram of the frame in tempbuf is the one sent under idx. */
static boolean ecx_datagrammatches(ec_stackT *stack, uint8 idx)
{
   const ec_comt *tx = (const ec_comt *)&((*stack->txbuf)[idx][ETH_HEADERSIZE]);
   const ec_comt *rx = (const ec_comt *)&((*stack->tempbuf)[ETH_HEADERSIZE]);

   if ((tx->command != rx->command) || (tx->ADO != rx->ADO) ||
       ((tx->dlength & htoes(0x07ff)) != (rx->dlength & htoes(0x07ff))))
   {
      return FALSE;
   }
   switch (tx->command)
   {
      case EC_CMD_APRD:
      case EC_CMD_APWR:
      case EC_CMD_APRW:
      case EC_CMD_ARMW:
      case EC_CMD_BRD:
      case EC_CMD_BWR:
      case EC_CMD_BRW:
         return TRUE;
      default:
         return (tx->ADP == rx->ADP);
   }
}

int ecx_inframe(ecx_portt *port, uint8 idx, int stacknumber)
{")
  vcpkg_replace_string(
      "${SOURCE_PATH}/${nicdrv}"
"            if (idxf == idx)
            {"
"            if ((idxf == idx) && ecx_datagrammatches(stack, idx))
            {")
  vcpkg_replace_string(
      "${SOURCE_PATH}/${nicdrv}"
"               if (idxf < EC_MAXBUF && (*stack->rxbufstat)[idxf] == EC_BUF_TX)
               {"
"               if (idxf < EC_MAXBUF && (*stack->rxbufstat)[idxf] == EC_BUF_TX && ecx_datagrammatches(stack, idxf))
               {")
endforeach()

# SOEM maps a slave short when a read of its PDO mapping fails.
#
# ecx_config_map_group reads 0x1C00, the PDO assignment of each sync manager and every mapping
# object over the mailbox, through ecx_readPDOmap and ecx_readPDOassign or their Complete Access
# forms. None of those reads checks its working counter. A read with no answer, or with the answer
# to an earlier request, leaves its value at zero: an assignment with no PDOs, a PDO with no
# entries, or an entry of no bits. The map comes out short, the sync manager gets the short length,
# and the function returns no error. One late frame on the host is enough (issue #119).
#
# The patch counts those failures in pdomapreadfails of the slave. Each read of the mapping starts
# the count at zero, so a Complete Access read that failed and fell back to the plain read does not
# count against a plain read that succeeded. The driver checks the count after the map and maps
# again when it is not zero.
vcpkg_replace_string(
    "${SOURCE_PATH}/include/soem/ec_main.h"
"   /** input bits */
   uint16 Ibits;"
"   /** input bits */
   uint16 Ibits;
   /** SDO reads that failed in the last read of the PDO mapping for the map */
   uint32 pdomapreadfails;")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_coe.c"
"   *Isize = 0;
   *Osize = 0;"
"   *Isize = 0;
   *Osize = 0;
   context->slavelist[Slave].pdomapreadfails = 0;")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_coe.c"
"   wkc = ecx_SDOread(context, Slave, PDOassign, 0x00, FALSE, &rdl, &rdat, EC_TIMEOUTRXM);
   rdat = etohs(rdat);"
"   wkc = ecx_SDOread(context, Slave, PDOassign, 0x00, FALSE, &rdl, &rdat, EC_TIMEOUTRXM);
   if (wkc <= 0) context->slavelist[Slave].pdomapreadfails++;
   rdat = etohs(rdat);")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_coe.c"
"         wkc = ecx_SDOread(context, Slave, PDOassign, (uint8)idxloop, FALSE, &rdl, &rdat, EC_TIMEOUTRXM);"
"         wkc = ecx_SDOread(context, Slave, PDOassign, (uint8)idxloop, FALSE, &rdl, &rdat, EC_TIMEOUTRXM);
         if (wkc <= 0) context->slavelist[Slave].pdomapreadfails++;")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_coe.c"
"            wkc = ecx_SDOread(context, Slave, idx, 0x00, FALSE, &rdl, &subcnt, EC_TIMEOUTRXM);"
"            wkc = ecx_SDOread(context, Slave, idx, 0x00, FALSE, &rdl, &subcnt, EC_TIMEOUTRXM);
            if (wkc <= 0) context->slavelist[Slave].pdomapreadfails++;")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_coe.c"
"               wkc = ecx_SDOread(context, Slave, idx, (uint8)subidxloop, FALSE, &rdl, &rdat2, EC_TIMEOUTRXM);"
"               wkc = ecx_SDOread(context, Slave, idx, (uint8)subidxloop, FALSE, &rdl, &rdat2, EC_TIMEOUTRXM);
               if (wkc <= 0) context->slavelist[Slave].pdomapreadfails++;")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_coe.c"
"   wkc = ecx_SDOread(context, Slave, PDOassign, 0x00, TRUE, &rdl,
                     &(context->PDOassign[Thread_n]), EC_TIMEOUTRXM);"
"   wkc = ecx_SDOread(context, Slave, PDOassign, 0x00, TRUE, &rdl,
                     &(context->PDOassign[Thread_n]), EC_TIMEOUTRXM);
   if (wkc <= 0) context->slavelist[Slave].pdomapreadfails++;")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_coe.c"
"            wkc = ecx_SDOread(context, Slave, idx, 0x00, TRUE, &rdl,
                              &(context->PDOdesc[Thread_n]), EC_TIMEOUTRXM);"
"            wkc = ecx_SDOread(context, Slave, idx, 0x00, TRUE, &rdl,
                              &(context->PDOdesc[Thread_n]), EC_TIMEOUTRXM);
            if (wkc <= 0) context->slavelist[Slave].pdomapreadfails++;")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_coe.c"
"   wkc = ecx_SDOread(context, Slave, ECT_SDO_SMCOMMTYPE, 0x00, FALSE, &rdl, &nSM, EC_TIMEOUTRXM);"
"   wkc = ecx_SDOread(context, Slave, ECT_SDO_SMCOMMTYPE, 0x00, FALSE, &rdl, &nSM, EC_TIMEOUTRXM);
   if (wkc <= 0) context->slavelist[Slave].pdomapreadfails++;")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_coe.c"
"         wkc = ecx_SDOread(context, Slave, ECT_SDO_SMCOMMTYPE, iSM + 1, FALSE, &rdl, &tSM, EC_TIMEOUTRXM);"
"         wkc = ecx_SDOread(context, Slave, ECT_SDO_SMCOMMTYPE, iSM + 1, FALSE, &rdl, &tSM, EC_TIMEOUTRXM);
         if (wkc <= 0) context->slavelist[Slave].pdomapreadfails++;")

vcpkg_replace_string(
    "${SOURCE_PATH}/src/ec_coe.c"
"   wkc = ecx_SDOread(context, Slave, ECT_SDO_SMCOMMTYPE, 0x00, TRUE, &rdl,
                     &(context->SMcommtype[Thread_n]), EC_TIMEOUTRXM);"
"   wkc = ecx_SDOread(context, Slave, ECT_SDO_SMCOMMTYPE, 0x00, TRUE, &rdl,
                     &(context->SMcommtype[Thread_n]), EC_TIMEOUTRXM);
   if (wkc <= 0) context->slavelist[Slave].pdomapreadfails++;")

# EC_MAXBUF is the number of frame indexes. The RT thread and the control plane take indexes from
# one pool. When no index is free, ecx_getindex does not wait or fail. It returns an index that is
# still in use, and two frames then share one receive buffer. The control plane is capped at
# ControlPlaneLocks::kFrameSlots frames (libs/comm/control_plane_locks.h), and the RT thread gets
# the rest. The cap stays small because the NIC sends frames in order, so each control-plane frame
# in flight can delay the RT frame. The default of 16 leaves the RT thread 8 indexes, which limits
# the bus size. 32 leaves it 24, and the delay in front of the RT frame stays the same. Each index
# costs one tx and one rx buffer of one Ethernet frame.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DSOEM_BUILD_SAMPLES=OFF
        -DEC_MAXNAME=80
        -DEC_MAXBUF=32
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

vcpkg_cmake_config_fixup(CONFIG_PATH "cmake")

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/bin"
    "${CURRENT_PACKAGES_DIR}/debug/bin"
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/scripts"
    "${CURRENT_PACKAGES_DIR}/debug/scripts"
)

file(REMOVE
    "${CURRENT_PACKAGES_DIR}/README.md"
    "${CURRENT_PACKAGES_DIR}/debug/README.md"
    "${CURRENT_PACKAGES_DIR}/debug/LICENSE.md"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")
