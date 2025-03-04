// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software licence, see the accompanying
// file LICENCE or http://opensource.org/license/mit

#include <string>
#include <algorithm>
#include <map>
#include <utility>
#include <list>
#include <vector>
#include <set>
#include <limits>

#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include "init.h"
#include "alert.h"
#include "checkpoints.h"
#include "db.h"
#include "wallet.h"
#include "util.h"
#include "main.h"

using namespace std;
using namespace boost;

extern CWallet *pwalletMain;

//
// Global state
//

CCriticalSection cs_setpwalletRegistered;
set<CWallet*> setpwalletRegistered;

CCriticalSection cs_main;

CTxMemPool mempool;
unsigned int nTransactionsUpdated = 0;

int nBaseMaturity = BASE_MATURITY;

map<uint256, CBlockIndex*> mapBlockIndex;
uint256 hashGenesisBlock("0xbe2f30f9e8db8f430056869c43503a992d232b28508e83eda101161a18cf7c73");
uint256 hashGenesisBlockTestNet("0xecd47eee16536f7d03d64643cfc8c61b22093f8bf2c9358bf8b6f4dcb5f13192");
static CBigNum bnProofOfWorkLimit(~uint256(0) >> 20);
/* The difficulty after switching to NeoScrypt (0.015625) */
static CBigNum bnNeoScryptSwitch(~uint256(0) >> 26);
CBlockIndex* pindexGenesisBlock = NULL;
int nBestHeight = -1;
CBigNum bnBestChainWork = 0;
CBigNum bnBestInvalidWork = 0;
uint256 hashBestChain = 0;
CBlockIndex* pindexBest = NULL;
int64 nTimeBestReceived = 0;

CMedianFilter<int> cPeerBlockCounts(5, 0); // Amount of blocks that other nodes claim to have

map<uint256, CBlock*> mapOrphanBlocks;
multimap<uint256, CBlock*> mapOrphanBlocksByPrev;

map<uint256, CDataStream*> mapOrphanTransactions;
map<uint256, map<uint256, CDataStream*> > mapOrphanTransactionsByPrev;

// Constant stuff for coinbase transactions we create:
CScript COINBASE_FLAGS;

const string strMessageMagic = "Phoenixcoin Signed Message:\n";

double dHashesPerSec;
int64 nHPSTimerStart;

// Settings
int64 nTransactionFee = 0;
int64 nMinimumInputValue = TX_DUST;

/* Network magic number;
 * 0xFE and ASCII 'P' 'X' 'C' mapped into extended characters */
uchar pchMessageStart[4] = { 0xFE, 0xD0, 0xD8, 0xC3 };

extern enum Checkpoints::CPMode CheckpointsMode;

//////////////////////////////////////////////////////////////////////////////
//
// dispatching functions
//

// These functions dispatch to one or all registered wallets


void RegisterWallet(CWallet* pwalletIn)
{
    {
        LOCK(cs_setpwalletRegistered);
        setpwalletRegistered.insert(pwalletIn);
    }
}

void UnregisterWallet(CWallet* pwalletIn)
{
    {
        LOCK(cs_setpwalletRegistered);
        setpwalletRegistered.erase(pwalletIn);
    }
}

// check whether the passed transaction is from us
bool static IsFromMe(CTransaction& tx)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        if (pwallet->IsFromMe(tx))
            return true;
    return false;
}

// get the wallet transaction with the given hash (if it exists)
bool static GetTransaction(const uint256& hashTx, CWalletTx& wtx)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        if (pwallet->GetTransaction(hashTx,wtx))
            return true;
    return false;
}

// erases transaction with the given hash from all wallets
static void EraseFromWallets(uint256 hash)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->EraseFromWallet(hash);
}

// make sure all wallets know about the given transaction, in the given block
void SyncWithWallets(const CTransaction& tx, const CBlock* pblock, bool fUpdate)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->AddToWalletIfInvolvingMe(tx, pblock, fUpdate);
}

// notify wallets about a new best chain
static void SetBestChain(const CBlockLocator& loc)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->SetBestChain(loc);
}

// notify wallets about an updated transaction
static void UpdatedTransaction(const uint256& hashTx)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->UpdatedTransaction(hashTx);
}

// dump all wallets
static void PrintWallets(const CBlock& block)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->PrintWallet(block);
}

// notify wallets about an incoming inventory (for request counts)
static void Inventory(const uint256& hash)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->Inventory(hash);
}

// ask wallets to resend their transactions
void ResendWalletTransactions(bool fForce) {
    BOOST_FOREACH(CWallet *pwallet, setpwalletRegistered)
      pwallet->ResendWalletTransactions(fForce);
}







//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

bool AddOrphanTx(const CDataStream& vMsg)
{
    CTransaction tx;
    CDataStream(vMsg) >> tx;
    uint256 hash = tx.GetHash();
    if (mapOrphanTransactions.count(hash))
        return false;

    CDataStream* pvMsg = new CDataStream(vMsg);

    // Ignore big transactions, to avoid a
    // send-big-orphans memory exhaustion attack. If a peer has a legitimate
    // large transaction with a missing parent then we assume
    // it will rebroadcast it later, after the parent transaction(s)
    // have been mined or received.
    // 10,000 orphans, each of which is at most 5,000 bytes big is
    // at most 500 megabytes of orphans:
    if(pvMsg->size() > 5000) {
        printf("ignoring large orphan tx (size: %" PRIszu ", hash: %s)\n", pvMsg->size(),
          hash.ToString().substr(0,10).c_str());
        delete(pvMsg);
        return(false);
    }

    mapOrphanTransactions[hash] = pvMsg;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
        mapOrphanTransactionsByPrev[txin.prevout.hash].insert(make_pair(hash, pvMsg));

    printf("stored orphan tx %s (mapsz %" PRIszu ")\n", hash.ToString().substr(0,10).c_str(),
      mapOrphanTransactions.size());

    return(true);
}

static void EraseOrphanTx(uint256 hash)
{
    if (!mapOrphanTransactions.count(hash))
        return;
    const CDataStream* pvMsg = mapOrphanTransactions[hash];
    CTransaction tx;
    CDataStream(*pvMsg) >> tx;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        mapOrphanTransactionsByPrev[txin.prevout.hash].erase(hash);
        if (mapOrphanTransactionsByPrev[txin.prevout.hash].empty())
            mapOrphanTransactionsByPrev.erase(txin.prevout.hash);
    }
    delete pvMsg;
    mapOrphanTransactions.erase(hash);
}

unsigned int LimitOrphanTxSize(unsigned int nMaxOrphans)
{
    unsigned int nEvicted = 0;
    while (mapOrphanTransactions.size() > nMaxOrphans)
    {
        // Evict a random orphan:
        uint256 randomhash = GetRandHash();
        map<uint256, CDataStream*>::iterator it = mapOrphanTransactions.lower_bound(randomhash);
        if (it == mapOrphanTransactions.end())
            it = mapOrphanTransactions.begin();
        EraseOrphanTx(it->first);
        ++nEvicted;
    }
    return nEvicted;
}







//////////////////////////////////////////////////////////////////////////////
//
// CTransaction and CTxIndex
//

bool CTransaction::ReadFromDisk(CTxDB& txdb, COutPoint prevout, CTxIndex& txindexRet)
{
    SetNull();
    if (!txdb.ReadTxIndex(prevout.hash, txindexRet))
        return false;
    if (!ReadFromDisk(txindexRet.pos))
        return false;
    if (prevout.n >= vout.size())
    {
        SetNull();
        return false;
    }
    return true;
}

bool CTransaction::ReadFromDisk(CTxDB& txdb, COutPoint prevout)
{
    CTxIndex txindex;
    return ReadFromDisk(txdb, prevout, txindex);
}

bool CTransaction::ReadFromDisk(COutPoint prevout)
{
    CTxDB txdb("r");
    CTxIndex txindex;
    return ReadFromDisk(txdb, prevout, txindex);
}

bool CTransaction::IsStandard() const
{
    if (nVersion > CTransaction::CURRENT_VERSION)
        return false;

    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        // Biggest 'standard' txin is a 3-signature 3-of-3 CHECKMULTISIG
        // pay-to-script-hash, which is 3 ~80-byte signatures, 3
        // ~65-byte public keys, plus a few script ops.
        if (txin.scriptSig.size() > 500)
            return false;
        if (!txin.scriptSig.IsPushOnly())
            return false;
    }
    BOOST_FOREACH(const CTxOut& txout, vout) {
        if (!::IsStandard(txout.scriptPubKey))
            return false;
        if (txout.nValue == 0)
            return false;
    }
    return true;
}

//
// Check transaction inputs, and make sure any
// pay-to-script-hash transactions are evaluating IsStandard scripts
//
// Why bother? To avoid denial-of-service attacks; an attacker
// can submit a standard HASH... OP_EQUAL transaction,
// which will get accepted into blocks. The redemption
// script can be anything; an attacker could use a very
// expensive-to-check-upon-redemption script like:
//   DUP CHECKSIG DROP ... repeated 100 times... OP_1
//
bool CTransaction::AreInputsStandard(const MapPrevTx& mapInputs) const
{
    if (IsCoinBase())
        return true; // Coinbases don't use vin normally

    for (unsigned int i = 0; i < vin.size(); i++)
    {
        const CTxOut& prev = GetOutputFor(vin[i], mapInputs);

        vector<vector<unsigned char> > vSolutions;
        txnouttype whichType;
        // get the scriptPubKey corresponding to this input:
        const CScript& prevScript = prev.scriptPubKey;
        if (!Solver(prevScript, whichType, vSolutions))
            return false;
        int nArgsExpected = ScriptSigArgsExpected(whichType, vSolutions);
        if (nArgsExpected < 0)
            return false;

        // Transactions with extra stuff in their scriptSigs are
        // non-standard. Note that this EvalScript() call will
        // be quick, because if there are any operations
        // beside "push data" in the scriptSig the
        // IsStandard() call returns false
        vector<vector<unsigned char> > stack;
        if (!EvalScript(stack, vin[i].scriptSig, *this, i, 0))
            return false;

        if (whichType == TX_SCRIPTHASH)
        {
            if (stack.empty())
                return false;
            CScript subscript(stack.back().begin(), stack.back().end());
            vector<vector<unsigned char> > vSolutions2;
            txnouttype whichType2;
            if (!Solver(subscript, whichType2, vSolutions2))
                return false;
            if (whichType2 == TX_SCRIPTHASH)
                return false;

            int tmpExpected;
            tmpExpected = ScriptSigArgsExpected(whichType2, vSolutions2);
            if (tmpExpected < 0)
                return false;
            nArgsExpected += tmpExpected;
        }

        if (stack.size() != (unsigned int)nArgsExpected)
            return false;
    }

    return true;
}

unsigned int
CTransaction::GetLegacySigOpCount() const
{
    unsigned int nSigOps = 0;
    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    BOOST_FOREACH(const CTxOut& txout, vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}


int CMerkleTx::SetMerkleBranch(const CBlock* pblock)
{
    if (fClient)
    {
        if (hashBlock == 0)
            return 0;
    }
    else
    {
        CBlock blockTmp;
        if (pblock == NULL)
        {
            // Load the block this tx is in
            CTxIndex txindex;
            if (!CTxDB("r").ReadTxIndex(GetHash(), txindex))
                return 0;
            if (!blockTmp.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos))
                return 0;
            pblock = &blockTmp;
        }

        // Update the tx's hashBlock
        hashBlock = pblock->GetHash();

        // Locate the transaction
        for (nIndex = 0; nIndex < (int)pblock->vtx.size(); nIndex++)
            if (pblock->vtx[nIndex] == *(CTransaction*)this)
                break;
        if (nIndex == (int)pblock->vtx.size())
        {
            vMerkleBranch.clear();
            nIndex = -1;
            printf("ERROR: SetMerkleBranch() : couldn't find tx in block\n");
            return 0;
        }

        // Fill in merkle branch
        vMerkleBranch = pblock->GetMerkleBranch(nIndex);
    }

    // Is the tx in a block that's in the main chain
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    return pindexBest->nHeight - pindex->nHeight + 1;
}







bool CTransaction::CheckTransaction() const
{
    // Basic checks that don't depend on any context
    if (vin.empty())
        return DoS(10, error("CTransaction::CheckTransaction() : vin empty"));
    if (vout.empty())
        return DoS(10, error("CTransaction::CheckTransaction() : vout empty"));
    // Size limits
    if (::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
        return DoS(100, error("CTransaction::CheckTransaction() : size limits failed"));

    // Check for negative or overflow output values
    int64 nValueOut = 0;
    BOOST_FOREACH(const CTxOut& txout, vout)
    {
        if (txout.nValue < 0)
            return DoS(100, error("CTransaction::CheckTransaction() : txout.nValue negative"));
        if (txout.nValue > MAX_MONEY)
            return DoS(100, error("CTransaction::CheckTransaction() : txout.nValue too high"));
        nValueOut += txout.nValue;
        if (!MoneyRange(nValueOut))
            return DoS(100, error("CTransaction::CheckTransaction() : txout total out of range"));
    }

    // Check for duplicate inputs
    set<COutPoint> vInOutPoints;
    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        if (vInOutPoints.count(txin.prevout))
            return false;
        vInOutPoints.insert(txin.prevout);
    }

    if (IsCoinBase())
    {
        if (vin[0].scriptSig.size() < 2 || vin[0].scriptSig.size() > 100)
            return DoS(100, error("CTransaction::CheckTransaction() : coinbase script size"));
    }
    else
    {
        BOOST_FOREACH(const CTxIn& txin, vin)
            if (txin.prevout.IsNull())
                return DoS(10, error("CTransaction::CheckTransaction() : prevout is null"));
    }

    return true;
}

int64 CTransaction::GetMinFee(uint nBytes, bool fAllowFree,
  enum GetMinFee_mode mode) const {

    // Base fee is either MIN_TX_FEE or MIN_RELAY_TX_FEE
    int64 nBaseFee = (mode == GMF_RELAY) ? MIN_RELAY_TX_FEE : MIN_TX_FEE;

    uint nNewBlockSize = (mode == GMF_SEND) ? nBytes : 1000 + nBytes;
    int64 nMinFee = (1 + (int64)nBytes / 1000) * nBaseFee;

    if(fAllowFree) {
        if(mode == GMF_SEND) {
            /* Limit size of free high priority transactions */
            if(nBytes < 2000) nMinFee = 0;
        } else {
            /* GMF_BLOCK, GMF_RELAY:
             * Limit block space for free transactions */
            if(nNewBlockSize < 11000) nMinFee = 0;
        }
    }

    /* Dust spam filter: require a base fee for any micro output */
    BOOST_FOREACH(const CTxOut &txout, vout)
      if(txout.nValue < TX_DUST) nMinFee += nBaseFee;

    // Raise the price as the block approaches full
    if((mode != GMF_SEND) && (nNewBlockSize >= MAX_BLOCK_SIZE_GEN / 2)) {
        if(nNewBlockSize >= MAX_BLOCK_SIZE_GEN) return(MAX_MONEY);
        nMinFee *= MAX_BLOCK_SIZE_GEN / (MAX_BLOCK_SIZE_GEN - nNewBlockSize);
        if(!MoneyRange(nMinFee)) nMinFee = MAX_MONEY;
    }

    return(nMinFee);
}


bool CTxMemPool::accept(CTxDB& txdb, CTransaction &tx, bool fCheckInputs,
                        bool* pfMissingInputs)
{
    if (pfMissingInputs)
        *pfMissingInputs = false;

    if (!tx.CheckTransaction())
        return error("CTxMemPool::accept() : CheckTransaction failed");

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return tx.DoS(100, error("CTxMemPool::accept() : coinbase as individual tx"));

    // To help v0.1.5 clients who would see it as a negative number
    if ((int64)tx.nLockTime > std::numeric_limits<int>::max())
        return error("CTxMemPool::accept() : not accepting nLockTime beyond 2038 yet");

    // Rather not work on nonstandard transactions (unless -testnet)
    if (!fTestNet && !tx.IsStandard())
        return error("CTxMemPool::accept() : nonstandard transaction type");

    // Do we already have it?
    uint256 hash = tx.GetHash();
    {
        LOCK(cs);
        if (mapTx.count(hash))
            return false;
    }
    if (fCheckInputs)
        if (txdb.ContainsTx(hash))
            return false;

    // Check for conflicts with in-memory transactions
    CTransaction* ptxOld = NULL;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        COutPoint outpoint = tx.vin[i].prevout;
        if (mapNextTx.count(outpoint))
        {
            // Disable replacement feature for now
            return false;

            // Allow replacing with a newer version of the same transaction
            if (i != 0)
                return false;
            ptxOld = mapNextTx[outpoint].ptx;
            if (ptxOld->IsFinal())
                return false;
            if (!tx.IsNewerThan(*ptxOld))
                return false;
            for (unsigned int i = 0; i < tx.vin.size(); i++)
            {
                COutPoint outpoint = tx.vin[i].prevout;
                if (!mapNextTx.count(outpoint) || mapNextTx[outpoint].ptx != ptxOld)
                    return false;
            }
            break;
        }
    }

    if (fCheckInputs)
    {
        MapPrevTx mapInputs;
        map<uint256, CTxIndex> mapUnused;
        bool fInvalid = false;
        if (!tx.FetchInputs(txdb, mapUnused, false, false, mapInputs, fInvalid))
        {
            if (fInvalid)
                return error("CTxMemPool::accept() : FetchInputs found invalid tx %s", hash.ToString().substr(0,10).c_str());
            if (pfMissingInputs)
                *pfMissingInputs = true;
            return false;
        }

        // Check for non-standard pay-to-script-hash in inputs
        if (!tx.AreInputsStandard(mapInputs) && !fTestNet)
            return error("CTxMemPool::accept() : nonstandard transaction input");

        // Note: if you modify this code to accept non-standard transactions, then
        // you should add code here to check that the transaction does a
        // reasonable number of ECDSA signature verifications.

        int64 nFees = tx.GetValueIn(mapInputs)-tx.GetValueOut();
        unsigned int nSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);

        // Don't accept it if it can't get into a block
        int64 txMinFee = tx.GetMinFee(nSize, true, GMF_RELAY);
        if(nFees < txMinFee) {
            return(error("CTxMemPool::accept() : not enough fees for tx %s, " \
              "%" PRI64d " < %" PRI64d "", hash.ToString().c_str(), nFees, txMinFee));
        }

        // Continuously rate-limit free transactions
        // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
        // be annoying or make others' transactions take longer to confirm.
        if (nFees < MIN_RELAY_TX_FEE)
        {
            static CCriticalSection cs;
            static double dFreeCount;
            static int64 nLastTime;
            int64 nNow = GetTime();

            {
                LOCK(cs);
                // Use an exponentially decaying ~10-minute window:
                dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
                nLastTime = nNow;
                // -limitfreerelay unit is thousand-bytes-per-minute
                // At default rate it would take over a month to fill 1GB
                if (dFreeCount > GetArg("-limitfreerelay", 15)*10*1000 && !IsFromMe(tx))
                    return error("CTxMemPool::accept() : free transaction rejected by rate limiter");
                if (fDebug)
                    printf("Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
                dFreeCount += nSize;
            }
        }

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        if (!tx.ConnectInputs(mapInputs, mapUnused, CDiskTxPos(1,1,1), pindexBest, false, false))
        {
            return error("CTxMemPool::accept() : ConnectInputs failed %s", hash.ToString().substr(0,10).c_str());
        }
    }

    // Store transaction in memory
    {
        LOCK(cs);
        if (ptxOld)
        {
            printf("CTxMemPool::accept() : replacing tx %s with new version\n", ptxOld->GetHash().ToString().c_str());
            remove(*ptxOld);
        }
        addUnchecked(hash, tx);
    }

    ///// are we sure this is ok when loading transactions or restoring block txes
    // If updated, erase old tx from wallet
    if (ptxOld)
        EraseFromWallets(ptxOld->GetHash());

    printf("CTxMemPool::accept() : accepted %s (poolsz %" PRIszu ")\n",
      hash.ToString().substr(0,10).c_str(), mapTx.size());

    return(true);
}

