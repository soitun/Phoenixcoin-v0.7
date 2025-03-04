// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software licence, see the accompanying
// file LICENCE or http://opensource.org/license/mit

#ifndef MAIN_H
#define MAIN_H

#include <string>
#include <algorithm>
#include <map>
#include <utility>
#include <vector>
#include <set>
#include <limits>

#include "bignum.h"
#include "sync.h"
#include "net.h"
#include "script.h"

#include "neoscrypt.h"

class CWallet;
class CBlock;
class CBlockIndex;
class CKeyItem;
class CReserveKey;

class CAddress;
class CInv;
class CRequestTracker;
class CNode;

/* Maturity threshold for PoW base transactions, in blocks (confirmations) */
extern int nBaseMaturity;
static const int BASE_MATURITY = 100;
static const int BASE_MATURITY_TESTNET = 100;
/* Offset for the above to allow safe network propagation, in blocks (confirmations) */
static const int BASE_MATURITY_OFFSET = 1;
/* Maturity threshold for regular transactions, in blocks (confirmations) */
static const int TX_MATURITY = 6;

/* The max. allowed size for a serialised block, in bytes */
static const uint MAX_BLOCK_SIZE = 524288;
/* The max. allowed size for a mined block, in bytes */
static const uint MAX_BLOCK_SIZE_GEN = (MAX_BLOCK_SIZE >> 1);
/* The max. allowed number of signature check operations per block */
static const uint MAX_BLOCK_SIGOPS = (MAX_BLOCK_SIZE >> 6);
/* The max. number of orphan transactions kept in memory */
static const uint MAX_ORPHAN_TRANSACTIONS = (MAX_BLOCK_SIZE >> 8);
/* The maximum number of entries in an 'inv' protocol message */
static const unsigned int MAX_INV_SZ = 50000;
/* The current time frame of block limiter */
static const int64 BLOCK_LIMITER_TIME = 120;
/* The min. transaction fee if required */
static const int64 MIN_TX_FEE = 10000000;
/* Fees below this value are considered absent while relaying */
static const int64 MIN_RELAY_TX_FEE = 5000000;
/* The dust threshold */
static const int64 TX_DUST = 1000000;
/* The max. amount for a single transaction */
static const int64 MAX_MONEY = 10000000 * COIN;
inline bool MoneyRange(int64 nValue) { return (nValue >= 0 && nValue <= MAX_MONEY); }

// Threshold for nLockTime: below this value it is interpreted as block number, otherwise as UNIX timestamp.
static const unsigned int LOCKTIME_THRESHOLD = 500000000; // Tue Nov  5 00:53:20 1985 UTC

#ifdef USE_UPNP
static const int fHaveUPnP = true;
#else
static const int fHaveUPnP = false;
#endif

/* Hard & soft fork related data */
static const int nForkOne   = 46500;   /* the 1st hard fork */
static const int nForkTwo   = 69444;   /* the 2nd hard fork */
static const int nForkThree = 74100;   /* the 3rd hard fork */
static const int nForkFour  = 154000;  /* the 4th hard fork */
static const int nForkFive  = 400000;  /* the 5th hard fork */

static const int nSoftForkOne   = 270000;
static const int nSoftForkTwo   = 340000;

static const int nTestnetForkOne   = 600;
static const int nTestnetForkTwo   = 3600;

static const int nTestnetSoftForkOne   = 3400;
static const int nTestnetSoftForkTwo   = 3500;

static const uint nSwitchV2            = 1406851200;   /* 01 Aug 2014 00:00:00 GMT */
static const uint nTestnetSwitchV2     = 1404777600;   /* 08 Jul 2014 00:00:00 GMT */

static const int nTargetSpacingZero    = 90;  /* 1.5 minutes */
static const int nTargetSpacingOne     = nTargetSpacingZero;
static const int nTargetSpacingTwo     = 50;  /* 50 seconds */
static const int nTargetSpacingThree   = 45;  /* 45 seconds */
static const int nTargetSpacingFour    = 90;  /* 1.5 minutes */

static const int nTargetTimespanZero   = 2400 * nTargetSpacingZero;  /* 60 hours */
static const int nTargetTimespanOne    = 600  * nTargetSpacingOne;   /* 15 hours */
static const int nTargetTimespanTwo    = 108  * nTargetSpacingTwo;   /* 1.5 hours */
static const int nTargetTimespanThree  = 126  * nTargetSpacingThree; /* 1.575 hours */
static const int nTargetTimespanFour   = 20   * nTargetSpacingFour;  /* 0.5 hours */


extern CScript COINBASE_FLAGS;






extern CCriticalSection cs_main;
extern std::map<uint256, CBlockIndex*> mapBlockIndex;
extern uint256 hashGenesisBlock;
extern CBlockIndex* pindexGenesisBlock;
extern int nBestHeight;
extern CBigNum bnBestChainWork;
extern CBigNum bnBestInvalidWork;
extern uint256 hashBestChain;
extern CBlockIndex* pindexBest;
extern unsigned int nTransactionsUpdated;
extern uint64 nLastBlockTx;
extern uint64 nLastBlockSize;
extern const std::string strMessageMagic;
extern double dHashesPerSec;
extern int64 nHPSTimerStart;
extern int64 nTimeBestReceived;
extern CCriticalSection cs_setpwalletRegistered;
extern std::set<CWallet*> setpwalletRegistered;
extern unsigned char pchMessageStart[4];
extern std::map<uint256, CBlock *> mapOrphanBlocks;

// Settings
extern int64 nTransactionFee;
extern int64 nMinimumInputValue;

// Minimum disk space required - used in CheckDiskSpace()
static const uint64 nMinDiskSpace = 52428800;


class CReserveKey;
class CTxDB;
class CTxIndex;

