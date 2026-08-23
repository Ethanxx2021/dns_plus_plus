#include "crypto/Paillier.h"
#include <iostream>
#include <random>

void gen_prime(mpz_class& p, unsigned int bits, gmp_randclass& rng) {
    p = rng.get_z_bits(bits);
    mpz_nextprime(p.get_mpz_t(), p.get_mpz_t());
}

void Paillier::keyGen(unsigned int bits) {
    std::random_device rd;
    gmp_randclass rng(gmp_randinit_default);
    rng.seed(rd());

    unsigned int half_bits = bits / 2;
    gen_prime(p, half_bits, rng);
    gen_prime(q, half_bits, rng);

    n = p * q;
    n_sq = n * n;

    mpz_class p_minus_1 = p - 1;
    mpz_class q_minus_1 = q - 1;
    mpz_lcm(lambda.get_mpz_t(), p_minus_1.get_mpz_t(), q_minus_1.get_mpz_t());

    g = n + 1;

    mpz_class g_lambda;
    mpz_powm(g_lambda.get_mpz_t(), g.get_mpz_t(), lambda.get_mpz_t(), n_sq.get_mpz_t());
    mpz_class L_g_lambda = L(g_lambda);
    
    if (!mpz_invert(mu.get_mpz_t(), L_g_lambda.get_mpz_t(), n.get_mpz_t())) {
        std::cerr << "Error: mu inversion failed during keygen" << std::endl;
    }

    e_m = rng.get_z_range(n);
    d_m = n - e_m;

    r_m = rng.get_z_bits(992);
    if (r_m == 0) r_m = 1;
}

mpz_class Paillier::L(const mpz_class& x) const {
    return (x - 1) / n;
}

mpz_class Paillier::encrypt(const mpz_class& m) const {
    std::random_device rd;
    gmp_randclass rng(gmp_randinit_default);
    rng.seed(rd());

    mpz_class r_orig = rng.get_z_range(n);
    while (r_orig == 0) r_orig = rng.get_z_range(n);

    mpz_class term1 = (1 + m * n) % n_sq;
    mpz_class term2;
    mpz_powm(term2.get_mpz_t(), r_orig.get_mpz_t(), n.get_mpz_t(), n_sq.get_mpz_t());

    return (term1 * term2) % n_sq;
}

mpz_class Paillier::decrypt(const mpz_class& c) const {
    mpz_class c_lambda;
    mpz_powm(c_lambda.get_mpz_t(), c.get_mpz_t(), lambda.get_mpz_t(), n_sq.get_mpz_t());
    return (L(c_lambda) * mu) % n;
}

mpz_class Paillier::add(const mpz_class& c1, const mpz_class& c2) const {
    return (c1 * c2) % n_sq;
}

mpz_class Paillier::blindNotification(const mpz_class& x) const {
    std::random_device rd;
    gmp_randclass rng(gmp_randinit_default);
    rng.seed(rd());

    mpz_class r_rand = rng.get_z_range(r_m); 
    if (r_rand == 0) r_rand = 1;

    mpz_class exp = (r_m * x + r_rand) * lambda;
    
    mpz_class term1, term2;
    mpz_powm(term1.get_mpz_t(), g.get_mpz_t(), e_m.get_mpz_t(), n_sq.get_mpz_t());
    mpz_powm(term2.get_mpz_t(), g.get_mpz_t(), exp.get_mpz_t(), n_sq.get_mpz_t());
    
    return (term1 * term2) % n_sq;
}

mpz_class Paillier::blindSubscription(const mpz_class& E_neg_v) const {
    mpz_class exp = r_m * lambda;
    
    mpz_class term1, term2;
    mpz_powm(term1.get_mpz_t(), g.get_mpz_t(), d_m.get_mpz_t(), n_sq.get_mpz_t());
    mpz_powm(term2.get_mpz_t(), E_neg_v.get_mpz_t(), exp.get_mpz_t(), n_sq.get_mpz_t());
    return (term1 * term2) % n_sq;
}

mpz_class Paillier::match(const mpz_class& bval_n, const mpz_class& bval_m) const {
    mpz_class y = (bval_n * bval_m) % n_sq;
    mpz_class L_y = L(y);
    return (L_y * mu) % n;
}

void Paillier::setParams(const mpz_class& n_in, const mpz_class& g_in, const mpz_class& mu_in, 
                         const mpz_class& lambda_in, const mpz_class& e_m_in, 
                         const mpz_class& d_m_in, const mpz_class& r_m_in) {
    n = n_in;
    g = g_in;
    mu = mu_in;
    lambda = lambda_in;
    n_sq = n * n;
    e_m = e_m_in;
    d_m = d_m_in;
    r_m = r_m_in;
}