#pragma once
#include "platform.hpp"

namespace replay {
struct Credentials { unsigned short port = 4455; std::string password; };
class Integration {
public:
    explicit Integration(Paths paths);
    bool configured() const;
    Json options(const Json& settings) const;
    Json prepare(const Json& settings);
    void reset();
    Credentials credentials() const;
private:
    Paths paths_;
};
}