bool CTransaction::AcceptToMemoryPool(CTxDB& txdb, bool fCheckInputs, bool* pfMissingInputs)
{
    return mempool.accept(txdb, *this, fCheckInputs, pfMissingInputs);
}

bool CTxMemPool::addUnchecked(const uint256& hash, CTransaction &tx)
{
    // Add to memory pool without checking anything.  Don't call this directly,
    // call CTxMemPool::accept to properly check the transaction first.
    {
        mapTx[hash] = tx;
        for (unsigned int i = 0; i < tx.vin.size(); i++)
            mapNextTx[tx.vin[i].prevout] = CInPoint(&mapTx[hash], i);
        nTransactionsUpdated++;
    }
    return true;
}


bool CTxMemPool::remove(CTransaction &tx)
{
    // Remove transaction from memory pool
    {
        LOCK(cs);
        uint256 hash = tx.GetHash();
        if (mapTx.count(hash))
        {
            BOOST_FOREACH(const CTxIn& txin, tx.vin)
                mapNextTx.erase(txin.prevout);
            mapTx.erase(hash);
            nTransactionsUpdated++;
        }
    }
    return true;
}

void CTxMemPool::clear()
{
    LOCK(cs);
    mapTx.clear();
    mapNextTx.clear();
    ++nTransactionsUpdated;
}

void CTxMemPool::queryHashes(std::vector<uint256>& vtxid)
{
    vtxid.clear();

    LOCK(cs);
    vtxid.reserve(mapTx.size());
    for (map<uint256, CTransaction>::iterator mi = mapTx.begin(); mi != mapTx.end(); ++mi)
        vtxid.push_back((*mi).first);
}




int CMerkleTx::GetDepthInMainChain(CBlockIndex* &pindexRet) const
{
    if (hashBlock == 0 || nIndex == -1)
        return 0;

    // Find the block it claims to be in
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    // Make sure the merkle branch connects to this block
    if (!fMerkleVerified)
    {
        if (CBlock::CheckMerkleBranch(GetHash(), vMerkleBranch, nIndex) != pindex->hashMerkleRoot)
            return 0;
        fMerkleVerified = true;
    }

    pindexRet = pindex;
    return pindexBest->nHeight - pindex->nHeight + 1;
}


int CMerkleTx::GetBlocksToMaturity() const
{
    if (!IsCoinBase())
        return 0;
    return(max(0, (nBaseMaturity + BASE_MATURITY_OFFSET) - GetDepthInMainChain()));
}


bool CMerkleTx::AcceptToMemoryPool(CTxDB& txdb, bool fCheckInputs)
{
    if (fClient)
    {
        if (!IsInMainChain() && !ClientConnectInputs())
            return false;
        return CTransaction::AcceptToMemoryPool(txdb, false);
    }
    else
    {
        return CTransaction::AcceptToMemoryPool(txdb, fCheckInputs);
    }
}

bool CMerkleTx::AcceptToMemoryPool()
{
    CTxDB txdb("r");
    return AcceptToMemoryPool(txdb);
}



bool CWalletTx::AcceptWalletTransaction(CTxDB& txdb, bool fCheckInputs)
{

    {
        LOCK(mempool.cs);
        // Add previous supporting transactions first
        BOOST_FOREACH(CMerkleTx& tx, vtxPrev)
        {
            if (!tx.IsCoinBase())
            {
                uint256 hash = tx.GetHash();
                if (!mempool.exists(hash) && !txdb.ContainsTx(hash))
                    tx.AcceptToMemoryPool(txdb, fCheckInputs);
            }
        }
        return AcceptToMemoryPool(txdb, fCheckInputs);
    }
    return false;
}

bool CWalletTx::AcceptWalletTransaction()
{
    CTxDB txdb("r");
    return AcceptWalletTransaction(txdb);
}

int CTxIndex::GetDepthInMainChain() const
{
    // Read block header
    CBlock block;
    if (!block.ReadFromDisk(pos.nFile, pos.nBlockPos, false))
        return 0;
    // Find the block in the index
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(block.GetHash());
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;
    return 1 + nBestHeight - pindex->nHeight;
}

// Return transaction in tx, and if it was found inside a block, its hash is placed in hashBlock
bool GetTransaction(const uint256 &hash, CTransaction &tx, uint256 &hashBlock)
{
    {
        LOCK(cs_main);
        {
            LOCK(mempool.cs);
            if (mempool.exists(hash))
            {
                tx = mempool.lookup(hash);
                return true;
            }
        }
        CTxDB txdb("r");
        CTxIndex txindex;
        if (tx.ReadFromDisk(txdb, COutPoint(hash, 0), txindex))
        {
            CBlock block;
            if (block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
                hashBlock = block.GetHash();
            return true;
        }
    }
    return false;
}








//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

static CBlockIndex* pblockindexFBBHLast;
CBlockIndex* FindBlockByHeight(int nHeight)
{
    CBlockIndex *pblockindex;
    if (nHeight < nBestHeight / 2)
        pblockindex = pindexGenesisBlock;
    else
        pblockindex = pindexBest;
    if (pblockindexFBBHLast && abs(nHeight - pblockindex->nHeight) > abs(nHeight - pblockindexFBBHLast->nHeight))
        pblockindex = pblockindexFBBHLast;
    while (pblockindex->nHeight > nHeight)
        pblockindex = pblockindex->pprev;
    while (pblockindex->nHeight < nHeight)
        pblockindex = pblockindex->pnext;
    pblockindexFBBHLast = pblockindex;
    return pblockindex;
}

bool CBlock::ReadFromDisk(const CBlockIndex* pindex, bool fReadTransactions)
{
    if (!fReadTransactions)
    {
        *this = pindex->GetBlockHeader();
        return true;
    }
    if (!ReadFromDisk(pindex->nFile, pindex->nBlockPos, fReadTransactions))
        return false;
    if (GetHash() != pindex->GetBlockHash())
        return error("CBlock::ReadFromDisk() : GetHash() doesn't match index");
    return true;
}

uint256 static GetOrphanRoot(const CBlock* pblock)
{
    // Work back to the first block in the orphan chain
    while (mapOrphanBlocks.count(pblock->hashPrevBlock))
        pblock = mapOrphanBlocks[pblock->hashPrevBlock];
    return pblock->GetHash();
}

/* Find the parent block needed by a given orphan block */
uint256 WantedByOrphan(const CBlock *pblockOrphan) {

    while(mapOrphanBlocks.count(pblockOrphan->hashPrevBlock)) {
        pblockOrphan = mapOrphanBlocks[pblockOrphan->hashPrevBlock];
    }
    return(pblockOrphan->hashPrevBlock);
}

int64 GetProofOfWorkReward(int nHeight, int64 nFees) {

    int64 nSubsidy = 50 * COIN;

    /* 25 PXC per block:
     * a) between the 3rd and 4th hard fork;
     * b) before the 1st testnet hard fork */
    if(((nHeight >= nForkThree) && (nHeight < nForkFour)) || (fTestNet && (nHeight < nTestnetForkOne)))
      nSubsidy = 25 * COIN;

    /* Block reward gets halved every 1 million blocks */
    nSubsidy >>= (nHeight / 1000000);

    return(nSubsidy + nFees);
}

/* Quick money supply calculator for the given block height */
int64 GetMoneySupply(int nHeight) {
    int64 nMoneySupply = 0;

    if(!fTestNet) {

        /* Includes the genesis block which is unspendable */
        if(nHeight < nForkThree) return((nHeight + 1) * 50 * COIN);
        else nMoneySupply = nForkThree * 50 * COIN;

        if(nHeight < nForkFour) {
            return(nMoneySupply + (nHeight - nForkThree + 1) * 25 * COIN);
        } else {
            nMoneySupply += (nForkFour - nForkThree) * 25 * COIN;
        }

        if(nHeight < 1000000) {
            return(nMoneySupply + (nHeight - nForkFour + 1) * 50 * COIN);
        } else {
            nMoneySupply += (1000000 - nForkFour) * 50 * COIN;
        }

        if(nHeight < 2000000) {
            return(nMoneySupply + (nHeight - 1000000 + 1) * 25 * COIN);
        } else {
            nMoneySupply += (2000000 - 1000000) * 25 * COIN;
        }

        if(nHeight < 3000000) {
            return(nMoneySupply + (nHeight - 2000000 + 1) * 25 * COIN / 2);
        } else {
            nMoneySupply += (3000000 - 2000000) * 25 * COIN / 2;
        }

        if(nHeight < 4000000) {
            return(nMoneySupply + (nHeight - 3000000 + 1) * 25 * COIN / 4);
        } else {
            nMoneySupply += (4000000 - 3000000) * 25 * COIN / 4;
        }

    } else {

        if(nHeight < nTestnetForkOne) return(nHeight * 25 * COIN);
        else nMoneySupply = nTestnetForkOne * 25 * COIN;

        if(nHeight < 1000000) {
            return(nMoneySupply + (nHeight - nTestnetForkOne + 1) * 50 * COIN);
        } else {
            nMoneySupply += (1000000 - nTestnetForkOne) * 50 * COIN;
        }

        if(nHeight < 2000000) {
            return(nMoneySupply + (nHeight - 1000000 + 1) * 25 * COIN);
        } else {
            nMoneySupply += (2000000 - 1000000) * 25 * COIN;
        }

    }

    return(nMoneySupply);
}

uint static GetNextWorkRequired(const CBlockIndex *pindexLast, const CBlock *pblock) {
    uint nProofOfWorkLimit = bnProofOfWorkLimit.GetCompact();
    int i;

    /* The genesis block */
    if(!pindexLast) return(nProofOfWorkLimit);

    /* The next block */
    int nHeight = pindexLast->nHeight + 1;

    /* The initial settings */
    int nTargetSpacing  = nTargetSpacingZero;
    int nTargetTimespan = nTargetTimespanZero;

    /* The 1st hard fork */
    if(nHeight >= nForkOne) {
        nTargetSpacing  = nTargetSpacingOne;
        nTargetTimespan = nTargetTimespanOne;
    }

    /* The 2nd hard fork */
    if(nHeight >= nForkTwo) {
        nTargetSpacing  = nTargetSpacingTwo;
        nTargetTimespan = nTargetTimespanTwo;
    }

    /* The 3nd hard fork; testnet gets started here */
    if(nHeight >= nForkThree || fTestNet) {
        nTargetSpacing  = nTargetSpacingThree;
        nTargetTimespan = nTargetTimespanThree;
    }

    /* The 4th hard fork; testnet gets hard forked as well */
    if((nHeight >= nForkFour) || (fTestNet && (nHeight >= nTestnetForkOne))) {
        nTargetSpacing  = nTargetSpacingFour;
        nTargetTimespan = nTargetTimespanFour;
    }

    /* The 5th hard fork and 2nd testnet hard fork */
    if((nHeight >= nForkFive) || (fTestNet && (nHeight >= nTestnetForkTwo))) {
        if(!fNeoScrypt) fNeoScrypt = true;
        /* Difficulty reset after the switch */
        if(nHeight == nForkFive)
          return(bnNeoScryptSwitch.GetCompact());
    }

    /* 2400, 600, 108, 126 and 20 blocks respectively */
    int nInterval = nTargetTimespan / nTargetSpacing;

    /* Just in case a hard fork isn't aligned properly */
    bool fHardFork = false;
    if(fTestNet) {
        fHardFork = (nHeight == nTestnetForkOne) || (nHeight == nTestnetForkTwo);
    } else {
        fHardFork = (nHeight == nForkOne) || (nHeight == nForkTwo) ||
          (nHeight == nForkThree) || (nHeight == nForkFour) || (nHeight == nForkFive);
    }

    /* Difficulty rules for regular blocks */
    if((nHeight % nInterval != 0) && !fHardFork) {

        // Testnet has a special difficulty rule
        if(fTestNet) {
            // Reset the difficulty if the difference in time stamps between
            // this and the previous block is over 2x of nTargetSpacing
            if(pblock->nTime > pindexLast->nTime + nTargetSpacing*2)
              return nProofOfWorkLimit;
            else {
                // Return the difficulty of the last regular block
                // with no minimal difficulty set as above
                const CBlockIndex* pindex = pindexLast;
                while(pindex->pprev && (pindex->nHeight % nInterval != 0) && (pindex->nBits == nProofOfWorkLimit))
                  pindex = pindex->pprev;
                return pindex->nBits;
            }
        }

        return(pindexLast->nBits);
    }

    /* Basic 100 blocks averaging after the 4th livenet or 1st testnet hard fork */
    if((nHeight >= nForkFour) || (fTestNet && (nHeight >= nTestnetForkOne))) {
        nInterval *= 5;
        nTargetTimespan *= 5;
    }

    /* The 1st retarget after the genesis */
    if(nInterval >= nHeight) nInterval = nHeight - 1;

    /* Go back by nInterval */
    const CBlockIndex *pindexFirst = pindexLast;
    for(i = 0; pindexFirst && (i < nInterval); i++)
      pindexFirst = pindexFirst->pprev;

    int nActualTimespan = pindexLast->GetBlockTime() - pindexFirst->GetBlockTime();

    printf("RETARGET: nActualTimespan = %d before bounds\n", nActualTimespan);

    /* Extended 500 blocks averaging after the 4th livenet or 1st testnet hard fork */
    if((nHeight >= nForkFour) || (fTestNet && (nHeight >= nTestnetForkOne))) {
        nInterval *= 4;

        for(i = 0; pindexFirst && (i < nInterval); i++)
          pindexFirst = pindexFirst->pprev;

        int nActualTimespanExtended =
          (pindexLast->GetBlockTime() - pindexFirst->GetBlockTime())/5;

        /* Average between the basic and extended windows */
        int nActualTimespanAvg = (nActualTimespan + nActualTimespanExtended) / 2;

        /* Apply 0.1 damping */
        nActualTimespan = nActualTimespanAvg + 9 * nTargetTimespan;
        nActualTimespan /= 10;

        printf("RETARGET: nActualTimespanExtended = %d (%d), nActualTimeSpanAvg = %d, nActualTimespan (damped) = %d\n",
          nActualTimespanExtended, nActualTimespanExtended * 5, nActualTimespanAvg, nActualTimespan);
    }

    /* The initial settings (4.0 difficulty limiter) */
    int nActualTimespanMax = nTargetTimespan * 4;
    int nActualTimespanMin = nTargetTimespan / 4;

    /* The 1st hard fork (1.8 difficulty limiter) */
    if(nHeight >= nForkOne) {
        nActualTimespanMax = nTargetTimespan * 99 / 55;
        nActualTimespanMin = nTargetTimespan * 55 / 99;
    }

    /* The 3rd hard fork (1.09 difficulty limiter) */
    if(nHeight >= nForkThree) {
        nActualTimespanMax = nTargetTimespan * 109 / 100;
        nActualTimespanMin = nTargetTimespan * 100 / 109;
    }

    /* The 4th livenet or 1st testnet hard fork (1.02 difficulty limiter) */
    if((nHeight >= nForkFour) || (fTestNet && (nHeight >= nTestnetForkOne))) {
        nActualTimespanMax = nTargetTimespan * 102 / 100;
        nActualTimespanMin = nTargetTimespan * 100 / 102;
    }

    /* The 5th livenet or 2nd testnet hard fork (+2% to -5% difficulty limiter) */
    if((nHeight >= nForkFive) || (fTestNet && (nHeight >= nTestnetForkTwo))) {
        nActualTimespanMax = nTargetTimespan * 105 / 100;
    }

    if(nActualTimespan < nActualTimespanMin) nActualTimespan = nActualTimespanMin;
    if(nActualTimespan > nActualTimespanMax) nActualTimespan = nActualTimespanMax;

    printf("RETARGET: nActualTimespan = %d after bounds\n", nActualTimespan);
    printf("RETARGET: nTargetTimespan = %d, nTargetTimespan/nActualTimespan = %.4f\n",
      nTargetTimespan, (float) nTargetTimespan/nActualTimespan);

    CBigNum bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    bnNew *= nActualTimespan;
    bnNew /= nTargetTimespan;

    if(bnNew > bnProofOfWorkLimit) bnNew = bnProofOfWorkLimit;

    printf("GetNextWorkRequired RETARGET\n");
    printf("Before: %08x  %s\n", pindexLast->nBits, CBigNum().SetCompact(pindexLast->nBits).getuint256().ToString().c_str());
    printf("After:  %08x  %s\n", bnNew.GetCompact(), bnNew.getuint256().ToString().c_str());

    return(bnNew.GetCompact());
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits)
{
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);

    // Check range
    if (bnTarget <= 0 || bnTarget > bnProofOfWorkLimit)
        return error("CheckProofOfWork() : nBits below minimum work");

    // Check proof of work matches claimed amount
    if (hash > bnTarget.getuint256())
        return error("CheckProofOfWork() : hash doesn't match nBits");

    return true;
}

