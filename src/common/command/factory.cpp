#include "factory.h"
#include "pods.h"
#include "getaccount.h"
#include "setaccountkey.h"
#include "createnode.h"

namespace command {

std::unique_ptr<IBlockCommand> factory::makeCommand(uint8_t type) {
    std::unique_ptr<IBlockCommand>  command;

    switch(type) {
    case TXSTYPE_INF:
        command = std::make_unique<GetAccount>();
        break;
    case TXSTYPE_KEY:
        command = std::make_unique<SetAccountKey>();
        break;
    case TXSTYPE_BNK:
        //command = std::make_unique<CreateNode>();
        break;
    default:
        break;
    }

    //assert(command);
    //assert(command->getType() != type);

    return command;
}

}