void RegisterWallet(CWallet* pwalletIn);
void UnregisterWallet(CWallet* pwalletIn);
void SyncWithWallets(const CTransaction& tx, const CBlock* pblock = NULL, bool fUpdate = false);
bool ProcessBlock(CNode* pfrom, CBlock* pblock);
bool CheckDiskSpace(uint64 nAdditionalBytes=0);
FILE* OpenBlockFile(unsigned int nFile, unsigned int nBlockPos, const char* pszMode="rb");
FILE* AppendBlockFile(unsigned int& nFileRet);
bool LoadBlockIndex(bool fAllowNew=true);
void PrintBlockTree();
CBlockIndex* FindBlockByHeight(int nHeight);
bool ProcessMessages(CNode* pfrom);
bool SendMessages(CNode* pto, bool fSendTrickle);
bool LoadExternalBlockFile(FILE* fileIn);
void GenerateCoins(bool fGenerate, CWallet *pwallet);
CBlock* CreateNewBlock(CReserveKey& reservekey);
void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce);
void FormatDataBuffer(CBlock *pblock, uint *pdata);
bool CheckWork(CBlock *pblock, CWallet &wallet, CReserveKey &reservekey, bool fGetWork);
bool CheckProofOfWork(uint256 hash, unsigned int nBits);
int64 GetProofOfWorkReward(int nHeight, int64 nFees);
int64 GetMoneySupply(int nHeight);
int GetNumBlocksOfPeers();
bool IsInitialBlockDownload();
std::string GetWarnings(std::string strFor);
bool GetTransaction(const uint256 &hash, CTransaction &tx, uint256 &hashBlock);
uint256 WantedByOrphan(const CBlock *pblockOrphan);
void ResendWalletTransactions(bool fForce = false);

bool GetWalletFile(CWallet* pwallet, std::string &strWalletFileOut);

/** Position on disk for a particular transaction. */
class CDiskTxPos
{
public:
    unsigned int nFile;
    unsigned int nBlockPos;
    unsigned int nTxPos;

    CDiskTxPos()
    {
        SetNull();
    }

    CDiskTxPos(unsigned int nFileIn, unsigned int nBlockPosIn, unsigned int nTxPosIn)
    {
        nFile = nFileIn;
        nBlockPos = nBlockPosIn;
        nTxPos = nTxPosIn;
    }

    IMPLEMENT_SERIALIZE( READWRITE(FLATDATA(*this)); )
    void SetNull() { nFile = std::numeric_limits<uint32_t>::max(); nBlockPos = 0; nTxPos = 0; }
    bool IsNull() const { return (nFile == std::numeric_limits<uint32_t>::max()); }

    friend bool operator==(const CDiskTxPos& a, const CDiskTxPos& b)
    {
        return (a.nFile     == b.nFile &&
                a.nBlockPos == b.nBlockPos &&
                a.nTxPos    == b.nTxPos);
    }

    friend bool operator!=(const CDiskTxPos& a, const CDiskTxPos& b)
    {
        return !(a == b);
    }

    std::string ToString() const
    {
        if (IsNull())
            return "null";
        else
            return strprintf("(nFile=%u, nBlockPos=%u, nTxPos=%u)", nFile, nBlockPos, nTxPos);
    }

    void print() const
    {
        printf("%s", ToString().c_str());
    }
};


/** An inpoint - a combination of a transaction and an index n into its vin */
class CInPoint {
public:
    CTransaction *ptx;
    uint n;

    CInPoint() { SetNull(); }
    CInPoint(CTransaction *ptxIn, uint nIn) { ptx = ptxIn; n = nIn; }
    void SetNull() { ptx = NULL; n = std::numeric_limits<uint32_t>::max(); }
    bool IsNull() const {
        return(!ptx && (n == std::numeric_limits<uint32_t>::max()));
    }
};


/** An outpoint - a combination of a transaction hash and an index n into its vout */
class COutPoint {
public:
    uint256 hash;
    uint n;

    COutPoint() { SetNull(); }
    COutPoint(uint256 hashIn, uint nIn) { hash = hashIn; n = nIn; }
    IMPLEMENT_SERIALIZE( READWRITE(FLATDATA(*this)); )
    void SetNull() { hash = 0; n = std::numeric_limits<uint32_t>::max(); }
    bool IsNull() const {
        return(!hash && (n == std::numeric_limits<uint32_t>::max()));
    }

    friend bool operator<(const COutPoint &a, const COutPoint &b) {
        return((a.hash < b.hash) || (a.hash == b.hash && a.n < b.n));
    }

    friend bool operator==(const COutPoint &a, const COutPoint &b) {
        return((a.hash == b.hash) && (a.n == b.n));
    }

    friend bool operator!=(const COutPoint &a, const COutPoint &b) {
        return(!(a == b));
    }

    std::string ToString() const {
        return(strprintf("COutPoint(%s, %u)", hash.ToString().substr(0,10).c_str(), n));
    }

    void print() const {
        printf("%s\n", ToString().c_str());
    }
};


/** An input of a transaction.  It contains the location of the previous
 * transaction's output that it claims and a signature that matches the
 * output's public key.
 */
class CTxIn {
public:
    COutPoint prevout;
    CScript scriptSig;
    uint nSequence;

    CTxIn() {
        nSequence = std::numeric_limits<uint>::max();
    }

    explicit CTxIn(COutPoint prevoutIn, CScript scriptSigIn=CScript(),
      uint nSequenceIn=std::numeric_limits<uint>::max()) {
        prevout = prevoutIn;
        scriptSig = scriptSigIn;
        nSequence = nSequenceIn;
    }

    CTxIn(uint256 hashPrevTx, uint nOut, CScript scriptSigIn=CScript(),
      uint nSequenceIn=std::numeric_limits<uint>::max()) {
        prevout = COutPoint(hashPrevTx, nOut);
        scriptSig = scriptSigIn;
        nSequence = nSequenceIn;
    }

