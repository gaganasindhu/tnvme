/*
 * Copyright (c) 2011, Intel Corporation.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include "boost/format.hpp"
#include "prpOffsetMultiPgMultiBlk_r10b.h"
#include "grpDefs.h"
#include "../Utils/irq.h"


namespace GrpNVMWriteReadCombo {


PRPOffsetMultiPgMultiBlk_r10b::PRPOffsetMultiPgMultiBlk_r10b(int fd,
    string mGrpName, string mTestName, ErrorRegs errRegs) :
    Test(fd, mGrpName, mTestName, SPECREV_10b, errRegs)
{
    // 63 chars allowed:     xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
    mTestDesc.SetCompliance("revision 1.0b, section 6");
    mTestDesc.SetShort(     "Vary buff offset for multi-page/multi-blk against PRP1/PRP2.");
    // No string size limit for the long description
    mTestDesc.SetLong(
        "Search for 1 of the following namspcs to run test. Find 1st bare "
        "namspc, or find 1st meta namspc, or find 1st E2E namspc. Issue "
        "identical write cmd starting at LBA 0, sending multiple blocks with "
        "approp meta/E2E requirements if necessary, where write executes the "
        "ALGO listed below; subsequently after each write a read must verify "
        "the data pattern for success, any metadata will also need to be "
        "verified for it will have the same data pattern as the data. "
        "ALGO) Alloc discontig memory; in an outer loop vary offset into 1st "
        "memory page from 0 to X, where "
        "X = (CC.MPS - Identify.LBAF[Identify.FLBAS].LBADS) in steps of 4B, "
        "in inner loop vary the number of blocks sent from 1 to Y, where "
        "Y = ((Identify.MDTS - X) / Identify.LBAF[Identify.FLBAS].LBADS) or "
        "Y~1MB if Identify.MDTS is unlimited; alternate the data pattern "
        "between dword++, dwordK for each write.");
}


PRPOffsetMultiPgMultiBlk_r10b::~PRPOffsetMultiPgMultiBlk_r10b()
{
    ///////////////////////////////////////////////////////////////////////////
    // Allocations taken from the heap and not under the control of the
    // RsrcMngr need to be freed/deleted here.
    ///////////////////////////////////////////////////////////////////////////
}


PRPOffsetMultiPgMultiBlk_r10b::
PRPOffsetMultiPgMultiBlk_r10b(const PRPOffsetMultiPgMultiBlk_r10b &other) :
    Test(other)
{
    ///////////////////////////////////////////////////////////////////////////
    // All pointers in this object must be NULL, never allow shallow or deep
    // copies, see Test::Clone() header comment.
    ///////////////////////////////////////////////////////////////////////////
}


PRPOffsetMultiPgMultiBlk_r10b &
PRPOffsetMultiPgMultiBlk_r10b::operator=(const PRPOffsetMultiPgMultiBlk_r10b
    &other)
{
    ///////////////////////////////////////////////////////////////////////////
    // All pointers in this object must be NULL, never allow shallow or deep
    // copies, see Test::Clone() header comment.
    ///////////////////////////////////////////////////////////////////////////
    Test::operator=(other);
    return *this;
}


void
PRPOffsetMultiPgMultiBlk_r10b::RunCoreTest()
{
    /** \verbatim
     * Assumptions:
     * None.
     * \endverbatim
     */
    string work;
    bool enableLog;

    if (gCtrlrConfig->SetState(ST_DISABLE_COMPLETELY) == false)
        throw FrmwkEx();

    // Create ACQ and ASQ objects which have test life time
    SharedACQPtr acq = CAST_TO_ACQ(SharedACQPtr(new ACQ(mFd)))
    acq->Init(5);
    SharedASQPtr asq = CAST_TO_ASQ(SharedASQPtr(new ASQ(mFd)))
    asq->Init(5);

    IRQ::SetAnySchemeSpecifyNum(2);     // throws upon error

    gCtrlrConfig->SetCSS(CtrlrConfig::CSS_NVM_CMDSET);
    if (gCtrlrConfig->SetState(ST_ENABLE) == false)
        throw FrmwkEx();

    SharedIOCQPtr iocq;
    SharedIOSQPtr iosq;
    InitTstRsrcs(asq, acq, iosq, iocq);

    // Compute memory page size from CC.MPS.
    uint8_t mpsRegVal;
    if (gCtrlrConfig->GetMPS(mpsRegVal) == false)
        throw FrmwkEx("Unable to get MPS value from CC.");
    uint64_t ccMPS = (uint64_t)(1 << (mpsRegVal + 12));

    Informative::Namspc namspcData = gInformative->Get1stBareMetaE2E();
    send_64b_bitmask prpBitmask =
        (send_64b_bitmask)(MASK_PRP1_PAGE | MASK_PRP2_PAGE | MASK_PRP2_LIST);
    LBAFormat lbaFormat = namspcData.idCmdNamspc->GetLBAFormat();
    uint64_t lbaDataSize = (1 << lbaFormat.LBADS);
    if (namspcData.type != Informative::NS_BARE) {
        if (gRsrcMngr->SetMetaAllocSize(lbaFormat.MS * (ccMPS / lbaDataSize))
            == false) {
            throw FrmwkEx("Unable to allocate Meta buffers.");
        }
    }

    ConstSharedIdentifyPtr idCmdCtrlr = gInformative->GetIdentifyCmdCtrlr();
    uint32_t maxDtXferSz = idCmdCtrlr->GetMaxDataXferSize();

    SharedWritePtr writeCmd = SharedWritePtr(new Write());
    writeCmd->SetNSID(namspcData.id);

    SharedReadPtr readCmd = SharedReadPtr(new Read());
    readCmd->SetNSID(namspcData.id);

    SharedMemBufferPtr writeMem = SharedMemBufferPtr(new MemBuffer());
    SharedMemBufferPtr readMem = SharedMemBufferPtr(new MemBuffer());

    if (namspcData.type == Informative::NS_META) {
        writeCmd->AllocMetaBuffer();
        readCmd->AllocMetaBuffer();
    } else if (namspcData.type == Informative::NS_E2E) {
        writeCmd->AllocMetaBuffer();
        readCmd->AllocMetaBuffer();
        LOG_ERR("Deferring E2E namspc work to the future");
        throw FrmwkEx("Need to add CRC's to correlate to buf pattern");
    }

    DataPattern dataPat;
    uint64_t wrVal;
    uint64_t Y;

    int64_t X =  ccMPS - lbaDataSize;
    if (X < 0) {
        LOG_WARN("CC.MPS < lba data size(LBADS); Can't run test.");
        return;
    }

    uint64_t altPattern = 0;
    for (int64_t pgOff = 0; pgOff <= X; pgOff += 4) {
        if (maxDtXferSz != 0)
            Y = (maxDtXferSz - pgOff) / lbaDataSize;
        else
            Y = (1024 * 1024) / lbaDataSize;

        // lbaPow2 = {2, 4, 8, 16, 32, 64, ...}
        for (uint64_t lbaPow2 = 2; lbaPow2 <= Y; lbaPow2 <<= 1) {
            // nLBA = {(0, 1, 2), (3, 4, 5), (7, 8, 9), (15, 16, 17), ...}
            for (uint64_t nLBA = (lbaPow2 - 1); nLBA <= (lbaPow2 + 1); nLBA++) {
                if ((++altPattern % 2) != 0) {
                    dataPat = DATAPAT_INC_32BIT;
                    wrVal = pgOff + nLBA;
                } else {
                    dataPat = DATAPAT_CONST_32BIT;
                    wrVal = pgOff + nLBA;
                }
                uint64_t dtPayloadSz = lbaDataSize * nLBA;
                if ((maxDtXferSz != 0) && (maxDtXferSz < dtPayloadSz)) {
                    // If the total data xfer exceeds the maximum data xfer
                    // allowed then we break from the inner loop and continue
                    // test with next offset (outer loop).
                    LOG_WARN("Data xfer sz exceeds max allowed, continuing..");
                    break;
                }
                work = str(boost::format("pgOff.%d.nlba.%d") % pgOff % nLBA);
                writeMem->InitOffset1stPage(dtPayloadSz, pgOff, false);
                writeCmd->SetPrpBuffer(prpBitmask, writeMem);
                writeMem->SetDataPattern(dataPat, wrVal);
                writeCmd->SetNLB(nLBA - 1); // convert to 0 based.

                uint64_t metabufSz = nLBA * lbaFormat.MS;
                if (namspcData.type != Informative::NS_BARE)
                    writeCmd->SetMetaDataPattern(dataPat, wrVal, 0, metabufSz);

                enableLog = false;
                if ((pgOff <= 8) || (pgOff >= (X - 8)))
                    enableLog = true;

                IO::SendCmdToHdw(mGrpName, mTestName, DEFAULT_CMD_WAIT_ms, iosq,
                    iocq, writeCmd, work, enableLog);

                readMem->InitOffset1stPage(dtPayloadSz, pgOff, false);
                readCmd->SetPrpBuffer(prpBitmask, readMem);
                readCmd->SetNLB(nLBA - 1); // convert to 0 based.

                IO::SendCmdToHdw(mGrpName, mTestName, DEFAULT_CMD_WAIT_ms, iosq,
                    iocq, readCmd, work, enableLog);

                VerifyDataPat(readCmd, dataPat, wrVal, metabufSz);
            }
        }
    }
}


