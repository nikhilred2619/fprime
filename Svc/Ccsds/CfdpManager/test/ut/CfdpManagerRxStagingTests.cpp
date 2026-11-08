// ======================================================================
// \title  CfdpManagerRxStagingTests.cpp
// \brief  Receive staging, commit and cleanup tests (issue #6230)
//
// A received file is written to a staging file beside its destination and renamed onto the
// destination only once the transfer has completed and its checksum has passed. These tests
// check that an incomplete or failed receive leaves nothing at the destination, never alters an
// existing destination file, and removes only the files the transaction itself created.
// ======================================================================

#include <sys/resource.h>
#include <sys/stat.h>

#include <csignal>
#include <cstring>

#include "CFDP/Checksum/Checksum.hpp"
#include "CfdpManagerTester.hpp"
#include "Os/File.hpp"
#include "Os/FileSystem.hpp"

namespace Svc {
namespace Ccsds {
namespace Cfdp {

namespace {

constexpr U8 RX_CHANNEL = 0;
const char* const RX_DST = "test/ut/output/rx_staging_dst.bin";
const char* const RX_GROUND_SRC = "/ground/rx_staging_src.bin";
const char* const RX_SENTINEL = "test/ut/output/rx_staging_dst.bin/sentinel";

//! Size declared by the Metadata PDU of a transfer that is cut off after one File Data PDU
constexpr FileSize RX_DECLARED_SIZE = 100;
constexpr U16 RX_CHUNK_SIZE = 10;
const U8 RX_NEW_DATA[RX_CHUNK_SIZE] = {'N', 'E', 'W', '-', 'D', 'A', 'T', 'A', '-', '1'};
const U8 RX_ORIGINAL[] = {'O', 'R', 'I', 'G', 'I', 'N', 'A', 'L', '-', 'G', 'O', 'O', 'D', '-', 'F', 'I', 'L', 'E'};
const U8 RX_FOREIGN[] = {'F', 'O', 'R', 'E', 'I', 'G', 'N'};
const U8 RX_FOREIGN_CHUNK[RX_CHUNK_SIZE] = {'R', 'E', 'P', 'E', 'A', 'T', 'E', 'D', '-', '0'};

//! Upper bound on run1Hz cycles for any transaction in these tests to be recycled
constexpr U32 RX_MAX_CYCLES = 300;

U32 crcOf(const U8* data, FwSizeType size) {
    CFDP::Checksum crc;
    crc.update(data, 0, static_cast<U32>(size));
    return crc.getValue();
}

}  // namespace

// ----------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------

void CfdpManagerTester::rxStagingConfigure(const char* tmpDir) {
    Cfdp::ChannelArrayParams channelConfig;
    for (FwSizeType chan = 0; chan < Cfdp::NumChannels; chan++) {
        Cfdp::ChannelParams& params = channelConfig[chan];
        // The default timers and limits, so transfers end the way they do on a stock channel
        params.set_ack_limit(10);
        params.set_nack_limit(10);
        params.set_ack_timer(2);
        params.set_inactivity_timer(120);
        params.set_dequeue_enabled(Fw::Enabled::ENABLED);
        params.set_move_dir(Fw::String(""));
        params.set_max_outgoing_pdus_per_cycle(64);
        params.set_tmp_dir(Fw::String(tmpDir));
        params.set_fail_dir(Fw::String(""));
        params.set_rx_dir(Fw::String(""));
    }
    this->paramSet_ChannelConfig(channelConfig, Fw::ParamValid::VALID);
    this->paramSend_ChannelConfig(0, 0);
    this->clearHistory();
}

void CfdpManagerTester::rxStagingPath(TransactionSeq seq, Fw::String& path) {
    ASSERT_EQ(Fw::FormatStatus::SUCCESS,
              path.format("%s.%u-%u-%u.part", RX_DST, static_cast<unsigned>(RX_CHANNEL),
                          static_cast<unsigned>(TEST_GROUND_EID), static_cast<unsigned>(seq)));
}

void CfdpManagerTester::rxStagingTmpPath(TransactionSeq seq, Fw::String& path) {
    ASSERT_EQ(Fw::FormatStatus::SUCCESS,
              path.format("%s/%u:%u.tmp", this->component.getTmpDirParam(RX_CHANNEL).toChar(),
                          static_cast<unsigned>(TEST_GROUND_EID), static_cast<unsigned>(seq)));
}

void CfdpManagerTester::rxStagingWriteFile(const char* path, const U8* data, FwSizeType size) {
    Os::File file;
    ASSERT_EQ(Os::File::OP_OK, file.open(path, Os::File::OPEN_CREATE, Os::File::OVERWRITE));
    FwSizeType written = size;
    const Os::File::Status status = file.write(data, written, Os::File::WAIT);
    file.close();
    ASSERT_EQ(Os::File::OP_OK, status);
    ASSERT_EQ(size, written);
}

void CfdpManagerTester::rxStagingExpectFile(const char* path, const U8* data, FwSizeType size) {
    FwSizeType actualSize = 0;
    ASSERT_EQ(Os::FileSystem::OP_OK, Os::FileSystem::getFileSize(path, actualSize)) << path << " should exist";
    ASSERT_EQ(size, actualSize) << path << " has the wrong size";
    this->verifyReceivedFile(path, data, size);
}

void CfdpManagerTester::rxStagingExpectNoFile(const char* path) {
    EXPECT_EQ(Os::FileSystem::NOT_EXIST, Os::FileSystem::getPathType(path)) << path << " should not exist";
}

void CfdpManagerTester::rxStagingRemove(const char* path) {
    (void)Os::FileSystem::removeFile(path);  // test cleanup; the file may legitimately be absent
}

void CfdpManagerTester::rxStagingCleanup(TransactionSeq seq) {
    Fw::String staging;
    Fw::String tmp;
    this->rxStagingPath(seq, staging);
    this->rxStagingTmpPath(seq, tmp);
    this->rxStagingRemove(RX_SENTINEL);
    (void)Os::FileSystem::removeDirectory(RX_DST);  // only present in the commit-failure test
    this->rxStagingRemove(RX_DST);
    this->rxStagingRemove(staging.toChar());
    this->rxStagingRemove(tmp.toChar());
}

void CfdpManagerTester::rxStagingSendMetadata(TransactionSeq seq, Cfdp::Class::T cfdpClass, FileSize fileSize) {
    this->sendMetadataPdu(RX_CHANNEL, TEST_GROUND_EID, this->component.getLocalEidParam(), seq, fileSize, RX_GROUND_SRC,
                          RX_DST, cfdpClass, (cfdpClass == Cfdp::Class::CLASS_1) ? 0 : 1);
    this->component.doDispatch();
}

void CfdpManagerTester::rxStagingSendData(TransactionSeq seq, Cfdp::Class::T cfdpClass) {
    this->sendFileDataPdu(RX_CHANNEL, TEST_GROUND_EID, this->component.getLocalEidParam(), seq, 0, RX_CHUNK_SIZE,
                          RX_NEW_DATA, cfdpClass);
    this->component.doDispatch();
}

void CfdpManagerTester::rxStagingSendEof(TransactionSeq seq,
                                         Cfdp::Class::T cfdpClass,
                                         Cfdp::ConditionCode conditionCode,
                                         U32 checksum) {
    this->sendEofPdu(RX_CHANNEL, TEST_GROUND_EID, this->component.getLocalEidParam(), seq, conditionCode, checksum,
                     RX_CHUNK_SIZE, cfdpClass);
    this->component.doDispatch();
}

void CfdpManagerTester::rxStagingExpectFailed(Cfdp::ConditionCode conditionCode) {
    ASSERT_EQ(1u, this->eventHistory_RxFileTransferFailed->size()) << "expected one RxFileTransferFailed";
    EXPECT_EQ(static_cast<U8>(conditionCode), this->eventHistory_RxFileTransferFailed->at(0).conditionCode);
}

bool CfdpManagerTester::rxStagingRunUntilFreed(TransactionSeq seq) {
    bool freed = (this->findTransaction(RX_CHANNEL, seq) == nullptr);
    for (U32 cycle = 0; (cycle < RX_MAX_CYCLES) && !freed; ++cycle) {
        // Events are kept for the assertions; sent PDUs and telemetry are dropped so a long run
        // does not exhaust the tester's fixed-size histories
        this->clearFromPortHistory();
        this->clearTlm();
        this->m_pduCopyCount = 0;
        this->invoke_to_run1Hz(0, 0);
        this->component.doDispatch();
        freed = (this->findTransaction(RX_CHANNEL, seq) == nullptr);
    }
    return freed;
}

void CfdpManagerTester::rxStagingRunClass2ToFinAck(TransactionSeq seq) {
    Transaction* txn = this->findTransaction(RX_CHANNEL, seq);
    ASSERT_NE(nullptr, txn);
    for (U32 cycle = 0;
         (cycle < RX_MAX_CYCLES) && (txn->m_state_data.receive.sub_state != RxSubState::RX_SUB_STATE_CLOSEOUT_SYNC);
         ++cycle) {
        this->clearFromPortHistory();
        this->clearTlm();
        this->m_pduCopyCount = 0;
        this->invoke_to_run1Hz(0, 0);
        this->component.doDispatch();
    }
    ASSERT_EQ(RxSubState::RX_SUB_STATE_CLOSEOUT_SYNC, txn->m_state_data.receive.sub_state) << "FIN was never sent";

    this->sendAckPdu(RX_CHANNEL, TEST_GROUND_EID, this->component.getLocalEidParam(), seq,
                     Cfdp::FileDirective::FILE_DIRECTIVE_FIN, 1, Cfdp::ConditionCode::CONDITION_CODE_NO_ERROR,
                     Cfdp::AckTxnStatus::ACK_TXN_STATUS_TERMINATED);
    this->component.doDispatch();
    ASSERT_TRUE(this->rxStagingRunUntilFreed(seq));
}

void CfdpManagerTester::rxStagingClass1Segments(TransactionSeq seq,
                                                const U8* payload,
                                                FileSize fileSize,
                                                const FileSize* offsets,
                                                const U16* sizes,
                                                U32 count) {
    this->rxStagingSendMetadata(seq, Cfdp::Class::CLASS_1, fileSize);
    for (U32 i = 0; i < count; ++i) {
        this->sendFileDataPdu(RX_CHANNEL, TEST_GROUND_EID, this->component.getLocalEidParam(), seq, offsets[i],
                              sizes[i], &payload[offsets[i]], Cfdp::Class::CLASS_1);
        this->component.doDispatch();
    }
    // The EOF declares the full size and carries the checksum of the full payload
    this->sendEofPdu(RX_CHANNEL, TEST_GROUND_EID, this->component.getLocalEidParam(), seq,
                     Cfdp::ConditionCode::CONDITION_CODE_NO_ERROR, crcOf(payload, fileSize), fileSize,
                     Cfdp::Class::CLASS_1);
    this->component.doDispatch();
}

bool CfdpManagerTester::rxStagingCrossDeviceTmpDir(const char*& tmpDir) {
    // /dev/shm is a tmpfs on Linux; it only gives a real cross-device move when it is not on the
    // same file system as the test output directory
    tmpDir = "/dev/shm";
    struct stat tmpStat;
    struct stat outStat;
    return (::stat(tmpDir, &tmpStat) == 0) && (::stat("test/ut/output", &outStat) == 0) &&
           (tmpStat.st_dev != outStat.st_dev);
}

void CfdpManagerTester::rxStagingTimeout(Cfdp::Class::T cfdpClass, bool existing, TransactionSeq seq) {
    Fw::String staging;
    this->rxStagingPath(seq, staging);
    this->rxStagingConfigure("test/ut/output");
    this->rxStagingCleanup(seq);
    if (existing) {
        this->rxStagingWriteFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));
    }