// Return maximum amount of blocks that other nodes claim to have
int GetNumBlocksOfPeers()
{
    return std::max(cPeerBlockCounts.median(), Checkpoints::GetTotalBlocksEstimate());
}

bool IsInitialBlockDownload()
{
    if (pindexBest == NULL || nBestHeight < Checkpoints::GetTotalBlocksEstimate())
        return true;
    static int64 nLastUpdate;
    static CBlockIndex* pindexLastBest;
    if (pindexBest != pindexLastBest)
    {
        pindexLastBest = pindexBest;
        nLastUpdate = GetTime();
    }
    return(((GetTime() - nLastUpdate) < 10) &&
      (pindexBest->GetBlockTime() < (GetTime() - 4 * 60 * 60)));
}

static void InvalidChainFound(CBlockIndex* pindexNew)
{
    if (pindexNew->bnChainWork > bnBestInvalidWork)
    {
        bnBestInvalidWork = pindexNew->bnChainWork;
        CTxDB().WriteBestInvalidWork(bnBestInvalidWork);
        uiInterface.NotifyBlocksChanged();
    }
    printf("InvalidChainFound: invalid block=%s  height=%d  work=%s  date=%s\n",
      pindexNew->GetBlockHash().ToString().substr(0,20).c_str(), pindexNew->nHeight,
      pindexNew->bnChainWork.ToString().c_str(),
      DateTimeStrFormat(pindexNew->GetBlockTime()).c_str());
    printf("InvalidChainFound:  current best=%s  height=%d  work=%s  date=%s\n",
      hashBestChain.ToString().substr(0,20).c_str(), nBestHeight, bnBestChainWork.ToString().c_str(),
      DateTimeStrFormat(pindexBest->GetBlockTime()).c_str());
    if (pindexBest && bnBestInvalidWork > bnBestChainWork + pindexBest->GetBlockWork() * 6)
        printf("InvalidChainFound: Warning: Displayed transactions may not be correct! You may need to upgrade, or other nodes may need to upgrade.\n");
}

void CBlock::UpdateTime(const CBlockIndex* pindexPrev)
{
    nTime = max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    // Updating time can change work required on testnet:
    if (fTestNet)
        nBits = GetNextWorkRequired(pindexPrev, this);
}











bool CTransaction::DisconnectInputs(CTxDB& txdb)
{
    // Relinquish previous transactions' spent pointers
    if (!IsCoinBase())
    {
        BOOST_FOREACH(const CTxIn& txin, vin)
        {
            COutPoint prevout = txin.prevout;

            // Get prev txindex from disk
            CTxIndex txindex;
            if (!txdb.ReadTxIndex(prevout.hash, txindex))
                return error("DisconnectInputs() : ReadTxIndex failed");

            if (prevout.n >= txindex.vSpent.size())
                return error("DisconnectInputs() : prevout.n out of range");

            // Mark outpoint as not spent
            txindex.vSpent[prevout.n].SetNull();

            // Write back
            if (!txdb.UpdateTxIndex(prevout.hash, txindex))
                return error("DisconnectInputs() : UpdateTxIndex failed");
        }
    }

    // Remove transaction from index
    // This can fail if a duplicate of this transaction was in a chain that got
    // reorganized away. This is only possible if this transaction was completely
    // spent, so erasing it would be a no-op anyway.
    txdb.EraseTxIndex(*this);

    return true;
}


bool CTransaction::FetchInputs(CTxDB& txdb, const map<uint256, CTxIndex>& mapTestPool,
                               bool fBlock, bool fMiner, MapPrevTx& inputsRet, bool& fInvalid)
{
    // FetchInputs can return false either because we just haven't seen some inputs
    // (in which case the transaction should be stored as an orphan)
    // or because the transaction is malformed (in which case the transaction should
    // be dropped).  If tx is definitely invalid, fInvalid will be set to true.
    fInvalid = false;

    if (IsCoinBase())
        return true; // Coinbase transactions have no inputs to fetch.

    for (unsigned int i = 0; i < vin.size(); i++)
    {
        COutPoint prevout = vin[i].prevout;
        if (inputsRet.count(prevout.hash))
            continue; // Got it already

        // Read txindex
        CTxIndex& txindex = inputsRet[prevout.hash].first;
        bool fFound = true;
        if ((fBlock || fMiner) && mapTestPool.count(prevout.hash))
        {
            // Get txindex from current proposed changes
            txindex = mapTestPool.find(prevout.hash)->second;
        }
        else
        {
            // Read txindex from txdb
            fFound = txdb.ReadTxIndex(prevout.hash, txindex);
        }
        if (!fFound && (fBlock || fMiner))
            return fMiner ? false : error("FetchInputs() : %s prev tx %s index entry not found", GetHash().ToString().substr(0,10).c_str(),  prevout.hash.ToString().substr(0,10).c_str());

        // Read txPrev
        CTransaction& txPrev = inputsRet[prevout.hash].second;
        if (!fFound || txindex.pos == CDiskTxPos(1,1,1))
        {
            // Get prev tx from single transactions in memory
            {
                LOCK(mempool.cs);
                if (!mempool.exists(prevout.hash))
                    return error("FetchInputs() : %s mempool Tx prev not found %s", GetHash().ToString().substr(0,10).c_str(),  prevout.hash.ToString().substr(0,10).c_str());
                txPrev = mempool.lookup(prevout.hash);
            }
            if (!fFound)
                txindex.vSpent.resize(txPrev.vout.size());
        }
        else
        {
            // Get prev tx from disk
            if (!txPrev.ReadFromDisk(txindex.pos))
                return error("FetchInputs() : %s ReadFromDisk prev tx %s failed", GetHash().ToString().substr(0,10).c_str(),  prevout.hash.ToString().substr(0,10).c_str());
        }
    }

    // Make sure all prevout.n indexes are valid:
    for (unsigned int i = 0; i < vin.size(); i++)
    {
        const COutPoint prevout = vin[i].prevout;
        assert(inputsRet.count(prevout.hash) != 0);
        const CTxIndex& txindex = inputsRet[prevout.hash].first;
        const CTransaction& txPrev = inputsRet[prevout.hash].second;
        if (prevout.n >= txPrev.vout.size() || prevout.n >= txindex.vSpent.size())
        {
            // Revisit this if/when transaction replacement is implemented and allows
            // adding inputs:
            fInvalid = true;
            return(DoS(100, error("FetchInputs() : %s prevout.n out of range %d " \
              "%" PRIszu " %" PRIszu " prev tx %s\n%s",
              GetHash().ToString().substr(0,10).c_str(), prevout.n, txPrev.vout.size(),
              txindex.vSpent.size(), prevout.hash.ToString().substr(0,10).c_str(),
              txPrev.ToString().c_str())));
        }
    }

    return true;
}

const CTxOut& CTransaction::GetOutputFor(const CTxIn& input, const MapPrevTx& inputs) const
{
    MapPrevTx::const_iterator mi = inputs.find(input.prevout.hash);
    if (mi == inputs.end())
        throw std::runtime_error("CTransaction::GetOutputFor() : prevout.hash not found");

    const CTransaction& txPrev = (mi->second).second;
    if (input.prevout.n >= txPrev.vout.size())
        throw std::runtime_error("CTransaction::GetOutputFor() : prevout.n out of range");

    return txPrev.vout[input.prevout.n];
}

int64 CTransaction::GetValueIn(const MapPrevTx& inputs) const
{
    if (IsCoinBase())
        return 0;

    int64 nResult = 0;
    for (unsigned int i = 0; i < vin.size(); i++)
    {
        nResult += GetOutputFor(vin[i], inputs).nValue;
    }
    return nResult;

}

unsigned int CTransaction::GetP2SHSigOpCount(const MapPrevTx& inputs) const
{
    if (IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < vin.size(); i++)
    {
        const CTxOut& prevout = GetOutputFor(vin[i], inputs);
        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(vin[i].scriptSig);
    }
    return nSigOps;
}

bool CTransaction::ConnectInputs(MapPrevTx inputs,
                                 map<uint256, CTxIndex>& mapTestPool, const CDiskTxPos& posThisTx,
                                 const CBlockIndex* pindexBlock, bool fBlock, bool fMiner, bool fStrictPayToScriptHash)
{
    // Take over previous transactions' spent pointers
    // fBlock is true when this is called from AcceptBlock when a new best-block is added to the blockchain
    // fMiner is true when called from the internal Phoenixcoin miner
    // ... both are false when called from CTransaction::AcceptToMemoryPool
    if (!IsCoinBase())
    {
        int64 nValueIn = 0;
        int64 nFees = 0;
        for (unsigned int i = 0; i < vin.size(); i++)
        {
            COutPoint prevout = vin[i].prevout;
            assert(inputs.count(prevout.hash) > 0);
            CTxIndex& txindex = inputs[prevout.hash].first;
            CTransaction& txPrev = inputs[prevout.hash].second;

            if((prevout.n >= txPrev.vout.size()) || (prevout.n >= txindex.vSpent.size())) {
                return(DoS(100,
                  error("ConnectInputs() : %s prevout.n out of range %d " \
                  "%" PRIszu " %" PRIszu " prev tx %s\n%s",
                  GetHash().ToString().substr(0,10).c_str(), prevout.n, txPrev.vout.size(),
                  txindex.vSpent.size(), prevout.hash.ToString().substr(0,10).c_str(),
                  txPrev.ToString().c_str())));
            }

            // If prev is coinbase, check that it's matured
            if (txPrev.IsCoinBase())
                for(const CBlockIndex *pindex = pindexBlock;
                  pindex && (pindexBlock->nHeight - pindex->nHeight < nBaseMaturity); pindex = pindex->pprev)
                    if (pindex->nBlockPos == txindex.pos.nBlockPos && pindex->nFile == txindex.pos.nFile)
                        return error("ConnectInputs() : tried to spend coinbase at depth %d", pindexBlock->nHeight - pindex->nHeight);

            // Check for negative or overflow input values
            nValueIn += txPrev.vout[prevout.n].nValue;
            if (!MoneyRange(txPrev.vout[prevout.n].nValue) || !MoneyRange(nValueIn))
                return DoS(100, error("ConnectInputs() : txin values out of range"));

        }
        // The first loop above does all the inexpensive checks.
        // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
        // Helps prevent CPU exhaustion attacks.
        for (unsigned int i = 0; i < vin.size(); i++)
        {
            COutPoint prevout = vin[i].prevout;
            assert(inputs.count(prevout.hash) > 0);
            CTxIndex& txindex = inputs[prevout.hash].first;
            CTransaction& txPrev = inputs[prevout.hash].second;

            // Check for conflicts (double-spend)
            // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
            // for an attacker to attempt to split the network.
            if (!txindex.vSpent[prevout.n].IsNull())
                return fMiner ? false : error("ConnectInputs() : %s prev tx already used at %s", GetHash().ToString().substr(0,10).c_str(), txindex.vSpent[prevout.n].ToString().c_str());

            // Skip ECDSA signature verification when connecting blocks (fBlock=true)
            // before the last blockchain checkpoint. This is safe because block merkle hashes are
            // still computed and checked, and any change will be caught at the next checkpoint.
            if (!(fBlock && (nBestHeight < Checkpoints::GetTotalBlocksEstimate())))
            {
                // Verify signature
                if (!VerifySignature(txPrev, *this, i, fStrictPayToScriptHash, 0))
                {
                    // only during transition phase for P2SH: do not invoke anti-DoS code for
                    // potentially old clients relaying bad P2SH transactions
                    if (fStrictPayToScriptHash && VerifySignature(txPrev, *this, i, false, 0))
                        return error("ConnectInputs() : %s P2SH VerifySignature failed", GetHash().ToString().substr(0,10).c_str());

                    return DoS(100,error("ConnectInputs() : %s VerifySignature failed", GetHash().ToString().substr(0,10).c_str()));
                }
            }

            // Mark outpoints as spent
            txindex.vSpent[prevout.n] = posThisTx;

            // Write back
            if (fBlock || fMiner)
            {
                mapTestPool[prevout.hash] = txindex;
            }
        }

        if (nValueIn < GetValueOut())
            return DoS(100, error("ConnectInputs() : %s value in < value out", GetHash().ToString().substr(0,10).c_str()));

        // Tally transaction fees
        int64 nTxFee = nValueIn - GetValueOut();
        if (nTxFee < 0)
            return DoS(100, error("ConnectInputs() : %s nTxFee < 0", GetHash().ToString().substr(0,10).c_str()));
        nFees += nTxFee;
        if (!MoneyRange(nFees))
            return DoS(100, error("ConnectInputs() : nFees out of range"));
    }

    return true;
}


bool CTransaction::ClientConnectInputs()
{
    if (IsCoinBase())
        return false;

    // Take over previous transactions' spent pointers
    {
        LOCK(mempool.cs);
        int64 nValueIn = 0;
        for (unsigned int i = 0; i < vin.size(); i++)
        {
            // Get prev tx from single transactions in memory
            COutPoint prevout = vin[i].prevout;
            if (!mempool.exists(prevout.hash))
                return false;
            CTransaction& txPrev = mempool.lookup(prevout.hash);

            if (prevout.n >= txPrev.vout.size())
                return false;

            // Verify signature
            if (!VerifySignature(txPrev, *this, i, true, 0))
                return error("ConnectInputs() : VerifySignature failed");

            ///// this is redundant with the mempool.mapNextTx stuff,
            ///// not sure which I want to get rid of
            ///// this has to go away now that posNext is gone
            // // Check for conflicts
            // if (!txPrev.vout[prevout.n].posNext.IsNull())
            //     return error("ConnectInputs() : prev tx already used");
            //
            // // Flag outpoints as used
            // txPrev.vout[prevout.n].posNext = posThisTx;

            nValueIn += txPrev.vout[prevout.n].nValue;

            if (!MoneyRange(txPrev.vout[prevout.n].nValue) || !MoneyRange(nValueIn))
                return error("ClientConnectInputs() : txin values out of range");
        }
        if (GetValueOut() > nValueIn)
            return false;
    }

    return true;
}




bool CBlock::DisconnectBlock(CTxDB& txdb, CBlockIndex* pindex)
{
    // Disconnect in reverse order
    for (int i = vtx.size()-1; i >= 0; i--)
        if (!vtx[i].DisconnectInputs(txdb))
            return false;

    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    if (pindex->pprev)
    {
        CDiskBlockIndex blockindexPrev(pindex->pprev);
        blockindexPrev.hashNext = 0;
        if (!txdb.WriteBlockIndex(blockindexPrev))
            return error("DisconnectBlock() : WriteBlockIndex failed");
    }

    return true;
}

bool CBlock::ConnectBlock(CTxDB& txdb, CBlockIndex* pindex, bool fJustCheck)
{
    // Check it again in case a previous version let a bad block in
    if (!CheckBlock(!fJustCheck, !fJustCheck))
        return false;

    // Do not allow blocks that contain transactions which 'overwrite' older transactions,
    // unless those are already completely spent.
    // If such overwrites are allowed, coinbases and transactions depending upon those
    // can be duplicated to remove the ability to spend the first instance -- even after
    // being sent to another address.
    // See BIP30 and http://r6.ca/blog/20120206T005236Z.html for more information.
    // This logic is not necessary for memory pool transactions, as AcceptToMemoryPool
    // already refuses previously-known transaction ids entirely.
    // This rule was originally applied all blocks whose timestamp was after March 15, 2012, 0:00 UTC.
    // Now that the whole chain is irreversibly beyond that time it is applied to all blocks except the
    // two in the chain that violate it. This prevents exploiting the issue against nodes in their
    // initial block download.
    bool fEnforceBIP30 = (!pindex->phashBlock) || // Enforce on CreateNewBlock invocations which don't have a hash.
                          !((pindex->nHeight==91842 && pindex->GetBlockHash() == uint256("0x00000000000a4d0a398161ffc163c503763b1f4360639393e0e4c8e300e0caec")) ||
                           (pindex->nHeight==91880 && pindex->GetBlockHash() == uint256("0x00000000000743f190a18c5577a3c2d2a1f610ae9601ac046a38084ccb7cd721")));

    // BIP16 didn't become active until Apr 1 2012
    int64 nBIP16SwitchTime = 1333238400;
    bool fStrictPayToScriptHash = (pindex->nTime >= nBIP16SwitchTime);

    //// issue here: it doesn't know the version
    unsigned int nTxPos;
    if (fJustCheck)
        // FetchInputs treats CDiskTxPos(1,1,1) as a special "refer to memorypool" indicator
        // Since we're just checking the block and not actually connecting it, it might not (and probably shouldn't) be on the disk to get the transaction from
        nTxPos = 1;
    else
        nTxPos = pindex->nBlockPos + ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) - 1 + GetSizeOfCompactSize(vtx.size());

    map<uint256, CTxIndex> mapQueuedChanges;
    int64 nFees = 0;
    unsigned int nSigOps = 0;
    BOOST_FOREACH(CTransaction& tx, vtx)
    {
        uint256 hashTx = tx.GetHash();

        if (fEnforceBIP30) {
            CTxIndex txindexOld;
            if (txdb.ReadTxIndex(hashTx, txindexOld)) {
                BOOST_FOREACH(CDiskTxPos &pos, txindexOld.vSpent)
                    if (pos.IsNull())
                        return false;
            }
        }

        nSigOps += tx.GetLegacySigOpCount();
        if (nSigOps > MAX_BLOCK_SIGOPS)
            return DoS(100, error("ConnectBlock() : too many sigops"));

        CDiskTxPos posThisTx(pindex->nFile, pindex->nBlockPos, nTxPos);
        if (!fJustCheck)
            nTxPos += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);

        MapPrevTx mapInputs;
        if (!tx.IsCoinBase())
        {
            bool fInvalid;
            if (!tx.FetchInputs(txdb, mapQueuedChanges, true, false, mapInputs, fInvalid))
                return false;

            if (fStrictPayToScriptHash)
            {
                // Add in sigops done by pay-to-script-hash inputs;
                // this is to prevent a "rogue miner" from creating
                // an incredibly-expensive-to-validate block.
                nSigOps += tx.GetP2SHSigOpCount(mapInputs);
                if (nSigOps > MAX_BLOCK_SIGOPS)
                    return DoS(100, error("ConnectBlock() : too many sigops"));
            }

            nFees += tx.GetValueIn(mapInputs)-tx.GetValueOut();

            if (!tx.ConnectInputs(mapInputs, mapQueuedChanges, posThisTx, pindex, true, false, fStrictPayToScriptHash))
                return false;
        }

        mapQueuedChanges[hashTx] = CTxIndex(posThisTx, tx.vout.size());
    }

    if(vtx[0].GetValueOut() > GetProofOfWorkReward(pindex->nHeight, nFees)) {
        return(error("ConnectBlock() : coin base pays too much (actual=%" PRI64d " vs limit=%" PRI64d ")",
          vtx[0].GetValueOut(), GetProofOfWorkReward(pindex->nHeight, nFees)));
    }

    if (fJustCheck)
        return true;

    // Write queued txindex changes
    for (map<uint256, CTxIndex>::iterator mi = mapQueuedChanges.begin(); mi != mapQueuedChanges.end(); ++mi)
    {
        if (!txdb.UpdateTxIndex((*mi).first, (*mi).second))
            return error("ConnectBlock() : UpdateTxIndex failed");
    }

    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    if (pindex->pprev)
    {
        CDiskBlockIndex blockindexPrev(pindex->pprev);
        blockindexPrev.hashNext = pindex->GetBlockHash();
        if (!txdb.WriteBlockIndex(blockindexPrev))
            return error("ConnectBlock() : WriteBlockIndex failed");
    }

    // Watch for transactions paying to me
    BOOST_FOREACH(CTransaction& tx, vtx)
        SyncWithWallets(tx, this, true);

    return true;
}

