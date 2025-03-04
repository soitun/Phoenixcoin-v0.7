// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software licence, see the accompanying
// file LICENCE or http://opensource.org/license/mit

#include <map>

#include <openssl/obj_mac.h>
#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
#include <openssl/ecdsa.h>
#endif

#include "key.h"

#include "ecies/ecies.h"

// Generate a private key from just the secret parameter
int EC_KEY_regenerate_key(EC_KEY *eckey, BIGNUM *priv_key) {
    int ok = 0;
    BN_CTX *ctx = NULL;
    EC_POINT *pub_key = NULL;

    if(!eckey) return(0);

    const EC_GROUP *group = EC_KEY_get0_group(eckey);

    if((ctx = BN_CTX_new()) == NULL)
      goto err;

    pub_key = EC_POINT_new(group);

    if(!pub_key)
      goto err;

    if(!EC_POINT_mul(group, pub_key, priv_key, NULL, NULL, ctx))
      goto err;

    EC_KEY_set_private_key(eckey,priv_key);
    EC_KEY_set_public_key(eckey,pub_key);

    ok = 1;

err:

    if(pub_key)
      EC_POINT_free(pub_key);
    if(ctx != NULL)
      BN_CTX_free(ctx);

    return(ok);
}

// Perform ECDSA key recovery (see SEC1 4.1.6) for curves over (mod p)-fields
// recid selects which key is recovered
// if check is non-zero, additional checks are performed
int ECDSA_SIG_recover_key_GFp(EC_KEY *eckey, ECDSA_SIG *ecsig, const uchar *msg,
  int msglen, int recid, int check) {

    if(!eckey) return(0);

#if (OPENSSL_VERSION_NUMBER > 0x10100000L)
    const BIGNUM *sig_r, *sig_s;
    ECDSA_SIG_get0(ecsig, &sig_r, &sig_s);
#endif

    int ret = 0;
    BN_CTX *ctx = NULL;

    BIGNUM *x = NULL;
    BIGNUM *e = NULL;
    BIGNUM *order = NULL;
    BIGNUM *sor = NULL;
    BIGNUM *eor = NULL;
    BIGNUM *field = NULL;
    EC_POINT *R = NULL;
    EC_POINT *O = NULL;
    EC_POINT *Q = NULL;
    BIGNUM *rr = NULL;
    BIGNUM *zero = NULL;
    int n = 0;
    int i = recid / 2;

    const EC_GROUP *group = EC_KEY_get0_group(eckey);
    if((ctx = BN_CTX_new()) == NULL) { ret = -1; goto err; }
    BN_CTX_start(ctx);
    order = BN_CTX_get(ctx);
    if(!EC_GROUP_get_order(group, order, ctx)) { ret = -2; goto err; }
    x = BN_CTX_get(ctx);
    if(!BN_copy(x, order)) { ret = -1; goto err; }
    if(!BN_mul_word(x, i)) { ret = -1; goto err; }
#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
    if(!BN_add(x, x, ecsig->r)) { ret = -1; goto err; }
#else
    if(!BN_add(x, x, sig_r)) { ret = -1; goto err; }
#endif
    field = BN_CTX_get(ctx);
    if(!EC_GROUP_get_curve_GFp(group, field, NULL, NULL, ctx)) { ret = -2; goto err; }
    if(BN_cmp(x, field) >= 0) { ret = 0; goto err; }
    if((R = EC_POINT_new(group)) == NULL) { ret = -2; goto err; }
    if(!EC_POINT_set_compressed_coordinates_GFp(group, R, x, recid % 2, ctx)) { ret = 0; goto err; }
    if(check) {
        if((O = EC_POINT_new(group)) == NULL) { ret = -2; goto err; }
        if(!EC_POINT_mul(group, O, NULL, R, order, ctx)) { ret = -2; goto err; }
        if(!EC_POINT_is_at_infinity(group, O)) { ret = 0; goto err; }
    }
    if((Q = EC_POINT_new(group)) == NULL) { ret = -2; goto err; }
    n = EC_GROUP_get_degree(group);
    e = BN_CTX_get(ctx);
    if(!BN_bin2bn(msg, msglen, e)) { ret = -1; goto err; }
    if((8 * msglen) > n) BN_rshift(e, e, 8 - (n & 7));
    zero = BN_CTX_get(ctx);
    if(!BN_zero(zero)) { ret = -1; goto err; }
    if(!BN_mod_sub(e, zero, e, order, ctx)) { ret = -1; goto err; }
    rr = BN_CTX_get(ctx);
#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
    if(!BN_mod_inverse(rr, ecsig->r, order, ctx)) { ret = -1; goto err; }
#else
    if(!BN_mod_inverse(rr, sig_r, order, ctx)) { ret = -1; goto err; }
#endif
    sor = BN_CTX_get(ctx);
#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
    if(!BN_mod_mul(sor, ecsig->s, rr, order, ctx)) { ret = -1; goto err; }
#else
    if(!BN_mod_mul(sor, sig_s, rr, order, ctx)) { ret = -1; goto err; }
#endif
    eor = BN_CTX_get(ctx);
    if(!BN_mod_mul(eor, e, rr, order, ctx)) { ret = -1; goto err; }
    if(!EC_POINT_mul(group, Q, eor, R, sor, ctx)) { ret = -2; goto err; }
    if(!EC_KEY_set_public_key(eckey, Q)) { ret = -2; goto err; }

    ret = 1;

err:
    if(ctx) {
        BN_CTX_end(ctx);
        BN_CTX_free(ctx);
    }
    if(R != NULL) EC_POINT_free(R);
    if(O != NULL) EC_POINT_free(O);
    if(Q != NULL) EC_POINT_free(Q);
    return(ret);
}