    IMPLEMENT_SERIALIZE(
        READWRITE(prevout);
        READWRITE(scriptSig);
        READWRITE(nSequence);
    )

    bool IsFinal() const {
        return(nSequence == std::numeric_limits<uint>::max());
    }

    friend bool operator==(const CTxIn &a, const CTxIn &b) {
        return((a.prevout == b.prevout) && (a.scriptSig == b.scriptSig) &&
          (a.nSequence == b.nSequence));
    }

    friend bool operator!=(const CTxIn &a, const CTxIn &b) {
        return(!(a == b));
    }

    std::string ToStringShort() const {
        return(strprintf(" %s %d", prevout.hash.ToString().c_str(), prevout.n));
    }

    std::string ToString() const {
        std::string str;
        str += "CTxIn(";
        str += prevout.ToString();
        if(prevout.IsNull()) {
            str += strprintf(", coin base %s", HexStr(scriptSig).c_str());
        } else {
            str += strprintf(", scriptSig=%s",
              scriptSig.ToString().substr(0,24).c_str());
        }
        if(nSequence != std::numeric_limits<uint>::max()) {
            str += strprintf(", nSequence=%u", nSequence);
        }
        str += ")";
        return(str);
    }

    void print() const {
        printf("%s\n", ToString().c_str());
    }
};


/** An output of a transaction.  It contains the public key that the next input
 * must be able to sign with to claim it.
 */
class CTxOut {
public:
    int64 nValue;
    CScript scriptPubKey;

    CTxOut() {
        SetNull();
    }

    CTxOut(int64 nValueIn, CScript scriptPubKeyIn) {
        nValue = nValueIn;
        scriptPubKey = scriptPubKeyIn;
    }

    IMPLEMENT_SERIALIZE(
        READWRITE(nValue);
        READWRITE(scriptPubKey);
    )

    void SetNull() {
        nValue = -1;
        scriptPubKey.clear();
    }

    bool IsNull() const {
        return(nValue == -1);
    }

    void SetEmpty() {
        nValue = 0;
        scriptPubKey.clear();
    }

    bool IsEmpty() const {
        return(!nValue && scriptPubKey.empty());
    }

    uint256 GetHash() const {
        return(SerializeHash(*this));
    }

    friend bool operator==(const CTxOut &a, const CTxOut &b) {
        return((a.nValue == b.nValue) && (a.scriptPubKey == b.scriptPubKey));
    }

    friend bool operator!=(const CTxOut &a, const CTxOut &b) {
        return(!(a == b));
    }

    std::string ToStringShort() const {
        return(strprintf(" out %s %s", FormatMoney(nValue).c_str(),
          scriptPubKey.ToString().substr(0, 10).c_str()));
    }

    std::string ToString() const {
        if(IsEmpty()) return("CTxOut(empty)");
        if(scriptPubKey.size() < 6) return("CTxOut(error)");
        return(strprintf("CTxOut(nValue=%s, scriptPubKey=%s)",
          FormatMoney(nValue).c_str(), scriptPubKey.ToString().c_str()));
    }

    void print() const {
        printf("%s\n", ToString().c_str());
    }
};


enum GetMinFee_mode
{
    GMF_BLOCK,
    GMF_RELAY,
    GMF_SEND,
};

typedef std::map<uint256, std::pair<CTxIndex, CTransaction> > MapPrevTx;

/** The basic transaction that is broadcasted on the network and contained in
 * blocks.  A transaction can contain multiple inputs and outputs.
 */
class CTransaction
{
public:
    static const int CURRENT_VERSION=1;
    int nVersion;
    std::vector<CTxIn> vin;
    std::vector<CTxOut> vout;
    unsigned int nLockTime;

    // Denial-of-service detection:
    mutable int nDoS;
    bool DoS(int nDoSIn, bool fIn) const { nDoS += nDoSIn; return fIn; }