bool static Reorganize(CTxDB& txdb, CBlockIndex* pindexNew)
{
    printf("REORGANIZE\n");

    // Find the fork
    CBlockIndex* pfork = pindexBest;
    CBlockIndex* plonger = pindexNew;
    while (pfork != plonger)
    {
        while (plonger->nHeight > pfork->nHeight)
            if (!(plonger = plonger->pprev))
                return error("Reorganize() : plonger->pprev is null");
        if (pfork == plonger)
            break;
        if (!(pfork = pfork->pprev))
            return error("Reorganize() : pfork->pprev is null");
    }

    // List of what to disconnect
    vector<CBlockIndex*> vDisconnect;
    for (CBlockIndex* pindex = pindexBest; pindex != pfork; pindex = pindex->pprev)
        vDisconnect.push_back(pindex);

    // List of what to connect
    vector<CBlockIndex*> vConnect;
    for (CBlockIndex* pindex = pindexNew; pindex != pfork; pindex = pindex->pprev)
        vConnect.push_back(pindex);
    reverse(vConnect.begin(), vConnect.end());

    printf("REORGANIZE: Disconnect %" PRIszu " blocks; %s..%s\n",
      vDisconnect.size(), pfork->GetBlockHash().ToString().substr(0,20).c_str(),
      pindexBest->GetBlockHash().ToString().substr(0,20).c_str());
    printf("REORGANIZE: Connect %" PRIszu " blocks; %s..%s\n",
      vConnect.size(), pfork->GetBlockHash().ToString().substr(0,20).c_str(),
      pindexNew->GetBlockHash().ToString().substr(0,20).c_str());

    // Disconnect shorter branch
    vector<CTransaction> vResurrect;
    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
    {
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return error("Reorganize() : ReadFromDisk for disconnect failed");
        if (!block.DisconnectBlock(txdb, pindex))
            return error("Reorganize() : DisconnectBlock %s failed", pindex->GetBlockHash().ToString().substr(0,20).c_str());

        // Queue memory transactions to resurrect
        BOOST_FOREACH(const CTransaction& tx, block.vtx)
            if (!tx.IsCoinBase())
                vResurrect.push_back(tx);
    }

    // Connect longer branch
    vector<CTransaction> vDelete;
    for (unsigned int i = 0; i < vConnect.size(); i++)
    {
        CBlockIndex* pindex = vConnect[i];
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return error("Reorganize() : ReadFromDisk for connect failed");
        if (!block.ConnectBlock(txdb, pindex))
        {
            // Invalid block
            return error("Reorganize() : ConnectBlock %s failed", pindex->GetBlockHash().ToString().substr(0,20).c_str());
        }

        // Queue memory transactions to delete
        BOOST_FOREACH(const CTransaction& tx, block.vtx)
            vDelete.push_back(tx);
    }
    if (!txdb.WriteHashBestChain(pindexNew->GetBlockHash()))
        return error("Reorganize() : WriteHashBestChain failed");

    // Make sure it's successfully written to disk before changing memory structure
    if (!txdb.TxnCommit())
        return error("Reorganize() : TxnCommit failed");

    // Disconnect shorter branch
    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
        if (pindex->pprev)
            pindex->pprev->pnext = NULL;

    // Connect longer branch
    BOOST_FOREACH(CBlockIndex* pindex, vConnect)
        if (pindex->pprev)
            pindex->pprev->pnext = pindex;

    // Resurrect memory transactions that were in the disconnected branch
    BOOST_FOREACH(CTransaction& tx, vResurrect)
        tx.AcceptToMemoryPool(txdb, false);

    // Delete redundant memory transactions that are in the connected branch
    BOOST_FOREACH(CTransaction& tx, vDelete)
        mempool.remove(tx);

    printf("REORGANIZE: done\n");

    return true;
}


// Called from inside SetBestChain: attaches a block to the new best chain being built
bool CBlock::SetBestChainInner(CTxDB& txdb, CBlockIndex *pindexNew)
{
    uint256 hash = GetHash();

    // Adding to current best branch
    if (!ConnectBlock(txdb, pindexNew) || !txdb.WriteHashBestChain(hash))
    {
        txdb.TxnAbort();
        InvalidChainFound(pindexNew);
        return false;
    }
    if (!txdb.TxnCommit())
        return error("SetBestChain() : TxnCommit failed");

    // Add to current best branch
    pindexNew->pprev->pnext = pindexNew;

    // Delete redundant memory transactions
    BOOST_FOREACH(CTransaction& tx, vtx)
        mempool.remove(tx);

    return true;
}

bool CBlock::SetBestChain(CTxDB& txdb, CBlockIndex* pindexNew)
{
    uint256 hash = GetHash();

    if (!txdb.TxnBegin())
        return error("SetBestChain() : TxnBegin failed");

    if (pindexGenesisBlock == NULL && hash == hashGenesisBlock)
    {
        txdb.WriteHashBestChain(hash);
        if (!txdb.TxnCommit())
            return error("SetBestChain() : TxnCommit failed");
        pindexGenesisBlock = pindexNew;
    }
    else if (hashPrevBlock == hashBestChain)
    {
        if (!SetBestChainInner(txdb, pindexNew))
            return error("SetBestChain() : SetBestChainInner failed");
    }
    else
    {
        // the first block in the new chain that will cause it to become the new best chain
        CBlockIndex *pindexIntermediate = pindexNew;

        // list of blocks that need to be connected afterwards
        std::vector<CBlockIndex*> vpindexSecondary;

        // Reorganize is costly in terms of db load, as it works in a single db transaction.
        // Try to limit how much needs to be done inside
        while (pindexIntermediate->pprev && pindexIntermediate->pprev->bnChainWork > pindexBest->bnChainWork)
        {
            vpindexSecondary.push_back(pindexIntermediate);
            pindexIntermediate = pindexIntermediate->pprev;
        }

        if(!vpindexSecondary.empty())
          printf("Postponing %" PRIszu " reconnects\n", vpindexSecondary.size());

        // Switch to new best branch
        if (!Reorganize(txdb, pindexIntermediate))
        {
            txdb.TxnAbort();
            InvalidChainFound(pindexNew);
            return error("SetBestChain() : Reorganize failed");
        }

        // Connect further blocks
        BOOST_REVERSE_FOREACH(CBlockIndex *pindex, vpindexSecondary)
        {
            CBlock block;
            if (!block.ReadFromDisk(pindex))
            {
                printf("SetBestChain() : ReadFromDisk failed\n");
                break;
            }
            if (!txdb.TxnBegin()) {
                printf("SetBestChain() : TxnBegin 2 failed\n");
                break;
            }
            // errors now are not fatal, we still did a reorganisation to a new chain in a valid way
            if (!block.SetBestChainInner(txdb, pindex))
                break;
        }
    }

    // Update best block in wallet (so we can detect restored wallets)
    bool fIsInitialDownload = IsInitialBlockDownload();
    if (!fIsInitialDownload)
    {
        const CBlockLocator locator(pindexNew);
        ::SetBestChain(locator);
    }

    // New best block
    hashBestChain = hash;
    pindexBest = pindexNew;
    pblockindexFBBHLast = NULL;
    nBestHeight = pindexBest->nHeight;
    bnBestChainWork = pindexNew->bnChainWork;
    nTimeBestReceived = GetTime();
    nTransactionsUpdated++;
    printf("SetBestChain: new best=%s  height=%d  work=%s  date=%s\n",
      hashBestChain.ToString().substr(0,20).c_str(), nBestHeight,
      bnBestChainWork.ToString().c_str(),
      DateTimeStrFormat(pindexBest->GetBlockTime()).c_str());

#if (0)
    // Check the version of the last 100 blocks to see if we need to upgrade:
    if (!fIsInitialDownload)
    {
        int nUpgraded = 0;
        const CBlockIndex* pindex = pindexBest;
        for (int i = 0; i < 100 && pindex != NULL; i++)
        {
            if (pindex->nVersion > CBlock::CURRENT_VERSION)
                ++nUpgraded;
            pindex = pindex->pprev;
        }
        if (nUpgraded > 0)
            printf("SetBestChain: %d of last 100 blocks above version %d\n", nUpgraded, CBlock::CURRENT_VERSION);
        if (nUpgraded > 100/2)
            // strMiscWarning is read by GetWarnings(), called by Qt and the JSON-RPC code to warn the user:
            strMiscWarning = _("Warning: This version is obsolete, upgrade required!");
    }
#endif

    std::string strCmd = GetArg("-blocknotify", "");

    if (!fIsInitialDownload && !strCmd.empty())
    {
        boost::replace_all(strCmd, "%s", hashBestChain.GetHex());
        boost::thread t(runCommand, strCmd); // thread runs free
    }

    return true;
}


bool CBlock::AddToBlockIndex(unsigned int nFile, unsigned int nBlockPos)
{
    // Check for duplicate
    uint256 hash = GetHash();
    if (mapBlockIndex.count(hash))
        return error("AddToBlockIndex() : %s already exists", hash.ToString().substr(0,20).c_str());

    // Construct new block index object
    CBlockIndex* pindexNew = new CBlockIndex(nFile, nBlockPos, *this);
    if (!pindexNew)
        return error("AddToBlockIndex() : new CBlockIndex failed");
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);
    map<uint256, CBlockIndex*>::iterator miPrev = mapBlockIndex.find(hashPrevBlock);
    if (miPrev != mapBlockIndex.end())
    {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
    }
    pindexNew->bnChainWork = (pindexNew->pprev ? pindexNew->pprev->bnChainWork : 0) + pindexNew->GetBlockWork();

    CTxDB txdb;
    if (!txdb.TxnBegin())
        return false;
    txdb.WriteBlockIndex(CDiskBlockIndex(pindexNew));
    if (!txdb.TxnCommit())
        return false;

    // New best
    if (pindexNew->bnChainWork > bnBestChainWork)
        if (!SetBestChain(txdb, pindexNew))
            return false;

    txdb.Close();

    if (pindexNew == pindexBest)
    {
        // Notify UI to display prev block's coinbase if it was ours
        static uint256 hashPrevBestCoinBase;
        UpdatedTransaction(hashPrevBestCoinBase);
        hashPrevBestCoinBase = vtx[0].GetHash();
    }

    uiInterface.NotifyBlocksChanged();
    return true;
}




bool CBlock::CheckBlock(bool fCheckPOW, bool fCheckMerkleRoot) const
{
    // These are checks that are independent of context
    // that can be verified before saving an orphan block.

    // Size limits
    if (vtx.empty() || vtx.size() > MAX_BLOCK_SIZE || ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
        return DoS(100, error("CheckBlock() : size limits failed"));

    /* Verify proof-of-work */
    if(fCheckPOW && !CheckProofOfWork(GetHashPoW(), nBits))
      return(DoS(50, error("CheckBlock() : proof-of-work verification failed")));

    // Check timestamp
    if (GetBlockTime() > GetAdjustedTime() + 2 * 60 * 60)
        return error("CheckBlock() : block timestamp too far in the future");

    // First transaction must be coinbase, the rest must not be
    if (vtx.empty() || !vtx[0].IsCoinBase())
        return DoS(100, error("CheckBlock() : first tx is not coinbase"));
    for (unsigned int i = 1; i < vtx.size(); i++)
        if (vtx[i].IsCoinBase())
            return DoS(100, error("CheckBlock() : more than one coinbase"));

    // Check transactions
    BOOST_FOREACH(const CTransaction& tx, vtx)
        if (!tx.CheckTransaction())
            return DoS(tx.nDoS, error("CheckBlock() : CheckTransaction failed"));

    // Check for duplicate txids. This is caught by ConnectInputs(),
    // but catching it earlier avoids a potential DoS attack:
    set<uint256> uniqueTx;
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        uniqueTx.insert(tx.GetHash());
    }
    if (uniqueTx.size() != vtx.size())
        return DoS(100, error("CheckBlock() : duplicate transaction"));

    unsigned int nSigOps = 0;
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        nSigOps += tx.GetLegacySigOpCount();
    }
    if (nSigOps > MAX_BLOCK_SIGOPS)
        return DoS(100, error("CheckBlock() : out-of-bounds SigOpCount"));

    // Check merkle root
    if (fCheckMerkleRoot && hashMerkleRoot != BuildMerkleTree())
        return DoS(100, error("CheckBlock() : hashMerkleRoot mismatch"));

    return true;
}

bool CBlock::AcceptBlock()
{
    // Check for duplicate
    uint256 hash = GetHash();
    if (mapBlockIndex.count(hash))
        return error("AcceptBlock() : block already in mapBlockIndex");

    // Get prev block index
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashPrevBlock);
    if (mi == mapBlockIndex.end())
        return DoS(10, error("AcceptBlock() : prev block not found"));
    CBlockIndex* pindexPrev = (*mi).second;
    int nHeight = pindexPrev->nHeight+1;

    /* Don't accept v1 blocks after this point */
    if((fTestNet && (nTime > nTestnetSwitchV2)) || (!fTestNet && (nTime > nSwitchV2))) {
        CScript expect = CScript() << nHeight;
        if(!std::equal(expect.begin(), expect.end(), vtx[0].vin[0].scriptSig.begin()))
          return(DoS(100, error("AcceptBlock() : incorrect block height in coin base")));
    }

    /* Don't accept blocks with bogus nVersion numbers after this point */
    if((nHeight >= nForkFive) || (fTestNet && (nHeight >= nTestnetForkTwo))) {
        if(nVersion != 2)
          return(DoS(100, error("AcceptBlock() : incorrect block version")));
    }

    // Check proof of work
    if(nBits != GetNextWorkRequired(pindexPrev, this))
      return(DoS(100, error("AcceptBlock() : incorrect proof of work for block %d", nHeight)));

    uint nOurTime   = (uint)GetAdjustedTime();

    // Check for time stamp (past limit #1)
    if(nTime <= (uint)pindexPrev->GetMedianTimePast())
      return(DoS(20, error("AcceptBlock() : block %s height %d has a time stamp behind the median",
        hash.ToString().substr(0,20).c_str(), nHeight)));

    // Soft fork 1: further restrictions
    if((fTestNet && (nHeight >= nTestnetSoftForkOne)) || (nHeight >= nSoftForkOne)) {

        if(nTime > (nOurTime + 10 * 60))
          return(DoS(5, error("AcceptBlock() : block %s height %d has a time stamp too far in the future",
            hash.ToString().substr(0,20).c_str(), nHeight)));

        if(nTime <= (pindexPrev->GetMedianTimePast() + BLOCK_LIMITER_TIME))
          return(DoS(5, error("AcceptBlock() : block %s height %d rejected by the block limiter",
            hash.ToString().substr(0,20).c_str(), nHeight)));

        if(nTime <= (pindexPrev->GetBlockTime() - 10 * 60))
          return(DoS(20, error("AcceptBlock() : block %s height %d has a time stamp too far in the past",
            hash.ToString().substr(0,20).c_str(), nHeight)));

    }

    /* Soft fork 2 */
    if(!IsInitialBlockDownload() &&
      ((fTestNet && (nHeight >= nTestnetSoftForkTwo)) || (nHeight >= nSoftForkTwo))) {

        /* Check for time stamp (future limit) */
        if(nTime > (nOurTime + 5 * 60))
          return(DoS(5, error("AcceptBlock() [Soft Fork 2] : block %s height %d has a time stamp too far in the future",
            hash.ToString().substr(0,20).c_str(), nHeight)));

        /* Future travel detector for the block limiter */
        if((nTime > (nOurTime + 60)) &&
          ((pindexPrev->GetAverageTimePast(5, 45) + BLOCK_LIMITER_TIME) > nOurTime))
          return(DoS(5, error("AcceptBlock() : block %s height %d rejected by the future travel detector",
            hash.ToString().substr(0,20).c_str(), nHeight)));

    }

    /* Check that all transactions are final */
    BOOST_FOREACH(const CTransaction &tx, vtx) {
        if(!tx.IsFinal(nHeight, GetBlockTime())) {
            return(DoS(10, error("AcceptBlock() : contains a non-final transaction")));
        }
    }

    /* Check against hardcoded checkpoints */
    if(!Checkpoints::CheckHardened(nHeight, hash)) {
        return(DoS(100, error("AcceptBlock(): rejected by a hardened checkpoint at height %d", nHeight)));
    }

    /* Check against advanced (synchronised) checkpoints */
    if(!IsInitialBlockDownload()) {
        bool cpSatisfies = Checkpoints::CheckSync(hash, pindexPrev);

        /* Failed blocks are rejected in strict mode */
        if((CheckpointsMode == Checkpoints::STRICT) && !cpSatisfies) {
            return(error("AcceptBlock(): block %s height %d rejected by advanced checkpointing",
              hash.ToString().substr(0,20).c_str(), nHeight));
        }

        /* Failed blocks are accepted in advisory mode with a warning issued */
        if((CheckpointsMode == Checkpoints::ADVISORY) && !cpSatisfies) {
            strMiscWarning = _("WARNING: failed against advanced checkpointing!");
        }
    }

