// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <txdb.h>

#include <chainparams.h>
#include <claimtrie/sqlite.h>
#include <hash.h>
#include <key_io.h>
#include <random.h>
#include <pow.h>
#include <script/standard.h>
#include <shutdown.h>
#include <uint256.h>
#include <util.h>
#include <ui_interface.h>

#include <stdint.h>

#include <boost/thread.hpp>

static const sqlite::sqlite_config sharedConfig {
        sqlite::OpenFlags::READWRITE | sqlite::OpenFlags::CREATE,
        nullptr, sqlite::Encoding::UTF8
};

CCoinsViewDB::CCoinsViewDB(size_t nCacheSize, bool fMemory, bool fWipe)
    : db(fMemory ? ":memory:" : (GetDataDir() / "coins.sqlite").string(), sharedConfig)
{
    db << "PRAGMA cache_size=-" + std::to_string(nCacheSize >> 10); // in -KB
    db << "PRAGMA temp_store=MEMORY";
    db << "PRAGMA case_sensitive_like=true";
    db << "PRAGMA journal_mode=WAL";
    db << "PRAGMA synchronous=OFF"; // don't disk sync after transaction commit; we handle that elsewhere
    db << "PRAGMA wal_autocheckpoint=4000"; // 4k page size * 4000 = 16MB

    db << "CREATE TABLE IF NOT EXISTS unspent (txID BLOB NOT NULL COLLATE BINARY, txN INTEGER NOT NULL, "
          "isCoinbase INTEGER NOT NULL, blockHeight INTEGER NOT NULL, amount INTEGER NOT NULL, "
          "script BLOB NOT NULL COLLATE BINARY, address TEXT, PRIMARY KEY(txID, txN));";

    db << "CREATE INDEX IF NOT EXISTS unspent_address ON unspent(address)";

    db << "CREATE TABLE IF NOT EXISTS marker ("
          "name TEXT NOT NULL PRIMARY KEY, "
          "value BLOB NOT NULL)";

    if (fWipe) {
        db << "DELETE FROM unspent";
        db << "DELETE FROM marker";
    }
}

bool CCoinsViewDB::GetCoin(const COutPoint &outpoint, Coin &coin) const {
    auto query = db << "SELECT isCoinbase, blockHeight, amount, script FROM unspent "
          "WHERE txID = ? AND txN = ?" << outpoint.hash << outpoint.n;
    for (auto&& row: query) {
        uint32_t coinbase = 0, height = 0;
        row >> coinbase >> height >> coin.out.nValue >> coin.out.scriptPubKey;
        coin.fCoinBase = coinbase;
        coin.nHeight = height;
        return true;
    }
    return false;
}

bool CCoinsViewDB::HaveCoin(const COutPoint &outpoint) const {
    auto query = db << "SELECT 1 FROM unspent "
                       "WHERE txID = ? AND txN = ?" << outpoint.hash << outpoint.n;
    return query.begin() != query.end();
}

uint256 CCoinsViewDB::GetBestBlock() const {
    auto query = db << "SELECT value FROM marker WHERE name = 'best_block'";
    for (auto&& row: query) {
        uint256 hashBestChain;
        row >> hashBestChain;
        return hashBestChain;
    }
    return {};
}

std::vector<uint256> CCoinsViewDB::GetHeadBlocks() const {
    std::vector<uint256> vhashHeadBlocks;
    auto query = db << "SELECT value FROM marker ORDER BY name DESC";
    for (auto&& row: query) {
        vhashHeadBlocks.emplace_back();
        row >> vhashHeadBlocks.back();
    }
    if (vhashHeadBlocks.size() != 2)
        vhashHeadBlocks.clear();
    return vhashHeadBlocks;
}

