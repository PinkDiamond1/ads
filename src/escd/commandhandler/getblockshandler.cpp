#include "getblockshandler.h"
#include "command/getblocks.h"
#include "../office.hpp"
#include "helper/vipkeys.h"
#include "helper/servers.h"
#include "helper/block.h"

GetBlocksHandler::GetBlocksHandler(office& office, boost::asio::ip::tcp::socket& socket)
    : CommandHandler(office, socket) {
}

void GetBlocksHandler::onInit(std::unique_ptr<IBlockCommand> command) {
    try {
        m_command = std::unique_ptr<GetBlocks>(dynamic_cast<GetBlocks*>(command.release()));
    } catch (std::bad_cast& bc) {
        DLOG("GetBlocks bad_cast caught: %s", bc.what());
        return;
    }
}

void GetBlocksHandler::onExecute() {
    assert(m_command);
    ErrorCodes::Code errorCode = prepareResponse();

    try {
        boost::asio::write(m_socket, boost::asio::buffer(&errorCode, ERROR_CODE_LENGTH));
        if (!errorCode) {
            sendVipKeys(m_firstVipKeys);
            if(m_serversHeaders.size() > 0) {
                sendBlockHeaders();
                sendLastBlockSignatures();
                sendVipKeys(m_newVipKeys);
            }
        }
    } catch (std::exception& e) {
        DLOG("Responding to client %08X error: %s\n", m_usera.user, e.what());
    }
}

bool GetBlocksHandler::onValidate() {
    auto startedTime = time(NULL);
    int32_t diff = m_command->getTime() - startedTime;
    ErrorCodes::Code errorCode = ErrorCodes::Code::eNone;

    if(diff>1) {
        DLOG("ERROR: time in the future (%d>1s)\n", diff);
        errorCode = ErrorCodes::Code::eTimeInFuture;
    }
    else if(m_command->getBankId()!=m_offi.svid) {
        errorCode = ErrorCodes::Code::eBankNotFound;
    }

    if (errorCode) {
        try {
            boost::asio::write(m_socket, boost::asio::buffer(&errorCode, ERROR_CODE_LENGTH));
        } catch (std::exception& e) {
            DLOG("Responding to client %08X error: %s\n", m_usera.user, e.what());
        }
        return false;
    }

    return true;
}

void GetBlocksHandler::sendVipKeys(const VipKeys& vipKeys) {
    const uint32_t length = vipKeys.getLength();
    boost::asio::write(m_socket, boost::asio::buffer(&length, sizeof(length)));
    if(length > 0) {
        boost::asio::write(m_socket, boost::asio::buffer(vipKeys.getVipKeys(), sizeof(length)));
    }
}

void GetBlocksHandler::sendBlockHeaders() {
    const uint32_t numOfHeaders = m_serversHeaders.size();
    boost::asio::write(m_socket, boost::asio::buffer(&numOfHeaders, sizeof(numOfHeaders)));
    boost::asio::write(m_socket, boost::asio::buffer(m_serversHeaders));
}

void GetBlocksHandler::sendLastBlockSignatures() {
    const uint32_t numOfSignatures = m_signatures.getSignaturesOk().size();
    boost::asio::write(m_socket, boost::asio::buffer(&numOfSignatures, sizeof(numOfSignatures)));
    boost::asio::write(m_socket, boost::asio::buffer(m_signatures.getSignaturesOk()));
}

uint32_t GetBlocksHandler::readStart() {
    std::ifstream in("blk/start.txt", std::ifstream::in);
    if(in.is_open()) {
        uint32_t start{};
        in >> std::hex >> start;
        in.close();
        return start;
    }
    ELOG("FATAL ERROR: failed to read from blk/start.txt\n");
    exit(-1);
}

int GetBlocksHandler::vipSize(uint8_t* viphash) {
    char hash[65];
    hash[64]='\0';
    ed25519_key2text(hash,viphash,32);
    char filename[128];
    sprintf(filename,"vip/%64s.vip",hash);
    struct stat sb;
    stat(filename,&sb);
    return((int)sb.st_size/(2+32));
}

bool GetBlocksHandler::prepareFirstVipKeys(uint8_t* vipHash) {
    char hash[65];
    hash[64]='\0';
    ed25519_key2text(hash, vipHash, 32);
    char vipFilename[128];
    sprintf(vipFilename, "vip/%64s.vip", hash);

    m_firstVipKeys.loadFromFile(vipFilename);
    const auto length = m_firstVipKeys.getLength();

    if(length == 0) {
        DLOG("ERROR, failed to provide vip keys for start viphash %.64s\n", hash);
        return false;
    }
    if(!m_firstVipKeys.checkVipKeys(vipHash, length/(2+32))) {
        DLOG("ERROR, failed to provide %d correct vip keys for start viphash %.64s\n", length/(2+32), hash);
        return false;
    }
    return true;
}