    // Metadata declares 100 bytes, one 10-byte File Data PDU arrives, then the sender goes quiet
    this->rxStagingSendMetadata(seq, cfdpClass, RX_DECLARED_SIZE);
    this->rxStagingSendData(seq, cfdpClass);
    ASSERT_TRUE(this->rxStagingRunUntilFreed(seq)) << "transaction should be recycled after it times out";

    if (existing) {
        this->rxStagingExpectFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));
    } else {
        this->rxStagingExpectNoFile(RX_DST);
    }
    this->rxStagingExpectNoFile(staging.toChar());
    ASSERT_EVENTS_RxFileTransferCompleted_SIZE(0);
    ASSERT_EVENTS_FileRemoveFailed_SIZE(0);
    if (cfdpClass == Cfdp::Class::CLASS_1) {
        // A Class 1 receive ends on its inactivity timer and is reported as failed
        ASSERT_EVENTS_RxInactivityTimeout_SIZE(1);
        ASSERT_EVENTS_DanglingFileHandleClosed_SIZE(0);
        this->rxStagingExpectFailed(Cfdp::ConditionCode::CONDITION_CODE_INACTIVITY_DETECTED);
    } else {
        ASSERT_EVENTS_RxFileTransferFailed_SIZE(1);
    }

    this->rxStagingCleanup(seq);
}

