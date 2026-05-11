#pragma once

#include <string>

class USBRecovery
{
  public:
    virtual ~USBRecovery() = default;

    virtual bool clearBulkOutHalt(const std::string& interface,
                                  std::string& status) = 0;
};

class LibusbUSBRecovery : public USBRecovery
{
  public:
    ~LibusbUSBRecovery() override = default;

    bool clearBulkOutHalt(const std::string& interface,
                          std::string& status) override;
};