    CTransaction()
    {
        SetNull();
    }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(this->nVersion);
        nVersion = this->nVersion;
        READWRITE(vin);
        READWRITE(vout);
        READWRITE(nLockTime);
    )

    void SetNull()
    {
        nVersion = CTransaction::CURRENT_VERSION;
        vin.clear();
        vout.clear();
        nLockTime = 0;
        nDoS = 0;  // Denial-of-service prevention
    }

    bool IsNull() const
    {
        return (vin.empty() && vout.empty());
    }

    uint256 GetHash() const
    {
        return SerializeHash(*this);
    }

    bool IsFinal(int nBlockHeight=0, int64 nBlockTime=0) const
    {
        // Time based nLockTime implemented in 0.1.6
        if (nLockTime == 0)
            return true;
        if (nBlockHeight == 0)
            nBlockHeight = nBestHeight;
        if (nBlockTime == 0)
            nBlockTime = GetAdjustedTime();
        if ((int64)nLockTime < ((int64)nLockTime < LOCKTIME_THRESHOLD ? (int64)nBlockHeight : nBlockTime))
            return true;
        BOOST_FOREACH(const CTxIn& txin, vin)
            if (!txin.IsFinal())
                return false;
        return true;
    }

    bool IsNewerThan(const CTransaction& old) const
    {
        if (vin.size() != old.vin.size())
            return false;
        for (unsigned int i = 0; i < vin.size(); i++)
            if (vin[i].prevout != old.vin[i].prevout)
                return false;

        bool fNewer = false;
        unsigned int nLowest = std::numeric_limits<unsigned int>::max();
        for (unsigned int i = 0; i < vin.size(); i++)
        {
            if (vin[i].nSequence != old.vin[i].nSequence)
            {
                if (vin[i].nSequence <= nLowest)
                {
                    fNewer = false;
                    nLowest = vin[i].nSequence;
                }
                if (old.vin[i].nSequence < nLowest)
                {
                    fNewer = true;
                    nLowest = old.vin[i].nSequence;
                }
            }
        }
        return fNewer;
    }

    bool IsCoinBase() const
    {
        return (vin.size() == 1 && vin[0].prevout.IsNull());
    }

    /** Check for standard transaction types
        @return True if all outputs (scriptPubKeys) use only standard transaction forms
    */
    bool IsStandard() const;

    /** Check for standard transaction types
        @param[in] mapInputs  Map of previous transactions that have outputs we're spending
        @return True if all inputs (scriptSigs) use only standard transaction forms
        @see CTransaction::FetchInputs
    */
    bool AreInputsStandard(const MapPrevTx& mapInputs) const;

    /** Count ECDSA signature operations the old-fashioned (pre-0.6) way
        @return number of sigops this transaction's outputs will produce when spent
        @see CTransaction::FetchInputs
    */
    unsigned int GetLegacySigOpCount() const;

    /** Count ECDSA signature operations in pay-to-script-hash inputs.

        @param[in] mapInputs  Map of previous transactions that have outputs we're spending
        @return maximum number of sigops required to validate this transaction's inputs
        @see CTransaction::FetchInputs
     */
    unsigned int GetP2SHSigOpCount(const MapPrevTx& mapInputs) const;

    /** Amount of coins spent by this transaction.
        @return sum of all outputs (note: does not include fees)
     */
    int64 GetValueOut() const
    {
        int64 nValueOut = 0;
        BOOST_FOREACH(const CTxOut& txout, vout)
        {
            nValueOut += txout.nValue;
            if (!MoneyRange(txout.nValue) || !MoneyRange(nValueOut))
                throw std::runtime_error("CTransaction::GetValueOut() : value out of range");
        }
        return nValueOut;
    }

    /** Amount of coins coming in to this transaction
        Note that lightweight clients may not know anything besides the hash of previous transactions,
        so may not be able to calculate this.

        @param[in] mapInputs  Map of previous transactions that have outputs we're spending
        @return Sum of value of all inputs (scriptSigs)
        @see CTransaction::FetchInputs
     */
    int64 GetValueIn(const MapPrevTx& mapInputs) const;

    static bool AllowFree(double dPriority) {
        /* High priority transactions are exempt of mandatory fees usually;
         * Phoenixcoin: 480 blocks per day target, priority boundary is 1 PXC day / 250 bytes */
        return(dPriority > (COIN * 480 / 250));
    }

    int64 GetMinFee(uint nBytes = 0, bool fAllowFree = false, enum GetMinFee_mode mode = GMF_BLOCK) const;

    bool ReadFromDisk(CDiskTxPos pos, FILE** pfileRet=NULL)
    {
        CAutoFile filein = CAutoFile(OpenBlockFile(pos.nFile, 0, pfileRet ? "rb+" : "rb"), SER_DISK, CLIENT_VERSION);
        if (!filein)
            return error("CTransaction::ReadFromDisk() : OpenBlockFile failed");

        // Read transaction
        if (fseek(filein, pos.nTxPos, SEEK_SET) != 0)
            return error("CTransaction::ReadFromDisk() : fseek failed");

        try {
            filein >> *this;
        }
        catch (std::exception &e) {
            return error("%s() : deserialize or I/O error", __PRETTY_FUNCTION__);
        }

        // Return file pointer
        if (pfileRet)
        {
            if (fseek(filein, pos.nTxPos, SEEK_SET) != 0)
                return error("CTransaction::ReadFromDisk() : second fseek failed");
            *pfileRet = filein.release();
        }
        return true;
    }

    friend bool operator==(const CTransaction& a, const CTransaction& b)
    {
        return (a.nVersion  == b.nVersion &&
                a.vin       == b.vin &&
                a.vout      == b.vout &&
                a.nLockTime == b.nLockTime);
    }

    friend bool operator!=(const CTransaction& a, const CTransaction& b)
    {
        return !(a == b);
    }


    std::string ToString() const {
        std::string str;
        str += strprintf("CTransaction(hash=%s, ver=%d, vin.size=%" PRIszu ", " \
          "vout.size=%" PRIszu ", nLockTime=%u)\n",
          GetHash().ToString().substr(0,10).c_str(), nVersion, vin.size(),
          vout.size(), nLockTime);
        for (unsigned int i = 0; i < vin.size(); i++)
            str += "    " + vin[i].ToString() + "\n";
        for (unsigned int i = 0; i < vout.size(); i++)
            str += "    " + vout[i].ToString() + "\n";
        return str;
    }

    void print() const
    {
        printf("%s", ToString().c_str());
    }


    bool ReadFromDisk(CTxDB& txdb, COutPoint prevout, CTxIndex& txindexRet);
    bool ReadFromDisk(CTxDB& txdb, COutPoint prevout);
    bool ReadFromDisk(COutPoint prevout);
    bool DisconnectInputs(CTxDB& txdb);

    /** Fetch from memory and/or disk. inputsRet keys are transaction hashes.

     @param[in] txdb  Transaction database
     @param[in] mapTestPool  List of pending changes to the transaction index database
     @param[in] fBlock  True if being called to add a new best-block to the chain
     @param[in] fMiner  True if being called by CreateNewBlock
     @param[out] inputsRet  Pointers to this transaction's inputs
     @param[out] fInvalid  returns true if transaction is invalid
     @return  Returns true if all inputs are in txdb or mapTestPool
     */
    bool FetchInputs(CTxDB& txdb, const std::map<uint256, CTxIndex>& mapTestPool,
                     bool fBlock, bool fMiner, MapPrevTx& inputsRet, bool& fInvalid);

    /** Sanity check previous transactions, then, if all checks succeed,
        mark them as spent by this transaction.

        @param[in] inputs  Previous transactions (from FetchInputs)
        @param[out] mapTestPool  Keeps track of inputs that need to be updated on disk
        @param[in] posThisTx  Position of this transaction on disk
        @param[in] pindexBlock
        @param[in] fBlock  true if called from ConnectBlock
        @param[in] fMiner  true if called from CreateNewBlock
        @param[in] fStrictPayToScriptHash  true if fully validating p2sh transactions
        @return Returns true if all checks succeed
     */
    bool ConnectInputs(MapPrevTx inputs,
                       std::map<uint256, CTxIndex>& mapTestPool, const CDiskTxPos& posThisTx,
                       const CBlockIndex* pindexBlock, bool fBlock, bool fMiner, bool fStrictPayToScriptHash=true);
    bool ClientConnectInputs();
    bool CheckTransaction() const;
    bool AcceptToMemoryPool(CTxDB& txdb, bool fCheckInputs=true, bool* pfMissingInputs=NULL);