bool CCoinsViewDB::BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock, bool sync) {

    size_t count = 0;
    size_t changed = 0;
    int crash_simulate = gArgs.GetArg("-dbcrashratio", 0);
    assert(!hashBlock.IsNull());

    uint256 old_tip = GetBestBlock();
    if (old_tip.IsNull()) {
        // We may be in the middle of replaying.
        std::vector<uint256> old_heads = GetHeadBlocks();
        if (old_heads.size() == 2) {
            assert(old_heads[0] == hashBlock);
            old_tip = old_heads[1];
        }
    }

    db << "BEGIN";
    db << "INSERT OR REPLACE INTO marker VALUES('head_block', ?)" << hashBlock;

    auto dbd = db << "DELETE FROM unspent WHERE txID = ? AND txN = ?";
    auto dbi = db << "INSERT OR REPLACE INTO unspent VALUES(?,?,?,?,?,?,?)";
    for (auto it = mapCoins.begin(); it != mapCoins.end();) {
        if (it->second.flags & CCoinsCacheEntry::DIRTY) {
            if (it->second.coin.IsSpent()) {
                // at present the "IsSpent" flag is used for both "spent" and "block going backwards"
                dbd << it->first.hash << it->first.n;
                dbd++;
            }
            else {
                CTxDestination address;
                std::string destination;
                if (ExtractDestination(it->second.coin.out.scriptPubKey, address))
                    destination = EncodeDestination(address);
                uint32_t isCoinBase = it->second.coin.fCoinBase; // bit-field
                uint32_t coinHeight = it->second.coin.nHeight; // bit-field
                dbi << it->first.hash << it->first.n << isCoinBase << coinHeight
                    << it->second.coin.out.nValue << it->second.coin.out.scriptPubKey << destination;
                dbi++;
            }
            changed++;
        }
        count++;
        auto itOld = it++;
        mapCoins.erase(itOld);
        if (crash_simulate && count % 200000 == 0) {
            static FastRandomContext rng;
            if (rng.randrange(crash_simulate) == 0) {
                LogPrintf("Simulating a crash. Goodbye.\n");
                _Exit(0);
            }
        }
    }
    dbd.used(true);
    dbi.used(true);
    db << "INSERT OR REPLACE INTO marker VALUES('best_block', ?)" << hashBlock;
    db << "DELETE FROM marker WHERE name = 'head_block'";

    auto code = sqlite::commit(db);
    if (code != SQLITE_OK) {
        LogPrint(BCLog::COINDB, "Error committing transaction outputs changes to coin database. SQLite error: %d\n", code);
        return false;
    }
    LogPrint(BCLog::COINDB, "Committed %u changed transaction outputs (out of %u) to coin database...\n", (unsigned int)changed, (unsigned int)count);
    if (sync) {
        auto code = sqlite::sync(db);
        if (code != SQLITE_OK) {
            LogPrint(BCLog::COINDB, "Error syncing coin database. SQLite error: %d\n", code);
            return false;
        }
    }
    return true;
}

size_t CCoinsViewDB::EstimateSize() const
{
    size_t ret = 0;
    db << "SELECT COUNT(*) FROM unspent" >> ret;
    return ret * 100;
}

CBlockTreeDB::CBlockTreeDB(size_t nCacheSize, bool fMemory, bool fWipe)
    : db(fMemory ? ":memory:" : (GetDataDir() / "block_index.sqlite").string(), sharedConfig)
{
    db << "PRAGMA cache_size=-" + std::to_string(nCacheSize >> 10); // in -KB
    db << "PRAGMA temp_store=MEMORY";
    db << "PRAGMA case_sensitive_like=true";
    db << "PRAGMA journal_mode=WAL";
    db << "PRAGMA synchronous=OFF"; // don't disk sync after transaction commit; we handle that elsewhere
    db << "PRAGMA wal_autocheckpoint=4000"; // 4k page size * 4000 = 16MB

    db << "CREATE TABLE IF NOT EXISTS block_file ("
          "file INTEGER NOT NULL PRIMARY KEY, "
          "blocks INTEGER NOT NULL, "
          "size INTEGER NOT NULL, "
          "undoSize INTEGER NOT NULL, "
          "heightFirst INTEGER NOT NULL, "
          "heightLast INTEGER NOT NULL, "
          "timeFirst INTEGER NOT NULL, "
          "timeLast INTEGER NOT NULL "
          ")";

    db << "CREATE TABLE IF NOT EXISTS block_info ("
          "hash BLOB NOT NULL PRIMARY KEY, "
          "prevHash BLOB NOT NULL, "
          "height INTEGER NOT NULL, "
          "file INTEGER NOT NULL, "
          "dataPos INTEGER NOT NULL, "
          "undoPos INTEGER NOT NULL, "
          "txCount INTEGER NOT NULL, "
          "status INTEGER NOT NULL, "
          "version INTEGER NOT NULL, "
          "rootTxHash BLOB NOT NULL, "
          "rootTrieHash BLOB NOT NULL, "
          "time INTEGER NOT NULL, "
          "bits INTEGER NOT NULL, "
          "nonce INTEGER NOT NULL "
          ")";

    db << "CREATE TABLE IF NOT EXISTS tx_to_block ("
          "txID BLOB NOT NULL PRIMARY KEY, "
          "file INTEGER NOT NULL, "
          "blockPos INTEGER NOT NULL, "
          "txPos INTEGER NOT NULL)";

    db << "CREATE TABLE IF NOT EXISTS flag ("
          "name TEXT NOT NULL PRIMARY KEY, "
          "value INTEGER NOT NULL)";

    if (fWipe) {
        db << "DELETE FROM block_file";
        db << "DELETE FROM block_info";
        db << "DELETE FROM tx_to_block";
        db << "DELETE FROM flag";
    }

    // not unique because we actually want to store forks:
    db << "CREATE INDEX IF NOT EXISTS block_info_height ON block_info (height)";
}

