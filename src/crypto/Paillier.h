#ifndef PAILLIER_H
#define PAILLIER_H

#include <gmpxx.h>
#include <string>

class Paillier {
public:
    Paillier();
    // Key Generation
    void keyGen(unsigned int bits = 2048);

    // --- Original Paillier (for baseline testing) ---
    mpz_class encrypt(const mpz_class& m) const;
    mpz_class decrypt(const mpz_class& c) const;
    mpz_class add(const mpz_class& c1, const mpz_class& c2) const;

    // --- Modified Paillier (Phase 3: Paper §3.2 & Nabeel 2012) ---
    mpz_class blindNotification(const mpz_class& x) const;
    mpz_class blindSubscription(const mpz_class& E_neg_v) const;
    mpz_class match(const mpz_class& bval_n, const mpz_class& bval_m) const;

    // Getters
    mpz_class getN() const { return n; }
    mpz_class getG() const { return g; }
    mpz_class getMu() const { return mu; }
    mpz_class getLambda() const { return lambda; }
    mpz_class getEM() const { return e_m; }
    mpz_class getDM() const { return d_m; }
    mpz_class getRM() const { return r_m; }

    // Setter for loading state
    void setParams(const mpz_class& n_in, const mpz_class& g_in, const mpz_class& mu_in, 
                   const mpz_class& lambda_in, const mpz_class& e_m_in, 
                   const mpz_class& d_m_in, const mpz_class& r_m_in);

private:
    mpz_class p, q;
    mpz_class n;
    mpz_class n_sq;
    mpz_class lambda;
    mpz_class g;
    mpz_class mu;

    mpz_class e_m, d_m;
    mpz_class r_m;

    // RNG 只在构造时用 std::random_device 播种一次，避免旧代码在 encrypt() /
    // blindNotification() / keyGen() 里每次重新构造 + 重新播种（既慢又是随机性
    // 隐患，见审计报告顺带发现 7）。mutable 是因为 encrypt() / blindNotification()
    // 等按语义是 const 但内部要抽随机数。
    mutable gmp_randclass rng_;

    mpz_class L(const mpz_class& x) const;
};

#endif