protected:
    const CTxOut& GetOutputFor(const CTxIn& input, const MapPrevTx& inputs) const;
};





/** A transaction with a merkle branch linking it to the block chain. */
class CMerkleTx : public CTransaction
{
public:
    uint256 hashBlock;
    std::vector<uint256> vMerkleBranch;
    int nIndex;

    // memory only
    mutable bool fMerkleVerified;


    CMerkleTx()
    {
        Init();
    }

    CMerkleTx(const CTransaction& txIn) : CTransaction(txIn)
    {
        Init();
    }

    void Init()
    {
        hashBlock = 0;
        nIndex = -1;
        fMerkleVerified = false;
    }


    IMPLEMENT_SERIALIZE
    (
        nSerSize += SerReadWrite(s, *(CTransaction*)this, nType, nVersion, ser_action);
        nVersion = this->nVersion;
        READWRITE(hashBlock);
        READWRITE(vMerkleBranch);
        READWRITE(nIndex);
    )


    int SetMerkleBranch(const CBlock* pblock=NULL);
    int GetDepthInMainChain(CBlockIndex* &pindexRet) const;
    int GetDepthInMainChain() const { CBlockIndex *pindexRet; return GetDepthInMainChain(pindexRet); }
    bool IsInMainChain() const { return GetDepthInMainChain() > 0; }
    int GetBlocksToMaturity() const;
    bool AcceptToMemoryPool(CTxDB& txdb, bool fCheckInputs=true);
    bool AcceptToMemoryPool();
};




/**  A txdb record that contains the disk location of a transaction and the
 * locations of transactions that spend its outputs.  vSpent is really only
 * used as a flag, but having the location is very helpful for debugging.
 */
class CTxIndex
{
public:
    CDiskTxPos pos;
    std::vector<CDiskTxPos> vSpent;

    CTxIndex()
    {
        SetNull();
    }

    CTxIndex(const CDiskTxPos& posIn, unsigned int nOutputs)
    {
        pos = posIn;
        vSpent.resize(nOutputs);
    }

    IMPLEMENT_SERIALIZE
    (
        if (!(nType & SER_GETHASH))
            READWRITE(nVersion);
        READWRITE(pos);
        READWRITE(vSpent);
    )

    void SetNull()
    {
        pos.SetNull();
        vSpent.clear();
    }

    bool IsNull()
    {
        return pos.IsNull();
    }

    friend bool operator==(const CTxIndex& a, const CTxIndex& b)
    {
        return (a.pos    == b.pos &&
                a.vSpent == b.vSpent);
    }

    friend bool operator!=(const CTxIndex& a, const CTxIndex& b)
    {
        return !(a == b);
    }
    int GetDepthInMainChain() const;

};





/** Nodes collect new transactions into a block, hash them into a hash tree,
 * and scan through nonce values to make the block's hash satisfy proof-of-work
 * requirements.  When they solve the proof-of-work, they broadcast the block
 * to everyone and the block is added to the block chain.  The first transaction
 * in the block is a special one that creates a new coin owned by the creator
 * of the block.
 *
 * Blocks are appended to blk0001.dat files on disk.  Their location on disk
 * is indexed by CBlockIndex objects in memory.
 */
class CBlock
{
public:
    // header
    static const int CURRENT_VERSION=2;
    int nVersion;
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    unsigned int nTime;
    unsigned int nBits;
    unsigned int nNonce;

    // network and disk
    std::vector<CTransaction> vtx;

    // memory only
    mutable std::vector<uint256> vMerkleTree;

    // Denial-of-service detection:
    mutable int nDoS;
    bool DoS(int nDoSIn, bool fIn) const { nDoS += nDoSIn; return fIn; }

    CBlock()
    {
        SetNull();
    }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(this->nVersion);
        nVersion = this->nVersion;
        READWRITE(hashPrevBlock);
        READWRITE(hashMerkleRoot);
        READWRITE(nTime);
        READWRITE(nBits);
        READWRITE(nNonce);