bool CBlockTreeDB::ReadBlockFileInfo(int nFile, CBlockFileInfo &info) {
    auto query = db << "SELECT blocks, size, undoSize, heightFirst, heightLast, timeFirst, timeLast "
          "FROM block_file WHERE file = ?" << nFile;
    for (auto&& row: query) {
        row >> info.nBlocks >> info.nSize >> info.nUndoSize >> info.nHeightFirst
            >> info.nHeightLast >> info.nTimeFirst >> info.nTimeLast;
        return true;
    }
    return false;
}

bool CBlockTreeDB::WriteReindexing(bool fReindexing) {
    return WriteFlag("reindexing", fReindexing);
}

void CBlockTreeDB::ReadReindexing(bool &fReindexing) {
    if (!ReadFlag("reindexing", fReindexing))
        fReindexing = false;
}

bool CBlockTreeDB::ReadLastBlockFile(int &nFile) {
    auto query = db << "SELECT value FROM flag WHERE name = 'last_block'";
    for (auto&& row: query) {
        row >> nFile;
        return true;
    }
    return false;
}

CCoinsViewCursor *CCoinsViewDB::Cursor() const
{
    return new CCoinsViewDBCursor(GetBestBlock(), this);
}

CCoinsViewDBCursor::CCoinsViewDBCursor(const uint256 &hashBlockIn, const CCoinsViewDB* view)
        : CCoinsViewCursor(hashBlockIn), owner(view),
        query(owner->db << "SELECT txID, txN, isCoinbase, blockHeight, amount, script FROM unspent")
{
    iter = query.begin();
}

bool CCoinsViewDBCursor::GetKey(COutPoint &key) const
{
    if (iter != query.end())
    {
        iter->index() = 0;
        *iter >> key.hash >> key.n;
        return true;
    }
    return false;
}

bool CCoinsViewDBCursor::GetValue(Coin &coin) const
{
    if (iter != query.end())
    {
        iter->index() = 2;
        uint32_t coinbase = 0, height = 0;
        *iter >> coinbase >> height >> coin.out.nValue >> coin.out.scriptPubKey;
        coin.fCoinBase = coinbase;
        coin.nHeight = height;
        return true;
    }
    return false;
}

bool CCoinsViewDBCursor::Valid() const
{
    return iter != query.end();
}

void CCoinsViewDBCursor::Next()
{
    ++iter;
}

bool CBlockTreeDB::BatchWrite(const std::vector<std::pair<int, const CBlockFileInfo*> >& fileInfo,
                              int nLastFile, const std::vector<const CBlockIndex*>& blockInfo, bool sync) {
    db << "BEGIN";
    auto ibf = db << "INSERT OR REPLACE INTO block_file(file, blocks, size, undoSize, heightFirst, heightLast, timeFirst, timeLast) "
                     "VALUES(?,?,?,?,?,?,?,?)";
    for (auto& kvp: fileInfo) {
        ibf << kvp.first << kvp.second->nBlocks << kvp.second->nSize << kvp.second->nUndoSize
            << kvp.second->nHeightFirst << kvp.second->nHeightLast << kvp.second->nTimeFirst << kvp.second->nTimeLast;
        ibf++;
    }
    ibf.used(true);
    db << "INSERT OR REPLACE INTO flag VALUES('last_block', ?)" << nLastFile; // TODO: is this always max(file column)?

    auto ibi = db << "INSERT OR REPLACE INTO block_info(hash, prevHash, height, file, dataPos, undoPos, "
                     "txCount, status, version, rootTxHash, rootTrieHash, time, bits, nonce) "
                     "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    for (auto& bi: blockInfo) {
        ibi << bi->GetBlockHash() << (bi->pprev ? bi->pprev->GetBlockHash() : uint256())
            << bi->nHeight << bi->nFile << bi->nDataPos << bi->nUndoPos << bi->nTx
            << bi->nStatus << bi->nVersion << bi->hashMerkleRoot << bi->hashClaimTrie
            << bi->nTime << bi->nBits << bi->nNonce;
        ibi++;
    }
    ibi.used(true);
    auto code = sqlite::commit(db);
    if (code != SQLITE_OK) {
        LogPrintf("%s: Error committing block info to database. SQLite error: %d\n", __func__, code);
        return false;
    }
    // by Sync they mean disk sync:
    if (sync) {
        auto code = sqlite::sync(db);
        if (code != SQLITE_OK) {
            LogPrintf("%s: Error syncing block database. SQLite error: %d\n", __func__, code);
            return false;
        }
    }
    return true;
}