#if (0)
    // Reject block.nVersion=1 blocks when 95% (75% on testnet) of the network has upgraded:
    if (nVersion < 2)
    {
        if ((!fTestNet && CBlockIndex::IsSuperMajority(2, pindexPrev, 950, 1000)) ||
            (fTestNet && CBlockIndex::IsSuperMajority(2, pindexPrev, 75, 100)))
        {
            return error("AcceptBlock() : rejected nVersion=1 block");
        }
    }
    // Enforce block.nVersion=2 rule that the coinbase starts with serialized block height
    if (nVersion >= 2)
    {
        // if 750 of the last 1,000 blocks are version 2 or greater (51/100 if testnet):
        if ((!fTestNet && CBlockIndex::IsSuperMajority(2, pindexPrev, 750, 1000)) ||
            (fTestNet && CBlockIndex::IsSuperMajority(2, pindexPrev, 51, 100)))
        {
            CScript expect = CScript() << nHeight;
            if (!std::equal(expect.begin(), expect.end(), vtx[0].vin[0].scriptSig.begin()))
                return DoS(100, error("AcceptBlock() : block height mismatch in coinbase"));
        }
    }
#endif

    // Write block to history file
    if (!CheckDiskSpace(::GetSerializeSize(*this, SER_DISK, CLIENT_VERSION)))
        return error("AcceptBlock() : out of disk space");
    unsigned int nFile = -1;
    unsigned int nBlockPos = 0;
    if (!WriteToDisk(nFile, nBlockPos))
        return error("AcceptBlock() : WriteToDisk failed");
    if (!AddToBlockIndex(nFile, nBlockPos))
        return error("AcceptBlock() : AddToBlockIndex failed");

    /* Relay inventory, but don't relay old inventory during initial block download */
    int nBlockEstimate = Checkpoints::GetTotalBlocksEstimate();
    if(hashBestChain == hash) {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode *pnode, vNodes) {
            if(nBestHeight > (pnode->nStartingHeight !=
              -1 ? pnode->nStartingHeight - 2000 : nBlockEstimate)) {
                pnode->PushInventory(CInv(MSG_BLOCK, hash));
            }
        }
    }

    /* Process a pending sync checkpoint;
     * disabled during initial block download to accelerate processing speed */
    if(!IsInitialBlockDownload()) Checkpoints::AcceptPendingSyncCheckpoint();

    return(true);
}

bool CBlockIndex::IsSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned int nRequired, unsigned int nToCheck)
{
    unsigned int nFound = 0;
    for (unsigned int i = 0; i < nToCheck && nFound < nRequired && pstart != NULL; i++)
    {
        if (pstart->nVersion >= minVersion)
            ++nFound;
        pstart = pstart->pprev;
    }
    return (nFound >= nRequired);
}

bool ProcessBlock(CNode *pfrom, CBlock *pblock) {
    uint256 hash = pblock->GetHash();

    /* Duplicate block check */
    if(mapBlockIndex.count(hash)) {
        return error("ProcessBlock() : block %s height %d have already",
          hash.ToString().substr(0,20).c_str(), mapBlockIndex[hash]->nHeight);
    }
    if(mapOrphanBlocks.count(hash)) {
        return error("ProcessBlock() : orphan block %s have already",
          hash.ToString().substr(0,20).c_str());
    }

    /* Ask for a pending sync checkpoint, if any */
    if(pfrom && !IsInitialBlockDownload())
      Checkpoints::AskForPendingSyncCheckpoint(pfrom);

    /* Basic block integrity checks including PoW target */
    if(!pblock->CheckBlock())
      return(error("ProcessBlock() : CheckBlock() FAILED"));

    CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint(mapBlockIndex);
    if (pcheckpoint && pblock->hashPrevBlock != hashBestChain)
    {

        if(((int64)(pblock->nTime) - (int64)(pcheckpoint->nTime)) < 0) {
            if(pfrom) pfrom->Misbehaving(100);
            return(error("ProcessBlock() : block has a time stamp %u before the last checkpoint %u",
              pblock->nTime, pcheckpoint->nTime));
        }

    }

    /* Accept an orphan block as long as there is a node to request preceding blocks from */
    if(!mapBlockIndex.count(pblock->hashPrevBlock)) {
        printf("ProcessBlock: ORPHAN BLOCK, prev=%s\n", pblock->hashPrevBlock.ToString().substr(0,20).c_str());

        if(pfrom) {
            CBlock *pblock2 = new CBlock(*pblock);
            mapOrphanBlocks.insert(make_pair(hash, pblock2));
            mapOrphanBlocksByPrev.insert(make_pair(pblock2->hashPrevBlock, pblock2));

            pfrom->PushGetBlocks(pindexBest, GetOrphanRoot(pblock2));
            /* Ask directly just in case */
            if(!IsInitialBlockDownload())
              pfrom->AskFor(CInv(MSG_BLOCK, WantedByOrphan(pblock2)));

        }

        return(true);
    }

    // Store to disk
    if (!pblock->AcceptBlock())
        return error("ProcessBlock() : AcceptBlock FAILED");

    // Recursively process any orphan blocks that depended on this one
    vector<uint256> vWorkQueue;
    vWorkQueue.push_back(hash);
    for (unsigned int i = 0; i < vWorkQueue.size(); i++)
    {
        uint256 hashPrev = vWorkQueue[i];
        for (multimap<uint256, CBlock*>::iterator mi = mapOrphanBlocksByPrev.lower_bound(hashPrev);
             mi != mapOrphanBlocksByPrev.upper_bound(hashPrev);
             ++mi)
        {
            CBlock* pblockOrphan = (*mi).second;
            if (pblockOrphan->AcceptBlock())
                vWorkQueue.push_back(pblockOrphan->GetHash());
            mapOrphanBlocks.erase(pblockOrphan->GetHash());
            delete pblockOrphan;
        }
        mapOrphanBlocksByPrev.erase(hashPrev);
    }

    printf("ProcessBlock: ACCEPTED\n");

    /* Checkpoint master sends a new sync checkpoint
     * according to the depth specified by -checkpointdepth */
    if(pfrom && !CSyncCheckpoint::strMasterPrivKey.empty())
      Checkpoints::SendSyncCheckpoint(Checkpoints::AutoSelectSyncCheckpoint());

    return(true);
}








bool CheckDiskSpace(uint64 nAdditionalBytes) {
    uint64 nFreeBytesAvailable = boost::filesystem::space(GetDataDir()).available;

    // Check for nMinDiskSpace bytes (currently 50MB)
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
    {
        fShutdown = true;
        string strMessage = _("Warning: Disk space is low!");
        strMiscWarning = strMessage;
        printf("*** %s\n", strMessage.c_str());
        uiInterface.ThreadSafeMessageBox(strMessage, "Phoenixcoin",
          CClientUIInterface::OK | CClientUIInterface::ICON_EXCLAMATION | CClientUIInterface::MODAL);
        StartShutdown();
        return false;
    }
    return true;
}

static boost::filesystem::path BlockFilePath(unsigned int nFile) {
    string strBlockFn = strprintf("blk%04u.dat", nFile);
    return GetDataDir() / strBlockFn;
}

FILE* OpenBlockFile(unsigned int nFile, unsigned int nBlockPos, const char* pszMode)
{
    if ((nFile < 1) || (nFile == std::numeric_limits<uint32_t>::max()))
        return NULL;
    FILE* file = fopen(BlockFilePath(nFile).string().c_str(), pszMode);
    if (!file)
        return NULL;
    if (nBlockPos != 0 && !strchr(pszMode, 'a') && !strchr(pszMode, 'w'))
    {
        if (fseek(file, nBlockPos, SEEK_SET) != 0)
        {
            fclose(file);
            return NULL;
        }
    }
    return file;
}

static unsigned int nCurrentBlockFile = 1;

FILE* AppendBlockFile(unsigned int& nFileRet)
{
    nFileRet = 0;
    while(true) {
        FILE* file = OpenBlockFile(nCurrentBlockFile, 0, "ab");
        if (!file)
            return NULL;
        if (fseek(file, 0, SEEK_END) != 0)
            return NULL;
        // FAT32 file size max 4GB, fseek and ftell max 2GB, so we must stay under 2GB
        if(ftell(file) < (int)(0x7F000000 - MAX_SIZE)) {
            nFileRet = nCurrentBlockFile;
            return(file);
        }
        fclose(file);
        nCurrentBlockFile++;
    }
}

bool LoadBlockIndex(bool fAllowNew) {

    if(fTestNet) {
        pchMessageStart[0] = 0xFE;
        pchMessageStart[1] = 0xD0;
        pchMessageStart[2] = 0xD8;
        pchMessageStart[3] = 0xD4;
        hashGenesisBlock = uint256("0xecd47eee16536f7d03d64643cfc8c61b22093f8bf2c9358bf8b6f4dcb5f13192");
        nBaseMaturity = BASE_MATURITY_TESTNET;
    }

    //
    // Load block index
    //
    CTxDB txdb("cr");
    if (!txdb.LoadBlockIndex())
        return false;
    txdb.Close();

    //
    // Init with genesis block
    //
    if(mapBlockIndex.empty()) {

        if(!fAllowNew) return(false);

        CTransaction txNew;
        CBlock block;

        if(!fTestNet) {

            /* The Phoenixcoin genesis block:
             * CBlock(hash=be2f30f9e8db8f430056, PoW=00000e9c6e417d3d8068, ver=1, hashPrevBlock=00000000000000000000, hashMerkleRoot=ff2aa75842, nTime=1317972665, nBits=1e0ffff0, nNonce=2084931085, vtx=1)
             *  CTransaction(hash=ff2aa75842, ver=1, vin.size=1, vout.size=1, nLockTime=0)
             *    CTxIn(COutPoint(0000000000, -1), coinbase 04ffff001d010446552e532e204973205765696768696e672057696465204f7665726861756c206f662057697265746170204c617773202d204e592054696d6573202d204d617920382032303133)
             *    CTxOut(error)
             *  vMerkleTree: ff2aa75842 */

            const char *pszTimestamp = "U.S. Is Weighing Wide Overhaul of Wiretap Laws - NY Times - May 8 2013";
            txNew.vin.resize(1);
            txNew.vout.resize(1);
            txNew.vin[0].scriptSig = CScript() << 486604799 << CBigNum(4) << vector<uchar>((const uchar *) pszTimestamp, (const uchar *) pszTimestamp + strlen(pszTimestamp));
            txNew.vout[0].nValue = 50 * COIN;
            txNew.vout[0].scriptPubKey = CScript() << 0x00 << OP_CHECKSIG;
            block.vtx.push_back(txNew);
            block.hashPrevBlock = 0;
            block.hashMerkleRoot = block.BuildMerkleTree();
            block.nVersion = 1;
            block.nTime    = 1317972665;
            block.nBits    = 0x1e0ffff0;
            block.nNonce   = 2084931085;

        } else {

            /* The Phoenixcoin testnet genesis block:
             * CBlock(hash=ecd47eee16536f7d03d6, PoW=000004b4022863f9ecf0, ver=1, hashPrevBlock=00000000000000000000, hashMerkleRoot=9bf4ade403, nTime=1383768000, nBits=1e0ffff0, nNonce=1029893, vtx=1)
             *  CTransaction(hash=9bf4ade403, ver=1, vin.size=1, vout.size=1, nLockTime=0)
             *    CTxIn(COutPoint(0000000000, -1), coinbase 04ffff001d01044a57656220466f756e6465722044656e6f756e636573204e534120456e6372797074696f6e20437261636b696e67202d2054686520477561726469616e202d2030362f4e6f762f32303133)
             *    CTxOut(nValue=500.00000000, scriptPubKey=049023f10bccda76f971d6417d420c)
             *  vMerkleTree: 9bf4ade403 */

            const char *pszTimestamp = "Web Founder Denounces NSA Encryption Cracking - The Guardian - 06/Nov/2013";
            txNew.vin.resize(1);
            txNew.vout.resize(1);
            txNew.vin[0].scriptSig = CScript() << 486604799 << CBigNum(4) << vector<uchar>((const uchar *) pszTimestamp, (const uchar *) pszTimestamp + strlen(pszTimestamp));
            txNew.vout[0].nValue = 500 * COIN;
            txNew.vout[0].scriptPubKey = CScript() << ParseHex("049023F10BCCDA76F971D6417D420C6BB5735D3286669CE03B49C5FEA07078F0E07B19518EE1C0A4F81BCF56A5497AD7D8200CE470EEA8C6E2CF65F1EE503F0D3E") << OP_CHECKSIG;
            block.vtx.push_back(txNew);
            block.hashPrevBlock = 0;
            block.hashMerkleRoot = block.BuildMerkleTree();
            block.nVersion = 1;
            block.nTime    = 1383768000;
            block.nBits    = 0x1e0ffff0;
            block.nNonce   = 1029893;

        }

        printf("%s\n", block.GetHash().ToString().c_str());
        printf("%s\n", hashGenesisBlock.ToString().c_str());
        printf("%s\n", block.hashMerkleRoot.ToString().c_str());

        /* Must be set before mining the genesis block */
        if(!fTestNet) assert(block.hashMerkleRoot == uint256("0xff2aa75842fae1bfb100b656c57229ce37b03643434da2043ddab7a11cfe69a6"));
        else assert(block.hashMerkleRoot == uint256("0x9bf4ade403d775b44e872935609367aee5bd7df698e0f4c73e5f30f46b30a537"));

        /* Generate a new one if no match */
        if(false && ((fTestNet && (block.GetHash() != hashGenesisBlockTestNet)) ||
                    (!fTestNet && (block.GetHash() != hashGenesisBlock)))) {

            printf("Genesis block mining...\n");

            uint profile = fNeoScrypt ? 0x0 : 0x3;
            uint256 hashTarget = CBigNum().SetCompact(block.nBits).getuint256();
            uint256 hash;

            profile |= nNeoScryptOptions;

            while(true) {
                neoscrypt((uchar *) &block.nVersion, (uchar *) &hash, profile);
                if(hash <= hashTarget) break;
                if((block.nNonce & 0xFFF) == 0)
                  printf("nonce %08X: hash = %s (target = %s)\n",
                    block.nNonce, hash.ToString().c_str(), hashTarget.ToString().c_str());
                block.nNonce++;
                if(!block.nNonce) {
                    printf("Nonce limit reached, incrementing nTime\n");
                    block.nTime++;
                }
            }
            printf("block.nTime = %u\n", block.nTime);
            printf("block.nNonce = %u\n", block.nNonce);
            printf("block.GetHash = %s\n", block.GetHash().ToString().c_str());
            printf("block.GetHashPoW = %s\n", block.GetHashPoW().ToString().c_str());
        }

        block.print();

        if(!fTestNet) assert(block.GetHash() == hashGenesisBlock);
        else assert(block.GetHash() == hashGenesisBlockTestNet);

        /* Start the first block file */
        uint nFile;
        uint nBlockPos;
        if(!block.WriteToDisk(nFile, nBlockPos))
          return(error("LoadBlockIndex(): failed to write the genesis block to disk"));
        if(!block.AddToBlockIndex(nFile, nBlockPos))
          return(error("LoadBlockIndex(): failed to add the genesis block to the block index"));

        /* Initialise sync checkpointing */
        if(!Checkpoints::WriteSyncCheckpoint(hashGenesisBlock))
          return(error("LoadBlockIndex(): failed to initialise advanced checkpointing"));

    }

    /* Verify the master public key and reset sync checkpointing if changed */
    std::string strPubKey = "";
    std::string strMasterPubKey = fTestNet ? CSyncCheckpoint::strTestPubKey : CSyncCheckpoint::strMainPubKey;

#if 0
    if(!pblocktree->ReadCheckpointPubKey(strPubKey) || (strPubKey != strMasterPubKey)) {

        {
            LOCK(Checkpoints::cs_hashSyncCheckpoint);
            if(!pblocktree->WriteCheckpointPubKey(strMasterPubKey))
              return(error("LoadBlockIndex(): failed to write the new checkpoint master key to the data base"));
        }

        if(!Checkpoints::ResetSyncCheckpoint())
          return(error("LoadBlockIndex(): failed to reset advanced checkpointing"));
    }
#else
    CTxDB txdbs;
    if(!txdbs.ReadCheckpointPubKey(strPubKey) || (strPubKey != strMasterPubKey)) {
        txdbs.TxnBegin();
        if(!txdbs.WriteCheckpointPubKey(strMasterPubKey))
          return(error("LoadBlockIndex(): failed to write the new checkpoint master key to the data base"));
        if(!txdbs.TxnCommit())
          return(error("LoadBlockIndex(): failed to commit the new checkpoint master key to the data base"));

        if(!Checkpoints::ResetSyncCheckpoint())
          return(error("LoadBlockIndex(): failed to reset advanced checkpointing"));
    }
    txdbs.Close();
#endif

    return(true);
}