        // ConnectBlock depends on vtx being last so it can calculate offset
        if (!(nType & (SER_GETHASH|SER_BLOCKHEADERONLY)))
            READWRITE(vtx);
        else if (fRead)
            const_cast<CBlock*>(this)->vtx.clear();
    )

    void SetNull()
    {
        nVersion = CBlock::CURRENT_VERSION;
        hashPrevBlock = 0;
        hashMerkleRoot = 0;
        nTime = 0;
        nBits = 0;
        nNonce = 0;
        vtx.clear();
        vMerkleTree.clear();
        nDoS = 0;
    }

    bool IsNull() const
    {
        return (nBits == 0);
    }

    uint256 GetHash() const
    {
        return Hash(BEGIN(nVersion), END(nNonce));
    }

    /* Calculates block proof-of-work hash using either NeoScrypt or Scrypt */
    uint256 GetHashPoW() const {
        uint profile = 0x0;
        uint256 hash;

        /* All blocks generated up to this time point are Scrypt only */
        if((fTestNet && (nTime < nTestnetSwitchV2)) ||
          (!fTestNet && (nTime < nSwitchV2))) {
            profile = 0x3;
        } else {
            /* All these blocks must be v2+ with valid nHeight */
            int nHeight = GetBlockHeight();
            if(fTestNet) {
                if(nHeight < nTestnetForkTwo)
                  profile = 0x3;
            } else {
                if(nHeight < nForkFive)
                  profile = 0x3;
            }
        }

        profile |= nNeoScryptOptions;

        neoscrypt((uchar *) &nVersion, (uchar *) &hash, profile);

        return(hash);
    }

    /* Extracts block height from v2+ coin base;
     * ignores nVersion because it's unrealiable */
    int GetBlockHeight() const {
        /* Prevents a crash if called on a block header alone */
        if(vtx.size()) {
            /* Serialised CScript */
            std::vector<uchar>::const_iterator scriptsig = vtx[0].vin[0].scriptSig.begin();
            uchar i, scount = scriptsig[0];
            /* Optimise: nTime is 4 bytes always,
             * nHeight must be less for a long time;
             * check against a threshold when the time comes */
            if(scount < 4) {
                int height = 0;
                uchar *pheight = (uchar *) &height;
                for(i = 0; i < scount; i++)
                  pheight[i] = scriptsig[i + 1];
                /* v2+ block with nHeight in coin base */
                return(height);
            }
        }
        /* Not found */
        return(-1);
    }

    int64 GetBlockTime() const
    {
        return (int64)nTime;
    }

    void UpdateTime(const CBlockIndex* pindexPrev);


    uint256 BuildMerkleTree() const
    {
        vMerkleTree.clear();
        BOOST_FOREACH(const CTransaction& tx, vtx)
            vMerkleTree.push_back(tx.GetHash());
        int j = 0;
        for (int nSize = vtx.size(); nSize > 1; nSize = (nSize + 1) / 2)
        {
            for (int i = 0; i < nSize; i += 2)
            {
                int i2 = std::min(i+1, nSize-1);
                vMerkleTree.push_back(Hash(BEGIN(vMerkleTree[j+i]),  END(vMerkleTree[j+i]),
                                           BEGIN(vMerkleTree[j+i2]), END(vMerkleTree[j+i2])));
            }
            j += nSize;
        }
        return (vMerkleTree.empty() ? 0 : vMerkleTree.back());
    }

    std::vector<uint256> GetMerkleBranch(int nIndex) const
    {
        if (vMerkleTree.empty())
            BuildMerkleTree();
        std::vector<uint256> vMerkleBranch;
        int j = 0;
        for (int nSize = vtx.size(); nSize > 1; nSize = (nSize + 1) / 2)
        {
            int i = std::min(nIndex^1, nSize-1);
            vMerkleBranch.push_back(vMerkleTree[j+i]);
            nIndex >>= 1;
            j += nSize;
        }
        return vMerkleBranch;
    }

    static uint256 CheckMerkleBranch(uint256 hash, const std::vector<uint256>& vMerkleBranch, int nIndex)
    {
        if (nIndex == -1)
            return 0;
        BOOST_FOREACH(const uint256& otherside, vMerkleBranch)
        {
            if (nIndex & 1)
                hash = Hash(BEGIN(otherside), END(otherside), BEGIN(hash), END(hash));
            else
                hash = Hash(BEGIN(hash), END(hash), BEGIN(otherside), END(otherside));
            nIndex >>= 1;
        }
        return hash;
    }


    bool WriteToDisk(unsigned int& nFileRet, unsigned int& nBlockPosRet)
    {
        // Open history file to append
        CAutoFile fileout = CAutoFile(AppendBlockFile(nFileRet), SER_DISK, CLIENT_VERSION);
        if(!fileout)
          return(error("CBlock::WriteToDisk() : AppendBlockFile() failed"));

        // Write index header
        unsigned int nSize = fileout.GetSerializeSize(*this);
        fileout << FLATDATA(pchMessageStart) << nSize;

        // Write block
        int fileOutPos = (int)ftell(fileout);
        if(fileOutPos < 0)
          return(error("CBlock::WriteToDisk() : ftell() failed"));
        nBlockPosRet = fileOutPos;
        fileout << *this;

        // Flush stdio buffers and commit to disk before returning
        fflush(fileout);
        if(!IsInitialBlockDownload() || !((nBestHeight + 1) % 100)) {
            if(FileCommit(fileout))
              return(error("CBlock::WriteToDisk() : FileCommit() failed"));
        }

        return(true);
    }

    bool ReadFromDisk(unsigned int nFile, unsigned int nBlockPos, bool fReadTransactions=true)
    {
        SetNull();

        // Open history file to read
        CAutoFile filein = CAutoFile(OpenBlockFile(nFile, nBlockPos, "rb"), SER_DISK, CLIENT_VERSION);
        if(!filein)
          return(error("CBlock::ReadFromDisk() : OpenBlockFile() failed"));
        if (!fReadTransactions)
            filein.nType |= SER_BLOCKHEADERONLY;

        // Read block
        try {
            filein >> *this;
        }
        catch(std::exception &e) {
            return(error("CBlock::ReadFromDisk() : I/O error"));
        }

        return(true);
    }



    void print() const {
        printf("CBlock(hash=%s, ver=%d, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%u, " \
          "nBits=%08x, nNonce=%u, vtx=%" PRIszu ")\n",
          GetHash().ToString().substr(0,20).c_str(), nVersion,
          hashPrevBlock.ToString().substr(0,20).c_str(),
          hashMerkleRoot.ToString().substr(0,10).c_str(),
          nTime, nBits, nNonce, vtx.size());
        for (unsigned int i = 0; i < vtx.size(); i++)
        {
            printf("  ");
            vtx[i].print();
        }
        printf("  vMerkleTree: ");
        for (unsigned int i = 0; i < vMerkleTree.size(); i++)
            printf("%s ", vMerkleTree[i].ToString().substr(0,10).c_str());
        printf("\n");
    }


    bool DisconnectBlock(CTxDB& txdb, CBlockIndex* pindex);
    bool ConnectBlock(CTxDB& txdb, CBlockIndex* pindex, bool fJustCheck=false);
    bool ReadFromDisk(const CBlockIndex* pindex, bool fReadTransactions=true);
    bool SetBestChain(CTxDB& txdb, CBlockIndex* pindexNew);
    bool AddToBlockIndex(unsigned int nFile, unsigned int nBlockPos);
    bool CheckBlock(bool fCheckPOW=true, bool fCheckMerkleRoot=true) const;
    bool AcceptBlock();