void
PRPOffsetMultiPgMultiBlk_r10b::InitTstRsrcs(SharedASQPtr asq, SharedACQPtr acq,
    SharedIOSQPtr &iosq, SharedIOCQPtr &iocq)
{
    uint8_t iocqes = (gInformative->GetIdentifyCmdCtrlr()->
        GetValue(IDCTRLRCAP_CQES) & 0xf);
    uint8_t iosqes = (gInformative->GetIdentifyCmdCtrlr()->
        GetValue(IDCTRLRCAP_SQES) & 0xf);

    gCtrlrConfig->SetIOCQES(iocqes);
    gCtrlrConfig->SetIOSQES(iosqes);

    const uint32_t nEntriesIOQ = 2; // minimum Q entries always supported.
    if (Queues::SupportDiscontigIOQ() == true) {
        SharedMemBufferPtr iocqBackMem =  SharedMemBufferPtr(new MemBuffer());
        iocqBackMem->InitOffset1stPage((nEntriesIOQ * (1 << iocqes)), 0, true);

        iocq = Queues::CreateIOCQDiscontigToHdw(mGrpName, mTestName,
            DEFAULT_CMD_WAIT_ms, asq, acq, IOQ_ID, nEntriesIOQ, false,
            IOCQ_GROUP_ID, true, 1, iocqBackMem);

        SharedMemBufferPtr iosqBackMem = SharedMemBufferPtr(new MemBuffer());
        iosqBackMem->InitOffset1stPage((nEntriesIOQ * (1 << iosqes)), 0, true);
        iosq = Queues::CreateIOSQDiscontigToHdw(mGrpName, mTestName,
            DEFAULT_CMD_WAIT_ms, asq, acq, IOQ_ID, nEntriesIOQ, false,
            IOSQ_GROUP_ID, IOQ_ID, 0, iosqBackMem);
    } else {
       iocq = Queues::CreateIOCQContigToHdw(mGrpName, mTestName,
           DEFAULT_CMD_WAIT_ms, asq, acq, IOQ_ID, nEntriesIOQ, false,
           IOCQ_GROUP_ID, true, 1);

       iosq = Queues::CreateIOSQContigToHdw(mGrpName, mTestName,
           DEFAULT_CMD_WAIT_ms, asq, acq, IOQ_ID, nEntriesIOQ, false,
           IOSQ_GROUP_ID, IOQ_ID, 0);
    }
}