void PrintBlockTree()
{
    // pre-compute tree structure
    map<CBlockIndex*, vector<CBlockIndex*> > mapNext;
    for (map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi)
    {
        CBlockIndex* pindex = (*mi).second;
        mapNext[pindex->pprev].push_back(pindex);
        // test
        //while (rand() % 3 == 0)
        //    mapNext[pindex->pprev].push_back(pindex);
    }

    vector<pair<int, CBlockIndex*> > vStack;
    vStack.push_back(make_pair(0, pindexGenesisBlock));

    int nPrevCol = 0;
    while (!vStack.empty())
    {
        int nCol = vStack.back().first;
        CBlockIndex* pindex = vStack.back().second;
        vStack.pop_back();

        // print split or gap
        if (nCol > nPrevCol)
        {
            for (int i = 0; i < nCol-1; i++)
                printf("| ");
            printf("|\\\n");
        }
        else if (nCol < nPrevCol)
        {
            for (int i = 0; i < nCol; i++)
                printf("| ");
            printf("|\n");
       }
        nPrevCol = nCol;

        // print columns
        for (int i = 0; i < nCol; i++)
            printf("| ");

        // print item
        CBlock block;
        block.ReadFromDisk(pindex);
        printf("%d (%u,%u) %s  %s  tx %" PRIszu "",
          pindex->nHeight, pindex->nFile, pindex->nBlockPos,
          block.GetHash().ToString().substr(0,20).c_str(),
          DateTimeStrFormat(block.GetBlockTime()).c_str(),
          block.vtx.size());

        PrintWallets(block);

        // put the main time-chain first
        vector<CBlockIndex*>& vNext = mapNext[pindex];
        for (unsigned int i = 0; i < vNext.size(); i++)
        {
            if (vNext[i]->pnext)
            {
                swap(vNext[0], vNext[i]);
                break;
            }
        }

        // iterate children
        for (unsigned int i = 0; i < vNext.size(); i++)
            vStack.push_back(make_pair(nCol+i, vNext[i]));
    }
}

bool LoadExternalBlockFile(FILE* fileIn)
{
    int64 nStart = GetTimeMillis();

    int nLoaded = 0;
    {
        LOCK(cs_main);
        try {
            CAutoFile blkdat(fileIn, SER_DISK, CLIENT_VERSION);
            unsigned int nPos = 0;
            while (nPos != std::numeric_limits<uint32_t>::max() && blkdat.good() && !fRequestShutdown)
            {
                unsigned char pchData[65536];
                do {
                    fseek(blkdat, nPos, SEEK_SET);
                    int nRead = fread(pchData, 1, sizeof(pchData), blkdat);
                    if (nRead <= 8)
                    {
                        nPos = std::numeric_limits<uint32_t>::max();
                        break;
                    }
                    void* nFind = memchr(pchData, pchMessageStart[0], nRead+1-sizeof(pchMessageStart));
                    if (nFind)
                    {
                        if (memcmp(nFind, pchMessageStart, sizeof(pchMessageStart))==0)
                        {
                            nPos += ((unsigned char*)nFind - pchData) + sizeof(pchMessageStart);
                            break;
                        }
                        nPos += ((unsigned char*)nFind - pchData) + 1;
                    }
                    else
                        nPos += sizeof(pchData) - sizeof(pchMessageStart) + 1;
                } while(!fRequestShutdown);
                if (nPos == std::numeric_limits<uint32_t>::max())
                    break;
                fseek(blkdat, nPos, SEEK_SET);
                unsigned int nSize;
                blkdat >> nSize;
                if (nSize > 0 && nSize <= MAX_BLOCK_SIZE)
                {
                    CBlock block;
                    blkdat >> block;
                    if (ProcessBlock(NULL,&block))
                    {
                        nLoaded++;
                        nPos += 4 + nSize;
                    }
                }
            }
        }
        catch (std::exception &e) {
            printf("%s() : Deserialize or I/O error caught during load\n",
                   __PRETTY_FUNCTION__);
        }
    }
    printf("Loaded %i blocks from external file in %" PRI64d "ms\n",
      nLoaded, GetTimeMillis() - nStart);

    return(nLoaded > 0);
}









//////////////////////////////////////////////////////////////////////////////
//
// CAlert
//

extern map<uint256, CAlert> mapAlerts;
extern CCriticalSection cs_mapAlerts;

string GetWarnings(string strFor)
{
    int nPriority = 0;
    string strStatusBar;
    string strRPC;
    if (GetBoolArg("-testsafemode"))
        strRPC = "test";

    // Misc warnings like out of disk space and clock is wrong
    if (!strMiscWarning.empty())
    {
        nPriority = 1000;
        strStatusBar = strMiscWarning;
    }

    /* Don't enter safe mode if the last received sync checkpoint is too old;
     * display a warning in STRICT mode only */
    if((CheckpointsMode == Checkpoints::STRICT) &&
      Checkpoints::IsSyncCheckpointTooOld(60 * 60 * 24 * 10) &&
      !fTestNet && !IsInitialBlockDownload()) {
        nPriority = 100;
        strStatusBar = _("WARNING: Advanced checkpoint is too old. Please notify the developers.");
    }

    /* Enter safe mode if an invalid sync checkpoint has been detected */
    if(Checkpoints::hashInvalidCheckpoint != 0) {
        nPriority = 3000;
        strStatusBar = strRPC = _("WARNING: Inconsistent advanced checkpoint found! Please notify the developers.");
    }

    // Alerts
    {
        LOCK(cs_mapAlerts);
        BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
        {
            const CAlert& alert = item.second;
            if (alert.AppliesToMe() && alert.nPriority > nPriority)
            {
                nPriority = alert.nPriority;
                strStatusBar = alert.strStatusBar;
            }
        }
    }

    if (strFor == "statusbar")
        return strStatusBar;
    else if (strFor == "rpc")
        return strRPC;
    assert(!"GetWarnings() : invalid parameter");
    return "error";
}








//////////////////////////////////////////////////////////////////////////////
//
// Messages
//


bool static AlreadyHave(CTxDB& txdb, const CInv& inv)
{
    switch (inv.type)
    {
    case MSG_TX:
        {
        bool txInMap = false;
            {
            LOCK(mempool.cs);
            txInMap = (mempool.exists(inv.hash));
            }
        return txInMap ||
               mapOrphanTransactions.count(inv.hash) ||
               txdb.ContainsTx(inv.hash);
        }

    case MSG_BLOCK:
        return mapBlockIndex.count(inv.hash) ||
               mapOrphanBlocks.count(inv.hash);
    }
    // Don't know what it is, just say we already got one
    return true;
}


extern void AddTimeData(const CNetAddr& ip, int64 nTime);

bool static ProcessMessage(CNode* pfrom, string strCommand, CDataStream& vRecv)
{
    static map<CService, CPubKey> mapReuseKey;
    RandAddSeedPerfmon();

    if(fDebug)
      printf("received: %s (%" PRIszu " bytes)\n", strCommand.c_str(), vRecv.size());

    if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0)
    {
        printf("dropmessagestest DROPPING RECV MESSAGE\n");
        return true;
    }





    if (strCommand == "version")
    {
        /* Process the 1st version message received per connection
         * and ignore the others if any */
        if(pfrom->nVersion) return(true);

        int64 nTime;
        CAddress addrMe;
        CAddress addrFrom;
        uint64 nNonce = 1;
        vRecv >> pfrom->nVersion >> pfrom->nServices >> nTime >> addrMe;

        /* Do not connect to these peers as they are not compatible */
        if((pfrom->nVersion > MAX_PROTOCOL_VERSION) ||
          (pfrom->nVersion < MIN_PROTOCOL_VERSION)) {
            printf("peer %s reports incompatible version %i; disconnecting\n",
              pfrom->addr.ToString().c_str(), pfrom->nVersion);
            pfrom->fDisconnect = true;
            return(false);
        }

        if (!vRecv.empty())
            vRecv >> addrFrom >> nNonce;
        if (!vRecv.empty())
            vRecv >> pfrom->strSubVer;
        if (!vRecv.empty())
            vRecv >> pfrom->nStartingHeight;

        if (pfrom->fInbound && addrMe.IsRoutable())
        {
            pfrom->addrLocal = addrMe;
            SeenLocal(addrMe);
        }

        // Disconnect if we connected to ourself
        if (nNonce == nLocalHostNonce && nNonce > 1)
        {
            printf("connected to self at %s, disconnecting\n", pfrom->addr.ToString().c_str());
            pfrom->fDisconnect = true;
            return true;
        }

        /* Record my external IP as reported */
        if(addrFrom.IsRoutable() && addrMe.IsRoutable())
          addrSeenByPeer = addrMe;

        // Be shy and don't send version until we hear
        if (pfrom->fInbound)
            pfrom->PushVersion();

        pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);

        AddTimeData(pfrom->addr, nTime);

        // Change version
        pfrom->PushMessage("verack");
        pfrom->vSend.SetVersion(min(pfrom->nVersion, PROTOCOL_VERSION));

        if (!pfrom->fInbound)
        {
            // Advertise our address
            if (!fNoListen && !IsInitialBlockDownload())
            {
                CAddress addr = GetLocalAddress(&pfrom->addr);
                if (addr.IsRoutable())
                    pfrom->PushAddress(addr);
            }

            /* Ask for new peer addresses */
            if(pfrom->fOneShot || (addrman.size() < 1000)) {
                pfrom->PushMessage("getaddr");
                pfrom->fGetAddr = true;
            }
            addrman.Good(pfrom->addr);
        } else {
            if (((CNetAddr)pfrom->addr) == (CNetAddr)addrFrom)
            {
                addrman.Add(addrFrom, addrFrom);
                addrman.Good(addrFrom);
            }
        }

        /* Ask for inventory */
        if(!pfrom->fClient && !pfrom->fOneShot &&
          (pfrom->nStartingHeight > nBestHeight)) {
            pfrom->PushGetBlocks(pindexBest, uint256(0));
        }

        // Relay alerts
        {
            LOCK(cs_mapAlerts);
            BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
                item.second.RelayTo(pfrom);
        }

        /* Relay sync checkpoints */
        {
            LOCK(Checkpoints::cs_hashSyncCheckpoint);
            if(!Checkpoints::checkpointMessage.IsNull())
              Checkpoints::checkpointMessage.RelayTo(pfrom);
        }

        pfrom->fSuccessfullyConnected = true;

        printf("received version message from %s, version %d, blocks=%d, us=%s, them=%s\n",
          pfrom->addr.ToString().c_str(), pfrom->nVersion, pfrom->nStartingHeight,
          addrMe.ToString().c_str(), addrFrom.ToString().c_str());

        cPeerBlockCounts.input(pfrom->nStartingHeight);

        /* Check for a pending sync checkpoint, if any */
        if(!IsInitialBlockDownload()) Checkpoints::AskForPendingSyncCheckpoint(pfrom);
    }

    else if (pfrom->nVersion == 0)
    {
        // Must have a version message before anything else
        pfrom->Misbehaving(1);
        return false;
    }


    else if (strCommand == "verack")
    {
        pfrom->vRecv.SetVersion(min(pfrom->nVersion, PROTOCOL_VERSION));
    }


    else if (strCommand == "addr")
    {
        vector<CAddress> vAddr;
        vRecv >> vAddr;

        if(vAddr.size() > 1000) {
            pfrom->Misbehaving(20);
            return(error("message addr size() = %" PRIszu "", vAddr.size()));
        }

        // Store the new addresses
        vector<CAddress> vAddrOk;
        int64 nNow = GetAdjustedTime();
        int64 nSince = nNow - 10 * 60;
        BOOST_FOREACH(CAddress& addr, vAddr)
        {
            if (fShutdown)
                return true;
            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            pfrom->AddAddressKnown(addr);
            bool fReachable = IsReachable(addr);
            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable())
            {
                // Relay to a limited number of other nodes
                {
                    LOCK(cs_vNodes);
                    // Use deterministic randomness to send to the same nodes for 24 hours
                    // at a time so the setAddrKnowns of the chosen nodes prevent repeats
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint64 hashAddr = addr.GetHash();
                    uint256 hashRand = hashSalt ^ (hashAddr<<32) ^ ((GetTime()+hashAddr)/(24*60*60));
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    multimap<uint256, CNode*> mapMix;
                    BOOST_FOREACH(CNode* pnode, vNodes)
                    {
                        unsigned int nPointer;
                        memcpy(&nPointer, &pnode, sizeof(nPointer));
                        uint256 hashKey = hashRand ^ nPointer;
                        hashKey = Hash(BEGIN(hashKey), END(hashKey));
                        mapMix.insert(make_pair(hashKey, pnode));
                    }
                    int nRelayNodes = fReachable ? 2 : 1; // limited relaying of addresses outside our network(s)
                    for (multimap<uint256, CNode*>::iterator mi = mapMix.begin(); mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
                        ((*mi).second)->PushAddress(addr);
                }
            }
            // Do not store addresses outside our network
            if (fReachable)
                vAddrOk.push_back(addr);
        }
        addrman.Add(vAddrOk, pfrom->addr, 2 * 60 * 60);
        if (vAddr.size() < 1000)
            pfrom->fGetAddr = false;
        if (pfrom->fOneShot)
            pfrom->fDisconnect = true;
    }


    else if (strCommand == "inv")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if(vInv.size() > MAX_INV_SZ) {
            pfrom->Misbehaving(20);
            return(error("message inv size() = %" PRIszu "", vInv.size()));
        }

        // find last block in inv vector
        unsigned int nLastBlock = (unsigned int)(-1);
        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++) {
            if (vInv[vInv.size() - 1 - nInv].type == MSG_BLOCK) {
                nLastBlock = vInv.size() - 1 - nInv;
                break;
            }
        }
        CTxDB txdb("r");
        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++)
        {
            const CInv &inv = vInv[nInv];

            if (fShutdown)
                return true;
            pfrom->AddInventoryKnown(inv);

            bool fAlreadyHave = AlreadyHave(txdb, inv);
            if (fDebug)
                printf("  got inventory: %s  %s\n", inv.ToString().c_str(), fAlreadyHave ? "have" : "new");

            if (!fAlreadyHave)
                pfrom->AskFor(inv);
            else if (inv.type == MSG_BLOCK && mapOrphanBlocks.count(inv.hash)) {
                pfrom->PushGetBlocks(pindexBest, GetOrphanRoot(mapOrphanBlocks[inv.hash]));
            } else if (nInv == nLastBlock) {
                // In case we are on a very long side-chain, it is possible that we already have
                // the last block in an inv bundle sent in response to getblocks. Try to detect
                // this situation and push another getblocks to continue.
                pfrom->PushGetBlocks(mapBlockIndex[inv.hash], uint256(0));
                if (fDebug)
                    printf("force request: %s\n", inv.ToString().c_str());
            }

            // Track requests for our stuff
            Inventory(inv.hash);
        }
    }


    else if (strCommand == "getdata")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if(vInv.size() > MAX_INV_SZ) {
            pfrom->Misbehaving(20);
            return(error("message getdata size() = %" PRIszu "", vInv.size()));
        }

        if(fDebugNet || (vInv.size() != 1))
          printf("received getdata (%" PRIszu " invsz)\n", vInv.size());

        BOOST_FOREACH(const CInv& inv, vInv)
        {
            if (fShutdown)
                return true;
            if (fDebugNet || (vInv.size() == 1))
                printf("received getdata for: %s\n", inv.ToString().c_str());

            if (inv.type == MSG_BLOCK)
            {
                // Send block from disk
                map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(inv.hash);
                if (mi != mapBlockIndex.end())
                {
                    CBlock block;
                    block.ReadFromDisk((*mi).second);
                    pfrom->PushMessage("block", block);
                }
            }
            else if (inv.IsKnownType())
            {
                // Send stream from relay memory
                bool pushed = false;
                {
                    LOCK(cs_mapRelay);
                    map<CInv, CDataStream>::iterator mi = mapRelay.find(inv);
                    if (mi != mapRelay.end()) {
                        pfrom->PushMessage(inv.GetCommand(), (*mi).second);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_TX) {
                    LOCK(mempool.cs);
                    if (mempool.exists(inv.hash)) {
                        CTransaction tx = mempool.lookup(inv.hash);
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << tx;
                        pfrom->PushMessage("tx", ss);
                    }
                }
            }

            // Track requests for our stuff
            Inventory(inv.hash);
        }
    }


    else if (strCommand == "getblocks")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        /* Time limit for responding to a particular peer */
        uint nCurrentTime = (uint)GetTime();
        if((nCurrentTime - 5U) < pfrom->nGetblocksReceiveTime) {
            return(error("message getblocks spam"));
        } else {
            pfrom->nGetblocksReceiveTime = nCurrentTime;
        }

        /* Locate the best block of the caller that matches our chain;
         * it must be the genesis block if no match */
        CBlockIndex *pindex = locator.GetBlockIndex();

        /* Start with the next block if available */
        if(pindex->pnext) pindex = pindex->pnext;
        else return(true);

        if(hashStop != uint256(0)) {
            printf("getblocks height %d up to block %s received from peer %s\n",
              pindex->nHeight, hashStop.ToString().substr(0,20).c_str(),
              pfrom->addr.ToString().c_str());
        } else {
            printf("getblocks height %d received from peer %s\n",
              pindex->nHeight, pfrom->addr.ToString().c_str());
        }

        /* Send inventory on the rest of the chain up to the limit */
        uint nLimit = 1000;

        while(nLimit--) {

            if(pindex->GetBlockHash() == hashStop) {
                printf("getblocks stopping at height %d block %s for peer %s\n",
                  pindex->nHeight, pindex->GetBlockHash().ToString().substr(0,20).c_str(),
                  pfrom->addr.ToString().c_str());
                break;
            }

            pfrom->PushInventory(CInv(MSG_BLOCK, pindex->GetBlockHash()));

            if(pindex->pnext) pindex = pindex->pnext;
            else break;

        }

        if(!nLimit) {
            printf("getblocks height %d block %s stopping at limit for peer %s\n",
              pindex->nHeight, pindex->GetBlockHash().ToString().substr(0,20).c_str(),
              pfrom->addr.ToString().c_str());
        }

        /* Advertise our best block */
        if(((pindexBest->nHeight - pindex->nHeight) < 4000) &&
          (pindex->GetBlockHash() != hashBestChain)) {
            pfrom->PushInventory(CInv(MSG_BLOCK, hashBestChain));
            printf("getblocks advertised height %d block %s to peer %s\n",
              nBestHeight, hashBestChain.ToString().substr(0,20).c_str(),
              pfrom->addr.ToString().c_str());
        }
    }


    else if (strCommand == "getheaders")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        /* Time limit for responding to a particular peer */
        uint nCurrentTime = (uint)GetTime();
        if((nCurrentTime - 5U) < pfrom->nGetheadersReceiveTime) {
            return(error("message getheaders spam"));
        } else {
            pfrom->nGetheadersReceiveTime = nCurrentTime;
        }

        CBlockIndex* pindex = NULL;
        if (locator.IsNull())
        {
            // If locator is null, return the hashStop block
            map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashStop);
            if (mi == mapBlockIndex.end())
                return true;
            pindex = (*mi).second;
        }
        else
        {
            // Find the last block the caller has in the main chain
            pindex = locator.GetBlockIndex();
            if (pindex)
                pindex = pindex->pnext;
        }

        vector<CBlock> vHeaders;
        int nLimit = 4000;
        printf("getheaders %d to %s\n", (pindex ? pindex->nHeight : -1), hashStop.ToString().substr(0,20).c_str());
        for (; pindex; pindex = pindex->pnext)
        {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }
        pfrom->PushMessage("headers", vHeaders);
    }


    else if (strCommand == "tx")
    {
        vector<uint256> vWorkQueue;
        vector<uint256> vEraseQueue;
        CDataStream vMsg(vRecv);
        CTxDB txdb("r");
        CTransaction tx;
        vRecv >> tx;

        CInv inv(MSG_TX, tx.GetHash());
        pfrom->AddInventoryKnown(inv);

        // Truncate messages to the size of the tx inside
        unsigned int nSize = ::GetSerializeSize(tx,SER_NETWORK,PROTOCOL_VERSION);
        if (nSize < vMsg.size()) {
            vMsg.resize(nSize);
        }

        bool fMissingInputs = false;
        if (tx.AcceptToMemoryPool(txdb, true, &fMissingInputs))
        {
            SyncWithWallets(tx, NULL, true);
            RelayMessage(inv, vMsg);
            mapAlreadyAskedFor.erase(inv);
            vWorkQueue.push_back(inv.hash);
            vEraseQueue.push_back(inv.hash);

            // Recursively process any orphan transactions that depended on this one
            for (unsigned int i = 0; i < vWorkQueue.size(); i++)
            {
                uint256 hashPrev = vWorkQueue[i];
                for (map<uint256, CDataStream*>::iterator mi = mapOrphanTransactionsByPrev[hashPrev].begin();
                     mi != mapOrphanTransactionsByPrev[hashPrev].end();
                     ++mi)
                {
                    const CDataStream& vMsg = *((*mi).second);
                    CTransaction tx;
                    CDataStream(vMsg) >> tx;
                    CInv inv(MSG_TX, tx.GetHash());
                    bool fMissingInputs2 = false;

                    if (tx.AcceptToMemoryPool(txdb, true, &fMissingInputs2))
                    {
                        printf("   accepted orphan tx %s\n", inv.hash.ToString().substr(0,10).c_str());
                        SyncWithWallets(tx, NULL, true);
                        RelayMessage(inv, vMsg);
                        mapAlreadyAskedFor.erase(inv);
                        vWorkQueue.push_back(inv.hash);
                        vEraseQueue.push_back(inv.hash);
                    }
                    else if (!fMissingInputs2)
                    {
                        // invalid orphan
                        vEraseQueue.push_back(inv.hash);
                        printf("   removed invalid orphan tx %s\n", inv.hash.ToString().substr(0,10).c_str());
                    }
                }
            }

            BOOST_FOREACH(uint256 hash, vEraseQueue)
                EraseOrphanTx(hash);
        }
        else if (fMissingInputs)
        {
            AddOrphanTx(vMsg);

            // DoS prevention: do not allow mapOrphanTransactions to grow unbounded
            unsigned int nEvicted = LimitOrphanTxSize(MAX_ORPHAN_TRANSACTIONS);
            if (nEvicted > 0)
                printf("mapOrphan overflow, removed %u tx\n", nEvicted);
        }
        if (tx.nDoS) pfrom->Misbehaving(tx.nDoS);
    }


    else if (strCommand == "block")
    {
        CBlock block;
        vRecv >> block;
        uint256 hashBlock = block.GetHash();
        int nBlockHeight = block.GetBlockHeight();

        if(nBlockHeight > (nBestHeight + 5000)) {
            /* Discard this block because cannot verify it any time soon */
            printf("received and discarded a distant block %s height %d\n",
              hashBlock.ToString().substr(0,20).c_str(), nBlockHeight);
        } else {
            printf("received block %s height %d\n",
              hashBlock.ToString().substr(0,20).c_str(), nBlockHeight);

            CInv inv(MSG_BLOCK, hashBlock);
            pfrom->AddInventoryKnown(inv);

            if(ProcessBlock(pfrom, &block))
              mapAlreadyAskedFor.erase(inv);

            if(block.nDoS)
              pfrom->Misbehaving(block.nDoS);
        }
    }


    else if (strCommand == "getaddr")
    {
        pfrom->vAddrToSend.clear();
        vector<CAddress> vAddr = addrman.GetAddr();
        BOOST_FOREACH(const CAddress &addr, vAddr)
            pfrom->PushAddress(addr);
    }


    else if (strCommand == "mempool")
    {
        std::vector<uint256> vtxid;
        mempool.queryHashes(vtxid);
        vector<CInv> vInv;
        for (unsigned int i = 0; i < vtxid.size(); i++) {
            CInv inv(MSG_TX, vtxid[i]);
            vInv.push_back(inv);
            if (i == (MAX_INV_SZ - 1))
                    break;
        }
        if (vInv.size() > 0)
            pfrom->PushMessage("inv", vInv);
    }


    else if(strCommand == "ping") {
        int64 nonce;
        vRecv >> nonce;
        pfrom->PushMessage("pong", nonce);
        if(fDebug) printf("pong sent to peer %s nonce %" PRI64d "\n",
          pfrom->addr.ToString().c_str(), nonce);
    }


    else if(strCommand == "pong") {
        int64 nonce;
        vRecv >> nonce;
        if(pfrom->nPingStamp == nonce) {
            pfrom->nPongStamp = nonce;
            pfrom->nPingTime = (uint)((GetTimeMicros() - nonce) / 1000);
            if(fDebug) printf("pong received from peer %s time %u ms\n",
              pfrom->addr.ToString().c_str(), pfrom->nPingTime);
        } else {
            if(fDebug) printf("invalid pong received from peer %s nonce %" PRI64d "\n",
              pfrom->addr.ToString().c_str(), nonce);
        }
    }


    else if (strCommand == "alert")
    {
        CAlert alert;
        vRecv >> alert;

        uint256 alertHash = alert.GetHash();
        if (pfrom->setKnown.count(alertHash) == 0)
        {
            if (alert.ProcessAlert())
            {
                // Relay
                pfrom->setKnown.insert(alertHash);
                {
                    LOCK(cs_vNodes);
                    BOOST_FOREACH(CNode* pnode, vNodes)
                        alert.RelayTo(pnode);
                }
            }
            else {
                // Small DoS penalty so peers that send us lots of
                // duplicate/expired/invalid-signature/whatever alerts
                // eventually get banned.
                // This isn't a Misbehaving(100) (immediate ban) because the
                // peer might be an older or different implementation with
                // a different signature key, etc.
                pfrom->Misbehaving(10);
            }
        }
    }


    /* Sync checkpoint */
    else if(strCommand == "checkpoint") {

        if(pfrom->fDisconnect) {
            printf("advanced checkpoint received from a disconnected peer %s of version %i; ignoring\n",
              pfrom->addr.ToString().c_str(), pfrom->nVersion);
            return(false);
        }

        CSyncCheckpoint checkpoint;
        vRecv >> checkpoint;

        if(checkpoint.ProcessSyncCheckpoint(pfrom)) {
            /* Relay to connected nodes */
            pfrom->hashCheckpointKnown = checkpoint.hashCheckpoint;
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode *pnode, vNodes) checkpoint.RelayTo(pnode);
        }
    }


    else
    {
        // Ignore unknown commands for extensibility
    }


    // Update the last seen time for this node's address
    if (pfrom->fNetworkNode)
        if (strCommand == "version" || strCommand == "addr" || strCommand == "inv" || strCommand == "getdata" || strCommand == "ping")
            AddressCurrentlyConnected(pfrom->addr);


    return true;
}