void CfdpManagerTester::rxStagingCrcMismatch(Cfdp::Class::T cfdpClass, TransactionSeq seq) {
    Fw::String staging;
    this->rxStagingPath(seq, staging);
    this->rxStagingConfigure("test/ut/output");
    this->rxStagingCleanup(seq);
    this->rxStagingWriteFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));

    const U32 badCrc = crcOf(RX_NEW_DATA, sizeof(RX_NEW_DATA)) ^ 0x1u;
    this->rxStagingSendMetadata(seq, cfdpClass, RX_CHUNK_SIZE);
    this->rxStagingSendData(seq, cfdpClass);
    this->rxStagingSendEof(seq, cfdpClass, Cfdp::ConditionCode::CONDITION_CODE_NO_ERROR, badCrc);
    if (cfdpClass == Cfdp::Class::CLASS_2) {
        this->rxStagingRunClass2ToFinAck(seq);
    }

    this->rxStagingExpectFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));
    this->rxStagingExpectNoFile(staging.toChar());
    ASSERT_EVENTS_RxCrcMismatch_SIZE(1);
    ASSERT_EVENTS_RxFileTransferCompleted_SIZE(0);
    this->rxStagingExpectFailed(Cfdp::ConditionCode::CONDITION_CODE_FILE_CHECKSUM_FAILURE);

    this->rxStagingCleanup(seq);
}