void CKey::SetCompressedPubKey(bool fCompressed) {
    EC_KEY_set_conv_form(pkey, fCompressed ? POINT_CONVERSION_COMPRESSED : POINT_CONVERSION_UNCOMPRESSED);
    fCompressedPubKey = true;
}

void CKey::Reset() {
    fCompressedPubKey = false;
    if(pkey)
      EC_KEY_free(pkey);
    pkey = EC_KEY_new_by_curve_name(NID_secp256k1);
    if(!pkey)
      throw(key_error("CKey::CKey() : EC_KEY_new_by_curve_name failed"));
    fSet = false;
}

CKey::CKey() {
    pkey = NULL;
    Reset();
}

CKey::CKey(const CKey &b) {
    pkey = EC_KEY_dup(b.pkey);
    if(!pkey)
      throw(key_error("CKey::CKey(const CKey&) : EC_KEY_dup failed"));
    fSet = b.fSet;
}

CKey &CKey::operator=(const CKey &b) {
    if(!EC_KEY_copy(pkey, b.pkey))
      throw(key_error("CKey::operator=(const CKey&) : EC_KEY_copy failed"));
    fSet = b.fSet;
    return(*this);
}

CKey::~CKey() {
    EC_KEY_free(pkey);
}

bool CKey::IsNull() const {
    return(!fSet);
}

bool CKey::IsCompressed() const {
    return(fCompressedPubKey);
}

void CKey::MakeNewKey(bool fCompressed) {
    if(!EC_KEY_generate_key(pkey))
      throw(key_error("CKey::MakeNewKey() : EC_KEY_generate_key failed"));
    if(fCompressed)
      SetCompressedPubKey();
    fSet = true;
}

bool CKey::SetPrivKey(const CPrivKey &vchPrivKey) {
    const uchar *pbegin = &vchPrivKey[0];
    if(d2i_ECPrivateKey(&pkey, &pbegin, vchPrivKey.size())) {
        // In testing, d2i_ECPrivateKey can return true
        // but fill in pkey with a key that fails
        // EC_KEY_check_key, so:
        if(EC_KEY_check_key(pkey)) {
            fSet = true;
            return true;
        }
    }
    // If vchPrivKey data is bad d2i_ECPrivateKey() can
    // leave pkey in a state where calling EC_KEY_free()
    // crashes. To avoid that, set pkey to NULL and
    // leak the memory (a leak is better than a crash)
    pkey = NULL;
    Reset();
    return(false);
}

bool CKey::SetSecret(const CSecret& vchSecret, bool fCompressed) {
    EC_KEY_free(pkey);
    pkey = EC_KEY_new_by_curve_name(NID_secp256k1);
    if(!pkey)
      throw(key_error("CKey::SetSecret() : EC_KEY_new_by_curve_name failed"));
    if(vchSecret.size() != 32)
      throw(key_error("CKey::SetSecret() : secret must be 32 bytes"));
    BIGNUM *bn = BN_bin2bn(&vchSecret[0], 32, BN_new());
    if(!bn)
      throw(key_error("CKey::SetSecret() : BN_bin2bn failed"));
    if(!EC_KEY_regenerate_key(pkey, bn)) {
        BN_clear_free(bn);
        throw(key_error("CKey::SetSecret() : EC_KEY_regenerate_key failed"));
    }
    BN_clear_free(bn);
    fSet = true;
    if(fCompressed || fCompressedPubKey)
      SetCompressedPubKey();
    return(true);
}

CSecret CKey::GetSecret(bool &fCompressed) const {
    CSecret vchRet;
    vchRet.resize(32);
    const BIGNUM *bn = EC_KEY_get0_private_key(pkey);
    int nBytes = BN_num_bytes(bn);
    if(!bn)
      throw(key_error("CKey::GetSecret() : EC_KEY_get0_private_key failed"));
    int n = BN_bn2bin(bn, &vchRet[32 - nBytes]);
    if(n != nBytes)
      throw(key_error("CKey::GetSecret(): BN_bn2bin failed"));
    fCompressed = fCompressedPubKey;
    return(vchRet);
}

