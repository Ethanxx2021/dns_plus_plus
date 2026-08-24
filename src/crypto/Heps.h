#ifndef HEPS_H
#define HEPS_H

#include "Paillier.h"
#include <string>
#include <utility>

class Heps {
public:
    void init();
    // 返回是否成功。失败时不修改任何成员（n_、mu_、paillier_ 保持默认构造的 0），
    // 让调用方能安全地退化或 exit —— 旧接口是 void 且失败只打印一行，让 n_ 变成 0
    // 后续 blindSubscription() 会在 GMP 里对 0 取模崩溃（PR #1 已实测 SIGFPE）。
    bool loadState(const std::string& filename);
    
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