void GetBlocksHandler::prepareNewVipKeys(uint8_t* vipHash) {
    char hash[65];
    hash[64]='\0';
    ed25519_key2text(hash, vipHash, 32);
    char vipFilename[128];
    sprintf(vipFilename, "vip/%64s.vip", hash);

    m_newVipKeys.loadFromFile(vipFilename);
    const auto length = m_newVipKeys.getLength();

    if(length == 0) {
        DLOG("ERROR, failed to provide vip keys for new viphash %.64s\n", hash);
        return;
    }
    if(!m_newVipKeys.checkVipKeys(vipHash, length/(2+32))) {
        DLOG("ERROR, failed to provide %d correct vip keys for new viphash %.64s\n", length/(2+32), hash);
        m_newVipKeys.reset();
        return;
    }

    DLOG("INFO, will send vip keys for start viphash %.64s [len:%d]\n", hash, length);
}

void GetBlocksHandler::loadLastBlockSignatures(uint32_t path) {
    m_signatures.loadSignaturesOk(path);
    uint32_t signOk = m_signatures.getSignaturesOk().size();
    if(signOk == 0) {
        DLOG("ERROR, will not send signatures for block %08X\n", path);
    }
    else {
        DLOG("INFO, will send %d signatures for block %08X\n", signOk, path);
    }
}

void GetBlocksHandler::readBlockHeaders(
    uint32_t from,
    uint32_t to,
    header_t& header,
    Helper::Block& block,
    int vipTot) {

    m_newviphash=false;
    uint32_t trystop=from+128;
    for(block.getData().ttime=from; block.getData().ttime<=to; ) {
        if(!block.readDataFromHeaderFile()) {
            break;
        }
        if(memcmp(header.viphash, block.getData().vipHash, 32)) {
            m_newviphash=true;
            to=block.getData().ttime;
        }
        header = block.getHeader();
        DLOG("INFO, adding block %08X\n", block.getData().ttime);
        m_serversHeaders.push_back(header);
        block.getData().ttime += BLOCKSEC;
        //FIXME, user different stop criterion !!! do not stop if there are not enough signatures
        if(block.getData().ttime>trystop && 2*block.getData().voteYes>=vipTot) {
            break;
        }
    }
    block.getData().ttime-=BLOCKSEC;
}

ErrorCodes::Code GetBlocksHandler::prepareResponse() {
    uint32_t from = m_command->getBlockNumberFrom() - m_command->getBlockNumberFrom()%BLOCKSEC; //TODO replace 0 by the available beginning or last checkpoint
    uint32_t to = m_command->getBlockNumberTo() - m_command->getBlockNumberTo()%BLOCKSEC;

    const uint32_t start = readStart();
    if(!start) {
        DLOG("ERROR, failed to read block start\n");
        return ErrorCodes::Code::eFailedToReadBlockStart;
    }
    int vipTot = 0;
    Block block;
    header_t header{};

    if(!from || from==to) {
        if (!from) {
            from = start;
        }
        block.getData().ttime = from;

        if(!block.readDataFromHeaderFile()) {
            DLOG("ERROR, failed to read header %08X\n", start);
            return ErrorCodes::Code::eFailedToReadBlockAtStart;
        }
        header = block.getHeader();

        if(!prepareFirstVipKeys(block.getData().vipHash)) {
            DLOG("ERROR, vip keys for start viphash %.64s will not be sent", block.getData().vipHash);
            return ErrorCodes::Code::eCouldNotReadCorrectVipKeys;
        }

        uint32_t length = m_firstVipKeys.getLength();
        vipTot=length/(2+32);
        DLOG("INFO, will send vip keys for start viphash %.64s [len:%d]\n", header.viphash, length);
    }
    else if(from < start) {
        DLOG("ERROR, failed to read block %08X before start %08X\n", from, start);
        return ErrorCodes::Code::eNoBlockInSpecifiedRange;
    }
    else {
        block.getData().ttime = from-BLOCKSEC;
        if(block.readDataFromHeaderFile()) {
            vipTot=vipSize(block.getData().vipHash);
            header = block.getHeader();
        }
    }

    if(!to) {
        to=time(NULL);
        to-=to%BLOCKSEC-BLOCKSEC;
    }

    if(from>to) {
        DLOG("ERROR, no block between %08X and %08X\n", from, to);
    }
    else {
        readBlockHeaders(from, to, header, block, vipTot);
        if(m_serversHeaders.size() > 0) {
            DLOG("Will send %lu blocks from %08X to %08X\n", m_serversHeaders.size(), from, block.getData().ttime);
            loadLastBlockSignatures(block.getData().ttime);
            if(m_newviphash) {
                prepareNewVipKeys(block.getData().vipHash);
            }
        }
        else {
            DLOG("ERROR, failed to provide blocks from %08X to %08X\n", from, block.getData().ttime);
        }
    }

    return ErrorCodes::Code::eNone;
}
