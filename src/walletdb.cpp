// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software licence, see the accompanying
// file LICENCE or http://opensource.org/license/mit

#include <fstream>
#include <algorithm>
#include <map>

#include <boost/algorithm/string.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/variant/get.hpp>
#include <boost/version.hpp>

#include "base58.h"
#include "walletdb.h"
#include "wallet.h"

using namespace std;
using namespace boost;

static uint64 nAccountingEntryNumber = 0;

//
// CWalletDB
//

bool CWalletDB::WriteName(const string& strAddress, const string& strName)
{
    nWalletDBUpdated++;
    return Write(make_pair(string("name"), strAddress), strName);
}

bool CWalletDB::EraseName(const string& strAddress)
{
    // This should only be used for sending addresses, never for receiving addresses,
    // receiving addresses must always have an address book entry if they're not change return.
    nWalletDBUpdated++;
    return Erase(make_pair(string("name"), strAddress));
}

bool CWalletDB::ReadAccount(const string& strAccount, CAccount& account)
{
    account.SetNull();
    return Read(make_pair(string("acc"), strAccount), account);
}

bool CWalletDB::WriteAccount(const string& strAccount, const CAccount& account)
{
    return Write(make_pair(string("acc"), strAccount), account);
}

bool CWalletDB::WriteAccountingEntry(const uint64 nAccEntryNum, const CAccountingEntry& acentry)
{
    return Write(boost::make_tuple(string("acentry"), acentry.strAccount, nAccEntryNum), acentry);
}

bool CWalletDB::WriteAccountingEntry(const CAccountingEntry& acentry)
{
    return WriteAccountingEntry(++nAccountingEntryNumber, acentry);
}

int64 CWalletDB::GetAccountCreditDebit(const string& strAccount)
{
    list<CAccountingEntry> entries;
    ListAccountCreditDebit(strAccount, entries);

    int64 nCreditDebit = 0;
    BOOST_FOREACH (const CAccountingEntry& entry, entries)
        nCreditDebit += entry.nCreditDebit;

    return nCreditDebit;
}

void CWalletDB::ListAccountCreditDebit(const string& strAccount, list<CAccountingEntry>& entries)
{
    bool fAllAccounts = (strAccount == "*");

    Dbc* pcursor = GetCursor();
    if (!pcursor)
        throw runtime_error("CWalletDB::ListAccountCreditDebit() : cannot create DB cursor");
    unsigned int fFlags = DB_SET_RANGE;
    while(true) {
        // Read next record
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        if (fFlags == DB_SET_RANGE)
            ssKey << boost::make_tuple(string("acentry"), (fAllAccounts? string("") : strAccount), uint64(0));
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        int ret = ReadAtCursor(pcursor, ssKey, ssValue, fFlags);
        fFlags = DB_NEXT;
        if (ret == DB_NOTFOUND)
            break;
        else if (ret != 0)
        {
            pcursor->close();
            throw runtime_error("CWalletDB::ListAccountCreditDebit() : error scanning DB");
        }

        // Unserialize
        string strType;
        ssKey >> strType;
        if (strType != "acentry")
            break;
        CAccountingEntry acentry;
        ssKey >> acentry.strAccount;
        if (!fAllAccounts && acentry.strAccount != strAccount)
            break;

        ssValue >> acentry;
        ssKey >> acentry.nEntryNo;
        entries.push_back(acentry);
    }

    pcursor->close();
}


