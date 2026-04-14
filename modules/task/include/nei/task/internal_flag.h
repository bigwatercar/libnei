#pragma once

#ifndef NEI_TASK_INTERNAL_FLAG_H
#define NEI_TASK_INTERNAL_FLAG_H

#include <memory>

#include <nei/macros/nei_export.h>

namespace nei {

class NEI_API InternalFlag final {
public:
    class Impl;

    InternalFlag();
    ~InternalFlag();

    InternalFlag(const InternalFlag&) = delete;
    InternalFlag& operator=(const InternalFlag&) = delete;

    bool IsValid() const;
    void Invalidate();

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace nei

#endif // NEI_TASK_INTERNAL_FLAG_H