void CfdpManagerTester::rxStagingLateMetadataCommit(const char* tmpDir, TransactionSeq seq) {
    Fw::String staging;
    Fw::String tmp;
    this->rxStagingConfigure(tmpDir);
    this->rxStagingPath(seq, staging);
    this->rxStagingTmpPath(seq, tmp);
    this->rxStagingCleanup(seq);
    this->rxStagingWriteFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));

    // File Data before Metadata goes to the temp file; the destination is untouched until commit
    this->rxStagingSendData(seq, Cfdp::Class::CLASS_2);
    this->rxStagingSendMetadata(seq, Cfdp::Class::CLASS_2, RX_CHUNK_SIZE);
    this->rxStagingExpectNoFile(tmp.toChar());
    this->rxStagingExpectFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));

    this->rxStagingSendEof(seq, Cfdp::Class::CLASS_2, Cfdp::ConditionCode::CONDITION_CODE_NO_ERROR,
                           crcOf(RX_NEW_DATA, sizeof(RX_NEW_DATA)));
    this->rxStagingRunClass2ToFinAck(seq);

    this->rxStagingExpectFile(RX_DST, RX_NEW_DATA, sizeof(RX_NEW_DATA));
    this->rxStagingExpectNoFile(staging.toChar());
    this->rxStagingExpectNoFile(tmp.toChar());
    ASSERT_EVENTS_RxFileTransferCompleted_SIZE(1);
    ASSERT_EVENTS_RxFileTransferFailed_SIZE(0);
    ASSERT_EVENTS_RxFileRenameFailed_SIZE(0);

    this->rxStagingCleanup(seq);
}

// ----------------------------------------------------------------------
// Tests
// ----------------------------------------------------------------------

void CfdpManagerTester::testRxStagingClass1TimeoutLeavesNoPartialFile() {
    this->rxStagingTimeout(Cfdp::Class::CLASS_1, false, 7001);
}

void CfdpManagerTester::testRxStagingClass1TimeoutKeepsExistingFile() {
    this->rxStagingTimeout(Cfdp::Class::CLASS_1, true, 7002);
}

void CfdpManagerTester::testRxStagingClass2TimeoutLeavesNoPartialFile() {
    this->rxStagingTimeout(Cfdp::Class::CLASS_2, false, 7003);
}

void CfdpManagerTester::testRxStagingClass2TimeoutKeepsExistingFile() {
    this->rxStagingTimeout(Cfdp::Class::CLASS_2, true, 7004);
}

void CfdpManagerTester::testRxStagingClass1CommitReplacesExistingFile() {
    const TransactionSeq seq = 7005;
    Fw::String staging;
    this->rxStagingPath(seq, staging);
    this->rxStagingConfigure("test/ut/output");
    this->rxStagingCleanup(seq);
    this->rxStagingWriteFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));

    this->rxStagingSendMetadata(seq, Cfdp::Class::CLASS_1, RX_CHUNK_SIZE);
    this->rxStagingSendData(seq, Cfdp::Class::CLASS_1);
    // Until the transfer is verified, the data is only in the staging file
    this->rxStagingExpectFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));
    this->rxStagingSendEof(seq, Cfdp::Class::CLASS_1, Cfdp::ConditionCode::CONDITION_CODE_NO_ERROR,
                           crcOf(RX_NEW_DATA, sizeof(RX_NEW_DATA)));

    this->rxStagingExpectFile(RX_DST, RX_NEW_DATA, sizeof(RX_NEW_DATA));
    this->rxStagingExpectNoFile(staging.toChar());
    ASSERT_EVENTS_RxFileTransferCompleted_SIZE(1);
    ASSERT_EVENTS_RxFileTransferFailed_SIZE(0);

    this->rxStagingCleanup(seq);
}

void CfdpManagerTester::testRxStagingClass1CrcMismatchKeepsExistingFile() {
    this->rxStagingCrcMismatch(Cfdp::Class::CLASS_1, 7006);
}

void CfdpManagerTester::testRxStagingClass2CrcMismatchKeepsExistingFile() {
    this->rxStagingCrcMismatch(Cfdp::Class::CLASS_2, 7007);
}

