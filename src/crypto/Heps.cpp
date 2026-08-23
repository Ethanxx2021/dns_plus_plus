#include "crypto/Heps.h"
#include <iostream>
#include <fstream>
#include <functional>

void Heps::init() {
    paillier_.keyGen(2048);
    n_ = paillier_.getN();
    mu_ = paillier_.getMu();
}

void Heps::loadState(const std::string& filename) {
    std::ifstream ifs(filename);
    std::string n_hex, mu_hex, lambda_hex, e_m_hex, d_m_hex, r_m_hex;
    
    if (std::getline(ifs, n_hex) &&
        std::getline(ifs, mu_hex) &&
        std::getline(ifs, lambda_hex) &&
        std::getline(ifs, e_m_hex) &&
        std::getline(ifs, d_m_hex) &&
        std::getline(ifs, r_m_hex)) {
        
        mpz_class n_val = hexToMpz(n_hex);
        mpz_class mu_val = hexToMpz(mu_hex);
        mpz_class lambda_val = hexToMpz(lambda_hex);
        mpz_class e_m_val = hexToMpz(e_m_hex);
        mpz_class d_m_val = hexToMpz(d_m_hex);
        mpz_class r_m_val = hexToMpz(r_m_hex);
        
        paillier_.setParams(n_val, n_val + 1, mu_val, lambda_val, e_m_val, d_m_val, r_m_val);
        n_ = n_val;
        mu_ = mu_val;
    } else {
        std::cerr << "Error: Could not load HEPS state from " << filename << std::endl;
    }
}

mpz_class Heps::stringToMpz(const std::string& s) const {
    std::hash<std::string> hasher;
    size_t h = hasher(s);
    return static_cast<unsigned long>(h);
}

std::string Heps::mpzToHex(const mpz_class& val) const {
    char* str = mpz_get_str(nullptr, 16, val.get_mpz_t());
    std::string result(str);
    free(str);
    return result;
}

mpz_class Heps::hexToMpz(const std::string& hex) const {
    mpz_class val;
    mpz_set_str(val.get_mpz_t(), hex.c_str(), 16);
    return val;
}

std::string Heps::blindNotification(const std::string& name) const {
    mpz_class x = stringToMpz(name);
    mpz_class bval_n = paillier_.blindNotification(x);
    return mpzToHex(bval_n);
}

std::pair<std::string, std::string> Heps::blindSubscription(const std::string& name) const {
    mpz_class v = stringToMpz(name);
    
    mpz_class neg_v = (n_ - v) % n_;
    mpz_class E_neg_v = paillier_.encrypt(neg_v);
    mpz_class bval_m1 = paillier_.blindSubscription(E_neg_v);
    
    mpz_class neg_v1 = (n_ - (v + 1)) % n_;
    mpz_class E_neg_v1 = paillier_.encrypt(neg_v1);
    mpz_class bval_m2 = paillier_.blindSubscription(E_neg_v1);
    
    return {mpzToHex(bval_m1), mpzToHex(bval_m2)};
}

void Heps::getPublicKey(mpz_class& n_out, mpz_class& mu_out) const {
    n_out = n_;
    mu_out = mu_;
}