bool CBlockTreeDB::WriteFlag(const std::string &name, bool fValue) {
    db << "INSERT OR REPLACE INTO flag VALUES(?, ?)" << name << int(fValue);
    return db.rows_modified() > 0;
}

bool CBlockTreeDB::ReadFlag(const std::string &name, bool &fValue) {
    auto query = db << "SELECT value FROM flag WHERE name = ?" << name;
    for (auto&& row: query) {
        int value = 0;
        row >> value;
        fValue = value != 0;
        return true;
    }
    return false;
}

bool CBlockTreeDB::LoadBlockIndexGuts(const Consensus::Params& consensusParams, std::function<CBlockIndex*(const uint256&)> insertBlockIndex)
{
    auto query = db << "SELECT hash, prevHash, height, file, dataPos, undoPos, txCount, "
                       "version, rootTxHash, rootTrieHash, time, bits, nonce, status "
                       "FROM block_info ORDER BY height";

    for (auto&& row: query) {
        // Construct block index object
        uint256 hash, prevHash;
        row >> hash >> prevHash;
        CBlockIndex* pindexNew    = insertBlockIndex(hash);
        pindexNew->pprev          = insertBlockIndex(prevHash);

        row >> pindexNew->nHeight
            >> pindexNew->nFile
            >> pindexNew->nDataPos
            >> pindexNew->nUndoPos
            >> pindexNew->nTx
            >> pindexNew->nVersion
            >> pindexNew->hashMerkleRoot
            >> pindexNew->hashClaimTrie
            >> pindexNew->nTime
            >> pindexNew->nBits
            >> pindexNew->nNonce
            >> pindexNew->nStatus;

        // nChainTx gets set later; don't set it here or you'll mess up the Unlinked list

        if (!CheckProofOfWork(pindexNew->GetBlockPoWHash(), pindexNew->nBits, consensusParams))
        {
            LogPrintf("%s: CheckProofOfWorkFailed: %s\n", __func__, pindexNew->ToString());
            LogPrintf("%s: CheckProofOfWorkFailed: %s (hash %s, nBits=%x, nTime=%d)\n", __func__, pindexNew->GetBlockPoWHash().GetHex(), pindexNew->GetBlockHash().GetHex(), pindexNew->nBits, pindexNew->nTime);
            return error("%s: CheckProofOfWork failed: %s", __func__, pindexNew->ToString());
        }

        if ((pindexNew->nHeight & 0x3ff) == 0x3ff) { // don't check for shutdown on every single block
            boost::this_thread::interruption_point();
            if (ShutdownRequested())
                return false;
        }
    }

    return true;
}

bool CBlockTreeDB::WriteTxIndex(const std::vector<std::pair<uint256, CDiskTxPos>> &list) {
    if (list.empty()) return true;
    db << "BEGIN";
    auto query = db << "INSERT OR REPLACE INTO tx_to_block VALUES(?,?,?,?)";
    for (auto& kvp: list) {
        query << kvp.first << kvp.second.nFile << kvp.second.nPos << kvp.second.nTxOffset;
        query++;
    }
    query.used(true);
    auto code = sqlite::commit(db);
    if (code != SQLITE_OK) {
        LogPrintf("%s: Error committing tx to database. SQLite error: %d\n", __func__, code);
        return false;
    }
    return true;
}

bool CBlockTreeDB::ReadTxIndex(const uint256 &txid, CDiskTxPos &pos) {
    auto query = db << "SELECT file, blockPos, txPos FROM tx_to_block WHERE txID = ?" << txid;
    for (auto&& row: query) {
        row >> pos.nFile >> pos.nPos >> pos.nTxOffset;
        return true;
    }
    return false;
}