CPrivKey CKey::GetPrivKey() const {
    int nSize = i2d_ECPrivateKey(pkey, NULL);
    if(!nSize)
      throw(key_error("CKey::GetPrivKey() : i2d_ECPrivateKey failed"));
    CPrivKey vchPrivKey(nSize, 0);
    uchar *pbegin = &vchPrivKey[0];
    if(i2d_ECPrivateKey(pkey, &pbegin) != nSize)
      throw(key_error("CKey::GetPrivKey() : i2d_ECPrivateKey returned unexpected size"));
    return(vchPrivKey);
}

bool CKey::SetPubKey(const CPubKey &vchPubKey) {
    const uchar *pbegin = &vchPubKey.vchPubKey[0];
    if(o2i_ECPublicKey(&pkey, &pbegin, vchPubKey.vchPubKey.size())) {
        fSet = true;
        if(vchPubKey.vchPubKey.size() == 33)
          SetCompressedPubKey();
        return(true);
    }
    pkey = NULL;
    Reset();
    return(false);
}

CPubKey CKey::GetPubKey() const {
    int nSize = i2o_ECPublicKey(pkey, NULL);
    if(!nSize)
      throw(key_error("CKey::GetPubKey() : i2o_ECPublicKey failed"));
    std::vector<uchar> vchPubKey(nSize, 0);
    uchar *pbegin = &vchPubKey[0];
    if(i2o_ECPublicKey(pkey, &pbegin) != nSize)
      throw(key_error("CKey::GetPubKey() : i2o_ECPublicKey returned unexpected size"));
    return(CPubKey(vchPubKey));
}

bool CKey::Sign(uint256 hash, std::vector<uchar> &vchSig) {
    uint nSize = ECDSA_size(pkey);
    vchSig.resize(nSize); // Make sure it is big enough
    if(!ECDSA_sign(0, (uchar *) &hash, sizeof(hash), &vchSig[0], &nSize, pkey)) {
        vchSig.clear();
        return(false);
    }
    vchSig.resize(nSize); // Shrink to fit actual size
    return(true);
}

// create a compact signature (65 bytes), which allows reconstructing the used public key
// The format is one header byte, followed by two times 32 bytes for the serialized r and s values.
// The header byte: 0x1B = first key with even y, 0x1C = first key with odd y,
//                  0x1D = second key with even y, 0x1E = second key with odd y
bool CKey::SignCompact(uint256 hash, std::vector<uchar> &vchSig) {
    bool fOk = false;
    ECDSA_SIG *sig = ECDSA_do_sign((uchar *) &hash, sizeof(hash), pkey);
    if(!sig) return(false);
    vchSig.clear();
    vchSig.resize(65, 0);
#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
    int nBitsR = BN_num_bits(sig->r);
    int nBitsS = BN_num_bits(sig->s);
#else
    const BIGNUM *sig_r, *sig_s;
    ECDSA_SIG_get0(sig, &sig_r, &sig_s);

    int nBitsR = BN_num_bits(sig_r);
    int nBitsS = BN_num_bits(sig_s);
#endif

    int i;
    if((nBitsR <= 256) && (nBitsS <= 256)) {
        int nRecId = -1;
        for(i = 0; i < 4; i++) {
            CKey keyRec;
            keyRec.fSet = true;
            if(fCompressedPubKey)
              keyRec.SetCompressedPubKey();
            if(ECDSA_SIG_recover_key_GFp(keyRec.pkey, sig, (uchar *) &hash, sizeof(hash), i, 1) == 1) {
                if(keyRec.GetPubKey() == this->GetPubKey()) {
                    nRecId = i;
                    break;
                }
            }
        }

        if(nRecId == -1)
          throw(key_error("CKey::SignCompact() : unable to construct recoverable key"));

        vchSig[0] = nRecId + 27 + (fCompressedPubKey ? 4 : 0);
#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
        BN_bn2bin(sig->r, &vchSig[33 - (nBitsR + 7) / 8]);
        BN_bn2bin(sig->s, &vchSig[65 - (nBitsS + 7) / 8]);
#else
        BN_bn2bin(sig_r, &vchSig[33 - (nBitsR + 7) / 8]);
        BN_bn2bin(sig_s, &vchSig[65 - (nBitsS + 7) / 8]);
#endif
        fOk = true;
    }
    ECDSA_SIG_free(sig);
    return(fOk);
}

