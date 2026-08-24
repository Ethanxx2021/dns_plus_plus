#include "crypto/Paillier.h"
#include <cassert>
#include <iostream>
#include <random>
#include <stdexcept>

// RNG 在 Paillier 构造时一次性播种。旧实现每个 encrypt()/blindNotification()/
// keyGen() 调用都本地构造一个 gmp_randclass 并 rd() 播种一次，慢且随机性可疑。
Paillier::Paillier()
    : rng_(gmp_randinit_default)
{
    std::random_device rd;
    // 用两个 64 位 word 叠加，避免只喂 32 位而降低种子熵。
    unsigned long s = (static_cast<unsigned long>(rd()) << 32) ^ rd();
    rng_.seed(s);
}

// 生成一个恰好 bits 位的素数。做两件事：
//   (1) 强制最高位为 1，保证 p ∈ [2^(bits-1), 2^bits)
//   (2) 同时强制次高位为 1，保证 p ≥ (3/4) · 2^bits —— 这样两个这样的素数相乘
//       n = p·q ≥ (9/16)·2^(2·bits) > 2^(2·bits-1)，从而 n 的位长恒等于 2·bits。
//       这是 RSA 密钥生成中广泛使用的技巧；只强制最高位不够，两个各 half_bits 位
//       的素数相乘仍可能得到 (2·half_bits-1) 位的 n（本 PR 冒烟第一次实测确认）。
// mpz_nextprime 单调递增，理论上可能把结果推过 2^bits，静默接受会毁掉位长不变量。
// 因此循环校验 bit-size，超出则重采样，超过重试上限则 fail-fast（不 assert 崩溃）。
static void gen_prime(mpz_class& p, unsigned int bits, gmp_randclass& rng) {
    const int kMaxAttempts = 32;
    for (int attempt = 0; attempt < kMaxAttempts; attempt++) {
        p = rng.get_z_bits(bits);
        mpz_setbit(p.get_mpz_t(), bits - 1);   // top bit
        mpz_setbit(p.get_mpz_t(), bits - 2);   // 次高位（保证 n 的位长稳定）
        mpz_nextprime(p.get_mpz_t(), p.get_mpz_t());
        if (mpz_sizeinbase(p.get_mpz_t(), 2) == bits) {
            return;
        }
    }
    std::cerr << "FATAL: gen_prime(" << bits << ") failed to produce a "
              << bits << "-bit prime after " << kMaxAttempts << " attempts"
              << std::endl;
    throw std::runtime_error("gen_prime attempts exhausted");
}

void Paillier::keyGen(unsigned int bits) {
    unsigned int half_bits = bits / 2;
    const int kMaxKeyAttempts = 8;
    for (int attempt = 0; attempt < kMaxKeyAttempts; attempt++) {
        // p 和 q 各 half_bits 位、最高两位都为 1，理论上 n = p·q 恒为 bits 位。
        gen_prime(p, half_bits, rng_);
        gen_prime(q, half_bits, rng_);
        n = p * q;
        if (mpz_sizeinbase(n.get_mpz_t(), 2) == bits) break;
        // 极端边界情况下仍可能少一位；重采样，不静默接受。
        if (attempt == kMaxKeyAttempts - 1) {
            std::cerr << "FATAL: keyGen(" << bits << ") produced n of "
                      << mpz_sizeinbase(n.get_mpz_t(), 2) << " bits after "
                      << kMaxKeyAttempts << " attempts" << std::endl;
            throw std::runtime_error("keyGen attempts exhausted");
        }
    }
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

    e_m = rng_.get_z_range(n);
    d_m = n - e_m;

    // r_m 是 modified Paillier 的盲化随机标量。位宽约束 (u - l) 见协议规范
    // (Nabeel 2012 §4；本仓库 README Appendix D 的参数表)：
    //   u = 1024  上界：确保 r_m·(x-v) 加噪声后仍落在 λ 的可判别区间
    //   l =   32  下界：不小于服务名哈希域的位数
    // 于是 r_m ∈ [0, 2^{u-l}) = [0, 2^992)。改动这个位宽必须同步 Appendix D。
    r_m = rng_.get_z_bits(992);
    if (r_m == 0) r_m = 1;
}

mpz_class Paillier::L(const mpz_class& x) const {
    // L(x) = (x - 1) / n 只在 x ≡ 1 (mod n) 时数学上有意义 (Paillier §2)；
    // 否则整数除法会静默给出错误结果 —— 见 CLAUDE.md 密码学额外要求。
    // 用 mpz_congruent_p(x, 1_mpz, n) 而非 mpz_congruent_ui_p —— 后者要求
    // 模数是 unsigned long，装不下 2048 位的 n。
    {
        mpz_class one = 1;
        assert(mpz_congruent_p(x.get_mpz_t(), one.get_mpz_t(), n.get_mpz_t()));
    }
    return (x - 1) / n;
}

mpz_class Paillier::encrypt(const mpz_class& m) const {
    mpz_class r_orig = rng_.get_z_range(n);
    while (r_orig == 0) r_orig = rng_.get_z_range(n);

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
    mpz_class r_rand = rng_.get_z_range(r_m);
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