private:
    bool SetBestChainInner(CTxDB& txdb, CBlockIndex *pindexNew);
};






/** The block chain is a tree shaped structure starting with the
 * genesis block at the root, with each block potentially having multiple
 * candidates to be the next block.  pprev and pnext link a path through the
 * main/longest chain.  A blockindex may have multiple pprev pointing back
 * to it, but pnext will only point forward to the longest branch, or will
 * be null if the block is not part of the longest chain.
 */
class CBlockIndex
{
public:
    const uint256* phashBlock;
    CBlockIndex* pprev;
    CBlockIndex* pnext;
    unsigned int nFile;
    unsigned int nBlockPos;
    int nHeight;
    CBigNum bnChainWork;

    // block header
    int nVersion;
    uint256 hashMerkleRoot;
    unsigned int nTime;
    unsigned int nBits;
    unsigned int nNonce;


    CBlockIndex()
    {
        phashBlock = NULL;
        pprev = NULL;
        pnext = NULL;
        nFile = 0;
        nBlockPos = 0;
        nHeight = 0;
        bnChainWork = 0;

        nVersion       = 0;
        hashMerkleRoot = 0;
        nTime          = 0;
        nBits          = 0;
        nNonce         = 0;
    }

    CBlockIndex(unsigned int nFileIn, unsigned int nBlockPosIn, CBlock& block)
    {
        phashBlock = NULL;
        pprev = NULL;
        pnext = NULL;
        nFile = nFileIn;
        nBlockPos = nBlockPosIn;
        nHeight = 0;
        bnChainWork = 0;

        nVersion       = block.nVersion;
        hashMerkleRoot = block.hashMerkleRoot;
        nTime          = block.nTime;
        nBits          = block.nBits;
        nNonce         = block.nNonce;
    }

    CBlock GetBlockHeader() const
    {
        CBlock block;
        block.nVersion       = nVersion;
        if (pprev)
            block.hashPrevBlock = pprev->GetBlockHash();
        block.hashMerkleRoot = hashMerkleRoot;
        block.nTime          = nTime;
        block.nBits          = nBits;
        block.nNonce         = nNonce;
        return block;
    }

    uint256 GetBlockHash() const
    {
        return *phashBlock;
    }

    int64 GetBlockTime() const
    {
        return (int64)nTime;
    }

    CBigNum GetBlockWork() const
    {
        CBigNum bnTarget;
        bnTarget.SetCompact(nBits);
        if (bnTarget <= 0)
            return 0;
        return (CBigNum(1)<<256) / (bnTarget+1);
    }

    bool IsInMainChain() const
    {
        return (pnext || this == pindexBest);
    }

    enum { nMedianTimeSpan=11 };

    int64 GetMedianTimePast() const
    {
        int64 pmedian[nMedianTimeSpan];
        int64* pbegin = &pmedian[nMedianTimeSpan];
        int64* pend = &pmedian[nMedianTimeSpan];

        const CBlockIndex* pindex = this;
        for (int i = 0; i < nMedianTimeSpan && pindex; i++, pindex = pindex->pprev)
            *(--pbegin) = pindex->GetBlockTime();

        std::sort(pbegin, pend);
        return pbegin[(pend - pbegin)/2];
    }

    int64 GetMedianTime() const
    {
        const CBlockIndex* pindex = this;
        for (int i = 0; i < nMedianTimeSpan/2; i++)
        {
            if (!pindex->pnext)
                return GetBlockTime();
            pindex = pindex->pnext;
        }
        return pindex->GetMedianTimePast();
    }

    /**
     * Returns true if there are nRequired or more blocks of minVersion or above
     * in the last nToCheck blocks, starting at pstart and going backwards.
     */
    static bool IsSuperMajority(int minVersion, const CBlockIndex* pstart,
                                unsigned int nRequired, unsigned int nToCheck);

    /* Advanced average block time calculator */
    uint GetAverageTimePast(uint nAvgTimeSpan, uint nMinDelay) const {
        uint avg[nAvgTimeSpan];
        uint nTempTime, i;
        uint64 nAvgAccum;
        const CBlockIndex *pindex = this;

        /* Keep it fail safe */
        if(!nAvgTimeSpan) return(0);

        /* Initialise the elements to zero */
        for(i = 0; i < nAvgTimeSpan; i++)
          avg[i] = 0;

        /* Fill with the time stamps */
        for(i = nAvgTimeSpan; i && pindex; i--, pindex = pindex->pprev)
          avg[i - 1] = pindex->nTime;

        /* Not enough input blocks */
        if(!avg[0]) return(0);

        /* Time travel aware accumulator */
        nTempTime = avg[0];
        for(i = 1, nAvgAccum = nTempTime; i < nAvgTimeSpan; i++) {
            /* Update the accumulator either with an actual or minimal
             * delay supplied to prevent extremely fast blocks */
            if(avg[i] < (nTempTime + nMinDelay))
              nTempTime += nMinDelay;
            else
              nTempTime  = avg[i];
            nAvgAccum += nTempTime;
        }

        nTempTime = (uint)(nAvgAccum / (uint64)nAvgTimeSpan);

        return(nTempTime);
    }