void CfdpManagerTester::testRxStagingClass1EofCancelKeepsExistingFile() {
    const TransactionSeq seq = 7008;
    Fw::String staging;
    this->rxStagingPath(seq, staging);
    this->rxStagingConfigure("test/ut/output");
    this->rxStagingCleanup(seq);
    this->rxStagingWriteFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));

    this->rxStagingSendMetadata(seq, Cfdp::Class::CLASS_1, RX_DECLARED_SIZE);
    this->rxStagingSendData(seq, Cfdp::Class::CLASS_1);
    this->rxStagingSendEof(seq, Cfdp::Class::CLASS_1, Cfdp::ConditionCode::CONDITION_CODE_CANCEL_REQUEST_RECEIVED, 0);

    this->rxStagingExpectFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));
    this->rxStagingExpectNoFile(staging.toChar());
    ASSERT_EVENTS_RxFileTransferCompleted_SIZE(0);
    ASSERT_EVENTS_RxFileTransferFailed_SIZE(1);

    this->rxStagingCleanup(seq);
}

void CfdpManagerTester::testRxStagingClass2LateMetadataCommits() {
    this->rxStagingLateMetadataCommit("test/ut/output", 7009);
}

void CfdpManagerTester::testRxStagingClass2LateMetadataCrossDeviceCommits() {
    const char* tmpDir = nullptr;
    if (!this->rxStagingCrossDeviceTmpDir(tmpDir)) {
        GTEST_SKIP() << "no tmpfs on a different file system than test/ut/output";
    }
    this->rxStagingLateMetadataCommit(tmpDir, 7010);
}

void CfdpManagerTester::testRxStagingClass2LateMetadataCrossDeviceCopyFailure() {
    const TransactionSeq seq = 7011;
    const char* tmpDir = nullptr;
    if (!this->rxStagingCrossDeviceTmpDir(tmpDir)) {
        GTEST_SKIP() << "no tmpfs on a different file system than test/ut/output";
    }
    Fw::String staging;
    Fw::String tmp;
    this->rxStagingConfigure(tmpDir);
    this->rxStagingPath(seq, staging);
    this->rxStagingTmpPath(seq, tmp);
    this->rxStagingCleanup(seq);
    this->rxStagingWriteFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));

    this->rxStagingSendData(seq, Cfdp::Class::CLASS_2);

    // Limit file size below the received data so the cross-device copy into the staging file fails
    // part way through, as on a full file system
    struct rlimit saved;
    ASSERT_EQ(0, ::getrlimit(RLIMIT_FSIZE, &saved));
    struct rlimit limited = saved;
    limited.rlim_cur = RX_CHUNK_SIZE / 2;
    void (*savedHandler)(int) = std::signal(SIGXFSZ, SIG_IGN);
    ASSERT_EQ(0, ::setrlimit(RLIMIT_FSIZE, &limited));
    this->rxStagingSendMetadata(seq, Cfdp::Class::CLASS_2, RX_CHUNK_SIZE);
    ASSERT_EQ(0, ::setrlimit(RLIMIT_FSIZE, &saved));
    (void)std::signal(SIGXFSZ, savedHandler);

    ASSERT_EVENTS_RxFileRenameFailed_SIZE(1);
    this->rxStagingExpectNoFile(staging.toChar());
    this->rxStagingExpectFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));

    // A retransmitted Metadata PDU must not retry the move on a failed transaction
    this->rxStagingSendMetadata(seq, Cfdp::Class::CLASS_2, RX_CHUNK_SIZE);
    ASSERT_EVENTS_RxFileRenameFailed_SIZE(1);

    ASSERT_TRUE(this->rxStagingRunUntilFreed(seq));
    this->rxStagingExpectFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));
    this->rxStagingExpectNoFile(staging.toChar());
    this->rxStagingExpectNoFile(tmp.toChar());
    ASSERT_EVENTS_RxFileTransferCompleted_SIZE(0);

    this->rxStagingCleanup(seq);
}

void CfdpManagerTester::testRxStagingClass2LateMetadataSizeMismatchThenRetransmit() {
    const TransactionSeq seq = 7012;
    Fw::String staging;
    Fw::String tmp;
    this->rxStagingConfigure("test/ut/output");
    this->rxStagingPath(seq, staging);
    this->rxStagingTmpPath(seq, tmp);
    this->rxStagingCleanup(seq);
    this->rxStagingWriteFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));

    // File Data and EOF arrive before Metadata, then a Metadata PDU whose size disagrees with the EOF
    this->rxStagingSendData(seq, Cfdp::Class::CLASS_2);
    this->rxStagingSendEof(seq, Cfdp::Class::CLASS_2, Cfdp::ConditionCode::CONDITION_CODE_NO_ERROR,
                           crcOf(RX_NEW_DATA, sizeof(RX_NEW_DATA)));
    this->rxStagingSendMetadata(seq, Cfdp::Class::CLASS_2, RX_DECLARED_SIZE);
    ASSERT_EVENTS_RxEofMdSizeMismatch_SIZE(1);

    // A retransmitted, consistent Metadata PDU and more File Data must not reach the destination
    this->rxStagingSendMetadata(seq, Cfdp::Class::CLASS_2, RX_CHUNK_SIZE);
    this->rxStagingSendData(seq, Cfdp::Class::CLASS_2);
    this->rxStagingExpectFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));
    Transaction* txn = this->findTransaction(RX_CHANNEL, seq);
    ASSERT_NE(nullptr, txn);
    EXPECT_EQ(TxnStatus::TXN_STATUS_FILE_SIZE_ERROR, txn->m_history->txn_stat);
    EXPECT_FALSE(txn->m_flags.rx.md_recv);

    ASSERT_TRUE(this->rxStagingRunUntilFreed(seq));
    this->rxStagingExpectFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));
    this->rxStagingExpectNoFile(staging.toChar());
    this->rxStagingExpectNoFile(tmp.toChar());
    ASSERT_EVENTS_RxFileTransferCompleted_SIZE(0);

    this->rxStagingCleanup(seq);
}