DBErrors
CWalletDB::ReorderTransactions(CWallet* pwallet)
{
    LOCK(pwallet->cs_wallet);
    // Old wallets didn't have any defined order for transactions
    // Probably a bad idea to change the output of this

    // First: get all CWalletTx and CAccountingEntry into a sorted-by-time multimap.
    typedef pair<CWalletTx*, CAccountingEntry*> TxPair;
    typedef multimap<int64, TxPair > TxItems;
    TxItems txByTime;

    for (map<uint256, CWalletTx>::iterator it = pwallet->mapWallet.begin(); it != pwallet->mapWallet.end(); ++it)
    {
        CWalletTx* wtx = &((*it).second);
        txByTime.insert(make_pair(wtx->nTimeReceived, TxPair(wtx, (CAccountingEntry*)0)));
    }
    list<CAccountingEntry> acentries;
    ListAccountCreditDebit("", acentries);
    BOOST_FOREACH(CAccountingEntry& entry, acentries)
    {
        txByTime.insert(make_pair(entry.nTime, TxPair((CWalletTx*)0, &entry)));
    }

    int64& nOrderPosNext = pwallet->nOrderPosNext;
    nOrderPosNext = 0;
    std::vector<int64> nOrderPosOffsets;
    for (TxItems::iterator it = txByTime.begin(); it != txByTime.end(); ++it)
    {
        CWalletTx *const pwtx = (*it).second.first;
        CAccountingEntry *const pacentry = (*it).second.second;
        int64& nOrderPos = (pwtx != 0) ? pwtx->nOrderPos : pacentry->nOrderPos;

        if (nOrderPos == -1)
        {
            nOrderPos = nOrderPosNext++;
            nOrderPosOffsets.push_back(nOrderPos);

            if (pacentry)
                // Have to write accounting regardless, since we don't keep it in memory
                if (!WriteAccountingEntry(pacentry->nEntryNo, *pacentry))
                    return DB_LOAD_FAIL;
        }
        else
        {
            int64 nOrderPosOff = 0;
            BOOST_FOREACH(const int64& nOffsetStart, nOrderPosOffsets)
            {
                if (nOrderPos >= nOffsetStart)
                    ++nOrderPosOff;
            }
            nOrderPos += nOrderPosOff;
            nOrderPosNext = std::max(nOrderPosNext, nOrderPos + 1);

            if (!nOrderPosOff)
                continue;

            // Since we're changing the order, write it back
            if (pwtx)
            {
                if (!WriteTx(pwtx->GetHash(), *pwtx))
                    return DB_LOAD_FAIL;
            }
            else
                if (!WriteAccountingEntry(pacentry->nEntryNo, *pacentry))
                    return DB_LOAD_FAIL;
        }
    }

    return DB_LOAD_OK;
}

class CWalletScanState {
public:
    uint nKeys;
    uint nCKeys;
    uint nKeyMeta;
    bool fIsEncrypted;
    bool fAnyUnordered;
    int nFileVersion;
    vector<uint256> vWalletUpgrade;

    CWalletScanState() {
        nKeys = nCKeys = nKeyMeta = 0;
        fIsEncrypted = false;
        fAnyUnordered = false;
        nFileVersion = 0;
    }
};

