#include <neixx/task/internal_flag.h>

#include <atomic>
#include <memory>

namespace nei {

class InternalFlag::Impl {
public:
    Impl() : valid_(std::make_shared<std::atomic<bool>>(true)) {}

    bool IsValid() const {
        return valid_->load(std::memory_order_acquire);
    }

    void Invalidate() {
        valid_->store(false, std::memory_order_release);
    }

private:
    std::shared_ptr<std::atomic<bool>> valid_;
};

InternalFlag::InternalFlag() : impl_(std::make_unique<Impl>()) {}

InternalFlag::~InternalFlag() {
    impl_->Invalidate();
}

bool InternalFlag::IsValid() const {
    return impl_->IsValid();
}

void InternalFlag::Invalidate() {
    impl_->Invalidate();
}

} // namespace nei
