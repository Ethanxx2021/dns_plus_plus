#ifndef HEPS_H
#define HEPS_H

#include "Paillier.h"
#include <string>
#include <utility>

class Heps {
public:
    void init();
    void loadState(const std::string& filename);
    
    std::string blindNotification(const std::string& name) const;
    std::pair<std::string, std::string> blindSubscription(const std::string& name) const;

    void getPublicKey(mpz_class& n_out, mpz_class& mu_out) const;

private:
    Paillier paillier_;
    mpz_class n_;
    mpz_class mu_;

    mpz_class stringToMpz(const std::string& s) const;
    std::string mpzToHex(const mpz_class& val) const;
    mpz_class hexToMpz(const std::string& hex) const;
};

#endif