// reconstruct public key from a compact signature
// This is only slightly more CPU intensive than just verifying it.
// If this function succeeds, the recovered public key is guaranteed to be valid
// (the signature is a valid signature of the given data for that key)
bool CKey::SetCompactSignature(uint256 hash, const std::vector<uchar> &vchSig) {
    if(vchSig.size() != 65) return(false);
    int nV = vchSig[0];
    if((nV < 27) || (nV >= 35)) return(false);
    ECDSA_SIG *sig = ECDSA_SIG_new();
    if(!sig) return(false);

#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
    BN_bin2bn(&vchSig[1], 32, sig->r);
    BN_bin2bn(&vchSig[33], 32, sig->s);
#else
    /* sig_r and sig_s are deallocated by ECDSA_SIG_free(sig); */
    BIGNUM *sig_r = BN_bin2bn(&vchSig[1], 32, BN_new());
    BIGNUM *sig_s = BN_bin2bn(&vchSig[33], 32, BN_new());
    if(!sig_r || !sig_s) return(false);
    /* Copy and transfer ownership */
    ECDSA_SIG_set0(sig, sig_r, sig_s);
#endif

    EC_KEY_free(pkey);
    pkey = EC_KEY_new_by_curve_name(NID_secp256k1);
    if(nV >= 31) {
        SetCompressedPubKey();
        nV -= 4;
    }
    if(ECDSA_SIG_recover_key_GFp(pkey, sig, (uchar *) &hash, sizeof(hash), nV - 27, 0) == 1) {
        fSet = true;
        ECDSA_SIG_free(sig);
        return(true);
    }
    return(false);
}

bool CKey::Verify(uint256 hash, const std::vector<uchar> &vchSig) {
    // -1 = error, 0 = bad sig, 1 = good
    if(ECDSA_verify(0, (uchar *) &hash, sizeof(hash), &vchSig[0], vchSig.size(), pkey) != 1)
      return(false);

    return(true);
}

bool CKey::VerifyCompact(uint256 hash, const std::vector<uchar> &vchSig) {
    CKey key;
    if(!key.SetCompactSignature(hash, vchSig))
      return(false);
    if(GetPubKey() != key.GetPubKey())
      return(false);

    return(true);
}

bool CKey::IsValid() {
    if(!fSet) return(false);

    if(!EC_KEY_check_key(pkey)) return(false);

    bool fCompr;
    CSecret secret = GetSecret(fCompr);
    CKey key2;
    key2.SetSecret(secret, fCompr);
    return GetPubKey() == key2.GetPubKey();
}

void CPubKey::EncryptData(const std::vector<uchar> &plaindata, std::vector<uchar> &encdata) {
    CKey key;

    key.SetPubKey(*this);
    key.EncryptData(plaindata, encdata);
}

void CKey::EncryptData(const std::vector<uchar> &plaindata, std::vector<uchar> &encdata) {
    ecies_ctx_t *ctx = new ecies_ctx_t;
    char error[256] = "Unknown error";
    secure_t *cryptex;

    ctx->cipher = EVP_aes_128_cbc();
    ctx->md = EVP_ripemd160();
    ctx->kdf_md = EVP_ripemd160();
    ctx->stored_key_length = 33;
    ctx->user_key = pkey;

    if(!EC_KEY_get0_public_key(ctx->user_key))
      throw(key_error("Invalid public key"));

    cryptex = ecies_encrypt(ctx, (uchar *) &plaindata[0], plaindata.size(), error);

    if(!cryptex) {
        delete ctx;
        throw(key_error(std::string("Error in encryption: ") + error));
    }

    encdata.resize(secure_data_sum_length(cryptex));
    memcpy(&encdata[0], secure_key_data(cryptex), encdata.size());
    secure_free(cryptex);
    delete ctx;
}

void CKey::DecryptData(const std::vector<uchar> &encdata, std::vector<uchar> &plaindata) {
    ecies_ctx_t *ctx = new ecies_ctx_t;
    char error[256] = "Unknown error";
    secure_t *cryptex;
    uchar *decrypted;
    size_t length;

    ctx->cipher = EVP_aes_128_cbc();
    ctx->md = EVP_ripemd160();
    ctx->kdf_md = EVP_ripemd160();
    ctx->stored_key_length = 33;
    ctx->user_key = pkey;

    if(!EC_KEY_get0_private_key(ctx->user_key))
      throw(key_error("Invalid private key"));

    size_t key_length = ctx->stored_key_length;
    size_t mac_length = EVP_MD_size(ctx->md);

    cryptex = secure_alloc(key_length, mac_length, encdata.size() - key_length - mac_length);
    memcpy(secure_key_data(cryptex), &encdata[0], encdata.size());
    decrypted = ecies_decrypt(ctx, cryptex, &length, error);

    secure_free(cryptex);
    delete ctx;

    if(!decrypted) {
      throw(key_error(std::string("Error in decryption: ") + error));
    }

    plaindata.resize(length);
    memcpy(&plaindata[0], decrypted, length);
    free(decrypted);
}