void
PRPOffsetMultiPgMultiBlk_r10b::VerifyDataPat(SharedReadPtr readCmd,
    DataPattern dataPat, uint64_t wrVal, uint64_t metabufSz)
{
    LOG_NRM("Compare read vs written data to verify");
    SharedMemBufferPtr wrPayload = SharedMemBufferPtr(new MemBuffer());
    wrPayload->Init(readCmd->GetPrpBufferSize());
    wrPayload->SetDataPattern(dataPat, wrVal);

    SharedMemBufferPtr rdPayload = readCmd->GetRWPrpBuffer();
    if (rdPayload->Compare(wrPayload) == false) {
        readCmd->Dump(
            FileSystem::PrepLogFile(mGrpName, mTestName, "ReadCmd"),
            "Read command");
        rdPayload->Dump(
            FileSystem::PrepLogFile(mGrpName, mTestName, "ReadPayload"),
            "Data read from media miscompared from written");
        wrPayload->Dump(
            FileSystem::PrepLogFile(mGrpName, mTestName, "WrittenPayload"),
            "Data read from media miscompared from written");
        throw FrmwkEx("Data miscompare");
    }

    if (readCmd->GetMetaBuffer() != NULL) {
        SharedMemBufferPtr metaWrPayload = SharedMemBufferPtr(new MemBuffer());
        metaWrPayload->Init(metabufSz);
        metaWrPayload->SetDataPattern(dataPat, wrVal);

        if (readCmd->CompareMetaBuffer(metaWrPayload) == false) {
            readCmd->Dump(
                FileSystem::PrepLogFile(mGrpName, mTestName, "MetaRdPayload"),
                "Meta Data read from media miscompared from written");
            metaWrPayload->Dump(
                FileSystem::PrepLogFile(mGrpName, mTestName, "MetaWrPayload"),
                "Meta Data read from media miscompared from written");
            throw FrmwkEx("Meta Data miscompare");
        }
    }
}

}   // namespace