bool ProcessMessages(CNode *pfrom) {

    CDataStream &vRecv = pfrom->vRecv;
    if(vRecv.empty()) return(true);

//    if(fDebug) printf("ProcessMessages(%u bytes)\n", vRecv.size());

    /* Message format
     * (4 bytes) message start aka network magic number
     * (12 bytes) command, null terminated
     * (4 bytes) message size
     * (4 bytes) message checksum
     * (X bytes) message data */

    while(true) {

        // Don't bother if send buffer is too full to respond anyway
        if(pfrom->vSend.size() >= SendBufferSize()) break;

        // Scan for message start
        CDataStream::iterator pstart = search(vRecv.begin(), vRecv.end(),
          BEGIN(pchMessageStart), END(pchMessageStart));
        int nHeaderSize = vRecv.GetSerializeSize(CMessageHeader());
        if((vRecv.end() - pstart) < nHeaderSize) {
            if((int)vRecv.size() > nHeaderSize) {
                if(fDebug) printf("ProcessMessages(): message start not found\n");
                vRecv.erase(vRecv.begin(), vRecv.end() - nHeaderSize);
            }
            break;
        }
        if((pstart - vRecv.begin()) > 0) {
            if(fDebug) printf("ProcessMessages(): %" PRIpdd " bytes skipped\n",
              pstart - vRecv.begin());
        }
        vRecv.erase(vRecv.begin(), pstart);

        // Read header
        vector<char> vHeaderSave(vRecv.begin(), vRecv.begin() + nHeaderSize);
        CMessageHeader hdr;
        vRecv >> hdr;
        if(!hdr.IsCommandValid()) {
            if(fDebug) {
                /* Dump the invalid command as a hex sequence */
                printf("ProcessMessages(): invalid command ");
                uint i;
                for(i = 0; i < CMessageHeader::COMMAND_SIZE; i++) {
                    printf("%02X", hdr.pchCommand[i]);
                }
                printf("\n");
            }
            continue;
        }

        // Message size
        uint nMessageSize = hdr.nMessageSize;
        if(nMessageSize > MAX_SIZE) {
            if(fDebug) printf("ProcessMessages(%s): very large message %u bytes\n",
              hdr.pchCommand, nMessageSize);
            continue;
        }
        if(nMessageSize > vRecv.size()) {
            // Rewind and wait for rest of message
            vRecv.insert(vRecv.begin(), vHeaderSave.begin(), vHeaderSave.end());
            break;
        }

        // Checksum
        uint256 hash = Hash(vRecv.begin(), vRecv.begin() + nMessageSize);
        uint nChecksum;
        memcpy(&nChecksum, &hash, CMessageHeader::CHECKSUM_SIZE);
        if(nChecksum != hdr.nChecksum) {
            if(fDebug) printf("ProcessMessages(%s): checksum mismatch %08X %08X\n",
              hdr.pchCommand, nChecksum, hdr.nChecksum);
            continue;
        }

        // Copy message to its own buffer
        CDataStream vMsg(vRecv.begin(), vRecv.begin() + nMessageSize, vRecv.nType, vRecv.nVersion);
        vRecv.ignore(nMessageSize);

        // Process message
        bool fRet = false;
        try {
            {
                LOCK(cs_main);
                fRet = ProcessMessage(pfrom, string(hdr.pchCommand), vMsg);
            }
            if(fShutdown) return(true);
        } catch(std::ios_base::failure &e) {
            if(strstr(e.what(), "end of data")) {
                printf("ProcessMessages(%s, %u bytes): exception '%s' caught, "
                  "normally caused by an undersized message\n",
                  hdr.pchCommand, nMessageSize, e.what());
            }
            else if(strstr(e.what(), "size too large")) {
                printf("ProcessMessages(%s, %u bytes): exception '%s' caught, "
                  "normally caused by an oversized message\n",
                  hdr.pchCommand, nMessageSize, e.what());
            }
            else {
                PrintExceptionContinue(&e, "ProcessMessages()");
            }
        } catch(std::exception &e) {
            PrintExceptionContinue(&e, "ProcessMessages()");
        } catch (...) {
            PrintExceptionContinue(NULL, "ProcessMessages()");
        }

        if(!fRet)
          printf("ProcessMessages(%s, %u bytes) FAILED\n", hdr.pchCommand, nMessageSize);
    }

    vRecv.Compact();
    return(true);
}


/* Time stamp of the last getblocks polling request */
uint nGetblocksTimePolling = 0;

bool SendMessages(CNode *pto, bool fSendTrickle) {

    /* Valid version must be received first */
    if(pto->nVersion < 1) return(true);

    TRY_LOCK(cs_main, lockMain);
    if(lockMain) {
        int64 nCurrentTime = GetTimeMicros();

        /* Keep-alive ping using an arbitrary nonce which is an extended time stamp */
        if(((nCurrentTime - pto->nPingStamp) > 60 * 1000000) && pto->vSend.empty()) {
            pto->nPingStamp = nCurrentTime;
            pto->PushMessage("ping", nCurrentTime);
            if(fDebugNet) printf("ping sent to peer %s nonce %" PRI64d "\n",
              pto->addr.ToString().c_str(), nCurrentTime);
        }

        /* Disconnect if the peer hasn't responded to pings recently */
        if(pto->nPingTime && ((pto->nPingStamp - pto->nPongStamp) > 5 * 60 * 1000000)) {
            pto->fDisconnect = true;
            printf("disconnecting peer %s due to pings timed out\n",
              pto->addr.ToString().c_str());
            return(true);
        }

        /* Convert to seconds */
        nCurrentTime /= 1000000;

        /* Ask a random peer for inventory */
        if(fSendTrickle && IsInitialBlockDownload() &&
          (nBestHeight < pto->nStartingHeight) &&
          ((nCurrentTime - nTimeBestReceived) > 1LL) &&
          ((nCurrentTime - nGetblocksTimePolling) > 1LL)) {
            nGetblocksTimePolling = nCurrentTime;
            pto->PushGetBlocks(pindexBest, uint256(0));
        }

        // Resend wallet transactions that haven't gotten in a block yet
        ResendWalletTransactions();

        // Address refresh broadcast
        static int64 nLastRebroadcast;
        if(!IsInitialBlockDownload() && ((nCurrentTime - nLastRebroadcast) > 24 * 60 * 60)) {

            {
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                {
                    // Periodically clear setAddrKnown to allow refresh broadcasts
                    if (nLastRebroadcast)
                        pnode->setAddrKnown.clear();

                    // Rebroadcast our address
                    if (!fNoListen)
                    {
                        CAddress addr = GetLocalAddress(&pnode->addr);
                        if (addr.IsRoutable())
                            pnode->PushAddress(addr);
                    }
                }
            }
            nLastRebroadcast = GetTime();
        }

        //
        // Message: addr
        //
        if (fSendTrickle)
        {
            vector<CAddress> vAddr;
            vAddr.reserve(pto->vAddrToSend.size());
            BOOST_FOREACH(const CAddress& addr, pto->vAddrToSend)
            {
                // returns true if wasn't already contained in the set
                if (pto->setAddrKnown.insert(addr).second)
                {
                    vAddr.push_back(addr);
                    // receiver rejects addr messages larger than 1000
                    if (vAddr.size() >= 1000)
                    {
                        pto->PushMessage("addr", vAddr);
                        vAddr.clear();
                    }
                }
            }
            pto->vAddrToSend.clear();
            if (!vAddr.empty())
                pto->PushMessage("addr", vAddr);
        }


        //
        // Message: inventory
        //
        vector<CInv> vInv;
        vector<CInv> vInvWait;
        {
            LOCK(pto->cs_inventory);
            vInv.reserve(pto->vInventoryToSend.size());
            vInvWait.reserve(pto->vInventoryToSend.size());
            BOOST_FOREACH(const CInv& inv, pto->vInventoryToSend)
            {
                if (pto->setInventoryKnown.count(inv))
                    continue;

                // trickle out tx inv to protect privacy
                if (inv.type == MSG_TX && !fSendTrickle)
                {
                    // 1/4 of tx invs blast to all immediately
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint256 hashRand = inv.hash ^ hashSalt;
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    bool fTrickleWait = ((hashRand & 3) != 0);

                    // always trickle our own transactions
                    if (!fTrickleWait)
                    {
                        CWalletTx wtx;
                        if (GetTransaction(inv.hash, wtx))
                            if (wtx.fFromMe)
                                fTrickleWait = true;
                    }

                    if (fTrickleWait)
                    {
                        vInvWait.push_back(inv);
                        continue;
                    }
                }

                // returns true if wasn't already contained in the set
                if (pto->setInventoryKnown.insert(inv).second)
                {
                    vInv.push_back(inv);
                    if (vInv.size() >= 1000)
                    {
                        pto->PushMessage("inv", vInv);
                        vInv.clear();
                    }
                }
            }
            pto->vInventoryToSend = vInvWait;
        }
        if (!vInv.empty())
            pto->PushMessage("inv", vInv);


        //
        // Message: getdata
        //
        vector<CInv> vGetData;
        int64 nNow = GetTime() * 1000000;
        CTxDB txdb("r");
        while (!pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow)
        {
            const CInv& inv = (*pto->mapAskFor.begin()).second;
            if (!AlreadyHave(txdb, inv))
            {
                if (fDebugNet)
                    printf("sending getdata: %s\n", inv.ToString().c_str());
                vGetData.push_back(inv);
                if (vGetData.size() >= 1000)
                {
                    pto->PushMessage("getdata", vGetData);
                    vGetData.clear();
                }
                mapAlreadyAskedFor[inv] = nNow;
            }
            pto->mapAskFor.erase(pto->mapAskFor.begin());
        }
        if (!vGetData.empty())
            pto->PushMessage("getdata", vGetData);

    }
    return true;
}














