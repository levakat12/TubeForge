#pragma once

#include <string>
#include <string_view>

namespace nts::ecosystem
{
/** Identifier stored in signed manifests. Manifests carrying any other algorithm
    string are rejected, including the removed unpadded `rsa-sha256-raw-v1` scheme.
*/
inline constexpr const char* signatureAlgorithmId = "rsa-pkcs1-sha256-v1";

/** Signing and verification both refuse moduli below this size. */
inline constexpr int minimumSignatureModulusBits = 2048;

/** Signs the canonical manifest text with RSASSA-PKCS1-v1_5 over SHA-256 (RFC 8017).

    Returns the signature as a fixed-width lowercase hex string exactly as wide as
    the modulus, or an empty string with `error` set.

    `juce::RSAKey` only exposes the raw modular exponentiation primitive, so the
    EMSA-PKCS1-v1_5 padding is built here rather than by the key class.
*/
[[nodiscard]] std::string signCanonicalText(std::string_view canonical,
                                            std::string_view encodedPrivateKey,
                                            std::string& error);

/** Verifies a signature produced by signCanonicalText.

    The full encoded message is reconstructed from `canonical` and compared whole.
    Nothing is parsed back out of the recovered value, which is what keeps
    signature forgery (Bleichenbacher-style padding tricks, and small-exponent
    root extraction against a short unpadded digest) infeasible.
*/
[[nodiscard]] bool verifyCanonicalText(std::string_view canonical,
                                       std::string_view signatureHex,
                                       std::string_view encodedPublicKey,
                                       std::string& error);
} // namespace nts::ecosystem