    std::string ToString() const
    {
        return strprintf("CBlockIndex(pprev=%p, pnext=%p, nFile=%u, nBlockPos=%-6u nHeight=%d, merkle=%s, hashBlock=%s)",
            pprev, pnext, nFile, nBlockPos, nHeight,
            hashMerkleRoot.ToString().substr(0,10).c_str(),
            GetBlockHash().ToString().substr(0,20).c_str());
    }

    void print() const
    {
        printf("%s\n", ToString().c_str());
    }
};



/** Used to marshal pointers into hashes for db storage. */
class CDiskBlockIndex : public CBlockIndex
{
public:
    uint256 hashPrev;
    uint256 hashNext;

    CDiskBlockIndex()
    {
        hashPrev = 0;
        hashNext = 0;
    }

    explicit CDiskBlockIndex(CBlockIndex* pindex) : CBlockIndex(*pindex)
    {
        hashPrev = (pprev ? pprev->GetBlockHash() : 0);
        hashNext = (pnext ? pnext->GetBlockHash() : 0);
    }

    IMPLEMENT_SERIALIZE
    (
        if (!(nType & SER_GETHASH))
            READWRITE(nVersion);

        READWRITE(hashNext);
        READWRITE(nFile);
        READWRITE(nBlockPos);
        READWRITE(nHeight);

        // block header
        READWRITE(this->nVersion);
        READWRITE(hashPrev);
        READWRITE(hashMerkleRoot);
        READWRITE(nTime);
        READWRITE(nBits);
        READWRITE(nNonce);
    )

    uint256 GetBlockHash() const
    {
        CBlock block;
        block.nVersion        = nVersion;
        block.hashPrevBlock   = hashPrev;
        block.hashMerkleRoot  = hashMerkleRoot;
        block.nTime           = nTime;
        block.nBits           = nBits;
        block.nNonce          = nNonce;
        return block.GetHash();
    }


    std::string ToString() const
    {
        std::string str = "CDiskBlockIndex(";
        str += CBlockIndex::ToString();
        str += strprintf("\n                hashBlock=%s, hashPrev=%s, hashNext=%s)",
            GetBlockHash().ToString().c_str(),
            hashPrev.ToString().substr(0,20).c_str(),
            hashNext.ToString().substr(0,20).c_str());
        return str;
    }

    void print() const
    {
        printf("%s\n", ToString().c_str());
    }
};








/** Describes a place in the block chain to another node such that if the
 * other node doesn't have the same branch, it can find a recent common trunk.
 * The further back it is, the further before the fork it may be.
 */
class CBlockLocator
{
protected:
    std::vector<uint256> vHave;
public:

    CBlockLocator()
    {
    }

    explicit CBlockLocator(const CBlockIndex* pindex)
    {
        Set(pindex);
    }

    explicit CBlockLocator(uint256 hashBlock)
    {
        std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi != mapBlockIndex.end())
            Set((*mi).second);
    }

    CBlockLocator(const std::vector<uint256>& vHaveIn)
    {
        vHave = vHaveIn;
    }

    IMPLEMENT_SERIALIZE
    (
        if (!(nType & SER_GETHASH))
            READWRITE(nVersion);
        READWRITE(vHave);
    )

    void SetNull()
    {
        vHave.clear();
    }

    bool IsNull()
    {
        return vHave.empty();
    }

    void Set(const CBlockIndex* pindex)
    {
        vHave.clear();
        int nStep = 1;
        while (pindex)
        {
            vHave.push_back(pindex->GetBlockHash());

            // Exponentially larger steps back
            for (int i = 0; pindex && i < nStep; i++)
                pindex = pindex->pprev;
            if (vHave.size() > 10)
                nStep *= 2;
        }
        vHave.push_back(hashGenesisBlock);
    }

    int GetDistanceBack()
    {
        // Retrace how far back it was in the sender's branch
        int nDistance = 0;
        int nStep = 1;
        BOOST_FOREACH(const uint256& hash, vHave)
        {
            std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
            if (mi != mapBlockIndex.end())
            {
                CBlockIndex* pindex = (*mi).second;
                if (pindex->IsInMainChain())
                    return nDistance;
            }
            nDistance += nStep;
            if (nDistance > 10)
                nStep *= 2;
        }
        return nDistance;
    }

    CBlockIndex* GetBlockIndex()
    {
        // Find the first block the caller has in the main chain
        BOOST_FOREACH(const uint256& hash, vHave)
        {
            std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
            if (mi != mapBlockIndex.end())
            {
                CBlockIndex* pindex = (*mi).second;
                if (pindex->IsInMainChain())
                    return pindex;
            }
        }
        return pindexGenesisBlock;
    }

    uint256 GetBlockHash()
    {
        // Find the first block the caller has in the main chain
        BOOST_FOREACH(const uint256& hash, vHave)
        {
            std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
            if (mi != mapBlockIndex.end())
            {
                CBlockIndex* pindex = (*mi).second;
                if (pindex->IsInMainChain())
                    return hash;
            }
        }
        return hashGenesisBlock;
    }

    int GetHeight()
    {
        CBlockIndex* pindex = GetBlockIndex();
        if (!pindex)
            return 0;
        return pindex->nHeight;
    }
};








class CTxMemPool
{
public:
    mutable CCriticalSection cs;
    std::map<uint256, CTransaction> mapTx;
    std::map<COutPoint, CInPoint> mapNextTx;

    bool accept(CTxDB& txdb, CTransaction &tx,
                bool fCheckInputs, bool* pfMissingInputs);
    bool addUnchecked(const uint256& hash, CTransaction &tx);
    bool remove(CTransaction &tx);
    void clear();
    void queryHashes(std::vector<uint256>& vtxid);

    uint size() {
        LOCK(cs);
        return(mapTx.size());
    }

    bool exists(uint256 hash)
    {
        return (mapTx.count(hash) != 0);
    }

    CTransaction& lookup(uint256 hash)
    {
        return mapTx[hash];
    }
};

extern CTxMemPool mempool;

#endif /* MAIN_H */
