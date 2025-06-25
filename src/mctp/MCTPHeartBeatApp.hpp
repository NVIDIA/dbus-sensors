#ifndef MCTP_HEARTBEAT_APP_HPP
#define MCTP_HEARTBEAT_APP_HPP

#include <endian.h>

#include <cstdint>
#include <iostream>

/* MCTP VDM Command codes */
constexpr uint8_t mctpVendorCmdBootcomplete = 0x02;
constexpr uint8_t mctpVendorCmdHeartbeat = 0x03;
constexpr uint8_t mctpVendorCmdEnableHeartbeat = 0x04;
constexpr uint8_t mctpVendorCmdRestart = 0x0A;

/* Default instance ID macro */
constexpr uint8_t mctpVdmInstanceIdDefault = 0x00;

/* VDM Header macros */
constexpr uint32_t mctpVdmHdrIana = 0x1647;
constexpr uint8_t mctpVdmHdrVendorMsgType = 0x01;
constexpr uint8_t mctpVdmHdrMsgVer1 = 0x01;
constexpr uint8_t mctpVdmHdrMsgVer2 = 0x02;

constexpr int mctpCtrlHdrMsgType = 0;
constexpr uint8_t mctpCtrlHdrFlagRequest = (1 << 7);
constexpr uint8_t mctpCtrlHdrFlagDgram = (1 << 6);
constexpr uint8_t mctpCtrlHdrInstanceIdMask = 0x1F;
constexpr uint8_t mctpVendorMsgType = 0x7f;

static inline uint8_t createInstanceId()
{
    static uint8_t instanceId = mctpVdmInstanceIdDefault;

    instanceId = (instanceId)&mctpCtrlHdrInstanceIdMask;
    return instanceId;
}

static inline uint8_t getRqDgramInst()
{
    uint8_t instanceId = createInstanceId();
    uint8_t rqDgramInst = instanceId | mctpCtrlHdrFlagRequest;
    return rqDgramInst;
}

struct MctpVendorMsgHdr
{
    uint32_t iana;
    uint8_t rqDgramInst;
    uint8_t vendorMsgType;
    uint8_t commandCode;
    uint8_t msgVersion;
} __attribute__((__packed__));

static inline void
    encodeVendorCmdHeader(MctpVendorMsgHdr* mctpVdrHdr, uint8_t rqDgramInst,
                          uint8_t cmdCode)
{
    mctpVdrHdr->iana = htobe32(mctpVdmHdrIana);
    mctpVdrHdr->rqDgramInst = rqDgramInst;
    mctpVdrHdr->vendorMsgType = mctpVdmHdrVendorMsgType;
    mctpVdrHdr->commandCode = cmdCode;
    mctpVdrHdr->msgVersion = mctpVdmHdrMsgVer1;
}

struct MctpVendorCmdBootcompleteV2
{
    MctpVendorMsgHdr vdrMsgHdr;
    uint8_t slot : 2;
    uint8_t valid : 6;
    uint8_t rvsd1;
    uint8_t rvsd2;
} __attribute__((__packed__));

struct MctpVendorCmdHbenvent
{
    MctpVendorMsgHdr vdrMsgHdr;
} __attribute__((__packed__));

struct MctpVendorCmdHbenable
{
    MctpVendorMsgHdr vdrMsgHdr;
    uint8_t enable;
} __attribute__((__packed__));

struct MctpVendorCmdRestartnoti
{
    MctpVendorMsgHdr vdrMsgHdr;
} __attribute__((__packed__));

inline bool mctpEncodeVendorCmdHbenvent(MctpVendorCmdHbenvent* cmd)
{
    if (cmd == nullptr)
    {
        std::cerr << "cmd is nullptr" << std::endl;
        return false;
    }
    encodeVendorCmdHeader(&cmd->vdrMsgHdr, getRqDgramInst(),
                          mctpVendorCmdHeartbeat);
    return true;
}

inline bool mctpEncodeVendorCmdRestartnoti(MctpVendorCmdRestartnoti* cmd)
{
    if (cmd == nullptr)
    {
        std::cerr << "cmd is nullptr" << std::endl;
        return false;
    }
    encodeVendorCmdHeader(&cmd->vdrMsgHdr, getRqDgramInst(),
                          mctpVendorCmdRestart);
    return true;
}

inline bool mctpEncodeVendorCmdBootcmpltV2(MctpVendorCmdBootcompleteV2* cmd)
{
    if (cmd == nullptr)
    {
        std::cerr << "cmd is nullptr" << std::endl;
        return false;
    }

    encodeVendorCmdHeader(&cmd->vdrMsgHdr, getRqDgramInst(),
                          mctpVendorCmdBootcomplete);
    cmd->vdrMsgHdr.msgVersion = mctpVdmHdrMsgVer2;

    return true;
}

inline bool mctpEncodeVendorCmdHbenable(MctpVendorCmdHbenable* cmd)
{
    if (cmd == nullptr)
    {
        std::cerr << "cmd is nullptr" << std::endl;
        return false;
    }
    encodeVendorCmdHeader(&cmd->vdrMsgHdr, getRqDgramInst(),
                          mctpVendorCmdEnableHeartbeat);
    return true;
}

#endif
