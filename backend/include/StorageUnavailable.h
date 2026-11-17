#pragma once

#include <stdexcept>

// Carries no SQL, credentials or dependency error text into HTTP responses.
class StorageUnavailable : public std::runtime_error {
public:
    StorageUnavailable() : std::runtime_error("storage unavailable") {}
};
