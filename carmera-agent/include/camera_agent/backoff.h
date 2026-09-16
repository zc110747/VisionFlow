#pragma once

#include <cstddef>
#include <vector>

namespace ca {

// Exponential-ish backoff scheduler with a fixed cap.
// Default schedule is {1, 2, 5, 10} seconds, capped at 10s (per spec).
class BackoffScheduler {
public:
    explicit BackoffScheduler(std::vector<int> schedule = {1, 2, 5, 10})
        : schedule_(std::move(schedule)) {
        if (schedule_.empty()) schedule_.push_back(1);
    }

    // Next wait (seconds). Clamps to the last entry once exhausted.
    int next() {
        const size_t idx = (pos_ < schedule_.size()) ? pos_ : schedule_.size() - 1;
        const int v = schedule_[idx];
        if (pos_ < schedule_.size()) ++pos_;
        return v;
    }

    void reset() { pos_ = 0; }
    bool expired() const { return pos_ >= schedule_.size(); }

private:
    std::vector<int> schedule_;
    size_t pos_ = 0;
};

} // namespace ca