bool ReadKeyValue(CWallet *pwallet, CDataStream &ssKey, CDataStream &ssValue,
  CWalletScanState &wss, string &strType, string &strErr) {

    try {
        // Unserialize
        // Taking advantage of the fact that pair serialization
        // is just the two items serialized one after the other
        ssKey >> strType;
        if (strType == "name")
        {
            string strAddress;
            ssKey >> strAddress;
            ssValue >> pwallet->mapAddressBook[CCoinAddress(strAddress).Get()];
        }
        else if (strType == "tx")
        {
            uint256 hash;
            ssKey >> hash;
            CWalletTx& wtx = pwallet->mapWallet[hash];
            ssValue >> wtx;
            if (wtx.CheckTransaction() && (wtx.GetHash() == hash))
                wtx.BindWallet(pwallet);
            else
            {
                pwallet->mapWallet.erase(hash);
                return false;
            }

            // Undo serialize changes in 31600
            if (31404 <= wtx.fTimeReceivedIsTxTime && wtx.fTimeReceivedIsTxTime <= 31703)
            {
                if (!ssValue.empty())
                {
                    char fTmp;
                    char fUnused;
                    ssValue >> fTmp >> fUnused >> wtx.strFromAccount;
                    strErr = strprintf("LoadWallet() upgrading tx ver=%d %d '%s' %s",
                                       wtx.fTimeReceivedIsTxTime, fTmp, wtx.strFromAccount.c_str(), hash.ToString().c_str());
                    wtx.fTimeReceivedIsTxTime = fTmp;
                }
                else
                {
                    strErr = strprintf("LoadWallet() repairing tx ver=%d %s", wtx.fTimeReceivedIsTxTime, hash.ToString().c_str());
                    wtx.fTimeReceivedIsTxTime = 0;
                }
                wss.vWalletUpgrade.push_back(hash);
            }

            if(wtx.nOrderPos == -1)
              wss.fAnyUnordered = true;
        }
        else if (strType == "acentry")
        {
            string strAccount;
            ssKey >> strAccount;
            uint64 nNumber;
            ssKey >> nNumber;
            if (nNumber > nAccountingEntryNumber)
                nAccountingEntryNumber = nNumber;

            if(!wss.fAnyUnordered) {
                CAccountingEntry acentry;
                ssValue >> acentry;
                if(acentry.nOrderPos == -1)
                  wss.fAnyUnordered = true;
            }
        }
        else if(strType == "watch") {
            CScript script;
            ssKey >> script;
            char fYes;
            ssValue >> fYes;
            if(fYes == '1')
              pwallet->LoadWatchOnly(script);

            pwallet->UpdateTimeFirstKey();
        }
        else if (strType == "key" || strType == "wkey")
        {
            vector<unsigned char> vchPubKey;
            ssKey >> vchPubKey;
            CKey key;
            if(strType == "key") {
                wss.nKeys++;
                CPrivKey pkey;
                ssValue >> pkey;
                key.SetPubKey(vchPubKey);
                if (!key.SetPrivKey(pkey))
                {
                    strErr = "Error reading wallet database: CPrivKey corrupt";
                    return false;
                }
                if (key.GetPubKey() != vchPubKey)
                {
                    strErr = "Error reading wallet database: CPrivKey pubkey inconsistency";
                    return false;
                }
                if (!key.IsValid())
                {
                    strErr = "Error reading wallet database: invalid CPrivKey";
                    return false;
                }
            }
            else
            {
                CWalletKey wkey;
                ssValue >> wkey;
                key.SetPubKey(vchPubKey);
                if (!key.SetPrivKey(wkey.vchPrivKey))
                {
                    strErr = "Error reading wallet database: CPrivKey corrupt";
                    return false;
                }
                if (key.GetPubKey() != vchPubKey)
                {
                    strErr = "Error reading wallet database: CWalletKey pubkey inconsistency";
                    return false;
                }
                if (!key.IsValid())
                {
                    strErr = "Error reading wallet database: invalid CWalletKey";
                    return false;
                }
            }
            if (!pwallet->LoadKey(key))
            {
                strErr = "Error reading wallet database: LoadKey failed";
                return false;
            }
        }
        else if (strType == "mkey")
        {
            unsigned int nID;
            ssKey >> nID;
            CMasterKey kMasterKey;
            ssValue >> kMasterKey;
            if(pwallet->mapMasterKeys.count(nID) != 0)
            {
                strErr = strprintf("Error reading wallet database: duplicate CMasterKey id %u", nID);
                return false;
            }
            pwallet->mapMasterKeys[nID] = kMasterKey;
            if (pwallet->nMasterKeyMaxID < nID)
                pwallet->nMasterKeyMaxID = nID;
        } else if(strType == "ckey") {
            wss.nCKeys++;
            vector<unsigned char> vchPubKey;
            ssKey >> vchPubKey;
            vector<unsigned char> vchPrivKey;
            ssValue >> vchPrivKey;
            if (!pwallet->LoadCryptedKey(vchPubKey, vchPrivKey))
            {
                strErr = "Error reading wallet database: LoadCryptedKey failed";
                return false;
            }
            wss.fIsEncrypted = true;
        } else if(strType == "keymeta") {
            CPubKey vchPubKey;
            ssKey >> vchPubKey;
            CKeyMetadata keyMeta;
            ssValue >> keyMeta;
            wss.nKeyMeta++;

            pwallet->LoadKeyMetadata(vchPubKey, keyMeta);
            pwallet->UpdateTimeFirstKey(keyMeta.nCreateTime);
        } else if(strType == "defaultkey") {
            ssValue >> pwallet->vchDefaultKey;
        } else if (strType == "pool") {
            int64 nIndex;
            ssKey >> nIndex;
            pwallet->setKeyPool.insert(nIndex);
            CKeyPool keypool;
            ssValue >> keypool;
            pwallet->setKeyPool.insert(nIndex);

            /* If no metadata exists yet, create a default with the pool key's
             * creation time. Note that this may be overwritten by actually
             * stored metadata for that key later, which is fine. */
            CKeyID keyid = keypool.vchPubKey.GetID();
            if(pwallet->mapKeyMetadata.count(keyid) == 0)
              pwallet->mapKeyMetadata[keyid] = CKeyMetadata(keypool.nTime);
        } else if(strType == "version") {
            ssValue >> wss.nFileVersion;
            if(wss.nFileVersion == 10300)
              wss.nFileVersion = 300;
        }
        else if (strType == "cscript")
        {
            uint160 hash;
            ssKey >> hash;
            CScript script;
            ssValue >> script;
            if (!pwallet->LoadCScript(script))
            {
                strErr = "Error reading wallet database: LoadCScript failed";
                return false;
            }
        }
        else if (strType == "orderposnext")
        {
            ssValue >> pwallet->nOrderPosNext;
        }
    } catch (...)
    {
        return false;
    }
    return true;
}