//////////////////////////////////////////////////////////////////////////////
//
// CoinMiner
//

// Some explaining would be appreciated
class COrphan
{
public:
    CTransaction* ptx;
    set<uint256> setDependsOn;
    double dPriority;
    double dFeePerKb;

    COrphan(CTransaction* ptxIn)
    {
        ptx = ptxIn;
        dPriority = dFeePerKb = 0;
    }

    void print() const
    {
        printf("COrphan(hash=%s, dPriority=%.1f, dFeePerKb=%.1f)\n",
               ptx->GetHash().ToString().substr(0,10).c_str(), dPriority, dFeePerKb);
        BOOST_FOREACH(uint256 hash, setDependsOn)
            printf("   setDependsOn %s\n", hash.ToString().substr(0,10).c_str());
    }
};


uint64 nLastBlockTx = 0;
uint64 nLastBlockSize = 0;

// We want to sort transactions by priority and fee, so:
typedef boost::tuple<double, double, CTransaction*> TxPriority;
class TxPriorityCompare
{
    bool byFee;
public:
    TxPriorityCompare(bool _byFee) : byFee(_byFee) { }
    bool operator()(const TxPriority& a, const TxPriority& b)
    {
        if (byFee)
        {
            if (a.get<1>() == b.get<1>())
                return a.get<0>() < b.get<0>();
            return a.get<1>() < b.get<1>();
        }
        else
        {
            if (a.get<0>() == b.get<0>())
                return a.get<1>() < b.get<1>();
            return a.get<0>() < b.get<0>();
        }
    }
};

/* Creates a new block and collects transactions into */
CBlock *CreateNewBlock(CReserveKey &reservekey) {

    CBlock *pblock = new CBlock();
    if(!pblock) return(NULL);

    // Create coinbase tx
    CTransaction txNew;
    txNew.vin.resize(1);
    txNew.vin[0].prevout.SetNull();
    txNew.vout.resize(1);
    txNew.vout[0].scriptPubKey << reservekey.GetReservedKey() << OP_CHECKSIG;

    // Add our coinbase tx as first transaction
    pblock->vtx.push_back(txNew);

    // Largest block you're willing to create:
    unsigned int nBlockMaxSize = GetArg("-blockmaxsize", MAX_BLOCK_SIZE_GEN/2);
    // Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SIZE-1000), nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", 27000);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = GetArg("-blockminsize", 0);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    // Fee-per-kilobyte amount considered the same as "free"
    // Be careful setting this: if you set it to zero then
    // a transaction spammer can cheaply fill blocks using
    // 1-satoshi-fee transactions. It should be set above the real
    // cost to you of processing a transaction.
    int64 nMinTxFee = MIN_TX_FEE;
    if (mapArgs.count("-mintxfee"))
        ParseMoney(mapArgs["-mintxfee"], nMinTxFee);

    // Collect memory pool transactions into the block
    int64 nFees = 0;
    {
        LOCK2(cs_main, mempool.cs);
        CBlockIndex* pindexPrev = pindexBest;
        CTxDB txdb("r");

        // Priority order to process transactions
        list<COrphan> vOrphan; // list memory doesn't move
        map<uint256, vector<COrphan*> > mapDependers;

        // This vector will be sorted into a priority queue:
        vector<TxPriority> vecPriority;
        vecPriority.reserve(mempool.mapTx.size());
        for (map<uint256, CTransaction>::iterator mi = mempool.mapTx.begin(); mi != mempool.mapTx.end(); ++mi)
        {
            CTransaction& tx = (*mi).second;
            if (tx.IsCoinBase() || !tx.IsFinal())
                continue;

            COrphan* porphan = NULL;
            double dPriority = 0;
            int64 nTotalIn = 0;
            bool fMissingInputs = false;
            BOOST_FOREACH(const CTxIn& txin, tx.vin)
            {
                // Read prev transaction
                CTransaction txPrev;
                CTxIndex txindex;
                if (!txPrev.ReadFromDisk(txdb, txin.prevout, txindex))
                {
                    // This should never happen; all transactions in the memory
                    // pool should connect to either transactions in the chain
                    // or other transactions in the memory pool.
                    if (!mempool.mapTx.count(txin.prevout.hash))
                    {
                        printf("ERROR: mempool transaction missing input\n");
                        if (fDebug) assert("mempool transaction missing input" == 0);
                        fMissingInputs = true;
                        if (porphan)
                            vOrphan.pop_back();
                        break;
                    }

                    // Has to wait for dependencies
                    if (!porphan)
                    {
                        // Use list for automatic deletion
                        vOrphan.push_back(COrphan(&tx));
                        porphan = &vOrphan.back();
                    }
                    mapDependers[txin.prevout.hash].push_back(porphan);
                    porphan->setDependsOn.insert(txin.prevout.hash);
                    nTotalIn += mempool.mapTx[txin.prevout.hash].vout[txin.prevout.n].nValue;
                    continue;
                }
                int64 nValueIn = txPrev.vout[txin.prevout.n].nValue;
                nTotalIn += nValueIn;

                int nConf = txindex.GetDepthInMainChain();
                dPriority += (double)nValueIn * nConf;
            }
            if (fMissingInputs) continue;

            // Priority is sum(valuein * age) / txsize
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            dPriority /= nTxSize;

            // This is a more accurate fee-per-kilobyte than is used by the client code, because the
            // client code rounds up the size to the nearest 1K. That's good, because it gives an
            // incentive to create smaller transactions.
            double dFeePerKb =  double(nTotalIn-tx.GetValueOut()) / (double(nTxSize)/1000.0);

            if (porphan)
            {
                porphan->dPriority = dPriority;
                porphan->dFeePerKb = dFeePerKb;
            }
            else
                vecPriority.push_back(TxPriority(dPriority, dFeePerKb, &(*mi).second));
        }

        // Collect transactions into block
        map<uint256, CTxIndex> mapTestPool;
        uint64 nBlockSize = 1000;
        uint64 nBlockTx = 0;
        int nBlockSigOps = 100;
        bool fSortedByFee = (nBlockPrioritySize <= 0);

        TxPriorityCompare comparer(fSortedByFee);
        std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

        while (!vecPriority.empty())
        {
            // Take highest priority transaction off the priority queue:
            double dPriority = vecPriority.front().get<0>();
            double dFeePerKb = vecPriority.front().get<1>();
            CTransaction& tx = *(vecPriority.front().get<2>());

            std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
            vecPriority.pop_back();

            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            if (nBlockSize + nTxSize >= nBlockMaxSize)
                continue;

            // Legacy limits on sigOps:
            unsigned int nTxSigOps = tx.GetLegacySigOpCount();
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            // Skip free transactions if we're past the minimum block size:
            if (fSortedByFee && (dFeePerKb < nMinTxFee) && (nBlockSize + nTxSize >= nBlockMinSize))
                continue;

            // Prioritize by fee once past the priority size or we run out of high-priority
            // transactions:
            if (!fSortedByFee &&
                ((nBlockSize + nTxSize >= nBlockPrioritySize) || (dPriority < COIN * 144 / 250)))
            {
                fSortedByFee = true;
                comparer = TxPriorityCompare(fSortedByFee);
                std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
            }

            // Connecting shouldn't fail due to dependency on other memory pool transactions
            // because we're already processing them in order of dependency
            map<uint256, CTxIndex> mapTestPoolTmp(mapTestPool);
            MapPrevTx mapInputs;
            bool fInvalid;
            if (!tx.FetchInputs(txdb, mapTestPoolTmp, false, true, mapInputs, fInvalid))
                continue;

            int64 nTxFees = tx.GetValueIn(mapInputs)-tx.GetValueOut();

            nTxSigOps += tx.GetP2SHSigOpCount(mapInputs);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            if (!tx.ConnectInputs(mapInputs, mapTestPoolTmp, CDiskTxPos(1,1,1), pindexPrev, false, true))
                continue;
            mapTestPoolTmp[tx.GetHash()] = CTxIndex(CDiskTxPos(1,1,1), tx.vout.size());
            swap(mapTestPool, mapTestPoolTmp);

            // Added
            pblock->vtx.push_back(tx);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            nFees += nTxFees;

            if (fDebug && GetBoolArg("-printpriority"))
            {
                printf("priority %.1f feeperkb %.1f txid %s\n",
                       dPriority, dFeePerKb, tx.GetHash().ToString().c_str());
            }

            // Add transactions that depend on this one to the priority queue
            uint256 hash = tx.GetHash();
            if (mapDependers.count(hash))
            {
                BOOST_FOREACH(COrphan* porphan, mapDependers[hash])
                {
                    if (!porphan->setDependsOn.empty())
                    {
                        porphan->setDependsOn.erase(hash);
                        if (porphan->setDependsOn.empty())
                        {
                            vecPriority.push_back(TxPriority(porphan->dPriority, porphan->dFeePerKb, porphan->ptx));
                            std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
                        }
                    }
                }
            }
        }

        nLastBlockTx = nBlockTx;
        nLastBlockSize = nBlockSize;
        printf("CreateNewBlock(): total size %" PRI64u "\n", nBlockSize);

    pblock->vtx[0].vout[0].nValue = GetProofOfWorkReward(pindexPrev->nHeight + 1, nFees);

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    pblock->UpdateTime(pindexPrev);
    pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock);
    pblock->nNonce         = 0;

        pblock->vtx[0].vin[0].scriptSig = CScript() << OP_0 << OP_0;
        CBlockIndex indexDummy(1, 1, *pblock);
        indexDummy.pprev = pindexPrev;
        indexDummy.nHeight = pindexPrev->nHeight + 1;
        if (!pblock->ConnectBlock(txdb, &indexDummy, true))
            throw std::runtime_error("CreateNewBlock() : ConnectBlock failed");
    }

    return(pblock);
}


void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    pblock->vtx[0].vin[0].scriptSig = (CScript() << nHeight << CBigNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(pblock->vtx[0].vin[0].scriptSig.size() <= 100);

    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
}


/* Prepares a block header for transmission using RPC getwork */
void FormatDataBuffer(CBlock *pblock, uint *pdata) {
    uint i;

    struct {
        int nVersion;
        uint256 hashPrevBlock;
        uint256 hashMerkleRoot;
        uint nTime;
        uint nBits;
        uint nNonce;
    } data;

    data.nVersion       = pblock->nVersion;
    data.hashPrevBlock  = pblock->hashPrevBlock;
    data.hashMerkleRoot = pblock->hashMerkleRoot;
    data.nTime          = pblock->nTime;
    data.nBits          = pblock->nBits;
    data.nNonce         = pblock->nNonce;

    if(fNeoScrypt) {
        /* Copy the LE data */
        for(i = 0; i < 20; i++)
          pdata[i] = ((uint *) &data)[i];
    } else {
        /* Block header size in bits */
        pdata[31] = 640;
        /* Convert LE to BE and copy */
        for(i = 0; i < 20; i++)
          pdata[i] = ByteReverse(((uint *) &data)[i]);
        /* Erase the remaining part */
        for(i = 20; i < 31; i++)
          pdata[i] = 0;
    }
}


bool CheckWork(CBlock *pblock, CWallet &wallet, CReserveKey &reservekey, bool fGetWork) {
    uint256 hash = pblock->GetHashPoW();
    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

    if(hash > hashTarget) return(false);

    printf("%sproof-of-work found\n   hash: %s\n target: %s\n",
      fGetWork ? "GW " : "",
      hash.GetHex().c_str(), hashTarget.GetHex().c_str());
    pblock->print();
    printf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue).c_str());

    // Found a solution
    {
        LOCK(cs_main);
        if(pblock->hashPrevBlock != hashBestChain)
          return(error("CoinMiner : generated block is stale"));

        // Remove key from key pool
        reservekey.KeepKey();

        // Track how many getdata requests this block gets
        {
            LOCK(wallet.cs_wallet);
            wallet.mapRequestCount[pblock->GetHash()] = 0;
        }

        // Process this block the same as if we had received it from another node
        if(!ProcessBlock(NULL, pblock))
          return(error("CoinMiner : ProcessBlock, block not accepted"));
    }

    return(true);
}

static void ThreadCoinMiner(void *parg);

static bool fGenerateCoins = false;
static bool fLimitProcessors = false;
static int nLimitProcessors = -1;

static void CoinMiner(CWallet *pwallet) {

    printf("CoinMiner started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);

    // Make this thread recognisable as the mining thread
    RenameThread("pxc-miner");

    // Each thread has its own key and counter
    CReserveKey reservekey(pwallet);
    unsigned int nExtraNonce = 0;

    while(fGenerateCoins) {

        if (fShutdown)
            return;
        while (vNodes.empty() || IsInitialBlockDownload())
        {
            Sleep(1000);
            if (fShutdown)
                return;
            if(!fGenerateCoins) return;
        }


        //
        // Create new block
        //
        unsigned int nTransactionsUpdatedLast = nTransactionsUpdated;
        CBlockIndex* pindexPrev = pindexBest;

        CBlock *pblock = CreateNewBlock(reservekey);

        if(!pblock) return;

        IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

        printf("Running CoinMiner with %u transactions in block (%u bytes)\n",
          (uint)pblock->vtx.size(),
          ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION));

        //
        // Search
        //
        int64 nStart = GetTime();
        uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

        while(true) {
            unsigned int nHashesDone = 0;
            uint profile = fNeoScrypt ? 0x0 : 0x3;
            uint256 hash;

            profile |= nNeoScryptOptions;

            while(true) {
                neoscrypt((uchar *) &pblock->nVersion, (uchar *) &hash, profile);
                if(hash <= hashTarget) {
                    // Found a solution
                    SetThreadPriority(THREAD_PRIORITY_NORMAL);
                    CheckWork(pblock, *pwalletMain, reservekey, false);
                    SetThreadPriority(THREAD_PRIORITY_LOWEST);
                    break;
                }
                pblock->nNonce += 1;
                nHashesDone += 1;
                if(!(pblock->nNonce & 0xFF)) break;
            }

            // Meter hashes/sec
            static int64 nHashCounter;
            if (nHPSTimerStart == 0)
            {
                nHPSTimerStart = GetTimeMillis();
                nHashCounter = 0;
            }
            else
                nHashCounter += nHashesDone;
            if (GetTimeMillis() - nHPSTimerStart > 4000)
            {
                static CCriticalSection cs;
                {
                    LOCK(cs);
                    if (GetTimeMillis() - nHPSTimerStart > 4000)
                    {
                        dHashesPerSec = 1000.0 * nHashCounter / (GetTimeMillis() - nHPSTimerStart);
                        nHPSTimerStart = GetTimeMillis();
                        nHashCounter = 0;
                        static int64 nLogTime;
                        if (GetTime() - nLogTime > 30 * 60)
                        {
                            nLogTime = GetTime();
                            printf("hashmeter %3d CPUs %6.0f KH/s\n",
                              vnThreadsRunning[THREAD_MINER], dHashesPerSec/1000.0);
                        }
                    }
                }
            }

            if((fLimitProcessors && (vnThreadsRunning[THREAD_MINER] > nLimitProcessors)) ||
              !fGenerateCoins || fShutdown) {
                delete(pblock);
                return;
            }

            if(pblock->nNonce >= 0xFFFF0000)
              break;
            if((nTransactionsUpdated != nTransactionsUpdatedLast) && (GetTime() - nStart > 60))
              break;
            if(pindexPrev != pindexBest)
              break;
            if(vNodes.empty())
              break;

            pblock->UpdateTime(pindexPrev);

            if(fTestNet) {
                /* UpdateTime() can change work required on testnet */
                hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
            }
        }
    }
}

static void ThreadCoinMiner(void *parg) {

    CWallet *pwallet = (CWallet *) parg;
    try {
        vnThreadsRunning[THREAD_MINER]++;
        CoinMiner(pwallet);
        vnThreadsRunning[THREAD_MINER]--;
    } catch(std::exception& e) {
        vnThreadsRunning[THREAD_MINER]--;
        PrintException(&e, "ThreadCoinMiner()");
    } catch (...) {
        vnThreadsRunning[THREAD_MINER]--;
        PrintException(NULL, "ThreadCoinMiner()");
    }
    nHPSTimerStart = 0;
    if(vnThreadsRunning[THREAD_MINER] == 0)
      dHashesPerSec = 0;
    printf("ThreadCoinMiner exiting, %d threads remaining\n",
      vnThreadsRunning[THREAD_MINER]);
}


void GenerateCoins(bool fGenerate, CWallet *pwallet) {

    fGenerateCoins = fGenerate;
    nLimitProcessors = GetArg("-genproclimit", -1);
    if(nLimitProcessors == 0)
      fGenerateCoins = false;
    fLimitProcessors = (nLimitProcessors != -1);

    if (fGenerate)
    {
        int nProcessors = boost::thread::hardware_concurrency();
        printf("%d processors\n", nProcessors);
        if (nProcessors < 1)
            nProcessors = 1;
        if (fLimitProcessors && nProcessors > nLimitProcessors)
            nProcessors = nLimitProcessors;
        int nAddThreads = nProcessors - vnThreadsRunning[THREAD_MINER];
        printf("Starting %d CoinMiner threads\n", nAddThreads);
        for (int i = 0; i < nAddThreads; i++)
        {
            if(!NewThread(ThreadCoinMiner, pwallet))
              printf("Error: NewThread(ThreadCoinMiner) failed\n");
            Sleep(10);
        }
    }
}