void CfdpManagerTester::testRxStagingExistingStagingFileNotReplaced() {
    const TransactionSeq seq = 7013;
    Fw::String staging;
    this->rxStagingPath(seq, staging);
    this->rxStagingConfigure("test/ut/output");
    this->rxStagingCleanup(seq);
    this->rxStagingWriteFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));
    // A file already at the staging path is not this transaction's
    this->rxStagingWriteFile(staging.toChar(), RX_FOREIGN, sizeof(RX_FOREIGN));

    this->rxStagingSendMetadata(seq, Cfdp::Class::CLASS_1, RX_CHUNK_SIZE);
    this->rxStagingSendData(seq, Cfdp::Class::CLASS_1);
    this->rxStagingSendEof(seq, Cfdp::Class::CLASS_1, Cfdp::ConditionCode::CONDITION_CODE_NO_ERROR,
                           crcOf(RX_NEW_DATA, sizeof(RX_NEW_DATA)));

    ASSERT_EVENTS_RxFileCreateFailed_SIZE(1);
    ASSERT_EVENTS_RxFileCreateFailed(0, Cfdp::Class::CLASS_1, TEST_GROUND_EID, seq, staging.toChar(),
                                     Os::File::FILE_EXISTS);
    this->rxStagingExpectFile(staging.toChar(), RX_FOREIGN, sizeof(RX_FOREIGN));
    this->rxStagingExpectFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));
    ASSERT_EVENTS_RxFileTransferCompleted_SIZE(0);
    this->rxStagingExpectFailed(Cfdp::ConditionCode::CONDITION_CODE_FILESTORE_REJECTION);

    this->rxStagingCleanup(seq);
}

void CfdpManagerTester::testRxStagingClass2LateMetadataExistingStagingFileNotReplaced() {
    const TransactionSeq seq = 7014;
    Fw::String staging;
    Fw::String tmp;
    this->rxStagingConfigure("test/ut/output");
    this->rxStagingPath(seq, staging);
    this->rxStagingTmpPath(seq, tmp);
    this->rxStagingCleanup(seq);
    this->rxStagingWriteFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));
    this->rxStagingWriteFile(staging.toChar(), RX_FOREIGN, sizeof(RX_FOREIGN));

    this->rxStagingSendData(seq, Cfdp::Class::CLASS_2);
    this->rxStagingSendMetadata(seq, Cfdp::Class::CLASS_2, RX_CHUNK_SIZE);
    ASSERT_EVENTS_RxFileCreateFailed_SIZE(1);

    // Retransmitted Metadata on the failed transaction is ignored
    this->rxStagingSendMetadata(seq, Cfdp::Class::CLASS_2, RX_CHUNK_SIZE);
    ASSERT_EVENTS_RxFileCreateFailed_SIZE(1);

    ASSERT_TRUE(this->rxStagingRunUntilFreed(seq));
    this->rxStagingExpectFile(staging.toChar(), RX_FOREIGN, sizeof(RX_FOREIGN));
    this->rxStagingExpectFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));
    this->rxStagingExpectNoFile(tmp.toChar());
    ASSERT_EVENTS_RxFileTransferCompleted_SIZE(0);

    this->rxStagingCleanup(seq);
}