static bool IsKeyType(string strType)
{
    return (strType== "key" || strType == "wkey" ||
            strType == "mkey" || strType == "ckey");
}

DBErrors CWalletDB::LoadWallet(CWallet *pwallet) {
    CWalletScanState wss;
    pwallet->vchDefaultKey = CPubKey();
    bool fNoncriticalErrors = false;
    DBErrors result = DB_LOAD_OK;

    try {
        LOCK(pwallet->cs_wallet);
        int nMinVersion = 0;
        if (Read((string)"minversion", nMinVersion))
        {
            if (nMinVersion > CLIENT_VERSION)
                return DB_TOO_NEW;
            pwallet->LoadMinVersion(nMinVersion);
        }

        // Get cursor
        Dbc* pcursor = GetCursor();
        if (!pcursor)
        {
            printf("Error getting wallet database cursor\n");
            return DB_CORRUPT;
        }

        while(true) {
            // Read next record
            CDataStream ssKey(SER_DISK, CLIENT_VERSION);
            CDataStream ssValue(SER_DISK, CLIENT_VERSION);
            int ret = ReadAtCursor(pcursor, ssKey, ssValue);
            if (ret == DB_NOTFOUND)
                break;
            else if (ret != 0)
            {
                printf("Error reading next record from wallet database\n");
                return DB_CORRUPT;
            }

            // Try to be tolerant of single corrupt records:
            string strType, strErr;
            if(!ReadKeyValue(pwallet, ssKey, ssValue, wss, strType, strErr)) {
                // losing keys is considered a catastrophic error, anything else
                // we assume the user can live with:
                if (IsKeyType(strType))
                    result = DB_CORRUPT;
                else
                {
                    // Leave other errors alone, if we try to fix them we might make things worse.
                    fNoncriticalErrors = true; // ... but do warn the user there is something wrong.
                    if (strType == "tx")
                        // Rescan if there is a bad transaction record:
                        SoftSetBoolArg("-rescan", true);
                }
            }
            if (!strErr.empty())
                printf("%s\n", strErr.c_str());
        }
        pcursor->close();
    }
    catch (...)
    {
        result = DB_CORRUPT;
    }

    if (fNoncriticalErrors && result == DB_LOAD_OK)
        result = DB_NONCRITICAL_ERROR;

    // Any wallet corruption at all: skip any rewriting or
    // upgrading, we don't want to make it worse.
    if (result != DB_LOAD_OK)
        return result;

    printf("nFileVersion = %d\n", wss.nFileVersion);

    printf("Keys: %u plaintext, %u encrypted, %u with metadata, %u total\n",
           wss.nKeys, wss.nCKeys, wss.nKeyMeta, wss.nKeys + wss.nCKeys);

    if((wss.nKeys + wss.nCKeys) != wss.nKeyMeta)
      pwallet->UpdateTimeFirstKey();

    BOOST_FOREACH(uint256 hash, wss.vWalletUpgrade)
      WriteTx(hash, pwallet->mapWallet[hash]);

    /* Version update */
    if(wss.nFileVersion < CLIENT_VERSION)
      WriteVersion(CLIENT_VERSION);

    if(wss.fAnyUnordered)
      result = ReorderTransactions(pwallet);

    return(result);
}

