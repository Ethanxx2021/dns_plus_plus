#include "crypto/Paillier.h"
#include "crypto/Heps.h"
#include <iostream>
#include <chrono>

int main() {
    Heps heps;
    heps.init();

    mpz_class n, mu;
    heps.getPublicKey(n, mu);

    const int ITERATIONS = 1000;

    // --- Benchmark blindNotification ---
    {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < ITERATIONS; i++) {
            heps.blindNotification("test.service.example");
        }
        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count() / ITERATIONS;
        std::cout << "blindNotification: " << ms << " ms/op" << std::endl;
    }

    // --- Benchmark blindSubscription ---
    {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < ITERATIONS; i++) {
            heps.blindSubscription("test.service.example");
        }
        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count() / ITERATIONS;
        std::cout << "blindSubscription: " << ms << " ms/op" << std::endl;
    }

    // --- Benchmark executeMatch ---
    {
        // Pre-compute blinded values
        std::string bval_n = heps.blindNotification("test.service.example");
        auto [bval_m1, bval_m2] = heps.blindSubscription("test.service.example");

        // Parse to mpz
        mpz_class bn, bm1, bm2;
        mpz_set_str(bn.get_mpz_t(), bval_n.c_str(), 16);
        mpz_set_str(bm1.get_mpz_t(), bval_m1.c_str(), 16);
        mpz_set_str(bm2.get_mpz_t(), bval_m2.c_str(), 16);

        mpz_class n_sq = n * n;

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < ITERATIONS; i++) {
            // Replicate executeMatch logic
            mpz_class y1 = (bn * bm1) % n_sq;
            mpz_class L_y1 = (y1 - 1) / n;
            mpz_class diff1 = (L_y1 * mu) % n;

            mpz_class y2 = (bn * bm2) % n_sq;
            mpz_class L_y2 = (y2 - 1) / n;
            mpz_class diff2 = (L_y2 * mu) % n;

            volatile bool match = (diff1 < n / 2) && (diff2 > n / 2);
            (void)match;
        }
        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count() / ITERATIONS;
        std::cout << "executeMatch: " << ms << " ms/op" << std::endl;
    }

    return 0;
}