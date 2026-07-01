#include "net/ws_frame.hpp"
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <cstring>

// ── ComputeWsAccept ──
//
// Sec-WebSocket-Accept = Base64(SHA1(client_key + "258EAFA5-E914-47DA-95CA-5AB9DC11B85B"))

std::string ComputeWsAccept(std::string_view client_key)
{
    // 1. Concatenate key + magic GUID
    std::string input;
    input.reserve(client_key.size() + 36);
    input += client_key;
    input += "258EAFA5-E914-47DA-95CA-5AB9DC11B85B";

    // 2. SHA-1
    unsigned char sha1_digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(input.data()),
         input.size(), sha1_digest);

    // 3. Base64
    // OpenSSL BIO chain for Base64 encoding
    auto* bio_b64 = BIO_new(BIO_f_base64());
    auto* bio_mem = BIO_new(BIO_s_mem());
    BIO_push(bio_b64, bio_mem);

    BIO_set_flags(bio_b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio_b64, sha1_digest, SHA_DIGEST_LENGTH);
    (void)BIO_flush(bio_b64);

    const char* encoded;
    long encoded_len = BIO_get_mem_data(bio_b64, &encoded);

    std::string result(encoded, static_cast<size_t>(encoded_len));
    BIO_free_all(bio_b64);

    return result;
}