void ThreadFlushWalletDB(void* parg)
{
    // Make this thread recognisable as the wallet flushing thread
    RenameThread("pxc-wallet");

    const string& strFile = ((const string*)parg)[0];
    static bool fOneThread;
    if (fOneThread)
        return;
    fOneThread = true;
    if (!GetBoolArg("-flushwallet", true))
        return;

    unsigned int nLastSeen = nWalletDBUpdated;
    unsigned int nLastFlushed = nWalletDBUpdated;
    int64 nLastWalletUpdate = GetTime();
    while (!fShutdown)
    {
        Sleep(500);

        if (nLastSeen != nWalletDBUpdated)
        {
            nLastSeen = nWalletDBUpdated;
            nLastWalletUpdate = GetTime();
        }

        if (nLastFlushed != nWalletDBUpdated && GetTime() - nLastWalletUpdate >= 2)
        {
            TRY_LOCK(bitdb.cs_db,lockDb);
            if (lockDb)
            {
                // Don't do this if any databases are in use
                int nRefCount = 0;
                map<string, int>::iterator mi = bitdb.mapFileUseCount.begin();
                while (mi != bitdb.mapFileUseCount.end())
                {
                    nRefCount += (*mi).second;
                    mi++;
                }

                if (nRefCount == 0 && !fShutdown)
                {
                    map<string, int>::iterator mi = bitdb.mapFileUseCount.find(strFile);
                    if (mi != bitdb.mapFileUseCount.end())
                    {
                        printf("Flushing wallet.dat\n");
                        nLastFlushed = nWalletDBUpdated;
                        int64 nStart = GetTimeMillis();

                        // Flush wallet.dat so it's self contained
                        bitdb.CloseDb(strFile);
                        bitdb.CheckpointLSN(strFile);

                        bitdb.mapFileUseCount.erase(mi++);
                        printf("Flushed wallet.dat %" PRI64d "ms\n", GetTimeMillis() - nStart);
                    }
                }
            }
        }
    }
}

bool BackupWallet(const CWallet& wallet, const string& strDest)
{
    if (!wallet.fFileBacked)
        return false;
    while (!fShutdown)
    {
        {
            LOCK(bitdb.cs_db);
            if (!bitdb.mapFileUseCount.count(wallet.strWalletFile) || bitdb.mapFileUseCount[wallet.strWalletFile] == 0)
            {
                // Flush log data to the dat file
                bitdb.CloseDb(wallet.strWalletFile);
                bitdb.CheckpointLSN(wallet.strWalletFile);
                bitdb.mapFileUseCount.erase(wallet.strWalletFile);

                // Copy wallet.dat
                boost::filesystem::path pathSrc = GetDataDir() / wallet.strWalletFile;
                boost::filesystem::path pathDest(strDest);
                if(boost::filesystem::is_directory(pathDest)) pathDest /= wallet.strWalletFile;

                try {
#if (BOOST_VERSION >= 107400)
                    boost::filesystem::copy_file(pathSrc, pathDest,
                      boost::filesystem::copy_options::overwrite_existing);
#elif (BOOST_VERSION >= 104000)
                    boost::filesystem::copy_file(pathSrc, pathDest,
                      boost::filesystem::copy_option::overwrite_if_exists);
#else
                    boost::filesystem::copy_file(pathSrc, pathDest);
#endif
                    printf("copied wallet.dat to %s\n", pathDest.string().c_str());
                    return true;
                } catch(const boost::filesystem::filesystem_error &e) {
                    printf("error copying wallet.dat to %s - %s\n", pathDest.string().c_str(), e.what());
                    return false;
                }
            }
        }
        Sleep(100);
    }
    return false;
}