void CfdpManagerTester::testRxStagingCommitFailureKeepsDestination() {
    const TransactionSeq seq = 7015;
    Fw::String staging;
    this->rxStagingPath(seq, staging);
    this->rxStagingConfigure("test/ut/output");
    this->rxStagingCleanup(seq);
    // The destination is a non-empty directory, so the final rename fails
    ASSERT_EQ(Os::FileSystem::OP_OK, Os::FileSystem::createDirectory(RX_DST));
    this->rxStagingWriteFile(RX_SENTINEL, RX_ORIGINAL, sizeof(RX_ORIGINAL));

    this->rxStagingSendMetadata(seq, Cfdp::Class::CLASS_1, RX_CHUNK_SIZE);
    this->rxStagingSendData(seq, Cfdp::Class::CLASS_1);
    this->rxStagingSendEof(seq, Cfdp::Class::CLASS_1, Cfdp::ConditionCode::CONDITION_CODE_NO_ERROR,
                           crcOf(RX_NEW_DATA, sizeof(RX_NEW_DATA)));

    ASSERT_EVENTS_RxFileRenameFailed_SIZE(1);
    EXPECT_EQ(Os::FileSystem::DIRECTORY, Os::FileSystem::getPathType(RX_DST));
    this->rxStagingExpectFile(RX_SENTINEL, RX_ORIGINAL, sizeof(RX_ORIGINAL));
    this->rxStagingExpectNoFile(staging.toChar());
    ASSERT_EVENTS_RxFileTransferCompleted_SIZE(0);
    this->rxStagingExpectFailed(Cfdp::ConditionCode::CONDITION_CODE_FILESTORE_REJECTION);

    this->rxStagingCleanup(seq);
}

void CfdpManagerTester::testRxStagingPathFormat() {
    const TransactionSeq seq = 7016;
    this->rxStagingConfigure("test/ut/output");
    this->rxStagingCleanup(seq);
    this->rxStagingSendMetadata(seq, Cfdp::Class::CLASS_1, RX_DECLARED_SIZE);
    Transaction* txn = this->findTransaction(RX_CHANNEL, seq);
    ASSERT_NE(nullptr, txn);

    Fw::String expected;
    Fw::String staging;
    this->rxStagingPath(seq, expected);
    ASSERT_EQ(Cfdp::Status::SUCCESS, txn->rFormatStagingPath(Fw::String(RX_DST), staging));
    EXPECT_STREQ(expected.toChar(), staging.toChar());

    // A destination too long for the suffix is refused rather than truncated
    char longDst[FW_FIXED_LENGTH_STRING_SIZE];
    ::memset(longDst, 'a', sizeof(longDst) - 1);
    longDst[sizeof(longDst) - 1] = '\0';
    EXPECT_EQ(Cfdp::Status::ERROR, txn->rFormatStagingPath(Fw::String(longDst), staging));

    ASSERT_TRUE(this->rxStagingRunUntilFreed(seq));
    this->rxStagingCleanup(seq);
}

void CfdpManagerTester::testRxStagingClass1MissingTrailingZerosNotCommitted() {
    const TransactionSeq seq = 7017;
    Fw::String staging;
    this->rxStagingPath(seq, staging);
    this->rxStagingConfigure("test/ut/output");
    this->rxStagingCleanup(seq);
    this->rxStagingWriteFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));

    // 10 data bytes followed by 90 zero bytes; zeros add nothing to the CFDP checksum, so the
    // checksum of the 10 bytes received matches the checksum of the whole file
    U8 payload[RX_DECLARED_SIZE] = {};
    ::memcpy(payload, RX_NEW_DATA, sizeof(RX_NEW_DATA));
    const FileSize offsets[] = {0};
    const U16 sizes[] = {RX_CHUNK_SIZE};
    this->rxStagingClass1Segments(seq, payload, RX_DECLARED_SIZE, offsets, sizes, 1);

    this->rxStagingExpectFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));
    this->rxStagingExpectNoFile(staging.toChar());
    ASSERT_EVENTS_RxFileTransferCompleted_SIZE(0);
    this->rxStagingExpectFailed(Cfdp::ConditionCode::CONDITION_CODE_FILE_SIZE_ERROR);

    this->rxStagingCleanup(seq);
}

void CfdpManagerTester::testRxStagingClass1MissingMiddleSegmentNotCommitted() {
    const TransactionSeq seq = 7018;
    Fw::String staging;
    this->rxStagingPath(seq, staging);
    this->rxStagingConfigure("test/ut/output");
    this->rxStagingCleanup(seq);
    this->rxStagingWriteFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));

    // The missing middle segment is zeros, so the checksum alone cannot detect it, and the last
    // segment makes the staging file reach the declared size
    U8 payload[3 * RX_CHUNK_SIZE] = {};
    ::memcpy(&payload[0], RX_NEW_DATA, RX_CHUNK_SIZE);
    ::memcpy(&payload[2 * RX_CHUNK_SIZE], RX_NEW_DATA, RX_CHUNK_SIZE);
    const FileSize offsets[] = {0, 2 * RX_CHUNK_SIZE};
    const U16 sizes[] = {RX_CHUNK_SIZE, RX_CHUNK_SIZE};
    this->rxStagingClass1Segments(seq, payload, sizeof(payload), offsets, sizes, 2);

    this->rxStagingExpectFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));
    this->rxStagingExpectNoFile(staging.toChar());
    ASSERT_EVENTS_RxFileTransferCompleted_SIZE(0);
    this->rxStagingExpectFailed(Cfdp::ConditionCode::CONDITION_CODE_FILE_SIZE_ERROR);

    this->rxStagingCleanup(seq);
}

