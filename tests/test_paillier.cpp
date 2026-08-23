#include "crypto/Paillier.h"
#include <iostream>

int main() {
    Paillier paillier;
    std::cout << "Generating 2048-bit keys and blinding parameters..." << std::endl;
    paillier.keyGen(2048);
    std::cout << "Keys generated." << std::endl;

    mpz_class n = paillier.getN();

    // --- Test 1: Original Homomorphic Addition ---
    std::cout << "\n--- Test 1: Original Paillier Homomorphic Addition ---" << std::endl;
    mpz_class m1 = 123456;
    mpz_class m2 = 789012;
    mpz_class c1 = paillier.encrypt(m1);
    mpz_class c2 = paillier.encrypt(m2);
    if (paillier.decrypt(paillier.add(c1, c2)) == m1 + m2) {
        std::cout << "PASS: Original Homomorphic addition works!" << std::endl;
    } else {
        std::cout << "FAIL: Original Homomorphic addition failed!" << std::endl;
        return 1;
    }

    // --- Test 2: Modified Paillier Match Protocol (x == v) ---
    std::cout << "\n--- Test 2: Match Protocol (x == v) ---" << std::endl;
    mpz_class x1 = 500;
    mpz_class v1 = 500;
    
    mpz_class bval_n1 = paillier.blindNotification(x1);
    // Subscriber encrypts -v, HEPS blinds it
    mpz_class E_neg_v1 = paillier.encrypt(-v1 + n); // Paillier requires positive m, so -v mod n
    mpz_class bval_m1 = paillier.blindSubscription(E_neg_v1);
    mpz_class diff1 = paillier.match(bval_n1, bval_m1);
    
    std::cout << "x=" << x1 << ", v=" << v1 << ", diff=" << diff1 << std::endl;
    if (diff1 < n / 2) {
        std::cout << "PASS: diff < n/2 -> x >= v (Correct)" << std::endl;
    } else {
        std::cout << "FAIL: diff > n/2 -> x < v (Incorrect)" << std::endl;
        return 1;
    }

    // --- Test 3: Modified Paillier Match Protocol (x < v) ---
    std::cout << "\n--- Test 3: Match Protocol (x < v) ---" << std::endl;
    mpz_class x2 = 100;
    mpz_class v2 = 500;
    
    mpz_class bval_n2 = paillier.blindNotification(x2);
    mpz_class E_neg_v2 = paillier.encrypt(-v2 + n);
    mpz_class bval_m2 = paillier.blindSubscription(E_neg_v2);
    mpz_class diff2 = paillier.match(bval_n2, bval_m2);
    
    std::cout << "x=" << x2 << ", v=" << v2 << ", diff=" << diff2 << std::endl;
    if (diff2 > n / 2) {
        std::cout << "PASS: diff > n/2 -> x < v (Correct)" << std::endl;
    } else {
        std::cout << "FAIL: diff < n/2 -> x >= v (Incorrect)" << std::endl;
        return 1;
    }

    // --- Test 4: Semantic Security ---
    std::cout << "\n--- Test 4: Semantic Security ---" << std::endl;
    if (bval_n1 != paillier.blindNotification(x1)) {
        std::cout << "PASS: Same plaintext produces different blinded values!" << std::endl;
    } else {
        std::cout << "FAIL: Semantic security broken!" << std::endl;
        return 1;
    }

    return 0;
}