//
// Try to (very carefully!) recover wallet.dat if there is a problem.
//
bool CWalletDB::Recover(CDBEnv& dbenv, std::string filename, bool fOnlyKeys)
{
    // Recovery procedure:
    // move wallet.dat to wallet.timestamp.bak
    // Call Salvage with fAggressive=true to
    // get as much data as possible.
    // Rewrite salvaged data to wallet.dat
    // Set -rescan so any missing transactions will be
    // found.
    int64 now = GetTime();
    std::string newFilename = strprintf("wallet.%" PRI64d ".bak", now);

    int result = dbenv.dbenv.dbrename(NULL, filename.c_str(), NULL,
                                      newFilename.c_str(), DB_AUTO_COMMIT);
    if (result == 0)
        printf("Renamed %s to %s\n", filename.c_str(), newFilename.c_str());
    else
    {
        printf("Failed to rename %s to %s\n", filename.c_str(), newFilename.c_str());
        return false;
    }

    std::vector<CDBEnv::KeyValPair> salvagedData;
    bool allOK = dbenv.Salvage(newFilename, true, salvagedData);
    if (salvagedData.empty())
    {
        printf("Salvage(aggressive) found no records in %s.\n", newFilename.c_str());
        return false;
    }
    printf("Salvage(aggressive) found %" PRIszu " records\n", salvagedData.size());

    bool fSuccess = allOK;
    Db* pdbCopy = new Db(&dbenv.dbenv, 0);
    int ret = pdbCopy->open(NULL,                 // Txn pointer
                            filename.c_str(),   // Filename
                            "main",    // Logical db name
                            DB_BTREE,  // Database type
                            DB_CREATE,    // Flags
                            0);
    if (ret > 0)
    {
        printf("Cannot create database file %s\n", filename.c_str());
        return false;
    }
    CWallet dummyWallet;
    CWalletScanState wss;

    DbTxn* ptxn = dbenv.TxnBegin();
    BOOST_FOREACH(CDBEnv::KeyValPair& row, salvagedData)
    {
        if (fOnlyKeys)
        {
            CDataStream ssKey(row.first, SER_DISK, CLIENT_VERSION);
            CDataStream ssValue(row.second, SER_DISK, CLIENT_VERSION);
            string strType, strErr;
            bool fReadOK = ReadKeyValue(&dummyWallet, ssKey, ssValue,
              wss, strType, strErr);
            if (!IsKeyType(strType))
                continue;
            if (!fReadOK)
            {
                printf("WARNING: CWalletDB::Recover skipping %s: %s\n", strType.c_str(), strErr.c_str());
                continue;
            }
        }
        Dbt datKey(&row.first[0], row.first.size());
        Dbt datValue(&row.second[0], row.second.size());
        int ret2 = pdbCopy->put(ptxn, &datKey, &datValue, DB_NOOVERWRITE);
        if (ret2 > 0)
            fSuccess = false;
    }
    ptxn->commit(0);
    pdbCopy->close(0);
    delete pdbCopy;

    return fSuccess;
}

bool CWalletDB::Recover(CDBEnv& dbenv, std::string filename)
{
    return CWalletDB::Recover(dbenv, filename, false);
}


/* Wallet keys export / import */

namespace bt = boost::posix_time;

/* Extended DecodeDumpTime implementation, see this page for details:
 * http://stackoverflow.com/questions/3786201/parsing-of-date-time-from-string-boost */
