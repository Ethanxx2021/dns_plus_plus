#include "crypto/Heps.h"
#include "crypto/Paillier.h"
#include <iostream>

int main() {
    Heps heps;
    std::cout << "Initializing HEPS (generating 2048-bit keys)..." << std::endl;
    heps.init();
    std::cout << "HEPS initialized." << std::endl;

    // Broker gets public key
    mpz_class n, mu;
    heps.getPublicKey(n, mu);
    mpz_class n_sq = n * n;

    // --- Test 1: Matching service names ---
    std::cout << "\n--- Test 1: Matching service names ---" << std::endl;
    std::string pub_name = "weather.example";
    std::string sub_name = "weather.example";

    // Publisher blinds the notification
    std::string bval_n_hex = heps.blindNotification(pub_name);
    mpz_class bval_n;
    mpz_set_str(bval_n.get_mpz_t(), bval_n_hex.c_str(), 16);

    // Subscriber blinds the subscription
    auto [bval_m1_hex, bval_m2_hex] = heps.blindSubscription(sub_name);
    mpz_class bval_m1, bval_m2;
    mpz_set_str(bval_m1.get_mpz_t(), bval_m1_hex.c_str(), 16);
    mpz_set_str(bval_m2.get_mpz_t(), bval_m2_hex.c_str(), 16);

    // Broker performs Match
    // y1 = bval_n * bval_m1 mod n^2
    mpz_class y1 = (bval_n * bval_m1) % n_sq;
    mpz_class L_y1 = (y1 - 1) / n;
    mpz_class diff1 = (L_y1 * mu) % n;

    // y2 = bval_n * bval_m2 mod n^2
    mpz_class y2 = (bval_n * bval_m2) % n_sq;
    mpz_class L_y2 = (y2 - 1) / n;
    mpz_class diff2 = (L_y2 * mu) % n;

    std::cout << "diff1 (x >= v): " << (diff1 < n / 2 ? "YES" : "NO") << std::endl;
    std::cout << "diff2 (x < v+1): " << (diff2 > n / 2 ? "YES" : "NO") << std::endl;

    if (diff1 < n / 2 && diff2 > n / 2) {
        std::cout << "PASS: Service names match!" << std::endl;
    } else {
        std::cout << "FAIL: Service names do not match!" << std::endl;
        return 1;
    }

    // --- Test 2: Non-matching service names ---
    std::cout << "\n--- Test 2: Non-matching service names ---" << std::endl;
    std::string pub_name2 = "weather.example";
    std::string sub_name2 = "traffic.example";

    std::string bval_n_hex2 = heps.blindNotification(pub_name2);
    mpz_class bval_n2;
    mpz_set_str(bval_n2.get_mpz_t(), bval_n_hex2.c_str(), 16);

    auto [bval_m1_hex2, bval_m2_hex2] = heps.blindSubscription(sub_name2);
    mpz_class bval_m1_2, bval_m2_2;
    mpz_set_str(bval_m1_2.get_mpz_t(), bval_m1_hex2.c_str(), 16);
    mpz_set_str(bval_m2_2.get_mpz_t(), bval_m2_hex2.c_str(), 16);

    mpz_class y1_2 = (bval_n2 * bval_m1_2) % n_sq;
    mpz_class L_y1_2 = (y1_2 - 1) / n;
    mpz_class diff1_2 = (L_y1_2 * mu) % n;

    mpz_class y2_2 = (bval_n2 * bval_m2_2) % n_sq;
    mpz_class L_y2_2 = (y2_2 - 1) / n;
    mpz_class diff2_2 = (L_y2_2 * mu) % n;

    std::cout << "diff1 (x >= v): " << (diff1_2 < n / 2 ? "YES" : "NO") << std::endl;
    std::cout << "diff2 (x < v+1): " << (diff2_2 > n / 2 ? "YES" : "NO") << std::endl;

    if (!(diff1_2 < n / 2 && diff2_2 > n / 2)) {
        std::cout << "PASS: Service names correctly do not match!" << std::endl;
    } else {
        std::cout << "FAIL: Service names incorrectly matched!" << std::endl;
        return 1;
    }

    return 0;
}