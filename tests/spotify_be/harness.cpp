// Byte-order check of the cspot core that does not need the network: the same operations on little-endian x86 and
// on big-endian powerpc64 (qemu), printing hashes that must match line for line.
//   shannon  the stream cipher + MAC both ways, in odd-sized chunks, and the decrypt round trip
//   utils    hton64 / pack / extract / bigNumAdd / hex
//   nanopb   a keyexchange ClientHello and a mercury Header encoded and decoded
//   tremor   an Ogg Vorbis file decoded by the integer decoder bell uses (PCM hash and sample count)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <arpa/inet.h>

#include "Shannon.h"
#include "Utils.h"
#include "NanoPBHelper.h"
#include "pb_encode.h"
#include "pb_decode.h"
#include "protobuf/keyexchange.pb.h"
#include "protobuf/mercury.pb.h"
extern "C" {
#include "ivorbisfile.h"
}

static uint64_t fnv(const void *p, size_t n, uint64_t h = 1469598103934665603ULL) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_shannon() {
    std::vector<uint8_t> key(16), nonce = { 0, 0, 0, 7 };
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(i * 17 + 3);
    std::vector<uint8_t> plain(1500);
    for (size_t i = 0; i < plain.size(); i++) plain[i] = (uint8_t)(i * 31 + (i >> 3));

    Shannon enc, dec;
    enc.key(key); enc.nonce(nonce);
    dec.key(key); dec.nonce(nonce);
    std::vector<uint8_t> ct, all_mac;
    static const size_t chunks[] = { 1, 3, 4, 5, 17, 64, 100, 253, 1053 };
    size_t pos = 0;
    uint64_t h = 1469598103934665603ULL;
    for (size_t c : chunks) {
        std::vector<uint8_t> part(plain.begin() + (long)pos, plain.begin() + (long)(pos + c));
        enc.encrypt(part);
        h = fnv(part.data(), part.size(), h);
        ct.insert(ct.end(), part.begin(), part.end());
        pos += c;
    }
    std::vector<uint8_t> mac(4);
    enc.finish(mac);
    printf("shannon ciphertext %016llx mac %02x%02x%02x%02x\n", (unsigned long long)h, mac[0], mac[1], mac[2], mac[3]);

    std::vector<uint8_t> back;
    pos = 0;
    for (size_t c : chunks) {
        std::vector<uint8_t> part(ct.begin() + (long)pos, ct.begin() + (long)(pos + c));
        dec.decrypt(part);
        back.insert(back.end(), part.begin(), part.end());
        pos += c;
    }
    std::vector<uint8_t> mac2(4);
    dec.finish(mac2);
    CHECK(back == plain);
    CHECK(mac2 == mac);
}

static void test_utils() {
    printf("hton64 %016llx\n", (unsigned long long)hton64(0x0102030405060708ULL));
    printf("pack32 %s\n", bytesToHexString(pack<uint32_t>(htonl(0xA1B2C3D4u))).c_str());
    printf("pack16 %s\n", bytesToHexString(pack<uint16_t>(htons(0xBEEF))).c_str());
    std::vector<uint8_t> raw = { 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    printf("extract32 %08x\n", ntohl(extract<uint32_t>(raw, 2)));
    printf("extract64 %016llx\n", (unsigned long long)hton64(extract<uint64_t>(raw, 2)));
    // the sequence id round trip MercurySession does: hton64(extract(pack(hton64(n))))
    const uint64_t n = 0x1122334455667788ULL;
    CHECK(hton64(extract<uint64_t>(pack<uint64_t>(hton64(n)), 0)) == n);
    std::vector<uint8_t> num = { 0x00, 0xFF, 0xFF, 0xFF };
    printf("bignum %s\n", bytesToHexString(bigNumAdd(num, 3)).c_str());
    printf("hex %s\n", bytesToHexString(stringHexToBytes("00ff10a5b6")).c_str());
}

static void test_nanopb() {
    ClientHello hello = ClientHello_init_zero;
    hello.build_info.product = Product_PRODUCT_CLIENT;
    hello.build_info.platform = Platform2_PLATFORM_LINUX_X86;
    hello.build_info.version = 112800721;
    hello.cryptosuites_supported[0] = Cryptosuite_CRYPTO_SUITE_SHANNON;
    for (int i = 0; i < 16; i++) hello.client_nonce[i] = (uint8_t)(i * 9 + 1);
    hello.has_padding = true;
    hello.padding[0] = 0x1E;
    std::vector<uint8_t> enc = pbEncode(ClientHello_fields, &hello);
    printf("hello %zu bytes %016llx\n", enc.size(), (unsigned long long)fnv(enc.data(), enc.size()));
    ClientHello back = ClientHello_init_zero;
    pbDecode(back, ClientHello_fields, enc);
    CHECK(back.build_info.version == 112800721 && back.build_info.product == Product_PRODUCT_CLIENT);
    CHECK(memcmp(back.client_nonce, hello.client_nonce, 16) == 0);

    Header h = Header_init_zero;
    h.has_uri = true;
    snprintf(h.uri, sizeof h.uri, "hm://remote/user/someone/");
    h.has_method = true;
    snprintf(h.method, sizeof h.method, "SUB");
    std::vector<uint8_t> e2 = pbEncode(Header_fields, &h);
    printf("header %zu bytes %016llx\n", e2.size(), (unsigned long long)fnv(e2.data(), e2.size()));
    Header b2 = Header_init_zero;
    pbDecode(b2, Header_fields, e2);
    CHECK(strcmp(b2.uri, h.uri) == 0 && strcmp(b2.method, "SUB") == 0);
}

static void test_tremor(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("tremor: cannot open %s\n", path); g_fail++; return; }
    OggVorbis_File vf;
    if (ov_open(f, &vf, NULL, 0) < 0) { printf("tremor: ov_open failed\n"); g_fail++; return; }
    vorbis_info *vi = ov_info(&vf, -1);
    printf("tremor %ld Hz, %d ch\n", vi->rate, vi->channels);
    uint64_t h = 1469598103934665603ULL;
    long total = 0;
    char buf[4096];
    int bitstream = 0;
    long r;
    while ((r = ov_read(&vf, buf, sizeof buf, &bitstream)) > 0) {
        // ov_read writes 16-bit samples in host order; hash them by value so the two machines can agree.
        const int16_t *smp = (const int16_t *)buf;
        for (long i = 0; i < r / 2; i++) {
            const uint8_t two[2] = { (uint8_t)(smp[i] & 0xFF), (uint8_t)((smp[i] >> 8) & 0xFF) };
            h = fnv(two, 2, h);
        }
        total += r;
    }
    printf("tremor pcm %ld bytes %016llx\n", total, (unsigned long long)h);
    // the samples are 16-bit in host order: read them back as values, which must be the same on both machines
    ov_pcm_seek(&vf, 20000);
    int16_t s[8];
    r = ov_read(&vf, (char *)s, sizeof s, &bitstream);
    printf("tremor seek %ld:", r);
    for (int i = 0; i < 4; i++) printf(" %d", s[i]);
    printf("\n");
    ov_clear(&vf);
}

int main(int argc, char **argv) {
    test_shannon();
    test_utils();
    test_nanopb();
    if (argc > 1) test_tremor(argv[1]);
    printf("%s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