const std::locale formats[] = {
    std::locale(std::locale::classic(), new bt::time_input_facet("%Y-%m-%dT%H:%M:%SZ")),
    std::locale(std::locale::classic(), new bt::time_input_facet("%Y-%m-%d~%H:%M:%S")),
    std::locale(std::locale::classic(), new bt::time_input_facet("%Y-%m-%d %H:%M:%S")),
    std::locale(std::locale::classic(), new bt::time_input_facet("%Y/%m/%d %H:%M:%S")),
    std::locale(std::locale::classic(), new bt::time_input_facet("%d.%m.%Y %H:%M:%S")),
    std::locale(std::locale::classic(), new bt::time_input_facet("%Y-%m-%d"))
};

const size_t formats_n = sizeof(formats) / sizeof(formats[0]);

std::time_t pt_to_time_t(const bt::ptime &pt) {
    bt::ptime timet_start(boost::gregorian::date(1970, 1, 1));
    bt::time_duration diff = pt - timet_start;
    return(diff.ticks() / bt::time_duration::rep_type::ticks_per_second);
}

int64 DecodeDumpTime(const std::string &s) {
    bt::ptime pt;
    size_t i;

    for(i = 0; i < formats_n; ++i) {
        std::istringstream is(s);
        is.imbue(formats[i]);
        is >> pt;
        if(pt != bt::ptime()) break;
    }

    return(pt_to_time_t(pt));
}

std::string static EncodeDumpTime(int64 nTime) {
    /* ISO 8601 */
    return(DateTimeStrFormat("%Y-%m-%dT%H:%M:%SZ", nTime));
}

std::string static EncodeDumpString(const std::string &str) {
    std::stringstream ret;

    BOOST_FOREACH(uchar c, str) {
        if((c <= 32) || (c >= 128) || (c == '%')) {
            ret << '%' << HexStr(&c, &c + 1);
        } else {
            ret << c;
        }
    }
    return(ret.str());
}

std::string DecodeDumpString(const std::string &str) {
    std::stringstream ret;
    uint pos;

    for(pos = 0; pos < str.length(); pos++) {
        uchar c = str[pos];
        if((c == '%') && ((pos + 2) < str.length())) {
            c = (((str[pos + 1] >> 6) * 9 + ((str[pos + 1] - '0') & 15)) << 4) |
              ((str[pos + 2] >> 6) * 9 + ((str[pos + 2] - '0') & 15));
            pos += 2;
        }
        ret << c;
    }
    return(ret.str());
}

/* Exports wallet key pairs into a formatted text file */
bool ExportWallet(CWallet *pwallet, const string &strDst) {

    if(!pwallet->fFileBacked) return(false);

    ofstream file;
    file.open(strDst.c_str());
    if(!file.is_open()) return(false);

    std::map<CKeyID, int64> mapKeyBirth;

    std::set<CKeyID> setKeyPool;

    pwallet->GetKeyBirthTimes(mapKeyBirth);

    pwallet->GetAllReserveKeys(setKeyPool);

    /* Sort time/key pairs */
    std::vector<std::pair<int64, CKeyID> > vKeyBirth;
    for(std::map<CKeyID, int64>::const_iterator it = mapKeyBirth.begin(); it != mapKeyBirth.end(); it++) {
        vKeyBirth.push_back(std::make_pair(it->second, it->first));
    }
    mapKeyBirth.clear();
    std::sort(vKeyBirth.begin(), vKeyBirth.end());

    /* Produce output */
    file << strprintf("# Wallet export created by Phoenixcoin %s (%s)\n",
      CLIENT_BUILD_VERSION.c_str(), CLIENT_BUILD_DATE_TIME.c_str());
    file << strprintf("# * Created on %s\n", EncodeDumpTime(GetTime()).c_str());
    file << strprintf("# * The best block at the creation time was %i (%s),\n",
      nBestHeight, hashBestChain.ToString().c_str());
    file << strprintf("#   mined on %s\n", EncodeDumpTime(pindexBest->nTime).c_str());
    file << "\n";
    for(std::vector<std::pair<int64, CKeyID> >::const_iterator it = vKeyBirth.begin(); it != vKeyBirth.end(); it++) {
        const CKeyID &keyid = it->second;
        std::string strTime = EncodeDumpTime(it->first);
        std::string strAddr = CCoinAddress(keyid).ToString();
        bool IsCompressed;

        CKey key;
        if(pwallet->GetKey(keyid, key)) {
            if(pwallet->mapAddressBook.count(keyid)) {
                CSecret secret = key.GetSecret(IsCompressed);
                file << strprintf("%s %s label=%s # addr=%s\n",
                  CCoinSecret(secret, IsCompressed).ToString().c_str(), strTime.c_str(),
                  EncodeDumpString(pwallet->mapAddressBook[keyid]).c_str(), strAddr.c_str());
            } else if(setKeyPool.count(keyid)) {
                CSecret secret = key.GetSecret(IsCompressed);
                file << strprintf("%s %s reserve=1 # addr=%s\n",
                  CCoinSecret(secret, IsCompressed).ToString().c_str(), strTime.c_str(),
                  strAddr.c_str());
            } else {
                CSecret secret = key.GetSecret(IsCompressed);
                file << strprintf("%s %s change=1 # addr=%s\n",
                  CCoinSecret(secret, IsCompressed).ToString().c_str(), strTime.c_str(),
                  strAddr.c_str());
            }
        }
    }
    file << "\n";
    file << "# End of export\n";
    file.close();

    return(true);
}