void CfdpManagerTester::testRxStagingClass1OutOfOrderSegmentsCommit() {
    const TransactionSeq seq = 7019;
    Fw::String staging;
    this->rxStagingPath(seq, staging);
    this->rxStagingConfigure("test/ut/output");
    this->rxStagingCleanup(seq);
    this->rxStagingWriteFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));

    U8 payload[3 * RX_CHUNK_SIZE];
    for (FwSizeType i = 0; i < sizeof(payload); ++i) {
        payload[i] = static_cast<U8>(i + 1);
    }
    const FileSize offsets[] = {2 * RX_CHUNK_SIZE, 0, RX_CHUNK_SIZE};
    const U16 sizes[] = {RX_CHUNK_SIZE, RX_CHUNK_SIZE, RX_CHUNK_SIZE};
    this->rxStagingClass1Segments(seq, payload, sizeof(payload), offsets, sizes, 3);

    this->rxStagingExpectFile(RX_DST, payload, sizeof(payload));
    this->rxStagingExpectNoFile(staging.toChar());
    ASSERT_EVENTS_RxFileTransferCompleted_SIZE(1);
    ASSERT_EVENTS_RxFileTransferFailed_SIZE(0);

    this->rxStagingCleanup(seq);
}

void CfdpManagerTester::testRxStagingClass1DuplicateSegmentCommits() {
    const TransactionSeq seq = 7020;
    Fw::String staging;
    this->rxStagingPath(seq, staging);
    this->rxStagingConfigure("test/ut/output");
    this->rxStagingCleanup(seq);
    this->rxStagingWriteFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));

    // A segment whose whole range was already received, here resent with different bytes, is ignored:
    // it is not written again and not counted twice in the checksum, so the file matches the
    // checksummed data
    U8 payload[2 * RX_CHUNK_SIZE];
    for (FwSizeType i = 0; i < sizeof(payload); ++i) {
        payload[i] = static_cast<U8>(i + 1);
    }
    const FileSize offsets[] = {0, RX_CHUNK_SIZE};
    const U16 sizes[] = {RX_CHUNK_SIZE, RX_CHUNK_SIZE};
    this->rxStagingSendMetadata(seq, Cfdp::Class::CLASS_1, sizeof(payload));
    for (U32 i = 0; i < 2; ++i) {
        this->sendFileDataPdu(RX_CHANNEL, TEST_GROUND_EID, this->component.getLocalEidParam(), seq, offsets[i],
                              sizes[i], &payload[offsets[i]], Cfdp::Class::CLASS_1);
        this->component.doDispatch();
    }
    this->sendFileDataPdu(RX_CHANNEL, TEST_GROUND_EID, this->component.getLocalEidParam(), seq, 0, RX_CHUNK_SIZE,
                          RX_FOREIGN_CHUNK, Cfdp::Class::CLASS_1);
    this->component.doDispatch();
    this->sendEofPdu(RX_CHANNEL, TEST_GROUND_EID, this->component.getLocalEidParam(), seq,
                     Cfdp::ConditionCode::CONDITION_CODE_NO_ERROR, crcOf(payload, sizeof(payload)), sizeof(payload),
                     Cfdp::Class::CLASS_1);
    this->component.doDispatch();

    this->rxStagingExpectFile(RX_DST, payload, sizeof(payload));
    this->rxStagingExpectNoFile(staging.toChar());
    ASSERT_EVENTS_RxFileTransferCompleted_SIZE(1);
    ASSERT_EVENTS_RxFileTransferFailed_SIZE(0);

    this->rxStagingCleanup(seq);
}

void CfdpManagerTester::testRxStagingClass1ZeroLengthCommits() {
    const TransactionSeq seq = 7021;
    Fw::String staging;
    this->rxStagingPath(seq, staging);
    this->rxStagingConfigure("test/ut/output");
    this->rxStagingCleanup(seq);
    this->rxStagingWriteFile(RX_DST, RX_ORIGINAL, sizeof(RX_ORIGINAL));

    // A zero-length file has no File Data PDUs and nothing missing
    this->rxStagingClass1Segments(seq, RX_NEW_DATA, 0, nullptr, nullptr, 0);

    FwSizeType size = 1;
    ASSERT_EQ(Os::FileSystem::OP_OK, Os::FileSystem::getFileSize(RX_DST, size));
    EXPECT_EQ(0u, size);
    this->rxStagingExpectNoFile(staging.toChar());
    ASSERT_EVENTS_RxFileTransferCompleted_SIZE(1);
    ASSERT_EVENTS_RxFileTransferFailed_SIZE(0);

    this->rxStagingCleanup(seq);
}

}  // namespace Cfdp
}  // namespace Ccsds
}  // namespace Svc
