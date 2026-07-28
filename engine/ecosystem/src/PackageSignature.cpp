#include <nts/ecosystem/PackageSignature.h>

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cctype>
#include <optional>

namespace nts::ecosystem
{
namespace
{
// DER DigestInfo prefix for id-sha256, from RFC 8017 section 9.2 note 1.
constexpr const char* sha256DigestInfoPrefix = "3031300d060960864801650304020105000420";
constexpr int sha256DigestInfoBytes = 19 + 32;

int modulusBits(const juce::BigInteger& modulus) noexcept
{
    return modulus.getHighestBit() + 1;
}

int modulusBytes(const juce::BigInteger& modulus) noexcept
{
    const auto bits = modulusBits(modulus);
    return bits <= 0 ? 0 : (bits + 7) / 8;
}

bool isLowercaseHex(std::string_view text) noexcept
{
    return ! text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char character)
    { return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'); });
}

bool isHex(std::string_view text) noexcept
{
    return ! text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char character)
    { return std::isxdigit(character) != 0; });
}

/** Reads the modulus out of an encoded key.

    `juce::RSAKey` holds "exponent,modulus" as hex and exposes no accessor for
    either half, so the width the padding has to fill is recovered from the text.
*/
std::optional<juce::BigInteger> modulusOf(std::string_view encodedKey)
{
    const auto comma = encodedKey.find(',');
    if (comma == std::string_view::npos) return std::nullopt;
    const auto text = encodedKey.substr(comma + 1);
    if (! isHex(text)) return std::nullopt;
    juce::BigInteger modulus;
    modulus.parseString(juce::String::fromUTF8(text.data(), static_cast<int>(text.size())), 16);
    if (modulus.isZero()) return std::nullopt;
    return modulus;
}

juce::String toFixedWidthHex(const juce::BigInteger& value, int bytes)
{
    auto hex = value.toString(16).toLowerCase();
    const auto width = bytes * 2;
    return hex.length() >= width ? hex : juce::String::repeatedString("0", width - hex.length()) + hex;
}

/** Builds EM = 0x00 || 0x01 || PS || 0x00 || DigestInfo || H, where PS is a run of
    0xFF bytes long enough to fill the modulus width (RFC 8017 section 9.2).

    The leading zero byte keeps EM strictly below the modulus, so `applyToValue`
    stays on its single-block path and behaves as plain modular exponentiation.
*/
std::optional<juce::BigInteger> encodeMessage(std::string_view canonical, int keyBytes)
{
    if (keyBytes < sha256DigestInfoBytes + 11) return std::nullopt;
    const auto digest = juce::SHA256(canonical.data(), canonical.size()).toHexString().toLowerCase();
    if (digest.length() != 64) return std::nullopt;

    juce::String hex("0001");
    hex += juce::String::repeatedString("ff", keyBytes - sha256DigestInfoBytes - 3);
    hex += "00";
    hex += sha256DigestInfoPrefix;
    hex += digest;

    juce::BigInteger value;
    value.parseString(hex, 16);
    return value;
}
} // namespace

std::string signCanonicalText(std::string_view canonical, std::string_view encodedPrivateKey,
                              std::string& error)
{
    const juce::RSAKey key { juce::String::fromUTF8(encodedPrivateKey.data(),
                                                    static_cast<int>(encodedPrivateKey.size())) };
    const auto modulus = modulusOf(encodedPrivateKey);
    if (! key.isValid() || ! modulus) { error = "Invalid signing key"; return {}; }

    if (modulusBits(*modulus) < minimumSignatureModulusBits)
    { error = "Signing key is shorter than the required 2048-bit modulus"; return {}; }

    const auto keyBytes = modulusBytes(*modulus);
    auto encoded = encodeMessage(canonical, keyBytes);
    if (! encoded) { error = "Unable to encode the manifest digest for signing"; return {}; }
    if (! key.applyToValue(*encoded)) { error = "Unable to sign the manifest"; return {}; }

    error.clear();
    return toFixedWidthHex(*encoded, keyBytes).toStdString();
}

bool verifyCanonicalText(std::string_view canonical, std::string_view signatureHex,
                         std::string_view encodedPublicKey, std::string& error)
{
    const juce::RSAKey key { juce::String::fromUTF8(encodedPublicKey.data(),
                                                    static_cast<int>(encodedPublicKey.size())) };
    const auto modulus = modulusOf(encodedPublicKey);
    if (! key.isValid() || ! modulus) { error = "Invalid public key"; return false; }

    if (modulusBits(*modulus) < minimumSignatureModulusBits)
    { error = "Public key is shorter than the required 2048-bit modulus"; return false; }

    // Only the exact fixed-width encoding is accepted, so a signature has one
    // representation and cannot be padded, truncated, or shifted past the modulus.
    const auto keyBytes = modulusBytes(*modulus);
    if (static_cast<int>(signatureHex.size()) != keyBytes * 2 || ! isLowercaseHex(signatureHex))
    { error = "Signature is not a canonical fixed-width value"; return false; }

    juce::BigInteger signature;
    signature.parseString(juce::String::fromUTF8(signatureHex.data(),
                                                 static_cast<int>(signatureHex.size())), 16);
    if (signature.isZero() || signature >= *modulus)
    { error = "Signature is outside the modulus range"; return false; }
    if (! key.applyToValue(signature)) { error = "Signature could not be verified"; return false; }

    const auto expected = encodeMessage(canonical, keyBytes);
    if (! expected) { error = "Unable to encode the manifest digest for verification"; return false; }
    if (signature != *expected) { error = "Signature does not match the manifest"; return false; }

    error.clear();
    return true;
}
} // namespace nts::ecosystem