/* Imports wallet key pairs from a formatted text file */
bool ImportWallet(CWallet *pwallet, const string &strSrc) {

    if(!pwallet->fFileBacked) return(false);

    ifstream file;
    file.open(strSrc.c_str());
    if(!file.is_open()) return(false);

    int64 nTimeBegin = pindexBest->nTime;

    bool fGood = true;

    while(file.good()) {
        std::string line;
        std::getline(file, line);
        if(line.empty() || (line[0] == '#'))
          continue;

        std::vector<std::string> vstr;
        istringstream iss(line);
        copy(istream_iterator<string>(iss), istream_iterator<string>(), back_inserter(vstr));
        if(vstr.size() < 2)
          continue;

        CCoinSecret vchSecret;
        if(!vchSecret.SetString(vstr[0]))
          continue;

        bool fCompressed;
        CKey key;
        CSecret secret = vchSecret.GetSecret(fCompressed);
        key.SetSecret(secret, fCompressed);
        CKeyID keyid = key.GetPubKey().GetID();

        if(pwallet->HaveKey(keyid)) {
            printf("Skipping import of %s (key already present)\n",
              CCoinAddress(keyid).ToString().c_str());
            continue;
        }
        int64 nTime = DecodeDumpTime(vstr[1]);
        std::string strLabel;
        bool fLabel = true;
        uint nStr;
        for(nStr = 2; nStr < vstr.size(); nStr++) {
            if(boost::algorithm::starts_with(vstr[nStr], "#"))
              break;
            if(vstr[nStr] == "change=1")
              fLabel = false;
            if(vstr[nStr] == "reserve=1")
              fLabel = false;
            if(boost::algorithm::starts_with(vstr[nStr], "label=")) {
                strLabel = DecodeDumpString(vstr[nStr].substr(6));
                fLabel = true;
            }
        }
        printf("Importing %s...\n", CCoinAddress(keyid).ToString().c_str());
        if(!pwallet->AddKey(key)) {
            fGood = false;
            continue;
        }
        pwallet->mapKeyMetadata[keyid].nCreateTime = nTime;
        if(fLabel)
          pwallet->SetAddressBookName(keyid, strLabel);
        nTimeBegin = std::min(nTimeBegin, nTime);
    }
    file.close();

    CBlockIndex *pindex = pindexBest;
    while(pindex && pindex->pprev && (pindex->nTime > (nTimeBegin - 7200)))
      pindex = pindex->pprev;

    printf("Rescanning last %i blocks\n", pindexBest->nHeight - pindex->nHeight + 1);
    pwallet->ScanForWalletTransactions(pindex);
    pwallet->ReacceptWalletTransactions();
    pwallet->MarkDirty();

    return(fGood);
}
